#include "../../product_brand.hpp"
#include "buffer_placement.hpp"
#include "buffer_protocol.hpp"
#include "desktop.hpp"
#include "panel_window.hpp"
#include "launcher_client.hpp"
#include "native_buffer_metrics.hpp"
#include <filesystem>
#include <fstream>
#include <cmath>

#include <gdk/gdk.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

#ifdef HAVE_GTK_LAYER_SHELL
#include <gtk-layer-shell.h>
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kWindowWidth = 760;
constexpr int kWindowHeight = rimes::native::bufferHeight;
constexpr int kToolbarHeight = 30;
constexpr int kGapAroundCaret = 10;

struct App {
    GtkWidget* window = nullptr;
    GtkWidget* root = nullptr;
    GtkWidget* chip_box = nullptr;
    GtkWidget* source_row = nullptr;
    GtkWidget* result_row = nullptr;
    GtkWidget* result_text = nullptr;
    GtkWidget* result_chips = nullptr;
    GtkWidget* source_actions = nullptr;
    GtkWidget* result_actions = nullptr;
    GtkWidget* selector = nullptr;
    GtkWidget* edit = nullptr;
    GtkWidget* auto_send = nullptr;
    GtkWidget* close_after_last = nullptr;
    GtkWidget* features = nullptr;
    GtkWidget* body = nullptr;
    GtkWidget* toolbar = nullptr;
    GtkWidget* copy = nullptr;
    GtkWidget* generate = nullptr;
    GtkWidget* pager = nullptr;
    GtkWidget* pager_count = nullptr;
    bool render = false;
    int height = kWindowHeight;
    std::string pending_copy;
    std::string theme;
    GtkWidget* placeholder = nullptr;
    GtkWidget* preedit = nullptr;
    GtkWidget* caret = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* send = nullptr;
    GtkWidget* progress = nullptr;
    GtkWidget* target = nullptr;
    int socket_fd = -1;
    guint io_id = 0;
    std::string incoming;
    std::string dump_geometry;
    bool quit_after_dump = false;
    bool dumped_geometry = false;
    guint geometry_timer = 0;
    rimes::buffer::Snapshot snapshot;
    bool wayland_layer = false;
    std::unique_ptr<rimes::desktop::PanelWindow> panel;
    bool placed = false;
    bool moving = false;
    bool moved = false;
    bool folded = false;
    bool updating_switch = false;
    double drag_x = 0, drag_y = 0;
    double drag_root_x = 0, drag_root_y = 0;
    rimes::runtime::WindowRect drag_origin;
    guint drag_poll_id = 0;
    guint drag_cap_id = 0;
};

App* g_app = nullptr;

std::string DefaultSocketPath() {
    if (const char* override_path = g_getenv("RIMES_BUFFER_SOCKET")) {
        if (override_path[0] != '\0') {
            return override_path;
        }
    }
    if (const char* runtime = g_getenv("XDG_RUNTIME_DIR")) {
        return std::string(runtime) + "/rimes-buffer.sock";
    }
    return "/tmp/rimes-buffer-" + std::to_string(geteuid()) + ".sock";
}

