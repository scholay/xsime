#include "desktop_shortcuts.hpp"
#include "private_files.hpp"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>
#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/dbus/variant.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <xcb/xcb.h>

namespace fcitx {
namespace {
bool Wayland() {
    const char* display = std::getenv("WAYLAND_DISPLAY");
    const char* type = std::getenv("XDG_SESSION_TYPE");
    return (display && *display) || (type && std::string(type) == "wayland");
}
bool Available(const std::string& id) { return id == "buffer" || id == "capsule" || id == "settings"; }
constexpr auto kPortal = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kShortcuts = "org.freedesktop.portal.GlobalShortcuts";
using Properties = std::vector<dbus::DictEntry<std::string, dbus::Variant>>;
using PortalBindings = std::vector<dbus::DBusStruct<std::string, Properties>>;
}
struct DesktopShortcuts::Impl {
    EventLoop& loop;
    Open open;
    std::function<Json()> lastResult;
    Json bindings = Json::object(), states = Json::object();
    bool enabled = false, configured = false, destroying = false;
    std::string backend = "fcitx", detail;
    std::string recordingOwner, recordingToken;
    std::unique_ptr<EventSourceTime> recordingTimeout;
    std::unique_ptr<dbus::Bus> bus;
    std::unique_ptr<dbus::Slot> service;
    xcb_connection_t* xcb = nullptr;
    std::unique_ptr<EventSourceIO> xcbEvents;
    struct Grab { uint8_t code; uint16_t mask; std::string id; };
    std::vector<Grab> grabs;
    std::set<uint8_t> pressed;
    std::map<uint8_t, uint32_t> releases;
    uint16_t ignoredLocks = XCB_MOD_MASK_LOCK;
    std::string portalOwner, portalSession, portalRequest;
    uint64_t portalEpoch = 0, ownerRevision = 0;
    std::unique_ptr<dbus::Slot> portalOwnerQuery, portalStart, portalCall, portalResponse;
    std::vector<std::unique_ptr<dbus::Slot>> portalSignals;
    std::unique_ptr<EventSourceTime> portalTimeout;
    std::set<std::string> portalPressed;
    std::vector<std::unique_ptr<dbus::Slot>> retiredSlots;
    std::vector<std::unique_ptr<EventSourceTime>> retiredTimers;
    std::unique_ptr<EventSourceTime> retireSweep;

