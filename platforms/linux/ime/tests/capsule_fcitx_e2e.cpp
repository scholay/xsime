#include "capsule_history.hpp"
#include "capsule_capture.hpp"
#include "capsule_rich_text.hpp"
#include "capsule_protocol.hpp"
#include "capsule_store.hpp"
#include "capsule_transport.hpp"
#include "official_catalog.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/testing.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <testfrontend_public.h>

namespace {

int g_exit_status = EXIT_SUCCESS;
std::unique_ptr<fcitx::EventSourceTime> resume_timer;

struct TestFailed : std::runtime_error {
    explicit TestFailed(const std::string& message) : std::runtime_error(message) {}
};

void Die(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    g_exit_status = EXIT_FAILURE;
    throw TestFailed(message);
}

void Type(fcitx::AddonInstance* frontend, const fcitx::ICUUID& uuid, const char* keys) {
    for (const char* cursor = keys; *cursor != '\0'; ++cursor) {
        const char name[] = {*cursor, '\0'};
        frontend->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(name), false);
        frontend->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(name), true);
    }
}

void SendKey(fcitx::AddonInstance* frontend, const fcitx::ICUUID& uuid, const char* name) {
    frontend->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(name), false);
    frontend->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(name), true);
}

std::string IsolateDirs() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("rimes-capsule-e2e-" + std::to_string(getpid()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "user" / "log", error);
    if (error) {
        Die("could not create isolated dirs: " + error.message());
    }
    static std::string user_env;
    static std::string log_env;
    static std::string buffer_socket;
    static std::string buffer_dump;
    static std::string capsule_socket;
    static std::string capsule_dump;
    user_env = (root / "user").string();
    log_env = (root / "user" / "log").string();
    buffer_socket = (root / "rimes-buffer.sock").string();
    buffer_dump = (root / "buffer-snapshot.json").string();
    capsule_socket = (root / "rimes-capsule.sock").string();
    capsule_dump = (root / "capsule-snapshot.json").string();
    setenv("RIMES_USER_DIR", user_env.c_str(), 1);
    setenv("RIMES_LOG_DIR", log_env.c_str(), 1);
    setenv("RIMES_BUFFER_SOCKET", buffer_socket.c_str(), 1);
    setenv("RIMES_BUFFER_DUMP", buffer_dump.c_str(), 1);
    setenv("RIMES_BUFFER_HEADLESS", "1", 1);
    setenv("RIMES_BUFFER_AUTO_CAPTURE", "0", 1);
    setenv("RIMES_CAPSULE_SOCKET", capsule_socket.c_str(), 1);
    setenv("RIMES_CAPSULE_DUMP", capsule_dump.c_str(), 1);
    setenv("RIMES_CAPSULE_HEADLESS", "1", 1);
    setenv("RIMES_CAPSULE_FOCUS_GRACE_MS", "200", 1);
    return capsule_dump;
}

std::string ReadDump(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        Die("capsule dump is missing at " + path);
    }
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (json.empty()) {
        Die("capsule dump is empty");
    }
    return json;
}

