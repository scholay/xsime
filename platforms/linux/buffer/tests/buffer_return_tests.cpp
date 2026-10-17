#include <chrono>
#include <cstdlib>
#include <iostream>

#include "buffer_return.hpp"

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    using rimes::buffer::ReturnGesture;
    using Action = ReturnGesture::Action;
    using clock = std::chrono::steady_clock;

    const auto t0 = clock::now();
    ReturnGesture gesture;
    Expect(gesture.on_press(true, false, t0) == Action::SettleOnly, "composing press settles");
    Expect(gesture.on_release(t0 + std::chrono::milliseconds(10)) == Action::Consume,
           "settling release does not send");

    Expect(gesture.on_press(false, false, t0) == Action::ArmHold, "ready press arms hold");
    Expect(gesture.on_tick(t0 + std::chrono::milliseconds(400)) == Action::None,
           "short tick does not send all");
    Expect(gesture.progress(t0 + std::chrono::milliseconds(600)) > 0.4, "progress accrues");
    Expect(gesture.on_release(t0 + std::chrono::milliseconds(400)) == Action::SendNext,
           "short release sends next");

    Expect(gesture.on_press(false, false, t0) == Action::ArmHold, "second press arms");
    Expect(gesture.on_tick(t0 + std::chrono::milliseconds(1200)) == Action::SendAll,
           "1.2s tick sends all");
    Expect(gesture.on_release(t0 + std::chrono::milliseconds(1300)) == Action::Consume,
           "late release after send-all is consumed");

    Expect(gesture.on_press(false, false, t0) == Action::ArmHold, "third press");
    Expect(gesture.on_press(false, true, t0 + std::chrono::milliseconds(30)) == Action::Consume,
           "repeat is consumed");
    gesture.cancel();
    Expect(gesture.on_release(t0 + std::chrono::milliseconds(50)) == Action::None,
           "cancel drops the release");

    Expect(gesture.on_press(false, false, t0, true) == Action::SendNext,
           "ready derived output sends its first child on key down");
    Expect(gesture.on_release(t0 + std::chrono::milliseconds(100)) == Action::Consume,
           "derived key up never sends another child");
    Expect(gesture.on_press(false, false, t0, true) == Action::SendNext,
           "next derived press sends next child immediately");
    Expect(gesture.on_tick(t0 + std::chrono::milliseconds(1200)) == Action::SendAll,
           "derived hold sends remaining children");
    Expect(gesture.on_tick(t0 + std::chrono::milliseconds(1300)) == Action::None &&
           gesture.on_release(t0 + std::chrono::milliseconds(1400)) == Action::Consume,
           "derived hold never repeats delivery");
    Expect(gesture.on_press(true, false, t0, true) == Action::SettleOnly &&
           gesture.on_tick(t0 + std::chrono::milliseconds(2000)) == Action::None,
           "composition disarms even a derived hold");

    if (failures != 0) {
        std::cerr << failures << " return-gesture checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "ok: buffer return gesture\n";
    return EXIT_SUCCESS;
}
