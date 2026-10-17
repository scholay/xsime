#include "session_monitor.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>
#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/dbus/variant.h>

namespace fcitx {
namespace {
constexpr auto login = "org.freedesktop.login1";
constexpr auto managerPath = "/org/freedesktop/login1";
constexpr auto managerInterface = "org.freedesktop.login1.Manager";
constexpr auto sessionInterface = "org.freedesktop.login1.Session";
constexpr auto properties = "org.freedesktop.DBus.Properties";
constexpr uint64_t timeout = 2000000;
using Slot = std::unique_ptr<dbus::Slot>;
}

struct SessionMonitor::Impl {
    enum Reason { Sleep, Locked, Inactive, Freedesktop, Gnome, Kde, Count };
    struct Channel { bool blocked = true; uint64_t revision = 0; Slot query; };
    struct Provider {
        dbus::Bus* bus = nullptr;
        const char *name = "", *path = "", *interface = "";
        std::string owner;
        bool seen = false;
        uint64_t epoch = 0;
        Slot ownerQuery, ownerWatch, sessionQuery;
        std::vector<Slot> signals, sessionSignals;
    };
    std::unique_ptr<dbus::Bus> system, desktop;
    std::array<Channel, Count> channels;
    std::array<Provider, 4> providers;
    std::string sessionPath;
    bool sessionSeen = false, active = false;
    std::function<void(bool)> changed;

