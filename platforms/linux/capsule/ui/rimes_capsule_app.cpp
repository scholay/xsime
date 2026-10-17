#include "../../product_brand.hpp"
#include "background_jobs.hpp"
#include "buffer_protocol.hpp"
#include "capsule_click.hpp"
#include "capsule_capture.hpp"
#include "capsule_resources.hpp"
#include "capture_controller.hpp"
#include "capsule_clipboard.hpp"
#include "capsule_kinds.hpp"
#include "capsule_manager.hpp"
#include "capsule_protocol.hpp"
#include "capsule_text.hpp"
#include "capsule_transport.hpp"
#include "clipboard_controller.hpp"
#include "clipboard_preview.hpp"
#include "desktop.hpp"
#include "official_catalog.hpp"
#include "panel_window.hpp"

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

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr int kWindowWidth = rimes::capsule::kRailWidth;
constexpr int kWindowHeight = rimes::capsule::kRailHeight;

struct App {
    GtkWidget* window = nullptr;
    GtkWidget* tab_box = nullptr;
    GtkWidget* filter_box = nullptr;
    std::unique_ptr<rimes::capsule::ManagerWindow> manager;
    std::unique_ptr<rimes::capsule::ClipboardController> clipboard;
    std::unique_ptr<rimes::capsule::CaptureController> capture;
    GtkWidget* count = nullptr;
    GtkWidget* card_box = nullptr;
    GtkWidget*card_scroll = nullptr, *detail_box = nullptr;
    GtkWidget* hint = nullptr;
    GtkWidget* query = nullptr;
    GtkWidget* clear_history = nullptr;
    GtkWidget* capture_button = nullptr;
    GtkWidget* search_shell = nullptr;
    GtkWidget* history_menu = nullptr;
    std::string scrolled_selection;
    bool clear_armed = false, caret_visible = true;
    guint clear_timer = 0, caret_timer = 0;
    rimes::runtime::BackgroundJobs thumbnails;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<std::uint64_t> thumbnail_epoch{0};
    std::map<std::string, std::shared_ptr<GdkPixbuf>> thumbnail_cache;
    std::set<std::string> thumbnail_pending;
    ~App() {
        if (clear_timer)
            g_source_remove(clear_timer);
        if (caret_timer)
            g_source_remove(caret_timer);
        alive->store(false);
        thumbnail_epoch.fetch_add(1);
        thumbnails.Shutdown();
        capture.reset();clipboard.reset();
    }
    int socket_fd = -1;
    guint io_id = 0, reconnect_id = 0;
    std::string socket_path;
    std::string incoming;
    std::string dump_geometry;
    bool quit_after_dump = false;
    bool dumped_geometry = false;
    rimes::capsule::Snapshot snapshot;
    bool wayland_layer = false;
    bool preview = false;
    std::string preview_image;
    bool placed = false;
    std::string theme;
    std::unique_ptr<rimes::desktop::PanelWindow> panel;
    struct CardRow {
        std::string id;
        GtkWidget* event = nullptr;
        GtkWidget* title = nullptr;
        GtkWidget* preview = nullptr;
        GtkWidget *picture = nullptr, *copy = nullptr, *menu = nullptr, *marker = nullptr;
        std::shared_ptr<GdkPixbuf> thumbnail;
    };
    std::vector<CardRow> card_rows;
    int last_press_index = -1;
    std::int64_t last_press_ms = -1;
    bool seen_copy_seq = false;
    gint64 detail_copy_until = 0;
    int detail_copy_index = -1;
    std::string detail_copy_id;
    guint detail_copy_timer = 0;
    std::uint64_t handled_copy_seq = 0;
};

App* g_app = nullptr;

std::string DefaultSocketPath() {
    if (const char* override_path = g_getenv("RIMES_CAPSULE_SOCKET")) {
        if (override_path[0] != '\0') {
            return override_path;
        }
    }
    if (const char* runtime = g_getenv("XDG_RUNTIME_DIR")) {
        return std::string(runtime) + "/rimes-capsule.sock";
    }
    return "/tmp/rimes-capsule-" + std::to_string(geteuid()) + ".sock";
}

bool SendCommand(App* app, const std::string& json) {
    std::string frame;
    if (app->socket_fd < 0 || !rimes::capsule::EncodeFrame(json, &frame)) {
        return false;
    }
    return send(app->socket_fd, frame.data(), frame.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(frame.size());
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
    std::fprintf(file,
                 "{\"width\":%d,\"height\":%d,\"size_request_w\":%d,\"size_request_h\":%d,"
                 "\"layer\":%s,\"min_width\":%d,\"min_height\":%d}\n",
                 allocation.width, allocation.height, request_w, request_h,
                 app->wayland_layer ? "true" : "false", kWindowWidth, kWindowHeight);
    std::fclose(file);
    app->dumped_geometry = true;
    if (app->quit_after_dump) {
        gtk_main_quit();
    }
}

void OnSizeAllocate(GtkWidget* /*widget*/, GdkRectangle* allocation, gpointer data) {
    if (allocation == nullptr || allocation->width < 2 || allocation->height < 2) {
        return;
    }
    WriteGeometryDump(static_cast<App*>(data));
}

void ClearBox(GtkWidget* box) {
    GList* children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList* item = children; item != nullptr; item = item->next) {
        gtk_widget_destroy(GTK_WIDGET(item->data));
    }
    g_list_free(children);
}

void RebuildTabs(App* app) {
    ClearBox(app->tab_box); ClearBox(app->filter_box);
    for (auto module : app->snapshot.modules) {
        const auto& definition = rimes::capsule::ModuleDefinition(module);
        auto* button = rimes::desktop::Button(definition.at("title"), [app, module] {
            SendCommand(app, rimes::desktop::Json{{"v",1},{"op","module"},{"text",rimes::capsule::ModuleRaw(module)}}.dump());
        }, "rimes-tab");
        if (rimes::capsule::ModuleRaw(module) == app->snapshot.module) rimes::desktop::Class(button,"rimes-tab-active");
        gtk_widget_set_size_request(button, module == rimes::capsule::Module::Passwords ? 50 : 42,
                                    22);
        if (module == rimes::capsule::Module::Passwords) {
            gtk_button_set_image(GTK_BUTTON(button), rimes::desktop::Icon("lock", 9));
            gtk_button_set_always_show_image(GTK_BUTTON(button), TRUE);
        }
        gtk_widget_set_can_focus(button,FALSE);
        gtk_box_pack_start(GTK_BOX(app->tab_box),button,FALSE,FALSE,0);
    }
    if (const auto module = rimes::capsule::ModuleFromRaw(app->snapshot.module)) {
        for (const auto& filter : rimes::capsule::ModuleDefinition(*module).at("filters")) {
            auto* button = rimes::desktop::Button(filter.at("title"), [app, id = filter.at("id").get<std::string>()] {
                SendCommand(app, rimes::desktop::Json{{"v",1},{"op","filter"},{"text",id}}.dump());
            }, "rimes-filter");
            if (filter.at("id") == app->snapshot.filter) rimes::desktop::Class(button,"selected");
            gtk_widget_set_size_request(button,60,18); gtk_widget_set_can_focus(button,FALSE);
            gtk_box_pack_start(GTK_BOX(app->filter_box),button,FALSE,FALSE,0);
        }
    }
    gtk_widget_show_all(app->tab_box); gtk_widget_show_all(app->filter_box);
}

void ShowHistoryMenu(App* app, int index) {
    if (app->snapshot.password_field || !app->snapshot.visible || index < 0 ||
        index >= static_cast<int>(app->snapshot.cards.size()))
        return;
    const auto card = app->snapshot.cards[index];
    if (card.module != rimes::capsule::Module::Temporary && card.module != rimes::capsule::Module::Capture)
        return;
    if (app->history_menu)
        gtk_widget_destroy(app->history_menu);
    auto* menu = gtk_menu_new();
    app->history_menu = menu;
    g_signal_connect(menu, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer raw) {
                         static_cast<App*>(raw)->history_menu = nullptr;
                     }),
                     app);
    for (const auto& action : std::vector<std::pair<const char*, std::string>>{
             {"复制", "copy"},
             {card.module == rimes::capsule::Module::Capture ? (card.favorite ? "已收藏到 Capsule" : "收藏到 Capsule") : (card.favorite ? "取消收藏" : "收藏"), "favorite"},
             {"删除", "remove"}}) {
        auto* item = gtk_menu_item_new_with_label(action.first);
        if (card.module == rimes::capsule::Module::Capture && card.favorite && action.second != "copy")
            gtk_widget_set_sensitive(item, FALSE);
        auto* callback = new std::function<void()>([app, card, op = action.second,
                                                    life = app->alive] {
            if (life->load() && app->snapshot.visible && !app->snapshot.password_field)
                SendCommand(app,
                            rimes::desktop::Json{
                                {"v", 1}, {"op", op}, {"id", card.id}, {"revision", card.revision}}
                                .dump());
        });
        g_signal_connect_data(
            item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data) {
                (*static_cast<std::function<void()>*>(data))();
            }),
            callback,
            +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); },
            GConnectFlags(0));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    g_signal_connect(menu, "selection-done", G_CALLBACK(+[](GtkMenuShell* menu, gpointer) {
                         gtk_widget_destroy(GTK_WIDGET(menu));
                     }),
                     nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), nullptr);
}
void RequestThumbnails(App* app);

