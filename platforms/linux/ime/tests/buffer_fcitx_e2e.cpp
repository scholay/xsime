#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "buffer_protocol.hpp"
#include "configuration.hpp"
#include "official_catalog.hpp"
#include "session_fixture.hpp"

#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/rect.h>
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
                      ("rimes-buffer-e2e-" + std::to_string(getpid()));
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
    user_env = (root / "user").string();
    log_env = (root / "user" / "log").string();
    socket_env = (root / "rimes-buffer.sock").string();
    dump_env = (root / "buffer-snapshot.json").string();
    setenv("RIMES_USER_DIR", user_env.c_str(), 1);
    setenv("RIMES_LOG_DIR", log_env.c_str(), 1);
    setenv("RIMES_BUFFER_SOCKET", socket_env.c_str(), 1);
    setenv("RIMES_BUFFER_DUMP", dump_env.c_str(), 1);
    setenv("RIMES_BUFFER_HEADLESS", "1", 1);
    // Enable via the user hotkey. AUTO_CAPTURE plus a synthetic key that also
    // activates the IC would Toggle the workbench closed in the same event.
    setenv("RIMES_BUFFER_AUTO_CAPTURE", "0", 1);
    setenv("RIMES_BUFFER_CLOSE_AFTER_LAST", "1", 1);
    setenv("RIMES_BUFFER_FOCUS_GRACE_MS", "200", 1);
    if (std::getenv("RIMES_TEST_SESSION_PROTECTION") || std::getenv("RIMES_TEST_LAUNCHER")) {
        setenv("RIMES_CAPSULE_HEADLESS", "1", 1);
        setenv("RIMES_CAPSULE_SOCKET", (root / "capsule.sock").c_str(), 1);
        setenv("RIMES_CAPSULE_DUMP", (root / "capsule.json").c_str(), 1);
    }
    if (const auto* endpoint = std::getenv("RIMES_TEST_AI_API")) {
        rimes::runtime::ConfigurationStore config(root / "user/configuration");
        auto settings = config.Load();
        auto provider = rimes::runtime::FieldDefaults(rimes::runtime::ProviderFields());
        const auto id = rimes::runtime::RandomID();
        provider["id"] = id; provider["name"] = "Loopback fixture";
        provider["baseURL"] = std::string(endpoint) + "/fragmented"; provider["model"] = "fixture-model";
        settings["providers"].push_back(provider); settings["activeProvider"] = id;
        config.Save(settings, settings.at("revision"), {{id, "fixture-credential"}});
        const auto packages = rimes::plugins::BundledPackages();
        rimes::plugins::PackageStore store(root / "user/plugins", rimes::plugins::OfficialCatalog(), packages,
                                           rimes::plugins::CompiledAdapters(), "1.1.0");
        const auto plugin = "builtin.openai-compatible";
        store.Install(plugin, packages.at(plugin), store.Inspect(plugin).grant);
        store.Enable(plugin, true, store.Inspect(plugin).grant);
    }
    if (const auto* resources = std::getenv("RIMES_TEST_TRANSLATION_RESOURCES")) {
        // Copy only the active verified bundle, never a real user profile.
        const auto source = std::filesystem::path(resources);
        std::ifstream input(source / "current.json");
        rimes::runtime::Json pointer; input >> pointer;
        const auto bundle = pointer.at("bundle").get<std::string>();
        if (bundle.size() != 39 || bundle.substr(0, 7) != "bundle-" || bundle.find_first_not_of("0123456789abcdef", 7) != std::string::npos) Die("invalid translation fixture");
        std::filesystem::create_directories(root / "user/translation");
        std::filesystem::permissions(root / "user/translation", std::filesystem::perms::owner_all);
        std::filesystem::copy_file(source / "current.json", root / "user/translation/current.json");
        std::filesystem::copy(source / bundle, root / "user/translation" / bundle, std::filesystem::copy_options::recursive);
    }
    return dump_env;
}

std::string ReadDump(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        Die("buffer dump is missing at " + path);
    }
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (json.empty()) {
        Die("buffer dump is empty");
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

void EnsureCapturing(fcitx::AddonInstance* frontend, const fcitx::ICUUID& uuid,
                     const std::string& dump_path) {
    auto snapshot = ReadDump(dump_path);
    if (HasNeedle(snapshot, "\"capturing\":true")) {
        return;
    }
    SendKey(frontend, uuid, "Control+Shift+B");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":true", "could not resume Buffer capture");
}

using ExtraTimers = std::vector<std::unique_ptr<fcitx::EventSourceTime>>;

struct SuiteCtx {
    fcitx::Instance* instance = nullptr;
    ExtraTimers* timers = nullptr;
    fcitx::AddonInstance* frontend = nullptr;
    fcitx::ICUUID uuid{};
    fcitx::InputContext* ic = nullptr;
    std::string dump_path;
    std::unique_ptr<fcitx::EventSourceTime>* grace_timer = nullptr;
    std::unique_ptr<fcitx::EventSourceTime>* hold_timer = nullptr;
};

void AfterUs(SuiteCtx ctx, int delay_us, std::function<void(SuiteCtx)> fn) {
    ctx.timers->emplace_back(ctx.instance->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + delay_us, 0,
        [ctx, fn = std::move(fn)](fcitx::EventSourceTime*, uint64_t) {
            try {
                fn(ctx);
            } catch (const TestFailed&) {
                ctx.instance->exit();
            }
            return true;
        }));
}

