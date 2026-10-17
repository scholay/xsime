#include "panel_window.hpp"
#include <algorithm>
#include <cmath>
#include <gdk/gdkx.h>
#include <gdk/gdkwayland.h>
#ifdef HAVE_GTK_LAYER_SHELL
#include <gtk-layer-shell.h>
#endif

namespace rimes::desktop {
namespace {
std::string Text(const char* value) { return value ? value : ""; }
runtime::WindowRect Rectangle(const GdkRectangle& r) { return {r.x, r.y, r.width, r.height}; }
GdkDisplay* FallbackDisplay() {
    // Displays belong to GDK, not to an individual panel. Reuse the connection
    // so recreating a panel neither leaks connections nor tears down GTK's
    // display-scoped settings while other widgets still reference them.
    auto* displays = gdk_display_manager_list_displays(gdk_display_manager_get());
    GdkDisplay* found = nullptr;
    for (auto* at = displays; at; at = at->next) {
        auto* display = GDK_DISPLAY(at->data);
        if (GDK_IS_X11_DISPLAY(display) && !gdk_display_is_closed(display)) { found = display; break; }
    }
    g_slist_free(displays);
    if (found) return found;
    gdk_set_allowed_backends("x11");
    auto* display = gdk_display_open(g_getenv("DISPLAY"));
    gdk_set_allowed_backends("*");
    return display;
}
}
PanelWindow::PanelWindow(GtkWindow* window, std::string identity, int bottom_offset, bool persist)
    : window_(window), bottom_(bottom_offset) {
    g_object_ref(window_);
    auto* display = gtk_widget_get_display(GTK_WIDGET(window_));
    const bool wayland = GDK_IS_WAYLAND_DISPLAY(display);
#ifdef HAVE_GTK_LAYER_SHELL
    layer_ = wayland && gtk_layer_is_supported();
    if (layer_) {
        gtk_layer_init_for_window(window_);
        gtk_layer_set_namespace(window_, identity.c_str());
        gtk_layer_set_layer(window_, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_exclusive_zone(window_, 0);
        gtk_layer_set_anchor(window_, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(window_, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        // Only one edge per axis: anchoring both sides stretches the panel.
        gtk_layer_set_anchor(window_, GTK_LAYER_SHELL_EDGE_RIGHT, FALSE);
        gtk_layer_set_anchor(window_, GTK_LAYER_SHELL_EDGE_BOTTOM, FALSE);
    }
#endif
    if (wayland && !layer_ && g_getenv("DISPLAY")) {
        // A compositor without layer-shell cannot promise a non-activating
        // xdg_toplevel. Prefer its XWayland utility-window support when present.
        fallback_display_ = FallbackDisplay();
        if (fallback_display_) gtk_window_set_screen(window_, gdk_display_get_default_screen(fallback_display_));
    }
    display = gtk_widget_get_display(GTK_WIDGET(window_));
    display_ = display; g_object_ref(display_);
    x11_ = GDK_IS_X11_DISPLAY(display);
    screen_ = gtk_widget_get_screen(GTK_WIDGET(window_));
    g_object_ref(screen_);
    gtk_window_set_accept_focus(window_, FALSE);
    gtk_window_set_focus_on_map(window_, FALSE);
    gtk_window_set_keep_above(window_, TRUE);
    gtk_window_set_skip_taskbar_hint(window_, TRUE);
    gtk_window_set_skip_pager_hint(window_, TRUE);
    gtk_window_set_type_hint(window_, GDK_WINDOW_TYPE_HINT_UTILITY);
    g_signal_connect(window_, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
        auto* self = static_cast<PanelWindow*>(data);
        // gtk-layer-shell < 0.10 forwards an output's closed event as GTK
        // delete-event. Keep the content alive so monitor removal can remap it.
        if (!self->layer_) return FALSE;
        self->topology_changed_ = true; self->QueueReposition();
        return TRUE;
    }), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
        auto* self = static_cast<PanelWindow*>(data); self->destroyed_ = true;
        if (self->pending_) { g_source_remove(self->pending_); self->pending_ = 0; }
    }), this);
    for (const auto* signal : {"monitors-changed", "size-changed"})
        g_signal_connect(screen_, signal, G_CALLBACK(+[](GdkScreen*, gpointer data) {
            auto* self = static_cast<PanelWindow*>(data); self->topology_changed_ = true; self->QueueReposition();
        }), this);
    for (const auto* signal : {"monitor-added", "monitor-removed"})
        g_signal_connect(display_, signal, G_CALLBACK(+[](GdkDisplay*, GdkMonitor*, gpointer data) {
            auto* self = static_cast<PanelWindow*>(data); self->topology_changed_ = true; self->QueueReposition();
        }), this);
    g_signal_connect(window_, "notify::scale-factor", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        auto* self = static_cast<PanelWindow*>(data); self->topology_changed_ = true; self->QueueReposition();
    }), this);
    WatchMonitors();
    if (persist) try {
        store_ = std::make_unique<runtime::WindowPositionStore>(runtime::UserRoot() / "windows", identity);
        saved_ = store_->Load();
    } catch (const std::exception&) { g_warning("XSIME window position could not be restored; existing file retained"); }
}
PanelWindow::~PanelWindow() {
    if (pending_) g_source_remove(pending_);
    g_signal_handlers_disconnect_by_data(window_, this);
    g_signal_handlers_disconnect_by_data(screen_, this);
    g_signal_handlers_disconnect_by_data(display_, this);
    for (auto* monitor : watched_monitors_) { g_signal_handlers_disconnect_by_data(monitor, this); g_object_unref(monitor); }
    g_object_unref(window_); g_object_unref(screen_); g_object_unref(display_);
    // display_ is our one owned reference. fallback_display_ is borrowed from
    // GDK's display manager and may also be used by another panel.
}
std::string PanelWindow::Backend() const {
    if (layer_) return "Wayland layer-shell";
    if (fallback_display_) return "XWayland 浮窗（当前桌面不提供 layer-shell）";
    if (x11_) return "X11";
    return "桌面管理的窗口：无法固定位置；切回输入框后重新唤出以恢复捕获";
}
std::vector<PanelWindow::Monitor> PanelWindow::Monitors() const {
    auto* display = gtk_widget_get_display(GTK_WIDGET(window_));
    std::vector<Monitor> result;
    for (int index = 0; index < gdk_display_get_n_monitors(display); ++index) {
        auto* monitor = gdk_display_get_monitor(display, index);
        GdkRectangle work{}, bounds{};
        gdk_monitor_get_workarea(monitor, &work); gdk_monitor_get_geometry(monitor, &bounds);
        const auto identity = Text(gdk_monitor_get_manufacturer(monitor)) + ":" + Text(gdk_monitor_get_model(monitor)) + ":" +
            std::to_string(gdk_monitor_get_width_mm(monitor)) + "x" + std::to_string(gdk_monitor_get_height_mm(monitor));
        result.push_back({monitor, {runtime::Sha256(identity), Rectangle(work)}, Rectangle(bounds)});
    }
    // Identical monitors are distinguishable only by their current arrangement
    // through GDK. Keep their positions distinct, then clamp after rearranging.
    for (std::size_t i = 0; i < result.size(); ++i) {
        bool duplicate = false;
        for (std::size_t j = i + 1; j < result.size(); ++j) if (result[i].area.id == result[j].area.id) duplicate = true;
        if (duplicate) {
            const auto identity = result[i].area.id;
            for (std::size_t j = i; j < result.size(); ++j) if (result[j].area.id == identity)
                result[j].area.id += ":" + std::to_string(result[j].bounds.x) + ":" + std::to_string(result[j].bounds.y);
        }
    }
    return result;
}
std::vector<std::string> PanelWindow::MonitorNames() const {
    std::vector<std::string> names;
    for (const auto& monitor : Monitors()) {
        const auto title = Text(gdk_monitor_get_model(monitor.native));
        names.push_back(std::to_string(names.size() + 1) + " · " + (title.empty() ? "显示器" : title));
    }
    return names;
}
void PanelWindow::Apply(Rect frame, const Monitor& monitor) {
    frame = runtime::ConstrainWindow(frame, monitor.area.work);
    gtk_widget_set_size_request(GTK_WIDGET(window_), frame.width, frame.height);
    gtk_window_resize(window_, frame.width, frame.height);
#ifdef HAVE_GTK_LAYER_SHELL
    if (layer_) {
        gtk_layer_set_monitor(window_, monitor.native);
        gtk_layer_set_margin(window_, GTK_LAYER_SHELL_EDGE_LEFT, frame.x - monitor.bounds.x);
        gtk_layer_set_margin(window_, GTK_LAYER_SHELL_EDGE_TOP, frame.y - monitor.bounds.y);
    }
#endif
    if (x11_) gtk_window_move(window_, frame.x, frame.y);
    frame_ = frame; work_ = monitor.area.work; monitor_id_ = monitor.area.id; positioned_ = true;
}
PanelWindow::Rect PanelWindow::Place(int width, int height, std::optional<Rect> physical_caret, bool opening, Placement placement) {
    if (destroyed_) return frame_;
    const bool resize = width_ != width || height_ != height;
    width_ = width; height_ = height; caret_ = physical_caret; placement_ = std::move(placement);
    const auto monitors = Monitors();
    if (monitors.empty()) return frame_;
    if (positioned_ && !opening && !resize && !topology_changed_) return Frame();
    auto* display = gtk_widget_get_display(GTK_WIDGET(window_));
    auto* preferred = gdk_display_get_primary_monitor(display);
    if (!preferred) preferred = monitors.front().native;
    auto selected = std::find_if(monitors.begin(), monitors.end(), [&](const auto& m) { return m.native == preferred; });
    if (selected == monitors.end()) selected = monitors.begin();
    auto select_id = [&](const std::string& id) {
        const auto found = std::find_if(monitors.begin(), monitors.end(), [&](const auto& m) { return m.area.id == id; });
        if (found != monitors.end()) { selected = found; return true; }
        return false;
    };
    std::optional<Rect> logical_caret;
    if (x11_ && physical_caret && placement_) {
        // Fcitx global X11 rectangles are physical root pixels. GDK exposes
        // application pixels; its X11 display scale is independent of the
        // input client's (possibly different) toolkit scale.
        const int scale = std::max(1, gdk_monitor_get_scale_factor(preferred));
        const auto& c = *physical_caret;
        logical_caret = Rect{static_cast<int>(std::floor(c.x / static_cast<double>(scale))),
            static_cast<int>(std::floor(c.y / static_cast<double>(scale))),
            static_cast<int>(std::ceil(c.width / static_cast<double>(scale))),
            static_cast<int>(std::ceil(c.height / static_cast<double>(scale)))};
    }
    Rect next;
    if (opening && logical_caret) {
        const auto& c = *logical_caret;
        auto* near = gdk_display_get_monitor_at_point(display, c.x, c.y);
        const auto found = std::find_if(monitors.begin(), monitors.end(), [&](const auto& m) { return m.native == near; });
        if (found != monitors.end()) selected = found;
        next = placement_(selected->area.work, c);
    } else if (positioned_ && !opening && !topology_changed_ && select_id(monitor_id_)) {
        const auto current = Frame();
        next = {current.x + (current.width - width) / 2, current.y + current.height - height, width, height};
    } else if (saved_) {
        select_id(saved_->monitor);
        next = runtime::RestoreWindow(*saved_, selected->area.work, width, height);
    } else {
        select_id(monitor_id_);
        next = runtime::BottomWindow(selected->area.work, width, height, bottom_);
    }
    next.width = width; next.height = height;
    Apply(next, *selected); topology_changed_ = false;
    return frame_;
}
PanelWindow::Rect PanelWindow::Frame() const {
    Rect result = frame_;
    if (x11_ && positioned_ && !destroyed_) {
        gtk_window_get_position(window_, &result.x, &result.y);
        gtk_window_get_size(window_, &result.width, &result.height);
    }
    return result;
}
void PanelWindow::Move(Rect frame) {
    if (!CanPosition() || destroyed_) return;
    const auto monitors = Monitors();
    if (monitors.empty()) return;
    std::vector<runtime::MonitorArea> areas; for (const auto& m : monitors) areas.push_back(m.area);
    Apply(frame, monitors[runtime::BestMonitor(areas, frame)]);
}
void PanelWindow::Remember() {
    if (!CanPosition() || destroyed_ || !positioned_) return;
    const auto monitors = Monitors(); if (monitors.empty()) return;
    std::vector<runtime::MonitorArea> areas; for (const auto& m : monitors) areas.push_back(m.area);
    const auto frame = Frame();
    const auto index = runtime::BestMonitor(areas, frame);
    saved_ = runtime::RememberWindow(frame, areas[index]); monitor_id_ = saved_->monitor;
    try { if (store_) store_->Save(*saved_); }
    catch (const std::exception&) { g_warning("XSIME window position could not be saved"); }
}
void PanelWindow::SelectMonitor(std::size_t index) {
    const auto monitors = Monitors();
    if (index >= monitors.size() || !CanPosition()) return;
    Apply(runtime::BottomWindow(monitors[index].area.work, width_, height_, bottom_), monitors[index]);
    Remember();
}
void PanelWindow::Reset() {
    saved_.reset(); positioned_ = false;
    Place(width_, height_, caret_, true, placement_);
    Remember();
}
void PanelWindow::QueueReposition() {
    if (pending_ || destroyed_) return;
    if (positioned_) saved_ = runtime::RememberWindow(Frame(), {monitor_id_, work_});
    pending_ = g_idle_add(+[](gpointer data) -> gboolean {
        auto* self = static_cast<PanelWindow*>(data); self->pending_ = 0;
        self->WatchMonitors();
        if (gtk_widget_get_visible(GTK_WIDGET(self->window_)))
            self->Place(self->width_, self->height_, self->caret_, false, self->placement_);
        return G_SOURCE_REMOVE;
    }, this);
}
void PanelWindow::WatchMonitors() {
    for (auto* monitor : watched_monitors_) { g_signal_handlers_disconnect_by_data(monitor, this); g_object_unref(monitor); }
    watched_monitors_.clear();
    for (const auto& monitor : Monitors()) {
        g_object_ref(monitor.native); watched_monitors_.push_back(monitor.native);
        for (const auto* signal : {"notify::workarea", "notify::geometry", "notify::scale-factor"})
            g_signal_connect(monitor.native, signal, G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                auto* self = static_cast<PanelWindow*>(data); self->topology_changed_ = true; self->QueueReposition();
            }), this);
    }
}
} // namespace rimes::desktop