gboolean OnCardButtonPress(GtkWidget* self, GdkEventButton* ev, gpointer data) {
    auto* host = static_cast<App*>(data);
    const int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(self), "index"));
    if (ev && ev->button == 3) {
        ShowHistoryMenu(host, index);
        return TRUE;
    }
    if (ev && ev->button != 1)
        return FALSE;
    const std::int64_t now_ms = g_get_monotonic_time() / 1000;
    const bool gdk_double = ev != nullptr && ev->type == GDK_2BUTTON_PRESS;
    const bool activate = rimes::capsule::ShouldActivateOnPress(
        index, host->last_press_index, now_ms, host->last_press_ms, gdk_double);
    const std::string select =
        rimes::desktop::Json{{"v",1},{"op","select"},{"id",host->snapshot.cards.at(index).id}}.dump();
    SendCommand(host, select);
    if (activate) {
        SendCommand(host, rimes::desktop::Json{{"v",1},{"op", "detail"},{"id",host->snapshot.cards.at(index).id}}.dump());
    }
    host->last_press_index = index;
    host->last_press_ms = now_ms;
    return TRUE;
}

void SetCardSelectedStyle(GtkWidget* event, bool selected) {
    GtkStyleContext* context = gtk_widget_get_style_context(event);
    if (selected) {
        gtk_style_context_add_class(context, "rimes-card-selected");
    } else {
        gtk_style_context_remove_class(context, "rimes-card-selected");
    }
}

bool CardsMatchIds(const App* app) {
    if (app->card_rows.size() != app->snapshot.cards.size()) {
        return false;
    }
    for (std::size_t i = 0; i < app->card_rows.size(); ++i) {
        if (app->card_rows[i].id != app->snapshot.cards[i].id) {
            return false;
        }
    }
    return true;
}

void UpdateCardsInPlace(App* app) {
    for (int i = 0; i < static_cast<int>(app->card_rows.size()); ++i) {
        auto& row = app->card_rows[static_cast<std::size_t>(i)];
        const auto& card = app->snapshot.cards[static_cast<std::size_t>(i)];
        gtk_label_set_text(
            GTK_LABEL(row.title),
            (card.module == rimes::capsule::Module::Temporary ? card.source : card.module == rimes::capsule::Module::Capture ? rimes::capsule::CharacterPrefix(card.title,25) : card.title).c_str());
        if (row.menu)
            gtk_widget_set_visible(row.menu, i == app->snapshot.selected);
        if (row.marker)
            gtk_widget_set_visible(row.marker, card.favorite && card.module != rimes::capsule::Module::Capture);
        gtk_label_set_text(GTK_LABEL(row.preview), card.preview.c_str());
        SetCardSelectedStyle(row.event, i == app->snapshot.selected);
        g_object_set_data(G_OBJECT(row.event), "index", GINT_TO_POINTER(i));
    }
}

App::CardRow MakeCardRow(App* app, const rimes::capsule::Card& card, int index) {
    App::CardRow row;
    row.id = card.id;
    row.event = gtk_event_box_new();
    gtk_widget_set_name(row.event, "rimes-card");
    SetCardSelectedStyle(row.event, index == app->snapshot.selected);
    if (card.module == rimes::capsule::Module::Temporary || card.module == rimes::capsule::Module::Capture) {
        const bool capture = card.module == rimes::capsule::Module::Capture;
        using namespace rimes::desktop;
        Class(row.event, "clipboard-card");
        if (capture) Class(row.event, "capture-card");
        auto* fixed = gtk_fixed_new();
        gtk_container_add(GTK_CONTAINER(row.event), fixed);
        auto place = [&](GtkWidget* widget, int x, int y, int w, int h) {
            gtk_fixed_put(GTK_FIXED(fixed), widget, x, y);
            gtk_widget_set_size_request(widget, w, h);
        };
        auto* quick = Label(index < 9 ? "⌃" + std::to_string(index + 1) : "", "clipboard-quick");
        place(quick, 10, 9, 28, 15);
        gtk_widget_set_no_show_all(quick, capture);
        auto* source_icon =
            gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_MENU);
        const auto app_icon = rimes::capsule::LowerText(card.source);
        if (gtk_icon_theme_has_icon(
                gtk_icon_theme_get_for_screen(gtk_widget_get_screen(app->window)),
                app_icon.c_str()))
            gtk_image_set_from_icon_name(GTK_IMAGE(source_icon), app_icon.c_str(),
                                         GTK_ICON_SIZE_MENU);
        place(source_icon, 43, 8, 16, 16);
        gtk_widget_set_no_show_all(source_icon, capture);
        row.title = Label(capture ? rimes::capsule::CharacterPrefix(card.title,25) : card.source, "clipboard-source");
        gtk_label_set_xalign(GTK_LABEL(row.title), 0);
        gtk_label_set_ellipsize(GTK_LABEL(row.title), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(row.title), 1);
        if (capture) place(row.title, 8, 83, 190, 14);
        else place(row.title, 64, 9, 73, 15);
        row.preview = Label(card.preview, "clipboard-preview");
        gtk_label_set_xalign(GTK_LABEL(row.preview), 0);
        gtk_label_set_yalign(GTK_LABEL(row.preview), 0);
        gtk_label_set_line_wrap(GTK_LABEL(row.preview), TRUE);
        gtk_label_set_lines(GTK_LABEL(row.preview), capture ? 1 : 4);
        gtk_label_set_max_width_chars(GTK_LABEL(row.preview), 1);
        gtk_label_set_ellipsize(GTK_LABEL(row.preview), PANGO_ELLIPSIZE_END);
        if (capture) place(row.preview, 8, 100, 190, 14);
        else place(row.preview, 11, 34, 184, 83);
        gtk_widget_show(row.preview);
        gtk_widget_set_no_show_all(row.preview, TRUE);
        row.picture = Class(gtk_drawing_area_new(), "clipboard-picture");
        if (capture) place(row.picture, 8, 8, 186, 72);
        else place(row.picture, 11, 34, 184, 83);
        g_object_set_data(G_OBJECT(row.picture), "capture", GINT_TO_POINTER(capture));
        gtk_widget_set_no_show_all(row.picture, TRUE);
        auto* identity = new std::pair<App*, std::string>(app, card.id);
        g_signal_connect_data(
            row.picture, "draw",
            G_CALLBACK((+[](GtkWidget* widget, cairo_t* cr, gpointer raw) -> gboolean {
                auto* identity = static_cast<std::pair<App*, std::string>*>(raw);
                const auto found = std::find_if(
                    identity->first->card_rows.begin(), identity->first->card_rows.end(),
                    [&](const auto& row) { return row.id == identity->second; });
                if (found == identity->first->card_rows.end() || !found->thumbnail)
                    return FALSE;
                auto* image = found->thumbnail.get();
                const double w = gdk_pixbuf_get_width(image);
                const double h = gdk_pixbuf_get_height(image);
                const auto scale_x = gtk_widget_get_allocated_width(widget) / w;
                const auto scale_y = gtk_widget_get_allocated_height(widget) / h;
                const double scale = g_object_get_data(G_OBJECT(widget), "capture")
                    ? std::max(scale_x, scale_y) : std::min(scale_x, scale_y);
                gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0,
                                      gtk_widget_get_allocated_width(widget),
                                      gtk_widget_get_allocated_height(widget));
                cairo_save(cr);
                const double width = gtk_widget_get_allocated_width(widget);
                const double height = gtk_widget_get_allocated_height(widget);
                if (g_object_get_data(G_OBJECT(widget), "capture")) {
                    constexpr double pi = 3.14159265358979323846;
                    cairo_arc(cr, width - 6, 6, 6, -pi / 2, 0);
                    cairo_arc(cr, width - 6, height - 6, 6, 0, pi / 2);
                    cairo_arc(cr, 6, height - 6, 6, pi / 2, pi);
                    cairo_arc(cr, 6, 6, 6, pi, 3 * pi / 2);
                    cairo_close_path(cr);
                } else cairo_rectangle(cr, 0, 0, width, height);
                cairo_clip(cr);
                cairo_translate(cr, (gtk_widget_get_allocated_width(widget) - w * scale) / 2,
                                (gtk_widget_get_allocated_height(widget) - h * scale) / 2);
                cairo_scale(cr, scale, scale);
                gdk_cairo_set_source_pixbuf(cr, image, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
                return FALSE;
            })),
            identity,
            +[](gpointer raw, GClosure*) {
                delete static_cast<std::pair<App*, std::string>*>(raw);
            },
            GConnectFlags(0));
        row.menu = IconButton("ellipsis", "更多操作", [app, id = card.id] {
            for (std::size_t i = 0; i < app->snapshot.cards.size(); ++i)
                if (app->snapshot.cards[i].id == id) {
                    ShowHistoryMenu(app, i);
                    break;
                }
        });
        if (capture) {
            auto* icon = gtk_bin_get_child(GTK_BIN(row.menu));
            gtk_widget_set_size_request(icon, 11, 11);
            place(row.menu, 176, 6, 24, 24);
        }
        else place(row.menu, 174, 5, 22, 22);
        gtk_widget_show_all(row.menu);
        gtk_widget_set_no_show_all(row.menu, TRUE);
        gtk_widget_set_visible(row.menu, index == app->snapshot.selected);
        row.copy = IconButton("doc.on.doc", "复制", [app, id = card.id] {
            if (!app->snapshot.visible || app->snapshot.password_field)
                return;
            for (const auto& current : app->snapshot.cards)
                if (current.id == id) {
                    SendCommand(
                        app,
                        Json{{"v", 1}, {"op", "copy"}, {"id", id}, {"revision", current.revision}}
                            .dump());
                    break;
                }
        });
        if (capture) {
            gtk_widget_set_size_request(gtk_bin_get_child(GTK_BIN(row.copy)), 11, 11);
            place(row.copy, 176, 96, 24, 24);
        } else place(row.copy, 174, 97, 22, 22);
        gtk_widget_show_all(row.copy);
        gtk_widget_hide(row.copy);
        gtk_widget_set_no_show_all(row.copy, TRUE);
        row.marker = Label("★", "clipboard-quick");
        place(row.marker, 154, 10, 13, 13);
        gtk_widget_set_no_show_all(row.marker, TRUE);
        gtk_widget_set_visible(row.marker, card.favorite && card.module != rimes::capsule::Module::Capture);
        gtk_widget_add_events(row.event, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
        g_object_set_data(G_OBJECT(row.event), "copy", row.copy);
        g_signal_connect(
            row.event, "enter-notify-event",
            G_CALLBACK(+[](GtkWidget* widget, GdkEventCrossing*, gpointer) -> gboolean {
                gtk_widget_show(GTK_WIDGET(g_object_get_data(G_OBJECT(widget), "copy")));
                return FALSE;
            }),
            nullptr);
        g_signal_connect(
            row.event, "leave-notify-event",
            G_CALLBACK(+[](GtkWidget* widget, GdkEventCrossing* event, gpointer) -> gboolean {
                if (event->detail != GDK_NOTIFY_INFERIOR)
                    gtk_widget_hide(GTK_WIDGET(g_object_get_data(G_OBJECT(widget), "copy")));
                return FALSE;
            }),
            nullptr);
        gtk_widget_set_size_request(row.event, 206, 126);
        g_object_set_data(G_OBJECT(row.event), "index", GINT_TO_POINTER(index));
        g_signal_connect(row.event, "button-press-event", G_CALLBACK(OnCardButtonPress), app);
        gtk_box_pack_start(GTK_BOX(app->card_box), row.event, FALSE, FALSE, 0);
        return row;
    }
    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(column), 8);
    gtk_container_add(GTK_CONTAINER(row.event), column);
    row.title = gtk_label_new(card.title.c_str());
    gtk_widget_set_name(row.title, "rimes-card-title");
    gtk_label_set_xalign(GTK_LABEL(row.title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(row.title), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(column), row.title, FALSE, FALSE, 0);
    row.preview = gtk_label_new(card.preview.c_str());
    gtk_widget_set_name(row.preview, "rimes-card-preview");
    gtk_label_set_xalign(GTK_LABEL(row.preview), 0);
    gtk_label_set_line_wrap(GTK_LABEL(row.preview), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(row.preview), 28);
    gtk_label_set_lines(GTK_LABEL(row.preview),4);
    gtk_label_set_ellipsize(GTK_LABEL(row.preview),PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(column), row.preview, TRUE, TRUE, 0);
    gtk_widget_set_size_request(row.event, 206, 126);
    g_object_set_data(G_OBJECT(row.event), "index", GINT_TO_POINTER(index));
    g_signal_connect(row.event, "button-press-event", G_CALLBACK(OnCardButtonPress), app);
    gtk_box_pack_start(GTK_BOX(app->card_box), row.event, FALSE, FALSE, 0);
    return row;
}