bool SendCommand(App* app, const std::string& json) {
    std::string frame;
    if (app->socket_fd < 0 || !rimes::buffer::EncodeFrame(json, &frame)) {
        return false;
    }
    return send(app->socket_fd, frame.data(), frame.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(frame.size());
}

void PlacePanel(App* app, bool opening) {
    if (!app->panel) return;
    const auto& caret = app->snapshot.caret;
    std::optional<rimes::runtime::WindowRect> anchor;
    if (caret.valid) anchor = rimes::runtime::WindowRect{caret.x, caret.y, caret.width, caret.height};
    app->panel->Place(app->snapshot.width, app->height, anchor, opening,
        [app](const auto& work, const auto& c) {
            const auto placed = rimes::buffer::PlaceX11Panel({c.x, c.y, c.width, c.height, true},
                {work.x, work.y, work.width, work.height}, app->snapshot.width, app->height, kGapAroundCaret);
            return rimes::runtime::WindowRect{placed.x, placed.y, app->snapshot.width, app->height};
        });
    app->placed = true;
}

void RebuildChips(App* app) {
    GList* children = gtk_container_get_children(GTK_CONTAINER(app->chip_box));
    for (GList* cursor = children; cursor != nullptr; cursor = cursor->next) {
        gtk_widget_destroy(GTK_WIDGET(cursor->data));
    }
    g_list_free(children);

    if (app->snapshot.secure) {
        gtk_label_set_text(GTK_LABEL(app->placeholder), "");
        gtk_label_set_text(GTK_LABEL(app->preedit), "");
        gtk_widget_hide(app->placeholder);
        gtk_widget_hide(app->preedit);
        return;
    }

    const bool idle = app->snapshot.blocks.empty() && app->snapshot.preedit.empty() &&
                      !app->snapshot.capturing;
    if (idle) {
        gtk_label_set_text(GTK_LABEL(app->placeholder), app->snapshot.placeholder.c_str());
        gtk_widget_show(app->placeholder);
    } else {
        gtk_widget_hide(app->placeholder);
    }

    for (const auto& block : app->snapshot.blocks) {
        GtkWidget* chip = gtk_label_new(block.text.c_str());
        gtk_widget_set_name(chip, "rimes-chip");
        const auto age = app->snapshot.auto_send_progress.find(block.id);
        gtk_widget_set_opacity(chip, 1.0 - 0.55 * (age == app->snapshot.auto_send_progress.end() ? 0 : age->second));
        gtk_widget_set_size_request(chip, -1, rimes::native::itemHeight);
        gtk_widget_set_tooltip_text(chip, block.text.c_str());
        gtk_label_set_single_line_mode(GTK_LABEL(chip), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(chip), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(app->chip_box), chip, FALSE, FALSE, 0);
        gtk_widget_show(chip);
    }
    if (!app->snapshot.preedit.empty()) {
        gtk_label_set_text(GTK_LABEL(app->preedit), app->snapshot.preedit.c_str());
        gtk_widget_show(app->preedit);
    } else {
        gtk_widget_hide(app->preedit);
    }
}

void LoadCss(const std::string& theme, GdkScreen* screen);
void Paste(App* app) {
    if (app->snapshot.secure || !app->snapshot.visible) return;
    auto* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    auto* text = gtk_clipboard_wait_for_text(clipboard);
    if (text && *text && !app->snapshot.secure && app->snapshot.visible)
        SendCommand(app, rimes::runtime::Json{{"v", 1}, {"op", "paste"}, {"text", text}}.dump());
    g_free(text);
}

void Refresh(App* app) {
    const auto& s = app->snapshot;
    if (app->theme != s.theme) { LoadCss(s.theme, gtk_widget_get_screen(app->window)); app->theme = s.theme; }
    const bool derived = !s.active_plugin.empty();
    const bool output = s.phase == rimes::buffer::DerivedPhase::Running ||
                        s.phase == rimes::buffer::DerivedPhase::Failed || !s.results.empty();
    const bool source = s.secure ? gtk_widget_get_visible(app->source_row) : !derived || (s.live_expand ? !s.blocks.empty() || !output : !output);
    const bool target = s.secure ? gtk_widget_get_visible(app->result_row) : derived && (output || (s.live_expand && !s.blocks.empty()));
    const int height = app->folded ? rimes::native::bufferToolbarOnlyHeight : s.secure ? app->height : source && target ? rimes::native::bufferDerivedHeight : kWindowHeight;
    app->height = height;
    if (app->render) {
        gtk_widget_set_size_request(app->window, s.width, height);
        gtk_window_resize(GTK_WINDOW(app->window), s.width, height);
    } else if (s.visible) PlacePanel(app, !app->placed);
    if (s.visible) {
        gtk_widget_show_all(app->window);
        gtk_window_set_keep_above(GTK_WINDOW(app->window), TRUE);
    } else {
        gtk_widget_hide(app->window);
        app->placed = false;
    }
    RebuildChips(app);
    if (s.secure) app->pending_copy.clear();
    gtk_widget_set_sensitive(app->source_actions, !s.secure);
    gtk_widget_set_sensitive(app->result_actions, !s.secure);
    gtk_widget_set_visible(app->caret, s.capturing && !s.secure);
    gtk_widget_set_visible(app->source_row, source);
    gtk_widget_set_visible(app->result_row, target);
    gtk_widget_set_visible(app->source_actions, source && !target);
    gtk_widget_set_visible(app->edit, derived && output);
    gtk_widget_set_visible(app->auto_send, !derived);
    gtk_widget_set_sensitive(app->auto_send, !s.secure);
    gtk_widget_set_visible(app->close_after_last, !derived);
    gtk_widget_set_sensitive(app->close_after_last, !s.secure);
    app->updating_switch = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->close_after_last), s.close_after_last);
    app->updating_switch = false;
    gtk_widget_queue_draw(app->close_after_last);
    gtk_widget_set_visible(app->body, !app->folded);
    const auto auto_title = s.auto_send_seconds ? std::to_string(s.auto_send_seconds) : "N";
    gtk_button_set_label(GTK_BUTTON(app->auto_send), auto_title.c_str());
    const auto auto_help = s.auto_send_seconds ? "自动上屏：" + auto_title + " 秒" : "自动上屏：关闭";
    gtk_widget_set_tooltip_text(app->auto_send, auto_help.c_str());
    atk_object_set_name(gtk_widget_get_accessible(app->auto_send), auto_help.c_str());
    const bool result_action = derived && (s.can_deliver || s.phase == rimes::buffer::DerivedPhase::Ready);
    gtk_widget_set_visible(app->send, !derived || result_action);
    gtk_widget_set_sensitive(app->send, !s.secure && s.preedit.empty() && (derived ? s.can_deliver : s.capturing && !s.blocks.empty()));
    gtk_widget_set_visible(app->generate, derived && !result_action);
    gtk_widget_set_sensitive(app->generate, !s.secure && (s.can_generate || s.phase == rimes::buffer::DerivedPhase::Running));
    const char* action = s.phase == rimes::buffer::DerivedPhase::Running ? "停止生成" : "生成结果";
    gtk_widget_set_tooltip_text(app->generate, action);
    atk_object_set_name(gtk_widget_get_accessible(app->generate), action);
    std::string title = "Default";
    for (const auto& p : s.plugins) if (p.id == s.active_plugin) title = p.name;
    gtk_button_set_label(GTK_BUTTON(app->selector), (title + "  ⌄").c_str());
    std::string status;
    if (s.secure) status = "内容已隐藏";
    else if (!s.message.empty()) status = s.message;
    else if (s.phase == rimes::buffer::DerivedPhase::Running) status = "正在生成…";
    else if (s.phase == rimes::buffer::DerivedPhase::Failed) status = "生成失败，可重试";
    else if (!s.capturing) status = "已暂停";
    gtk_label_set_text(GTK_LABEL(app->status), status.c_str());
    gtk_widget_set_visible(app->status, !status.empty());
    gtk_label_set_text(GTK_LABEL(app->target), s.secure ? "" : s.target.c_str());
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), CLAMP(s.hold_progress, 0.0, 1.0));
    gtk_widget_set_visible(app->progress, s.hold_progress > 0);
    const auto result = std::find_if(s.results.begin(), s.results.end(), [&](const auto& r) { return r.id == s.selected_result; });
    const std::string text = s.secure ? "" : result == s.results.end() ? (s.phase == rimes::buffer::DerivedPhase::Failed ? "生成失败，可重试" : "等待结果…") : result->text;
    gtk_label_set_text(GTK_LABEL(app->result_text), text.c_str());
    gtk_widget_set_tooltip_text(app->result_text, text.c_str());
    auto* old_chips = gtk_container_get_children(GTK_CONTAINER(app->result_chips));
    for (auto* child = old_chips; child; child = child->next) gtk_widget_destroy(GTK_WIDGET(child->data));
    g_list_free(old_chips);
    for (const auto& block : s.result_blocks) {
        if (s.secure) break;
        auto* chip = gtk_label_new(block.text.c_str());
        gtk_widget_set_name(chip, "rimes-result");
        gtk_widget_set_size_request(chip, -1, rimes::native::itemHeight);
        gtk_label_set_single_line_mode(GTK_LABEL(chip), TRUE);
        gtk_widget_set_tooltip_text(chip, block.text.c_str());
        gtk_box_pack_start(GTK_BOX(app->result_chips), chip, FALSE, FALSE, 0);
        gtk_widget_show(chip);
    }
    gtk_widget_set_visible(app->result_text, s.result_blocks.empty() || s.secure);
    gtk_widget_set_visible(app->copy, result != s.results.end());
    gtk_widget_set_sensitive(app->copy, result != s.results.end() && result->ready && !s.secure);
    gtk_widget_set_visible(app->pager, s.results.size() > 1);
    const auto index = result == s.results.end() ? 0 : result - s.results.begin();
    gtk_label_set_text(GTK_LABEL(app->pager_count), (std::to_string(index + 1) + "/" + std::to_string(s.results.size())).c_str());
    // The same action overlay moves between source and target without reserving rail width.
    auto* actions = target ? app->result_actions : app->source_actions;
    for (auto* control : {app->generate, app->send, app->copy}) {
        if (gtk_widget_get_parent(control) != actions) {
            g_object_ref(control);
            gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(control)), control);
            gtk_box_pack_start(GTK_BOX(actions), control, FALSE, FALSE, 0);
            g_object_unref(control);
        }
    }
}

