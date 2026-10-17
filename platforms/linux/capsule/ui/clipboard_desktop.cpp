#include "clipboard_backend.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

namespace rimes::capsule {
struct DesktopClipboard::Impl {
    ClipboardBackendState state;
    std::thread worker;
    Impl(Capture capture, Status status) {
        state.capture = std::move(capture);
        state.status = std::move(status);
        state.wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (state.wake < 0)
            throw std::runtime_error("Clipboard observer unavailable");
        worker = std::thread([this] {
            sigset_t blocked;
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
            while (!state.stop.load()) {
                std::function<void(bool)> completed;
                try {
                    const auto* wayland = std::getenv("WAYLAND_DISPLAY");
                    auto backend =
                        wayland && *wayland ? WaylandClipboard(state) : X11Clipboard(state);
                    state.available.store(true);
                    while (!state.stop.load()) {
                        std::optional<ClipboardArchive> restore;
                        std::uint64_t epoch = 0, write_epoch = 0;
                        {
                            std::lock_guard<std::mutex> lock(state.mutex);
                            epoch = state.restore_epoch;
                            write_epoch = state.restore_write_epoch;
                            if (!state.protected_input.load() && epoch == state.epoch.load())
                                restore = std::move(state.restore);
                            state.restore.reset();
                            completed = std::move(state.restore_complete);
                        }
                        bool written = false;
                        if (restore && !state.protected_input.load() &&
                            epoch == state.epoch.load() && write_epoch == state.write_epoch.load())
                            written = backend->Restore(std::move(*restore), epoch, write_epoch);
                        if (completed) {
                            auto notify = std::move(completed);
                            notify(written && !state.protected_input.load() &&
                                   epoch == state.epoch.load() &&
                                   write_epoch == state.write_epoch.load());
                        }
                        backend->Tick();
                        pollfd descriptors[2]{{state.wake, POLLIN, 0},
                                              {backend->SocketFd(), POLLIN, 0}};
                        poll(descriptors, 2, backend->Busy() ? 10 : 1000);
                        std::uint64_t value;
                        while (read(state.wake, &value, sizeof(value)) > 0) {
                        }
                    }
                } catch (...) {
                    state.available.store(false);
                    state.epoch.fetch_add(1);
                    if (completed) {
                        auto notify = std::move(completed);
                        notify(false);
                    }
                    state.status("当前桌面未提供可用的剪贴板历史接口");
                    pollfd wait{state.wake, POLLIN, 0};
                    poll(&wait, 1, 5000);
                    std::uint64_t value;
                    while (read(state.wake, &value, sizeof(value)) > 0) {
                    }
                }
            }
            std::function<void(bool)> pending;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                pending = std::move(state.restore_complete);
            }
            if (pending)
                pending(false);
        });
    }
    ~Impl() {
        state.stop.store(true);
        Wake();
        if (worker.joinable())
            worker.join();
        close(state.wake);
    }
    void Wake() {
        std::uint64_t one = 1;
        while (write(state.wake, &one, sizeof(one)) < 0 && errno == EINTR) {
        }
    }
};
DesktopClipboard::DesktopClipboard(Capture capture, Status status)
    : impl_(std::make_unique<Impl>(std::move(capture), std::move(status))) {}
DesktopClipboard::~DesktopClipboard() = default;
void DesktopClipboard::Configure(bool capture, bool protected_input) {
    auto& state = impl_->state;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.allowed.load() != capture || state.protected_input.load() != protected_input) {
        state.protected_input.store(true);
        state.epoch.fetch_add(1);
        state.allowed.store(capture);
        state.protected_input.store(protected_input);
    }
    impl_->Wake();
}
bool DesktopClipboard::Current(std::uint64_t epoch) const { return impl_->state.Current(epoch); }
bool DesktopClipboard::Available() const { return impl_->state.available.load(); }
void DesktopClipboard::CancelWrites() {
    std::function<void(bool)> cancelled;
    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        impl_->state.write_epoch.fetch_add(1);
        impl_->state.restore.reset();
        cancelled = std::move(impl_->state.restore_complete);
    }
    if (cancelled)
        cancelled(false);
    impl_->Wake();
}
bool DesktopClipboard::Restore(ClipboardArchive archive, bool sensitive,
                               std::function<void(bool)> completed) {
    archive.Validate();
    if (!Available())
        return false;
    archive.representations.erase(
        std::remove_if(archive.representations.begin(), archive.representations.end(),
                       [](const auto& value) { return value.type == "application/x-rimes-owned"; }),
        archive.representations.end());
    archive.representations.push_back(
        {sensitive ? "application/x-rimes-sensitive" : "application/x-rimes-owned", "", 8});
    std::function<void(bool)> superseded;
    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        if (impl_->state.protected_input.load())
            return false;
        superseded = std::move(impl_->state.restore_complete);
        impl_->state.restore = std::move(archive);
        impl_->state.restore_complete = std::move(completed);
        impl_->state.restore_epoch = impl_->state.epoch.load();
        impl_->state.restore_write_epoch = impl_->state.write_epoch.load();
    }
    if (superseded)
        superseded(false);
    impl_->Wake();
    return true;
}
} // namespace rimes::capsule