void RequestThumbnails(App* app) {
    if ((app->snapshot.module != "temporary" && app->snapshot.module != "capture") || !app->snapshot.visible ||
        app->snapshot.password_field)
        return;
    auto* scroll = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(app->card_scroll));
    const int start = std::max(0, static_cast<int>(gtk_adjustment_get_value(scroll) / 214) - 1);
    for (int i = start; i < std::min(start + 6, static_cast<int>(app->snapshot.cards.size()));
         ++i) {
        const auto card = app->snapshot.cards[i];
        if (card.kind != rimes::capsule::Kind::Image && card.kind != rimes::capsule::Kind::Video)
            continue;
        const auto key = card.id + card.revision;
        auto apply = [app, card, key](std::shared_ptr<GdkPixbuf> image) {
            if (!app->snapshot.visible || app->snapshot.password_field ||
                std::none_of(app->snapshot.cards.begin(), app->snapshot.cards.end(),
                    [&](const auto& current) { return current.id == card.id && current.revision == card.revision && current.module == card.module; }))
                return;
            app->thumbnail_cache[key] = image;
            while (app->thumbnail_cache.size() > 32)
                app->thumbnail_cache.erase(app->thumbnail_cache.begin());
            for (auto& row : app->card_rows)
                if (row.id == card.id && row.picture) {
                    row.thumbnail = image;
                    gtk_widget_set_visible(row.preview, card.module == rimes::capsule::Module::Capture || !image);
                    gtk_widget_set_visible(row.picture, bool(image));
                    gtk_widget_queue_draw(row.picture);
                }
        };
        if (app->thumbnail_cache.count(key)) {
            apply(app->thumbnail_cache.at(key));
            continue;
        }
        if (app->preview) {
            auto* image = gdk_pixbuf_new_from_file_at_scale(app->preview_image.empty() ? "Logo/AppIcon.iconset/icon_256x256.png" : app->preview_image.c_str(),
                                                            512, 512, TRUE, nullptr);
            apply({image, [](GdkPixbuf* value) {
                       if (value)
                           g_object_unref(value);
                   }});
            continue;
        }
        if (!app->thumbnail_pending.insert(key).second)
            continue;
        const auto epoch = app->thumbnail_epoch.load();
        auto life = app->alive;
        if (!app->thumbnails.Run(std::make_shared<std::atomic<bool>>(false), [app, life, epoch,
                                                                              card, key, apply] {
                std::shared_ptr<GdkPixbuf> image;
                try {
                    if (!life->load() || app->thumbnail_epoch.load() != epoch)
                        return;
                    rimes::plugins::PackageStore packages(
                        rimes::runtime::UserRoot() / "plugins", rimes::plugins::OfficialCatalog(),
                        rimes::plugins::BundledPackages(), rimes::plugins::CompiledAdapters(),
                        RIMES_PRODUCT_VERSION);
                    const auto grant = packages.Inspect(rimes::capsule::ModulePlugin(card.module));
                    if (!grant.enabled) throw std::runtime_error("插件已停用");
                    if (card.module == rimes::capsule::Module::Temporary) {
                        packages.WithEnabledGrant(grant.id, grant.grant, [&] {
                            if (!life->load() || app->thumbnail_epoch.load() != epoch) return;
                            rimes::capsule::ClipboardHistoryStore store(rimes::capsule::ClipboardHistoryStore::DefaultRoot());
                            auto archive = store.Read(card.id, card.revision);
                            if (archive) image = rimes::capsule::ClipboardThumbnail(*archive);
                        });
                    } else {
                        rimes::capsule::ContentStore library(rimes::capsule::ContentStore::DefaultRoot());
                        rimes::capsule::CaptureStore archive(rimes::capsule::CaptureStore::DefaultRoot());
                        rimes::capsule::Record media;
                        const auto asset = archive.Acquire(card.id, card.revision);
                        if (asset) {
                            media.kind = card.kind;
                            media.content = asset->path.string();
                        } else {
                            std::string error;
                            if (library.Get(card.id, &media, &error) != rimes::capsule::StoreStatus::Ok ||
                                media.revision != card.revision || rimes::capsule::RecordModule(media) != rimes::capsule::Module::Capture)
                                throw std::runtime_error("捕获已变化");
                        }
                        std::atomic<bool> cancelled{false};
                        const auto preview = rimes::capsule::PreviewResource(media, library, cancelled);
                        rimes::capsule::ClipboardArchive data;
                        data.representations.push_back({"image/png", preview.png});
                        packages.WithEnabledGrant(grant.id, grant.grant, [&] {
                            if (life->load() && app->thumbnail_epoch.load() == epoch)
                                image = rimes::capsule::ClipboardThumbnail(data);
                        });
                    }
                } catch (const std::exception&) {
                }
                auto* completion = new std::function<void()>([app, life, epoch, key, apply, image] {
                    if (!life->load())
                        return;
                    app->thumbnail_pending.erase(key);
                    if (app->thumbnail_epoch.load() == epoch)
                        apply(image);
                });
                g_idle_add_full(
                    G_PRIORITY_DEFAULT,
                    +[](gpointer raw) -> gboolean {
                        (*static_cast<std::function<void()>*>(raw))();
                        return G_SOURCE_REMOVE;
                    },
                    completion,
                    +[](gpointer raw) { delete static_cast<std::function<void()>*>(raw); });
            }))
            app->thumbnail_pending.erase(key);
    }
}