void ApplySnapshotJson(App* app, const std::string& json) {
    rimes::buffer::Snapshot snapshot;
    if (rimes::buffer::ParseSnapshot(json, &snapshot)) {
        app->snapshot = std::move(snapshot);
        Refresh(app);
        return;
    }
    try {
        const auto message = rimes::runtime::Json::parse(json);
        if (message.value("type", "") == "request_clipboard") Paste(app);
        else if (message.value("type", "") == "copy_result" && !app->pending_copy.empty() &&
                 message.value("id", "") == app->pending_copy && !app->snapshot.secure) {
            const auto text = message.at("text").get<std::string>();
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text.c_str(), text.size());
            app->pending_copy.clear();
        }
    } catch (const std::exception&) { /* Ignore unknown or incomplete messages. */ }
}

gboolean OnSocket(GIOChannel* /*channel*/, GIOCondition condition, gpointer data) {
    auto* app = static_cast<App*>(data);
    if ((condition & (G_IO_ERR | G_IO_HUP)) != 0) {
        gtk_main_quit();
        return FALSE;
    }
    char chunk[4096];
    const auto got = read(app->socket_fd, chunk, sizeof(chunk));
    if (got <= 0) {
        gtk_main_quit();
        return FALSE;
    }
    app->incoming.append(chunk, static_cast<std::size_t>(got));
    while (app->incoming.size() >= 4) {
        std::uint32_t length = 0;
        if (!rimes::buffer::DecodeFrameHeader(app->incoming.data(), &length)) {
            gtk_main_quit();
            return FALSE;
        }
        if (app->incoming.size() < 4 + length) {
            break;
        }
        const std::string payload = app->incoming.substr(4, length);
        app->incoming.erase(0, 4 + length);
        ApplySnapshotJson(app, payload);
    }
    return TRUE;
}

