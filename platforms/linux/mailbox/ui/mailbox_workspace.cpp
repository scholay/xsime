#include "background_jobs.hpp"
#include "capsule_text.hpp"
#include "desktop.hpp"
#include "launcher_client.hpp"
#include "mailbox_tokens.hpp"
#include "mailbox_workspace.hpp"
#include "official_catalog.hpp"
#include <algorithm>
#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <set>

namespace rimes::mailbox {
namespace {
using namespace rimes::desktop;
const Json& Tokens() {
    static const auto value = Json::parse(kMailboxTokens);
    return value;
}
int Metric(const char* name) { return Tokens().at("metrics").at(name); }
std::string Plugin(const std::string& module) { return "builtin.mailbox." + module; }
bool IsModule(const std::string& module) {
    return module == "inbox" || module == "chat" || module == "terminal";
}
std::string Caption(const std::string& module) {
    for (const auto& entry : Tokens().at("modules"))
        if (entry.at("id") == module)
            return entry.at("title");
    return module;
}
void Post(std::function<void()> fn) {
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        +[](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(fn)),
        +[](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}
GtkWidget* Named(GtkWidget* widget, const char* name) {
    gtk_widget_set_name(widget, name);
    return widget;
}
GtkWidget* Pad(GtkWidget* widget, int padding = 16) {
    auto* box = Column(0);
    gtk_container_set_border_width(GTK_CONTAINER(box), padding);
    Add(box, widget, true);
    return box;
}
GtkWidget* Surface(GtkWidget* child, const char* style) {
    auto* surface = Class(gtk_event_box_new(), style);
    gtk_container_add(GTK_CONTAINER(surface), child);
    return surface;
}
GtkWidget* Scroll(GtkWidget* child) {
    auto* view = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(view), child);
    return view;
}
GtkWidget* LineLabel(const std::string& text, const char* style, int line_height) {
    auto* label = Label(text, style);
    auto* attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes,
                           pango_attr_line_height_new_absolute(line_height * PANGO_SCALE));
    gtk_label_set_attributes(GTK_LABEL(label), attributes);
    pango_attr_list_unref(attributes);
    return label;
}
const Json& ModuleContract(const std::string& module) {
    for (const auto& value : Tokens().at("modules"))
        if (value.at("id") == module)
            return value;
    throw std::runtime_error("Unknown Mailbox module");
}
GtkWidget* Paragraph(const std::string& body, const char* css = "mailbox-body") {
    auto* label = Label(body, css);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_NONE);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    return label;
}
std::string Text(GtkWidget* view) {
    auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter a{}, b{};
    gtk_text_buffer_get_bounds(buffer, &a, &b);
    auto* raw = gtk_text_buffer_get_text(buffer, &a, &b, FALSE);
    std::string result = raw;
    g_free(raw);
    return result;
}
std::string Time(double seconds) {
    if (seconds < -62135596800.0 || seconds > 253402300799.0)
        return "";
    auto value = static_cast<std::time_t>(seconds + 978307200.0);
    std::tm local{};
    localtime_r(&value, &local);
    char buffer[32]{};
    auto now = std::time(nullptr);
    std::tm today{};
    localtime_r(&now, &today);
    const bool same_day = today.tm_year == local.tm_year && today.tm_yday == local.tm_yday;
    std::strftime(buffer, sizeof(buffer), same_day ? "%H:%M" : "%m-%d %H:%M", &local);
    return buffer;
}
void ApplyTheme(const std::string& requested, GtkWidget* root) {
    auto theme = requested == "liquidGlass" ? "day" : requested;
    if (!Tokens().at("palettes").contains(theme))
        theme = "night";
    Theme(theme);
    const auto& palette = Tokens().at("palettes").at(theme);
    auto c = [&](const char* name) { return palette.at(name).get<std::string>(); };
    const std::string css =
        ".mailbox, .mailbox viewport, .mailbox textview, .mailbox textview text { background: " +
        c("background") + "; color: " + c("label") +
        "; }"
        ".mailbox * { font-family: 'SF Pro Text', Inter, 'Noto Sans CJK SC', sans-serif; "
        "font-size: 13px; }"
        ".mailbox label { color: " +
        c("label") + "; } .mailbox .mailbox-secondary { color: " + c("secondary") +
        "; }"
        ".mailbox .mailbox-side, .mailbox .mailbox-note, .mailbox .mailbox-composer, .mailbox "
        ".mailbox-composer textview, .mailbox .mailbox-composer text { background: " +
        c("sidebar") +
        "; }"
        ".mailbox .mailbox-brand { font-size: 22px; font-weight: 600; color: " +
        c("accent") +
        "; }"
        ".mailbox .mailbox-heading { font-size: 16px; font-weight: 600; } .mailbox "
        ".mailbox-list-title { font-size: 19px; font-weight: 600; }"
        ".mailbox .mailbox-source, .mailbox .mailbox-hint { font-size: 11px; color: " +
        c("secondary") +
        "; }"
        ".mailbox .mailbox-time { font-size: 10px; color: " +
        c("secondary") +
        "; } .mailbox .mailbox-author { font-size: 12px; font-weight: 600; }"
        ".mailbox .mailbox-body { font-size: 14px; } .mailbox .mailbox-welcome { font-size: 23px; "
        "font-weight: 500; }"
        ".mailbox button { background: " +
        c("control") + "; color: " + c("label") +
        "; min-width: 0; min-height: 0; padding: 0 10px; border: 1px solid alpha(" +
        c("separator") +
        ", 0.3); border-radius: 6px; }"
        ".mailbox button label { color: inherit; font-size: 12px; } .mailbox button:hover { "
        "background: " +
        c("selection") +
        "; }"
        ".mailbox button.mailbox-nav { background: transparent; border: 0; border-radius: 8px; "
        "padding: 0; }"
        ".mailbox button.mailbox-nav label { font-size: 11px; color: " +
        c("secondary") +
        "; }"
        ".mailbox button.mailbox-nav.selected { background: " +
        c("selection") +
        "; } .mailbox button.mailbox-nav.selected label, .mailbox button.mailbox-nav.selected "
        ".glyph { color: " +
        c("accent") +
        "; font-weight: 600; }"
        ".mailbox .mailbox-badge { background: " +
        c("accent") + "; color: " + c("background") +
        "; border-radius: 7px; font-size: 9px; font-weight: 600; }"
        ".mailbox button.mailbox-list-row { background: transparent; border: 0; padding: 0; "
        "border-radius: 7px; margin: 3px 7px; }"
        ".mailbox button.mailbox-list-row.selected { background: " +
        c("selection") +
        "; } .mailbox .mailbox-list-name { font-size: 13px; font-weight: 600; }"
        ".mailbox .mailbox-list-preview { font-size: 11px; color: " +
        c("secondary") +
        "; } .mailbox .mailbox-list-meta { font-size: 10px; color: " + c("tertiary") +
        "; }"
        ".mailbox .mailbox-unread { color: " +
        c("accent") +
        "; }"
        ".mailbox button.mailbox-tool { background: transparent; border: 0; padding: 0; } .mailbox "
        "button.mailbox-tool:hover { background: " +
        c("selection") +
        "; }"
        ".mailbox button.mailbox-send { background: " +
        c("accent") + "; color: " + c("background") +
        "; }"
        ".mailbox .mailbox-composer { border-radius: 12px; } .mailbox .mailbox-composer textview { "
        "font-size: 14px; }"
        ".mailbox .mailbox-divider { background: " +
        c("separator") +
        "; min-height: 1px; }"
        ".mailbox entry { background: alpha(" +
        c("label") + ", 0.1); color: " + c("label") +
        "; min-height: 18px; padding: 0 5px; font-size: 12px; }"
        ".mailbox button.mailbox-filter { border-radius: 0; padding: 0 2px; min-height: 20px; "
        "background: alpha(" +
        c("label") +
        ", 0.04); border: 0; }"
        ".mailbox button.mailbox-filter label { font-size: 10px; } .mailbox "
        "button.mailbox-filter.selected { background: alpha(" +
        c("label") +
        ", 0.25); border-radius: 5px; }"
        ".mailbox .mailbox-scrim { background: alpha(black, 0.08); border: 0; border-radius: 0; }";
    const std::string details =
        ".mailbox .mailbox-side viewport { background: " + c("sidebar") +
        "; }"
        ".mailbox button.mailbox-nav label.mailbox-badge, .mailbox button.mailbox-nav.selected "
        "label.mailbox-badge { font-size: 9px; color: " +
        c("background") +
        "; }"
        ".mailbox .mailbox-placeholder { font-size: 14px; color: " +
        c("tertiary") +
        "; }"
        ".mailbox button.mailbox-send:disabled { background: " +
        c("background") + "; color: " + c("tertiary") + "; opacity: 1; }";
    auto* provider = gtk_css_provider_new();
    GError* error = nullptr;
    if (!gtk_css_provider_load_from_data(provider, (css + details).c_str(), -1, &error)) {
        g_clear_error(&error);
        g_object_unref(provider);
        throw std::runtime_error("Mailbox 主题无法加载");
    }
    auto* old = static_cast<GtkCssProvider*>(g_object_get_data(G_OBJECT(root), "mailbox-css"));
    if (old)
        gtk_style_context_remove_provider_for_screen(gtk_widget_get_screen(root),
                                                     GTK_STYLE_PROVIDER(old));
    gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(root),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_set_data_full(G_OBJECT(root), "mailbox-css", provider, g_object_unref);
}
} // namespace
struct Workspace::Impl {
    struct Access {
        std::mutex mutex;
        bool protected_session = true, alive = true;
        std::uint64_t epoch = 0;
    };
    const bool fixture;
    std::filesystem::path root = runtime::UserRoot() / "mailbox";
    plugins::PackageStore packages{runtime::UserRoot() / "plugins", plugins::OfficialCatalog(),
                                   plugins::BundledPackages(), plugins::CompiledAdapters(),
                                   RIMES_PRODUCT_VERSION};
    std::shared_ptr<Access> access = std::make_shared<Access>();
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> load_token = std::make_shared<std::atomic<bool>>(false);
    runtime::BackgroundJobs jobs;
    GtkWidget *window, *root_view, *rail, *navigation, *content, *overlay, *layout, *side, *main,
        *index, *search, *filters, *heading, *source, *toolbar, *transcript, *composer, *hint,
        *send, *feedback, *scrim, *placeholder;
    GtkWidget* badge = nullptr;
    std::map<std::string, GtkWidget*> nav;
    std::map<std::string, std::string> grants, selections, drafts, queries, filter_values;
    std::map<std::string, std::uint64_t> draft_revisions;
    std::map<std::string, std::optional<bool>> sidebar_preferences;
    Snapshot state{{{"schemaVersion", 1}, {"nextSequence", 1}, {"threads", Json::array()}}, ""};
    std::string module = "inbox", selected, theme = "night", rendered_revision;
    bool busy = false, ready = false, storage_available = false, restoring = false, narrow = false,
         sidebar_visible = false, overlay_sidebar = false, prefs_available = true;
    int width = 1000, height = 700;
    GtkWidget* dialog = nullptr;
    std::map<std::string, bool> creating;
    guint geometry_timer = 0, refresh_timer = 0;
    bool refresh_running = false, refresh_again = false;
    unsigned refresh_backoff = 50;
    std::vector<GFileMonitor*> monitors;

