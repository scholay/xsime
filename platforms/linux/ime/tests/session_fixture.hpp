#pragma once
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/dbus/variant.h>

// Isolated dbus-run-session only; never sends lock signals to the host desktop.
class SessionFixture {
public:
    static constexpr auto login = "org.freedesktop.login1";
    static constexpr auto manager = "/org/freedesktop/login1";
    static constexpr auto path = "/org/freedesktop/login1/session/fixture";
    static constexpr auto sessionInterface = "org.freedesktop.login1.Session";
    static constexpr auto saver = "org.freedesktop.ScreenSaver";
    static constexpr auto saverPath = "/org/freedesktop/ScreenSaver";
    fcitx::dbus::Bus bus{fcitx::dbus::BusType::Session};
    bool locked = false, sleeping = false, active = true, saverActive = false;
    bool deferSaver = false;
    bool aliases = false;
    unsigned reads = 0;
    std::unique_ptr<fcitx::dbus::Message> deferred;
    std::vector<std::unique_ptr<fcitx::dbus::Slot>> objects;
    explicit SessionFixture(fcitx::EventLoop& loop, bool otherProviders = false) : aliases(otherProviders) {
        const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        if (!address || !std::getenv("RIMES_PRIVATE_SESSION_TEST")) throw std::runtime_error("private test bus required");
        setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);
        bus.attachEventLoop(&loop);
        if (!bus.requestName(login, fcitx::dbus::RequestNameFlag::None) ||
            !bus.requestName(saver, fcitx::dbus::RequestNameFlag::None)) throw std::runtime_error("fixture name unavailable");
        if (aliases) {
            bus.requestName("org.gnome.ScreenSaver", fcitx::dbus::RequestNameFlag::None);
            bus.requestName("org.kde.screensaver", fcitx::dbus::RequestNameFlag::None);
        }
        for (const auto* objectPath : {manager, path, saverPath, "/org/gnome/ScreenSaver", "/ScreenSaver"}) {
            objects.push_back(bus.addObject(objectPath, [this](fcitx::dbus::Message& msg) {
                auto reply = msg.createReply();
                if (msg.member() == "GetSessionByPID" || msg.member() == "GetSession") reply << fcitx::dbus::ObjectPath(path);
                else if (msg.member() == "GetActive") {
                    const auto expected = msg.path() == "/org/gnome/ScreenSaver" ? "org.gnome.ScreenSaver" : saver;
                    if (msg.interface() != expected) {
                        msg.createError("org.freedesktop.DBus.Error.UnknownMethod", "wrong fixture interface").send(); return true;
                    }
                    ++reads;
                    if (deferSaver) { deferred = std::make_unique<fcitx::dbus::Message>(std::move(reply)); return true; }
                    reply << saverActive;
                } else if (msg.member() == "Get") {
                    ++reads; std::string interface, name; msg >> interface >> name;
                    if (name == "LockedHint") reply << fcitx::dbus::Variant(locked);
                    else if (name == "Active") reply << fcitx::dbus::Variant(active);
                    else if (name == "PreparingForSleep") reply << fcitx::dbus::Variant(sleeping);
                    else return false;
                } else return false;
                reply.send(); return true;
            }));
        }
    }
    void Signal(const char* object, const char* interface, const char* member, bool value) {
        auto signal = bus.createSignal(object, interface, member); signal << value; signal.send(); bus.flush();
    }
    void Saver(bool value) {
        saverActive = value; Signal(saverPath, saver, "ActiveChanged", value);
        if (aliases) {
            Signal("/org/gnome/ScreenSaver", "org.gnome.ScreenSaver", "ActiveChanged", value);
            Signal("/ScreenSaver", saver, "ActiveChanged", value);
        }
    }
    void Sleep(bool value) { sleeping = value; Signal(manager, "org.freedesktop.login1.Manager", "PrepareForSleep", value); }
    void Property(const char* name, bool value) {
        Properties({{name, value}});
    }
    void Properties(const std::vector<std::pair<std::string, bool>>& updates) {
        auto signal = bus.createSignal(path, "org.freedesktop.DBus.Properties", "PropertiesChanged");
        std::vector<fcitx::dbus::DictEntry<std::string, fcitx::dbus::Variant>> values;
        for (const auto& [name, value] : updates) {
            if (name == "LockedHint") locked = value;
            if (name == "Active") active = value;
            values.emplace_back(name, fcitx::dbus::Variant(value));
        }
        signal << std::string(sessionInterface) << values << std::vector<std::string>{}; signal.send(); bus.flush();
    }
};