void RebuildCards(App* app) {
    ClearBox(app->card_box);
    app->card_rows.clear();
    if (app->snapshot.cards.empty()) {
        GtkWidget* empty = gtk_label_new(app->snapshot.hint.c_str());
        gtk_widget_set_name(empty, "rimes-placeholder");
        gtk_box_pack_start(GTK_BOX(app->card_box), empty, FALSE, FALSE, 0);
        gtk_widget_show_all(app->card_box);
        return;
    }
    app->card_rows.reserve(app->snapshot.cards.size());
    for (int i = 0; i < static_cast<int>(app->snapshot.cards.size()); ++i) {
        app->card_rows.push_back(
            MakeCardRow(app, app->snapshot.cards[static_cast<std::size_t>(i)], i));
    }
    gtk_widget_show_all(app->card_box);
}

void SyncCards(App* app) {
    const bool rebuilt = app->snapshot.cards.empty() || !CardsMatchIds(app);
    if (rebuilt)
        RebuildCards(app);
    else
        UpdateCardsInPlace(app);
    if (!app->snapshot.visible || app->snapshot.password_field || app->snapshot.cards.empty()) {
        app->scrolled_selection.clear();
        return;
    }
    if (rebuilt || app->scrolled_selection != app->snapshot.selected_id) {
        app->scrolled_selection = app->snapshot.selected_id;
        auto* work =
            new std::function<void()>([app, life = app->alive, id = app->snapshot.selected_id] {
                if (!life->load() || !app->snapshot.visible || app->snapshot.password_field ||
                    app->snapshot.selected_id != id)
                    return;
                auto* adjustment =
                    gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(app->card_scroll));
                const double left = app->snapshot.selected * 214.0;
                const auto at = gtk_adjustment_get_value(adjustment),
                           page = gtk_adjustment_get_page_size(adjustment);
                if (left < at)
                    gtk_adjustment_set_value(adjustment, left);
                else if (left + 206 > at + page)
                    gtk_adjustment_set_value(adjustment, left + 206 - page);
            });
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                (*static_cast<std::function<void()>*>(raw))();
                return G_SOURCE_REMOVE;
            },
            work, +[](gpointer raw) { delete static_cast<std::function<void()>*>(raw); });
    }
}

bool DisplayIsWayland() {
#ifdef GDK_WINDOWING_WAYLAND
    GdkDisplay* display = gdk_display_get_default();
    return display != nullptr && GDK_IS_WAYLAND_DISPLAY(display);
#else
    return false;
#endif
}

void ApplyCopiedText(App* app, const rimes::capsule::ClipboardWriteDecision& decision) {
    if (decision.path != rimes::capsule::ClipboardWritePath::Skip && app->clipboard)
        app->clipboard->CopyText(app->snapshot.last_copied);
}

void LoadCss(const std::string& theme, GdkScreen* screen);

void RebuildDetail(App* app) {
    using namespace rimes::desktop;
    Clear(app->detail_box);
    if (!app->snapshot.detail || !app->snapshot.visible || app->snapshot.password_field)
        return;
    const auto& state = app->snapshot;
    auto send = [app, id = state.selected_id, revision = state.detail_revision](const char* op,
                                                                                int index) {
        SendCommand(app,
                    Json{{"v", 1}, {"op", op}, {"id", id}, {"revision", revision}, {"index", index}}
                        .dump());
    };
    auto* info = Class(gtk_event_box_new(), "capsule-detail-info");
    gtk_widget_set_size_request(info, 206, 126);
    auto* content = Column(4);
    gtk_container_add(GTK_CONTAINER(info), content);
    gtk_widget_set_margin_start(content, 10);
    gtk_widget_set_margin_end(content, 10);
    gtk_widget_set_margin_top(content, 8);
    gtk_widget_set_margin_bottom(content, 8);
    auto* title_row = Row(5);
    Add(title_row, Icon("note.text", 11));
    auto* title = Label(state.selected_title, "capsule-detail-title");
    gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_lines(GTK_LABEL(title), 2);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 18);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    Add(title_row, title, true);
    Add(content, title_row);
    auto* summary = Label(state.selected_preview, "capsule-detail-summary");
    gtk_label_set_line_wrap(GTK_LABEL(summary), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(summary), 20);
    gtk_label_set_lines(GTK_LABEL(summary), 3);
    gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
    Add(content, summary);
    Add(content, gtk_label_new(""), true);
    if (!state.detail_notice.empty()) {
        auto* notice = Label(state.detail_notice, "capsule-detail-notice");
        gtk_label_set_line_wrap(GTK_LABEL(notice), TRUE);
        gtk_label_set_lines(GTK_LABEL(notice), 3);
        gtk_label_set_max_width_chars(GTK_LABEL(notice), 1);
        gtk_label_set_ellipsize(GTK_LABEL(notice), PANGO_ELLIPSIZE_END);
        Add(content, notice);
    }
    Add(app->detail_box, info);
    auto* right = Column(5);
    gtk_widget_set_margin_top(right, 3);
    Add(app->detail_box, right, true);
    auto* bar = Row(0);
    Add(bar, gtk_label_new(""), true);
    auto* back = Button(
        "返回", [app] { SendCommand(app, R"({"v":1,"op":"detail-close"})"); },
        "capsule-detail-back");
    gtk_button_set_image(GTK_BUTTON(back), Icon("chevron.left", 10));
    gtk_button_set_always_show_image(GTK_BUTTON(back), TRUE);
    gtk_widget_set_size_request(back, 72, 24);
    gtk_widget_set_margin_end(back, 4);
    Add(bar, back);
    Add(right, bar);
    auto* rows = Column(0);
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), rows);
    Add(right, scroll, true);
    std::string last_heading;
    int selected_y = 0, y = 0;
    for (std::size_t i = 0; i < state.detail_rows.size(); ++i) {
        const auto& row = state.detail_rows[i];
        const int index = state.detail_offset + static_cast<int>(i);
        const auto heading = i < state.detail_headings.size() ? state.detail_headings[i] : "";
        if (!heading.empty() && heading != last_heading) {
            auto* label = Label(heading, "capsule-detail-section");
            gtk_widget_set_size_request(label, -1, 18);
            gtk_widget_set_margin_start(label, 6);
            Add(rows, label);
            y += 18;
        }
        last_heading = heading;
        auto* event = Class(gtk_event_box_new(), "capsule-detail-row");
        gtk_widget_set_size_request(event, -1, 24);
        if (index == state.detail_selected) {
            Class(event, "selected");
            selected_y = y;
        }
        y += 24;
        auto* line = Row(0);
        gtk_container_add(GTK_CONTAINER(event), line);
        gtk_widget_set_margin_start(line, 6);
        gtk_widget_set_margin_end(line, 4);
        if (!row.label.empty()) {
            auto* label = Label(row.label, "capsule-detail-label");
            gtk_widget_set_size_request(label, 92, -1);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 12);
            gtk_widget_set_margin_end(label, 8);
            Add(line, label);
        }
        auto* value = Label(row.value, "capsule-detail-value");
        if (row.type == "code")
            Class(value, "capsule-detail-code");
        gtk_label_set_max_width_chars(GTK_LABEL(value), 1);
        gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
        Add(line, value, true);
        auto* space = gtk_label_new("");
        gtk_widget_set_size_request(space, 26, -1);
        Add(line, space);
        Add(line, IconButton(index == app->detail_copy_index &&
                                     state.selected_id == app->detail_copy_id &&
                                     g_get_monotonic_time() < app->detail_copy_until
                                 ? "checkmark"
                                 : "doc.on.doc",
                             "复制这一行", [send, index] { send("row-copy", index); }));
        Add(rows, event);
        auto* select = new std::function<void(bool)>(
            [send, index](bool activate) { send(activate ? "row-insert" : "row-select", index); });
        g_signal_connect_data(
            event, "button-press-event",
            reinterpret_cast<GCallback>(+[](GtkWidget*, GdkEventButton* event,
                                            gpointer raw) -> gboolean {
                if (event->button != 1)
                    return FALSE;
                (*static_cast<std::function<void(bool)>*>(raw))(event->type == GDK_2BUTTON_PRESS);
                return TRUE;
            }),
            select,
            +[](gpointer raw, GClosure*) { delete static_cast<std::function<void(bool)>*>(raw); },
            GConnectFlags(0));
    }
    gtk_widget_set_no_show_all(app->detail_box, FALSE);
    gtk_widget_show_all(app->detail_box);
    gtk_widget_set_no_show_all(app->detail_box, TRUE);
    if (state.detail_selected >= state.detail_offset) {
        auto* adjust = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
        struct Scroll {
            GtkAdjustment* adjustment;
            double value;
        };
        auto* pending =
            new Scroll{GTK_ADJUSTMENT(g_object_ref(adjust)), double(std::max(0, selected_y - 42))};
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* pending = static_cast<Scroll*>(raw);
                gtk_adjustment_set_value(pending->adjustment, pending->value);
                return G_SOURCE_REMOVE;
            },
            pending,
            +[](gpointer raw) {
                auto* pending = static_cast<Scroll*>(raw);
                g_object_unref(pending->adjustment);
                delete pending;
            });
    }
}

