#pragma once
#include "capsule_password.hpp"
#include "clipboard_desktop.hpp"
#include <algorithm>
#include <mutex>
#include <optional>
#include <thread>

namespace rimes::capsule {
struct ClipboardBackendState {
    using Clock = std::chrono::steady_clock;
    // Injectable only by the backend test harness; desktop callers use 60 seconds.
    std::function<Clock::time_point()> now = [] { return Clock::now(); };
    DesktopClipboard::Capture capture;
    DesktopClipboard::Status status;
    std::atomic<bool> stop{false}, allowed{false}, protected_input{true}, available{false};
    std::atomic<std::uint64_t> epoch{1}, write_epoch{1};
    std::mutex mutex;
    std::optional<ClipboardArchive> restore;
    std::function<void(bool)> restore_complete;
    DesktopClipboard::WriteGate restore_gate, commit_gate;
    std::uint64_t restore_epoch = 0, restore_write_epoch = 0;
    int wake = -1;
    // Linearize protection/revocation with the protocol's actual ownership write.
    // Decoding, allocation and compositor acknowledgments stay outside this gate.
    bool CommitWrite(std::uint64_t expected_epoch, std::uint64_t expected_write,
                     const std::function<void()>& commit) {
        bool written = false;
        auto guarded = [&] {
            std::lock_guard<std::mutex> lock(mutex);
            if (stop.load() || protected_input.load() || epoch.load() != expected_epoch ||
                write_epoch.load() != expected_write)
                return;
            commit();
            written = true;
        };
        // The optional package/credential gate encloses the actual ownership
        // write. Acquire its lock before mutex, as UI callers do when queueing.
        try {
            if (commit_gate)
                commit_gate(guarded);
            else
                guarded();
        } catch (const std::exception&) {
            return false;
        }
        return written;
    }
    bool Current(std::uint64_t value) const {
        return !stop.load() && !protected_input.load() && allowed.load() && epoch.load() == value;
    }
};
inline bool SensitiveClipboard(const ClipboardArchive& archive) {
    return std::any_of(archive.representations.begin(), archive.representations.end(),
                       [](const auto& v) { return v.type == "application/x-rimes-sensitive"; });
}
inline void WipeClipboard(ClipboardArchive& archive) {
    if (SensitiveClipboard(archive))
        for (auto& value : archive.representations)
            EraseSecret(value.bytes);
}
inline std::shared_ptr<ClipboardArchive> OwnClipboard(ClipboardArchive archive) {
    return {new ClipboardArchive(std::move(archive)), [](ClipboardArchive* value) {
                WipeClipboard(*value);
                delete value;
            }};
}
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