bool HasNeedle(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

void ExpectContains(const std::string& haystack, const char* needle, const char* message) {
    if (!HasNeedle(haystack, needle)) {
        std::cerr << haystack << '\n';
        Die(message);
    }
}

void ExpectMissing(const std::string& haystack, const char* needle, const char* message) {
    if (HasNeedle(haystack, needle)) {
        std::cerr << haystack << '\n';
        Die(message);
    }
}

void ExpectNoZwsp(fcitx::InputContext* ic, const char* where) {
    const auto client = ic->inputPanel().clientPreedit().toString();
    const auto popup = ic->inputPanel().preedit().toString();
    constexpr const char* kZwsp = "\xe2\x80\x8b";
    if (client.find(kZwsp) != std::string::npos || popup.find(kZwsp) != std::string::npos) {
        Die(std::string("U+200B leaked into host preedit at ") + where);
    }
}

std::unique_ptr<fcitx::EventSourceTime> capture_timer;
void RunCaptureSuite(fcitx::Instance& instance, fcitx::AddonInstance* frontend, fcitx::ICUUID uuid,
                     fcitx::InputContext* ic, const std::string& dump_path) {
    using namespace rimes::capsule;
    struct State {
        int fd=-1, phase=0; CaptureRecord image, other; std::uint64_t sequence=0;
        ~State() {if(fd>=0) close(fd);}
    };
    auto state=std::make_shared<State>();CaptureStore archive(CaptureStore::DefaultRoot());
    const auto file=archive.root().parent_path()/"fixture.png";
    const auto bytes=Base64Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=");
    std::ofstream(file,std::ios::binary)<<*bytes;
    CaptureRecord record;record.title="Host capture";record.source="捕获来源";record.width=1;record.height=1;
    std::atomic<bool> cancelled{false};state->image=archive.Import(file,record,cancelled);
    record.title="Temporary capture";state->other=archive.Import(file,record,cancelled);
    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit});
    SendKey(frontend,uuid,"Control+Shift+V");
    state->fd=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    sockaddr_un address{};address.sun_family=AF_UNIX;
    std::strncpy(address.sun_path,std::getenv("RIMES_CAPSULE_SOCKET"),sizeof(address.sun_path)-1);
    if(state->fd<0 || connect(state->fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))) Die("capture socket");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    capture_timer=instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,fcitx::now(CLOCK_MONOTONIC)+20000,20000,
        [&,frontend,uuid,ic,dump_path,state,deadline](fcitx::EventSourceTime* timer,uint64_t) {
            try {
                if(std::chrono::steady_clock::now()>deadline) Die("capture timeout phase "+std::to_string(state->phase));
                char buffer[65536];while(recv(state->fd,buffer,sizeof(buffer),MSG_DONTWAIT)>0) {}
                auto send=[&](nlohmann::json command) {
                    command["v"]=1;std::string frame;EncodeFrame(command.dump(),&frame);
                    if(::send(state->fd,frame.data(),frame.size(),MSG_NOSIGNAL)!=static_cast<ssize_t>(frame.size())) Die("capture socket send");
                };
                const auto snapshot=nlohmann::json::parse(ReadDump(dump_path));
                switch(state->phase) {
                case 0: send({{"op","module"},{"text","capture"}});state->phase=1;break;
                case 1:
                    if(snapshot.at("module")!="capture" || snapshot.at("loading").get<bool>() || snapshot.at("count")!=2) break;
                    send({{"op","search"},{"text","Host 捕获来源"}});state->phase=2;break;
                case 2:
                    if(snapshot.at("query")!="Host 捕获来源") break;
                    if(snapshot.at("count")!=1 || snapshot.at("selected_id")!=state->image.id) Die("capture source/title search failed");
                    send({{"op","copy"},{"id",state->image.id},{"revision",state->image.revision}});state->phase=3;break;
                case 3:
                    if(snapshot.at("restore_id")!=state->image.id) break;
                    if(snapshot.at("restore_revision")!=state->image.revision || !snapshot.at("last_copied").get<std::string>().empty()) Die("capture copied text instead of media intent");
                    SendKey(frontend,uuid,"Return");state->phase=4;break;
                case 4:
                    if(!snapshot.at("managing").get<bool>() || snapshot.at("loading").get<bool>()) break;
                    send({{"op","favorite"},{"id",state->image.id},{"revision",state->image.revision}});state->phase=5;break;
                case 5: {
                    if(snapshot.at("loading").get<bool>() || snapshot.at("cards").empty() || !snapshot.at("cards").at(0).at("favorite").get<bool>()) break;
                    CaptureStore archive(CaptureStore::DefaultRoot());state->image=*archive.Get(state->image.id);
                    ContentStore library(ContentStore::DefaultRoot());Record saved;std::string error;
                    if(library.Get(state->image.id,&saved,&error)!=StoreStatus::Ok || saved.content!=archive.File(state->image)) Die("capture favorite missing permanent entry");
                    send({{"op","remove"},{"id",state->image.id},{"revision",state->image.revision}});state->phase=6;break;
                }
                case 6:
                    if(snapshot.at("hint").get<std::string>().find("已收藏")==std::string::npos) break;
                    send({{"op","search"},{"text","Temporary"}});state->phase=7;break;
                case 7:
                    if(snapshot.at("selected_id")!=state->other.id || snapshot.at("loading").get<bool>()) break;
                    send({{"op","remove"},{"id",state->other.id},{"revision",state->other.revision}});state->phase=8;break;
                case 8:
                    if(snapshot.at("loading").get<bool>() || snapshot.at("count")!=0) break;
                    if(CaptureStore(CaptureStore::DefaultRoot()).Get(state->other.id)) Die("capture temporary media not removed");
                    send({{"op","search"},{"text","Host"}});state->phase=9;break;
                case 9:
                    if(snapshot.at("selected_id")!=state->image.id) break;
                    state->sequence=snapshot.at("restore_seq");
                    send({{"op","copy"},{"id",state->image.id},{"revision",std::string(64,'b')}});
                    send({{"op","search"},{"text","Host capture"}});state->phase=10;break;
                case 10:
                    if(snapshot.at("query")!="Host capture") break;
                    if(snapshot.at("restore_seq")!=state->sequence) Die("stale capture revision restored");
                    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit}|fcitx::CapabilityFlag::Password);state->phase=11;break;
                case 11:
                    if(!snapshot.at("password_field").get<bool>()) break;
                    if(!snapshot.at("cards").empty() || !snapshot.at("restore_id").get<std::string>().empty()) Die("protected capture exposed metadata");
                    std::cout<<"ok: Fcitx capture search, media-copy identity, Return preview, permanent favorite, temporary removal, stale revisions and protection passed\n";
                    instance.exit();return true;
                }
                timer->setTime(fcitx::now(CLOCK_MONOTONIC)+20000);timer->setOneShot();return true;
            } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';g_exit_status=EXIT_FAILURE;instance.exit();return true;}
        });
}