    Impl(EventLoop& loop, std::function<void(bool)> callback) : changed(std::move(callback)) {
        Publish(); // Shield while asynchronous initial state is being read.
        system = Connect(dbus::BusType::System, loop);
        desktop = Connect(dbus::BusType::Session, loop);
        providers[0].bus = system.get(); providers[0].name = login;
        providers[0].path = managerPath; providers[0].interface = managerInterface;
        const char* names[] = {"org.freedesktop.ScreenSaver", "org.gnome.ScreenSaver", "org.kde.screensaver"};
        const char* paths[] = {"/org/freedesktop/ScreenSaver", "/org/gnome/ScreenSaver", "/ScreenSaver"};
        for (unsigned i = 1; i < providers.size(); ++i) {
            providers[i].bus = desktop.get(); providers[i].name = names[i - 1];
            providers[i].path = paths[i - 1]; providers[i].interface = names[i - 1];
        }
        // KDE exports only configure/AboutToLock on org.kde.screensaver;
        // GetActive/ActiveChanged belong to the standard adaptor on this path.
        providers[3].interface = "org.freedesktop.ScreenSaver";
        for (unsigned i = 0; i < providers.size(); ++i) Watch(i);
        Publish();
    }
    ~Impl() {
        // Slots are destroyed before their buses, with no callbacks left alive.
        for (auto& channel : channels) channel.query.reset();
        for (auto& provider : providers) {
            provider.signals.clear(); provider.sessionSignals.clear();
            provider.ownerQuery.reset(); provider.ownerWatch.reset(); provider.sessionQuery.reset();
        }
    }
    static std::unique_ptr<dbus::Bus> Connect(dbus::BusType type, EventLoop& loop) {
        try {
            auto bus = std::make_unique<dbus::Bus>(type);
            if (!bus->isOpen()) return {};
            bus->attachEventLoop(&loop); return bus;
        } catch (const std::exception&) { return {}; }
    }
    void Publish() {
        const bool next = std::any_of(channels.begin(), channels.end(), [](const auto& c) { return c.blocked; });
        if (next != active) { active = next; changed(active); }
    }
    void Assign(unsigned reason, bool blocked) { auto& c = channels[reason]; ++c.revision; c.blocked = blocked; }
    void Set(unsigned reason, bool blocked) { Assign(reason, blocked); Publish(); }
    void SetProvider(unsigned i, bool blocked) {
        if (i == 0) { Set(Sleep, blocked); Set(Locked, blocked); Set(Inactive, blocked); }
        else Set(i + 2, blocked);
    }
    bool Sender(unsigned i, const dbus::Message& message) const {
        return !providers[i].owner.empty() && message.sender() == providers[i].owner;
    }
    void Watch(unsigned i) {
        auto& p = providers[i];
        if (!p.bus) { SetProvider(i, false); return; }
        p.ownerWatch = p.bus->addMatch(dbus::MatchRule("org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "NameOwnerChanged", {p.name}), [this, i](dbus::Message& msg) {
                if (msg.signature() != "sss") return false;
                std::string name, oldOwner, newOwner; msg >> name >> oldOwner >> newOwner;
                if (msg && name == providers[i].name) Owner(i, newOwner);
                return true;
            });
        p.signals.push_back(p.bus->addMatch(dbus::MatchRule("", "/org/freedesktop/DBus/Local",
            "org.freedesktop.DBus.Local", "Disconnected"), [this, i](dbus::Message&) {
                SetProvider(i, true); return true;
            }));
        if (i == 0) {
            p.signals.push_back(p.bus->addMatch(dbus::MatchRule(login, managerPath, managerInterface, "SessionRemoved"),
                [this](dbus::Message& msg) {
                    if (!Sender(0, msg) || msg.signature() != "so") return false;
                    std::string id; dbus::ObjectPath path; msg >> id >> path;
                    if (msg && path.path() == sessionPath) {
                        Set(Locked, true); Set(Inactive, true); sessionPath.clear();
                        providers[0].sessionSignals.clear();
                    }
                    return true;
                }));
            p.signals.push_back(p.bus->addMatch(dbus::MatchRule(login, managerPath, managerInterface, "PrepareForSleep"),
                [this](dbus::Message& msg) {
                    if (!Sender(0, msg) || msg.signature() != "b") return false;
                    bool value = true; msg >> value;
                    if (msg) {
                        // Re-read locks before clearing sleep: a wake is not an unlock.
                        if (!value) RefreshLocks();
                        Set(Sleep, value);
                    }
                    return true;
                }));
        } else {
            p.signals.push_back(p.bus->addMatch(dbus::MatchRule(p.name, p.path, p.interface, "ActiveChanged"),
                [this, i](dbus::Message& msg) {
                    if (!Sender(i, msg) || msg.signature() != "b") return false;
                    bool value = true; msg >> value;
                    if (msg) Set(i + 2, value);
                    return true;
                }));
        }
        const auto epoch = p.epoch;
        p.ownerQuery = p.bus->serviceOwnerAsync(p.name, timeout, [this, i, epoch](dbus::Message& msg) {
            if (providers[i].epoch != epoch) return true;
            std::string owner;
            if (!msg.isError() && msg.signature() == "s") msg >> owner;
            Owner(i, owner); return true;
        });
    }
    void Owner(unsigned i, const std::string& owner) {
        auto& p = providers[i]; ++p.epoch;
        p.owner = owner;
        p.sessionQuery.reset(); p.sessionSignals.clear();
        if (owner.empty()) {
            // Missing on initial discovery is unsupported. Losing an observed
            // provider is unknown, never an implicit unlock.
            SetProvider(i, p.seen); return;
        }
        p.seen = true; SetProvider(i, true);
        if (i == 0) {
            sessionPath.clear();
            QueryProperty(Sleep, managerPath, managerInterface, "PreparingForSleep");
            ResolveSession(false);
        } else QuerySaver(i);
    }
    void QuerySaver(unsigned i) {
        auto& p = providers[i]; if (p.owner.empty()) return;
        auto& c = channels[i + 2]; Set(i + 2, true);
        const auto revision = c.revision;
        auto call = p.bus->createMethodCall(p.owner.c_str(), p.path, p.interface, "GetActive");
        c.query = call.callAsync(timeout, [this, i, revision](dbus::Message& msg) {
            if (channels[i + 2].revision != revision || msg.isError() || msg.signature() != "b") return true;
            bool value = true; msg >> value;
            if (msg) Set(i + 2, value);
            return true;
        });
    }
    void QueryProperty(unsigned reason, const std::string& path, const char* interface, const char* property) {
        auto& p = providers[0]; if (p.owner.empty()) return;
        auto& c = channels[reason]; Set(reason, true);
        const auto revision = c.revision;
        auto call = p.bus->createMethodCall(p.owner.c_str(), path.c_str(), properties, "Get");
        call << std::string(interface) << std::string(property);
        c.query = call.callAsync(timeout, [this, reason, revision](dbus::Message& msg) {
            if (channels[reason].revision != revision || msg.isError() || msg.signature() != "v") return true;
            dbus::Variant value; msg >> value;
            if (msg && value.signature() == "b") Set(reason, reason == Inactive ? !value.dataAs<bool>() : value.dataAs<bool>());
            return true;
        });
    }
    void ResolveSession(bool fallback) {
        auto& p = providers[0]; const auto epoch = p.epoch;
        const char* id = std::getenv("XDG_SESSION_ID");
        auto call = p.bus->createMethodCall(p.owner.c_str(), managerPath, managerInterface,
                                          fallback ? "GetSession" : "GetSessionByPID");
        if (fallback) call << std::string(id ? id : "");
        else call << static_cast<uint32_t>(getpid());
        p.sessionQuery = call.callAsync(timeout, [this, epoch, fallback](dbus::Message& msg) {
            auto& provider = providers[0]; if (provider.epoch != epoch) return true;
            dbus::ObjectPath path;
            if (!msg.isError() && msg.signature() == "o") msg >> path;
            if (msg.isError() || path.path().empty()) {
                const char* id = std::getenv("XDG_SESSION_ID");
                if (!fallback && id && *id && std::string(id).size() <= 128) ResolveSession(true);
                else if (!sessionSeen) { Set(Locked, false); Set(Inactive, false); }
                return true;
            }
            sessionSeen = true; sessionPath = path.path();
            WatchSession(); return true;
        });
    }
    void WatchSession() {
        auto& p = providers[0];
        p.sessionSignals.push_back(p.bus->addMatch(dbus::MatchRule(login, sessionPath, sessionInterface, "Lock"),
            [this](dbus::Message& msg) {
                if (Sender(0, msg) && msg.signature().empty()) Set(Locked, true);
                return true;
            }));
        p.sessionSignals.push_back(p.bus->addMatch(dbus::MatchRule(login, sessionPath, sessionInterface, "Unlock"),
            [this](dbus::Message& msg) {
                if (Sender(0, msg) && msg.signature().empty()) QueryProperty(Locked, sessionPath, sessionInterface, "LockedHint");
                return true;
            }));
        p.sessionSignals.push_back(p.bus->addMatch(dbus::MatchRule(login, sessionPath, properties, "PropertiesChanged", {sessionInterface}),
            [this](dbus::Message& msg) {
                if (!Sender(0, msg) || msg.signature() != "sa{sv}as") return false;
                std::string interface; std::vector<dbus::DictEntry<std::string, dbus::Variant>> values;
                std::vector<std::string> invalidated; msg >> interface >> values >> invalidated;
                if (!msg || interface != sessionInterface) return true;
                // One PropertiesChanged can unlock while marking the session
                // inactive. Publish its final aggregate atomically, otherwise
                // the first property could briefly expose a plaintext frame.
                for (const auto& name : invalidated) {
                    if (name == "LockedHint") QueryProperty(Locked, sessionPath, sessionInterface, "LockedHint");
                    if (name == "Active") QueryProperty(Inactive, sessionPath, sessionInterface, "Active");
                }
                for (const auto& value : values) {
                    if (value.value().signature() != "b" || std::find(invalidated.begin(), invalidated.end(), value.key()) != invalidated.end()) continue;
                    if (value.key() == "LockedHint") Assign(Locked, value.value().dataAs<bool>());
                    if (value.key() == "Active") Assign(Inactive, !value.value().dataAs<bool>());
                }
                Publish();
                return true;
            }));
        QueryProperty(Locked, sessionPath, sessionInterface, "LockedHint");
        QueryProperty(Inactive, sessionPath, sessionInterface, "Active");
    }
    void RefreshLocks() {
        if (!sessionPath.empty()) {
            QueryProperty(Locked, sessionPath, sessionInterface, "LockedHint");
            QueryProperty(Inactive, sessionPath, sessionInterface, "Active");
        }
        for (unsigned i = 1; i < providers.size(); ++i) QuerySaver(i);
    }
};
SessionMonitor::SessionMonitor(EventLoop& loop, std::function<void(bool)> changed)
    : impl_(std::make_unique<Impl>(loop, std::move(changed))) {}
SessionMonitor::~SessionMonitor() = default;
bool SessionMonitor::protectedSession() const { return impl_->active; }
}  // namespace fcitx
