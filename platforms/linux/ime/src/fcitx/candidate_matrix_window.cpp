#include "candidate_matrix_window.hpp"
#include "session/protected_input.hpp"
#include <algorithm>
#include <cmath>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkwayland.h>
#ifdef HAVE_GTK_LAYER_SHELL
#include <gtk-layer-shell.h>
#endif
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/misc.h>

namespace fcitx {
class CandidateMatrixWindow::Impl {
public:
    explicit Impl(Instance* owner) : instance(owner) {}
    ~Impl() { timer.reset(); if (window) gtk_widget_destroy(window); if (xdisplay) { gdk_display_close(xdisplay); g_object_unref(xdisplay); } }
    bool ensure() {
        if (window) return true;
        if (!gtk_init_check(nullptr, nullptr)) return false;
        const bool wayland = GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default());
        bool layer = false;
#ifdef HAVE_GTK_LAYER_SHELL
        layer = wayland && gtk_layer_is_supported();
#endif
        // A Wayland popup needs a parent surface which a client-owned text field
        // cannot give us. Layer-shell provides a non-focusing surface instead.
        if (wayland && !layer) {
            // GNOME commonly supplies XWayland even though it has no layer-shell.
            // Open an X11 display explicitly without changing the host backend.
            const char* display = g_getenv("DISPLAY");
            if (!display || !*display) return false;
            gdk_set_allowed_backends("x11");
            xdisplay = gdk_display_open(display);
            gdk_set_allowed_backends("*");
            if (!xdisplay) return false;
        }
        window = gtk_window_new(layer ? GTK_WINDOW_TOPLEVEL : GTK_WINDOW_POPUP);
        g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
            auto* self = static_cast<Impl*>(data);
            self->window = nullptr; self->box = nullptr;
            self->queuedSelection.reset(); self->queuedMove = 0; self->numbers.clear();
            if (self->timer) self->timer->setEnabled(false);
        }), this);
        if (xdisplay) gtk_window_set_screen(GTK_WINDOW(window), gdk_display_get_default_screen(xdisplay));
        gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);
        gtk_window_set_focus_on_map(GTK_WINDOW(window), FALSE);
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
        gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
        gtk_widget_set_name(window, "xsime-candidate-matrix");
        box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        gtk_container_set_border_width(GTK_CONTAINER(box), 6);
        gtk_container_add(GTK_CONTAINER(window), box);
#ifdef HAVE_GTK_LAYER_SHELL
        if (layer) {
            gtk_layer_init_for_window(GTK_WINDOW(window));
            gtk_layer_set_namespace(GTK_WINDOW(window), "xsime-candidate-matrix");
            gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
            gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
            gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
            gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, 48);
        }