std::unique_ptr<fcitx::EventSourceTime> resource_timer;
void RunResourceSuite(fcitx::Instance& instance, fcitx::AddonInstance* frontend, fcitx::ICUUID uuid,
                      fcitx::InputContext* ic, const std::string& dump_path) {
    using namespace rimes::capsule;
    struct State {
        int fd=-1, phase=0; Record record; std::uint64_t sequence=0;
        ~State() {if(fd>=0) close(fd);}
    };
    auto state=std::make_shared<State>(); ContentStore store(ContentStore::DefaultRoot()); std::string error;
    const auto file=store.root().parent_path()/"reference.pdf"; std::ofstream(file)<<"%PDF-1.4\n";
    state->record.kind=Kind::Pdf; state->record.title="Host reference"; state->record.content=file.string();
    state->record.reference.fields={{"authors","资源作者"},{"year","2024"},{"publisher","本地出版社"}};
    if(store.Put(state->record,"",&state->record,&error)!=StoreStatus::Ok) Die("resource fixture save");
    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit});
    SendKey(frontend,uuid,"Control+Shift+V");
    state->fd=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    sockaddr_un address{};address.sun_family=AF_UNIX;
    std::strncpy(address.sun_path,std::getenv("RIMES_CAPSULE_SOCKET"),sizeof(address.sun_path)-1);
    if(state->fd<0 || connect(state->fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))) Die("resource socket");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    resource_timer=instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,fcitx::now(CLOCK_MONOTONIC)+20000,20000,
        [&,frontend,uuid,ic,dump_path,state,deadline](fcitx::EventSourceTime* timer,uint64_t) {
            try {
                if(std::chrono::steady_clock::now()>deadline) Die("resource timeout phase "+std::to_string(state->phase));
                char buffer[65536];while(recv(state->fd,buffer,sizeof(buffer),MSG_DONTWAIT)>0) {}
                auto send=[&](nlohmann::json command) {
                    command["v"]=1;std::string frame;EncodeFrame(command.dump(),&frame);
                    if(::send(state->fd,frame.data(),frame.size(),MSG_NOSIGNAL)!=static_cast<ssize_t>(frame.size())) Die("resource socket send");
                };
                const auto snapshot=nlohmann::json::parse(ReadDump(dump_path));
                switch(state->phase) {
                case 0: send({{"op","module"},{"text","resources"}}); state->phase=1; break;
                case 1:
                    if(snapshot.at("module")!="resources" || snapshot.at("loading").get<bool>() || snapshot.at("count")!=1) break;
                    send({{"op","search"},{"text","资源作者"}}); state->phase=2; break;
                case 2:
                    if(snapshot.at("query")!="资源作者") break;
                    if(snapshot.at("count")!=1 || snapshot.at("selected_id")!=state->record.id) Die("resource metadata search lost record");
                    send({{"op","copy"},{"id",state->record.id},{"revision",state->record.revision}}); state->phase=3; break;
                case 3:
                    if(snapshot.at("restore_id")!=state->record.id) break;
                    if(snapshot.at("restore_revision")!=state->record.revision || snapshot.at("restore_close").get<bool>() || !snapshot.at("last_copied").get<std::string>().empty()) Die("resource copy lost file semantics");
                    state->sequence=snapshot.at("restore_seq"); SendKey(frontend,uuid,"Return"); state->phase=4; break;
                case 4:
                    if(snapshot.at("restore_seq")==state->sequence) break;
                    if(!snapshot.at("restore_close").get<bool>() || !snapshot.at("visible").get<bool>()) Die("resource activation closed before clipboard acknowledgement");
                    send({{"op","detail"},{"id",state->record.id},{"revision",state->record.revision}}); state->phase=5; break;
                case 5:
                    if(!snapshot.at("managing").get<bool>() || snapshot.at("loading").get<bool>()) break;
                    state->sequence=snapshot.at("restore_seq");
                    send({{"op","copy"},{"id",state->record.id},{"revision",std::string(64,'b')}});
                    // A rejected command does not change model generation. Use a subsequent
                    // command on this socket as an ordered barrier before checking the intent.
                    send({{"op","search"},{"text","Host reference"}}); state->phase=6; break;
                case 6:
                    if(snapshot.at("query")!="Host reference") break;
                    if(snapshot.at("restore_seq")!=state->sequence) Die("stale resource copy accepted");
                    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit}|fcitx::CapabilityFlag::Password); state->phase=7; break;
                case 7:
                    if(!snapshot.at("password_field").get<bool>()) break;
                    if(!snapshot.at("cards").empty() || !snapshot.at("restore_id").get<std::string>().empty()) Die("resource protection exposed path or restore intent");
                    std::cout<<"ok: Fcitx resource metadata search, file-copy identity, activation acknowledgment, management, stale revisions and protection passed\n";
                    RunCaptureSuite(instance,frontend,uuid,ic,dump_path);return true;
                }
                timer->setTime(fcitx::now(CLOCK_MONOTONIC)+20000);timer->setOneShot();return true;
            } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';g_exit_status=EXIT_FAILURE;instance.exit();return true;}
        });
}

