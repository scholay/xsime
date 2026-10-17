#include "buffer_return.hpp"

#include <algorithm>

namespace rimes::buffer {

double ReturnGesture::progress(std::chrono::steady_clock::time_point now) const {
    if (!pending_ || settle_only_ || sent_all_) {
        return 0;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - down_at_);
    return std::clamp(static_cast<double>(elapsed.count()) / kHoldMilliseconds, 0.0, 1.0);
}

ReturnGesture::Action ReturnGesture::on_press(bool composing, bool is_repeat,
                                              std::chrono::steady_clock::time_point now, bool send_on_press) {
    if (is_repeat) {
        return pending_ ? Action::Consume : Action::None;
    }
    pending_ = true;
    settle_only_ = composing;
    sent_all_ = false;
    sent_next_ = send_on_press && !composing;
    down_at_ = now;
    return composing ? Action::SettleOnly : sent_next_ ? Action::SendNext : Action::ArmHold;
}

ReturnGesture::Action ReturnGesture::on_release(std::chrono::steady_clock::time_point now) {
    if (!pending_) {
        return Action::None;
    }
    pending_ = false;
    if (settle_only_) {
        settle_only_ = false;
        return Action::Consume;
    }
    if (sent_all_) {
        sent_all_ = false;
        return Action::Consume;
    }
    if (sent_next_) { sent_next_ = false; return Action::Consume; }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - down_at_);
    if (elapsed.count() >= kHoldMilliseconds) {
        sent_all_ = false;
        return Action::Consume;
    }
    return Action::SendNext;
}

ReturnGesture::Action ReturnGesture::on_tick(std::chrono::steady_clock::time_point now) {
    if (!pending_ || settle_only_ || sent_all_) {
        return Action::None;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - down_at_);
    if (elapsed.count() < kHoldMilliseconds) {
        return Action::None;
    }
    sent_all_ = true;
    return Action::SendAll;
}

void ReturnGesture::cancel() {
    pending_ = false;
    settle_only_ = false;
    sent_all_ = false;
    sent_next_ = false;
}

}  // namespace rimes::buffer