    Impl(GtkApplication* app, bool preview) : fixture(preview) {
        access->protected_session = !fixture;
        window = fixture ? gtk_offscreen_window_new()
                 : app   ? gtk_application_window_new(app)
                         : gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "Mailbox");
        gtk_window_set_role(GTK_WINDOW(window), "xsime-mailbox");
        gtk_window_set_default_size(GTK_WINDOW(window), Metric("defaultWidth"),
                                    Metric("defaultHeight"));
        GdkGeometry minimum{};
        minimum.min_width = Metric("minimumWidth");
        minimum.min_height = Metric("minimumHeight");
        gtk_window_set_geometry_hints(GTK_WINDOW(window), nullptr, &minimum, GDK_HINT_MIN_SIZE);
        root_view = Named(Class(Row(0), "mailbox"), "mailbox-root");
        gtk_container_add(GTK_CONTAINER(window), root_view);
        rail = Class(Column(20), "mailbox-side");
        gtk_widget_set_size_request(rail, Metric("railWidth"), -1);
        Add(root_view, rail);
        auto* rail_inner = Column(20);
        gtk_container_set_border_width(GTK_CONTAINER(rail_inner), 8);
        gtk_widget_set_margin_top(rail_inner, 8);
        gtk_widget_set_margin_bottom(rail_inner, 4);
        Add(rail, rail_inner, true);
        auto* brand = Label("M", "mailbox-brand");
        gtk_label_set_xalign(GTK_LABEL(brand), 0.5);
        Add(rail_inner, brand);
        navigation = Column(8);
        Add(rail_inner, navigation);
        Add(rail_inner, gtk_label_new(""), true);
        auto* settings = IconButton("slider.horizontal.3", "管理 Mailbox 插件", [this] {
            LauncherCall("Open", "settings", nullptr, [life = alive, this](Json result) {
                if (life->load() && !result.value("ok", false))
                    Notice(result.value("message", "无法打开设置"));
            });
        });
        gtk_widget_set_halign(settings, GTK_ALIGN_CENTER);
        gtk_widget_set_size_request(settings, 28, 28);
        Add(rail_inner, settings);
        content = Column(0);
        Add(root_view, content, true);
        BuildMessages();
        ApplyTheme(theme, root_view);
        g_signal_connect(window, "delete-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean {
                             static_cast<Impl*>(p)->Hide();
                             return TRUE;
                         }),
                         this);
        g_signal_connect_after(window, "focus-in-event",
                               G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer p) -> gboolean {
                                   static_cast<Impl*>(p)->MarkRead();
                                   return FALSE;
                               }),
                               this);
        g_signal_connect(window, "key-press-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer p) -> gboolean {
                             auto* self = static_cast<Impl*>(p);
                             if (event->keyval == GDK_KEY_Escape) {
                                 if (self->overlay_sidebar)
                                     self->Dismiss();
                                 else
                                     self->Hide();
                                 return TRUE;
                             }
                             if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_w) {
                                 self->Hide();
                                 return TRUE;
                             }
                             return FALSE;
                         }),
                         this);
        g_signal_connect(
            window, "configure-event",
            G_CALLBACK(+[](GtkWidget*, GdkEventConfigure* event, gpointer p) -> gboolean {
                auto* self = static_cast<Impl*>(p);
                if (!self->fixture && gtk_widget_get_visible(self->window) &&
                    !gtk_window_is_maximized(GTK_WINDOW(self->window))) {
                    self->width = event->width;
                    self->height = event->height;
                    self->SaveGeometrySoon();
                }
                return FALSE;
            }),
            this);
        if (!fixture) {
            Watch(root);
            Watch(runtime::UserRoot() / "plugins");
            Watch(runtime::UserRoot() / "configuration");
            Refresh();
        }
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(access->mutex);
            access->alive = false;
            ++access->epoch;
        }
        alive->store(false);
        load_token->store(true);
        if (geometry_timer)
            g_source_remove(geometry_timer);
        if (refresh_timer)
            g_source_remove(refresh_timer);
        for (auto* monitor : monitors) {
            g_file_monitor_cancel(monitor);
            g_object_unref(monitor);
        }
        jobs.Shutdown();
        if (dialog)
            gtk_widget_destroy(dialog);
        if (auto* provider =
                static_cast<GtkCssProvider*>(g_object_get_data(G_OBJECT(root_view), "mailbox-css")))
            gtk_style_context_remove_provider_for_screen(gtk_widget_get_screen(root_view),
                                                         GTK_STYLE_PROVIDER(provider));
        g_signal_handlers_disconnect_by_data(window, this);
        gtk_widget_destroy(window);
    }
    void Notice(const std::string& message) {
        gtk_label_set_text(GTK_LABEL(feedback), message.c_str());
        gtk_widget_set_visible(feedback, !message.empty());
    }
    void BuildMessages() {
        overlay = Named(gtk_overlay_new(), "mailbox-workspace");
        Add(content, overlay, true);
        layout = Row(0);
        gtk_container_add(GTK_CONTAINER(overlay), layout);
        main = Column(0);
        side = Class(Column(0), "mailbox-side");
        Add(layout, side);
        Add(layout, main, true);
        auto* title_row = Row(0);
        auto* list_title = Named(Label("收件箱", "mailbox-list-title"), "mailbox-list-title");
        Add(title_row, list_title, true);
        auto* new_chat =
            Named(IconButton("square.and.pencil", "新对话", [this] { SelectThread(""); }),
                  "mailbox-new-chat");
        Class(new_chat, "mailbox-tool");
        gtk_widget_set_no_show_all(new_chat, TRUE);
        Add(title_row, new_chat);
        auto* close = IconButton("chevron.left", "收起列表", [this] { ToggleSidebar(); });
        Class(close, "mailbox-tool");
        Add(title_row, close);
        search = Named(gtk_search_entry_new(), "mailbox-search");
        g_signal_connect(search, "search-changed",
                         G_CALLBACK(+[](GtkSearchEntry* entry, gpointer p) {
                             auto* self = static_cast<Impl*>(p);
                             self->queries[self->module] = gtk_entry_get_text(GTK_ENTRY(entry));
                             self->RenderIndex();
                         }),
                         this);
        filters = Row(0);
        gtk_box_set_homogeneous(GTK_BOX(filters), TRUE);
        for (const auto& pair :
             std::vector<std::pair<std::string, std::string>>{{"all", "全部"},
                                                              {"unread", "未读"},
                                                              {"attention", "待处理"},
                                                              {"archived", "归档"}}) {
            auto* button = Button(
                pair.second,
                [this, id = pair.first] {
                    filter_values[module] = id;
                    RenderIndex();
                },
                "mailbox-filter");
            Named(button, ("mailbox-filter-" + pair.first).c_str());
            Add(filters, button, true);
        }
        auto* list_header = Column(12);
        Add(list_header, title_row);
        Add(list_header, search);
        Add(list_header, filters);
        Add(side, Pad(list_header));
        index = Named(Column(0), "mailbox-index");
        Add(side, Scroll(index), true);
        auto* top = Row(12);
        auto* titles = Column(4);
        heading = Named(LineLabel("", "mailbox-heading", 19), "mailbox-heading");
        source = LineLabel("", "mailbox-source", 13);
        Add(titles, heading);
        Add(titles, source);
        Add(top, titles, true);
        toolbar = Row(4);
        Add(top, toolbar);
        Add(main, Pad(top));
        auto* line = Class(gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), "mailbox-divider");
        gtk_widget_set_size_request(line, -1, 1);
        Add(main, line);
        transcript = Named(Column(0), "mailbox-transcript");
        Add(main, Scroll(transcript), true);
        composer = Named(gtk_text_view_new(), "mailbox-composer-input");
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(composer), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(composer), 6);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(composer), 6);
        auto* scroll = Scroll(composer);
        gtk_widget_set_size_request(scroll, -1, 60);
        auto* composer_overlay = gtk_overlay_new();
        gtk_container_add(GTK_CONTAINER(composer_overlay), scroll);
        placeholder = Label("写下本地备注…", "mailbox-placeholder");
        gtk_widget_set_halign(placeholder, GTK_ALIGN_START);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_START);
        gtk_widget_set_margin_top(placeholder, 6);
        gtk_widget_set_no_show_all(placeholder, TRUE);
        gtk_overlay_add_overlay(GTK_OVERLAY(composer_overlay), placeholder);
        gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(composer_overlay), placeholder, TRUE);
        auto* actions = Row(8);
        hint = Label("", "mailbox-hint");
        Add(actions, hint, true);
        send = Named(Button("添加备注", [this] { Submit(); }, "mailbox-send"), "mailbox-send");
        gtk_widget_set_size_request(send, -1, 28);
        Add(actions, send);
        auto* input = Column(5);
        Add(input, composer_overlay);
        Add(input, actions);
        auto* input_frame = Surface(Pad(input, 10), "mailbox-composer");
        auto* footer = Column(7);
        Add(footer, input_frame);
        feedback = Named(Label("", "mailbox-hint"), "mailbox-feedback");
        Add(footer, feedback);
        Add(main, Pad(footer));
        gtk_widget_set_no_show_all(feedback, TRUE);
        gtk_widget_hide(feedback);
        g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer)), "changed",
                         G_CALLBACK(+[](GtkTextBuffer*, gpointer p) {
                             auto* self = static_cast<Impl*>(p);
                             if (!self->restoring) {
                                 self->drafts[self->DraftKey()] = Text(self->composer);
                                 ++self->draft_revisions[self->DraftKey()];
                             }
                             self->ComposerState();
                         }),
                         this);
        // Give the active input method first refusal on Return; a preedit commit
        // must never submit the composer. Shift/Alt+Return insert a newline.
        g_signal_connect(composer, "key-press-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer p) -> gboolean {
                             if (event->keyval == GDK_KEY_Return &&
                                 !(event->state & (GDK_SHIFT_MASK | GDK_MOD1_MASK))) {
                                 auto* self = static_cast<Impl*>(p);
                                 if (gtk_text_view_im_context_filter_keypress(
                                         GTK_TEXT_VIEW(self->composer), event))
                                     return TRUE;
                                 self->Submit();
                                 return TRUE;
                             }
                             return FALSE;
                         }),
                         this);
        scrim = Class(Button("", [this] { Dismiss(); }), "mailbox-scrim");
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), scrim);
        gtk_widget_set_no_show_all(scrim, TRUE);
        gtk_widget_hide(scrim);
        atk_object_set_name(gtk_widget_get_accessible(scrim), "收起侧栏");
        g_signal_connect(overlay, "size-allocate",
                         G_CALLBACK(+[](GtkWidget*, GdkRectangle* area, gpointer p) {
                             static_cast<Impl*>(p)->Layout(area->width);
                         }),
                         this);
    }
    GtkWidget* Find(GtkWidget* widget, const char* name) {
        if (std::string(gtk_widget_get_name(widget)) == name)
            return widget;
        if (!GTK_IS_CONTAINER(widget))
            return nullptr;
        auto* children = gtk_container_get_children(GTK_CONTAINER(widget));
        GtkWidget* result = nullptr;
        for (auto* n = children; n && !result; n = n->next)
            result = Find(GTK_WIDGET(n->data), name);
        g_list_free(children);
        return result;
    }
    std::string DraftKey() const { return module + ":" + selected; }
    bool Protected() const {
        std::lock_guard<std::mutex> lock(access->mutex);
        return access->protected_session;
    }
    bool Allowed() const {
        return !Protected() && grants.count(module) && storage_available && !fixture;
    }
    const Json* Selected() const {
        for (const auto& thread : state.document.at("threads"))
            if (thread.at("id") == selected)
                return &thread;
        return nullptr;
    }
    void Revoke() {
        std::lock_guard<std::mutex> lock(access->mutex);
        ++access->epoch;
    }
    void Hide() {
        Revoke();
        if (dialog)
            gtk_widget_destroy(dialog);
        gtk_widget_hide(window);
    }
    void Protect(bool value) {
        {
            std::lock_guard<std::mutex> lock(access->mutex);
            access->protected_session = value;
            if (value)
                ++access->epoch;
        }
        if (value) {
            if (dialog)
                gtk_widget_destroy(dialog);
            gtk_widget_hide(window);
        }
        ComposerState();
    }
    void RenderNavigation() {
        Clear(navigation);
        nav.clear();
        badge = nullptr;
        for (const auto& name : {"terminal", "chat", "inbox"}) {
            if (!grants.count(name))
                continue;
            auto* button = Button("", [this, name] { SelectModule(name); }, "mailbox-nav");
            Named(button, ("mailbox-nav-" + std::string(name)).c_str());
            gtk_container_remove(GTK_CONTAINER(button), gtk_bin_get_child(GTK_BIN(button)));
            auto* face = gtk_overlay_new();
            auto* column = Column(5);
            gtk_widget_set_margin_top(column, 8);
            gtk_widget_set_margin_bottom(column, 7);
            auto* icon = Icon(ModuleContract(name).at("symbol"), 18);
            gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
            Add(column, icon);
            auto* label = Label(Caption(name));
            gtk_label_set_xalign(GTK_LABEL(label), 0.5);
            Add(column, label);
            gtk_container_add(GTK_CONTAINER(face), column);
            if (std::string(name) == "inbox") {
                badge = Class(Label(""), "mailbox-badge");
                gtk_label_set_xalign(GTK_LABEL(badge), 0.5);
                gtk_widget_set_size_request(badge, 16, 14);
                gtk_widget_set_halign(badge, GTK_ALIGN_END);
                gtk_widget_set_valign(badge, GTK_ALIGN_START);
                gtk_widget_set_margin_top(badge, 5);
                gtk_overlay_add_overlay(GTK_OVERLAY(face), badge);
            }
            gtk_container_add(GTK_CONTAINER(button), face);
            gtk_widget_set_size_request(button, Metric("navigationWidth"),
                                        Metric("navigationHeight"));
            atk_object_set_name(gtk_widget_get_accessible(button), Caption(name).c_str());
            Add(navigation, button);
            nav[name] = button;
        }
        gtk_widget_show_all(navigation);
        UpdateNavigation();
    }
    void UpdateNavigation() {
        for (const auto& pair : nav) {
            auto* context = gtk_widget_get_style_context(pair.second);
            if (pair.first == module)
                gtk_style_context_add_class(context, "selected");
            else
                gtk_style_context_remove_class(context, "selected");
        }
        if (badge) {
            auto count = Unread(state);
            auto title = count > 9 ? "9+" : std::to_string(count);
            gtk_label_set_text(GTK_LABEL(badge), title.c_str());
            gtk_widget_set_visible(badge, count > 0);
            auto label = count ? "收件，" + std::to_string(count) + " 条未读" : "收件";
            atk_object_set_name(gtk_widget_get_accessible(nav.at("inbox")), label.c_str());
        }
    }
    void SelectModule(const std::string& requested) {
        if (!IsModule(requested) || !grants.count(requested))
            return;
        Revoke();
        module = requested;
        selected = selections[module];
        rendered_revision.clear();
        gtk_entry_set_text(GTK_ENTRY(search), queries[module].c_str());
        gtk_entry_set_placeholder_text(GTK_ENTRY(search),
                                       module == "chat" ? "搜索对话" : "搜索收件");
        gtk_label_set_text(GTK_LABEL(Find(side, "mailbox-list-title")), module == "inbox" ? "收件箱"
                                                                        : module == "chat"
                                                                            ? "对话"
                                                                            : "会话");
        gtk_widget_set_visible(Find(side, "mailbox-new-chat"), module == "chat");
        EnsureSelection();
        RestoreDraft();
        RenderIndex();
        RenderDetail();
        UpdateNavigation();
        Layout(gtk_widget_get_allocated_width(overlay));
        SaveGeometrySoon();
        MarkRead();
    }
    void EnsureSelection() {
        if (Selected() && Module(*Selected()) == module)
            return;
        selected.clear();
        if (creating[module])
            return;
        const auto rows = Filter(state, module, queries[module],
                                 filter_values.count(module) ? filter_values[module] : "all");
        if (!rows.empty())
            selected = rows.front().at("id");
        selections[module] = selected;
    }
    void SelectThread(const std::string& id) {
        if (busy)
            return;
        const auto old = selected;
        selected = id;
        if (!id.empty() && (!Selected() || Module(*Selected()) != module)) {
            selected = old;
            return;
        }
        Revoke();
        creating[module] = id.empty() && module == "chat";
        selections[module] = selected;
        rendered_revision.clear();
        if (const auto* thread = Selected())
            filter_values[module] =
                thread->contains("archivedAt") && !thread->at("archivedAt").is_null() ? "archived"
                                                                                      : "all";
        RestoreDraft();
        RenderIndex();
        RenderDetail();
        Dismiss();
        MarkRead();
    }
    void RestoreDraft() {
        restoring = true;
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer)),
                                 drafts[DraftKey()].c_str(), -1);
        restoring = false;
        ComposerState();
    }
    void RenderIndex() {
        if (!index)
            return;
        Clear(index);
        const auto filter = filter_values.count(module) ? filter_values[module] : "all";
        for (const auto& key : {"all", "unread", "attention", "archived"}) {
            auto* button = Find(filters, ("mailbox-filter-" + std::string(key)).c_str());
            auto* style = gtk_widget_get_style_context(button);
            if (filter == key)
                gtk_style_context_add_class(style, "selected");
            else
                gtk_style_context_remove_class(style, "selected");
            const auto& permitted = ModuleContract(module).at("filters");
            gtk_widget_set_sensitive(button, std::find(permitted.begin(), permitted.end(), key) !=
                                                 permitted.end());
        }
        const auto rows = Filter(state, module, queries[module], filter);
        if (rows.empty())
            Add(index,
                Pad(Paragraph(!queries[module].empty() ? "没有找到匹配的内容。"
                              : module == "chat"       ? "在这里开始新的对话。"
                              : module == "terminal"   ? "暂无终端会话。"
                                                     : "通知、任务产物和外部来信\n会汇集在这里。",
                              "mailbox-secondary"),
                    20));
        for (const auto& thread : rows) {
            const std::string id = thread.at("id");
            auto* button = Named(Button(
                                     "", [this, id] { SelectThread(id); }, "mailbox-list-row"),
                                 ("mailbox-thread-" + id).c_str());
            if (id == selected)
                Class(button, "selected");
            gtk_container_remove(GTK_CONTAINER(button), gtk_bin_get_child(GTK_BIN(button)));
            auto* lines = Column(3);
            auto* title = Row(4);
            Add(title, LineLabel(Title(thread), "mailbox-list-name", 19), true);
            if (thread.at("unread") == true)
                Add(title, Label("•", "mailbox-unread"));
            Add(lines, title);
            Add(lines, LineLabel(capsule::CharacterPrefix(Preview(thread), 160),
                                 "mailbox-list-preview", 19));
            Add(lines, LineLabel(thread.at("source").at("displayName").get<std::string>() + " · " +
                                     Time(thread.at("updatedAt")) + " · " + Status(thread),
                                 "mailbox-list-meta", 19));
            auto* row_content = Pad(lines, 0);
            gtk_widget_set_margin_top(row_content, 8);
            gtk_widget_set_margin_bottom(row_content, 7);
            gtk_widget_set_margin_start(row_content, 11);
            gtk_widget_set_margin_end(row_content, 15);
            gtk_container_add(GTK_CONTAINER(button), row_content);
            gtk_widget_set_size_request(button, -1, Metric("rowHeight") - 6);
            atk_object_set_name(gtk_widget_get_accessible(button), Title(thread).c_str());
            Add(index, button);
        }
        gtk_widget_show_all(index);
    }
    GtkWidget* Tool(const char* icon, const char* title, std::function<void()> fn) {
        auto* button = Class(IconButton(icon, title, std::move(fn)), "mailbox-tool");
        gtk_widget_set_size_request(button, 28, 28);
        return button;
    }
    void RenderDetail() {
        const auto* thread = Selected();
        const auto revision = module + ":" + (thread ? Revision(*thread) : "empty");
        if (revision == rendered_revision) {
            ComposerState();
            return;
        }
        rendered_revision = revision;
        Clear(toolbar);
        Clear(transcript);
        Add(toolbar, Tool("sidebar.left", "显示或隐藏列表", [this] { ToggleSidebar(); }));
        if (!thread) {
            gtk_label_set_text(GTK_LABEL(heading), module == "chat"       ? "新对话"
                                                   : module == "terminal" ? "终端"
                                                                          : "收件箱");
            gtk_label_set_text(GTK_LABEL(source), module == "chat" ? "选择连接器，开始一个独立会话"
                                                  : module == "terminal"
                                                      ? "独立会话，保留工作现场"
                                                      : "让完成的工作，有一个落点");
            auto* welcome = Column(18);
            Add(welcome, Label(module == "chat"       ? "今天想一起做什么？"
                               : module == "terminal" ? "暂无终端会话"
                                                      : "等待下一份收件",
                               "mailbox-welcome"));
            Add(welcome,
                Paragraph(
                    module == "chat" ? "继续思考、整理文字，或开始一项新任务。\n每个会话独立保留模"
                                       "型、上下文和草稿。"
                    : module == "terminal"
                        ? "终端运行组件尚未可用。"
                        : "从终端收取产物，接收 Buffer 生成的图片，\n或查看已连接来源推送的消息。",
                    "mailbox-secondary"));
            Add(transcript, Pad(welcome));
        } else {
            gtk_label_set_text(GTK_LABEL(heading), Title(*thread).c_str());
            gtk_widget_set_tooltip_text(heading, Title(*thread).c_str());
            std::string detail = thread->at("source").at("displayName");
            if (thread->at("source").contains("model") &&
                !thread->at("source").at("model").is_null())
                detail += " · " + thread->at("source").at("model").get<std::string>();
            detail += " · " + Status(*thread);
            gtk_label_set_text(GTK_LABEL(source), detail.c_str());
            Add(toolbar, Tool("pencil", "重命名", [this] { Rename(); }));
            const bool archived =
                thread->contains("archivedAt") && !thread->at("archivedAt").is_null();
            Add(toolbar, Tool("archivebox", archived ? "移回收件" : "归档", [this, archived] {
                    Mutate([archived](Store& store, const Json& row) {
                        store.Archive(row.at("id"), Revision(row), !archived);
                    });
                }));
            for (const auto& message : thread->at("messages")) {
                auto* block = Column(12);
                auto* identity = Row(8);
                const bool local = message.at("kind") == "localNote";
                const std::string author =
                    local ? "本地备注"
                    : message.at("role") == "user"
                        ? "你"
                        : (message.contains("author") && !message.at("author").is_null()
                               ? message.at("author").get<std::string>()
                               : thread->at("source").at("displayName").get<std::string>());
                Add(identity, LineLabel(author, "mailbox-author", 14), true);
                Add(identity, LineLabel(Time(message.at("createdAt")), "mailbox-time", 14));
                Add(block, identity);
                // Full text is selectable. Parsing rich bodies and review actions are
                // supplied by the inbox/chat adapters, not by the window host.
                Add(block, Paragraph(message.at("body")));
                if (message.contains("imageFileName") && !message.at("imageFileName").is_null())
                    Add(block, Label("图片附件", "mailbox-hint"));
                auto* padded = local ? Surface(Pad(block), "mailbox-note") : Pad(block);
                Add(transcript, padded);
            }
        }
        gtk_widget_show_all(toolbar);
        gtk_widget_show_all(transcript);
        ComposerState();
    }
    void ComposerState() {
        if (!send)
            return;
        const auto* thread = Selected();
        const bool note = thread && thread->at("source").at("replyCapability") == "localNotesOnly";
        gtk_label_set_text(GTK_LABEL(placeholder), note ? "写下本地备注…" : "输入消息…");
        gtk_widget_set_visible(placeholder, Text(composer).empty());
        gtk_button_set_label(GTK_BUTTON(send), note ? "添加备注" : "发送");
        gtk_label_set_text(GTK_LABEL(hint),
                           note ? "本地备注 · 不会发送给来源" : "选择连接器，开始对话");
        gtk_widget_set_sensitive(composer,
                                 !Protected() && grants.count(module) && module != "terminal");
        gtk_widget_set_sensitive(send, Allowed() && !busy && note &&
                                           !capsule::TrimText(Text(composer)).empty());
        atk_object_set_name(gtk_widget_get_accessible(composer), note ? "本地备注" : "消息输入框");
    }
    void Mutate(std::function<void(Store&, const Json&)> action,
                std::function<void()> completed = {}, std::function<void()> committed = {}) {
        if (!Allowed() || busy || !Selected())
            return;
        const auto row = *Selected();
        const auto grant = grants.at(module), plugin = Plugin(module);
        const auto life = alive;
        const auto authorization = access;
        std::uint64_t epoch;
        {
            std::lock_guard<std::mutex> lock(access->mutex);
            epoch = access->epoch;
        }
        busy = true;
        ComposerState();
        auto token = std::make_shared<std::atomic<bool>>(false);
        if (!jobs.Run(token, [this, life, authorization, epoch, row, grant, plugin, action,
                              completed, committed] {
                bool ok = false;
                try {
                    packages.WithEnabledGrant(
                        plugin, grant,
                        [&] {
                            Store store(root, {},
                                        [authorization, epoch](const std::function<void()>& write) {
                                            std::lock_guard<std::mutex> lock(authorization->mutex);
                                            if (authorization->alive &&
                                                !authorization->protected_session &&
                                                authorization->epoch == epoch)
                                                write();
                                        });
                            action(store, row);
                            ok = true;
                        },
                        false);
                } catch (const std::exception&) {
                }
                Post([this, life, authorization, epoch, ok, completed, committed] {
                    if (!life->load())
                        return;
                    busy = false;
                    if (ok && committed)
                        committed();
                    bool current;
                    {
                        std::lock_guard<std::mutex> lock(authorization->mutex);
                        current = authorization->epoch == epoch;
                    }
                    if (current) {
                        Notice(ok ? "已保存" : "操作未完成，数据或授权可能已变化；原内容已保留。");
                        if (ok && completed)
                            completed();
                    }
                    Refresh();
                    ComposerState();
                });
            })) {
            busy = false;
            Notice("后台繁忙，请稍后重试");
            ComposerState();
        }
    }
    void Submit() {
        if (!Selected() || Selected()->at("source").at("replyCapability") != "localNotesOnly")
            return;
        const auto body = Text(composer);
        if (capsule::TrimText(body).empty())
            return;
        const auto key = DraftKey();
        const auto draft_revision = draft_revisions[key];
        Mutate([body](Store& store,
                      const Json& row) { store.AddNote(row.at("id"), Revision(row), body); },
               {},
               [this, key, body, draft_revision] {
                   // Consume only the published draft, including after navigation.
                   // Text typed while saving belongs to a newer draft.
                   const auto saved = drafts.find(key);
                   if (saved != drafts.end() && saved->second == body &&
                       draft_revisions[key] == draft_revision) {
                       drafts.erase(saved);
                       if (DraftKey() == key && Text(composer) == body)
                           RestoreDraft();
                   }
               });
    }
    void Rename() {
        if (!Allowed() || busy || !Selected())
            return;
        const auto row = *Selected();
        dialog =
            gtk_dialog_new_with_buttons("重命名", GTK_WINDOW(window), GTK_DIALOG_MODAL, "取消",
                                        GTK_RESPONSE_CANCEL, "保存", GTK_RESPONSE_ACCEPT, nullptr);
        auto* input = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(input), Title(row).c_str());
        gtk_entry_set_max_length(GTK_ENTRY(input), 256);
        gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                          Pad(input));
        auto callback = new std::function<void(int)>([this, input, row](int response) {
            if (response == GTK_RESPONSE_ACCEPT && Allowed() && Selected() &&
                Revision(*Selected()) == Revision(row)) {
                const std::string title = gtk_entry_get_text(GTK_ENTRY(input));
                Mutate([title](Store& store, const Json& selected_row) {
                    store.Rename(selected_row.at("id"), Revision(selected_row), title);
                });
            }
            if (dialog)
                gtk_widget_destroy(dialog);
        });
        g_signal_connect_data(
            dialog, "response", G_CALLBACK(+[](GtkDialog*, gint response, gpointer raw) {
                auto fn = *static_cast<std::function<void(int)>*>(raw);
                fn(response);
            }),
            callback,
            +[](gpointer raw, GClosure*) { delete static_cast<std::function<void(int)>*>(raw); },
            GConnectFlags(0));
        g_signal_connect(dialog, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer raw) {
                             static_cast<Impl*>(raw)->dialog = nullptr;
                         }),
                         this);
        gtk_widget_show_all(dialog);
    }

    void MarkRead() {
        if (!Allowed() || busy || !Selected() || Selected()->at("unread") != true ||
            !gtk_window_is_active(GTK_WINDOW(window)) || !gtk_widget_get_visible(window))
            return;
        Mutate([](Store& store, const Json& row) { store.MarkRead(row.at("id"), Revision(row)); });
    }
    void Layout(int available) {
        narrow = available < Metric("sidebarBreakpoint");
        const bool show = sidebar_preferences[module].value_or(!narrow);
        const bool over = show && narrow;
        if (overlay_sidebar != over) {
            g_object_ref(side);
            gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(side)), side);
            if (over)
                gtk_overlay_add_overlay(GTK_OVERLAY(overlay), side);
            else {
                Add(layout, side);
                gtk_box_reorder_child(GTK_BOX(layout), side, 0);
            }
            g_object_unref(side);
        }
        overlay_sidebar = over;
        sidebar_visible = show;
        gtk_widget_set_halign(side, over ? GTK_ALIGN_START : GTK_ALIGN_FILL);
        gtk_widget_set_valign(side, GTK_ALIGN_FILL);
        gtk_widget_set_size_request(
            side, module == "inbox" ? Metric("inboxListWidth") : Metric("chatListWidth"), -1);
        gtk_widget_set_visible(side, show);
        gtk_widget_set_visible(scrim, over);
        if (over)
            gtk_overlay_reorder_overlay(GTK_OVERLAY(overlay), side, -1);
    }
    void ToggleSidebar() {
        sidebar_preferences[module] =
            sidebar_visible ? (overlay_sidebar ? std::optional<bool>{} : std::optional<bool>{false})
                            : std::optional<bool>{true};
        Layout(gtk_widget_get_allocated_width(overlay));
    }
    void Dismiss() {
        if (overlay_sidebar) {
            sidebar_preferences[module].reset();
            Layout(gtk_widget_get_allocated_width(overlay));
        }
    }
    void Watch(const std::filesystem::path& path) {
        try {
            runtime::PrivateDirectory verify(path);
        } catch (...) {
            return;
        }
        auto* file = g_file_new_for_path(path.c_str());
        GError* error = nullptr;
        auto* monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error);
        g_object_unref(file);
        g_clear_error(&error);
        if (!monitor)
            return;
        g_signal_connect(monitor, "changed",
                         G_CALLBACK((+[](GFileMonitor*, GFile* file, GFile* other,
                                         GFileMonitorEvent, gpointer p) {
                             for (auto* changed : {file, other}) {
                                 if (!changed)
                                     continue;
                                 auto* name = g_file_get_basename(changed);
                                 const std::string value = name ? name : "";
                                 g_free(name);
                                 if (value.size() > 5 &&
                                     value.substr(value.size() - 5) == ".json" &&
                                     value != "window.json") {
                                     static_cast<Impl*>(p)->Refresh();
                                     break;
                                 }
                             }
                         })),
                         this);
        monitors.push_back(monitor);
    }
    void Refresh() {
        if (fixture)
            return;
        if (refresh_running) {
            refresh_again = true;
            return;
        }
        if (refresh_timer) {
            g_source_remove(refresh_timer);
            refresh_timer = 0;
        }
        refresh_running = true;
        const auto token = load_token, life = alive;
        if (!jobs.Run(token, [this, life, token] {
                auto loaded = std::make_shared<Snapshot>();
                std::map<std::string, std::string> enabled;
                std::string next_theme = "night";
                Json prefs = Json::object();
                bool ok = false, prefs_ok = true, storage_busy = false;
                try {
                    *loaded = Store(root).Read();
                    ok = true;
                } catch (const runtime::StorageBusy&) {
                    storage_busy = true;
                } catch (...) {
                }
                try {
                    for (const auto& name : {"terminal", "chat", "inbox"}) {
                        const auto entry = packages.Inspect(Plugin(name));
                        if (entry.available && entry.enabled)
                            enabled[name] = entry.grant;
                    }
                } catch (...) {
                }
                try {
                    next_theme = runtime::ConfigurationStore(runtime::UserRoot() / "configuration")
                                     .Load()
                                     .at("theme");
                } catch (...) {
                }
                try {
                    const auto raw = runtime::PrivateDirectory(root).Read("window.json", 4096);
                    if (raw) {
                        prefs = Json::parse(*raw);
                        if (!prefs.is_object() || prefs.at("version") != 1 ||
                            !prefs.at("width").is_number_integer() ||
                            !prefs.at("height").is_number_integer() ||
                            !IsModule(prefs.at("module")))
                            throw std::runtime_error("window prefs");
                    }
                } catch (...) {
                    prefs_ok = false;
                    prefs = Json::object();
                }
                Post([this, life, token, loaded, enabled, next_theme, prefs, prefs_ok, ok,
                      storage_busy] {
                    if (!life->load() || token->load())
                        return;
                    refresh_running = false;
                    prefs_available = prefs_ok;
                    const bool first = !ready;
                    ready = ready || !storage_busy;
                    if (!storage_busy)
                        storage_available = ok;
                    if (ok)
                        state = *loaded;
                    const bool changed = grants != enabled;
                    grants = enabled;
                    if (theme != next_theme) {
                        theme = next_theme;
                        ApplyTheme(theme, root_view);
                    }
                    if (first && !prefs.empty()) {
                        width =
                            std::clamp(prefs.at("width").get<int>(), Metric("minimumWidth"), 4096);
                        height = std::clamp(prefs.at("height").get<int>(), Metric("minimumHeight"),
                                            4096);
                        module = prefs.at("module");
                        gtk_window_resize(GTK_WINDOW(window), width, height);
                    }
                    if (changed || first)
                        RenderNavigation();
                    if (grants.empty()) {
                        selected.clear();
                        Clear(index);
                        Clear(transcript);
                        restoring = true;
                        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer)),
                                                 "", 0);
                        restoring = false;
                        gtk_label_set_text(GTK_LABEL(heading), "Mailbox 插件已停用");
                        gtk_label_set_text(GTK_LABEL(source), "打开左下角的插件管理，启用工作区。");
                        Clear(toolbar);
                        Revoke();
                    } else {
                        if (!grants.count(module))
                            module = grants.begin()->first;
                        if (changed || first) {
                            SelectModule(module);
                        } else {
                            EnsureSelection();
                            RenderIndex();
                            RenderDetail();
                            UpdateNavigation();
                            MarkRead();
                        }
                    }
                    if (!ok && !storage_busy)
                        Notice("Mailbox 数据无法读取，原文件已保留。请检查文件或恢复备份后重试。");
                    else if (!prefs_ok)
                        Notice("窗口偏好无法读取；会话内容不受影响，原偏好已保留。");
                    ComposerState();
                    if (storage_busy)
                        RetryRefresh();
                    else {
                        refresh_backoff = 50;
                        if (refresh_again) {
                            refresh_again = false;
                            Refresh();
                        }
                    }
                });
            })) {
            refresh_running = false;
            RetryRefresh();
        }
    }
    void RetryRefresh() {
        if (refresh_timer)
            return;
        refresh_timer = g_timeout_add(
            refresh_backoff,
            +[](gpointer raw) -> gboolean {
                auto* self = static_cast<Impl*>(raw);
                self->refresh_timer = 0;
                self->Refresh();
                return G_SOURCE_REMOVE;
            },
            this);
        refresh_backoff = std::min(1000u, refresh_backoff * 2);
    }
    void SaveGeometrySoon() {
        if (fixture || !ready || !prefs_available)
            return;
        if (geometry_timer)
            g_source_remove(geometry_timer);
        geometry_timer = g_timeout_add(
            250,
            +[](gpointer p) -> gboolean {
                auto* self = static_cast<Impl*>(p);
                self->geometry_timer = 0;
                const Json value = {
                    {"version", 1},
                    {"width", std::clamp(self->width, Metric("minimumWidth"), 4096)},
                    {"height", std::clamp(self->height, Metric("minimumHeight"), 4096)},
                    {"module", self->module}};
                const auto path = self->root;
                self->jobs.Run(std::make_shared<std::atomic<bool>>(false), [path, value] {
                    try {
                        runtime::PrivateDirectory files(path);
                        auto lock = runtime::PrivateDirectory(path / "window").Lock(false);
                        files.Write("window.json", value.dump());
                    } catch (...) {
                    }
                });
                return G_SOURCE_REMOVE;
            },
            this);
    }
    bool Open(const std::string& requested, const std::string& thread) {
        if (!ready || Protected())
            return false;
        if (!requested.empty() && !IsModule(requested))
            return false;
        if (!thread.empty()) {
            bool found = false;
            for (const auto& row : state.document.at("threads"))
                if (row.at("id") == thread) {
                    if (!grants.count(Module(row)))
                        return false;
                    SelectModule(Module(row));
                    SelectThread(thread);
                    found = true;
                    break;
                }
            if (!found)
                return false;
        } else if (!requested.empty()) {
            if (!grants.count(requested))
                return false;
            SelectModule(requested);
        }
        gtk_widget_show_all(root_view);
        Layout(gtk_widget_get_allocated_width(overlay));
        UpdateNavigation();
        gtk_widget_set_visible(Find(side, "mailbox-new-chat"), module == "chat");
        gtk_widget_set_visible(feedback, *gtk_label_get_text(GTK_LABEL(feedback)) != '\0');
        gtk_widget_show(window);
        gtk_window_present(GTK_WINDOW(window));
        MarkRead();
        return true;
    }
};
Workspace::Workspace(GtkApplication* application, bool fixture)
    : impl_(std::make_unique<Impl>(application, fixture)) {}