void SendBufferOp(const char* op, std::string_view text = {}) {
    const char* path = std::getenv("RIMES_BUFFER_SOCKET");
    if (path == nullptr || path[0] == '\0') {
        Die("RIMES_BUFFER_SOCKET is missing");
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        Die("could not open the Buffer socket");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        Die("could not connect to the Buffer socket");
    }
    const std::string json = std::string("{\"v\":1,\"op\":\"") + op + "\"" +
        (text.empty() ? "" : (std::string(op) == "select_plugin" ? ",\"id\":" : ",\"text\":") + rimes::buffer::JsonEscape(text)) + "}";
    std::string frame;
    if (!rimes::buffer::EncodeFrame(json, &frame) ||
        send(fd, frame.data(), frame.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(frame.size())) {
        close(fd);
        Die(std::string("could not send Buffer op ") + op);
    }
    close(fd);
}

void DrainStaged(fcitx::AddonInstance* frontend, const fcitx::ICUUID& uuid,
                 const std::string& dump_path) {
    EnsureCapturing(frontend, uuid, dump_path);
    for (int attempt = 0; attempt < 12; ++attempt) {
        const auto snapshot = ReadDump(dump_path);
        if (HasNeedle(snapshot, "\"empty\":true")) {
            return;
        }
        SendKey(frontend, uuid, "BackSpace");
    }
    const auto leftover = ReadDump(dump_path);
    if (!HasNeedle(leftover, "\"empty\":true")) {
        Die("could not drain leftover Buffer chips");
    }
}

void RunLauncherSuite(SuiteCtx ctx) {
    auto phase = std::make_shared<unsigned>(0);
    auto other = std::make_shared<fcitx::ICUUID>();
    auto config = std::make_shared<rimes::runtime::ConfigurationStore>(
        std::filesystem::path(std::getenv("RIMES_USER_DIR")) / "configuration");
    auto initial = config->Load();
    initial["resetOnAppSwitch"] = false; config->Save(initial, initial.at("revision"));
    ctx.ic->setCapabilityFlags(fcitx::CapabilityFlag::Preedit); ctx.ic->focusIn();
    auto check = [](bool condition, const char* message) { if (!condition) Die(message); };
    auto capturing = [ctx](bool expected) {
        ExpectContains(ReadDump(ctx.dump_path), expected ? "\"capturing\":true" : "\"capturing\":false", "launcher capture state");
    };
    const auto started = fcitx::now(CLOCK_MONOTONIC);
    ctx.timers->emplace_back(ctx.instance->eventLoop().addTimeEvent(CLOCK_MONOTONIC, started + 200000, 0,
        [ctx, phase, other, config, check, capturing, started](fcitx::EventSourceTime* timer, uint64_t) {
            try {
                check(fcitx::now(CLOCK_MONOTONIC) - started < 10000000, "launcher suite deadline");
                auto* instance = ctx.instance;
                auto key = [&](const char* value) { SendKey(ctx.frontend, ctx.uuid, value); };
                auto keyboard = [&] { instance->setCurrentInputMethod(ctx.ic, "keyboard-us", true); };
                switch (*phase) {
                case 0:
                    keyboard(); check(instance->inputMethod(ctx.ic) == "keyboard-us", "keyboard fixture unavailable");
                    ctx.frontend->call<fcitx::ITestFrontend::keyEvent>(ctx.uuid, fcitx::Key("Control+Shift+b"), false);
                    check(instance->inputMethod(ctx.ic) == "rimes", "global shortcut did not switch this IC");
                    break;
                case 1:
                    capturing(true);
                    ctx.frontend->call<fcitx::ITestFrontend::keyEvent>(ctx.uuid, fcitx::Key("Control+Shift+b"), false);
                    capturing(true); // held press must not toggle it closed
                    ctx.frontend->call<fcitx::ITestFrontend::keyEvent>(ctx.uuid, fcitx::Key("Control+Shift+b"), true);
                    key("Control+Shift+b"); capturing(false);
                    check(instance->inputMethod(ctx.ic) == "rimes", "closing unexpectedly switched IM");
                    keyboard(); key("Control+Shift+b"); keyboard(); // user overrides the pending activation
                    break;
                case 2:
                    capturing(false);
                    key("Control+Shift+b"); ctx.ic->focusOut();
                    *other = ctx.frontend->call<fcitx::ITestFrontend::createInputContext>("launcher-other-app");
                    instance->inputContextManager().findByUUID(*other)->focusIn();
                    break;
                case 3:
                    capturing(false);
                    instance->inputContextManager().findByUUID(*other)->focusOut();
                    ctx.frontend->call<fcitx::ITestFrontend::destroyInputContext>(*other);
                    ctx.ic->focusIn(); keyboard(); ctx.ic->setCapabilityFlags(fcitx::CapabilityFlag::Sensitive);
                    key("Control+Shift+b");
                    check(instance->inputMethod(ctx.ic) == "keyboard-us", "protected field switched IM");
                    capturing(false);
                    ctx.ic->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
                    {
                        auto next = config->Load(); next["shortcuts"]["buffer"] = "CTRL+ALT+j";
                        config->Save(next, next.at("revision"));
                    }
                    break;
                case 4:
                    key("Control+Shift+b");
                    check(instance->inputMethod(ctx.ic) == "keyboard-us", "retired binding still active");
                    key("Control+Alt+j"); break;
                case 5:
                    capturing(true); key("Control+Alt+j"); capturing(false);
                    {
                        auto next = config->Load(); next["globalShortcuts"] = false; config->Save(next, next.at("revision"));
                    }
                    keyboard(); break;
                case 6:
                    key("Control+Alt+j");
                    check(instance->inputMethod(ctx.ic) == "keyboard-us", "global disable ignored");
                    instance->setCurrentInputMethod(ctx.ic, "rimes", true); key("Control+Alt+j"); capturing(true);
                    key("Control+Alt+j"); capturing(false);
                    {
                        auto next = config->Load(); next["globalShortcuts"] = true; config->Save(next, next.at("revision"));
                    }
                    keyboard(); break;
                case 7: key("Control+Shift+v"); break;
                case 8:
                    ExpectContains(ReadDump(std::getenv("RIMES_CAPSULE_DUMP")), "\"visible\":true", "Capsule independent shortcut did not open");
                    check(instance->inputMethod(ctx.ic) == "rimes", "Capsule did not select XSIME");
                    key("Control+Shift+v");
                    std::cout << "Launcher: other IM activation, held keys, focus/user-switch cancellation, sensitive fields, custom bindings and disable passed\n";
                    instance->exit(); return true;
                }
                ++*phase; timer->setTime(fcitx::now(CLOCK_MONOTONIC) + 200000); timer->setOneShot();
            } catch (const TestFailed&) { ctx.instance->exit(); }
            return true;
        }));
}

void RunBufferSuite(fcitx::Instance& instance,
                    fcitx::AddonInstance* frontend,
                    const fcitx::ICUUID& uuid,
                    fcitx::InputContext* ic,
                    const std::string& dump_path,
                    std::unique_ptr<fcitx::EventSourceTime>* grace_timer,
                    std::unique_ptr<fcitx::EventSourceTime>* hold_timer,
                    ExtraTimers* extra_timers) {
    ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit} |
                           fcitx::CapabilityFlag::ClientUnfocusCommit);

    SendKey(frontend, uuid, "Control+Shift+B");
    auto snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":true", "Ctrl+Shift+B did not enable capture");
    ExpectContains(snapshot, "\"visible\":true", "Ctrl+Shift+B did not show the workbench");
    std::cout << "ok: Ctrl+Shift+B enabled capture\n";

    SendKey(frontend, uuid, "Control+Shift+B");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":false", "hotkey did not pause capture");
    ExpectContains(snapshot, "\"visible\":false", "hotkey did not hide the workbench");
    std::cout << "ok: Ctrl+Shift+B paused an open workbench\n";

    SendKey(frontend, uuid, "Control+Shift+B");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":true", "hotkey did not resume capture");
    ExpectContains(snapshot, "\"visible\":true", "hotkey did not show the workbench");
    std::cout << "ok: Ctrl+Shift+B resumed capture\n";

    Type(frontend, uuid, "nihao");
    SendKey(frontend, uuid, "space");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "你好", "Rime commit was not staged into Buffer");
    ExpectContains(snapshot, "\"capturing\":true", "capture lost after commit");
    std::cout << "ok: buffer staged nihao + Space\n";

    Type(frontend, uuid, "shijie");
    SendKey(frontend, uuid, "space");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "世界", "second commit was not staged");
    std::cout << "ok: buffer kept a second block\n";

    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("你好");
    SendKey(frontend, uuid, "Return");
    snapshot = ReadDump(dump_path);
    ExpectMissing(snapshot, "你好", "Return tap left the delivered block");
    ExpectContains(snapshot, "世界", "Return tap consumed more than one block");
    std::cout << "ok: buffer Return tap sendNext delivered 你好\n";

    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("世界");
    SendKey(frontend, uuid, "Return");
    snapshot = ReadDump(dump_path);
    ExpectMissing(snapshot, "世界", "second Return tap did not send the remaining block");
    std::cout << "ok: buffer second Return tap sendNext\n";

    EnsureCapturing(frontend, uuid, dump_path);
    Type(frontend, uuid, "nihao");
    SendKey(frontend, uuid, "space");
    SendKey(frontend, uuid, "BackSpace");
    snapshot = ReadDump(dump_path);
    ExpectMissing(snapshot, "你好", "Backspace did not drop the staged block");
    std::cout << "ok: buffer Backspace remove_last\n";

    Type(frontend, uuid, "nihao");
    SendKey(frontend, uuid, "Return");
    snapshot = ReadDump(dump_path);
    // rime_ice maps Return to commit_raw_input, so the settled chip is the
    // spelling, not the highlighted candidate.
    ExpectContains(snapshot, "nihao", "composing Return must settle into a block");
    ExpectContains(snapshot, "\"capturing\":true", "settle must not send or pause");
    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("nihao");
    SendKey(frontend, uuid, "Return");
    snapshot = ReadDump(dump_path);
    ExpectMissing(snapshot, "nihao", "ready Return did not send the settled block");
    std::cout << "ok: buffer composing Return settles, next Return sends\n";

    EnsureCapturing(frontend, uuid, dump_path);
    Type(frontend, uuid, "nihao");
    ExpectNoZwsp(ic, "composing while capturing");
    snapshot = ReadDump(dump_path);
    ExpectMissing(snapshot, "\\u200b", "snapshot must not advertise a ZWSP preedit");
    ExpectMissing(snapshot, "\xe2\x80\x8b", "snapshot must not contain U+200B");
    ic->focusOut();
    ExpectNoZwsp(ic, "focus-out while capturing");
    ic->focusIn();
    ExpectNoZwsp(ic, "focus-in after capturing focus-out");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":false",
                   "same-IC reactivation without a drag must pause capture");
    ExpectContains(snapshot, "nihao", "open composition is staged on same-IC reactivate");
    std::cout << "ok: capturing never installs a client-preedit ZWSP\n";
    std::cout << "ok: same-IC reactivate without a fresh caret pauses capture\n";

    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("你好");
    Type(frontend, uuid, "nihao");
    SendKey(frontend, uuid, "space");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":false",
                   "next key after a stale same-IC reactivate must stay direct");
    ExpectMissing(snapshot, "你好", "next key after same-IC reactivate must not stage");
    std::cout << "ok: next key after same-IC reactivate is not captured\n";

    EnsureCapturing(frontend, uuid, dump_path);
    DrainStaged(frontend, uuid, dump_path);
    ic->focusOut();
    ic->focusIn();
    ic->setCursorRect(fcitx::Rect(80, 200, 88, 220));
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":false",
                   "caret update after activate must not restore capture");
    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("你好");
    Type(frontend, uuid, "nihao");
    SendKey(frontend, uuid, "space");
    snapshot = ReadDump(dump_path);
    ExpectMissing(snapshot, "你好", "late caret update must not recapture the next key");
    std::cout << "ok: caret update after activate does not recapture\n";

    EnsureCapturing(frontend, uuid, dump_path);
    SendKey(frontend, uuid, "Escape");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":false", "Escape did not hide");
    ExpectContains(snapshot, "\"capturing\":false", "Escape did not pause capture");

    frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("你好");
    Type(frontend, uuid, "nihao");
    SendKey(frontend, uuid, "space");
    std::cout << "ok: buffer paused; later commits go to the host\n";

    SendKey(frontend, uuid, "Control+Shift+B");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":true", "could not reopen for Escape-scope");
    const auto uuid2 = frontend->call<fcitx::ITestFrontend::createInputContext>("rimes-buffer-other");
    auto* ic2 = instance.inputContextManager().findByUUID(uuid2);
    if (ic2 == nullptr) {
        Die("second test input context was not created");
    }
    instance.setCurrentInputMethod(ic2, "rimes", true);
    ic2->focusIn();
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":false", "focusing another IC must drop capture");
    ExpectContains(snapshot, "\"visible\":true", "focus change must keep the workbench");
    const bool other_handled =
        frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid2, fcitx::Key("Escape"), false);
    frontend->call<fcitx::ITestFrontend::keyEvent>(uuid2, fcitx::Key("Escape"), true);
    if (other_handled) {
        Die("Escape on an uncaptured IC was swallowed");
    }
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"visible\":true", "Escape on another IC must not close Buffer");
    std::cout << "ok: Escape is scoped to the captured input context\n";

    frontend->call<fcitx::ITestFrontend::destroyInputContext>(uuid2);
    ic->focusIn();
    EnsureCapturing(frontend, uuid, dump_path);

    const auto uuid_dying =
        frontend->call<fcitx::ITestFrontend::createInputContext>("rimes-buffer-dying");
    auto* ic_dying = instance.inputContextManager().findByUUID(uuid_dying);
    if (ic_dying == nullptr) {
        Die("dying test input context was not created");
    }
    instance.setCurrentInputMethod(ic_dying, "rimes", true);
    ic_dying->focusIn();
    SendKey(frontend, uuid_dying, "Control+Shift+B");
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":true", "dying IC did not capture");
    frontend->call<fcitx::ITestFrontend::destroyInputContext>(uuid_dying);
    snapshot = ReadDump(dump_path);
    ExpectContains(snapshot, "\"capturing\":false", "destroyed IC must drop capture");
    std::cout << "ok: destroying the captured IC pauses capture and clears the route\n";

    ic->focusIn();
    EnsureCapturing(frontend, uuid, dump_path);
    const SuiteCtx ctx{&instance, extra_timers, frontend, uuid, ic, dump_path, grace_timer,
                       hold_timer};
    SendBufferOp("drag_begin");
    AfterUs(ctx, 50000, [](SuiteCtx ctx) {
        AfterUs(ctx, 2000000, [](SuiteCtx ctx) {
            ctx.ic->focusOut();
            ctx.ic->focusIn();
            auto snapshot = ReadDump(ctx.dump_path);
            ExpectContains(snapshot, "\"capturing\":true",
                           "same-IC reactivate 2s after drag_begin without drag_end must keep");
            std::cout << "ok: drag_begin without drag_end keeps capture for 2s\n";

            SendBufferOp("drag_begin");
            AfterUs(ctx, 50000, [](SuiteCtx ctx) {
                AfterUs(ctx, 300000, [](SuiteCtx ctx) {
                    SendBufferOp("drag_end");
                    // IME command-order coverage for #44: the next same-IC
                    // activation after the socket command is dispatched is
                    // a field switch. This headless test does not exercise
                    // physical button release or GTK's 50ms release poll.
                    AfterUs(ctx, 10000, [](SuiteCtx ctx) {
                        ctx.ic->focusOut();
                        ctx.ic->focusIn();
                        auto snapshot = ReadDump(ctx.dump_path);
                        ExpectContains(
                            snapshot, "\"capturing\":false",
                            "same-IC reactivate within 10ms after drag_end must pause");
                        std::cout << "ok: immediate field switch after drag_end pauses capture\n";

                        ctx.ic->focusOut();
                        ctx.ic->focusIn();
                        snapshot = ReadDump(ctx.dump_path);
                        ExpectContains(snapshot, "\"capturing\":false",
                                       "second same-IC after a consumed drag must pause");
                        std::cout << "ok: field switch after a completed drag pauses capture\n";

                        EnsureCapturing(ctx.frontend, ctx.uuid, ctx.dump_path);
                        SendBufferOp("drag_begin");
                        AfterUs(ctx, 50000, [](SuiteCtx ctx) {
                            SendBufferOp("drag_end");
                            AfterUs(ctx, 1200000, [](SuiteCtx ctx) {
                                ctx.ic->focusOut();
                                ctx.ic->focusIn();
                                auto snapshot = ReadDump(ctx.dump_path);
                                ExpectContains(snapshot, "\"capturing\":false",
                                               "same-IC reactivate after drag_end must pause");
                                std::cout
                                    << "ok: same-IC reactivate 1.2s after drag_end pauses\n";

                                ctx.ic->focusIn();
                                EnsureCapturing(ctx.frontend, ctx.uuid, ctx.dump_path);
                                DrainStaged(ctx.frontend, ctx.uuid, ctx.dump_path);
                                Type(ctx.frontend, ctx.uuid, "zhongguoren");
                                const auto uuid_switch =
                                    ctx.frontend->call<fcitx::ITestFrontend::createInputContext>(
                                        "rimes-buffer-switch");
                                auto* ic_switch =
                                    ctx.instance->inputContextManager().findByUUID(uuid_switch);
                                if (ic_switch == nullptr) {
                                    Die("switch test input context was not created");
                                }
                                ctx.instance->setCurrentInputMethod(ic_switch, "rimes", true);
                                ic_switch->focusIn();
                                snapshot = ReadDump(ctx.dump_path);
                                ExpectContains(snapshot, "zhongguoren",
                                               "app switch must stage raw input, not "
                                               "syllable-spaced preedit");
                                ExpectMissing(snapshot, "zhong guo",
                                              "staged composition must not keep syllable spaces");
                                ExpectContains(snapshot, "\"capturing\":false",
                                               "app switch must pause capture");
                                std::cout << "ok: switching apps stages raw input zhongguoren\n";
                                ctx.frontend->call<fcitx::ITestFrontend::destroyInputContext>(
                                    uuid_switch);

                                ctx.ic->focusIn();
                                DrainStaged(ctx.frontend, ctx.uuid, ctx.dump_path);
                                Type(ctx.frontend, ctx.uuid, "shi");
                                ctx.ic->focusOut();
                                *ctx.grace_timer = ctx.instance->eventLoop().addTimeEvent(
                                    CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 400000, 0,
                                    [ctx](fcitx::EventSourceTime*, uint64_t) {
                                        try {
                                            auto snapshot = ReadDump(ctx.dump_path);
                                            ExpectContains(snapshot, "\"capturing\":false",
                                                           "focus-out grace must drop capture");
                                            ExpectContains(
                                                snapshot, "shi",
                                                "open composition must stage into Buffer");
                                            std::cout << "ok: leaving a field stages the open "
                                                         "preedit and drops capture\n";

                                            ctx.ic->focusIn();
                                            DrainStaged(ctx.frontend, ctx.uuid, ctx.dump_path);
                                            Type(ctx.frontend, ctx.uuid, "nihao");
                                            SendKey(ctx.frontend, ctx.uuid, "space");
                                            Type(ctx.frontend, ctx.uuid, "shijie");
                                            SendKey(ctx.frontend, ctx.uuid, "space");
                                            snapshot = ReadDump(ctx.dump_path);
                                            ExpectContains(snapshot, "你好",
                                                           "hold-Return fixture missing first chip");
                                            ExpectContains(
                                                snapshot, "世界",
                                                "hold-Return fixture missing second chip");
                                            ctx.frontend
                                                ->call<fcitx::ITestFrontend::pushCommitExpectation>(
                                                    "你好");
                                            ctx.frontend
                                                ->call<fcitx::ITestFrontend::pushCommitExpectation>(
                                                    "世界");
                                            ctx.frontend->call<fcitx::ITestFrontend::keyEvent>(
                                                ctx.uuid, fcitx::Key("Return"), false);

                                            *ctx.hold_timer =
                                                ctx.instance->eventLoop().addTimeEvent(
                                                    CLOCK_MONOTONIC,
                                                    fcitx::now(CLOCK_MONOTONIC) + 1400000, 0,
                                                    [ctx](fcitx::EventSourceTime*, uint64_t) {
                                                        try {
                                                            const fcitx::Key repeat_return(
                                                                FcitxKey_Return,
                                                                fcitx::KeyStates{
                                                                    fcitx::KeyState::Repeat});
                                                            for (int index = 0; index < 8;
                                                                 ++index) {
                                                                ctx.frontend->call<
                                                                    fcitx::ITestFrontend::keyEvent>(
                                                                    ctx.uuid, repeat_return, false);
                                                            }
                                                            ctx.frontend->call<
                                                                fcitx::ITestFrontend::keyEvent>(
                                                                ctx.uuid, fcitx::Key("Return"),
                                                                true);
                                                            const auto after =
                                                                ReadDump(ctx.dump_path);
                                                            ExpectMissing(
                                                                after, "你好",
                                                                "hold-Return left the first chip");
                                                            ExpectMissing(
                                                                after, "世界",
                                                                "hold-Return left the second chip");
                                                            ExpectContains(
                                                                after, "\"capturing\":false",
                                                                "send-all + close-after-last must "
                                                                "pause");
                                                            ExpectNoZwsp(
                                                                ctx.ic,
                                                                "after held-Return send-all");
                                                            std::cout << "ok: held-Return send-all "
                                                                         "ate leftover repeats\n";
                                                            // Real settings-store transaction must
                                                            // update the running addon without a restart.
                                                            unsetenv("RIMES_BUFFER_CLOSE_AFTER_LAST");
                                                            rimes::runtime::ConfigurationStore settings(
                                                                rimes::runtime::UserRoot() / "configuration");
                                                            auto changed=settings.Load();
                                                            changed["closeAfterLast"]=false;
                                                            changed["bufferWidth"]=920;
                                                            changed["resetOnAppSwitch"]=true;
                                                            settings.Save(changed,changed.at("revision"));
                                                            AfterUs(ctx,100000,[](SuiteCtx next) {
                                                                const auto updated=ReadDump(next.dump_path);
                                                                ExpectContains(updated,"\"close_after_last\":false","running IME missed settings change");
                                                                ExpectContains(updated,"\"width\":920","running IME missed width change");
                                                                std::cout << "ok: settings transaction updates running Buffer\n";
                                                                EnsureCapturing(next.frontend,next.uuid,next.dump_path);
                                                                Type(next.frontend,next.uuid,"nihao");
                                                                SendKey(next.frontend,next.uuid,"space");
                                                                ExpectContains(ReadDump(next.dump_path),"你好","local app-switch fixture missing");
                                                                const auto other=next.frontend->call<fcitx::ITestFrontend::createInputContext>("privacy-target");
                                                                auto* other_ic=next.instance->inputContextManager().findByUUID(other);
                                                                if(!other_ic) Die("privacy target unavailable");
                                                                next.instance->setCurrentInputMethod(other_ic,"rimes",true);
                                                                other_ic->focusIn();
                                                                ExpectContains(ReadDump(next.dump_path),"\"empty\":true","opted-in application switch retained local typing");
                                                                next.frontend->call<fcitx::ITestFrontend::destroyInputContext>(other);
                                                                next.ic->focusIn();
                                                                SendBufferOp("paste","external delivery fixture");
                                                                AfterUs(next,100000,[](SuiteCtx done) {
                                                                    ExpectContains(ReadDump(done.dump_path),"external delivery fixture","external fixture missing");
                                                                    const auto target=done.frontend->call<fcitx::ITestFrontend::createInputContext>("privacy-external-target");
                                                                    auto* target_ic=done.instance->inputContextManager().findByUUID(target);
                                                                    if(!target_ic) Die("external target unavailable");
                                                                    done.instance->setCurrentInputMethod(target_ic,"rimes",true);
                                                                    target_ic->focusIn();
                                                                    ExpectContains(ReadDump(done.dump_path),"external delivery fixture","app switch discarded external content");
                                                                    std::cout << "ok: app-switch reset clears local typing and preserves external delivery\n";
                                                                    done.frontend->call<fcitx::ITestFrontend::destroyInputContext>(target);
                                                                    done.frontend->call<fcitx::ITestFrontend::destroyInputContext>(done.uuid);
                                                                    done.instance->exit();
                                                                });
                                                            });
                                                        } catch (const TestFailed&) {
                                                            ctx.instance->exit();
                                                        }
                                                        return true;
                                                    });
                                        } catch (const TestFailed&) {
                                            ctx.instance->exit();
                                        }
                                        return true;
                                    });
                            });
                        });
                    });
                });
            });
        });
    });
}

}  // namespace

