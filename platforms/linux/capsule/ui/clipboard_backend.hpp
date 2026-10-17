#pragma once
#include "clipboard_desktop.hpp"
#include <mutex>
#include <optional>
#include <thread>

namespace rimes::capsule {
struct ClipboardBackendState {
    DesktopClipboard::Capture capture;
    DesktopClipboard::Status status;
    std::atomic<bool> stop{false}, allowed{false}, protected_input{true}, available{false};
    std::atomic<std::uint64_t> epoch{1}, write_epoch{1};
    std::mutex mutex;
    std::optional<ClipboardArchive> restore;
    std::function<void(bool)> restore_complete;
    std::uint64_t restore_epoch = 0, restore_write_epoch = 0;
    int wake = -1;
    // Linearize protection/revocation with the protocol's actual ownership write.
    // Decoding, allocation and compositor acknowledgments stay outside this gate.
    bool CommitWrite(std::uint64_t expected_epoch, std::uint64_t expected_write,
                     const std::function<void()>& commit) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stop.load() || protected_input.load() || epoch.load() != expected_epoch ||
            write_epoch.load() != expected_write)
            return false;
        commit();
        return true;
    }
    bool Current(std::uint64_t value) const {
        return !stop.load() && !protected_input.load() && allowed.load() && epoch.load() == value;
    }
};
class ClipboardBackend {
  public:
    explicit ClipboardBackend(ClipboardBackendState& state) : state_(state) {}
    virtual ~ClipboardBackend() = default;
    virtual void Tick() = 0;
    virtual int SocketFd() const = 0;
    virtual bool Busy() const = 0;
    bool Restore(ClipboardArchive archive) {
        return Restore(std::move(archive), state_.epoch.load(), state_.write_epoch.load());
    }
    virtual bool Restore(ClipboardArchive archive, std::uint64_t epoch,
                         std::uint64_t write_epoch) = 0;

  protected:
    ClipboardBackendState& state_;
};
std::unique_ptr<ClipboardBackend> X11Clipboard(ClipboardBackendState& state);
std::unique_ptr<ClipboardBackend> WaylandClipboard(ClipboardBackendState& state);
} // namespace rimes::capsule
