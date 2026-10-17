#include "clipboard_backend.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/xfixes.h>

namespace rimes::capsule {
namespace {
using Clock = std::chrono::steady_clock;
template <class T> using Owned = std::unique_ptr<T, decltype(&std::free)>;
class X11Backend final : public ClipboardBackend {
    xcb_connection_t* connection_ = nullptr;
    xcb_window_t window_ = 0, owner_ = 0, read_window_ = 0, root_ = 0;
    xcb_visualid_t visual_ = 0;
    bool baseline_ = false;
    xcb_atom_t clipboard_ = 0, targets_ = 0, property_ = 0, incr_ = 0, timestamp_ = 0;
    std::uint8_t xfixes_ = 0;
    std::uint64_t epoch_ = 0, capture_epoch_ = 0;
    xcb_timestamp_t time_ = XCB_CURRENT_TIME;
    bool capturing_ = false, targets_pending_ = false, incremental_ = false;
    ClipboardArchive capture_;
    std::vector<std::pair<xcb_atom_t, std::string>> types_;
    std::size_t at_ = 0, total_ = 0;
    std::string chunked_;
    int format_ = 8;
    Clock::time_point deadline_;
    std::shared_ptr<ClipboardArchive> own_;
    struct Transfer {
        xcb_window_t window;
        xcb_atom_t property, type;
        int format;
        std::shared_ptr<ClipboardArchive> archive;
        std::size_t representation = 0, sent = 0;
        Clock::time_point deadline;
    };
    std::vector<Transfer> transfers_;
    std::map<std::string, xcb_atom_t> atoms_;
    template <class T> Owned<T> Reply(unsigned int sequence) {
        const auto end = Clock::now() + std::chrono::milliseconds(500);
        void* reply = nullptr;
        xcb_generic_error_t* error = nullptr;
        while (!state_.stop.load() && Clock::now() < end) {
            if (xcb_poll_for_reply(connection_, sequence, &reply, &error)) {
                free(error);
                return Owned<T>(static_cast<T*>(reply), &std::free);
            }
            xcb_flush(connection_);
            pollfd fd{xcb_get_file_descriptor(connection_), POLLIN, 0};
            poll(&fd, 1, 5);
        }
        xcb_discard_reply(connection_, sequence);
        return Owned<T>(nullptr, &std::free);
    }
    xcb_atom_t Atom(const std::string& name) {
        if (atoms_.count(name))
            return atoms_.at(name);
        auto reply = Reply<xcb_intern_atom_reply_t>(
            xcb_intern_atom(connection_, false, name.size(), name.data()).sequence);
        if (!reply)
            throw std::runtime_error("X11 atom unavailable");
        return atoms_[name] = reply->atom;
    }
    xcb_window_t Owner() {
        auto reply = Reply<xcb_get_selection_owner_reply_t>(
            xcb_get_selection_owner(connection_, clipboard_).sequence);
        return reply ? reply->owner : 0;
    }
    void Cancel() {
        capturing_ = false;
        targets_pending_ = false;
        incremental_ = false;
        capture_ = {};
        types_.clear();
        chunked_.clear();
        if (read_window_)
            xcb_destroy_window(connection_, read_window_);
        read_window_ = 0;
    }
    bool Current() { return capturing_ && state_.Current(capture_epoch_) && Owner() == owner_; }
    void Convert(xcb_atom_t type) {
        deadline_ = Clock::now() + std::chrono::seconds(5);
        xcb_convert_selection(connection_, read_window_, clipboard_, type, property_, time_);
        xcb_flush(connection_);
    }
    void Start() {
        Cancel();
        capture_epoch_ = state_.epoch.load();
        if (baseline_ || !state_.Current(capture_epoch_) || !owner_ || owner_ == window_)
            return;
        read_window_ = xcb_generate_id(connection_);
        const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, read_window_, root_, 0, 0, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, visual_, XCB_CW_EVENT_MASK, &mask);
        capturing_ = true;
        targets_pending_ = true;
        at_ = 0;
        total_ = 0;
        Convert(targets_);
    }
    void SkipType() {
        if (targets_pending_ || !Current()) {
            Cancel();
            return;
        }
        FinishType("", 0);
    }
    void FinishType(std::string bytes, int format) {
        if (!Current()) {
            Cancel();
            return;
        }
        if (bytes.size() > ClipboardArchive::maxTotalBytes - total_) {
            bytes.clear();
            format = 0;
        }
        total_ += bytes.size();
        if (format)
            capture_.representations.push_back({types_[at_].second, std::move(bytes), format});
        incremental_ = false;
        chunked_.clear();
        if (++at_ < types_.size()) {
            if (read_window_)
                xcb_destroy_window(connection_, read_window_);
            read_window_ = xcb_generate_id(connection_);
            const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
            xcb_create_window(connection_, XCB_COPY_FROM_PARENT, read_window_, root_, 0, 0, 1, 1, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, visual_, XCB_CW_EVENT_MASK, &mask);
            Convert(types_[at_].first);
        } else {
            ClipboardSource source{"x11", "", std::to_string(owner_)};
            auto name = Reply<xcb_get_property_reply_t>(xcb_get_property(connection_, false, owner_,
                                                                         Atom("WM_CLASS"),
                                                                         XCB_ATOM_STRING, 0, 128)
                                                            .sequence);
            if (name && name->type == XCB_ATOM_STRING && name->format == 8) {
                auto* raw = static_cast<const char*>(xcb_get_property_value(name.get()));
                const auto size = xcb_get_property_value_length(name.get());
                source.application = std::string(raw, std::find(raw, raw + size, '\0'));
            }
            if (Current() && !capture_.representations.empty())
                state_.capture(std::move(capture_), std::move(source), capture_epoch_);
            Cancel();
        }
    }
    void Receive() {
        if (!Current()) {
            Cancel();
            return;
        }
        const auto bound = targets_pending_ ? 1024 : ClipboardArchive::maxTypeBytes;
        auto reply = Reply<xcb_get_property_reply_t>(
            xcb_get_property(connection_, true, read_window_, property_, XCB_GET_PROPERTY_TYPE_ANY,
                             0, (bound + 3) / 4)
                .sequence);
        if (!reply || reply->bytes_after || reply->type == XCB_ATOM_NONE) {
            SkipType();
            return;
        }
        const auto size = static_cast<std::size_t>(xcb_get_property_value_length(reply.get()));
        auto* bytes = static_cast<const char*>(xcb_get_property_value(reply.get()));
        if (targets_pending_) {
            if (reply->type != XCB_ATOM_ATOM || reply->format != 32 || size % 4 || size > 1024) {
                Cancel();
                return;
            }
            std::vector<std::string> names;
            auto* values = static_cast<xcb_atom_t*>(xcb_get_property_value(reply.get()));
            for (std::size_t i = 0; i < size / 4; ++i) {
                auto name = Reply<xcb_get_atom_name_reply_t>(
                    xcb_get_atom_name(connection_, values[i]).sequence);
                if (!name) {
                    Cancel();
                    return;
                }
                std::string text(xcb_get_atom_name_name(name.get()),
                                 xcb_get_atom_name_name_length(name.get()));
                if (text == "TARGETS" || text == "MULTIPLE" || text == "TIMESTAMP" ||
                    text == "SAVE_TARGETS")
                    continue;
                names.push_back(text);
                types_.emplace_back(values[i], std::move(text));
            }
            if (types_.empty() || types_.size() > 128 || !ClipboardArchive::AllowsTypes(names) ||
                std::find(names.begin(), names.end(), "application/x-rimes-owned") != names.end()) {
                Cancel();
                return;
            }
            targets_pending_ = false;
            Convert(types_.front().first);
            return;
        }
        if (reply->type == incr_) {
            if (incremental_ || reply->format != 32 || size != 4 ||
                *reinterpret_cast<const std::uint32_t*>(bytes) > ClipboardArchive::maxTypeBytes) {
                SkipType();
                return;
            }
            incremental_ = true;
            chunked_.clear();
            xcb_flush(connection_);
            return;
        }
        if (reply->format != 8 && reply->format != 16 && reply->format != 32) {
            SkipType();
            return;
        }
        if (incremental_) {
            if (chunked_.size() + size > ClipboardArchive::maxTypeBytes ||
                (!chunked_.empty() && format_ != reply->format)) {
                SkipType();
                return;
            }
            format_ = reply->format;
            chunked_.append(bytes, size);
            if (!size)
                FinishType(std::move(chunked_), format_);
            else {
                deadline_ = Clock::now() + std::chrono::seconds(5);
                xcb_flush(connection_);
            }
        } else
            FinishType(std::string(bytes, size), reply->format);
    }
    void SendNext(Transfer& transfer) {
        const auto& bytes = transfer.archive->representations[transfer.representation].bytes;
        const auto count = std::min<std::size_t>(65536, bytes.size() - transfer.sent);
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, transfer.window, transfer.property,
                            transfer.type, transfer.format, count / (transfer.format / 8),
                            bytes.data() + transfer.sent);
        transfer.sent += count;
        transfer.deadline = Clock::now() + std::chrono::seconds(5);
        if (!count)
            transfer.property = 0;
    }
    void Request(const xcb_selection_request_event_t& request) {
        xcb_selection_notify_event_t reply{};
        reply.response_type = XCB_SELECTION_NOTIFY;
        reply.time = request.time;
        reply.requestor = request.requestor;
        reply.selection = request.selection;
        reply.target = request.target;
        reply.property = XCB_ATOM_NONE;
        const auto property = request.property ? request.property : request.target;
        if (own_ && request.selection == clipboard_) {
            if (request.target == targets_) {
                std::vector<xcb_atom_t> types{targets_, timestamp_};
                for (const auto& value : own_->representations)
                    types.push_back(Atom(value.type));
                xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request.requestor, property,
                                    XCB_ATOM_ATOM, 32, types.size(), types.data());
                reply.property = property;
            } else if (request.target == timestamp_) {
                xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request.requestor, property,
                                    XCB_ATOM_INTEGER, 32, 1, &time_);
                reply.property = property;
            } else
                for (const auto& value : own_->representations)
                    if (Atom(value.type) == request.target) {
                        if (value.bytes.size() <= 65536) {
                            xcb_change_property(
                                connection_, XCB_PROP_MODE_REPLACE, request.requestor, property,
                                request.target, value.format,
                                value.bytes.size() / (value.format / 8), value.bytes.data());
                            reply.property = property;
                        } else if (transfers_.size() < 8) {
                            const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
                            const std::uint32_t bytes = value.bytes.size();
                            xcb_change_window_attributes(connection_, request.requestor,
                                                         XCB_CW_EVENT_MASK, &mask);
                            xcb_change_property(connection_, XCB_PROP_MODE_REPLACE,
                                                request.requestor, property, incr_, 32, 1, &bytes);
                            transfers_.push_back(
                                {request.requestor, property, request.target, value.format, own_,
                                 static_cast<std::size_t>(&value - own_->representations.data()), 0,
                                 Clock::now() + std::chrono::seconds(5)});
                            reply.property = property;
                        }
                        break;
                    }
        }
        xcb_send_event(connection_, false, request.requestor, 0,
                       reinterpret_cast<const char*>(&reply));
        xcb_flush(connection_);
    }

  public:
    explicit X11Backend(ClipboardBackendState& state) : ClipboardBackend(state) {
        try {
            connection_ = xcb_connect(nullptr, nullptr);
            if (!connection_ || xcb_connection_has_error(connection_))
                throw std::runtime_error("X11 clipboard unavailable");
            const auto* extension = xcb_get_extension_data(connection_, &xcb_xfixes_id);
            if (!extension || !extension->present)
                throw std::runtime_error("XFixes unavailable");
            xfixes_ = extension->first_event;
            auto version = Reply<xcb_xfixes_query_version_reply_t>(
                xcb_xfixes_query_version(connection_, 5, 0).sequence);
            if (!version)
                throw std::runtime_error("XFixes version unavailable");
            auto screen = xcb_setup_roots_iterator(xcb_get_setup(connection_)).data;
            root_ = screen->root;
            visual_ = screen->root_visual;
            window_ = xcb_generate_id(connection_);
            const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
            xcb_create_window(connection_, XCB_COPY_FROM_PARENT, window_, screen->root, 0, 0, 1, 1,
                              0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                              XCB_CW_EVENT_MASK, &mask);
            clipboard_ = Atom("CLIPBOARD");
            targets_ = Atom("TARGETS");
            property_ = Atom("RIMES_CLIPBOARD_TRANSFER");
            incr_ = Atom("INCR");
            timestamp_ = Atom("TIMESTAMP");
            xcb_xfixes_select_selection_input(
                connection_, window_, clipboard_,
                XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                    XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                    XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
            owner_ = Owner();
            epoch_ = state.epoch.load();
            state.status("X11 剪贴板历史可用");
            xcb_flush(connection_);
        } catch (...) {
            if (connection_)
                xcb_disconnect(connection_);
            connection_ = nullptr;
            throw;
        }
    }
    int SocketFd() const override { return xcb_get_file_descriptor(connection_); }
    bool Busy() const override { return capturing_ || !transfers_.empty(); }
    ~X11Backend() override {
        if (connection_)
            xcb_disconnect(connection_);
    }
    void Tick() override {
        if (xcb_connection_has_error(connection_))
            throw std::runtime_error("X11 disconnected");
        if (epoch_ != state_.epoch.load()) {
            epoch_ = state_.epoch.load();
            Cancel();
            owner_ = Owner();
            baseline_ = true;
        }
        if (capturing_ && !state_.Current(capture_epoch_))
            Cancel();
        else if (capturing_ && Clock::now() > deadline_)
            SkipType();
        while (auto* raw = xcb_poll_for_event(connection_)) {
            Owned<xcb_generic_event_t> event(raw, &std::free);
            const auto type = raw->response_type & 0x7f;
            if (type == xfixes_ + XCB_XFIXES_SELECTION_NOTIFY) {
                const auto* change = reinterpret_cast<xcb_xfixes_selection_notify_event_t*>(raw);
                if (change->selection == clipboard_) {
                    owner_ = change->owner;
                    time_ = change->selection_timestamp;
                    Start();
                }
            } else if (type == XCB_SELECTION_NOTIFY) {
                const auto* selection = reinterpret_cast<xcb_selection_notify_event_t*>(raw);
                if (capturing_ && selection->requestor == read_window_ &&
                    selection->selection == clipboard_ &&
                    selection->target == (targets_pending_ ? targets_ : types_[at_].first)) {
                    if (selection->property == property_)
                        Receive();
                    else
                        SkipType();
                }
            } else if (type == XCB_PROPERTY_NOTIFY) {
                const auto* property = reinterpret_cast<xcb_property_notify_event_t*>(raw);
                if (capturing_ && incremental_ && property->window == read_window_ &&
                    property->atom == property_ && property->state == XCB_PROPERTY_NEW_VALUE)
                    Receive();
                if (property->state == XCB_PROPERTY_DELETE)
                    for (auto& transfer : transfers_)
                        if (transfer.window == property->window &&
                            transfer.property == property->atom)
                            SendNext(transfer);
            } else if (type == XCB_SELECTION_REQUEST)
                Request(*reinterpret_cast<xcb_selection_request_event_t*>(raw));
            else if (type == XCB_SELECTION_CLEAR) {
                if (Owner() != window_) {
                    own_.reset();
                    transfers_.clear();
                }
            }
        }
        baseline_ = false;
        transfers_.erase(
            std::remove_if(transfers_.begin(), transfers_.end(),
                           [](const auto& t) { return !t.property || Clock::now() > t.deadline; }),
            transfers_.end());
        xcb_flush(connection_);
    }
    bool Restore(ClipboardArchive archive, std::uint64_t epoch,
                 std::uint64_t write_epoch) override {
        auto next = std::make_shared<ClipboardArchive>(std::move(archive));
        if (!state_.CommitWrite(epoch, write_epoch, [&] {
                Cancel();
                own_ = std::move(next);
                xcb_set_selection_owner(connection_, window_, clipboard_, XCB_CURRENT_TIME);
                xcb_flush(connection_);
            }))
            return false;
        if (Owner() != window_) {
            own_.reset();
            state_.status("剪贴板恢复失败，请重试");
            return false;
        }
        return true;
    }
};
} // namespace
std::unique_ptr<ClipboardBackend> X11Clipboard(ClipboardBackendState& state) {
    return std::make_unique<X11Backend>(state);
}
} // namespace rimes::capsule