void RunProtectionSuite(SuiteCtx ctx, SessionFixture* session) {
    ctx.ic->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
    EnsureCapturing(ctx.frontend, ctx.uuid, ctx.dump_path);
    SendBufferOp("paste", "保留原文");
    auto phase = std::make_shared<int>(0);
    const auto started = fcitx::now(CLOCK_MONOTONIC);
    ctx.timers->emplace_back(ctx.instance->eventLoop().addTimeEvent(CLOCK_MONOTONIC, started + 100000, 0,
        [ctx, session, phase, started](fcitx::EventSourceTime* timer, std::uint64_t) {
            try {
                rimes::buffer::Snapshot s;
                if (!rimes::buffer::ParseSnapshot(ReadDump(ctx.dump_path), &s)) Die("protection snapshot malformed");
                if (fcitx::now(CLOCK_MONOTONIC) - started > 8000000) Die("protection suite deadline");
                switch (*phase) {
                    case 0:
                        if (s.staged_text != "保留原文") break;
                        ctx.ic->setCapabilityFlags(fcitx::CapabilityFlag::Sensitive); ++*phase; break;
                    case 1: {
                        if (!s.secure || s.capturing || !s.staged_text.empty()) Die("Sensitive field was not shielded");
                        const auto background = ctx.frontend->call<fcitx::ITestFrontend::createInputContext>("background-field");
                        auto* ic = ctx.instance->inputContextManager().findByUUID(background);
                        // createInputContext may focus its IC; restore the protected one
                        // before emitting unrelated background capability changes.
                        ctx.ic->focusIn(); ic->focusOut();
                        ic->setCapabilityFlags(fcitx::CapabilityFlag::Password);
                        ic->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
                        SendBufferOp("paste", "must-reject"); SendBufferOp("show"); ++*phase; break;
                    }
                    case 2:
                        if (!s.secure || s.capturing) Die("background capability or command removed protection");
                        ctx.ic->setCapabilityFlags(fcitx::CapabilityFlag::Preedit); ++*phase; break;
                    case 3:
                        if (s.secure || s.staged_text != "保留原文" || s.capturing) Die("field recovery lost source/reused target");
                        SendBufferOp("show"); ++*phase; break;
                    case 4:
                        if (!s.capturing) break;
                        Type(ctx.frontend, ctx.uuid, "nihao"); session->Sleep(true); ++*phase; break;
                    case 5:
                        if (!s.secure || s.visible || s.capturing || !s.preedit.empty() || !s.staged_text.empty()) Die("sleep exposed state");
                        SendBufferOp("paste", "must-reject"); SendBufferOp("show"); SendBufferOp("send_all");
                        session->Property("LockedHint", true); session->Sleep(false); ++*phase; break;
                    case 6:
                        if (!s.secure || s.visible) Die("wake cleared lock");
                        session->Property("Active", false); session->Property("LockedHint", false); ++*phase; break;
                    case 7:
                        if (!s.secure || s.visible) Die("unlock cleared inactive session");
                        session->Property("Active", true); ++*phase; break;
                    case 8:
                        if (s.secure) break;
                        if (!s.visible || s.capturing || s.staged_text != "保留原文" || !s.preedit.empty()) Die("resume reused old target/preedit or modified source");
                        SendBufferOp("send_all"); ++*phase; break;
                    case 9:
                        if (s.staged_text != "保留原文") Die("resume delivered without fresh capture");
                        SendBufferOp("show"); ++*phase; break;
                    case 10:
                        if (!s.capturing) break;
                        ctx.frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("保留原文");
                        SendBufferOp("send_all"); ++*phase; break;
                    case 11: {
                        if (!s.staged_text.empty()) break;
                        const auto password = ctx.frontend->call<fcitx::ITestFrontend::createInputContext>("firefox-disabled-password");
                        auto* ic = ctx.instance->inputContextManager().findByUUID(password);
                        ic->focusOut(); ic->setCapabilityFlags(fcitx::CapabilityFlag::Password);
                        ctx.ic->focusOut(); ic->focusIn(); ++*phase; break;
                    }
                    case 12:
                        if (!s.secure) Die("password focus without engine activation was missed");
                        ctx.ic->focusIn(); ++*phase; break;
                    case 13:
                        if (s.secure) Die("fresh non-sensitive focus did not recover");
                        SendBufferOp("show"); SendBufferOp("paste", "恢复"); ++*phase; break;
                    case 14:
                        if (!s.capturing || s.staged_text != "恢复") break;
                        ctx.frontend->call<fcitx::ITestFrontend::keyEvent>(ctx.uuid, fcitx::Key("Return"), false);
                        ctx.ic->setCapabilityFlags(fcitx::CapabilityFlag::Password); ++*phase; break;
                    case 15:
                        if (!s.secure) Die("held Return password transition did not protect");
                        ctx.frontend->call<fcitx::ITestFrontend::keyEvent>(ctx.uuid, fcitx::Key("Return"), true);
                        ctx.ic->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
                        SendBufferOp("show"); ++*phase; break;
                    case 16:
                        if (!s.capturing) break;
                        ctx.frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("恢复");
                        SendKey(ctx.frontend, ctx.uuid, "Return"); ++*phase; break;
                    case 17:
                        if (!s.staged_text.empty()) Die("first Return after password recovery was swallowed");
                        SendBufferOp("show"); SendBufferOp("paste", "唤醒"); ++*phase; break;
                    case 18:
                        if (!s.capturing || s.staged_text != "唤醒") break;
                        ctx.frontend->call<fcitx::ITestFrontend::keyEvent>(ctx.uuid, fcitx::Key("Return"), false);
                        session->Sleep(true); ++*phase; break;
                    case 19:
                        if (!s.secure) break;
                        ctx.frontend->call<fcitx::ITestFrontend::keyEvent>(ctx.uuid, fcitx::Key("Return"), true);
                        session->Sleep(false); ++*phase; break;
                    case 20:
                        if (s.secure) break;
                        SendBufferOp("show"); ++*phase; break;
                    case 21:
                        if (!s.capturing) break;
                        ctx.frontend->call<fcitx::ITestFrontend::pushCommitExpectation>("唤醒");
                        SendKey(ctx.frontend, ctx.uuid, "Return"); ++*phase; break;
                    case 22:
                        if (!s.staged_text.empty()) Die("first Return after session recovery was swallowed");
                        std::cout << "ok: production Fcitx sensitive/password focus, background events, sleep/lock/inactive overlap, IPC gates, source preservation, fresh-target delivery and held-Return recovery\n";
                        ctx.instance->exit(); return true;
                }
                timer->setTime(fcitx::now(CLOCK_MONOTONIC) + 100000); timer->setOneShot();
            } catch (const std::exception& error) { std::cerr << error.what() << '\n'; g_exit_status = EXIT_FAILURE; ctx.instance->exit(); }
            return true;
        }));
}