void WriteGeometryDump(App* app) {
    if (app->dump_geometry.empty() || app->dumped_geometry) {
        return;
    }
    GtkAllocation allocation{};
    gtk_widget_get_allocation(app->window, &allocation);
    if (allocation.width < 2 || allocation.height < 2) {
        gtk_window_get_size(GTK_WINDOW(app->window), &allocation.width, &allocation.height);
    }
    if (allocation.width < 2 && !app->quit_after_dump) {
        return;
    }
    gint request_w = 0;
    gint request_h = 0;
    gtk_widget_get_size_request(app->window, &request_w, &request_h);
    FILE* file = std::fopen(app->dump_geometry.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    rimes::runtime::Json dump{{"width", allocation.width}, {"height", allocation.height},
        {"size_request_w", request_w}, {"size_request_h", request_h}, {"layer", app->wayland_layer},
        {"min_width", app->snapshot.width}, {"min_height", app->height}};
    for (const auto& [name, widget] : std::vector<std::pair<const char*, GtkWidget*>>{
        {"closeAfterLast", app->close_after_last}, {"autoSend", app->auto_send}, {"features", app->features}}) {
        if (!widget) continue;
        int x = 0, y = 0;
        gtk_widget_translate_coordinates(widget, app->window, 0, 0, &x, &y);
        dump["controls"][name] = {{"x", x}, {"y", y}, {"width", gtk_widget_get_allocated_width(widget)},
                                  {"height", gtk_widget_get_allocated_height(widget)}};
    }
    const auto json = dump.dump();
    std::fwrite(json.data(), 1, json.size(), file);
    std::fclose(file);
    app->dumped_geometry = true;
    if (app->quit_after_dump) {
        g_idle_add(+[](gpointer) -> gboolean {
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        }, nullptr);
    }
}

void OnSizeAllocate(GtkWidget* /*widget*/, GdkRectangle* allocation, gpointer data) {
    if (allocation == nullptr || allocation->width < 2 || allocation->height < 2) {
        return;
    }
    auto* app = static_cast<App*>(data);
    if (app->dump_geometry.empty() || app->dumped_geometry || app->geometry_timer) return;
    // The window's size-allocate runs before all descendants have their final
    // coordinates. Capture settled geometry, never the pre-theme requisition.
    app->geometry_timer = g_timeout_add(100, +[](gpointer data) -> gboolean {
        auto* app = static_cast<App*>(data); app->geometry_timer = 0;
        WriteGeometryDump(app); return G_SOURCE_REMOVE;
    }, app);
}

void CancelDragTimers(App* app) {
    if (app->drag_poll_id != 0) {
        g_source_remove(app->drag_poll_id);
        app->drag_poll_id = 0;
    }
    if (app->drag_cap_id != 0) {
        g_source_remove(app->drag_cap_id);
        app->drag_cap_id = 0;
    }
}

void FinishMoveDrag(App* app, bool fold_click = false) {
    if (!app->moving) {
        return;
    }
    app->moving = false;
    CancelDragTimers(app);
    if (gtk_widget_has_grab(app->window)) gtk_grab_remove(app->window);
    if (app->panel) {
        const auto frame = app->panel->Frame();
        app->moved = app->moved || std::abs(frame.x - app->drag_origin.x) > 3 || std::abs(frame.y - app->drag_origin.y) > 3;
        if (app->moved) app->panel->Remember();
    }
    SendCommand(app, R"({"v":1,"op":"drag_end"})");
    if (fold_click && !app->moved && !app->snapshot.secure) {
        app->folded = !app->folded; Refresh(app);
    }
}

bool PointerButton1Down(App* app) {
    if (app->window == nullptr) {
        return false;
    }
    GdkWindow* gdk_window = gtk_widget_get_window(app->window);
    GdkDisplay* display = gtk_widget_get_display(app->window);
    if (gdk_window == nullptr || display == nullptr) {
        return true;
    }
    GdkSeat* seat = gdk_display_get_default_seat(display);
    if (seat == nullptr) {
        return true;
    }
    GdkDevice* pointer = gdk_seat_get_pointer(seat);
    if (pointer == nullptr) {
        return true;
    }
    GdkModifierType mask = static_cast<GdkModifierType>(0);
    gdk_device_get_state(pointer, gdk_window, nullptr, &mask);
    return (mask & GDK_BUTTON1_MASK) != 0;
}

gboolean OnDragPoll(gpointer data) {
    auto* app = static_cast<App*>(data);
    if (!app->moving) {
        app->drag_poll_id = 0;
        return G_SOURCE_REMOVE;
    }
    if (app->panel) {
        const auto frame = app->panel->Frame();
        app->moved = app->moved || std::abs(frame.x - app->drag_origin.x) > 3 || std::abs(frame.y - app->drag_origin.y) > 3;
    }
    if (!PointerButton1Down(app)) {
        FinishMoveDrag(app, true);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

gboolean OnDragHardCap(gpointer data) {
    auto* app = static_cast<App*>(data);
    app->drag_cap_id = 0;
    FinishMoveDrag(app);
    return G_SOURCE_REMOVE;
}

gboolean OnButtonReleaseDuringDrag(GtkWidget* /*widget*/, GdkEventButton* event, gpointer data) {
    auto* app = static_cast<App*>(data);
    if (event->button == 1) {
        FinishMoveDrag(app, true);
    }
    return FALSE;
}

gboolean OnToolbarDrag(GtkWidget* /*widget*/, GdkEventButton* event, gpointer data) {
    auto* app = static_cast<App*>(data);
    auto* source = gtk_get_event_widget(reinterpret_cast<GdkEvent*>(event));
    if (source && (GTK_IS_BUTTON(source) || gtk_widget_get_ancestor(source, GTK_TYPE_BUTTON))) return FALSE;
    if (event->y > kToolbarHeight + 2 || event->y < 0) return FALSE;
    if ((event->type == GDK_BUTTON_PRESS || event->type == GDK_2BUTTON_PRESS || event->type == GDK_3BUTTON_PRESS) &&
        event->button == 1 && !app->moving && app->panel && app->panel->CanPosition()) {
        CancelDragTimers(app);
        app->moving = true;
        app->moved = false;
        app->drag_x = event->x; app->drag_y = event->y;
        app->drag_root_x = event->x_root; app->drag_root_y = event->y_root;
        if (app->panel) app->drag_origin = app->panel->Frame();
        SendCommand(app, R"({"v":1,"op":"drag_begin"})");
        if (!app->wayland_layer) app->drag_poll_id = g_timeout_add(50, OnDragPoll, app);
        app->drag_cap_id = g_timeout_add(30000, OnDragHardCap, app);
        // Keep the same passive gesture on both backends. Beginning a WM move
        // for a simple click can leave a stale pointer grab when folding also
        // moves/resizes the window, particularly on scaled X11 desktops.
        gtk_grab_add(app->window);
        return TRUE;
    }
    return FALSE;
}

gboolean OnToolbarMotion(GtkWidget*, GdkEventMotion* event, gpointer data) {
    auto* app = static_cast<App*>(data);
    if (!app->moving) return FALSE;
    const int dx = std::lround(app->wayland_layer ? event->x - app->drag_x : event->x_root - app->drag_root_x);
    const int dy = std::lround(app->wayland_layer ? event->y - app->drag_y : event->y_root - app->drag_root_y);
    if (!app->moved && std::abs(dx) < 4 && std::abs(dy) < 4) return TRUE;
    app->moved = true;
    auto frame = app->wayland_layer ? app->panel->Frame() : app->drag_origin;
    frame.x += dx; frame.y += dy;
    app->panel->Move(frame);
    return TRUE;
}

int ConnectDelayMs() {
    const char* value = g_getenv("RIMES_BUFFER_CONNECT_DELAY_MS");
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }
    char* end = nullptr;
    const auto parsed = std::strtol(value, &end, 10);
    if (end == value || parsed < 0 || parsed > 30000) {
        return 0;
    }
    return static_cast<int>(parsed);
}

gboolean ConnectSocket(App* app, const std::string& path) {
    const int delay_ms = ConnectDelayMs();
    if (delay_ms > 0) {
        g_usleep(static_cast<gulong>(delay_ms) * 1000);
    }
    app->socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (app->socket_fd < 0) {
        return FALSE;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (connect(app->socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(app->socket_fd);
        app->socket_fd = -1;
        return FALSE;
    }
    const int flags = fcntl(app->socket_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(app->socket_fd, F_SETFL, flags | O_NONBLOCK);
    }
    GIOChannel* channel = g_io_channel_unix_new(app->socket_fd);
    g_io_channel_set_encoding(channel, nullptr, nullptr);
    g_io_channel_set_buffered(channel, FALSE);
    app->io_id = g_io_add_watch(channel, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
                                OnSocket, app);
    g_io_channel_unref(channel);
    SendCommand(app, R"({"v":1,"op":"hello"})");
    return TRUE;
}

void LoadCss(const std::string& theme, GdkScreen* screen) {
    using namespace rimes::desktop;
    Theme(theme, screen);
    const auto& palette = Visuals().at("palettes").at(theme == "liquidGlass" ? "day" : theme);
    auto c = [&](const char* name) { return palette.at(name).get<std::string>(); };
    static auto* provider = gtk_css_provider_new();
    gtk_style_context_remove_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider));
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    const std::string css =
        "window.rimes-buffer { background: transparent; }"
        "#rimes-chrome { background: " + c("bufferBackground") + "; border-radius: 9px; }"
        "#rimes-toolbar { min-height: 30px; border-bottom: 1px solid " + c("bufferDivider") + "; }"
        "#rimes-title, #rimes-status, #rimes-target { color: " + c("textSecondary") + "; font-size: 11px; }"
        "#rimes-rails { background: " + c("candidateBackground") + "; border: 0.5px solid " + c("borderStrong") + "; border-radius: 6px; }"
        "#rimes-chip { background: transparent; color: " + c("textPrimary") + "; border-bottom: 1px solid " + c("borderStrong") + "; font-size: 12px; padding: 0; }"
        "#rimes-result { background: transparent; color: " + c("textPrimary") + "; font-size: 12px; border-bottom: 1px solid " + c("borderStrong") + "; }"
        "#rimes-caret { background: " + c("accentBlue") + "; border-radius: 1px; }"
        "#rimes-placeholder, #rimes-preedit { color: " + c("bufferMuted") + "; font-size: 12px; }"
        "#rimes-preedit { text-decoration: underline; color: " + c("accentBlue") + "; }"
        "button.rimes-action, button.rimes-selector { background: " + c("surfaceSecondary") + "; color: " + c("textPrimary") + "; border: 0.5px solid " + c("border") + "; border-radius: 6px; min-width: 21px; min-height: 21px; padding: 0; }"
        "button.rimes-selector { padding: 0 7px; font-weight: 600; }"
        "button.rimes-action:hover, button.rimes-selector:hover { background: " + c("surfaceTertiary") + "; }"
        "button.rimes-action .glyph { color: " + c("textSecondary") + "; }"
        ".rimes-actions { background: linear-gradient(to right, transparent, " + c("candidateBackground") + " 18px); padding-left: 18px; }"
        "#rimes-progress trough { min-height: 2px; background: transparent; }"
        "#rimes-progress progress { min-height: 2px; background: " + c("accentBlue") + "; }";
    GError* error = nullptr;
    if (!gtk_css_provider_load_from_data(provider, css.c_str(), css.size(), &error)) {
        g_clear_error(&error);
        throw std::runtime_error("Buffer style could not load");
    }
}

GtkWidget* Action(const std::string& icon, const std::string& label, std::function<void()> clicked) {
    using namespace rimes::desktop;
    auto* button = Button("", std::move(clicked), "rimes-action");
    gtk_container_remove(GTK_CONTAINER(button), gtk_bin_get_child(GTK_BIN(button)));
    auto* glyph = Icon(icon, 12);
    gtk_widget_set_halign(glyph, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(glyph, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(button), glyph);
    gtk_widget_set_size_request(button, rimes::native::controlSize, rimes::native::controlSize);
    gtk_widget_set_tooltip_text(button, label.c_str());
    atk_object_set_name(gtk_widget_get_accessible(button), label.c_str());
    gtk_widget_set_can_focus(button, FALSE);
    return button;
}

void PluginMenu(App* app, GtkWidget* anchor) {
    auto* menu = gtk_menu_new();
    auto add = [&](std::string id, const std::string& title) {
        auto* item = gtk_menu_item_new_with_label(title.c_str());
        auto* fn = new std::function<void()>([app, id] { SendCommand(app, rimes::runtime::Json{{"v",1},{"op","select_plugin"},{"id",id}}.dump()); });
        g_signal_connect_data(item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer p) { (*static_cast<std::function<void()>*>(p))(); }), fn,
                              +[](gpointer p, GClosure*) { delete static_cast<std::function<void()>*>(p); }, GConnectFlags(0));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    };
    add("", "Default");
    for (const auto& plugin : app->snapshot.plugins) add(plugin.id, plugin.name);
    g_signal_connect(menu, "selection-done", G_CALLBACK(+[](GtkWidget* w) { gtk_widget_destroy(w); }), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), anchor, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
}

void FeatureMenu(App* app) {
    auto* menu = gtk_menu_new();
    auto add = [&](GtkWidget* parent, const std::string& title, std::function<void()> action, bool enabled = true) {
        auto* item = gtk_menu_item_new_with_label(title.c_str());
        gtk_widget_set_sensitive(item, enabled);
        auto* fn = new std::function<void()>(std::move(action));
        g_signal_connect_data(item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data) {
            (*static_cast<std::function<void()>*>(data))();
        }), fn, +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, GConnectFlags(0));
        gtk_menu_shell_append(GTK_MENU_SHELL(parent), item);
        return item;
    };
    add(menu, app->folded ? "展开内容区" : "收起内容区", [app] { app->folded = !app->folded; Refresh(app); }, !app->snapshot.secure);
    add(menu, "还原窗口位置", [app] { app->panel->Reset(); }, app->panel && app->panel->CanPosition());
    auto* displays = add(menu, "移到显示器", [] {}, app->panel && app->panel->CanPosition());
    auto* display_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(displays), display_menu);
    if (app->panel) {
        const auto names = app->panel->MonitorNames();
        for (std::size_t i = 0; i < names.size(); ++i)
            add(display_menu, names[i], [app, i] { app->panel->SelectMonitor(i); });
        if (!app->panel->CanPosition()) add(menu, app->panel->Backend(), [] {}, false);
    }
    add(menu, "设置…", [app] {
        rimes::desktop::LauncherCall("Open", "settings", nullptr, [app](const auto& result) {
            if (!result.value("ok", false)) {
                gtk_label_set_text(GTK_LABEL(app->status), "无法打开设置，请从应用菜单重试");
                gtk_widget_show(app->status);
            }
        });
    });
    g_signal_connect(menu, "selection-done", G_CALLBACK(+[](GtkWidget* widget) { gtk_widget_destroy(widget); }), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), app->features, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
}

