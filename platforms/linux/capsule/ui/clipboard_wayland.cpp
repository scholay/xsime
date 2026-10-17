#include "clipboard_backend.hpp"
#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>
#include <wayland-client.h>

namespace rimes::capsule {
namespace {
using Clock = std::chrono::steady_clock;
// The promoted protocol preserved these wire operations; generated interfaces
// still own all signatures and versions. Do not substitute an ext object for
// a wlr object: each connection binds and creates one consistent family.
static_assert(EXT_DATA_CONTROL_MANAGER_V1_CREATE_DATA_SOURCE ==
              ZWLR_DATA_CONTROL_MANAGER_V1_CREATE_DATA_SOURCE);
static_assert(EXT_DATA_CONTROL_MANAGER_V1_GET_DATA_DEVICE ==
              ZWLR_DATA_CONTROL_MANAGER_V1_GET_DATA_DEVICE);
static_assert(EXT_DATA_CONTROL_DEVICE_V1_SET_SELECTION ==
              ZWLR_DATA_CONTROL_DEVICE_V1_SET_SELECTION);
static_assert(EXT_DATA_CONTROL_OFFER_V1_RECEIVE == ZWLR_DATA_CONTROL_OFFER_V1_RECEIVE);
struct Interfaces {
    const wl_interface *manager, *device, *offer, *source;
};
class WaylandBackend final : public ClipboardBackend {
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_proxy *manager_ = nullptr, *device_ = nullptr, *current_ = nullptr, *source_ = nullptr;
    Interfaces interfaces_{};
    struct Global {
        std::uint32_t name, version;
    };
    std::map<std::string, Global> globals_;
    std::map<wl_proxy*, std::vector<std::string>> offers_;
    std::uint64_t epoch_ = 0, capture_epoch_ = 0;
    bool first_selection_ = true, baseline_ = true, healthy_ = true;
    ClipboardArchive capture_;
    int receive_ = -1;
    std::size_t type_ = 0, total_ = 0;
    Clock::time_point deadline_;
    std::shared_ptr<ClipboardArchive> owned_;
    struct Transfer {
        int fd;
        std::shared_ptr<ClipboardArchive> archive;
        std::size_t type, sent = 0;
        Clock::time_point deadline;
    };
    std::vector<Transfer> transfers_;
    void CancelRead() {
        if (receive_ >= 0)
            close(receive_);
        receive_ = -1;
        capture_ = {};
        type_ = 0;
        total_ = 0;
    }
    void SkipType() {
        if (receive_ >= 0)
            close(receive_);
        receive_ = -1;
        if (!capture_.representations.empty()) {
            total_ -= capture_.representations.back().bytes.size();
            capture_.representations.pop_back();
        }
        ++type_;
        ReadNext();
    }
    static void Destroy(wl_proxy*& value, std::uint32_t operation) {
        if (value)
            wl_proxy_marshal_flags(value, operation, nullptr, wl_proxy_get_version(value),
                                   WL_MARSHAL_FLAG_DESTROY);
        value = nullptr;
    }
    void DropOffer(wl_proxy* offer) {
        if (!offer)
            return;
        offers_.erase(offer);
        Destroy(offer, EXT_DATA_CONTROL_OFFER_V1_DESTROY);
    }
    bool Current() const {
        return state_.Current(capture_epoch_) && current_ && offers_.count(current_);
    }
    void ReadNext() {
        if (!Current()) {
            CancelRead();
            return;
        }
        const auto& types = offers_.at(current_);
        if (type_ >= types.size()) {
            auto archive = std::move(capture_);
            if (Current() && !archive.representations.empty())
                state_.capture(std::move(archive), {"wayland", "", "primary-seat"}, capture_epoch_);
            CancelRead();
            return;
        }
        int pipe[2];
        if (pipe2(pipe, O_CLOEXEC)) {
            CancelRead();
            return;
        }
        fcntl(pipe[0], F_SETFL, fcntl(pipe[0], F_GETFL) | O_NONBLOCK);
        receive_ = pipe[0];
        capture_.representations.push_back({types[type_], "", 8});
        wl_proxy_marshal_flags(current_, EXT_DATA_CONTROL_OFFER_V1_RECEIVE, nullptr,
                               wl_proxy_get_version(current_), 0, types[type_].c_str(), pipe[1]);
        close(pipe[1]);
        deadline_ = Clock::now() + std::chrono::seconds(5);
    }
    void Selected(wl_proxy* offer) {
        CancelRead();
        if (current_ && current_ != offer)
            DropOffer(current_);
        current_ = offer;
        if (first_selection_ || baseline_) {
            first_selection_ = false;
            return;
        }
        capture_epoch_ = state_.epoch.load();
        if (!Current())
            return;
        const auto& types = offers_.at(offer);
        if (types.empty() || types.size() > 128 || !ClipboardArchive::AllowsTypes(types) ||
            std::find(types.begin(), types.end(), "application/x-rimes-owned") != types.end())
            return;
        ReadNext();
    }
    static void Offered(void* raw, wl_proxy* offer, const char* type) {
        auto* self = static_cast<WaylandBackend*>(raw);
        auto& values = self->offers_.at(offer);
        if (values.size() < 129)
            values.emplace_back(type);
    }
    static void DataOffer(void* raw, wl_proxy*, wl_proxy* offer) {
        auto* self = static_cast<WaylandBackend*>(raw);
        self->offers_[offer] = {};
        static void (*listeners[])(void) = {reinterpret_cast<void (*)(void)>(&Offered)};
        wl_proxy_add_listener(offer, listeners, self);
    }
    static void Selection(void* raw, wl_proxy*, wl_proxy* offer) {
        static_cast<WaylandBackend*>(raw)->Selected(offer);
    }
    static void Finished(void* raw, wl_proxy*) {
        auto* self = static_cast<WaylandBackend*>(raw);
        self->CancelRead();
        self->state_.status("桌面已撤销剪贴板访问，请重新打开 Capsule");
        self->healthy_ = false;
        self->state_.available.store(false);
        self->state_.epoch.fetch_add(1);
    }
    static void Primary(void* raw, wl_proxy*, wl_proxy* offer) {
        auto* self = static_cast<WaylandBackend*>(raw);
        if (offer != self->current_)
            self->DropOffer(offer);
    }
    static void Send(void* raw, wl_proxy*, const char* type, int fd) {
        auto* self = static_cast<WaylandBackend*>(raw);
        if (!self->owned_ || self->transfers_.size() >= 8) {
            close(fd);
            return;
        }
        for (std::size_t index = 0; index < self->owned_->representations.size(); ++index)
            if (self->owned_->representations[index].type == type) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                self->transfers_.push_back(
                    {fd, self->owned_, index, 0, Clock::now() + std::chrono::seconds(5)});
                return;
            }
        close(fd);
    }
    static void Cancelled(void* raw, wl_proxy* source) {
        auto* self = static_cast<WaylandBackend*>(raw);
        if (source == self->source_) {
            self->owned_.reset();
            Destroy(self->source_, EXT_DATA_CONTROL_SOURCE_V1_DESTROY);
        }
    }
    void Dispatch() {
        if (wl_display_dispatch_pending(display_) < 0)
            throw std::runtime_error("Wayland clipboard disconnected");
        while (wl_display_prepare_read(display_) != 0)
            if (wl_display_dispatch_pending(display_) < 0)
                throw std::runtime_error("Wayland clipboard disconnected");
        const int flushed = wl_display_flush(display_);
        if (flushed < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display_);
            throw std::runtime_error("Wayland clipboard disconnected");
        }
        pollfd descriptor{wl_display_get_fd(display_), POLLIN, 0};
        if (poll(&descriptor, 1, 0) > 0 && (descriptor.revents & POLLIN)) {
            if (wl_display_read_events(display_) < 0)
                throw std::runtime_error("Wayland clipboard disconnected");
            if (wl_display_dispatch_pending(display_) < 0)
                throw std::runtime_error("Wayland clipboard disconnected");
        } else
            wl_display_cancel_read(display_);
    }
    void Roundtrip() {
        bool done = false;
        auto* callback = wl_display_sync(display_);
        static const wl_callback_listener listener{
            +[](void* data, wl_callback* callback, std::uint32_t) {
                *static_cast<bool*>(data) = true;
                wl_callback_destroy(callback);
            }};
        wl_callback_add_listener(callback, &listener, &done);
        const auto end = Clock::now() + std::chrono::seconds(2);
        while (!done && !state_.stop.load() && Clock::now() < end) {
            Dispatch();
            pollfd fd{wl_display_get_fd(display_), POLLIN, 0};
            poll(&fd, 1, 5);
        }
        if (!done) {
            wl_callback_destroy(callback);
            throw std::runtime_error("Wayland handshake timeout");
        }
    }