void RunAISuite(SuiteCtx ctx) {
    ctx.ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit});
    EnsureCapturing(ctx.frontend, ctx.uuid, ctx.dump_path);
    SendBufferOp("select_plugin", "builtin.openai-compatible"); SendBufferOp("paste", "原文🙂");
    struct Progress { int phase = 0; bool preview = false; std::string remaining; std::chrono::steady_clock::time_point closed; };
    auto progress = std::make_shared<Progress>();
    const auto started = std::chrono::steady_clock::now();
    ctx.timers->emplace_back(ctx.instance->eventLoop().addTimeEvent(CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 50000, 0,
        [ctx, progress, started](fcitx::EventSourceTime* timer, std::uint64_t) {
            try {
                rimes::buffer::Snapshot s;
                if (!rimes::buffer::ParseSnapshot(ReadDump(ctx.dump_path), &s)) Die("AI snapshot malformed");
                const auto elapsed = std::chrono::steady_clock::now() - started;
                if (elapsed > std::chrono::seconds(15)) Die("AI workbench deadline");
                if (s.phase == rimes::buffer::DerivedPhase::Failed) Die("AI workbench request failed: " + s.message);
                if (progress->phase == 0 && elapsed > std::chrono::milliseconds(250)) {
                    if (s.active_plugin != "builtin.openai-compatible" || s.phase != rimes::buffer::DerivedPhase::Idle || !s.results.empty())
                        Die("AI requested network before explicit generation");
                    SendBufferOp("generate"); progress->phase = 1;
                } else if (progress->phase == 1) {
                    if (s.phase == rimes::buffer::DerivedPhase::Running && !s.results.empty()) {
                        progress->preview = true;
                        if (s.can_deliver || s.results[0].ready) Die("AI stream preview became deliverable");
                    }
                    if (s.phase == rimes::buffer::DerivedPhase::Ready) {
                        if (!progress->preview || s.results.size() != 1 || s.results[0].text != "First 🙂.第二段。" ||
                            s.result_blocks.size() < 2 || s.staged_text != "原文🙂") Die("AI stream or source preservation differs");
                        ctx.frontend->call<fcitx::ITestFrontend::pushCommitExpectation>(s.result_blocks.front().text);
                        progress->remaining = s.results[0].text.substr(s.result_blocks.front().text.size());
                        SendBufferOp("send_next"); progress->phase = 2;
                    }
                } else if (progress->phase == 2 && !s.results.empty() && s.results[0].text == progress->remaining) {
                    if (s.staged_text != "原文🙂") Die("AI source consumed before final output child");
                    for (const auto& block : s.result_blocks) ctx.frontend->call<fcitx::ITestFrontend::pushCommitExpectation>(block.text);
                    SendBufferOp("send_all"); progress->phase = 3;
                } else if (progress->phase == 3 && !s.visible) {
                    if (!s.staged_text.empty() || !s.results.empty()) Die("AI final native delivery did not consume source");
                    EnsureCapturing(ctx.frontend, ctx.uuid, ctx.dump_path);
                    SendBufferOp("paste", "原文🙂"); progress->phase = 4;
                } else if (progress->phase == 4 && s.can_generate) {
                    SendBufferOp("generate"); progress->phase = 5;
                } else if (progress->phase == 5 && s.phase == rimes::buffer::DerivedPhase::Running && !s.results.empty()) {
                    SendBufferOp("close"); progress->closed = std::chrono::steady_clock::now(); progress->phase = 6;
                } else if (progress->phase == 6 && std::chrono::steady_clock::now() - progress->closed > std::chrono::milliseconds(500)) {
                    if (s.visible || !s.results.empty() || s.staged_text != "原文🙂") Die("closed AI request restored output or consumed source");
                    std::cout << "ok: production AI adapter, explicit HTTP, stream preview, final-only delivery, source consumption and close cancellation\n";
                    ctx.instance->exit(); return true;
                }
                timer->setTime(fcitx::now(CLOCK_MONOTONIC) + 20000); timer->setOneShot();
            } catch (const std::exception& error) { std::cerr << error.what() << '\n'; g_exit_status = EXIT_FAILURE; ctx.instance->exit(); }
            return true;
        }));
}