std::unique_ptr<fcitx::EventSourceTime> history_timer;
void RunHistorySuite(fcitx::Instance& instance, fcitx::AddonInstance* frontend, fcitx::ICUUID uuid,
                     fcitx::InputContext* ic, const std::string& dump_path) {
    using namespace rimes::capsule;
    SendKey(frontend, uuid, "Escape");
    SendKey(frontend, uuid, "Control+Shift+V");
    struct State {
        int fd = -1, phase = 0;
        ClipboardEntry text, image;
        ~State() {
            if (fd >= 0)
                close(fd);
        }
    };
    auto state = std::make_shared<State>();
    ClipboardHistoryStore store(ClipboardHistoryStore::DefaultRoot());
    state->image = store.Capture(ClipboardArchive{{{"image/png", "binary fixture"}}},
                                 {"x11", "Fixture", "image"});
    state->text =
        store.Capture(ClipboardArchive{{{"text/plain;charset=utf-8", "全文中文\nsecond line"}}},
                      {"x11", "Fixture", "text"});
    state->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, std::getenv("RIMES_CAPSULE_SOCKET"),
                 sizeof(address.sun_path) - 1);
    if (state->fd < 0 || connect(state->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
        Die("history socket");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    history_timer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 20000, 20000,
        [&, frontend, uuid, ic, dump_path, state, deadline](fcitx::EventSourceTime* timer,
                                                            uint64_t) {
            try {
                if (std::chrono::steady_clock::now() > deadline)
                    Die("history timeout phase " + std::to_string(state->phase));
                char buffer[65536];
                while (recv(state->fd, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {
                }
                auto send = [&](nlohmann::json command) {
                    command["v"] = 1;
                    std::string frame;
                    EncodeFrame(command.dump(), &frame);
                    if (::send(state->fd, frame.data(), frame.size(), MSG_NOSIGNAL) !=
                        static_cast<ssize_t>(frame.size()))
                        Die("history send phase " + std::to_string(state->phase) + " errno " +
                            std::to_string(errno));
                };
                const auto snapshot = nlohmann::json::parse(ReadDump(dump_path));
                auto loaded = [&] {
                    return snapshot.at("module") == "temporary" &&
                           !snapshot.at("loading").get<bool>() && snapshot.at("count") == 2;
                };
                switch (state->phase) {
                case 0:
                    send({{"op", "module"}, {"text", "temporary"}});
                    state->phase = 1;
                    break;
                case 1:
                    if (!loaded())
                        break;
                    send({{"op", "select"}, {"id", state->text.id}});
                    state->phase = 2;
                    break;
                case 2:
                    if (snapshot.at("selected_id") != state->text.id)
                        break;
                    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>(
                        "全文中文\nsecond line");
                    SendKey(frontend, uuid, "Return");
                    state->phase = 3;
                    break;
                case 3:
                    if (snapshot.at("visible").get<bool>())
                        break;
                    SendKey(frontend, uuid, "Control+Shift+V");
                    state->phase = 4;
                    break;
                case 4:
                    if (!loaded())
                        break;
                    send({{"op", "activate"},
                          {"id", state->image.id},
                          {"revision", state->image.revision}});
                    state->phase = 5;
                    break;
                case 5:
                    if (snapshot.at("restore_id") != state->image.id)
                        break;
                    if (!snapshot.at("restore_close").get<bool>() ||
                        !snapshot.at("visible").get<bool>())
                        Die("rich activation did not await clipboard acknowledgment");
                    send({{"op", "favorite"},
                          {"id", state->image.id},
                          {"revision", state->image.revision}});
                    state->phase = 6;
                    break;
                case 6: {
                    ClipboardHistoryStore history(ClipboardHistoryStore::DefaultRoot());
                    auto current = history.Get(state->image.id);
                    if (!current || !current->favorite || !loaded())
                        break;
                    send({{"op", "remove"},
                          {"id", state->image.id},
                          {"revision", state->image.revision}}); // stale CAS must not remove
                    state->image = *current;
                    state->phase = 7;
                    break;
                }
                case 7:
                    if (!loaded() || snapshot.at("cards")
                                             .at(snapshot.at("selected").get<int>())
                                             .at("revision") != state->image.revision)
                        break;
                    send({{"op", "search"}, {"text", "second line"}});
                    state->phase = 8;
                    break;
                case 8:
                    if (snapshot.at("loading").get<bool>() || snapshot.at("count") != 1)
                        break;
                    if (snapshot.at("cards")[0].at("id") != state->text.id)
                        Die("history full text search");
                    SendKey(frontend, uuid, "Escape");
                    state->phase = 9;
                    break;
                case 9:
                    if (!loaded())
                        break;
                    if (!snapshot.at("visible").get<bool>() || snapshot.at("query") != "")
                        Die("Escape must clear search before closing");
                    SendKey(frontend, uuid, "Down");
                    state->phase = 10;
                    break;
                case 10:
                    if (snapshot.at("loading").get<bool>() || snapshot.at("filter") != "text")
                        break;
                    if (snapshot.at("count") != 1 || snapshot.at("selected_id") != state->text.id)
                        Die("keyboard clipboard filter");
                    SendKey(frontend, uuid, "Up");
                    state->phase = 11;
                    break;
                case 11:
                    if (!loaded())
                        break;
                    send({{"op", "select"}, {"id", state->text.id}});
                    state->phase = 12;
                    break;
                case 12:
                    if (snapshot.at("selected_id") != state->text.id)
                        break;
                    SendKey(frontend, uuid, "Delete");
                    state->phase = 13;
                    break;
                case 13:
                    if (snapshot.at("loading").get<bool>() || snapshot.at("count") != 1)
                        break;
                    if (snapshot.at("cards")[0].at("id") != state->image.id)
                        Die("Delete removed the wrong history item");
                    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit} |
                                           fcitx::CapabilityFlag::Password);
                    send({{"op", "copy"},
                          {"id", state->image.id},
                          {"revision", state->image.revision}});
                    state->phase = 14;
                    break;
                case 14:
                    if (!snapshot.at("password_field").get<bool>())
                        break;
                    if (!snapshot.at("cards").empty() ||
                        !snapshot.at("restore_id").get<std::string>().empty() ||
                        snapshot.at("clipboard_capture").get<bool>())
                        Die("protected history/action exposure");
                    std::cout << "ok: Fcitx history text commits, rich explicit restore, favorite "
                                 "CAS, full-text search and protection passed\n";
                    RunResourceSuite(instance, frontend, uuid, ic, dump_path);
                    return true;
                }
                timer->setTime(fcitx::now(CLOCK_MONOTONIC) + 20000);
                timer->setOneShot();
                return true;
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                g_exit_status = EXIT_FAILURE;
                instance.exit();
                return true;
            }
        });
}

