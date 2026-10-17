#include "session/session_monitor.hpp"
#include "session/protected_input.hpp"
#include "session_fixture.hpp"
#include <fcitx-utils/event.h>
#include <chrono>
#include <iostream>

int main() {
    using namespace fcitx;
    EventLoop loop;
    SessionFixture fixture(loop, true);
    fixture.locked = true; fixture.saverActive = true;
    std::vector<bool> changes;
    SessionMonitor monitor(loop, [&](bool value) { changes.push_back(value); });
    dbus::Bus attacker(dbus::BusType::Session);
    int step = 0, result = 1;
    const auto started = now(CLOCK_MONOTONIC);
    uint64_t checkpoint = started;
    auto timer = loop.addTimeEvent(CLOCK_MONOTONIC, started + 50000, 0,
        [&](EventSourceTime* source, uint64_t) {
            try {
                auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
                const auto current = now(CLOCK_MONOTONIC);
                require(current - started < 5000000, "session test deadline");
                if (current - checkpoint < 50000) {
                    source->setTime(current + 10000); source->setOneShot(); return true;
                }
                if (step == 0 && fixture.reads >= 4) {
                    require(monitor.protectedSession(), "initial locked session exposed");
                    fixture.Saver(false); ++step;
                } else if (step == 1) {
                    require(monitor.protectedSession(), "saver unlock cleared login lock");
                    fixture.Properties({{"LockedHint", false}, {"Active", false}}); ++step;
                } else if (step == 2) {
                    require(monitor.protectedSession(), "session inactive was ignored");
                    fixture.Sleep(true); fixture.Property("Active", true); ++step;
                } else if (step == 3) {
                    require(monitor.protectedSession(), "session activation cleared sleep");
                    fixture.Property("LockedHint", true); fixture.Sleep(false); ++step;
                } else if (step == 4) {
                    require(monitor.protectedSession(), "wake cleared a locked session");
                    require(changes.size() == 1, "overlapping transitions briefly exposed plaintext");
                    fixture.Property("LockedHint", false); ++step;
                } else if (step == 5) {
                    require(!monitor.protectedSession(), "all reasons released but stayed locked");
                    auto spoof = attacker.createSignal(SessionFixture::saverPath, SessionFixture::saver, "ActiveChanged");
                    spoof << true; spoof.send(); attacker.flush(); ++step;
                } else if (step == 6) {
                    require(!monitor.protectedSession(), "unowned signal changed protection");
                    auto malformed = fixture.bus.createSignal(SessionFixture::saverPath, SessionFixture::saver, "ActiveChanged");
                    malformed << std::string("false"); malformed.send(); fixture.bus.flush(); ++step;
                } else if (step == 7) {
                    require(!monitor.protectedSession(), "malformed signal changed protection");
                    fixture.bus.releaseName(SessionFixture::saver); ++step;
                } else if (step == 8) {
                    require(monitor.protectedSession(), "provider loss silently unprotected");
                    fixture.deferSaver = true;
                    fixture.bus.requestName(SessionFixture::saver, dbus::RequestNameFlag::None); ++step;
                } else if (step == 9 && fixture.deferred) {
                    require(monitor.protectedSession(), "provider restart reused stale state");
                    fixture.Saver(true); ++step;
                } else if (step == 10) {
                    *fixture.deferred << false; fixture.deferred->send(); fixture.bus.flush(); fixture.deferred.reset(); ++step;
                } else if (step == 11) {
                    require(monitor.protectedSession(), "late initial reply overrode newer lock signal");
                    fixture.Saver(false); ++step;
                } else if (step == 12) {
                    require(!monitor.protectedSession(), "fresh unlock did not release protection");
                    require(ProtectedInput(CapabilityFlag::Password) && ProtectedInput(CapabilityFlag::Sensitive) &&
                        ProtectedInput(CapabilityFlag::Disable) && !ProtectedInput(CapabilityFlag::Preedit), "capability mask incorrect");
                    require(changes.front() && !changes.back(), "initial/final callback incorrect");
                    std::cout << "Session D-Bus: startup, overlap, wake, owner loss/restart, spoof rejection, stale query and sensitive flags passed\n";
                    result = 0; loop.exit(); return true;
                }
                checkpoint = current;
                source->setTime(current + 10000); source->setOneShot();
            } catch (const std::exception& error) { std::cerr << error.what() << '\n'; loop.exit(); }
            return true;
        });
    loop.exec(); return result;
}