#endif
        gtk_widget_add_events(window, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
        g_signal_connect(window, "scroll-event", G_CALLBACK(+[](GtkWidget*, GdkEventScroll* e, gpointer data) -> gboolean {
            auto* self = static_cast<Impl*>(data);
            int delta = e->direction == GDK_SCROLL_UP ? -1 : e->direction == GDK_SCROLL_DOWN ? 1 : e->delta_y < 0 ? -1 : e->delta_y > 0 ? 1 : 0;
            // Defer out of GTK signal dispatch: navigation may replace the buttons.
            if (delta && self->move) self->queuedMove = delta;
            return TRUE;
        }), this);
        timer = instance->eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 16000, 0,
            [this](EventSourceTime* source, uint64_t) {
                for (int i = 0; i < 32 && g_main_context_pending(nullptr); ++i) g_main_context_iteration(nullptr, FALSE);
                auto chosen = std::exchange(queuedSelection, std::nullopt);
                auto delta = std::exchange(queuedMove, 0);
                auto choose = select;
                auto navigate = move;
                if (window && gtk_widget_get_visible(window)) {
                    source->setTime(now(CLOCK_MONOTONIC) + 16000); source->setOneShot();
                } else source->setEnabled(false);
                // A commit may synchronously destroy its input context and this
                // renderer. Do not access members after invoking the callback.
                if (chosen && choose) choose(*chosen);
                else if (delta && navigate) navigate(delta);
                return true;
            });
        timer->setOneShot();
        return true;
    }
    Instance* instance;
    GtkWidget* window = nullptr;
    GtkWidget* box = nullptr;
    GdkDisplay* xdisplay = nullptr;
    std::unique_ptr<EventSourceTime> timer;
    std::function<void(std::size_t)> select;
    std::function<void(int)> move;
    std::optional<std::size_t> queuedSelection;
    int queuedMove = 0;
    std::vector<std::size_t> numbers;
};
CandidateMatrixWindow::CandidateMatrixWindow(Instance* instance) : impl_(std::make_unique<Impl>(instance)) {}
CandidateMatrixWindow::~CandidateMatrixWindow() = default;
void CandidateMatrixWindow::hide() {
    impl_->select = {}; impl_->move = {}; impl_->queuedSelection.reset(); impl_->queuedMove = 0; impl_->numbers.clear();
    if (impl_->window) gtk_widget_hide(impl_->window);
    if (impl_->timer) impl_->timer->setEnabled(false);
}
std::optional<std::size_t> CandidateMatrixWindow::digit(std::size_t index) const {
    return index < impl_->numbers.size() ? std::optional<std::size_t>(impl_->numbers[index]) : std::nullopt;
}
bool CandidateMatrixWindow::show(InputContext* ic, const CandidateNavigation& nav,
    std::function<void(std::size_t)> select, std::function<void(int)> move) {
    if (!ic || !ic->hasFocus() || ProtectedInput(ic->capabilityFlags()) || !impl_->ensure()) { hide(); return false; }
    impl_->timer->setTime(now(CLOCK_MONOTONIC) + 16000); impl_->timer->setOneShot();
    impl_->queuedSelection.reset(); impl_->queuedMove = 0;
    impl_->select = std::move(select); impl_->move = std::move(move); impl_->numbers.clear();
    GList* old = gtk_container_get_children(GTK_CONTAINER(impl_->box));
    for (auto* child = old; child; child = child->next) gtk_widget_destroy(GTK_WIDGET(child->data));
    g_list_free(old);
    GdkRectangle work{0, 0, 1024, 768};
    auto* display = gtk_widget_get_display(impl_->window);
    const auto raw = ic->cursorRect();
    auto* monitor = gdk_display_get_primary_monitor(display);
    if (!monitor && gdk_display_get_n_monitors(display)) monitor = gdk_display_get_monitor(display, 0);
    const int scale = monitor ? std::max(1, gdk_monitor_get_scale_factor(monitor)) : 1;
    const bool global = GDK_IS_X11_DISPLAY(display) && ic->display().rfind("x11:", 0) == 0 &&
        !ic->capabilityFlags().test(CapabilityFlag::RelativeRect) &&
        std::abs(static_cast<std::int64_t>(raw.left())) < 1000000 && std::abs(static_cast<std::int64_t>(raw.top())) < 1000000 &&
        raw.width() >= 0 && raw.width() < 100000 && raw.height() >= 0 && raw.height() < 100000 &&
        (raw.left() || raw.top() || raw.width() || raw.height());
    GdkRectangle caret{static_cast<int>(std::floor(raw.left() / static_cast<double>(scale))),
        static_cast<int>(std::floor(raw.top() / static_cast<double>(scale))),
        static_cast<int>(std::ceil(raw.width() / static_cast<double>(scale))),
        static_cast<int>(std::ceil(raw.height() / static_cast<double>(scale)))};
    if (global) monitor = gdk_display_get_monitor_at_point(display, caret.x, caret.y);
    if (monitor) gdk_monitor_get_workarea(monitor, &work);
    const int available = std::max(80, std::min(640, work.width - 24));
    std::size_t flat = 0;
    for (auto row = nav.base(); row < nav.end(); ++row) {
        auto* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_box_pack_start(GTK_BOX(impl_->box), line, FALSE, FALSE, 0);
        const auto& page = nav.page(row);
        std::vector<GtkWidget*> buttons;
        std::vector<int> widths;
        for (std::size_t col = 0; col < page.candidates.size(); ++col) {
            const auto& word = page.candidates[col];
            auto* button = gtk_button_new();
            auto* label = gtk_label_new((std::to_string(col + 1) + " " + word.text + (word.comment.empty() ? "" : " " + word.comment)).c_str());
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_container_add(GTK_CONTAINER(button), label);
            gtk_widget_show(label);
            gtk_widget_set_can_focus(button, FALSE);
            gtk_widget_set_tooltip_text(button, word.text.c_str());
            int natural = 0; gtk_widget_get_preferred_width(button, nullptr, &natural);
            widths.push_back(std::min(available, std::max(36, natural)));
            buttons.push_back(button);
        }
        auto endFrom = [&](std::size_t start) {
            int width = 0; auto end = start;
            while (end < widths.size() && end - start < 9) {
                const int next = widths[end] + (end > start ? 4 : 0);
                if (end > start && width + next > available) break;
                width += next; ++end;
            }
            return end;
        };
        std::size_t start = 0;
        if (row == nav.row()) while (nav.column() >= endFrom(start) && start < nav.column()) ++start;
        const auto end = endFrom(start);
        for (std::size_t col = 0; col < buttons.size(); ++col) {
            auto* button = buttons[col];
            if (col < start || col >= end) { g_object_ref_sink(button); g_object_unref(button); continue; }
            auto* label = gtk_bin_get_child(GTK_BIN(button));
            const auto& word = page.candidates[col];
            gtk_label_set_text(GTK_LABEL(label), (std::to_string(col - start + 1) + " " + word.text + (word.comment.empty() ? "" : " " + word.comment)).c_str());
            gtk_widget_set_size_request(button, widths[col], 32);
            if (row == nav.row() && col == nav.column()) gtk_style_context_add_class(gtk_widget_get_style_context(button), "suggested-action");
            const auto index = flat + col;
            g_object_set_data(G_OBJECT(button), "candidate-index", GSIZE_TO_POINTER(index + 1));
            g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* selected, gpointer data) {
                static_cast<Impl*>(data)->queuedSelection = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(selected), "candidate-index")) - 1;
            }), impl_.get());
            gtk_box_pack_start(GTK_BOX(line), button, FALSE, FALSE, 0);
            if (row == nav.row()) impl_->numbers.push_back(index);
        }
        flat += page.candidates.size();
    }
    gtk_widget_show_all(impl_->window);
    GtkRequisition size{}; gtk_widget_get_preferred_size(impl_->window, nullptr, &size);
    int x = std::clamp(global ? caret.x : work.x + (work.width - size.width) / 2,
                       work.x, std::max(work.x, work.x + work.width - size.width));
    int y = global ? caret.y + caret.height + 6 : work.y + work.height - size.height - 48;
    if (global && y + size.height > work.y + work.height) y = caret.y - size.height - 6;
    y = std::clamp(y, work.y, std::max(work.y, work.y + work.height - size.height));
    if (GDK_IS_X11_DISPLAY(display)) gtk_window_move(GTK_WINDOW(impl_->window), x, y);
#ifdef HAVE_GTK_LAYER_SHELL
    else if (gtk_layer_is_layer_window(GTK_WINDOW(impl_->window)) && monitor)
        gtk_layer_set_monitor(GTK_WINDOW(impl_->window), monitor);
#endif
    return true;
}
}