    Impl(EventLoop& eventLoop, Open callback, std::function<Json()> result)
        : loop(eventLoop), open(std::move(callback)), lastResult(std::move(result)) {
        try {
            bus = std::make_unique<dbus::Bus>(dbus::BusType::Session);
            if (!bus->isOpen()) { bus.reset(); return; }
            bus->attachEventLoop(&loop);
            if (!bus->requestName(kLauncherName, dbus::RequestNameFlag::None)) return;
            service = bus->addObject(kLauncherPath, [this](dbus::Message& msg) {
                if (msg.type() != dbus::MessageType::MethodCall || msg.interface() != kLauncherInterface) return false;
                Json response;
                if (msg.member() == "Open" && msg.signature() == "s") {
                    std::string action; msg >> action;
                    response = action.size() <= 32 ? open(action) : Json{{"ok", false}, {"status", "invalid-action"}};
                } else if (msg.member() == "GetStatus" && msg.signature().empty()) response = Status();
                else if (msg.member() == "Retry" && msg.signature().empty()) {
                    const auto saved = bindings; const bool active = enabled;
                    configured = false; Configure(saved, active); response = Status();
                } else if (msg.member() == "BeginRecording" && msg.signature() == "s") {
                    std::string token; msg >> token;
                    if (token.empty() || token.size() > 64) response = {{"ok", false}};
                    else if (!recordingOwner.empty() && recordingOwner != msg.sender())
                        response = {{"ok", false}, {"message", "另一个设置窗口正在录入快捷键。"}};
                    else {
                        recordingOwner = msg.sender(); recordingToken = token;
                        const auto saved = bindings; configured = false; Configure(saved, enabled);
                        if (!recordingTimeout) recordingTimeout = loop.addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 120000000, 0,
                            [this](EventSourceTime*, uint64_t) { EndRecording(); return true; });
                        else { recordingTimeout->setTime(now(CLOCK_MONOTONIC) + 120000000); recordingTimeout->setOneShot(); }
                        response = Status();
                    }
                } else if (msg.member() == "EndRecording" && msg.signature() == "s") {
                    std::string token; msg >> token;
                    if (recordingOwner == msg.sender() && recordingToken == token) EndRecording();
                    response = Status();
                } else return false;
                auto reply = msg.createReply(); reply << response.dump(); reply.send(); return true;
            });
            portalSignals.push_back(bus->addMatch(dbus::MatchRule("org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "NameOwnerChanged"), [this](dbus::Message& msg) {
                    if (!recordingOwner.empty() && msg.signature() == "sss") {
                        std::string name, before, after; msg >> name >> before >> after;
                        if (msg && name == recordingOwner && after.empty()) EndRecording();
                    }
                    return true;
                }));
            WatchPortal();
        } catch (const std::exception&) {
            portalSignals.clear(); portalOwnerQuery.reset(); service.reset(); bus.reset();
        }
    }
    ~Impl() { destroying = true; ClosePortal(); CloseX11(); }
    void ScheduleSweep() {
        // A D-Bus response can replace or cancel its own Slot. Destroying that
        // Slot in the callback also frees the lambda's captures before return.
        if (!retireSweep) retireSweep = loop.addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 1, 0,
            [this](EventSourceTime*, uint64_t) { retiredSlots.clear(); retiredTimers.clear(); return true; });
        else { retireSweep->setTime(now(CLOCK_MONOTONIC) + 1); retireSweep->setOneShot(); }
    }
    void Retire(std::unique_ptr<dbus::Slot>& slot) {
        if (destroying) { slot.reset(); return; }
        if (slot) { retiredSlots.push_back(std::move(slot)); ScheduleSweep(); }
    }
    void RetireTimeout() {
        if (destroying) { portalTimeout.reset(); return; }
        if (portalTimeout) { retiredTimers.push_back(std::move(portalTimeout)); ScheduleSweep(); }
    }
    Json Status() const {
        return {{"enabled", enabled}, {"backend", backend}, {"message", detail},
                {"recording", !recordingOwner.empty()},
                {"actions", states}, {"launcher", static_cast<bool>(service)}, {"lastAction", lastResult()}};
    }
    void EndRecording() {
        recordingOwner.clear(); recordingToken.clear();
        if (recordingTimeout) recordingTimeout->setEnabled(false);
        const auto saved = bindings; const bool active = enabled;
        configured = false; Configure(saved, active);
    }
    void Configure(const Json& next, bool active) {
        if (configured && next == bindings && enabled == active) return;
        configured = true; bindings = next; enabled = active;
        ClosePortal(); CloseX11(); states = Json::object(); detail.clear(); backend = "fcitx";
        for (auto it = bindings.begin(); it != bindings.end(); ++it)
            states[it.key()] = { {"state", !Available(it.key()) ? "unavailable" : it.value() == "" ? "unbound" : "input-method"},
                                 {"trigger", it.value()} };
        if (!recordingOwner.empty()) { backend = "recording"; PortalState("recording", "正在录入快捷键；全局绑定暂时停用。"); return; }
        if (!enabled) { detail = "全局快捷键已关闭；XSIME 输入时仍可使用已设快捷键。"; return; }
        if (!service) { PortalState("unavailable", "XSIME 启动服务不可用，请重启 Fcitx5 后重试；输入框内快捷键仍可用。"); return; }
        if (Wayland()) {
            StartPortal();
            return;
        }
        OpenX11();
    }
    void CloseX11() {
        xcbEvents.reset(); grabs.clear(); pressed.clear(); releases.clear();
        if (xcb) { xcb_disconnect(xcb); xcb = nullptr; }
    }
    void PortalState(const char* state, const char* message) {
        detail = message;
        for (auto it = bindings.begin(); it != bindings.end(); ++it)
            if (Available(it.key()) && it.value() != "") states[it.key()]["state"] = state;
    }
    void ClosePortal() {
        ++portalEpoch; portalPressed.clear(); RetireTimeout();
        Retire(portalCall); Retire(portalResponse);
        if (bus && !portalOwner.empty()) {
            for (const auto& [path, interface] : std::vector<std::pair<std::string, const char*>>{
                {portalRequest, "org.freedesktop.portal.Request"}, {portalSession, "org.freedesktop.portal.Session"}})
                if (!path.empty()) {
                    auto close = bus->createMethodCall(portalOwner.c_str(), path.c_str(), interface, "Close");
                    close.send();
                }
        }
        portalSession.clear(); portalRequest.clear();
    }
    void WatchPortal() {
        dbus::VariantTypeRegistry::defaultRegistry().registerType<PortalBindings>();
        portalSignals.push_back(bus->addMatch(dbus::MatchRule("org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "NameOwnerChanged", {kPortal}), [this](dbus::Message& msg) {
                if (msg.signature() != "sss") return false;
                std::string name, oldOwner, newOwner; msg >> name >> oldOwner >> newOwner;
                if (msg) {
                    ++ownerRevision;
                    ClosePortal(); portalOwner = newOwner;
                    if (configured && enabled && recordingOwner.empty() && Wayland()) StartPortal();
                }
                return true;
            }));
        for (const char* member : {"Activated", "Deactivated"}) {
            const bool activation = std::string(member) == "Activated";
            portalSignals.push_back(bus->addMatch(dbus::MatchRule(kPortal, kPortalPath, kShortcuts, member),
                [this, activation](dbus::Message& msg) {
                    if (!enabled || msg.sender() != portalOwner || msg.signature() != "osta{sv}") return false;
                    dbus::ObjectPath session; std::string id; uint64_t timestamp = 0; Properties options;
                    msg >> session >> id >> timestamp >> options;
                    if (!msg || portalSession.empty() || session.path() != portalSession ||
                        !states.contains(id) || states[id].value("state", "") != "registered") return true;
                    if (!activation) portalPressed.erase(id);
                    else if (portalPressed.insert(id).second) open(id);
                    return true;
                }));
        }
        portalSignals.push_back(bus->addMatch(dbus::MatchRule(kPortal, kPortalPath, kShortcuts, "ShortcutsChanged"),
            [this](dbus::Message& msg) {
                if (msg.sender() != portalOwner || msg.signature() != "oa(sa{sv})") return false;
                dbus::ObjectPath session; PortalBindings values; msg >> session >> values;
                if (msg && !portalSession.empty() && session.path() == portalSession) PortalBound(values);
                return true;
            }));
        portalSignals.push_back(bus->addMatch(dbus::MatchRule(kPortal, "", "org.freedesktop.portal.Session", "Closed"),
            [this](dbus::Message& msg) {
                if (msg.sender() == portalOwner && !portalSession.empty() && msg.path() == portalSession) {
                    ClosePortal(); PortalState("unbound", "桌面已关闭快捷键会话；可重新绑定。");
                }
                return true;
            }));
        const auto owner_version = ownerRevision;
        portalOwnerQuery = bus->serviceOwnerAsync(kPortal, 2000000, [this, owner_version](dbus::Message& msg) {
            if (owner_version != ownerRevision || !portalOwner.empty()) return true;
            if (!msg.isError() && msg.signature() == "s") msg >> portalOwner;
            if (configured && enabled && recordingOwner.empty() && Wayland()) StartPortal();
            return true;
        });
    }
    std::string PortalPath(const char* kind, const std::string& token) const {
        auto sender = bus->uniqueName();
        if (!sender.empty() && sender.front() == ':') sender.erase(0, 1);
        std::replace(sender.begin(), sender.end(), '.', '_');
        return std::string(kPortalPath) + "/" + kind + "/" + sender + "/" + token;
    }
    void PortalRequest(const char* method, bool bind) {
        Retire(portalResponse); Retire(portalCall); RetireTimeout();
        const auto epoch = portalEpoch;
        const auto token = "xsime_" + rimes::runtime::RandomID();
        portalRequest = PortalPath("request", token);
        const auto path = portalRequest;
        const auto sessionToken = "xsime_" + rimes::runtime::RandomID();
        const auto expectedSession = bind ? portalSession : PortalPath("session", sessionToken);
        portalResponse = bus->addMatch(dbus::MatchRule(kPortal, path, "org.freedesktop.portal.Request", "Response"),
            [this, epoch, path, expectedSession, bind](dbus::Message& msg) {
                if (epoch != portalEpoch || path != portalRequest || msg.sender() != portalOwner || msg.signature() != "ua{sv}") return false;
                uint32_t response = 2; Properties results; msg >> response >> results;
                RetireTimeout(); portalRequest.clear();
                if (!msg || response != 0) {
                    ClosePortal(); PortalState("not-granted", "桌面尚未授权快捷键；可重新绑定或使用桌面启动入口。"); return true;
                }
                if (!bind) {
                    auto found = std::find_if(results.begin(), results.end(), [](const auto& p) { return p.key() == "session_handle"; });
                    if (found == results.end() || found->value().signature() != "s" || found->value().template dataAs<std::string>() != expectedSession) {
                        ClosePortal(); PortalState("unavailable", "桌面快捷键会话无效，请重试。"); return true;
                    }
                    portalSession = expectedSession; PortalRequest("BindShortcuts", true);
                } else {
                    auto found = std::find_if(results.begin(), results.end(), [](const auto& p) { return p.key() == "shortcuts"; });
                    if (found == results.end() || found->value().signature() != "a(sa{sv})") {
                        ClosePortal(); PortalState("unavailable", "桌面未返回已绑定的快捷键，请重试。"); return true;
                    }
                    PortalBound(found->value().template dataAs<PortalBindings>());
                }
                return true;
            });
        auto call = bus->createMethodCall(portalOwner.c_str(), kPortalPath, kShortcuts, method);
        Properties options{{"handle_token", dbus::Variant(token)}};
        if (bind) {
            PortalBindings requested;
            for (const auto& action : rimes::runtime::ShortcutActions()) {
                const auto id = action.at("id").get<std::string>();
                if (!Available(id) || bindings.at(id) == "") continue;
                requested.emplace_back(id, Properties{{"description", dbus::Variant(action.at("title").get<std::string>())},
                    {"preferred_trigger", dbus::Variant(bindings.at(id).get<std::string>())}});
            }
            call << dbus::ObjectPath(portalSession) << requested << std::string{} << options;
        } else {
            options.emplace_back("session_handle_token", dbus::Variant(sessionToken)); call << options;
        }
        portalCall = call.callAsync(5000000, [this, epoch, path](dbus::Message& msg) {
            if (epoch != portalEpoch || path != portalRequest) return true;
            dbus::ObjectPath returned;
            if (!msg.isError() && msg.signature() == "o") msg >> returned;
            if (msg.isError() || returned.path() != path) {
                ClosePortal(); PortalState("unavailable", "桌面不支持全局快捷键；请在系统快捷键中绑定 XSIME 启动入口。");
            }
            return true;
        });
        portalTimeout = loop.addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 600000000, 0,
            [this, epoch, path](EventSourceTime*, uint64_t) {
                if (epoch == portalEpoch && path == portalRequest) {
                    ClosePortal(); PortalState("not-granted", "等待桌面确认已结束，可重新绑定。");
                }
                return true;
            });
    }
    void PortalBound(const PortalBindings& values) {
        PortalState("not-granted", "桌面快捷键绑定已更新。"); portalPressed.clear();
        for (const auto& value : values) {
            const auto& id = std::get<0>(value);
            if (!Available(id) || !states.contains(id) || bindings.at(id) == "") continue;
            states[id]["state"] = "registered";
            for (const auto& property : std::get<1>(value))
                if (property.key() == "trigger_description" && property.value().signature() == "s") {
                    const auto& description = property.value().template dataAs<std::string>();
                    if (description.size() <= 256) states[id]["trigger"] = description;
                }
        }
    }
    void StartPortal() {
        backend = "portal";
        if (!bus) {
            PortalState("unavailable", "桌面未提供全局快捷键；可在系统快捷键中绑定 XSIME 启动入口。"); return;
        }
        if (portalOwner.empty()) {
            PortalState("pending", "正在连接桌面快捷键服务…");
            auto start = bus->createMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "StartServiceByName");
            start << std::string(kPortal) << uint32_t{0};
            const auto epoch = portalEpoch;
            portalStart = start.callAsync(5000000, [this, epoch](dbus::Message& msg) {
                if (epoch != portalEpoch || !portalOwner.empty()) return true;
                if (msg.isError()) { PortalState("unavailable", "桌面未提供全局快捷键；请使用桌面启动入口或系统自定义快捷键。"); return true; }
                const auto owner_version = ownerRevision;
                portalOwnerQuery = bus->serviceOwnerAsync(kPortal, 2000000, [this, epoch, owner_version](dbus::Message& result) {
                    if (epoch != portalEpoch || owner_version != ownerRevision || !portalOwner.empty()) return true;
                    if (!result.isError() && result.signature() == "s") result >> portalOwner;
                    if (portalOwner.empty()) PortalState("unavailable", "无法连接桌面快捷键服务，请重试。");
                    else StartPortal();
                    return true;
                });
                return true;
            });
            return;
        }
        PortalState("pending", "正在等待桌面确认全局快捷键…");
        PortalRequest("CreateSession", false);
    }
    void OpenX11() {
        const char* display = std::getenv("DISPLAY");
        if (!display || !*display) { detail = "无桌面连接；快捷键在 Fcitx 输入框中可用。"; return; }
        xcb = xcb_connect(nullptr, nullptr);
        if (!xcb || xcb_connection_has_error(xcb)) { CloseX11(); detail = "无法连接 X11；可使用桌面启动入口。"; return; }
        backend = "x11";
        GrabX11();
        xcbEvents = loop.addIOEvent(xcb_get_file_descriptor(xcb), IOEventFlag::In, [this](EventSourceIO*, int, IOEventFlags) {
            while (auto* event = xcb_poll_for_event(xcb)) {
                const auto kind = event->response_type & 0x7f;
                if (kind == XCB_MAPPING_NOTIFY) GrabX11();
                else if (kind == XCB_KEY_RELEASE) {
                    const auto* key = reinterpret_cast<xcb_key_release_event_t*>(event);
                    releases[key->detail] = key->time;
                } else if (kind == XCB_KEY_PRESS) {
                    const auto* key = reinterpret_cast<xcb_key_press_event_t*>(event);
                    const auto release = releases.find(key->detail);
                    const bool repeated = pressed.count(key->detail) && (release == releases.end() || release->second == key->time);
                    pressed.insert(key->detail); releases.erase(key->detail);
                    if (!repeated) for (const auto& binding : grabs)
                        if (key->detail == binding.code && (key->state & 0xffU & ~ignoredLocks) == binding.mask) { open(binding.id); break; }
                }
                std::free(event);
            }
            if (xcb_connection_has_error(xcb)) {
                PortalState("unavailable", "X11 连接已断开；请重新绑定快捷键。");
                return false;
            }
            return true;
        });
    }
    void GrabX11() {
        grabs.clear(); pressed.clear(); releases.clear();
        const auto* setup = xcb_get_setup(xcb);
        std::vector<xcb_window_t> roots;
        for (auto screen = xcb_setup_roots_iterator(setup); screen.rem; xcb_screen_next(&screen)) {
            roots.push_back(screen.data->root);
            xcb_ungrab_key(xcb, XCB_GRAB_ANY, screen.data->root, XCB_MOD_MASK_ANY);
        }
        auto* mapping = xcb_get_keyboard_mapping_reply(xcb, xcb_get_keyboard_mapping(xcb, setup->min_keycode,
            setup->max_keycode - setup->min_keycode + 1), nullptr);
        auto* modifiers = xcb_get_modifier_mapping_reply(xcb, xcb_get_modifier_mapping(xcb), nullptr);
        if (!mapping || !modifiers) {
            std::free(mapping); std::free(modifiers);
            PortalState("layout-unavailable", "无法读取键盘布局，请重新绑定快捷键。"); return;
        }
        const auto* symbols = xcb_get_keyboard_mapping_keysyms(mapping);
        auto codes = [&](KeySym symbol) {
            std::vector<uint8_t> result;
            for (unsigned code = setup->min_keycode; code <= setup->max_keycode; ++code)
                for (unsigned column = 0; column < mapping->keysyms_per_keycode; column += 2)
                    if (symbols[(code - setup->min_keycode) * mapping->keysyms_per_keycode + column] == static_cast<uint32_t>(symbol)) {
                        result.push_back(code); break;
                    }
            return result;
        };
        uint16_t numLock = 0, alt = XCB_MOD_MASK_1, logo = XCB_MOD_MASK_4;
        auto modifierFor = [&](KeySym symbol, uint16_t fallback) {
            const auto keys = codes(symbol); uint16_t mask = 0;
            const auto* modCodes = xcb_get_modifier_mapping_keycodes(modifiers);
            for (unsigned mod = 0; mod < 8; ++mod)
                for (unsigned key = 0; key < modifiers->keycodes_per_modifier; ++key)
                    if (std::find(keys.begin(), keys.end(), modCodes[mod * modifiers->keycodes_per_modifier + key]) != keys.end()) mask |= 1U << mod;
            return mask ? mask : fallback;
        };
        numLock = modifierFor(FcitxKey_Num_Lock, 0);
        alt = modifierFor(FcitxKey_Alt_L, alt); logo = modifierFor(FcitxKey_Super_L, logo);
        ignoredLocks = XCB_MOD_MASK_LOCK | numLock;
        for (auto it = bindings.begin(); it != bindings.end(); ++it) {
            if (!Available(it.key())) continue;
            const auto shortcut = rimes::runtime::ParseShortcut(it.value());
            if (!shortcut) continue;
            uint16_t mask = 0;
            if (shortcut->modifiers & rimes::runtime::Shortcut::Control) mask |= XCB_MOD_MASK_CONTROL;
            if (shortcut->modifiers & rimes::runtime::Shortcut::Shift) mask |= XCB_MOD_MASK_SHIFT;
            if (shortcut->modifiers & rimes::runtime::Shortcut::Alt) mask |= alt;
            if (shortcut->modifiers & rimes::runtime::Shortcut::Logo) mask |= logo;
            const auto keys = codes(Key::keySymFromString(shortcut->key));
            const std::set<uint16_t> locks{0, XCB_MOD_MASK_LOCK, numLock, static_cast<uint16_t>(numLock | XCB_MOD_MASK_LOCK)};
            // Different groups can put two configured symbols on the same
            // physical key. Do not overwrite or ungrab the earlier action.
            if (std::any_of(grabs.begin(), grabs.end(), [&](const Grab& grab) {
                return grab.mask == mask && std::find(keys.begin(), keys.end(), grab.code) != keys.end();
            })) { states[it.key()]["state"] = "conflict"; continue; }
            bool ok = !keys.empty();
            for (const auto root : roots) for (auto code : keys) for (auto lock : locks) {
                auto* error = xcb_request_check(xcb, xcb_grab_key_checked(xcb, false, root, mask | lock, code,
                    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
                if (error) ok = false;
                std::free(error);
            }
            if (ok) for (auto code : keys) grabs.push_back({code, mask, it.key()});
            else for (const auto root : roots) for (auto code : keys) for (auto lock : locks)
                xcb_ungrab_key(xcb, code, root, mask | lock);
            states[it.key()]["state"] = ok ? "registered" : keys.empty() ? "layout-unavailable" : "conflict";
        }
        std::free(mapping); std::free(modifiers); xcb_flush(xcb);
        detail = "X11 全局绑定已检查；冲突项可修改键位后重试。";
    }
};
DesktopShortcuts::DesktopShortcuts(EventLoop& loop, Open open, std::function<Json()> last_result)
    : impl_(std::make_unique<Impl>(loop, std::move(open), std::move(last_result))) {}
DesktopShortcuts::~DesktopShortcuts() = default;
void DesktopShortcuts::Configure(const Json& bindings, bool enabled) {
    rimes::runtime::ValidateShortcuts(bindings); impl_->Configure(bindings, enabled);
}
DesktopShortcuts::Json DesktopShortcuts::Status() const { return impl_->Status(); }
} // namespace fcitx
