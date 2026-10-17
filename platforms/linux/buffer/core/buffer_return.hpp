#pragma once

#include <chrono>
#include <cstdint>

namespace rimes::buffer {

// Return tap / 1.2s hold state machine. A composing press only settles and
// must not send on the same physical key. Repeats and late releases never
// create a second send.
class ReturnGesture {
public:
    static constexpr std::int64_t kHoldMilliseconds = 1200;

    enum class Action {
        None,
        SettleOnly,
        ArmHold,
        SendNext,
        SendAll,
        Consume,
    };

    bool pending() const { return pending_; }
    bool settle_only() const { return settle_only_; }
    bool sent_all() const { return sent_all_; }
    double progress(std::chrono::steady_clock::time_point now) const;

    Action on_press(bool composing, bool is_repeat,
                    std::chrono::steady_clock::time_point now, bool send_on_press = false);
    Action on_release(std::chrono::steady_clock::time_point now);
    Action on_tick(std::chrono::steady_clock::time_point now);
    void cancel();

private:
    bool pending_ = false;
    bool settle_only_ = false;
    bool sent_all_ = false;
    bool sent_next_ = false;
    std::chrono::steady_clock::time_point down_at_{};
};

}  // namespace rimes::buffer
