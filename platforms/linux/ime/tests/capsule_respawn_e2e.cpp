#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <signal.h>
#include <stdexcept>
#include <string>
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
#include <fcitx/instance.h>
#include <testfrontend_public.h>

namespace {

int g_exit_status = EXIT_SUCCESS;

struct TestFailed : std::runtime_error {
    explicit TestFailed(const std::string& message) : std::runtime_error(message) {}
};

void Die(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    g_exit_status = EXIT_FAILURE;
    throw TestFailed(message);
}

void SendKey(fcitx::AddonInstance* frontend, const fcitx::ICUUID& uuid, const char* name) {
    frontend->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(name), false);
    frontend->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(name), true);
}

std::string IsolateDirs(const std::string& stub_path) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("rimes-capsule-respawn-" + std::to_string(getpid()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "user" / "log", error);
    if (error) {
        Die("could not create isolated dirs: " + error.message());
    }
    static std::string user_env;
    static std::string log_env;
    static std::string socket_env;
    static std::string dump_env;
    static std::string ui_env;
    static std::string clipboard_log;
    static std::string path_env;
    static std::string wl_copy_log;
    user_env = (root / "user").string();
    log_env = (root / "user" / "log").string();
    socket_env = (root / "rimes-capsule.sock").string();
    dump_env = (root / "capsule-snapshot.json").string();
    clipboard_log = (root / "clipboard-writes.log").string();
    wl_copy_log = (root / "wl-copy.log").string();
    const auto fake_bin = root / "bin";
    std::filesystem::create_directories(fake_bin, error);
    {
        const auto script = fake_bin / "wl-copy";
        std::ofstream out(script);
        out << "#!/bin/sh\n"
            << "cat >> '" << wl_copy_log << "'\n"
            << "printf '\\n' >> '" << wl_copy_log << "'\n";
        out.close();
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);
    }
    const char* old_path = std::getenv("PATH");
    path_env = fake_bin.string() + ":" + (old_path != nullptr ? old_path : "/usr/bin:/bin");
    ui_env = stub_path;
    setenv("PATH", path_env.c_str(), 1);
    setenv("RIMES_USER_DIR", user_env.c_str(), 1);
    setenv("RIMES_LOG_DIR", log_env.c_str(), 1);
    setenv("RIMES_BUFFER_HEADLESS", "1", 1);
    setenv("RIMES_CAPSULE_SOCKET", socket_env.c_str(), 1);
    setenv("RIMES_CAPSULE_DUMP", dump_env.c_str(), 1);
    setenv("RIMES_CAPSULE_UI", ui_env.c_str(), 1);
    setenv("RIMES_CAPSULE_CLIPBOARD_LOG", clipboard_log.c_str(), 1);
    setenv("RIMES_CAPSULE_WAYLAND_CLIPBOARD", "1", 1);
    setenv("RIMES_CAPSULE_CONNECT_DELAY_MS", "1500", 1);
    unsetenv("RIMES_CAPSULE_HEADLESS");
    setenv("RIMES_CAPSULE_FOCUS_GRACE_MS", "200", 1);
    return dump_env;
}

int LogLineCount(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return -1;
    }
    std::ifstream in(path);
    if (!in) {
        return 0;
    }
    int lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            ++lines;
        }
    }
    return lines;
}

int ClipboardWriteCount() { return LogLineCount(std::getenv("RIMES_CAPSULE_CLIPBOARD_LOG")); }

int WlCopyCount() {
    const char* clipboard = std::getenv("RIMES_CAPSULE_CLIPBOARD_LOG");
    if (clipboard == nullptr || clipboard[0] == '\0') {
        return -1;
    }
    const auto log = std::filesystem::path(clipboard).parent_path() / "wl-copy.log";
    return LogLineCount(log.c_str());
}

std::string ReadDump(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        Die("capsule dump is missing at " + path);
    }
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int UiClientCount(const std::string& dump_path) {
    const auto json = ReadDump(dump_path);
    const auto key = json.find("\"ui_clients\":");
    if (key == std::string::npos) {
        return -1;
    }
    return std::atoi(json.c_str() + key + std::strlen("\"ui_clients\":"));
}

int CountUiProcesses(const std::string& stub_path) {
    std::error_code canon_error;
    const auto wanted = std::filesystem::weakly_canonical(stub_path, canon_error);
    int count = 0;
    DIR* proc = opendir("/proc");
    if (proc == nullptr) {
        return -1;
    }
    while (dirent* entry = readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        const std::string exe = std::string("/proc/") + entry->d_name + "/exe";
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(exe, error);
        if (error) {
            continue;
        }
        if (resolved == wanted) {
            ++count;
        }
    }
    closedir(proc);
    return count;
}

