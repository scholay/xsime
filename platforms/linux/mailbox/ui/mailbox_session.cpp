#include "mailbox_session.hpp"
#include "session/session_monitor.hpp"
#include <atomic>
#include <fcitx-utils/event.h>
#include <gio/gio.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
namespace rimes::mailbox {
struct DesktopSession::Impl {
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    int stop = -1;
    std::thread worker;
    Impl(std::function<void(bool)> changed) {
        stop = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (stop < 0)
            throw std::runtime_error("无法观察桌面会话");
        auto notify = [life = alive, changed](bool value) {
            auto callback = new std::function<void()>([life, changed, value] {
                if (life->load())
                    changed(value);
            });
            g_idle_add_full(
                G_PRIORITY_HIGH,
                +[](gpointer p) -> gboolean {
                    (*static_cast<std::function<void()>*>(p))();
                    return G_SOURCE_REMOVE;
                },
                callback, +[](gpointer p) { delete static_cast<std::function<void()>*>(p); });
        };
        try {
            worker = std::thread([this, notify] {
                try {
                    fcitx::EventLoop loop;
                    auto stopping =
                        loop.addIOEvent(stop, fcitx::IOEventFlag::In,
                                        [&](fcitx::EventSourceIO*, int, fcitx::IOEventFlags) {
                                            loop.exit();
                                            return true;
                                        });
                    fcitx::SessionMonitor monitor(loop, notify);
                    loop.exec();
                } catch (...) {
                    notify(true);
                }
            });
        } catch (...) {
            close(stop);
            throw;
        }
    }
    ~Impl() {
        alive->store(false);
        std::uint64_t signal = 1;
        const auto written = write(stop, &signal, sizeof(signal));
        (void)written;
        if (worker.joinable())
            worker.join();
        close(stop);
    }
};
DesktopSession::DesktopSession(std::function<void(bool)> changed)
    : impl_(std::make_unique<Impl>(std::move(changed))) {}
DesktopSession::~DesktopSession() = default;
} // namespace rimes::mailbox