std::unique_ptr<fcitx::EventSourceTime> detail_timer;
void RunDetailSuite(fcitx::Instance& instance, fcitx::AddonInstance* frontend, fcitx::ICUUID uuid,
                    fcitx::InputContext* ic, const std::string& dump_path) {
    using namespace rimes::capsule;
    struct State {
        int fd = -1, phase = 0;
        Record note;
        std::string incoming, large;
        Snapshot wire;
        ~State() {
            if (fd >= 0)
                close(fd);
        }
    };
    auto state = std::make_shared<State>();
    state->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, std::getenv("RIMES_CAPSULE_SOCKET"),
                 sizeof(address.sun_path) - 1);
    if (state->fd < 0 || connect(state->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
        Die("detail socket connection");
    ContentStore store(ContentStore::DefaultRoot());
    std::string error;
    if (store.Get(kDefaultEntryId, &state->note, &error) != StoreStatus::Ok)
        Die("detail fixture missing");
    state->note.content = "# Login\n- 账号: user@example.com\n- 代码: "
                          "αβ\n```\nline one\nline two\n```";
    if (store.Put(state->note, state->note.revision, &state->note, &error) != StoreStatus::Ok)
        Die("detail fixture update");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    detail_timer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 20000, 20000,
        [&, frontend, uuid, ic, dump_path, state, deadline](fcitx::EventSourceTime* timer,
                                                            uint64_t) {
            try {
                if (std::chrono::steady_clock::now() > deadline)
                    Die("detail integration timed out at phase " + std::to_string(state->phase));
                char chunk[65536];
                for (;;) {
                    const auto size = recv(state->fd, chunk, sizeof(chunk), MSG_DONTWAIT);
                    if (size < 0 && errno == EINTR)
                        continue;
                    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        break;
                    if (size <= 0)
                        Die("detail socket disconnected");
                    state->incoming.append(chunk, size);
                }
                while (state->incoming.size() >= 4) {
                    std::uint32_t size = 0;
                    if (!DecodeFrameHeader(state->incoming.data(), &size))
                        Die("detail frame header");
                    if (state->incoming.size() < size + 4)
                        break;
                    Snapshot snapshot;
                    if (DecodeSnapshot(std::string_view(state->incoming).substr(4, size),
                                       &snapshot))
                        state->wire = std::move(snapshot);
                    state->incoming.erase(0, size + 4);
                }
                auto send = [&](const std::string& op, int index = -1,
                                const std::string& revision = "") {
                    std::string frame;
                    const auto raw = nlohmann::json{
                        {"v", 1},
                        {"op", op},
                        {"id", state->note.id},
                        {"revision", revision},
                        {"index", index}}.dump();
                    if (!EncodeFrame(raw, &frame) ||
                        ::send(state->fd, frame.data(), frame.size(), MSG_NOSIGNAL) !=
                            static_cast<ssize_t>(frame.size()))
                        Die("detail command send");
                };
                const auto snapshot = nlohmann::json::parse(ReadDump(dump_path));
                auto loaded = [&] {
                    return !snapshot.at("loading").get<bool>() && !snapshot.at("cards").empty() &&
                           snapshot.at("cards")[0].at("revision") == state->note.revision;
                };
                switch (state->phase) {
                case 0:
                    if (!loaded())
                        break;
                    send("detail");
                    state->phase = 1;
                    break;
                case 1:
                    if (!snapshot.at("detail").get<bool>())
                        break;
                    if (snapshot.at("detail_selected") != 0 || snapshot.at("detail_count") != 3)
                        Die("native detail rows/selection differ");
                    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("user@example.com");
                    SendKey(frontend, uuid, "Return");
                    ExpectContains(ReadDump(dump_path), "\"visible\":true",
                                   "row insertion closed the rail");
                    SendKey(frontend, uuid, "Control+c");
                    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("αβ");
                    send("row-insert", 1, state->note.revision);
                    state->phase = 2;
                    break;
                case 2:
                    if (snapshot.at("detail_selected") != 1)
                        break;
                    if (snapshot.at("last_copied") != "user@example.com")
                        Die("row copy included its label");
                    {
                        ContentStore storage(ContentStore::DefaultRoot());
                        std::string err;
                        const auto stale = state->note.revision;
                        state->note.content = "- 账号: external edit";
                        if (storage.Put(state->note, stale, &state->note, &err) != StoreStatus::Ok)
                            Die("external edit failed");
                        send("row-insert", 1, stale); // Must not insert an obsolete row
                                                      // even before inotify reload.
                    }
                    state->phase = 3;
                    break;
                case 3:
                    if (!loaded() || snapshot.at("detail").get<bool>())
                        break;
                    send("detail");
                    state->phase = 4;
                    break;
                case 4:
                    if (!snapshot.at("detail").get<bool>())
                        break;
                    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit} |
                                           fcitx::CapabilityFlag::Password);
                    {
                        const auto hidden = nlohmann::json::parse(ReadDump(dump_path));
                        if (hidden.at("detail").get<bool>() || !hidden.at("detail_rows").empty() ||
                            hidden.at("visible").get<bool>())
                            Die("protected detail remained exposed");
                    }
                    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit} |
                                           fcitx::CapabilityFlag::ClientUnfocusCommit);
                    instance.setCurrentInputMethod(ic, "rimes", true);
                    ic->focusIn();
                    SendKey(frontend, uuid, "Control+Shift+V");
                    for (int i = 0; i < 180000; ++i)
                        state->large += "👩🏽‍💻";
                    state->note.content = state->large;
                    {
                        ContentStore storage(ContentStore::DefaultRoot());
                        std::string err;
                        if (storage.Put(state->note, state->note.revision, &state->note, &err) !=
                            StoreStatus::Ok)
                            Die("large Unicode fixture failed");
                    }
                    state->phase = 5;
                    break;
                case 5:
                    if (!loaded())
                        break;
                    SendKey(frontend, uuid, "Control+c");
                    state->phase = 6;
                    break;
                case 6:
                    if (state->wire.last_copied != state->large)
                        break;
                    std::cout << "ok: real Fcitx details insert exact row values, "
                                 "reject stale revisions, conceal on protected input "
                                 "and copy 2.7 MiB Unicode over fragmented IPC\n";
                    SendKey(frontend, uuid, "Control+Shift+B");
                    SendKey(frontend, uuid, "Return");
                    ExpectContains(ReadDump(dump_path), "\"visible\":true",
                                   "Buffer Return must not close Capsule");
                    std::cout << "ok: Buffer capturing keeps Capsule from consuming Return\n";
                    SendKey(frontend, uuid, "Control+Shift+B");
                    RunHistorySuite(instance, frontend, uuid, ic, dump_path);
                    return true;
                }
                timer->setTime(fcitx::now(CLOCK_MONOTONIC) + 20000);
                timer->setOneShot();
                return true;
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                g_exit_status = EXIT_FAILURE;
                instance.exit();
                return true;
            }
        });
}

