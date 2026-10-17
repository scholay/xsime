#include "desktop/desktop_shortcuts.hpp"
#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/dbus/variant.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <xcb/xcb.h>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
using namespace fcitx;
using Json = rimes::plugins::Json;
using Properties = std::vector<dbus::DictEntry<std::string, dbus::Variant>>;
using Bindings = std::vector<dbus::DBusStruct<std::string, Properties>>;
constexpr auto portal = "org.freedesktop.portal.Desktop";
constexpr auto portalPath = "/org/freedesktop/portal/desktop";
constexpr auto shortcutsInterface = "org.freedesktop.portal.GlobalShortcuts";
void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::string Property(const Properties& values, const std::string& key) {
    for (const auto& value : values) if (value.key() == key) return value.value().dataAs<std::string>();
    throw std::runtime_error("missing portal property");
}
struct PortalFixture {
    dbus::Bus bus{dbus::BusType::Session};
    std::vector<std::unique_ptr<dbus::Slot>> objects;
    std::string session, delayed, delayedSession;
    bool defer = false, deny = false;
    unsigned creates = 0, closes = 0;
    Bindings requested;
    explicit PortalFixture(EventLoop& loop) {
        bus.attachEventLoop(&loop);
        Check(bus.requestName(portal, dbus::RequestNameFlag::None), "portal fixture name");
        dbus::VariantTypeRegistry::defaultRegistry().registerType<Bindings>();
        objects.push_back(bus.addObject(portalPath, [this](dbus::Message& msg) {
            if (msg.interface() != shortcutsInterface) return false;
            Properties options;
            if (msg.member() == "CreateSession") {
                Check(msg.signature() == "a{sv}", "CreateSession signature");
                msg >> options;
                session = Path(msg.sender(), "session", Property(options, "session_handle_token"));
                ++creates; CloseObject(session);
            } else if (msg.member() == "BindShortcuts") {
                Check(msg.signature() == "oa(sa{sv})sa{sv}", "BindShortcuts signature");
                dbus::ObjectPath handle; std::string parent;
                msg >> handle >> requested >> parent >> options;
                Check(handle.path() == session && requested.size() == 3, "binding set/session mismatch");
                for (const auto& binding : requested) {
                    Check(std::get<0>(binding) != "mailbox", "unavailable Mailbox was registered");
                    Check(!Property(std::get<1>(binding), "description").empty(), "native title missing");
                    Check(rimes::runtime::ParseShortcut(Property(std::get<1>(binding), "preferred_trigger")).has_value(), "trigger syntax");
                }
            } else return false;
            const auto path = Path(msg.sender(), "request", Property(options, "handle_token"));
            CloseObject(path);
            if (msg.member() == "CreateSession") Response(path, 0, {{"session_handle", dbus::Variant(session)}});
            else if (defer) { delayed = path; delayedSession = session; }
            else Bind(path);
            // Deliberately deliver the signal before the method reply.
            auto reply = msg.createReply(); reply << dbus::ObjectPath(path); reply.send();
            return true;
        }));
    }
    static std::string Path(std::string sender, const char* kind, const std::string& token) {
        sender.erase(0, 1); std::replace(sender.begin(), sender.end(), '.', '_');
        return std::string(portalPath) + "/" + kind + "/" + sender + "/" + token;
    }
    void CloseObject(const std::string& path) {
        objects.push_back(bus.addObject(path, [this](dbus::Message& msg) {
            if (msg.member() != "Close") return false;
            ++closes; msg.createReply().send(); return true;
        }));
    }
    void Response(const std::string& path, uint32_t result, const Properties& values) {
        auto signal = bus.createSignal(path.c_str(), "org.freedesktop.portal.Request", "Response");
        signal << result << values; signal.send(); bus.flush();
    }
    Bindings Granted() const {
        return {{"buffer", Properties{{"trigger_description", dbus::Variant(std::string("Ctrl+Alt+B (desktop)"))}}},
                {"settings", Properties{}}};
    }
    void Bind(const std::string& path) { Response(path, deny ? 1U : 0U, {{"shortcuts", dbus::Variant(Granted())}}); }
    void Activate(const std::string& id, const std::string& handle = "", bool release = false, dbus::Bus* sender = nullptr) {
        auto& connection = sender ? *sender : bus;
        auto signal = connection.createSignal(portalPath, shortcutsInterface, release ? "Deactivated" : "Activated");
        signal << dbus::ObjectPath(handle.empty() ? session : handle) << id << uint64_t{100} << Properties{};
        signal.send(); connection.flush();
    }
};
int PortalTest(const char* launcher) {
    Check(std::getenv("RIMES_PRIVATE_SESSION_TEST"), "private bus required");
    setenv("WAYLAND_DISPLAY", "fixture", 1);
    EventLoop loop; PortalFixture fixture(loop);
    dbus::Bus attacker(dbus::BusType::Session);
    std::vector<std::string> opened;
    DesktopShortcuts host(loop, [&](const std::string& action) {
        opened.push_back(action); return Json{{"ok", true}, {"status", "opened"}};
    }, [] { return Json{{"ok", true}, {"status", "opened"}}; });
    auto config = rimes::runtime::DefaultShortcuts(); host.Configure(config, true);
    int step = 0, result = 1; pid_t child = -1;
    std::unique_ptr<dbus::Bus> recorder;
    std::vector<std::unique_ptr<dbus::Slot>> recorderCalls;
    auto record = [&](dbus::Bus& connection, const char* method, const char* token) {
        auto call = connection.createMethodCall(kLauncherName, kLauncherPath, kLauncherInterface, method);
        call << std::string(token);
        recorderCalls.push_back(call.callAsync(1000000, [](dbus::Message& reply) {
            Check(!reply.isError(), "recording method failed"); return true;
        }));
    };
    const auto output = "/tmp/xsime-launcher-" + std::to_string(getpid()) + ".json";
    std::string oldSession;
    const auto started = now(CLOCK_MONOTONIC); uint64_t checkpoint = started;
    auto timer = loop.addTimeEvent(CLOCK_MONOTONIC, started + 20000, 0,
        [&](EventSourceTime* timer, uint64_t) {
            try {
                const auto current = now(CLOCK_MONOTONIC);
                Check(current - started < 8000000, "portal deadline");
                auto status = host.Status();
                auto state = [&](const char* id) { return status.at("actions").at(id).at("state").get<std::string>(); };
                if (current - checkpoint >= 70000) {
                    if (step == 0 && state("buffer") == "registered") {
                        Check(fixture.creates == 1 && state("capsule") == "not-granted", "duplicate session or fabricated grant");
                        Check(status["actions"]["buffer"]["trigger"] == "Ctrl+Alt+B (desktop)", "desktop binding ignored");
                        fixture.Activate("buffer"); fixture.Activate("buffer");
                        fixture.Activate("settings", "/invalid/session"); fixture.Activate("capsule"); ++step;
                    } else if (step == 1) {
                        Check(opened == std::vector<std::string>{"buffer"}, "repeat/invalid activation");
                        fixture.Activate("buffer", "", true); fixture.Activate("buffer"); ++step;
                    } else if (step == 2) {
                        Check(opened.size() == 2, "release failed to rearm");
                        auto changed = fixture.bus.createSignal(portalPath, shortcutsInterface, "ShortcutsChanged");
                        changed << dbus::ObjectPath(fixture.session) << Bindings{{"capsule", Properties{}}}; changed.send(); fixture.bus.flush(); ++step;
                    } else if (step == 3) {
                        Check(state("capsule") == "registered" && state("buffer") == "not-granted", "desktop rebind not applied");
                        fixture.Activate("buffer"); fixture.Activate("capsule"); ++step;
                    } else if (step == 4) {
                        Check(opened.size() == 3 && opened.back() == "capsule", "old portal binding still active");
                        oldSession = fixture.session; host.Configure(config, false); fixture.Activate("capsule", oldSession);
                        fixture.defer = true; host.Configure(config, true); ++step;
                    } else if (step == 5 && !fixture.delayed.empty()) {
                        Check(opened.size() == 3 && fixture.closes > 0, "disabled session not revoked");
                        config["buffer"] = "CTRL+ALT+b"; fixture.defer = false; host.Configure(config, true);
                        fixture.Bind(fixture.delayed); fixture.Activate("buffer", fixture.delayedSession); ++step;
                    } else if (step == 6 && state("buffer") == "registered") {
                        Check(opened.size() == 3, "late reply revived an old session");
                        fixture.deny = true; host.Configure(config, false); host.Configure(config, true); ++step;
                    } else if (step == 7 && state("buffer") == "not-granted") {
                        fixture.Activate("buffer"); fixture.bus.releaseName(portal); ++step;
                    } else if (step == 8) {
                        Check(opened.size() == 3 && state("buffer") != "registered", "denial or owner loss ignored");
                        fixture.deny = false; fixture.bus.requestName(portal, dbus::RequestNameFlag::None); ++step;
                    } else if (step == 9 && state("buffer") == "registered") {
                        fixture.Activate("buffer", "", false, &attacker); fixture.Activate("settings", oldSession); ++step;
                    } else if (step == 10) {
                        Check(opened.size() == 3, "unowned or retired-session signal accepted");
                        posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
                        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
                        char action[] = "buffer"; char* args[] = {const_cast<char*>(launcher), action, nullptr};
                        const auto error = posix_spawn(&child, launcher, &actions, nullptr, args, environ);
                        posix_spawn_file_actions_destroy(&actions); Check(error == 0, "launcher spawn"); ++step;
                    } else if (step == 11) {
                        int code = 0;
                        if (waitpid(child, &code, WNOHANG) == child) {
                            child = -1;
                            Check(WIFEXITED(code) && WEXITSTATUS(code) == 0 && opened.size() == 4, "independent launcher failed");
                            std::ifstream input(output); Json reply; input >> reply; Check(reply.at("ok"), "launcher response");
                            unlink(output.c_str());
                            recorder = std::make_unique<dbus::Bus>(dbus::BusType::Session);
                            recorder->attachEventLoop(&loop);
                            record(*recorder, "BeginRecording", "first"); ++step;
                        }
                    } else if (step == 12) {
                        Check(status.at("recording") && state("buffer") == "recording", "recorder did not suspend global shortcuts");
                        fixture.Activate("buffer");
                        record(*recorder, "BeginRecording", "second"); record(*recorder, "EndRecording", "first"); ++step;
                    } else if (step == 13) {
                        Check(status.at("recording") && opened.size() == 4, "old recorder stop reenabled globals");
                        record(attacker, "EndRecording", "second");
                        config["buffer"] = "CTRL+SHIFT+z"; host.Configure(config, true); ++step;
                    } else if (step == 14) {
                        Check(status.at("recording") && state("buffer") == "recording", "other process ended recording or config reenabled it");
                        record(*recorder, "EndRecording", "second"); ++step;
                    } else if (step == 15 && state("buffer") == "registered") {
                        Check(!status.at("recording").get<bool>(), "recorder did not restore bindings");
                        record(*recorder, "BeginRecording", "third"); ++step;
                    } else if (step == 16) {
                        Check(status.at("recording"), "second recording failed");
                        recorderCalls.clear(); recorder.reset(); ++step;
                    } else if (step == 17 && state("buffer") == "registered") {
                        Check(!status.at("recording").get<bool>(), "recorder crash left shortcuts suspended");
                        fixture.Activate("buffer"); ++step;
                    } else if (step == 18) {
                        Check(opened.size() == 5, "fresh shortcut did not work after recorder exit");
                        result = 0; loop.exit(); return true;
                    }
                    checkpoint = current;
                }
                timer->setTime(current + 10000); timer->setOneShot();
            } catch (const std::exception& error) { std::cerr << "portal step " << step << ": " << error.what() << '\n'; loop.exit(); }
            return true;
        });
    loop.exec();
    if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
    unlink(output.c_str());
    if (!result) std::cout << "Portal: early Response, subset/rebind, held keys, denial, stale callbacks, owner restart, spoof rejection, CLI and recorder lifecycle passed\n";
    return result;
}
int X11Test() {
    unsetenv("WAYLAND_DISPLAY"); setenv("XDG_SESSION_TYPE", "x11", 1);
    auto* other = xcb_connect(nullptr, nullptr);
    Check(other && !xcb_connection_has_error(other), "X11 fixture connection");
    const auto* setup = xcb_get_setup(other);
    auto root = xcb_setup_roots_iterator(setup).data->root;
    auto* mapping = xcb_get_keyboard_mapping_reply(other, xcb_get_keyboard_mapping(other, setup->min_keycode,
        setup->max_keycode - setup->min_keycode + 1), nullptr);
    Check(mapping, "X11 layout"); const auto* symbols = xcb_get_keyboard_mapping_keysyms(mapping);
    uint8_t code = 0;
    for (unsigned key = setup->min_keycode; key <= setup->max_keycode; ++key)
        if (symbols[(key - setup->min_keycode) * mapping->keysyms_per_keycode] == FcitxKey_v) code = key;
    std::free(mapping); Check(code, "X11 V key");
    auto* error = xcb_request_check(other, xcb_grab_key_checked(other, false, root,
        XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_SHIFT, code, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
    Check(!error, "fixture conflict grab"); std::free(error);
    EventLoop loop; std::vector<std::string> opened;
    DesktopShortcuts host(loop, [&](const std::string& id) { opened.push_back(id); return Json{{"ok", true}}; }, [] { return Json::object(); });
    const auto config = rimes::runtime::DefaultShortcuts(); host.Configure(config, true);
    Check(host.Status()["actions"]["capsule"]["state"] == "conflict", "conflict was not reported");
    Check(host.Status()["actions"]["buffer"]["state"] == "registered", "unrelated action lost binding");
    int step = 0, result = 1; const auto started = now(CLOCK_MONOTONIC);
    auto timer = loop.addTimeEvent(CLOCK_MONOTONIC, started + 150000, 0,
        [&](EventSourceTime* timer, uint64_t) {
            try {
                Check(now(CLOCK_MONOTONIC) - started < 5000000, "X11 deadline");
                if (step == 0) Check(std::system("xdotool keydown ctrl keydown shift keydown b") == 0, "XTest press");
                else if (step == 1) {
                    Check(opened == std::vector<std::string>{"buffer"}, "first X11 key not delivered");
                    Check(std::system("xdotool keydown b keydown b") == 0, "XTest held press");
                } else if (step == 2) {
                    Check(opened.size() == 1, "held shortcut repeated");
                    Check(std::system("xdotool keyup b keyup shift keyup ctrl") == 0, "XTest release");
                } else if (step == 3) Check(std::system("xdotool key ctrl+shift+b") == 0, "XTest fresh press");
                else if (step == 4) {
                    Check(opened.size() == 2, "key release did not rearm");
                    xcb_ungrab_key(other, code, root, XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_SHIFT); xcb_flush(other);
                    // Round-trip the release before retrying from another connection.
                    std::free(xcb_get_input_focus_reply(other, xcb_get_input_focus(other), nullptr));
                    host.Configure(config, false); host.Configure(config, true);
                    Check(host.Status()["actions"]["capsule"]["state"] == "registered", "cleared conflict stayed blocked");
                    Check(std::system("xdotool key Caps_Lock key ctrl+shift+v key Caps_Lock") == 0, "XTest Caps Lock variant");
                } else if (step == 5) {
                    Check(opened.size() == 3 && opened.back() == "capsule", "Caps Lock binding failed");
                    host.Configure(config, false);
                    Check(std::system("xdotool key ctrl+shift+b") == 0, "disabled XTest press");
                } else if (step == 6) {
                    Check(opened.size() == 3, "disabled X11 grab remained active");
                    result = 0; loop.exit(); return true;
                }
                ++step; timer->setTime(now(CLOCK_MONOTONIC) + 150000); timer->setOneShot();
            } catch (const std::exception& error) { std::cerr << "X11 step " << step << ": " << error.what() << '\n'; loop.exit(); }
            return true;
        });
    loop.exec(); xcb_disconnect(other);
    if (!result) std::cout << "X11: checked conflicts, real key events, held/released keys, Caps Lock and disable passed\n";
    return result;
}
int main(int argc, char** argv) {
    try { return argc == 3 && std::string(argv[1]) == "--portal" ? PortalTest(argv[2]) : X11Test(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
