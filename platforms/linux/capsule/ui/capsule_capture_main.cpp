// Capture runs outside both the IME and GTK process. stdin owns the lifetime;
// EOF/cancel closes the portal and discards the private unfinished file.
#include "capsule_resources.hpp"
#include "private_files.hpp"
#include <X11/Xlib.h>
#include <algorithm>
#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <gst/gst.h>
#include <iostream>
#include <libportal/portal.h>
#include <libportal/screenshot.h>
#include <nlohmann/json.hpp>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
using Json = nlohmann::json;
namespace {
struct Capture {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    GCancellable* cancellable = g_cancellable_new();
    XdpPortal* portal = nullptr;
    GDBusConnection* bus = nullptr;
    GDBusConnection* protection_bus = nullptr;
    std::vector<guint> protection_signals;
    std::string request_path, session_path, request_stage;
    guint response_signal = 0, closed_signal = 0, portal_version = 0, source_types = 0;
    guint stream_node = 0;
    guint64 stream_serial = 0;
    static constexpr auto service = "org.freedesktop.portal.Desktop";
    static constexpr auto desktop_path = "/org/freedesktop/portal/desktop";
    static constexpr auto screencast = "org.freedesktop.portal.ScreenCast";
    GstElement* pipeline = nullptr;
    GstElement* source = nullptr;
    int output_fd = -1, remote_fd = -1, width = 0, height = 0;
    std::filesystem::path output;
    std::string mode, backend, commands;
    bool cancelled = false, finished = false, started = false, stopping = false, success = false;
    gint64 began = 0, requested = g_get_monotonic_time(), stop_at = 0;
    ~Capture() {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }
        if (bus) {
            if (response_signal)
                g_dbus_connection_signal_unsubscribe(bus, response_signal);
            if (closed_signal)
                g_dbus_connection_signal_unsubscribe(bus, closed_signal);
            // Closing pending requests and the session is independent of the
            // cancelled operation. Wait for the reply before releasing our bus.
            for (const auto& item :
                 {std::make_pair(request_path, "org.freedesktop.portal.Request"),
                  std::make_pair(session_path, "org.freedesktop.portal.Session")}) {
                if (item.first.empty())
                    continue;
                auto* reply = g_dbus_connection_call_sync(
                    bus, service, item.first.c_str(), item.second, "Close", nullptr, nullptr,
                    G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
                if (reply)
                    g_variant_unref(reply);
            }
            g_object_unref(bus);
        }
        if (protection_bus) {
            for (auto id : protection_signals)
                g_dbus_connection_signal_unsubscribe(protection_bus, id);
            g_object_unref(protection_bus);
        }
        if (portal)
            g_object_unref(portal);
        g_object_unref(cancellable);
        g_main_loop_unref(loop);
        if (output_fd >= 0)
            close(output_fd);
        if (remote_fd >= 0)
            close(remote_fd);
        if (!success && output_fd >= 0) {
            std::error_code ignored;
            std::filesystem::remove(output, ignored);
        }
    }
    void Emit(const Json& j) { std::cout << j.dump() << '\n' << std::flush; }
    void Fail(const char* message) {
        if (finished)
            return;
        finished = true;
        g_cancellable_cancel(cancellable);
        Emit({{"event", cancelled ? "cancelled" : "error"}, {"message", message}});
        g_main_loop_quit(loop);
    }
    void Cancel() {
        cancelled = true;
        Fail("捕获已取消");
    }
    void Stop() {
        if (finished || stopping)
            return;
        if (!started || mode != "video") {
            Cancel();
            return;
        }
        stopping = true;
        stop_at = g_get_monotonic_time();
        if (!gst_element_send_event(pipeline, gst_event_new_eos()))
            Fail("录屏收尾失败");
    }
    void Dimensions() {
        auto* pad = source ? gst_element_get_static_pad(source, "src") : nullptr;
        auto* caps = pad ? gst_pad_get_current_caps(pad) : nullptr;
        if (caps && !gst_caps_is_empty(caps)) {
            const auto* structure = gst_caps_get_structure(caps, 0);
            gst_structure_get_int(structure, "width", &width);
            gst_structure_get_int(structure, "height", &height);
        }
        if (caps)
            gst_caps_unref(caps);
        if (pad)
            gst_object_unref(pad);
    }
    void Finish() {
        if (finished || cancelled)
            return;
        Dimensions();
        if (pipeline)
            gst_element_set_state(pipeline, GST_STATE_NULL);
        struct stat info {};
        if (output_fd < 0 || fstat(output_fd, &info) || info.st_size <= 0 || fsync(output_fd) ||
            width <= 0 || height <= 0) {
            Fail("捕获没有可用画面");
            return;
        }
        finished = success = true;
        Emit({{"event", "finished"},
              {"width", width},
              {"height", height},
              {"duration",
               mode == "video" && began > 0 ? (g_get_monotonic_time() - began) / 1000000.0 : 0},
              {"source", backend == "portal" ? "桌面授权捕获" : "X11 屏幕捕获"},
              {"kind", mode}});
        g_main_loop_quit(loop);
    }
    GstElement* Add(const char* factory) {
        auto* element = gst_element_factory_make(factory, nullptr);
        if (!element)
            throw std::runtime_error(std::string("缺少 GStreamer 组件：") + factory);
        gst_bin_add(GST_BIN(pipeline), element);
        return element;
    }
    void StartPipeline(std::uint32_t node = 0, std::uint64_t serial = 0) {
        if (finished)
            return;
        try {
            pipeline = gst_pipeline_new(nullptr);
            source = Add(backend == "portal" ? "pipewiresrc" : "ximagesrc");
            if (backend == "portal") {
                g_object_set(source, "fd", remote_fd, "do-timestamp", TRUE, nullptr);
                if (serial &&
                    g_object_class_find_property(G_OBJECT_GET_CLASS(source), "target-object"))
                    g_object_set(source, "target-object", std::to_string(serial).c_str(), nullptr);
                else
                    g_object_set(source, "path", std::to_string(node).c_str(), nullptr);
            } else
                g_object_set(source, "use-damage", FALSE, "show-pointer", TRUE, nullptr);
            auto* convert = Add("videoconvert");
            auto* filter = Add("capsfilter");
            // A stable software format avoids importing compositor GPU buffers
            // into the encoder and limits the frame rate before compression.
            auto* caps =
                gst_caps_from_string(mode == "video" ? "video/x-raw,format=I420,framerate=30/1"
                                                     : "video/x-raw,format=RGB");
            g_object_set(filter, "caps", caps, nullptr);
            gst_caps_unref(caps);
            auto* sink = Add("fdsink");
            g_object_set(sink, "fd", output_fd, "sync", FALSE, nullptr);
            bool linked = false;
            if (mode == "video") {
                auto* rate = Add("videorate");
                g_object_set(rate, "drop-only", TRUE, nullptr);
                auto* encoder = Add("vp8enc");
                g_object_set(encoder, "deadline", gint64(1), "threads", 2u, "target-bitrate",
                             4000000, nullptr);
                auto* mux = Add("webmmux");
                linked = gst_element_link_many(source, convert, rate, filter, encoder, mux, sink,
                                               nullptr);
            } else {
                g_object_set(source, "num-buffers", 1, nullptr);
                auto* encoder = Add("pngenc");
                linked = gst_element_link_many(source, convert, filter, encoder, sink, nullptr);
            }
            if (!linked)
                throw std::runtime_error("无法建立捕获管线");
            auto* bus = gst_element_get_bus(pipeline);
            gst_bus_add_watch(
                bus,
                +[](GstBus*, GstMessage* message, gpointer data) -> gboolean {
                    auto* self = static_cast<Capture*>(data);
                    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
                        self->Fail("捕获设备或编码器失败，请检查桌面支持与权限");
                    else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS)
                        self->Finish();
                    else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STATE_CHANGED &&
                             GST_MESSAGE_SRC(message) == GST_OBJECT(self->pipeline)) {
                        GstState old, current, pending;
                        gst_message_parse_state_changed(message, &old, &current, &pending);
                        if (current == GST_STATE_PLAYING && !self->started) {
                            self->started = true;
                            self->began = g_get_monotonic_time();
                            self->Dimensions();
                            self->Emit({{"event", "started"}, {"kind", self->mode}});
                        }
                    }
                    return G_SOURCE_CONTINUE;
                },
                this);
            gst_object_unref(bus);
            if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
                Fail("桌面捕获不可用");
        } catch (const std::exception& e) {
            Fail(e.what());
        }
    }
    void WatchScreenLock() {
        if (!g_getenv("DBUS_SESSION_BUS_ADDRESS"))
            return;
        protection_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellable, nullptr);
        if (!protection_bus)
            return;
        for (const auto* name : {"org.freedesktop.ScreenSaver", "org.gnome.ScreenSaver"}) {
            protection_signals.push_back(g_dbus_connection_signal_subscribe(
                protection_bus, name, name, "ActiveChanged", nullptr, nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE,
                +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                    GVariant* value, gpointer raw) {
                    gboolean active = FALSE;
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE("(b)"))) {
                        g_variant_get(value, "(b)", &active);
                        if (active)
                            static_cast<Capture*>(raw)->Cancel();
                    }
                },
                this, nullptr));
        }
    }
    void PortalScreenshot() {
        xdp_portal_take_screenshot(
            portal, nullptr, XDP_SCREENSHOT_FLAG_INTERACTIVE, cancellable,
            +[](GObject* object, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Capture*>(data);
                GError* error = nullptr;
                auto* uri = xdp_portal_take_screenshot_finish(XDP_PORTAL(object), result, &error);
                if (self->finished) {
                    g_free(uri);
                    g_clear_error(&error);
                    return;
                }
                if (!uri) {
                    self->Fail("截图未获授权、已取消，或桌面未提供截图接口");
                    g_clear_error(&error);
                    return;
                }
                auto* path = g_filename_from_uri(uri, nullptr, nullptr);
                g_free(uri);
                try {
                    if (!path)
                        throw std::runtime_error("桌面返回了非本机截图");
                    std::atomic<bool> cancelled{false};
                    const auto bytes =
                        rimes::capsule::ReadResource(path, 32 * 1024 * 1024, cancelled);
                    auto* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
                    const bool wrote =
                        loader && gdk_pixbuf_loader_write(
                                      loader, reinterpret_cast<const guchar*>(bytes.data()),
                                      bytes.size(), nullptr);
                    const bool closed = loader && gdk_pixbuf_loader_close(loader, nullptr);
                    auto* image = wrote && closed ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
                    if (!image) {
                        if (loader)
                            g_object_unref(loader);
                        throw std::runtime_error("桌面截图不是有效 PNG");
                    }
                    self->width = gdk_pixbuf_get_width(image);
                    self->height = gdk_pixbuf_get_height(image);
                    g_object_unref(loader);
                    if (static_cast<std::uint64_t>(self->width) * self->height > 128000000)
                        throw std::runtime_error("截图像素超过上限");
                    for (std::size_t offset = 0; offset < bytes.size();) {
                        const auto count =
                            write(self->output_fd, bytes.data() + offset, bytes.size() - offset);
                        if (count < 0 && errno == EINTR)
                            continue;
                        if (count <= 0)
                            throw std::runtime_error("截图保存失败");
                        offset += count;
                    }
                    self->Finish();
                } catch (const std::exception& e) {
                    self->Fail(e.what());
                }
                g_free(path);
            },
            this);
    }
    std::string Token() {
        auto* uuid = g_uuid_string_random();
        std::string value = uuid;
        g_free(uuid);
        value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
        return "rimes_" + value;
    }
    void Request(const char* stage) {
        if (finished)
            return;
        request_stage = stage;
        const auto token = Token();
        std::string owner = g_dbus_connection_get_unique_name(bus) + 1;
        std::replace(owner.begin(), owner.end(), '.', '_');
        request_path = std::string(desktop_path) + "/request/" + owner + "/" + token;
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token",
                              g_variant_new_string(token.c_str()));
        if (request_stage == "CreateSession")
            g_variant_builder_add(&options, "{sv}", "session_handle_token",
                                  g_variant_new_string(Token().c_str()));
        if (request_stage == "SelectSources") {
            g_variant_builder_add(&options, "{sv}", "types", g_variant_new_uint32(source_types));
            g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(FALSE));
            if (portal_version >= 4)
                g_variant_builder_add(&options, "{sv}", "persist_mode", g_variant_new_uint32(0));
            // Omit cursor_mode: the portal's default is hidden, including v1.
        }
        GVariant* arguments = request_stage == "CreateSession" ? g_variant_new("(a{sv})", &options)
                              : request_stage == "SelectSources"
                                  ? g_variant_new("(oa{sv})", session_path.c_str(), &options)
                                  : g_variant_new("(osa{sv})", session_path.c_str(), "", &options);
        if (response_signal)
            g_dbus_connection_signal_unsubscribe(bus, response_signal);
        // Subscribe before calling: portals may emit Response before replying
        // with the request handle. Responses may also be unicast to our owner.
        response_signal = g_dbus_connection_signal_subscribe(
            bus, service, "org.freedesktop.portal.Request", "Response", request_path.c_str(),
            nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                GVariant* parameters, gpointer raw) {
                auto* self = static_cast<Capture*>(raw);
                if (self->finished)
                    return;
                guint code = 2;
                GVariant* results = nullptr;
                g_variant_get(parameters, "(u@a{sv})", &code, &results);
                self->request_path.clear();
                if (code) {
                    self->cancelled = code == 1;
                    self->Fail("录屏已取消或未获授权");
                } else
                    self->Response(results);
                g_variant_unref(results);
            },
            this, nullptr);
        auto* call = new std::pair<Capture*, std::string>(this, request_path);
        g_dbus_connection_call(
            bus, service, desktop_path, screencast, stage, arguments, G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE, 120000, cancellable,
            +[](GObject* object, GAsyncResult* result, gpointer raw) {
                auto call = std::unique_ptr<std::pair<Capture*, std::string>>(
                    static_cast<std::pair<Capture*, std::string>*>(raw));
                GError* error = nullptr;
                auto* reply =
                    g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
                if (!reply && !call->first->finished && call->first->request_path == call->second)
                    call->first->Fail("桌面未提供可用的录屏接口");
                if (reply)
                    g_variant_unref(reply);
                g_clear_error(&error);
            },
            call);
    }
    void Response(GVariant* results) {
        if (request_stage == "CreateSession") {
            const char* handle = nullptr;
            if (!g_variant_lookup(results, "session_handle", "&s", &handle) ||
                !g_variant_is_object_path(handle)) {
                Fail("桌面返回了无效的录屏会话");
                return;
            }
            session_path = handle;
            closed_signal = g_dbus_connection_signal_subscribe(
                bus, service, "org.freedesktop.portal.Session", "Closed", session_path.c_str(),
                nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                    GVariant*, gpointer raw) { static_cast<Capture*>(raw)->Cancel(); },
                this, nullptr);
            Request("SelectSources");
        } else if (request_stage == "SelectSources")
            Request("Start");
        else {
            auto* streams = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
            if (!streams || g_variant_n_children(streams) != 1) {
                if (streams)
                    g_variant_unref(streams);
                Fail("桌面未提供单个可录制画面");
                return;
            }
            auto* stream = g_variant_get_child_value(streams, 0);
            GVariant* props = nullptr;
            g_variant_get(stream, "(u@a{sv})", &stream_node, &props);
            g_variant_lookup(props, "pipewire-serial", "t", &stream_serial);
            g_variant_unref(props);
            g_variant_unref(stream);
            g_variant_unref(streams);
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
            g_dbus_connection_call_with_unix_fd_list(
                bus, service, desktop_path, screencast, "OpenPipeWireRemote",
                g_variant_new("(oa{sv})", session_path.c_str(), &options), G_VARIANT_TYPE("(h)"),
                G_DBUS_CALL_FLAGS_NONE, 10000, nullptr, cancellable,
                +[](GObject* object, GAsyncResult* result, gpointer raw) {
                    auto* self = static_cast<Capture*>(raw);
                    GUnixFDList* descriptors = nullptr;
                    GError* error = nullptr;
                    auto* reply = g_dbus_connection_call_with_unix_fd_list_finish(
                        G_DBUS_CONNECTION(object), &descriptors, result, &error);
                    gint index = -1;
                    if (reply && descriptors && !self->finished) {
                        g_variant_get(reply, "(h)", &index);
                        self->remote_fd = g_unix_fd_list_get(descriptors, index, nullptr);
                    }
                    if (reply)
                        g_variant_unref(reply);
                    if (descriptors)
                        g_object_unref(descriptors);
                    g_clear_error(&error);
                    if (self->finished)
                        return;
                    if (self->remote_fd < 0)
                        self->Fail("无法连接授权的 PipeWire 画面");
                    else
                        self->StartPipeline(self->stream_node, self->stream_serial);
                },
                this);
        }
    }
    void PortalVideo() {
        g_bus_get(
            G_BUS_TYPE_SESSION, cancellable,
            +[](GObject*, GAsyncResult* result, gpointer raw) {
                auto* self = static_cast<Capture*>(raw);
                self->bus = g_bus_get_finish(result, nullptr);
                if (self->finished)
                    return;
                if (!self->bus) {
                    self->Fail("无法连接桌面会话");
                    return;
                }
                // Query only ScreenCast. RemoteDesktop is not a dependency and
                // is deliberately absent on screen-only portal backends.
                g_dbus_connection_call(
                    self->bus, service, desktop_path, "org.freedesktop.DBus.Properties", "GetAll",
                    g_variant_new("(s)", screencast), G_VARIANT_TYPE("(a{sv})"),
                    G_DBUS_CALL_FLAGS_NONE, 10000, self->cancellable,
                    +[](GObject* object, GAsyncResult* result, gpointer raw) {
                        auto* self = static_cast<Capture*>(raw);
                        auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object),
                                                                    result, nullptr);
                        if (reply) {
                            auto* props = g_variant_get_child_value(reply, 0);
                            g_variant_lookup(props, "version", "u", &self->portal_version);
                            g_variant_lookup(props, "AvailableSourceTypes", "u",
                                             &self->source_types);
                            self->source_types &= 3;
                            g_variant_unref(props);
                            g_variant_unref(reply);
                        }
                        if (self->finished)
                            return;
                        if (!self->portal_version || !self->source_types)
                            self->Fail("桌面未提供可录制的屏幕或窗口");
                        else
                            self->Request("CreateSession");
                    },
                    self);
            },
            this);
    }
};
} // namespace
int main(int argc, char** argv) {
    if (argc != 5 || std::string(argv[1]) != "--explicit" ||
        (std::string(argv[2]) != "image" && std::string(argv[2]) != "video"))
        return 2;
    umask(0077);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        return 1;
    const rlimit memory{1536ULL * 1024 * 1024, 1536ULL * 1024 * 1024},
        file{1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024}, core{0, 0};
    if (setrlimit(RLIMIT_AS, &memory) || setrlimit(RLIMIT_FSIZE, &file) ||
        setrlimit(RLIMIT_CORE, &core))
        return 1;
    XInitThreads();
    gst_init(nullptr, nullptr);
    Capture state;
    state.mode = argv[2];
    state.output = argv[3];
    state.backend = argv[4];
    if (state.backend != "x11" && state.backend != "portal")
        return 2;
    try {
        rimes::runtime::PrivateDirectory parent(state.output.parent_path());
        state.output_fd =
            open(state.output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (state.output_fd < 0)
            throw std::runtime_error("无法创建私有捕获文件");
        fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
        g_unix_signal_add(
            SIGTERM,
            +[](gpointer data) -> gboolean {
                static_cast<Capture*>(data)->Cancel();
                return G_SOURCE_REMOVE;
            },
            &state);
        g_timeout_add(
            50,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<Capture*>(data);
                char buffer[128];
                const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
                if (count == 0 || (count < 0 && errno != EINTR && errno != EAGAIN))
                    self->Cancel();
                else if (count > 0) {
                    self->commands.append(buffer, count);
                    if (self->commands.size() > 256)
                        self->Cancel();
                    for (auto at = self->commands.find('\n'); at != std::string::npos;
                         at = self->commands.find('\n')) {
                        const auto command = self->commands.substr(0, at);
                        self->commands.erase(0, at + 1);
                        if (command == "stop")
                            self->Stop();
                        else
                            self->Cancel();
                    }
                }
                const auto now = g_get_monotonic_time();
                if (!self->started && now - self->requested > 120000000)
                    self->Fail("等待桌面授权超时");
                if (self->started && !self->stopping && now - self->began > 600000000)
                    self->Stop();
                if (self->stopping && now - self->stop_at > 8000000)
                    self->Fail("录屏收尾超时");
                return self->finished ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
            },
            &state);
        state.WatchScreenLock();
        if (state.backend == "portal") {
            state.portal = xdp_portal_new();
            if (state.mode == "image")
                state.PortalScreenshot();
            else
                state.PortalVideo();
        } else
            state.StartPipeline();
        if (!state.finished)
            g_main_loop_run(state.loop);
    } catch (const std::exception& e) {
        state.Fail(e.what());
    }
    return state.success ? 0 : 1;
}