Workspace::~Workspace() = default;
GtkWidget* Workspace::Window() const { return impl_->window; }
GtkWidget* Workspace::Content() const { return impl_->root_view; }
bool Workspace::Open(const std::string& module, const std::string& thread) {
    return impl_->Open(module, thread);
}
void Workspace::Hide() { impl_->Hide(); }
void Workspace::Refresh() { impl_->Refresh(); }
void Workspace::Protect(bool value) { impl_->Protect(value); }
bool Workspace::Protected() const { return impl_->Protected(); }
void Workspace::Fixture(Snapshot state, const std::string& theme, const std::string& module,
                        const std::string& thread) {
    if (!impl_->fixture)
        throw std::runtime_error("Fixture rendering requires an isolated window");
    Validate(state.document);
    impl_->state = std::move(state);
    impl_->theme = theme;
    ApplyTheme(theme, impl_->root_view);
    impl_->ready = impl_->storage_available = true;
    impl_->grants = {{"terminal", "fixture"}, {"chat", "fixture"}, {"inbox", "fixture"}};
    impl_->RenderNavigation();
    impl_->SelectModule(module);
    if (!thread.empty())
        impl_->SelectThread(thread);
    gtk_widget_show_all(impl_->root_view);
    impl_->Layout(gtk_widget_get_allocated_width(impl_->overlay));
    impl_->UpdateNavigation();
}
Json Workspace::Inspect() const {
    return {{"module", impl_->module},
            {"thread", impl_->selected},
            {"revision", impl_->state.revision},
            {"busy", impl_->busy},
            {"unread", Unread(impl_->state)},
            {"ready", impl_->ready},
            {"protected", impl_->Protected()},
            {"narrow", impl_->narrow},
            {"sidebarVisible", impl_->sidebar_visible},
            {"sidebarOverlay", impl_->overlay_sidebar}};
}
} // namespace rimes::mailbox