  public:
    explicit WaylandBackend(ClipboardBackendState& state) : ClipboardBackend(state) {
        try {
            display_ = wl_display_connect(nullptr);
            if (!display_)
                throw std::runtime_error("Wayland unavailable");
            registry_ = wl_display_get_registry(display_);
            static const wl_registry_listener listener{
                +[](void* raw, wl_registry*, std::uint32_t name, const char* interface,
                    std::uint32_t version) {
                    auto* self = static_cast<WaylandBackend*>(raw);
                    if (!self->globals_.count(interface))
                        self->globals_[interface] = {name, version};
                },
                +[](void* raw, wl_registry*, std::uint32_t name) {
                    auto* self = static_cast<WaylandBackend*>(raw);
                    for (const auto& [interface, global] : self->globals_)
                        if (global.name == name &&
                            (interface == "wl_seat" || interface == "ext_data_control_manager_v1" ||
                             interface == "zwlr_data_control_manager_v1")) {
                            self->CancelRead();
                            self->healthy_ = false;
                            self->state_.available.store(false);
                            self->state_.epoch.fetch_add(1);
                        }
                }};
            wl_registry_add_listener(registry_, &listener, this);
            Roundtrip();
            bool ext = globals_.count("ext_data_control_manager_v1");
            const std::string family =
                ext ? "ext_data_control_manager_v1" : "zwlr_data_control_manager_v1";
            if (!globals_.count(family) || !globals_.count("wl_seat"))
                throw std::runtime_error("Desktop does not expose data control");
            interfaces_ = ext ? Interfaces{&ext_data_control_manager_v1_interface,
                                           &ext_data_control_device_v1_interface,
                                           &ext_data_control_offer_v1_interface,
                                           &ext_data_control_source_v1_interface}
                              : Interfaces{&zwlr_data_control_manager_v1_interface,
                                           &zwlr_data_control_device_v1_interface,
                                           &zwlr_data_control_offer_v1_interface,
                                           &zwlr_data_control_source_v1_interface};
            manager_ = static_cast<wl_proxy*>(
                wl_registry_bind(registry_, globals_.at(family).name, interfaces_.manager,
                                 ext ? 1 : std::min(2u, globals_.at(family).version)));
            seat_ = static_cast<wl_seat*>(
                wl_registry_bind(registry_, globals_.at("wl_seat").name, &wl_seat_interface, 1));
            static const wl_seat_listener seat_listener{+[](void*, wl_seat*, std::uint32_t) {},
                                                        +[](void*, wl_seat*, const char*) {}};
            wl_seat_add_listener(seat_, &seat_listener, this);
            device_ = wl_proxy_marshal_flags(manager_, EXT_DATA_CONTROL_MANAGER_V1_GET_DATA_DEVICE,
                                             interfaces_.device, wl_proxy_get_version(manager_), 0,
                                             nullptr, seat_);
            static void (*device_listener[])(void) = {reinterpret_cast<void (*)(void)>(&DataOffer),
                                                      reinterpret_cast<void (*)(void)>(&Selection),
                                                      reinterpret_cast<void (*)(void)>(&Finished),
                                                      reinterpret_cast<void (*)(void)>(&Primary)};
            wl_proxy_add_listener(device_, device_listener, this);
            Roundtrip();
            if (!healthy_)
                throw std::runtime_error("Wayland clipboard access revoked during setup");
            epoch_ = state_.epoch.load();
            baseline_ = false;
            state.status(ext ? "Wayland ext-data-control 剪贴板历史可用"
                             : "Wayland wlr-data-control 剪贴板历史可用");
        } catch (...) {
            Cleanup();
            throw;
        }
    }
    ~WaylandBackend() override { Cleanup(); }
    int SocketFd() const override { return wl_display_get_fd(display_); }
    bool Busy() const override { return receive_ >= 0 || !transfers_.empty(); }
    void Cleanup() {
        CancelRead();
        for (auto& transfer : transfers_)
            close(transfer.fd);
        for (const auto& [offer, types] : offers_) {
            (void)types;
            auto* item = offer;
            Destroy(item, EXT_DATA_CONTROL_OFFER_V1_DESTROY);
        }
        Destroy(source_, EXT_DATA_CONTROL_SOURCE_V1_DESTROY);
        Destroy(device_, EXT_DATA_CONTROL_DEVICE_V1_DESTROY);
        Destroy(manager_, EXT_DATA_CONTROL_MANAGER_V1_DESTROY);
        if (seat_)
            wl_seat_destroy(seat_);
        if (registry_)
            wl_registry_destroy(registry_);
        if (display_)
            wl_display_disconnect(display_);
    }
    void Tick() override {
        if (epoch_ != state_.epoch.load()) {
            epoch_ = state_.epoch.load();
            CancelRead();
            baseline_ = true;
            Roundtrip();
            baseline_ = false;
        }
        Dispatch();
        if (!healthy_)
            throw std::runtime_error("Wayland clipboard access revoked");
        if (receive_ >= 0 && !Current())
            CancelRead();
        else if (receive_ >= 0 && Clock::now() > deadline_)
            SkipType();
        if (receive_ >= 0) {
            std::array<char, 65536> bytes{};
            std::size_t budget = 256 * 1024;
            while (receive_ >= 0 && budget) {
                const auto count = read(receive_, bytes.data(), std::min(bytes.size(), budget));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                if (count < 0) {
                    SkipType();
                    break;
                }
                if (!count) {
                    // Wayland has no separate failure signal. An empty pipe
                    // contributes no useful representation, but must not erase
                    // the other MIME types that were already read successfully.
                    if (capture_.representations.back().bytes.empty()) {
                        SkipType();
                        break;
                    }
                    close(receive_);
                    receive_ = -1;
                    ++type_;
                    ReadNext();
                    break;
                }
                auto& value = capture_.representations.back().bytes;
                if (value.size() + count > ClipboardArchive::maxTypeBytes ||
                    total_ + count > ClipboardArchive::maxTotalBytes) {
                    SkipType();
                    break;
                }
                value.append(bytes.data(), count);
                total_ += count;
                budget -= count;
            }
        }
        for (auto& transfer : transfers_) {
            const auto& bytes = transfer.archive->representations[transfer.type].bytes;
            if (Clock::now() > transfer.deadline) {
                close(transfer.fd);
                transfer.fd = -1;
                continue;
            }
            const auto size = std::min<std::size_t>(256 * 1024, bytes.size() - transfer.sent);
            if (!size) {
                close(transfer.fd);
                transfer.fd = -1;
                continue;
            }
            const auto count = write(transfer.fd, bytes.data() + transfer.sent, size);
            if (count > 0)
                transfer.sent += count;
            else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(transfer.fd);
                transfer.fd = -1;
            }
        }
        transfers_.erase(std::remove_if(transfers_.begin(), transfers_.end(),
                                        [](const auto& value) { return value.fd < 0; }),
                         transfers_.end());
    }
    bool Restore(ClipboardArchive archive, std::uint64_t epoch,
                 std::uint64_t write_epoch) override {
        auto next = std::make_shared<ClipboardArchive>(std::move(archive));
        auto* source =
            wl_proxy_marshal_flags(manager_, EXT_DATA_CONTROL_MANAGER_V1_CREATE_DATA_SOURCE,
                                   interfaces_.source, wl_proxy_get_version(manager_), 0, nullptr);
        static void (*source_listener[])(void) = {reinterpret_cast<void (*)(void)>(&Send),
                                                  reinterpret_cast<void (*)(void)>(&Cancelled)};
        wl_proxy_add_listener(source, source_listener, this);
        for (const auto& value : next->representations)
            wl_proxy_marshal_flags(source, EXT_DATA_CONTROL_SOURCE_V1_OFFER, nullptr,
                                   wl_proxy_get_version(source), 0, value.type.c_str());
        if (!state_.CommitWrite(epoch, write_epoch, [&] {
                CancelRead();
                Destroy(source_, EXT_DATA_CONTROL_SOURCE_V1_DESTROY);
                owned_ = std::move(next);
                source_ = source;
                wl_proxy_marshal_flags(device_, EXT_DATA_CONTROL_DEVICE_V1_SET_SELECTION, nullptr,
                                       wl_proxy_get_version(device_), 0, source_);
                wl_display_flush(display_);
            })) {
            Destroy(source, EXT_DATA_CONTROL_SOURCE_V1_DESTROY);
            return false;
        }
        Roundtrip();
        return healthy_ && source_;
    }
};
} // namespace
std::unique_ptr<ClipboardBackend> WaylandClipboard(ClipboardBackendState& state) {
    return std::make_unique<WaylandBackend>(state);
}
} // namespace rimes::capsule