void RunCapsuleSuite(fcitx::Instance& instance, fcitx::AddonInstance* frontend,
                     const fcitx::ICUUID& uuid, fcitx::InputContext* ic,
                     const std::string& dump_path) {
    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit} |
                           fcitx::CapabilityFlag::ClientUnfocusCommit);

    SendKey(frontend, uuid, "Control+Shift+V");
    for (int i=0;i<5 && ReadDump(dump_path).find("\"module\":\"notes\"")==std::string::npos;++i) SendKey(frontend,uuid,"Tab");
    auto snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":true", "Ctrl+Shift+V did not show the rail");
    ExpectContains(snapshot, "\"armed\":true", "Ctrl+Shift+V did not arm the session");
    ExpectContains(snapshot, "RIMES 默认词条", "seed note is missing");
    ExpectMissing(snapshot, "\\u200b", "snapshot must not advertise a ZWSP");
    ExpectNoZwsp(ic, "open rail");
    std::cout << "ok: Ctrl+Shift+V showed an armed rail with the seed note\n";

    SendKey(frontend, uuid, "Control+Shift+V");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":false", "hotkey did not hide");
    ExpectContains(snapshot, "\"armed\":false", "hotkey did not disarm");
    std::cout << "ok: Ctrl+Shift+V hid the rail\n";

    SendKey(frontend, uuid, "Control+Shift+V");
    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("RIMES");
    SendKey(frontend, uuid, "Return");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":false", "activate must close the rail");
    std::cout << "ok: Return inserted the seed note and closed the rail\n";

    SendKey(frontend, uuid, "Control+Shift+V");
    Type(frontend, uuid, "nihao");
    ExpectNoZwsp(ic, "search while armed");
    SendKey(frontend, uuid, "space");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"query\":\"nihao ", "armed typing must edit the search query");
    ExpectMissing(snapshot, "你好", "armed typing must not leak into cards");
    std::cout << "ok: armed printable keys search and do not leak 你好\n";

    SendKey(frontend, uuid, "Escape");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":true", "first Escape must leave the rail open");
    ExpectContains(snapshot, "\"query\":\"\"", "first Escape must clear the search");
    SendKey(frontend, uuid, "Escape");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":false", "second Escape did not hide");
    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("你好");
    Type(frontend, uuid, "nihao");
    SendKey(frontend, uuid, "space");
    std::cout << "ok: after close, Rime commits go to the host\n";

    SendKey(frontend, uuid, "Control+Shift+V");
    ic->focusOut();
    ic->focusIn();
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"armed\":false", "same-IC reactivate must disarm");
    ExpectContains(snapshot, "\"visible\":true", "field switch keeps the rail");
    std::cout << "ok: field switch disarms; later Return is not consumed by Capsule\n";

    SendKey(frontend, uuid, "Control+Shift+V");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"armed\":true", "hotkey on a visible disarmed rail re-arms");
    ExpectContains(snapshot, "\"visible\":true", "re-arm keeps the rail up");
    ExpectContains(snapshot, "RIMES 默认词条", "re-arm shows the seed after hide cleared search");
    SendKey(frontend, uuid, "Control+Shift+V");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":false", "second hotkey while armed closes");
    SendKey(frontend, uuid, "Control+Shift+V");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"armed\":true", "open again after the close");
    const auto uuid2 =
        frontend->call<fcitx::ITestFrontend::createInputContext>("rimes-capsule-other");
    auto* ic2 = instance.inputContextManager().findByUUID(uuid2);
    if (ic2 == nullptr) {
        Die("second test input context was not created");
    }
    instance.setCurrentInputMethod(ic2, "rimes", true);
    ic2->focusIn();
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"armed\":false", "focusing another IC must disarm");
    ExpectContains(snapshot, "\"visible\":true", "focus change must keep the rail");
    const bool other_handled =
        frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid2, fcitx::Key("Escape"), false);
    frontend->call<fcitx::ITestFrontend::keyEvent>(uuid2, fcitx::Key("Escape"), true);
    if (other_handled) {
        Die("Escape on a disarmed IC was swallowed");
    }
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":true", "Escape on another IC must not close Capsule");
    std::cout << "ok: Escape is scoped to the armed input context\n";
    frontend->call<fcitx::ITestFrontend::destroyInputContext>(uuid2);

    ic->focusIn();
    SendKey(frontend, uuid, "Escape");
    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit} |
                           fcitx::CapabilityFlag::ClientUnfocusCommit |
                           fcitx::CapabilityFlag::Password);
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":false", "password capability must hide the rail");
    ExpectContains(snapshot, "\"password_field\":true", "password flag missing");
    SendKey(frontend, uuid, "Control+Shift+V");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":false", "password field must refuse the rail");
    ExpectContains(snapshot, "\"password_field\":true", "password flag must survive a toggle");
    std::cout << "ok: password field refuses Capsule\n";

    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit} |
                           fcitx::CapabilityFlag::ClientUnfocusCommit);
    instance.setCurrentInputMethod(ic, "rimes", true);
    ic->focusIn();
    SendKey(frontend, uuid, "Control+Shift+V");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"armed\":true", "normal field can re-arm after password");
    SendKey(frontend, uuid, "Control+c");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"last_copied\":\"RIMES\"", "Ctrl+C did not copy the seed body");
    ExpectContains(snapshot, "\"copy_seq\":1", "first Ctrl+C must bump copy_seq");
    Type(frontend, uuid, "z");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"copy_seq\":1", "search must not bump copy_seq");
    ExpectContains(snapshot, "\"last_copied\":\"RIMES\"", "search must not clear last_copied");
    SendKey(frontend, uuid, "Control+Shift+V");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":false", "armed hotkey closes");
    ExpectContains(snapshot, "\"last_copied\":\"\"", "Hide must clear last_copied");
    ExpectContains(snapshot, "\"copy_seq\":1", "Hide must not bump copy_seq");
    SendKey(frontend, uuid, "Control+Shift+V");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"copy_seq\":1", "reopen must not look like a new copy");
    ExpectContains(snapshot, "\"last_copied\":\"\"", "reopen must not restore last_copied");
    SendKey(frontend, uuid, "Control+c");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"copy_seq\":2", "second Ctrl+C on the same card must bump copy_seq");
    ExpectContains(snapshot, "\"last_copied\":\"RIMES\"", "second Ctrl+C must copy again");
    std::cout << "ok: Ctrl+C is edge-triggered and can copy the same card twice\n";

    auto* engine = instance.inputMethodEngine(ic);
    if (!engine || !engine->getConfig()) Die("missing plugin host configuration");
    rimes::plugins::PackageStore packages(rimes::runtime::UserRoot()/"plugins",rimes::plugins::OfficialCatalog(),
        rimes::plugins::BundledPackages(),rimes::plugins::CompiledAdapters(),RIMES_PRODUCT_VERSION);
    const auto notes=packages.Inspect("builtin.capsule.notes");
    packages.Enable(notes.id,false,notes.grant); engine->reloadConfig();
    snapshot=ReadDump(dump_path);
    ExpectContains(snapshot,"\"armed\":false","plugin disable must revoke insertion");
    ExpectContains(snapshot,"\"last_copied\":\"\"","plugin disable must clear clipboard intent");
    ExpectMissing(snapshot,"RIMES 默认词条","disabled notes still exposed");
    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("你好");
    Type(frontend,uuid,"nihao"); SendKey(frontend,uuid,"space");
    rimes::capsule::ContentStore content(rimes::capsule::ContentStore::DefaultRoot());
    rimes::capsule::Record saved; std::string error;
    if(content.Get(rimes::capsule::kDefaultEntryId,&saved,&error)!=rimes::capsule::StoreStatus::Ok) Die("disabled plugin lost its data");
    const auto disabled=packages.Inspect(notes.id);
    packages.Enable(notes.id,true,disabled.grant); engine->reloadConfig();
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    resume_timer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 50000, 50000,
        [&, frontend, uuid, ic, dump_path, until, opened = false](fcitx::EventSourceTime* timer,
                                                                  uint64_t) mutable {
            try {
                if(ReadDump(dump_path).find("\"loading\":true")!=std::string::npos) {
                    if(std::chrono::steady_clock::now()>until)
                        Die("Capsule reload timed out");
                    timer->setTime(fcitx::now(CLOCK_MONOTONIC)+50000);
                    timer->setOneShot();
                    return true;
                }
                if (!opened) {
                    SendKey(frontend, uuid, "Control+Shift+V");
                    for (int i = 0; i < 5 && ReadDump(dump_path).find("\"module\":\"notes\"") ==
                                                 std::string::npos;
                         ++i)
                        SendKey(frontend, uuid, "Tab");
                    opened = true;
                }
                const auto restored=ReadDump(dump_path);
                if (restored.find("\"loading\":true") != std::string::npos) {
                    if (std::chrono::steady_clock::now() > until)
                        Die("re-enabled notes reload timed out");
                    timer->setTime(fcitx::now(CLOCK_MONOTONIC) + 20000);
                    timer->setOneShot();
                    return true;
                }
                ExpectContains(restored,"\"armed\":true","re-enabled plugin needs explicit target capture");
                ExpectContains(restored,"RIMES 默认词条","re-enabled module failed to reload data");
                std::cout<< "ok: package disable revokes capture, preserves data "
                             "and ordinary typing; re-enable reloads notes\n";
                RunDetailSuite(instance, frontend, uuid, ic, dump_path);
                return true;
            } catch(const TestFailed&) {
            }
            instance.exit();
            return true;
        });
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: rimes-capsule-fcitx-e2e <build-dir> <addon-rel-dir> "
                     "<data-rel-dir>\n";
        return EXIT_FAILURE;
    }

    try {
        const auto dump_path = IsolateDirs();
        fcitx::setupTestingEnvironment(argv[1], {argv[2]}, {argv[3]});
        // setupTestingEnvironment deliberately disables config writes. This
        // suite verifies a real saved plugin preference in its isolated root.
        const auto config = std::filesystem::path(dump_path).parent_path() / "config";
        std::filesystem::create_directories(config / "conf");
        unsetenv("SKIP_FCITX_USER_PATH");
        setenv("FCITX_CONFIG_HOME", config.c_str(), 1);
        setenv("FCITX_DATA_HOME", (config / "data").c_str(), 1);
        setenv("XDG_CACHE_HOME", (config / "cache").c_str(), 1);

        char arg0[] = "rimes-capsule-fcitx-e2e";
        char arg1[] = "--disable=all";
        char arg2[] = "--enable=testfrontend,testui,rimes";
        char* args[] = {arg0, arg1, arg2};
        fcitx::Instance instance(3, args);
        instance.addonManager().registerDefaultLoader(nullptr);

        fcitx::EventDispatcher dispatcher;
        dispatcher.attach(&instance.eventLoop());

        fcitx::AddonInstance* frontend = nullptr;
        fcitx::ICUUID uuid{};
        fcitx::InputContext* ic = nullptr;
        std::unique_ptr<fcitx::EventSourceTime> wait_timer;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        bool suite_started = false;

        dispatcher.schedule([&]() {
            auto& manager = instance.inputMethodManager();
            if (manager.entry("rimes") == nullptr) {
                Die("input method 'rimes' was not registered");
            }
            if (manager.groupCount() == 0) {
                manager.addEmptyGroup("Default");
                manager.setGroupOrder({"Default"});
            }
            fcitx::InputMethodGroup group("Default");
            group.setDefaultLayout("us");
            group.inputMethodList().emplace_back(fcitx::InputMethodGroupItem("rimes"));
            group.setDefaultInputMethod("rimes");
            manager.setGroup(group);
            manager.setCurrentGroup("Default");

            frontend = instance.addonManager().addon("testfrontend", true);
            if (frontend == nullptr) {
                Die("testfrontend addon did not load");
            }
            uuid = frontend->call<fcitx::ITestFrontend::createInputContext>("rimes-capsule-e2e");
            ic = instance.inputContextManager().findByUUID(uuid);
            if (ic == nullptr) {
                Die("test input context was not created");
            }
            instance.setCurrentInputMethod(ic, "rimes", true);
            if (instance.inputMethod(ic) != "rimes") {
                Die("could not switch the test context to rimes");
            }

            wait_timer = instance.eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 50000, 50000,
                [&](fcitx::EventSourceTime* source, uint64_t) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        Die("addon stayed in Deploying; deploy-ready notify never ran");
                    }
                    auto* ime = instance.inputMethodEngine(ic);
                    const auto* entry = instance.inputMethodEntry(ic);
                    if (ime != nullptr && entry != nullptr &&
                        ime->subMode(*entry, *ic) == "Deploying") {
                        source->setTime(fcitx::now(CLOCK_MONOTONIC) + 200000);
                        source->setOneShot();
                        return true;
                    }
                    if (ReadDump(dump_path).find("\"loading\":true")!=std::string::npos) {
                        source->setTime(fcitx::now(CLOCK_MONOTONIC)+50000);source->setOneShot();return true;
                    }
                    if (suite_started) {
                        return true;
                    }
                    suite_started = true;
                    std::cout << "ok: capsule testfrontend left Deploying\n";
                    try {
                        RunCapsuleSuite(instance, frontend, uuid, ic, dump_path);
                    } catch (const TestFailed&) {
                        instance.exit();
                    }
                    return true;
                });
        });

        try {
            const int rc = instance.exec();
            return g_exit_status != EXIT_SUCCESS ? g_exit_status : rc;
        } catch (const fcitx::InstanceQuietQuit&) {
            return g_exit_status;
        }
    } catch (const TestFailed&) {
        return EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
