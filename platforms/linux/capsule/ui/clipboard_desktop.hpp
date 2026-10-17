#pragma once
#include "capsule_history.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace rimes::capsule {
// Callbacks execute on the clipboard worker. UI callers must post to GTK and
// check Current(epoch) again before using/persisting received data.
class DesktopClipboard {
  public:
    using Capture = std::function<void(ClipboardArchive, ClipboardSource, std::uint64_t)>;
    using Status = std::function<void(std::string)>;
    using Now = std::function<std::chrono::steady_clock::time_point()>;
    DesktopClipboard(Capture capture, Status status, Now now = {});
    ~DesktopClipboard();
    void Configure(bool capture, bool protected_input);
    bool Current(std::uint64_t epoch) const;
    // Original representations are restored only by an explicit product action.
    // Sensitive writes add exclusion markers and withdraw their own source after 60 seconds.
    using WriteGate = std::function<void(const std::function<void()>&)>;
    bool Restore(ClipboardArchive archive, bool sensitive = false,
                 std::function<void(bool)> completed = {}, WriteGate gate = {});
    bool Available() const;
    void CancelWrites();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::capsule