void RunAutoSendSuite(SuiteCtx ctx) {
    ctx.ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit});
    EnsureCapturing(ctx.frontend, ctx.uuid, ctx.dump_path);
    SendBufferOp("cycle_auto_send"); SendBufferOp("paste", "hello world.");
    AfterUs(ctx, 150000, [](SuiteCtx active) {
        ExpectContains(ReadDump(active.dump_path), "\"auto_send_seconds\":1", "toolbar cycle did not persist one-second timer");
        Type(active.frontend, active.uuid, "nihao");
        AfterUs(active, 1500000, [](SuiteCtx composing) {
            rimes::buffer::Snapshot snapshot;
            if (!rimes::buffer::ParseSnapshot(ReadDump(composing.dump_path), &snapshot) ||
                snapshot.preedit.empty() || snapshot.staged_text != "hello world.")
                Die("auto-send settled composition or consumed stable source while composing");
            for (const auto& block : snapshot.blocks)
                composing.frontend->call<fcitx::ITestFrontend::pushCommitExpectation>(block.text);
            SendKey(composing.frontend, composing.uuid, "Escape");
            AfterUs(composing, 250000, [](SuiteCtx delivered) {
                ExpectContains(ReadDump(delivered.dump_path), "\"visible\":false", "ready timer did not deliver after composition ended");
                EnsureCapturing(delivered.frontend, delivered.uuid, delivered.dump_path);
                SendBufferOp("paste", "pause check");
                AfterUs(delivered, 400000, [](SuiteCtx pause) {
                    SendBufferOp("close");
                    AfterUs(pause, 1400000, [](SuiteCtx hidden) {
                        ExpectContains(ReadDump(hidden.dump_path), "pause check", "hidden timer consumed source");
                        EnsureCapturing(hidden.frontend, hidden.uuid, hidden.dump_path);
                        AfterUs(hidden, 250000, [](SuiteCtx resumed) {
                            rimes::buffer::Snapshot snapshot;
                            if (!rimes::buffer::ParseSnapshot(ReadDump(resumed.dump_path), &snapshot) ||
                                snapshot.staged_text != "pause check") Die("hidden interval counted toward auto-send");
                            for (const auto& block : snapshot.blocks)
                                resumed.frontend->call<fcitx::ITestFrontend::pushCommitExpectation>(block.text);
                            AfterUs(resumed, 900000, [](SuiteCtx done) {
                                ExpectContains(ReadDump(done.dump_path), "\"visible\":false", "resumed timer did not drain and close");
                                unsetenv("RIMES_BUFFER_CLOSE_AFTER_LAST");
                                SendBufferOp("toggle_close_after_last");
                                AfterUs(done, 100000, [](SuiteCtx off) {
                                    rimes::runtime::ConfigurationStore settings(rimes::runtime::UserRoot() / "configuration");
                                    ExpectContains(ReadDump(off.dump_path), "\"close_after_last\":false", "toolbar switch did not update host");
                                    if (settings.Load().at("closeAfterLast").get<bool>()) Die("toolbar switch was not persisted");
                                    SendBufferOp("toggle_close_after_last");
                                    AfterUs(off, 100000, [](SuiteCtx on) {
                                        rimes::runtime::ConfigurationStore settings(rimes::runtime::UserRoot() / "configuration");
                                        ExpectContains(ReadDump(on.dump_path), "\"close_after_last\":true", "toolbar switch did not restore host");
                                        if (!settings.Load().at("closeAfterLast").get<bool>()) Die("toolbar switch restore was not persisted");
                                        std::cout << "ok: Default automatic native delivery, both toolbar controls persist, composition gate, hidden pause and close-after-last\n";
                                        on.instance->exit();
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    });
}

void RunTranslationSuite(SuiteCtx ctx) {
    ctx.ic->setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit} | fcitx::CapabilityFlag::ClientUnfocusCommit);
    EnsureCapturing(ctx.frontend, ctx.uuid, ctx.dump_path);
    SendBufferOp("select_plugin", "builtin.apple-translation");
    SendBufferOp("paste", "今天天气很好，我们下午去公园散步。");
    struct Progress { int phase = 0; std::string prefix, result_id; };
    auto progress = std::make_shared<Progress>();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    ctx.timers->emplace_back(ctx.instance->eventLoop().addTimeEvent(CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 50000, 0,
        [ctx, progress, deadline](fcitx::EventSourceTime* timer, std::uint64_t) {
            try {
                if (std::chrono::steady_clock::now() >= deadline) Die("real translation addon deadline");
                rimes::buffer::Snapshot snapshot;
                if (!rimes::buffer::ParseSnapshot(ReadDump(ctx.dump_path), &snapshot)) Die("translation snapshot malformed");
                if (snapshot.phase == rimes::buffer::DerivedPhase::Failed) Die("real translation adapter reported failure");
                if (progress->phase == 0 && snapshot.phase == rimes::buffer::DerivedPhase::Ready) {
                    if (snapshot.active_plugin != "builtin.apple-translation" || snapshot.results.size() != 1 || snapshot.results[0].text.find("park") == std::string::npos) Die("real translation result missing");
                    progress->prefix = snapshot.results[0].text; progress->result_id = snapshot.results[0].id;
                    if (snapshot.staged_text != "今天天气很好，我们下午去公园散步。") Die("translation consumed original source");
                    SendBufferOp("paste", "请保存文档。"); progress->phase = 1;
                } else if (progress->phase == 1 && snapshot.staged_text.find("请保存文档。") != std::string::npos) {
                    if (snapshot.results.empty() || snapshot.results[0].text.substr(0, progress->prefix.size()) != progress->prefix) Die("tail edit erased ready prefix");
                    if (snapshot.results[0].id == progress->result_id) Die("source edit reused whole-result copy authority");
                    progress->phase = 2;
                } else if (progress->phase == 2 && snapshot.phase == rimes::buffer::DerivedPhase::Ready) {
                    if (snapshot.results[0].text.size() <= progress->prefix.size()) Die("new sentence was not translated");
                    if (!snapshot.can_deliver || snapshot.result_blocks.size() < 2) Die("translated output was not segmented for delivery");
                    const auto first = snapshot.result_blocks.front();
                    ctx.frontend->call<fcitx::ITestFrontend::pushCommitExpectation>(first.text);
                    ctx.frontend->call<fcitx::ITestFrontend::keyEvent>(ctx.uuid, fcitx::Key("Return"), false);
                    rimes::buffer::Snapshot delivered;
                    if (!rimes::buffer::ParseSnapshot(ReadDump(ctx.dump_path), &delivered) ||
                        delivered.staged_text != "请保存文档。") Die("first translated child did not retire exactly its source unit on key-down");
                    progress->prefix.erase(0, first.text.size());
                    if (delivered.results.empty() || delivered.results[0].text.substr(0, progress->prefix.size()) != progress->prefix)
                        Die("first translation delivery lost immutable siblings");
                    const auto remaining = delivered.results[0].text;
                    ctx.frontend->call<fcitx::ITestFrontend::keyEvent>(ctx.uuid, fcitx::Key("Return"), true);
                    if (!rimes::buffer::ParseSnapshot(ReadDump(ctx.dump_path), &delivered) || delivered.results[0].text != remaining)
                        Die("derived Return release delivered twice");
                    SendBufferOp("close"); progress->phase = 3;
                } else if (progress->phase == 3 && !snapshot.visible) {
                    if (!snapshot.results.empty()) Die("hidden workbench retained translation authority");
                    EnsureCapturing(ctx.frontend, ctx.uuid, ctx.dump_path);
                    SendBufferOp("paste", "你好，世界。"); progress->phase = 4;
                } else if (progress->phase == 4 && snapshot.phase == rimes::buffer::DerivedPhase::Ready &&
                           snapshot.staged_text.find("你好，世界。") != std::string::npos) {
                    if (snapshot.results.empty() || snapshot.results[0].text.substr(0, progress->prefix.size()) != progress->prefix)
                        Die("reopen/source editing rewrote frozen translated output");
                    for (const auto& block : snapshot.result_blocks)
                        ctx.frontend->call<fcitx::ITestFrontend::pushCommitExpectation>(block.text);
                    SendBufferOp("send_all"); progress->phase = 5;
                } else if (progress->phase == 5 && !snapshot.visible) {
                    if (!snapshot.staged_text.empty() || !snapshot.results.empty()) Die("translated send-all left consumed source or output");
                    std::cout << "ok: production translation, stable prefix, exact source retirement, immediate Return, frozen siblings, reopen and send-all\n";
                    ctx.instance->exit(); return true;
                }
                timer->setTime(fcitx::now(CLOCK_MONOTONIC) + 50000); timer->setOneShot();
            } catch (const std::exception& error) { std::cerr << error.what() << '\n'; g_exit_status = EXIT_FAILURE; ctx.instance->exit(); }
            return true;
        }));
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: rimes-buffer-fcitx-e2e <build-dir> <addon-rel-dir> <data-rel-dir>\n";
        return EXIT_FAILURE;
    }

    try {
        const auto dump_path = IsolateDirs();
        struct FixtureCleanup {
            std::filesystem::path root;
            ~FixtureCleanup() { if (!root.empty()) { std::error_code error; std::filesystem::remove_all(root, error); } }
        } cleanup{std::getenv("RIMES_TEST_TRANSLATION_RESOURCES") || std::getenv("RIMES_TEST_AI_API") || std::getenv("RIMES_TEST_SESSION_PROTECTION") || std::getenv("RIMES_TEST_LAUNCHER") ?
                  std::filesystem::path(dump_path).parent_path() : std::filesystem::path{}};
        fcitx::setupTestingEnvironment(argv[1], {argv[2]}, {argv[3]});

        char arg0[] = "rimes-buffer-fcitx-e2e";
        char arg1[] = "--disable=all";
        char arg2[] = "--enable=testfrontend,testui,rimes";
        // Fcitx's shipped TestIM provides a pass-through keyboard-us entry.
        // The real keyboard addon is statically linked into the fcitx5 binary.
        char launcherArg[] = "--enable=testfrontend,testui,testim,rimes";
        char* args[] = {arg0, arg1, std::getenv("RIMES_TEST_LAUNCHER") ? launcherArg : arg2};
        fcitx::Instance instance(3, args);
        std::unique_ptr<SessionFixture> session_fixture;
        if (std::getenv("RIMES_TEST_SESSION_PROTECTION")) session_fixture = std::make_unique<SessionFixture>(instance.eventLoop());
        instance.addonManager().registerDefaultLoader(nullptr);

        fcitx::EventDispatcher dispatcher;
        dispatcher.attach(&instance.eventLoop());

        fcitx::AddonInstance* frontend = nullptr;
        fcitx::ICUUID uuid{};
        fcitx::InputContext* ic = nullptr;
        std::unique_ptr<fcitx::EventSourceTime> wait_timer;
        std::unique_ptr<fcitx::EventSourceTime> grace_timer;
        std::unique_ptr<fcitx::EventSourceTime> hold_timer;
        ExtraTimers extra_timers;
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
            if (std::getenv("RIMES_TEST_LAUNCHER")) group.inputMethodList().emplace_back(fcitx::InputMethodGroupItem("keyboard-us"));
            group.inputMethodList().emplace_back(fcitx::InputMethodGroupItem("rimes"));
            group.setDefaultInputMethod("rimes");
            manager.setGroup(group);
            manager.setCurrentGroup("Default");

            frontend = instance.addonManager().addon("testfrontend", true);
            if (frontend == nullptr) {
                Die("testfrontend addon did not load");
            }
            uuid = frontend->call<fcitx::ITestFrontend::createInputContext>("rimes-buffer-e2e");
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
                    std::cout << "ok: buffer testfrontend left Deploying\n";
                    try {
                        if (std::getenv("RIMES_TEST_LAUNCHER"))
                            RunLauncherSuite({&instance, &extra_timers, frontend, uuid, ic, dump_path, &grace_timer, &hold_timer});
                        else if (session_fixture)
                            RunProtectionSuite({&instance, &extra_timers, frontend, uuid, ic, dump_path, &grace_timer, &hold_timer}, session_fixture.get());
                        else if (std::getenv("RIMES_TEST_AI_API"))
                            RunAISuite({&instance, &extra_timers, frontend, uuid, ic, dump_path, &grace_timer, &hold_timer});
                        else if (std::getenv("RIMES_TEST_AUTO_SEND"))
                            RunAutoSendSuite({&instance, &extra_timers, frontend, uuid, ic, dump_path, &grace_timer, &hold_timer});
                        else if (std::getenv("RIMES_TEST_TRANSLATION_RESOURCES"))
                            RunTranslationSuite({&instance, &extra_timers, frontend, uuid, ic, dump_path, &grace_timer, &hold_timer});
                        else RunBufferSuite(instance, frontend, uuid, ic, dump_path, &grace_timer,
                                            &hold_timer, &extra_timers);
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
