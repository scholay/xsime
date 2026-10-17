#pragma once
#include "capsule_history.hpp"
#include <atomic>
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
    DesktopClipboard(Capture capture, Status status);
    ~DesktopClipboard();
    void Configure(bool capture, bool protected_input);
    bool Current(std::uint64_t epoch) const;
    // Original representations are restored only by an explicit product action.
    // Sensitive writes add exclusion markers and are separately cleared by the vault.
    bool Restore(ClipboardArchive archive, bool sensitive = false,
                 std::function<void(bool)> completed = {});
    bool Available() const;
    void CancelWrites();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::capsule