void BuildUi(App* app) {
    using namespace rimes::desktop;
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), rimes::branding::Title(" Buffer").c_str());
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "rimes");
    gtk_window_set_default_size(GTK_WINDOW(app->window), kWindowWidth, kWindowHeight);
    gtk_widget_set_size_request(app->window, kWindowWidth, kWindowHeight);
    gtk_window_set_resizable(GTK_WINDOW(app->window), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(app->window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(app->window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(app->window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(app->window), FALSE);
    gtk_window_set_focus_on_map(GTK_WINDOW(app->window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(app->window), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_window_set_keep_above(GTK_WINDOW(app->window), TRUE);
    gtk_widget_set_name(app->window, "rimes-buffer-window");
    Class(app->window, "rimes-buffer");
    gtk_widget_set_app_paintable(app->window, TRUE);
    gtk_widget_add_events(app->window, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    g_signal_connect(app->window, "size-allocate", G_CALLBACK(OnSizeAllocate), app);
    g_signal_connect(app->window, "button-release-event", G_CALLBACK(OnButtonReleaseDuringDrag), app);
    if (!app->render) {
        app->panel = std::make_unique<rimes::desktop::PanelWindow>(GTK_WINDOW(app->window), "buffer",
            rimes::buffer::kCandidatePopupReserve + kGapAroundCaret);
        app->wayland_layer = app->panel->Layer();
    }
    auto* outer = Column(0);
    app->root = outer;
    gtk_container_add(GTK_CONTAINER(app->window), outer);
    auto* chrome = Column(0);
    gtk_widget_set_margin_top(chrome, rimes::native::chromeInset);
    gtk_widget_set_margin_bottom(chrome, rimes::native::chromeInset);
    gtk_widget_set_margin_start(chrome, rimes::native::chromeInset);
    gtk_widget_set_margin_end(chrome, rimes::native::chromeInset);
    gtk_widget_set_name(chrome, "rimes-chrome");
    Add(outer, chrome, true);
    auto* toolbar = Column(0);
    app->toolbar = toolbar;
    gtk_widget_set_name(toolbar, "rimes-toolbar");
    gtk_widget_set_size_request(toolbar, -1, kToolbarHeight + 1);
    g_signal_connect(app->window, "button-press-event", G_CALLBACK(OnToolbarDrag), app);
    g_signal_connect(app->window, "motion-notify-event", G_CALLBACK(OnToolbarMotion), app);
    Add(chrome, toolbar);
    auto* tools = Row(4);
    gtk_widget_set_margin_start(tools, rimes::native::shelfHorizontalInset);
    gtk_widget_set_margin_end(tools, rimes::native::shelfHorizontalInset);
    gtk_widget_set_valign(tools, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(toolbar), tools);
    app->features = Action("square.grid.2x2", "Buffer 功能", [app] { FeatureMenu(app); });
    Add(tools, app->features);
    app->selector = Button("Default  ⌄", [app] { PluginMenu(app, app->selector); }, "rimes-selector");
    gtk_widget_set_can_focus(app->selector, FALSE);
    Add(tools, app->selector);
    app->status = Label("");
    gtk_widget_set_name(app->status, "rimes-status");
    Add(tools, app->status);
    Add(tools, Label(""), true);
    app->edit = Action("arrow.uturn.backward", "返回编辑", [app] { SendCommand(app, R"({"v":1,"op":"return_to_edit"})"); });
    Add(tools, app->edit);
    app->auto_send = Button("N", [app] { SendCommand(app, R"({"v":1,"op":"cycle_auto_send"})"); }, "rimes-action");
    gtk_widget_set_can_focus(app->auto_send, FALSE);
    gtk_widget_set_size_request(app->auto_send, rimes::native::controlSize, rimes::native::controlSize);
    Add(tools, app->auto_send);
    app->close_after_last = CompactSwitch("发送完关闭", [app] {
        if (!app->updating_switch) SendCommand(app, R"({"v":1,"op":"toggle_close_after_last"})");
    });
    Add(tools, app->close_after_last);
    app->target = Label("");
    gtk_label_set_ellipsize(GTK_LABEL(app->target), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app->target), 16);
    gtk_widget_set_name(app->target, "rimes-target");
    gtk_widget_set_tooltip_text(app->target, "上屏目标");
    Add(tools, app->target);
    Add(tools, Action("xmark", "关闭并暂停", [app] { SendCommand(app, R"({"v":1,"op":"close"})"); }));
    auto* body = Column(0);
    app->body = body;
    gtk_widget_set_margin_top(body, 3);
    gtk_widget_set_margin_bottom(body, 3);
    gtk_widget_set_margin_start(body, rimes::native::mainHorizontalInset);
    gtk_widget_set_margin_end(body, rimes::native::mainHorizontalInset);
    Add(chrome, body, true);
    auto* rails = Column(4);
    gtk_widget_set_name(rails, "rimes-rails");
    auto* rail_overlay = gtk_overlay_new();
    Add(body, rail_overlay, true);
    gtk_container_add(GTK_CONTAINER(rail_overlay), rails);
    auto make_rail = [&](GtkWidget** row, GtkWidget** actions) {
        *row = gtk_overlay_new();
        gtk_widget_set_size_request(*row, -1, 28);
        Add(rails, *row, true);
        auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
        gtk_container_add(GTK_CONTAINER(*row), scroll);
        auto* line = Row(rimes::native::blockSpacing);
        gtk_widget_set_margin_start(line, rimes::native::railHorizontalInset);
        gtk_widget_set_margin_end(line, rimes::native::railHorizontalInset);
        gtk_widget_set_valign(line, GTK_ALIGN_CENTER);
        gtk_container_add(GTK_CONTAINER(scroll), line);
        *actions = Class(Row(3), "rimes-actions");
        gtk_widget_set_margin_end(*actions, rimes::native::railHorizontalInset);
        gtk_widget_set_halign(*actions, GTK_ALIGN_END);
        gtk_widget_set_valign(*actions, GTK_ALIGN_CENTER);
        gtk_overlay_add_overlay(GTK_OVERLAY(*row), *actions);
        return line;
    };
    auto* source = make_rail(&app->source_row, &app->source_actions);
    app->placeholder = Label(rimes::buffer::kPlaceholder);
    gtk_widget_set_name(app->placeholder, "rimes-placeholder");
    Add(source, app->placeholder);
    app->chip_box = Row(rimes::native::blockSpacing);
    Add(source, app->chip_box);
    app->preedit = Label("");
    gtk_widget_set_name(app->preedit, "rimes-preedit");
    Add(source, app->preedit);
    app->caret = gtk_event_box_new();
    gtk_widget_set_name(app->caret, "rimes-caret");
    gtk_widget_set_size_request(app->caret, 2, 18);
    gtk_widget_set_valign(app->caret, GTK_ALIGN_CENTER);
    Add(source, app->caret);
    auto* target = make_rail(&app->result_row, &app->result_actions);
    app->result_chips = Row(rimes::native::blockSpacing);
    Add(target, app->result_chips);
    app->result_text = Label("");
    gtk_widget_set_size_request(app->result_text, -1, rimes::native::itemHeight);
    gtk_widget_set_name(app->result_text, "rimes-result");
    gtk_label_set_single_line_mode(GTK_LABEL(app->result_text), TRUE);
    Add(target, app->result_text);
    app->pager = Row(2);
    auto move = [app](int delta) {
        const auto& s = app->snapshot;
        const auto current = std::find_if(s.results.begin(), s.results.end(), [&](const auto& r) { return r.id == s.selected_result; });
        if (current == s.results.end()) return;
        const int index = static_cast<int>(current - s.results.begin()) + delta;
        if (index >= 0 && index < static_cast<int>(s.results.size()))
            SendCommand(app, rimes::runtime::Json{{"v",1},{"op","select_result"},{"id",s.results[index].id}}.dump());
    };
    Add(app->pager, Action("chevron.left", "上一个结果", [move] { move(-1); }));
    app->pager_count = Label("");
    Add(app->pager, app->pager_count);
    Add(app->pager, Action("chevron.right", "下一个结果", [move] { move(1); }));
    Add(app->result_actions, app->pager);
    app->copy = Action("doc.on.doc", "复制结果", [app] {
        app->pending_copy = app->snapshot.selected_result;
        SendCommand(app, rimes::runtime::Json{{"v",1},{"op","copy_result"},{"id",app->pending_copy}}.dump());
    });
    app->generate = Action("sparkles", "生成结果", [app] {
        SendCommand(app, app->snapshot.phase == rimes::buffer::DerivedPhase::Running ? R"({"v":1,"op":"cancel_generation"})" : R"({"v":1,"op":"generate"})");
    });
    app->send = Action("paperplane.fill", "发送下一块", [app] { SendCommand(app, R"({"v":1,"op":"send_next"})"); });
    Add(app->source_actions, Action("doc.on.clipboard", "导入剪贴板", [app] { Paste(app); }));
    for (auto* control : {app->copy, app->generate, app->send}) Add(app->source_actions, control);
    app->progress = gtk_progress_bar_new();
    gtk_widget_set_name(app->progress, "rimes-progress");
    gtk_widget_set_valign(app->progress, GTK_ALIGN_END);
    gtk_overlay_add_overlay(GTK_OVERLAY(rail_overlay), app->progress);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(rail_overlay), app->progress, TRUE);
}

int RenderFixtures(App* app, const std::filesystem::path& output) {
    std::filesystem::create_directories(output);
    auto& s = app->snapshot;
    s.visible = s.capturing = s.enabled = true;
    s.empty = false;
    s.blocks = {{"fixture-source", "同一窗口先展开为双轨", rimes::buffer::Origin::Rime, 0}};
    rimes::runtime::Json manifest = rimes::runtime::Json::array();
    for (const std::string theme : {"night", "day", "quiet", "rasta", "liquidGlass"}) {
        for (const std::string state : {"standard", "dual", "protected", "running", "failed", "alternatives", "exchange", "return-to-edit", "restored", "hold", "automatic", "translation-delivery"}) {
            s.theme = theme;
            s.secure = state == "protected";
            s.active_plugin = state == "standard" || state == "restored" || state == "hold" || state == "automatic" ? "" : "fixture.transform";
            s.auto_send_seconds = state == "automatic" ? 1 : 0;
            s.auto_send_progress = {{"fixture-source", state == "automatic" ? 0.5 : 0}};
            s.hold_progress = state == "hold" ? 0.5 : 0;
            s.plugins = {{"fixture.transform", "Default"}};
            s.live_expand = state != "exchange" && state != "return-to-edit";
            s.phase = state == "running" ? rimes::buffer::DerivedPhase::Running :
                      state == "failed" ? rimes::buffer::DerivedPhase::Failed :
                      state == "return-to-edit" || s.active_plugin.empty() ? rimes::buffer::DerivedPhase::Idle : rimes::buffer::DerivedPhase::Ready;
            s.results.clear();
            s.result_blocks.clear();
            if (s.phase == rimes::buffer::DerivedPhase::Ready || s.phase == rimes::buffer::DerivedPhase::Running)
                s.results.push_back({"fixture-result", "再验证它能缩回单轨", 0, s.phase == rimes::buffer::DerivedPhase::Ready});
            if (state == "alternatives") s.results.push_back({"fixture-alternative", "另一个结果", 1, true});
            if (state == "translation-delivery") {
                s.results = {{"fixture-result", "The weather is nice. Let's walk in the park. ", 0, true}};
                s.result_blocks = {{"part-1", "The weather is nice. ", rimes::buffer::Origin::Local, 0},
                                   {"part-2", "Let's walk in the ", rimes::buffer::Origin::Local, 0},
                                   {"part-3", "park. ", rimes::buffer::Origin::Local, 0}};
            }
            s.can_deliver = s.phase == rimes::buffer::DerivedPhase::Ready && !s.secure;
            s.selected_result = s.results.empty() ? "" : s.results.front().id;
            s.can_generate = !s.active_plugin.empty() && s.phase != rimes::buffer::DerivedPhase::Running;
            Refresh(app);
            if (s.secure && (!gtk_widget_get_visible(app->window) || app->height != rimes::native::bufferDerivedHeight ||
                std::strlen(gtk_label_get_text(GTK_LABEL(app->result_text))) || std::strlen(gtk_label_get_text(GTK_LABEL(app->target))))) {
                g_printerr("Protected Buffer must retain geometry and scrub content\n"); return 1;
            }
            auto* loop = g_main_loop_new(nullptr, FALSE);
            g_timeout_add(80, +[](gpointer p) -> gboolean { g_main_loop_quit(static_cast<GMainLoop*>(p)); return G_SOURCE_REMOVE; }, loop);
            g_main_loop_run(loop);
            g_main_loop_unref(loop);
            const auto filename = theme + "--" + state + ".png";
            const int width = gtk_widget_get_allocated_width(app->root), height = gtk_widget_get_allocated_height(app->root);
            if (width != s.width || height != app->height ||
                !rimes::desktop::Render(app->root, (output / filename).string())) {
                g_printerr("Buffer render dimensions: %dx%d expected %dx%d\n", width, height, s.width, app->height);
                return 1;
            }
            manifest.push_back({{"file",filename},{"theme",theme},{"state",state},{"width",width},{"height",height},{"fixture",true}});
        }
    }
    std::ofstream file(output / "manifest.json");
    file << manifest.dump(2) << '\n';
    return file ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    std::string socket_path = DefaultSocketPath();
    bool preview = false;
    std::string render_directory;
    std::string dump_geometry;
    bool quit_after_dump = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--socket") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (std::strcmp(argv[index], "--render") == 0 && index + 1 < argc) {
            render_directory = argv[++index];
        } else if (std::strcmp(argv[index], "--preview") == 0) {
            preview = true;
        } else if (std::strcmp(argv[index], "--dump-geometry") == 0 && index + 1 < argc) {
            dump_geometry = argv[++index];
        } else if (std::strcmp(argv[index], "--quit-after-dump") == 0) {
            quit_after_dump = true;
        }
    }

    App app;
    g_app = &app;
    app.render = !render_directory.empty();
    app.dump_geometry = dump_geometry;
    app.quit_after_dump = quit_after_dump;
    BuildUi(&app);
    LoadCss("night", gtk_widget_get_screen(app.window));
    app.theme = "night";
    if (app.render) return RenderFixtures(&app, render_directory);

    if (preview) {
        app.snapshot.visible = true;
        app.snapshot.capturing = true;
        app.snapshot.placeholder = rimes::buffer::kPlaceholder;
        rimes::buffer::Block demo;
        demo.id = "preview";
        demo.text = "你好";
        demo.origin = rimes::buffer::Origin::Rime;
        app.snapshot.blocks.push_back(demo);
        Refresh(&app);
        if (!app.dump_geometry.empty()) {
            g_timeout_add(200, [](gpointer data) -> gboolean {
                WriteGeometryDump(static_cast<App*>(data));
                return G_SOURCE_REMOVE;
            }, &app);
        }
        if (app.quit_after_dump) {
            g_timeout_add(2000, [](gpointer data) -> gboolean {
                WriteGeometryDump(static_cast<App*>(data));
                gtk_main_quit();
                return G_SOURCE_REMOVE;
            }, &app);
        }
        gtk_main();
        return 0;
    }

    if (!ConnectSocket(&app, socket_path)) {
        g_printerr("rimes-buffer: could not connect to %s\n", socket_path.c_str());
        return 1;
    }
    gtk_main();
    if (app.socket_fd >= 0) {
        close(app.socket_fd);
    }
    return 0;
}