void ResetClear(App* app) {
    if (app->clear_timer)
        g_source_remove(app->clear_timer);
    app->clear_timer = 0;
    app->clear_armed = false;
    gtk_button_set_label(GTK_BUTTON(app->clear_history), "清空");
    gtk_widget_set_tooltip_text(app->clear_history, "连续点击两次，清空未收藏的历史");
}
void ApplySnapshot(App* app) {
    if(app->capture) app->capture->Apply(app->snapshot);
    if(app->capture_button) gtk_widget_set_visible(app->capture_button,
        !app->snapshot.password_field && std::find(app->snapshot.modules.begin(), app->snapshot.modules.end(), rimes::capsule::Module::Capture) != app->snapshot.modules.end());
    if (app->clipboard)
        app->clipboard->Apply(app->snapshot);
    if (!app->snapshot.visible || app->snapshot.password_field ||
        (app->snapshot.module != "temporary" && app->snapshot.module != "capture")) {
        ResetClear(app);
        if (app->history_menu)
            gtk_widget_destroy(app->history_menu);
    }
    const bool live = app->snapshot.visible && app->snapshot.armed &&
                      !app->snapshot.password_field && !app->snapshot.managing;
    auto* search_style = gtk_widget_get_style_context(app->search_shell);
    if (live)
        gtk_style_context_add_class(search_style, "live");
    else
        gtk_style_context_remove_class(search_style, "live");
    if (live && !app->caret_timer && !app->preview)
        app->caret_timer = g_timeout_add(
            530,
            +[](gpointer raw) -> gboolean {
                auto* self = static_cast<App*>(raw);
                self->caret_visible = !self->caret_visible;
                gtk_widget_queue_draw(self->query);
                return G_SOURCE_CONTINUE;
            },
            app);
    if (!live && app->caret_timer) {
        g_source_remove(app->caret_timer);
        app->caret_timer = 0;
    }
    if (app->snapshot.query != gtk_label_get_text(GTK_LABEL(app->query)))
        app->caret_visible = true;
    if (app->snapshot.password_field || !app->snapshot.visible ||
        (app->snapshot.module != "temporary" && app->snapshot.module != "capture")) {
        app->thumbnail_epoch.fetch_add(1);
        app->thumbnail_pending.clear();
        app->thumbnail_cache.clear();
    }
    if (app->theme != app->snapshot.theme) {
        LoadCss(app->snapshot.theme, gtk_widget_get_screen(app->window));
        app->theme = app->snapshot.theme;
    }
    gtk_label_set_text(GTK_LABEL(app->count),
                       rimes::capsule::CountText(app->snapshot.count).c_str());
    gtk_label_set_text(GTK_LABEL(app->hint), app->snapshot.hint.c_str());
    const auto query =
        app->snapshot.query.empty() ? std::string("直接输入以搜索") : app->snapshot.query;
    gtk_label_set_text(GTK_LABEL(app->query), query.c_str());
    RebuildTabs(app);
    SyncCards(app);
    RequestThumbnails(app);
    gtk_widget_set_visible(app->clear_history, app->snapshot.module == "temporary");
    if (app->snapshot.detail && app->seen_copy_seq &&
        app->snapshot.copy_seq != app->handled_copy_seq && !app->snapshot.last_copied.empty()) {
        app->detail_copy_index = app->snapshot.detail_selected;
        app->detail_copy_id = app->snapshot.selected_id;
        app->detail_copy_until = g_get_monotonic_time() + 1200000;
        if (app->detail_copy_timer)
            g_source_remove(app->detail_copy_timer);
        app->detail_copy_timer = g_timeout_add(
            1200,
            +[](gpointer raw) -> gboolean {
                auto* app = static_cast<App*>(raw);
                app->detail_copy_timer = 0;
                RebuildDetail(app);
                return G_SOURCE_REMOVE;
            },
            app);
    }
    RebuildDetail(app);
    const bool wayland = DisplayIsWayland();
    const bool have_wl_copy = wayland && !rimes::capsule::FindOnPath("wl-copy").empty();
    rimes::capsule::ClipboardApplyState copy_state{app->seen_copy_seq, app->handled_copy_seq};
    const auto decision =
        rimes::capsule::ApplyCopiedNote(&copy_state, app->snapshot.copy_seq,
                                       app->snapshot.last_copied, wayland, have_wl_copy);
    app->seen_copy_seq = copy_state.seen_seq;
    app->handled_copy_seq = copy_state.handled_seq;
    ApplyCopiedText(app, decision);
    app->manager->Apply(app->snapshot);
    if (app->snapshot.visible && !app->snapshot.managing) {
        app->panel->Place(kWindowWidth, kWindowHeight, {}, !app->placed);
        gtk_widget_show_all(app->window);
        gtk_widget_set_visible(app->filter_box, TRUE);
        gtk_widget_set_visible(app->card_scroll, !app->snapshot.detail);
        gtk_widget_set_visible(app->detail_box, app->snapshot.detail);
        app->placed = true;
    } else {
        app->last_press_index = -1;
        app->last_press_ms = -1;
        gtk_widget_hide(app->window);
        app->placed = false;
    }
}

bool ConnectSocket(App* app, const std::string& path);
void Disconnected(App* app) {
    if (app->socket_fd >= 0)
        close(app->socket_fd);
    app->socket_fd = -1;
    app->io_id = 0;
    app->incoming.clear();
    app->seen_copy_seq = false;
    app->snapshot = {};
    app->snapshot.password_field = true;
    ApplySnapshot(app); // Conceal immediately while retaining ordinary unsaved drafts.
    if (!app->reconnect_id)
        app->reconnect_id = g_timeout_add(
            250,
            +[](gpointer raw) -> gboolean {
                auto* app = static_cast<App*>(raw);
                if (!ConnectSocket(app, app->socket_path))
                    return G_SOURCE_CONTINUE;
                app->reconnect_id = 0;
                return G_SOURCE_REMOVE;
            },
            app);
}

gboolean OnSocket(GIOChannel* /*channel*/, GIOCondition condition, gpointer data) {
    auto* app = static_cast<App*>(data);
    if ((condition & (G_IO_HUP | G_IO_ERR)) != 0) {
        Disconnected(app);
        return G_SOURCE_REMOVE;
    }
    char chunk[4096];
    const auto got = read(app->socket_fd, chunk, sizeof(chunk));
    if (got < 0 && errno == EINTR)
        return G_SOURCE_CONTINUE;
    if (got <= 0) {
        Disconnected(app);
        return G_SOURCE_REMOVE;
    }
    app->incoming.append(chunk, static_cast<std::size_t>(got));
    while (app->incoming.size() >= 4) {
        std::uint32_t length = 0;
        if (!rimes::capsule::DecodeFrameHeader(app->incoming.data(), &length)) {
            Disconnected(app);
            return G_SOURCE_REMOVE;
        }
        if (app->incoming.size() < 4 + length) {
            break;
        }
        const std::string payload = app->incoming.substr(4, length);
        app->incoming.erase(0, 4 + length);
        rimes::capsule::Snapshot parsed;
        if (!rimes::capsule::DecodeSnapshot(payload, &parsed)) {
            continue;
        }
        app->snapshot = parsed;
        ApplySnapshot(app);
    }
    return G_SOURCE_CONTINUE;
}

