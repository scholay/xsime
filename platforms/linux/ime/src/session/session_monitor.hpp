#pragma once

#include <functional>
#include <memory>

namespace fcitx {
class EventLoop;

// Runs entirely on the Fcitx event loop. A callback only announces aggregate
// protection; clearing one reason cannot override another desktop's lock.
class SessionMonitor {
public:
    SessionMonitor(EventLoop& loop, std::function<void(bool)> changed);
    ~SessionMonitor();
    SessionMonitor(const SessionMonitor&) = delete;
    SessionMonitor& operator=(const SessionMonitor&) = delete;
    bool protectedSession() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace fcitx
