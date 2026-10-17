#pragma once
#include <functional>
#include <memory>
namespace rimes::mailbox {
// Reuses the Fcitx host's owner-checked lock/sleep/session monitor.
class DesktopSession {
  public:
    explicit DesktopSession(std::function<void(bool)> changed);
    ~DesktopSession();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::mailbox