bool ConnectSocket(App* app, const std::string& path) {
    if (const char* delay = g_getenv("RIMES_CAPSULE_CONNECT_DELAY_MS")) {
        const auto ms = std::atoi(delay);
        if (ms > 0 && ms < 30000) {
            g_usleep(static_cast<gulong>(ms) * 1000);
        }
    }
    app->socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (app->socket_fd < 0) {
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (connect(app->socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(app->socket_fd);
        app->socket_fd = -1;
        return false;
    }
    GIOChannel* channel = g_io_channel_unix_new(app->socket_fd);
    app->io_id = g_io_add_watch(channel, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
                                OnSocket, app);
    g_io_channel_unref(channel);
    SendCommand(app, R"({"v":1,"op":"hello"})");
    return true;
}

void LoadCss(const std::string& theme, GdkScreen* screen) {
    using namespace rimes::desktop;
    Theme(theme, screen);
    const auto& palette = Visuals().at("palettes").at(theme == "liquidGlass" ? "day" : theme);
    auto c = [&](const char* key) { return palette.at(key).get<std::string>(); };
    static auto* provider = gtk_css_provider_new();
    const std::string css =
        "#rimes-capsule-window, .rimes-capsule, #rimes-chrome { background: " +
        c("bufferBackground") + "; color: " + c("textPrimary") +
        "; border-radius: 10px; }"
        "#rimes-title { color: " +
        c("textPrimary") +
        "; font-family: monospace; font-weight: bold; font-size: 15px; }"
        "#rimes-count, #rimes-query, #rimes-hint, #rimes-placeholder { color: " +
        c("textSecondary") +
        "; font-size: 12px; }"
        "button.rimes-tab, button.rimes-action { background: " +
        c("surfaceSecondary") + "; color: " + c("textPrimary") + "; border: 1px solid " +
        c("borderStrong") +
        "; border-radius: 6px; padding: 2px 8px; font-size: 12px; }"
        ".rimes-tab-group { background: " +
        c("surfaceSecondary") + "; border: 1px solid " + c("border") +
        "; border-radius: 8px; padding: 2px; }"
        "button.rimes-tab { background: transparent; border: 0; border-radius: "
        "6px; padding: 0; min-height: 0; font-size: 11px; font-weight: 600; "
        "color: " +
        c("textSecondary") +
        "; }"
        "button.rimes-tab-active { background: alpha(" +
        c("accentGreen") +
        ", 0.18); }"
        "button.rimes-action { min-width: 22px; min-height: 22px; padding: 0; }"
        "button.rimes-filter { padding: 0 8px; border: 0; min-height: 0; "
        "background: transparent; border-radius: 4px; font-size: 11px; color: " +
        c("textSecondary") +
        "; }"
        "button.rimes-filter.selected { background: " +
        c("surfaceTertiary") + "; font-weight: 600; color: " + c("textPrimary") +
        "; }"
        "#rimes-hint { font-size: 9px; font-family: monospace; font-weight: 600; } #rimes-count { "
        "font-family: monospace; "
        "font-size: 10px; }"
        ".capsule-search { background: " +
        c("surfaceSecondary") + "; border: 1px solid " + c("borderStrong") +
        "; border-radius: 8px; padding: 0 8px; }"
        ".capsule-search.live { border-color: alpha(" +
        c("accentText") +
        ", 0.55); }"
        ".capsule-search #rimes-query { color: " +
        c("textPrimary") +
        "; font-size: 12px; }"
        ".capsule-header-button { min-height: 0; min-width: 0; padding: 0; background: "
        "transparent; }"
        ".capsule-header-button label { font-size: 11px; font-weight: 600; color: " +
        c("textSecondary") +
        "; }"
        ".capsule-clear { min-height: 0; min-width: 0; padding: 0 6px; border-radius: 4px; "
        "font-size: 11px; }"
        "#rimes-card { background: " +
        c("surfaceSecondary") +
        "; border-radius: 8px; }"
        "#rimes-card.rimes-card-selected { border: 1px solid " +
        c("accentGreen") +
        "; }"
        "#rimes-card-title { color: " +
        c("textPrimary") +
        "; font-weight: bold; }"
        "#rimes-card-preview { color: " +
        c("textSecondary") +
        "; font-size: 11px; }"
        "#rimes-card.clipboard-card { border: 1px solid " +
        c("border") +
        "; border-radius: 10px; padding: 0; }"
        "#rimes-card.clipboard-card.rimes-card-selected { background: " +
        c("clipboardSelected") + "; border: 2px solid " + c("accentGreen") +
        "; }"
        ".clipboard-card .clipboard-quick { font-family: monospace; font-size: 9px; font-weight: "
        "bold; color: " +
        c("textMuted") +
        "; }"
        "#rimes-card.clipboard-card.rimes-card-selected .clipboard-quick { color: " +
        c("accentGreen") +
        "; }"
        ".clipboard-card .clipboard-source { font-size: 10px; font-weight: 600; color: " +
        c("textSecondary") +
        "; }"
        ".clipboard-card .clipboard-preview { font-size: 12px; color: " +
        c("textPrimary") +
        "; }"
        ".clipboard-card .clipboard-picture { background: " +
        c("surfaceTertiary") +
        "; border-radius: 7px; }"
        ".clipboard-card .icon-button { padding: 0; min-width: 0; min-height: 0; border: 1px "
        "solid " +
        c("borderStrong") + "; border-radius: 6px; background: " + c("surfaceSecondary") +
        "; }"
        "#rimes-card.capture-card { border-radius: 11px; background: " + c("surfaceTertiary") + "; }"
        "#rimes-card.capture-card.rimes-card-selected { border-width: 1px; background: " + c("surfaceTertiary") + "; }"
        ".capture-card .clipboard-source { font-size: 11px; font-weight: 400; color: " + c("textPrimary") + "; }"
        ".capture-card .clipboard-preview { font-size: 10px; color: " + c("textPrimary") + "; }"
        ".capture-card .icon-button { border: 0; border-radius: 12px; box-shadow: none; background: " +
        ((theme != "day" && theme != "liquidGlass") ? std::string("rgba(255,255,255,0.13)") : std::string("rgba(245,245,245,0.92)")) + "; }"
        ".capsule-detail-info { background: " +
        c("surfaceSecondary") +
        "; border-radius: 8px; padding: 0; }"
        ".capsule-detail-row { border-radius: 5px; } "
        ".capsule-detail-row.selected { background: alpha(" +
        c("accentSecondary") +
        ",0.22); }"
        ".capsule-detail-value,.capsule-detail-back { background: transparent; "
        "padding: 0; border: 0; min-height: 0; }"
        ".capsule-detail-label { color: " +
        c("textSecondary") +
        "; font-size: 11px; }"
        ".capsule-detail-value { color: " +
        c("textPrimary") +
        "; font-size: 12px; }"
        ".capsule-detail-notice { font-size: 10px; color: " +
        c("brandYellow") +
        "; }"
        ".capsule-detail-code { font-family: monospace; font-size: 11px; }"
        ".capsule-detail-title { font-size: 13px; font-weight: 600; } "
        ".capsule-detail-summary { font-size: 11px; color: " +
        c("textSecondary") +
        "; }"
        ".capsule-detail-section { font-size: 10px; font-weight: 600; color: " +
        c("textMuted") +
        "; }"
        ".capsule-detail-row .icon-button { min-width: 20px; min-height: 20px; }";
    gtk_css_provider_load_from_data(provider, css.c_str(), -1, nullptr);
    gtk_style_context_remove_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider));
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

void BuildUi(App* app) {
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), rimes::branding::Title(" Capsule").c_str());
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
    gtk_widget_set_name(app->window, "rimes-capsule-window");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->window), "rimes-capsule");
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    g_signal_connect(app->window, "size-allocate", G_CALLBACK(OnSizeAllocate), app);

    app->panel = std::make_unique<rimes::desktop::PanelWindow>(GTK_WINDOW(app->window), "capsule", 16, !app->preview);
    app->wayland_layer = app->panel->Layer();
    gtk_widget_set_size_request(app->window, kWindowWidth, kWindowHeight);

    GtkWidget* chrome = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(chrome, "rimes-chrome");
    gtk_container_add(GTK_CONTAINER(app->window), chrome);

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    gtk_widget_set_margin_start(header,14); gtk_widget_set_margin_end(header,14);
    gtk_widget_set_margin_top(header,12); gtk_widget_set_size_request(header,-1,30);
    gtk_box_pack_start(GTK_BOX(chrome), header, FALSE, FALSE, 0);

    GtkWidget* title = gtk_label_new("Capsule");
    gtk_widget_set_name(title, "rimes-title");
    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);

    app->count = gtk_label_new("0 ITEMS");
    gtk_widget_set_name(app->count, "rimes-count");
    gtk_box_pack_start(GTK_BOX(header), app->count, FALSE, FALSE, 0);
    app->tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    rimes::desktop::Class(app->tab_box, "rimes-tab-group");
    gtk_box_pack_start(GTK_BOX(header), app->tab_box, FALSE, FALSE, 0);

    auto* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(header), spacer, TRUE, TRUE, 0);
    app->search_shell = rimes::desktop::Class(gtk_event_box_new(), "capsule-search");
    gtk_widget_set_size_request(app->search_shell, 250, 28);
    gtk_widget_set_valign(app->search_shell, GTK_ALIGN_CENTER);
    auto* search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_container_add(GTK_CONTAINER(app->search_shell), search_row);
    gtk_box_pack_start(GTK_BOX(search_row), rimes::desktop::Icon("magnifyingglass", 14), FALSE,
                       FALSE, 0);
    app->query = gtk_label_new("输入以搜索");
    gtk_widget_set_name(app->query, "rimes-query");
    gtk_label_set_xalign(GTK_LABEL(app->query), 0);
    gtk_label_set_ellipsize(GTK_LABEL(app->query), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app->query), 1);
    gtk_widget_set_tooltip_text(app->search_shell,
                                "在目标输入框唤出后直接输入搜索；管理页支持独立搜索");
    atk_object_set_name(gtk_widget_get_accessible(app->search_shell), "搜索 Capsule");
    gtk_box_pack_start(GTK_BOX(search_row), app->query, TRUE, TRUE, 0);
    // The rail borrows the focused input context. Painting its search field
    // must not create a second GTK input context or steal the insertion target.
    g_signal_connect_after(
        app->query, "draw",
        G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer raw) -> gboolean {
            auto* self = static_cast<App*>(raw);
            if (!self->caret_visible || !self->snapshot.visible || !self->snapshot.armed ||
                self->snapshot.password_field)
                return FALSE;
            int w = 0;
            int h = 0;
            pango_layout_get_pixel_size(gtk_label_get_layout(GTK_LABEL(widget)), &w, &h);
            const auto x = self->snapshot.query.empty()
                               ? 0
                               : std::min(w, gtk_widget_get_allocated_width(widget) - 2);
            const auto& palette =
                rimes::desktop::Visuals()
                    .at("palettes")
                    .at(self->snapshot.theme == "liquidGlass" ? "day" : self->snapshot.theme);
            GdkRGBA color{};
            gdk_rgba_parse(&color, palette.at("accentText").get<std::string>().c_str());
            gdk_cairo_set_source_rgba(cr, &color);
            cairo_rectangle(cr, x, (gtk_widget_get_allocated_height(widget) - 16) / 2.0, 1.5, 16);
            cairo_fill(cr);
            return FALSE;
        }),
        app);
    gtk_box_pack_start(GTK_BOX(header), app->search_shell, FALSE, FALSE, 0);
    app->clear_history = rimes::desktop::Button(
        "清空",
        [app] {
            if (!app->snapshot.visible || app->snapshot.password_field ||
                app->snapshot.module != "temporary")
                return;
            if (app->clear_armed) {
                ResetClear(app);
                SendCommand(app, R"({"v":1,"op":"clear-history"})");
                return;
            }
            app->clear_armed = true;
            gtk_button_set_label(GTK_BUTTON(app->clear_history), "再次点击清空");
            app->clear_timer = g_timeout_add(
                4000,
                +[](gpointer raw) -> gboolean {
                    auto* self = static_cast<App*>(raw);
                    self->clear_timer = 0;
                    ResetClear(self);
                    return G_SOURCE_REMOVE;
                },
                app);
        },
        "capsule-clear");
    gtk_widget_set_valign(app->clear_history, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(app->clear_history, -1, 16);
    gtk_widget_set_can_focus(app->clear_history, FALSE);
    ResetClear(app);
    gtk_widget_set_no_show_all(app->clear_history, TRUE);
    gtk_box_pack_start(GTK_BOX(header), app->clear_history, FALSE, FALSE, 0);
    auto* capture = rimes::desktop::IconButton("camera", "截图 / 录屏", [app] {if(app->capture) app->capture->Open();});
    app->capture_button=capture;gtk_widget_set_no_show_all(capture,TRUE);
    gtk_widget_set_size_request(capture,24,24);gtk_widget_set_can_focus(capture,FALSE);
    gtk_box_pack_start(GTK_BOX(header),capture,FALSE,FALSE,0);
    auto* manage = rimes::desktop::Button(
        "管理", [app] { SendCommand(app, R"({"v":1,"op":"manage"})"); }, "capsule-header-button");
    gtk_button_set_image(GTK_BUTTON(manage), rimes::desktop::Icon("gearshape", 13));
    gtk_button_set_always_show_image(GTK_BUTTON(manage), TRUE);
    gtk_widget_set_size_request(manage, 54, 24);
    gtk_widget_set_valign(manage, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(manage, "打开 Capsule 模块管理（三栏视图）");
    gtk_widget_set_can_focus(manage,FALSE); gtk_box_pack_start(GTK_BOX(header),manage,FALSE,FALSE,0);

    GtkWidget* close = rimes::desktop::Class(gtk_button_new(), "capsule-header-button");
    gtk_container_add(GTK_CONTAINER(close), rimes::desktop::Icon("xmark", 13));
    gtk_widget_set_size_request(close, 24, 24);
    gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
    gtk_widget_set_can_focus(close, FALSE);
    gtk_widget_set_tooltip_text(close, "关闭 Capsule");
    g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         SendCommand(static_cast<App*>(data), R"({"v":1,"op":"close"})");
                     }),
                     app);
    gtk_box_pack_end(GTK_BOX(header), close, FALSE, FALSE, 0);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_NEVER);
    auto* band = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_widget_set_margin_start(band,14); gtk_widget_set_margin_end(band,14); gtk_widget_set_margin_top(band,9);
    gtk_box_pack_start(GTK_BOX(chrome),band,FALSE,FALSE,0);
    app->filter_box=gtk_box_new(GTK_ORIENTATION_VERTICAL,3); gtk_widget_set_size_request(app->filter_box,60,126);
    gtk_box_pack_start(GTK_BOX(band),app->filter_box,FALSE,FALSE,0);
    app->card_scroll = scroll;
    g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroll)),
                     "value-changed", G_CALLBACK(+[](GtkAdjustment*, gpointer raw) {
                         RequestThumbnails(static_cast<App*>(raw));
                     }),
                     app);
    gtk_widget_set_size_request(scroll,-1,126); gtk_box_pack_start(GTK_BOX(band),scroll,TRUE,TRUE,0);

    app->detail_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(band), app->detail_box, TRUE, TRUE, 0);
    gtk_widget_set_no_show_all(app->detail_box, TRUE);

    app->card_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_add(GTK_CONTAINER(scroll), app->card_box);

    app->hint = gtk_label_new("Return inserts the selected note · Esc closes");
    gtk_widget_set_name(app->hint, "rimes-hint");
    gtk_widget_set_halign(app->hint, GTK_ALIGN_START);
    gtk_widget_set_margin_start(app->hint, 14);
    gtk_widget_set_margin_top(app->hint,8);
    gtk_widget_set_margin_bottom(app->hint, 8);
    gtk_box_pack_start(GTK_BOX(chrome), app->hint, FALSE, FALSE, 0);
    app->capture=std::make_unique<rimes::capsule::CaptureController>([app](auto command){SendCommand(app,command);});
    app->manager = std::make_unique<rimes::capsule::ManagerWindow>([app](auto command) { SendCommand(app,command); },app->preview,
        [app](const rimes::capsule::Record& record) {if(app->clipboard) app->clipboard->CopyResource(record);},
        [app](const std::string& text) {if(app->clipboard) app->clipboard->CopyText(text);},[app] {if(app->capture) app->capture->Open();});
}

}  // namespace

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    std::string socket_path = DefaultSocketPath();
    bool resources_preview = false, capture_preview = false, password_preview = false;
    std::string capture_image, capture_kind="image";
    bool preview = false, manager_preview = false, detail_preview = false,
         clipboard_preview = false;
    std::string render_path, preview_theme = "night";
    std::string dump_geometry;
    bool quit_after_dump = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--socket") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (std::strcmp(argv[index], "--capture-preview") == 0 && index + 1 < argc) {
            preview = true;capture_preview = true;capture_image=argv[++index];
        } else if (std::strcmp(argv[index], "--capture-kind") == 0 && index + 1 < argc) {
            capture_kind=argv[++index];
        } else if (std::strcmp(argv[index], "--clipboard-preview") == 0) {
            preview = true;
            clipboard_preview = true;
        } else if (std::strcmp(argv[index], "--detail-preview") == 0) {
            preview = true;
            detail_preview = true;
        } else if (std::strcmp(argv[index], "--resources-preview") == 0) {
            preview = true; manager_preview = true; resources_preview = true;
        } else if (std::strcmp(argv[index], "--password-preview") == 0) {
            preview = true; manager_preview = true; password_preview = true;
        } else if (std::strcmp(argv[index], "--manager-preview") == 0) {
            preview = true; manager_preview = true;
        } else if (std::strcmp(argv[index], "--render") == 0 && index + 1 < argc) {
            render_path = argv[++index];
        } else if (std::strcmp(argv[index], "--theme") == 0 && index + 1 < argc) {
            preview_theme = argv[++index];
        } else if (std::strcmp(argv[index], "--preview") == 0) {
            preview = true;
        } else if (std::strcmp(argv[index], "--dump-geometry") == 0 && index + 1 < argc) {
            dump_geometry = argv[++index];
        } else if (std::strcmp(argv[index], "--quit-after-dump") == 0) {
            quit_after_dump = true;
        }
    }

    if (!rimes::desktop::Visuals().at("palettes").contains(preview_theme) && preview_theme!="liquidGlass") return 2;
    if (!render_path.empty() && !preview) return 2;
    App app;
    g_app = &app;
    app.dump_geometry = dump_geometry;
    app.quit_after_dump = quit_after_dump;
    app.preview = preview;app.preview_image=capture_image;
    app.socket_path = socket_path;
    BuildUi(&app);

    if (preview) {
        app.snapshot.visible = true;
        app.snapshot.managing = manager_preview;
        app.snapshot.theme = preview_theme;
        for (auto module : rimes::capsule::kModules) if (module != rimes::capsule::Module::Notes) app.snapshot.modules.push_back(module);
        std::sort(app.snapshot.modules.begin(),app.snapshot.modules.end());
        app.snapshot.armed = true;
        app.snapshot.tab = "note";
        app.snapshot.hint = manager_preview ? "Ctrl+S SAVE   ESC BACK TO RAIL" : "Return inserts the selected note · Esc closes";
        app.snapshot.count = 1;
        rimes::capsule::Card demo;
        demo.id = rimes::capsule::kDefaultEntryId;
        demo.title = rimes::capsule::kDefaultEntryTitle;
        demo.preview = rimes::capsule::kDefaultEntryContent;
        demo.kind = rimes::capsule::Kind::Note;
        if (manager_preview) {
            int identity=1;
            for(const auto& item:rimes::capsule::ModuleContract().at("managerPreview")) {
                auto card=demo;card.id="72696d65-7300-4000-8000-00000000000"+std::to_string(++identity);
                card.title=item.at("title"); card.payload=item.at("content");
                card.preview=rimes::capsule::PreviewOf(card.payload.substr(0,card.payload.find('\n')));app.snapshot.cards.push_back(card);
            }
        }
        app.snapshot.cards.push_back(demo); app.snapshot.selected_id=app.snapshot.cards.front().id;
        app.snapshot.count=static_cast<int>(app.snapshot.cards.size());
        if (capture_preview) {
            rimes::capsule::CaptureRecord record;record.id=demo.id;record.kind=capture_kind;
            record.title=capture_kind=="video"?"录屏示例 👨‍👩‍👧‍👦":"截图示例";
            record.source=capture_kind=="video"?"文件导入":"桌面捕获";record.duration=125.9;
            if(capture_kind=="video") {record.collection_id=record.id;record.incomplete=true;}
            app.snapshot.cards={rimes::capsule::CaptureCard(record)};app.snapshot.module="capture";
            app.snapshot.count=1;app.snapshot.selected_id=record.id;
        }
        if(password_preview) {
            auto card=demo;card.kind=rimes::capsule::Kind::Password;card.module=rimes::capsule::Module::Passwords;
            card.title="公开测试账号";card.preview="仅供测试";card.filter="login";card.revision=std::string(64,'a');card.payload.clear();
            app.snapshot.cards={card};app.snapshot.module="passwords";app.snapshot.tab="password";app.snapshot.count=1;app.snapshot.selected_id=card.id;
            app.snapshot.hint="↩ UNLOCK DETAILS   ESC CLOSE · 密码不会自动上屏";
        }
        if (resources_preview) {
            auto card=demo; card.kind=rimes::capsule::Kind::Pdf; card.module=rimes::capsule::Module::Resources;
            card.title="参考文献示例"; card.payload="/home/example/Documents/reference.pdf";
            card.preview="Ada Lovelace; Alan Turing · 2024 · 期刊论文";
            app.snapshot.cards={card}; app.snapshot.module="resources"; app.snapshot.tab="pdf";
            app.snapshot.count=1; app.snapshot.selected_id=card.id;
        }
        if (detail_preview) {
            const auto& sample = rimes::capsule::ModuleContract().at("detailPreview");
            app.snapshot.count = 2;
            app.snapshot.detail = true;
            app.snapshot.detail_revision = std::string(64, 'a');
            app.snapshot.detail_selected = sample.at("selected");
            app.snapshot.selected_title = sample.at("title");
            app.snapshot.selected_preview = sample.at("preview");
            std::vector<rimes::capsule::DetailRow> headers{
                {"field", sample.at("header").at("label"), sample.at("header").at("value"), false}};
            for (const auto& section :
                 rimes::capsule::NoteSections(sample.at("body").get<std::string>(), headers))
                for (const auto& row : section.rows) {
                    app.snapshot.detail_rows.push_back(row);
                    app.snapshot.detail_headings.push_back(section.heading);
                }
            app.snapshot.detail_count = app.snapshot.detail_rows.size();
            app.detail_copy_index = 1;
            app.detail_copy_id = app.snapshot.selected_id;
            app.detail_copy_until = g_get_monotonic_time() + 1200000;
            app.snapshot.hint = "↑ ↓ SELECT   ↩ INSERT ROW   Ctrl+C COPY ROW   "
                                "DOUBLE-CLICK INSERT   ESC BACK";
        }
        if (clipboard_preview) {
            app.snapshot.module = "temporary";
            app.snapshot.tab = "recent";
            app.snapshot.cards.clear();
            auto image = demo;
            image.id = "72696d65-7300-4000-8000-000000000099";
            image.title = "图像";
            image.preview = "图像";
            image.kind = rimes::capsule::Kind::Image;
            image.module = rimes::capsule::Module::Temporary;
            image.filter = "image";
            image.source = "Safari";
            app.snapshot.cards.push_back(image);
            const auto& texts = rimes::capsule::ModuleContract().at("clipboard").at("previewTexts");
            for (int i = 3; i >= 0; --i) {
                auto card = demo;
                card.id = "72696d65-7300-4000-8000-00000000000" + std::to_string(i + 2);
                card.module = rimes::capsule::Module::Temporary;
                card.filter = "text";
                card.title = texts[i];
                card.preview = texts[i];
                card.source = "Safari";
                app.snapshot.cards.push_back(card);
            }
            app.snapshot.selected_id = image.id;
            app.snapshot.count = app.snapshot.cards.size();
            app.snapshot.hint = "TYPE TO SEARCH   ← → SELECT   ↑ ↓ FILTER   ⇥ NEXT TAB   ↩ INSERT  "
                                " Ctrl+C COPY   DELETE REMOVE   ESC CLOSE";
        }
        ApplySnapshot(&app);
        if (!render_path.empty()) {
            if(manager_preview) gtk_window_set_focus(GTK_WINDOW(app.manager->Window()),nullptr);
            for (int i = 0; i < 30; ++i) { while (gtk_events_pending()) gtk_main_iteration(); g_usleep(10000); }
            return rimes::desktop::Render(capture_preview ? app.card_rows.front().event : manager_preview ? app.manager->Root() : gtk_bin_get_child(GTK_BIN(app.window)), render_path) ? 0 : 1;
        }
        if (!app.dump_geometry.empty()) {
            g_timeout_add(
                200,
                [](gpointer data) -> gboolean {
                    WriteGeometryDump(static_cast<App*>(data));
                    return G_SOURCE_REMOVE;
                },
                &app);
        }
        if (app.quit_after_dump) {
            g_timeout_add(
                2000,
                [](gpointer data) -> gboolean {
                    WriteGeometryDump(static_cast<App*>(data));
                    gtk_main_quit();
                    return G_SOURCE_REMOVE;
                },
                &app);
        }
        gtk_main();
        return 0;
    }

    app.clipboard = std::make_unique<rimes::capsule::ClipboardController>(
        [&app](auto command) { SendCommand(&app, command); },
        [&app](auto status) {
            gtk_label_set_text(GTK_LABEL(app.hint), status.c_str());
            app.manager->ClipboardStatus(status);
        });
    if (!ConnectSocket(&app, socket_path)) {
        g_printerr("rimes-capsule: could not connect to %s\n", socket_path.c_str());
        return 1;
    }
    gtk_main();
    if (app.reconnect_id)
        g_source_remove(app.reconnect_id);
    if (app.detail_copy_timer)
        g_source_remove(app.detail_copy_timer);
    if (app.io_id)
        g_source_remove(app.io_id);
    if (app.socket_fd >= 0) {
        close(app.socket_fd);
    }
    return 0;
}