void KillUiProcesses(const std::string& stub_path) {
    std::error_code canon_error;
    const auto wanted = std::filesystem::weakly_canonical(stub_path, canon_error);
    DIR* proc = opendir("/proc");
    if (proc == nullptr) {
        return;
    }
    while (dirent* entry = readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        const std::string exe = std::string("/proc/") + entry->d_name + "/exe";
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(exe, error);
        if (error || resolved != wanted) {
            continue;
        }
        const pid_t pid = static_cast<pid_t>(std::atoi(entry->d_name));
        if (pid > 0) {
            kill(pid, SIGKILL);
        }
    }
    closedir(proc);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: rimes-capsule-respawn-e2e <build-dir> <addon-rel-dir> <data-rel-dir>\n";
        return EXIT_FAILURE;
    }

    try {
        const auto build_dir = std::filesystem::path(argv[1]);
        const auto stub_path = (build_dir / "rimes-capsule-ui-stub").string();
        if (access(stub_path.c_str(), X_OK) != 0) {
            std::cerr << "FAIL: missing UI stub at " << stub_path << '\n';
            return EXIT_FAILURE;
        }
        const auto dump_path = IsolateDirs(stub_path);
        fcitx::setupTestingEnvironment(argv[1], {argv[2]}, {argv[3]});

        char arg0[] = "rimes-capsule-respawn-e2e";
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
        std::unique_ptr<fcitx::EventSourceTime> connected_timer;
        std::unique_ptr<fcitx::EventSourceTime> copy_timer;
        std::unique_ptr<fcitx::EventSourceTime> after_kill_timer;
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
            uuid = frontend->call<fcitx::ITestFrontend::createInputContext>("rimes-capsule-respawn");
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
                    if (suite_started) {
                        return true;
                    }
                    suite_started = true;
                    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit} |
                                           fcitx::CapabilityFlag::ClientUnfocusCommit);
                    SendKey(frontend, uuid, "Control+Shift+V");
                    const auto opened = ReadDump(dump_path);
                    if (opened.find("\"visible\":true") == std::string::npos) {
                        Die("Ctrl+Shift+V did not show the rail");
                    }
                    connected_timer = instance.eventLoop().addTimeEvent(
                        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 200000, 200000,
                        [&, stub_path](fcitx::EventSourceTime* poll, uint64_t) {
                            if (std::chrono::steady_clock::now() > deadline) {
                                Die("slow UI stub never connected");
                            }
                            if (UiClientCount(dump_path) < 1) {
                                poll->setTime(fcitx::now(CLOCK_MONOTONIC) + 200000);
                                poll->setOneShot();
                                return true;
                            }
                            if (CountUiProcesses(stub_path) != 1) {
                                Die("expected exactly one UI process before kill");
                            }
                            // This suite exercises note-copy replay. The first
                            // enabled module may be Temporary after startup.
                            const auto current = ReadDump(dump_path);
                            if (current.find("\"module\":\"notes\"") == std::string::npos) {
                                SendKey(frontend, uuid, "Tab");
                                poll->setTime(fcitx::now(CLOCK_MONOTONIC) + 50000);
                                poll->setOneShot();
                                return true;
                            }
                            if (current.find("\"loading\":true") != std::string::npos) {
                                poll->setTime(fcitx::now(CLOCK_MONOTONIC) + 50000);
                                poll->setOneShot();
                                return true;
                            }
                            std::cout << "ok: first Capsule UI connected with notes loaded\n";
                            SendKey(frontend, uuid, "Control+c");
                            if (ReadDump(dump_path).find("\"copy_seq\":1") == std::string::npos) {
                                Die("Ctrl+C did not increment copy_seq");
                            }
                            copy_timer = instance.eventLoop().addTimeEvent(
                                CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 50000, 50000,
                                [&, stub_path](fcitx::EventSourceTime* copy_poll, uint64_t) {
                                    if (std::chrono::steady_clock::now() > deadline) {
                                        Die("stub never logged the first Ctrl+C clipboard write");
                                    }
                                    if (ClipboardWriteCount() < 1 || WlCopyCount() < 1) {
                                        copy_poll->setTime(fcitx::now(CLOCK_MONOTONIC) + 50000);
                                        copy_poll->setOneShot();
                                        return true;
                                    }
                                    if (ClipboardWriteCount() != 1) {
                                        Die("stub must write the clipboard once for the first Ctrl+C");
                                    }
                                    if (WlCopyCount() != 1) {
                                        Die("stub must spawn wl-copy once for the first Ctrl+C");
                                    }
                                    KillUiProcesses(stub_path);
                                    after_kill_timer = instance.eventLoop().addTimeEvent(
                                CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 2000000, 0,
                                [&, stub_path](fcitx::EventSourceTime*, uint64_t) {
                                    try {
                                        const int processes = CountUiProcesses(stub_path);
                                        const int clients = UiClientCount(dump_path);
                                        if (processes != 1) {
                                            Die("after kill+2s expected 1 UI process, got " +
                                                std::to_string(processes));
                                        }
                                        if (clients != 1) {
                                            Die("after kill+2s expected 1 UI client, got " +
                                                std::to_string(clients));
                                        }
                                        if (ClipboardWriteCount() != 1) {
                                            Die("respawned UI re-wrote the clipboard");
                                        }
                                        if (WlCopyCount() != 1) {
                                            Die("respawned UI re-invoked wl-copy");
                                        }
                                        std::cout << "ok: Capsule UI respawn kept a single delayed client\n";
                                        std::cout << "ok: respawned UI did not replay Ctrl+C onto the clipboard\n";
                                        std::cout << "ok: respawned UI did not re-invoke wl-copy\n";
                                        frontend->call<fcitx::ITestFrontend::destroyInputContext>(
                                            uuid);
                                        instance.exit();
                                    } catch (const TestFailed&) {
                                        instance.exit();
                                    }
                                    return true;
                                });
                                    return true;
                                });
                            return true;
                        });
                    return true;
                });
        });

        try {
            const int rc = instance.exec();
            KillUiProcesses(stub_path);
            return g_exit_status != EXIT_SUCCESS ? g_exit_status : rc;
        } catch (const fcitx::InstanceQuietQuit&) {
            KillUiProcesses(stub_path);
            return g_exit_status;
        }
    } catch (const TestFailed&) {
        return EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
