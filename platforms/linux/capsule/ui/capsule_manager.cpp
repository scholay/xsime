#include "background_jobs.hpp"
#include "capsule_history.hpp"
#include "capsule_capture.hpp"
#include "capsule_resources.hpp"
#include "clipboard_preview.hpp"
#include "capsule_manager.hpp"
#include "capsule_password_view.hpp"
#include "capsule_note_editor.hpp"
#include "capsule_resource_editor.hpp"
#include "capsule_text.hpp"
#include "desktop.hpp"
#include "official_catalog.hpp"
#include "private_files.hpp"
#include <algorithm>
#include <filesystem>
#include <optional>
#include <utility>
namespace rimes::capsule {
using namespace rimes::desktop;
namespace {
void Post(std::function<void()> work) {
    g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer raw) -> gboolean {
        auto& fn = *static_cast<std::function<void()>*>(raw); fn(); return G_SOURCE_REMOVE;
    }, new std::function<void()>(std::move(work)), +[](gpointer raw) { delete static_cast<std::function<void()>*>(raw); });
}
std::string Text(GtkWidget* text) {
    auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
    GtkTextIter begin{}, end{}; gtk_text_buffer_get_bounds(buffer, &begin, &end);
    auto* value = gtk_text_buffer_get_text(buffer, &begin, &end, FALSE);
    std::string result = value; g_free(value); return result;
}
GtkWidget* Scroll(GtkWidget* child) {
    auto* scroll = gtk_scrolled_window_new(nullptr,nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll),child); return scroll;
}
GtkWidget* FormLabel(const char* text, const char* style, int line_height) {
    auto* label = Label(text, style);
    auto* attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes,
                          pango_attr_line_height_new_absolute(line_height * PANGO_SCALE));
    gtk_label_set_attributes(GTK_LABEL(label), attributes);
    pango_attr_list_unref(attributes);
    return label;
}
} // namespace
struct ManagerWindow::Impl {
    Send send;
    CopyFile copy_file;
    CopyText copy_text;
    std::function<void()> open_capture;
    GtkWidget *capture_form=nullptr,*capture_title=nullptr,*capture_meta=nullptr,*capture_picture=nullptr,*capture_collect=nullptr,*capture_remove=nullptr;
    std::shared_ptr<GdkPixbuf> capture_image;
    std::unique_ptr<ResourceEditor> resource_editor;
    std::unique_ptr<PasswordView> password_view;
    GtkWidget* migrate_password = nullptr;
    std::optional<Record> suspended_resource;
    std::string suspended_resource_key;
    bool suspended_resource_import = false;
    GtkWidget *window, *root, *header, *modules, *modules_scroll, *toolbar, *list, *list_scroll, *editor;
    GtkWidget *count, *search, *create, *hint, *title, *summary, *format, *body, *status, *save, *remove, *divider, *heading;
    GtkCssProvider* css = gtk_css_provider_new();
    Snapshot state;
    Record draft;
    bool dirty = false, updating = false, preview = false, writing = false;
    int layout_width = 0, layout_height = 0;
    std::string detail_key, theme;
    std::shared_ptr<GdkPixbuf> clipboard_picture;
    std::unique_ptr<NoteEditor> note_editor;
    GtkWidget *rich_bar = nullptr, *bullet_bar = nullptr, *projection_warning = nullptr;
    bool rich_supported = true;
    int current_format = 0;
    std::optional<RichDocument> suspended_rich;
    runtime::BackgroundJobs jobs;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> read_cancel, write_cancel;
    std::unique_ptr<plugins::PackageStore> packages;
    std::optional<Record> suspended_draft;
    std::string suspended_key;
    GtkWidget* confirmation = nullptr;
    GtkWidget *note_form = nullptr, *note_actions = nullptr, *clipboard_form = nullptr,
              *clipboard_title = nullptr, *clipboard_source = nullptr, *clipboard_text = nullptr,
              *clipboard_image = nullptr, *clipboard_status = nullptr, *clipboard_copy = nullptr;
    bool ConfirmCurrent(const std::string& caption, const std::string& message) {
        confirmation =
            gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                   GTK_BUTTONS_NONE, "%s", caption.c_str());
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(confirmation), "%s",
                                                 message.c_str());
        gtk_dialog_add_buttons(GTK_DIALOG(confirmation), "取消", GTK_RESPONSE_CANCEL, "确认",
                               GTK_RESPONSE_ACCEPT, nullptr);
        const bool result = gtk_dialog_run(GTK_DIALOG(confirmation)) == GTK_RESPONSE_ACCEPT;
        gtk_widget_destroy(confirmation);
        confirmation = nullptr;
        return result;
    }
    Record CaptureDraft() const {
        if (state.module == "resources" && resource_editor) return resource_editor->Capture();
        Record value=draft;
        value.title=gtk_entry_get_text(GTK_ENTRY(title)); value.summary=gtk_entry_get_text(GTK_ENTRY(summary)); value.content=Text(body);
        const int selected =gtk_combo_box_get_active(GTK_COMBO_BOX(format));
        value.note_format = selected == 1 ? "bullet" : selected == 2 ? "rich-text" : "other";
        if (value.note_format == "rich-text") {
            if (rich_supported) {
                const auto document = note_editor->Capture();
                value.content = document.Markdown();
                value.rich_asset = WriteRichText(document).value_or("");
            } else {
                value.content = draft.content;
                value.rich_asset = draft.rich_asset;
            }
        } else
            value.rich_asset.clear();
        return value;
    }
    void FillDraft() {
        if (state.module == "resources" && resource_editor) { resource_editor->Load(draft); Controls(); return; }
        updating=true;
        gtk_entry_set_text(GTK_ENTRY(title),draft.title.c_str());
        gtk_entry_set_text(GTK_ENTRY(summary),draft.summary.c_str());
        gtk_combo_box_set_active(GTK_COMBO_BOX(format),draft.note_format=="bullet"?1:draft.note_format=="rich-text"?2:0);
        current_format = gtk_combo_box_get_active(GTK_COMBO_BOX(format));
        rich_supported = true;
        if (draft.note_format == "rich-text") {
            auto document = ReadRichText(draft.rich_asset);
            if (document) {
                note_editor->Load(*document);
                rich_supported = document->editable;
            } else {
                note_editor->LoadPlain(draft.content);
                rich_supported = false;
            }
        } else
            note_editor->LoadPlain(draft.content);
        updating=false;
        gtk_label_set_text(GTK_LABEL(heading),draft.id.empty()?"新建笔记":"编辑笔记"); Controls();
    }
    Impl(Send callback, bool fixture, CopyFile file, CopyText text, std::function<void()> capture) : send(std::move(callback)), copy_file(std::move(file)), copy_text(std::move(text)), open_capture(std::move(capture)), preview(fixture) {
        if (!preview) packages=std::make_unique<plugins::PackageStore>(runtime::UserRoot()/"plugins", plugins::OfficialCatalog(), plugins::BundledPackages(), plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "Capsule");
        gtk_window_set_decorated(GTK_WINDOW(window),FALSE);
        gtk_window_set_default_size(GTK_WINDOW(window),940,660);
        GdkGeometry geometry{}; geometry.min_width=620; geometry.min_height=430;
        gtk_window_set_geometry_hints(GTK_WINDOW(window),nullptr,&geometry,GDK_HINT_MIN_SIZE);
        root = Class(gtk_layout_new(nullptr,nullptr),"capsule-manager"); gtk_container_add(GTK_CONTAINER(window),root);
        g_signal_connect(root,"draw",G_CALLBACK(+[](GtkWidget* widget,cairo_t* cr,gpointer)->gboolean {
            gtk_render_background(gtk_widget_get_style_context(widget),cr,0,0,gtk_widget_get_allocated_width(widget),gtk_widget_get_allocated_height(widget)); return FALSE;
        }),nullptr);
        header = Row(9); auto* caption=Label("Capsule","capsule-heading"); Add(header,caption);
        count=Label("0 ITEMS","capsule-count"); Add(header,count); Add(header,gtk_label_new(""),true);
        auto* plugins_button=Button("Capsule 插件…",[this] { g_spawn_command_line_async("rimes-settings",nullptr); },"capsule-link");
        auto* rail_button=Class(IconButton("gearshape","返回 Capsule 底栏",[this] { Navigate({{"v",1},{"op","rail"}}); }),"capsule-gear");
        auto* close_button=Class(IconButton("xmark","关闭 Capsule",[this] { Navigate({{"v",1},{"op","close"}}); }),"capsule-close");
        gtk_widget_set_size_request(plugins_button,-1,18);
        for(auto* action:{plugins_button,rail_button,close_button}) {
            gtk_widget_set_valign(action,GTK_ALIGN_CENTER); Add(header,action);
        }
        modules=Column(3); modules_scroll=Class(Scroll(modules),"capsule-modules");
        gtk_widget_set_margin_start(modules,8); gtk_widget_set_margin_end(modules,8);
        gtk_widget_set_margin_top(modules,10); gtk_widget_set_margin_bottom(modules,10);
        toolbar=Row(8); search=gtk_search_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(search),"搜索标题或内容");
        Class(search,"capsule-search");
        gtk_widget_set_tooltip_text(search,"搜索 Capsule"); Add(toolbar,search,true);
        create=Button("＋ 新建",[this] { NewNote(); }); Add(toolbar,create);
        gtk_widget_set_size_request(create,57,20);
        gtk_widget_set_valign(create,GTK_ALIGN_CENTER);
        list=Column(1); list_scroll=Class(Scroll(list),"capsule-list"); divider=Class(gtk_separator_new(GTK_ORIENTATION_VERTICAL),"capsule-divider");
        gtk_widget_set_margin_top(list,10); gtk_widget_set_margin_start(list,16); gtk_widget_set_margin_end(list,16);
        editor=Class(Column(8),"capsule-editor");
        auto* form=Column(10); gtk_widget_set_margin_start(form,16); gtk_widget_set_margin_end(form,16);
        gtk_widget_set_margin_top(form,16); gtk_widget_set_margin_bottom(form,16);
        heading=FormLabel("新建笔记","capsule-form-heading",13); Add(form,heading);
        auto field=[&](const char* name,GtkWidget* widget,bool expands=false) {
            auto* stack=Column(5); Add(stack,FormLabel(name,"capsule-field-label",11)); Add(stack,widget,expands); Add(form,stack,expands);
        };
        title=Class(gtk_entry_new(),"capsule-field"); gtk_entry_set_placeholder_text(GTK_ENTRY(title),"标题"); field("标题",title);
        summary=Class(gtk_entry_new(),"capsule-field"); gtk_entry_set_placeholder_text(GTK_ENTRY(summary),"一句话说明，显示在卡片上（可选）"); field("摘要",summary);
        format=Class(gtk_combo_box_text_new(),"capsule-note-format");
        for (const auto* name:{"其他","Bullet","富文本"}) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(format),name);
        auto* cells=gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(format));
        for(auto* cell=cells;cell;cell=cell->next) {
            if(!GTK_IS_CELL_RENDERER_TEXT(cell->data)) continue;
            auto* attributes=pango_attr_list_new();
            pango_attr_list_insert(attributes,pango_attr_line_height_new_absolute(16*PANGO_SCALE));
            g_object_set(cell->data,"ypad",0,"height",16,"attributes",attributes,nullptr);
            pango_attr_list_unref(attributes);
        }
        g_list_free(cells);
        g_signal_connect_after(format,"draw",G_CALLBACK(+[](GtkWidget* widget,cairo_t* cr,gpointer raw)->gboolean {
            const auto* self=static_cast<Impl*>(raw);
            const auto& palette=Visuals().at("palettes").at(self->theme=="liquidGlass"?"day":self->theme);
            const double opacity=gtk_widget_is_sensitive(widget)?1.0:0.46;
            auto color=[&](const char* key) {
                GdkRGBA value{};gdk_rgba_parse(&value,palette.at(key).get<std::string>().c_str());
                cairo_set_source_rgba(cr,value.red,value.green,value.blue,value.alpha*opacity);
            };
            // RimeFixedAccentPopUpButton's 19 pt, theme-owned indicator.
            const double w=19;
            const double h=19;
            const double r=6;
            const double x=gtk_widget_get_allocated_width(widget)-w-0.5;
            const double y=(gtk_widget_get_allocated_height(widget)-h)/2.0;
            cairo_save(cr);cairo_new_sub_path(cr);
            constexpr double pi=3.14159265358979323846;
            cairo_arc(cr,x+w-r,y+r,r,-pi/2,0);cairo_arc(cr,x+w-r,y+h-r,r,0,pi/2);
            cairo_arc(cr,x+r,y+h-r,r,pi/2,pi);cairo_arc(cr,x+r,y+r,r,pi,3*pi/2);
            cairo_close_path(cr);color("surfaceTertiary");cairo_fill_preserve(cr);
            color("border");cairo_set_line_width(cr,1);cairo_stroke(cr);
            color("accentText");cairo_set_line_width(cr,1.5);
            cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);cairo_set_line_join(cr,CAIRO_LINE_JOIN_ROUND);
            const double cx=x+w/2;
            const double cy=y+h/2;
            cairo_move_to(cr,cx-3,cy+1.2);cairo_line_to(cr,cx,cy-1.2);cairo_line_to(cr,cx+3,cy+1.2);
            cairo_stroke(cr);cairo_restore(cr);return FALSE;
        }),this);
        field("笔记类型",format);
        projection_warning = Column(6);
        auto* warning =
            Label("Markdown 副本已被修改。富文本原稿仍在，请显式导入副本后再保存。", "error");
        gtk_label_set_line_wrap(GTK_LABEL(warning), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(warning), 1);
        Add(projection_warning, warning);
        auto* import_projection = Button("导入 Markdown 副本", [this] {
            if (!draft.projection_conflict ||
                !ConfirmCurrent("导入 Markdown 副本？",
                                "将副本作为当前富文本正文，保存后替换富文本原稿。"))
                return;
            if (!state.visible || !state.managing || state.password_field ||
                !draft.projection_conflict)
                return;
            note_editor->LoadPlain(draft.content);
            rich_supported = true;
            draft.projection_conflict = false;
            draft.resolve_projection = true;
            Changed();
        });
        gtk_widget_set_name(import_projection, "capsule-import-projection");
        Add(projection_warning, import_projection);
        Add(form, projection_warning);
        rich_bar = Row(6);
        Add(rich_bar, Button("粗体", [this] { note_editor->Bold(); }, "capsule-link"));
        Add(rich_bar, Button("斜体", [this] { note_editor->Italic(); }, "capsule-link"));
        Add(rich_bar,
            Button("链接", [this] { note_editor->Link(GTK_WINDOW(window)); }, "capsule-link"));
        Add(rich_bar,
            Button("图片…", [this] { note_editor->Image(GTK_WINDOW(window)); }, "capsule-link"));
        Add(form, rich_bar);
        bullet_bar = Button("＋ 清单项", [this] { note_editor->AddBullet(); }, "capsule-link");
        gtk_widget_set_halign(bullet_bar, GTK_ALIGN_START);
        Add(form, bullet_bar);
        for (auto* bar : {rich_bar, bullet_bar, projection_warning}) {
            gtk_widget_show_all(bar);
            gtk_widget_set_no_show_all(bar, TRUE);
            gtk_widget_hide(bar);
        }
        body=gtk_text_view_new(); gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(body),GTK_WRAP_WORD_CHAR);
        // NSTextView uses an 8 pt inset and 5 pt line-fragment padding.
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(body),13); gtk_text_view_set_right_margin(GTK_TEXT_VIEW(body),13);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(body),8); gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(body),8);
        note_editor = std::make_unique<NoteEditor>(body, [this] { Changed(); });
        auto* area=Class(Scroll(body),"capsule-text"); gtk_widget_set_size_request(area,-1,220); field("内容",area,true);
        auto* writing_hint=FormLabel("写法：「- 标签：值」是字段，复制时只复制值；「## "
                                   "标题」分组；其余每行单独复制。","capsule-writing-hint",13);
        gtk_label_set_line_wrap(GTK_LABEL(writing_hint),TRUE); gtk_label_set_max_width_chars(GTK_LABEL(writing_hint),1); Add(form,writing_hint);
        note_form = Scroll(form);
        Add(editor, note_form, true);
        auto* actions=Row(8); gtk_widget_set_margin_start(actions,16); gtk_widget_set_margin_end(actions,16); gtk_widget_set_margin_bottom(actions,12);
        status=FormLabel("选择一条 Capsule 内容","capsule-status",11); Add(actions,status,true);
        remove=Button("删除",[this] { Remove(); },"capsule-link"); Add(actions,remove);
        gtk_widget_set_valign(remove,GTK_ALIGN_CENTER);
        save = Button("保存", [this] { Save(); });
        Add(actions, save);
        Add(editor, actions);
        note_actions = actions;
        migrate_password=Button("移入密码库…",[this] {
            if(preview||state.module!="notes"||draft.id.empty()||writing||!state.visible||state.password_field)return;
            if(dirty){gtk_label_set_text(GTK_LABEL(status),"请先保存笔记，再迁移到密码库");return;}
            if(!ConfirmCurrent("将笔记加密移入密码库？","验证查看口令后，加密正文并删除原笔记；摘要不会带入。已有的外部备份不会被追溯删除。"))return;
            if(state.module!="notes"||!state.visible||state.password_field)return;
            password_view->PrepareMigration(draft.id,draft.revision);
            send(Json{{"v",1},{"op","module"},{"text","passwords"}}.dump());
        },"capsule-link");Add(actions,migrate_password);
        password_view=std::make_unique<PasswordView>(GTK_WINDOW(window),send,preview);
        gtk_widget_show_all(password_view->Widget());gtk_widget_set_no_show_all(password_view->Widget(),TRUE);gtk_widget_hide(password_view->Widget());
        gtk_box_pack_start(GTK_BOX(editor),password_view->Widget(),TRUE,TRUE,0);
        gtk_box_reorder_child(GTK_BOX(editor),password_view->Widget(),1);
        resource_editor = std::make_unique<ResourceEditor>(GTK_WINDOW(window), [this] { Changed(); },
            [this](const Record& value) {
                if (state.visible && state.managing && !state.password_field && state.module == "resources" && copy_file) copy_file(value);
            }, [this](const std::string& text) {
                if (!state.visible || !state.managing || state.password_field || state.module != "resources" || preview) return;
                try {
                    if (!packages->Inspect(ModulePlugin(Module::Resources)).enabled) return;
                    if (copy_text) copy_text(text);
                    else gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text.data(), text.size());
                } catch (const std::exception&) {}
            }, preview);
        gtk_widget_set_name(resource_editor->Widget(), "capsule-resource-form");
        gtk_widget_show_all(resource_editor->Widget());
        gtk_widget_set_no_show_all(resource_editor->Widget(), TRUE);
        gtk_widget_hide(resource_editor->Widget());
        gtk_box_pack_start(GTK_BOX(editor), resource_editor->Widget(), TRUE, TRUE, 0);
        gtk_box_reorder_child(GTK_BOX(editor), resource_editor->Widget(), 1);
        clipboard_form = Class(Column(10), "capsule-clipboard-detail");
        gtk_widget_set_margin_start(clipboard_form, 16);
        gtk_widget_set_margin_end(clipboard_form, 16);
        gtk_widget_set_margin_top(clipboard_form, 16);
        gtk_widget_set_margin_bottom(clipboard_form, 16);
        clipboard_title = Label("", "capsule-clipboard-heading");
        clipboard_source = Label("", "caption");
        for (auto* label : {clipboard_title, clipboard_source}) {
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
        }
        clipboard_text = gtk_text_view_new();
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(clipboard_text), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_editable(GTK_TEXT_VIEW(clipboard_text), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(clipboard_text), FALSE);
        clipboard_image = gtk_drawing_area_new();
        g_signal_connect(
            clipboard_image, "draw",
            G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                if (!self->clipboard_picture)
                    return FALSE;
                auto* picture = self->clipboard_picture.get();
                const double width = gdk_pixbuf_get_width(picture);
                const double height = gdk_pixbuf_get_height(picture);
                const double scale = std::min(gtk_widget_get_allocated_width(widget) / width,
                                              gtk_widget_get_allocated_height(widget) / height);
                cairo_save(cr);
                cairo_translate(cr, (gtk_widget_get_allocated_width(widget) - width * scale) / 2,
                                (gtk_widget_get_allocated_height(widget) - height * scale) / 2);
                cairo_scale(cr, scale, scale);
                gdk_cairo_set_source_pixbuf(cr, picture, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
                return FALSE;
            }),
            this);
        gtk_widget_set_size_request(clipboard_image, -1, 220);
        gtk_widget_set_no_show_all(clipboard_image, TRUE);
        clipboard_status = Label("", "caption");
        gtk_label_set_line_wrap(GTK_LABEL(clipboard_status), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(clipboard_status), 1);
        clipboard_copy = Button("复制内容", [this] { ClipboardCommand("copy"); });
        gtk_widget_set_halign(clipboard_copy, GTK_ALIGN_START);
        auto* menu = Button("更多", [this] { ClipboardMenu(); }, "capsule-link");
        gtk_widget_set_halign(menu, GTK_ALIGN_START);
        Add(clipboard_form, clipboard_title);
        Add(clipboard_form, clipboard_source);
        Add(clipboard_form, Scroll(clipboard_text), true);
        Add(clipboard_form, clipboard_image);
        Add(clipboard_form, clipboard_copy);
        Add(clipboard_form, menu);
        Add(clipboard_form, clipboard_status);
        Add(editor, clipboard_form, true);
        capture_form=Column(12);gtk_widget_set_margin_start(capture_form,16);gtk_widget_set_margin_end(capture_form,16);
        gtk_widget_set_margin_top(capture_form,16);gtk_widget_set_margin_bottom(capture_form,16);
        capture_title=Label("", "capsule-clipboard-heading");capture_meta=Label("", "caption");
        gtk_label_set_line_wrap(GTK_LABEL(capture_meta),TRUE);gtk_label_set_max_width_chars(GTK_LABEL(capture_meta),1);
        capture_picture=gtk_drawing_area_new();gtk_widget_set_size_request(capture_picture,-1,240);
        g_signal_connect(capture_picture,"draw",G_CALLBACK(+[](GtkWidget* widget,cairo_t* cr,gpointer raw)->gboolean {
            auto* self=static_cast<Impl*>(raw);if(!self->capture_image) return FALSE;
            const int w=gdk_pixbuf_get_width(self->capture_image.get());const int h=gdk_pixbuf_get_height(self->capture_image.get());
            const auto scale=std::min(double(gtk_widget_get_allocated_width(widget))/w,double(gtk_widget_get_allocated_height(widget))/h);
            cairo_save(cr);cairo_translate(cr,(gtk_widget_get_allocated_width(widget)-w*scale)/2,(gtk_widget_get_allocated_height(widget)-h*scale)/2);
            cairo_scale(cr,scale,scale);gdk_cairo_set_source_pixbuf(cr,self->capture_image.get(),0,0);cairo_paint(cr);cairo_restore(cr);return FALSE;
        }),this);
        Add(capture_form,capture_title);Add(capture_form,capture_meta);Add(capture_form,capture_picture,true);
        auto* capture_actions=Row(8);
        auto* capture_copy=Button("复制",[this]{CaptureCommand("copy");});Add(capture_actions,capture_copy);
        capture_collect=Button("收藏到 Capsule",[this]{CaptureCommand("favorite");});Add(capture_actions,capture_collect);
        capture_remove=Button("删除",[this]{if(ConfirmCurrent("删除这条捕获？","未收藏的捕获文件会从本机历史移除。")) CaptureCommand("remove");});Add(capture_actions,capture_remove);
        Add(capture_form,capture_actions);
        Add(capture_form,Button("截图 / 录屏…",[this]{if(open_capture) open_capture();}));
        for(auto pair:{std::pair<GtkWidget*,const char*>{capture_form,"capsule-capture-form"},{capture_title,"capsule-capture-title"},{capture_meta,"capsule-capture-meta"},{capture_copy,"capsule-capture-copy"},{capture_collect,"capsule-capture-collect"},{capture_remove,"capsule-capture-remove"}}) gtk_widget_set_name(pair.first,pair.second);
        gtk_widget_show_all(capture_form);gtk_widget_set_no_show_all(capture_form,TRUE);gtk_widget_hide(capture_form);Add(editor,capture_form,true);
        for (auto* group : {note_form, note_actions, clipboard_form}) {
            gtk_widget_show_all(group);
            gtk_widget_set_no_show_all(group, TRUE);
        }
        gtk_widget_hide(clipboard_form);
        gtk_widget_set_name(clipboard_form, "capsule-clipboard-detail");
        gtk_widget_set_name(clipboard_text, "capsule-clipboard-text");
        gtk_widget_set_name(clipboard_copy, "capsule-clipboard-copy");
        gtk_widget_set_name(clipboard_status, "capsule-clipboard-status");
        for(const auto& item:std::initializer_list<std::pair<GtkWidget*,const char*>>{
            {title,"capsule-title"},{summary,"capsule-summary"},{body,"capsule-body"},{format,"capsule-format"},
            {save,"capsule-save"},{remove,"capsule-delete"},{create,"capsule-new"},{status,"capsule-status"},
            {modules_scroll,"capsule-modules"},{list_scroll,"capsule-list"},{editor,"capsule-editor"},
            {header,"capsule-header"},{toolbar,"capsule-toolbar"},
                 {rich_bar, "capsule-rich-tools"},
                 {bullet_bar, "capsule-bullet-tools"}})
            gtk_widget_set_name(item.first,item.second);
        hint=Label("管理本机 Capsule","capsule-hint"); gtk_label_set_xalign(GTK_LABEL(hint),0);
        for (auto* child : {header,modules_scroll,toolbar,list_scroll,divider,editor,hint}) gtk_layout_put(GTK_LAYOUT(root),child,0,0);
        g_signal_connect(root,"size-allocate",G_CALLBACK(+[](GtkWidget*,GtkAllocation* bounds,gpointer data) {
            auto* self=static_cast<Impl*>(data);
            if(self->layout_width==bounds->width && self->layout_height==bounds->height) return;
            self->layout_width=bounds->width; self->layout_height=bounds->height;
            // GtkLayout allocates its children before emitting this signal.
            // Queue the geometry change outside that allocation, so GTK can
            // honor the new child requests without imposing a minimum window
            // size based on the previous (larger) allocation.
            auto life=self->alive;
            Post([self,life] { if(life->load()) self->Layout(self->layout_width,self->layout_height); });
        }),this);
        g_signal_connect(search,"search-changed",G_CALLBACK(+[](GtkSearchEntry* entry,gpointer data) {
            auto* self=static_cast<Impl*>(data);
            if (!self->updating) self->Navigate({{"v",1},{"op","search"},{"text",gtk_entry_get_text(GTK_ENTRY(entry))}});
        }),this);
        g_signal_connect(summary,"changed",G_CALLBACK(+[](GtkEditable*,gpointer data) { static_cast<Impl*>(data)->Changed(); }),this);
        g_signal_connect(format,"changed",G_CALLBACK(+[](GtkComboBox*,gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (self->updating)
                    return;
                const int selected = gtk_combo_box_get_active(GTK_COMBO_BOX(self->format));
                if (self->current_format == 2 && selected != 2)
                    self->note_editor->LoadPlain(self->note_editor->Capture().PlainText());
                self->current_format = selected;
                self->rich_supported = true;
                self->Changed(); }),this);
        g_signal_connect(title,"changed",G_CALLBACK(+[](GtkEditable*,gpointer data) { static_cast<Impl*>(data)->Changed(); }),this);
        g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(body)),"changed",G_CALLBACK(+[](GtkTextBuffer*,gpointer data) { static_cast<Impl*>(data)->Changed(); }),this);
        g_signal_connect(window,"delete-event",G_CALLBACK(+[](GtkWidget*,GdkEvent*,gpointer data)->gboolean {
            static_cast<Impl*>(data)->Navigate({{"v",1},{"op","close"}}); return TRUE;
        }),this);
        g_signal_connect(window,"key-press-event",G_CALLBACK(+[](GtkWidget*,GdkEventKey* event,gpointer data)->gboolean {
            auto* self=static_cast<Impl*>(data);
            if (event->keyval==GDK_KEY_Escape) { self->Navigate({{"v",1},{"op","rail"}}); return TRUE; }
            if ((event->state&GDK_CONTROL_MASK) && event->keyval==GDK_KEY_s) { self->Save(); return TRUE; }
            auto* focused=gtk_window_get_focus(GTK_WINDOW(self->window));
            if(focused && gtk_widget_is_ancestor(focused,self->list)) {
                const int current=self->state.offset+self->state.selected;
                int next=current;
                switch(event->keyval) {
                case GDK_KEY_Up: next=current-1; break;
                case GDK_KEY_Down: next=current+1; break;
                case GDK_KEY_Home: next=0; break;
                case GDK_KEY_End: next=self->state.count-1; break;
                case GDK_KEY_Page_Up: next=current-8; break;
                case GDK_KEY_Page_Down: next=current+8; break;
                default: return FALSE;
                }
                if(self->state.count) self->Navigate({{"v",1},{"op","select"},{"index",std::clamp(next,0,self->state.count-1)}});
                return TRUE;
            }
            return FALSE;
        }),this);
        SetTheme("night"); ClearDraft();
    }
    ~Impl() {
        alive->store(false); jobs.Shutdown();
        g_signal_handlers_disconnect_by_data(window,this);
        password_view.reset();
        resource_editor.reset();
        note_editor.reset();
        gtk_widget_destroy(window); g_object_unref(css);
    }
    void Layout(int width,int height) {
        gtk_layout_set_size(GTK_LAYOUT(root),width,height);
        const auto& metrics=ModuleContract().at("metrics");
        const int inset=metrics.at("headerInset"); const int module_width=metrics.at("moduleWidth");
        const int list_width=std::clamp(static_cast<int>(width*metrics.at("listFraction").get<double>()),170,286);
        const int list_x=inset+module_width+10, rule_x=list_x+list_width+12, editor_x=rule_x+15;
        auto place=[&](GtkWidget* child,int x,int y,int w,int h) { gtk_layout_move(GTK_LAYOUT(root),child,x,y); gtk_widget_set_size_request(child,std::max(w,1),std::max(h,1)); };
        place(header,inset,12,width-2*inset,30); place(modules_scroll,inset,56,module_width,height-86);
        place(toolbar,list_x,56,list_width,22); place(list_scroll,list_x,90,list_width,height-119);
        place(divider,rule_x,56,1,height-85); place(editor,editor_x,56,width-editor_x-inset,height-85);
        place(hint,inset,height-20,width-2*inset,10);
    }
    void SetTheme(const std::string& name) {
        theme=name; Theme(name,gtk_widget_get_screen(window));
        const auto& palette=Visuals().at("palettes").at(name=="liquidGlass"?"day":name);
        auto c=[&](const char* key){ return palette.at(key).get<std::string>(); };
        const bool light=name=="day" || name=="liquidGlass";
        const std::string field_fill=light?"white":"mix("+c("surfaceSecondary")+",white,0.12)";
        const std::string popup_fill=light?"white":"mix("+c("surfaceSecondary")+",white,0.30)";
        const std::string search_fill=light?"white":"mix("+c("bufferBackground")+",white,0.12)";
        const std::string new_fill=light?"white":"mix("+c("bufferBackground")+",white,0.30)";
        const std::string rules =
            ".capsule-manager { background: " + c("bufferBackground") +
            "; color: " + c("textPrimary") +
            "; border-radius: 14px; }"
            ".capsule-manager viewport { background: transparent; }"
            ".capsule-clipboard-heading { font-size: 15px; font-weight: 600; }"
            ".capsule-clipboard-detail textview { background: transparent; font-size: 11px; }"
            ".capsule-heading { font-family: monospace; font-size: 15px; "
            "font-weight: bold; }"
            ".capsule-count { font-family: monospace; font-size: 10px; color: " +
            c("textSecondary") +
            "; }"
            ".capsule-hint { font-family: monospace; font-size: 9px; color: " +
            c("textSecondary") +
            "; }"
            ".capsule-list,.capsule-editor { background: " +
            c("surfaceSecondary") + "; border: 0; box-shadow: inset 0 0 0 1px " + c("border") +
            "; border-radius: 6px; }"
            ".capsule-modules { background: transparent; border: 0; }"
            ".capsule-manager textview,.capsule-manager textview text { "
            "font-family: monospace; font-size: 12px; background: " +
            c("surfaceTertiary") + "; color: " + c("textPrimary") +
            "; }"
            ".capsule-manager entry { min-height: 0; padding: 0 3px; font-family: "
            "monospace; font-size: 11px; }"
            ".capsule-manager .capsule-search { background: " + search_fill + "; border-color: alpha(" + c("textPrimary") + ",0.22); }"
            ".capsule-manager .capsule-search image { color: " + c("textPrimary") + "; }"
            ".capsule-manager .capsule-field { border-radius: 0; font-size: 12px; "
            "background: " + field_fill + "; border-color: alpha(" + c("textPrimary") + ",0.22); }"
            ".capsule-manager .capsule-field:focus { border-color: " + c("accentGreen") + "; }"
            ".capsule-manager combobox button { min-height: 20px; padding: 0 5px; border: 0; "
            "background: " + popup_fill + "; }"
            ".capsule-manager .capsule-note-format arrow { opacity: 0; }"
            ".capsule-manager #capsule-new,.capsule-manager #capsule-save { min-height: 20px; padding: 0 8px; }"
            ".capsule-manager #capsule-new { background: " + new_fill + "; color: " + (light?"black":"white") + "; }"
            ".capsule-manager #capsule-save { background: " + popup_fill + "; color: " + (light?"black":"white") + "; }"
            ".capsule-manager #capsule-new label,.capsule-manager #capsule-save label { font-family: monospace; font-size: 10px; font-weight: 600; }"
            ".capsule-manager #capsule-save { min-width: 24px; }"
            ".capsule-manager #capsule-delete { min-height: 18px; min-width: 22px; padding: 0 5px; }"
            ".capsule-manager #capsule-delete label { font-size: 10px; font-weight: 400; }"
            ".capsule-manager .capsule-text { border: 1px solid " +
            c("border") +
            "; }"
            ".capsule-resource-form entry { font-size: 11px; border-width: 1px; padding: 0 3px; }"
            ".capsule-resource-form button { font-size: 10px; padding: 0 8px; min-height: 18px; }"
            ".capsule-resource-form button label { font-size: 10px; }"
            ".capsule-form-heading,.capsule-field-label { font-family: monospace; "
            "font-size: 9px; font-weight: 600; color: " +
            c("textSecondary") +
            "; }"
            ".capsule-form-heading { font-size: 10px; color: " +
            c("accentText") +
            "; }"
            ".capsule-writing-hint,.capsule-status { font-family: monospace; "
            "font-size: 9px; color: " +
            c("textMuted") +
            "; }"
            ".capsule-writing-hint { font-size: 10px; }"
            ".capsule-manager .capsule-link label { font-family: monospace; "
            "font-size: 9px; font-weight: 600; color: " +
            c("textSecondary") +
            "; }"
            ".capsule-module,.capsule-filter,.capsule-link { background: alpha(" +
            c("textSecondary") +
            ",0.10); padding: 0 5px; min-height: 16px; border-radius: 12px; }"
            ".capsule-manager .capsule-gear { min-width: 22px; min-height: 22px; "
            "background: alpha(" +
            c("accentGreen") +
            ",0.2); border-radius: 6px; }"
            ".capsule-manager .capsule-gear .glyph { color: " + c("textPrimary") + "; }"
            ".capsule-manager .capsule-close { min-width: 16px; min-height: 16px; }"
            ".capsule-module label { font-size: 12px; font-weight: 500; color: " +
            c("textPrimary") +
            "; }"
            ".capsule-module.selected label { font-weight: bold; } .capsule-filter "
            "label { font-size: 10px; color: " +
            c("textSecondary") +
            "; }"
            ".capsule-filter.selected label { color: " +
            c("accentText") +
            "; font-weight: 600; }"
            ".capsule-entry { background: transparent; border-radius: 0; padding: "
            "0 10px; min-height: 0; }"
            ".capsule-entry.selected { background: alpha(" +
            c("accentGreen") +
            ",0.10); }"
            ".capsule-entry label { font-family: monospace; color: " +
            c("textPrimary") +
            "; } .capsule-entry .caption { font-size: 9px; color: " + c("textSecondary") +
            "; }"
            ".capsule-entry .capsule-kind { font-size: 8px; color: " +
            c("textSecondary") +
            "; }"
            ".capsule-entry.selected .name,.capsule-entry.selected .capsule-kind { "
            "color: " +
            c("accentText") +
            "; }"
            ".capsule-divider { background: " +
            c("border") + "; min-width: 1px; }"
            ".capsule-password-form .capsule-field { font-size: 11px; }"
            ".capsule-passcode-capture image { color: " + c("accentGreen") + "; }"
            ".capsule-passcode-slot { border: 1px solid " + c("border") + "; border-radius: 7px; background: " + c("surfaceTertiary") + "; font-size: 15px; font-weight: 600; }"
            ".capsule-passcode-slot.current { border-color: " + c("accentGreen") + "; }"
            ".capsule-passcode-capture { border: 1px solid " + c("accentGreen") + "; border-radius: 7px; background: " + c("surfaceTertiary") + "; color: " + c("accentGreen") + "; padding: 0; }"
            ".capsule-passcode-capture:focus { border-color: " + c("accentGreen") + "; }"
            ".capsule-password-row { background: transparent; padding: 5px 8px; border-radius: 6px; }"
            ".capsule-password-row:focus { background: alpha(" + c("accentGreen") + ",0.12); }"
            ".capsule-password-value { font-family: monospace; font-size: 12px; }";
        gtk_css_provider_load_from_data(css,rules.c_str(),-1,nullptr);
        gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(window),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION+2);
    }
    void SendCommand(const std::string& op,const std::string& id="") { send(Json{{"v",1},{"op",op},{"id",id}}.dump()); }
    bool Discard() {
        if (writing) return false;
        if (dirty && !ConfirmCurrent("放弃未保存的修改？","当前编辑尚未保存。")) return false;
        dirty=false; return true;
    }
    void Navigate(Json command) { if (command.at("op")=="search" || Discard()) send(command.dump()); }
    void Changed() { if (!updating) { dirty=true; gtk_label_set_text(GTK_LABEL(status),"未保存的修改"); Controls(); } }
    void ClearDraft() {
        if (read_cancel) read_cancel->store(true);
        draft=Record{}; detail_key.clear(); dirty=false;
        if (resource_editor) resource_editor->Conceal();
        capture_image.reset();
        if(capture_picture) {gtk_widget_queue_draw(capture_picture);gtk_label_set_text(GTK_LABEL(capture_title),"");gtk_label_set_text(GTK_LABEL(capture_meta),"");}
        updating = true;
        gtk_entry_set_text(GTK_ENTRY(title), ""); gtk_entry_set_text(GTK_ENTRY(summary), "");
        if (note_editor) note_editor->LoadPlain("");
        updating = false;
        FillDraft();
        if (clipboard_text) {
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(clipboard_text)), "",
                                     0);
            gtk_label_set_text(GTK_LABEL(clipboard_title), "");
            gtk_label_set_text(GTK_LABEL(clipboard_source), "");
            clipboard_picture.reset();
            gtk_widget_queue_draw(clipboard_image);
            gtk_widget_hide(clipboard_image);
            gtk_label_set_text(GTK_LABEL(clipboard_status), "");
        }
    }
    void Controls() {
        const bool notes=state.module=="notes" && std::find(state.modules.begin(),state.modules.end(),Module::Notes)!=state.modules.end();
        const bool temporary = state.module == "temporary" && !state.password_field &&
                               state.visible && state.managing &&
                               std::find(state.modules.begin(), state.modules.end(),
                                         Module::Temporary) != state.modules.end();
        const bool resources = state.module == "resources" && std::find(state.modules.begin(),state.modules.end(),Module::Resources)!=state.modules.end();
        const bool captures=state.module=="capture" && std::find(state.modules.begin(),state.modules.end(),Module::Capture)!=state.modules.end();
        const bool passwords=state.module=="passwords" && std::find(state.modules.begin(),state.modules.end(),Module::Passwords)!=state.modules.end();
        if(password_view)gtk_widget_set_visible(password_view->Widget(),passwords);
        if(migrate_password)gtk_widget_set_visible(migrate_password,notes && std::find(state.modules.begin(),state.modules.end(),Module::Passwords)!=state.modules.end());
        gtk_widget_set_visible(capture_form,captures);
        gtk_widget_set_visible(note_form, !temporary && !resources && !captures && !passwords);
        if (resource_editor) {
            gtk_widget_set_visible(resource_editor->Widget(), resources);
            resource_editor->SetEditable(resources && state.visible && state.managing && !state.password_field && !writing);
        }
        gtk_widget_set_visible(note_actions, !temporary && !captures && !passwords);
        gtk_widget_set_visible(clipboard_form, temporary);
        gtk_widget_set_sensitive(clipboard_copy, temporary && !state.selected_id.empty());
        const bool editable=(notes || resources) && state.visible && state.managing && !state.password_field && !writing;
        gtk_editable_set_editable(GTK_EDITABLE(title),editable); gtk_text_view_set_editable(GTK_TEXT_VIEW(body),editable && rich_supported);
        gtk_widget_set_sensitive(create,(notes || resources || captures || passwords) && !writing); gtk_widget_set_sensitive(save,editable);
        gtk_widget_set_sensitive(remove,!draft.id.empty() && !writing && (notes || resources));
        gtk_editable_set_editable(GTK_EDITABLE(summary),editable);
        gtk_widget_set_sensitive(format, editable && rich_supported && !draft.projection_conflict);
        const int selected = gtk_combo_box_get_active(GTK_COMBO_BOX(format));
        note_editor->SetFormat(selected == 2, selected == 1);
        gtk_widget_set_visible(rich_bar, selected == 2);
        gtk_widget_set_visible(bullet_bar, selected == 1);
        if (selected == 2)
            gtk_widget_show_all(rich_bar);
        gtk_widget_set_visible(projection_warning, draft.projection_conflict);
        if (draft.projection_conflict)
            gtk_widget_show_all(projection_warning);
        gtk_widget_set_sensitive(rich_bar,
                                 editable && rich_supported && !draft.projection_conflict);
        gtk_widget_set_sensitive(bullet_bar,editable);
        gtk_widget_set_sensitive(search,!writing);
    }
    void ClipboardCommand(const char* op) {
        if (state.module != "temporary" || !state.visible || state.password_field)
            return;
        const auto found =
            std::find_if(state.cards.begin(), state.cards.end(),
                         [&](const Card& card) { return card.id == state.selected_id; });
        if (found == state.cards.end())
            return;
        send(Json{{"v", 1}, {"op", op}, {"id", found->id}, {"revision", found->revision}}.dump());
    }
    void ClipboardMenu() {
        if (state.module != "temporary" || !state.visible || state.password_field)
            return;
        auto* menu = gtk_menu_new();
        const auto found =
            std::find_if(state.cards.begin(), state.cards.end(),
                         [&](const Card& card) { return card.id == state.selected_id; });
        if (found == state.cards.end()) {
            gtk_widget_destroy(menu);
            return;
        }
        for (const auto& action : std::vector<std::pair<const char*, std::string>>{
                 {found->favorite ? "取消收藏" : "收藏", "favorite"}, {"删除", "remove"}}) {
            auto* item = gtk_menu_item_new_with_label(action.first);
            auto* callback = new std::function<void()>(
                [this, op = action.second, id = found->id, revision = found->revision] {
                    if (state.visible && !state.password_field && state.module == "temporary")
                        send(Json{{"v", 1}, {"op", op}, {"id", id}, {"revision", revision}}.dump());
                });
            g_signal_connect_data(
                item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer raw) {
                    (*static_cast<std::function<void()>*>(raw))();
                }),
                callback,
                +[](gpointer raw, GClosure*) { delete static_cast<std::function<void()>*>(raw); },
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
    void ClipboardDetail(const Card& card) {
        gtk_label_set_text(GTK_LABEL(clipboard_title), card.title.c_str());
        gtk_label_set_text(GTK_LABEL(clipboard_source), card.source.c_str());
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(clipboard_text)),
                                 card.preview.data(), card.preview.size());
        if (preview)
            return;
        read_cancel = std::make_shared<std::atomic<bool>>(false);
        const auto cancelled = read_cancel;
        const auto life = alive;
        const auto id = card.id, revision = card.revision, key = detail_key;
        if (!jobs.Run(cancelled, [this, life, cancelled, id, revision, key] {
            std::string text, types;
            GdkPixbuf* image = nullptr;
            bool ok = false;
            try {
                const auto grant = packages->Inspect(ModulePlugin(Module::Temporary));
                packages->WithEnabledGrant(grant.id, grant.grant, [&] {
                    if (!life->load() || cancelled->load())
                        return;
                    ClipboardHistoryStore store(ClipboardHistoryStore::DefaultRoot());
                    const auto archive = store.Read(id, revision);
                    if (!archive)
                        return;
                    const auto projection = ProjectClipboard(*archive);
                    text = CharacterPrefix(projection.text, 16000);
                    std::replace(text.begin(), text.end(), '\0', ' ');
                    if (text.size() < projection.text.size())
                        text += "\n…（完整内容保留，可复制恢复）";
                    for (const auto& value : archive->representations) {
                        if (!types.empty())
                            types += " · ";
                        types += value.type;
                        if (!image && value.type.rfind("image/", 0) == 0 &&
                            value.bytes.size() <= 32 * 1024 * 1024) {
                            auto* loader = gdk_pixbuf_loader_new();
                            g_signal_connect(
                                loader, "size-prepared",
                                G_CALLBACK(+[](GdkPixbufLoader* loader, int width, int height,
                                               gpointer) {
                                    const double scale = std::min({1.0, 1000.0 / std::max(1, width),
                                                                   1000.0 / std::max(1, height)});
                                    gdk_pixbuf_loader_set_size(
                                        loader, std::max(1, static_cast<int>(width * scale)),
                                        std::max(1, static_cast<int>(height * scale)));
                                }),
                                nullptr);
                            if (gdk_pixbuf_loader_write(
                                    loader, reinterpret_cast<const guchar*>(value.bytes.data()),
                                    value.bytes.size(), nullptr) &&
                                gdk_pixbuf_loader_close(loader, nullptr)) {
                                image = gdk_pixbuf_loader_get_pixbuf(loader);
                                if (image)
                                    g_object_ref(image);
                            } else
                                gdk_pixbuf_loader_close(loader, nullptr);
                            g_object_unref(loader);
                        }
                    }
                    ok = true;
                });
            } catch (const std::exception&) {
            }
            auto picture = std::shared_ptr<GdkPixbuf>(image, [](GdkPixbuf* value) {
                if (value)
                    g_object_unref(value);
            });
            Post([this, life, cancelled, key, ok, text = std::move(text), types = std::move(types),
                  picture] {
                if (!life->load() || cancelled->load() || detail_key != key ||
                    state.module != "temporary" || !state.visible || state.password_field)
                    return;
                if (!ok) {
                    gtk_label_set_text(GTK_LABEL(clipboard_status), "条目已变化，请重新选择");
                    return;
                }
                gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(clipboard_text)),
                                         text.data(), text.size());
                gtk_widget_set_tooltip_text(clipboard_copy, types.c_str());
                if (picture) {
                    clipboard_picture = picture;
                    gtk_widget_queue_draw(clipboard_image);
                    gtk_widget_show(clipboard_image);
                }
                gtk_label_set_text(GTK_LABEL(clipboard_status), "已加载临时条目");
            });
        })) gtk_label_set_text(GTK_LABEL(clipboard_status), "读取正忙，请重新选择此条目");
    }
    void CaptureCommand(const std::string& op) {
        if(state.module!="capture" || !state.visible || state.password_field || state.selected_id.empty()) return;
        for(const auto& card:state.cards) if(card.id==state.selected_id)
            send(Json{{"v",1},{"op",op},{"id",card.id},{"revision",card.revision}}.dump());
    }
    void CaptureDetail(const Card& card) {
        gtk_label_set_text(GTK_LABEL(capture_title),card.title.c_str());gtk_label_set_text(GTK_LABEL(capture_meta),card.preview.c_str());
        gtk_widget_set_sensitive(capture_collect,!card.favorite);gtk_widget_set_sensitive(capture_remove,!card.favorite);
        if(preview) return;
        read_cancel=std::make_shared<std::atomic<bool>>(false);const auto cancelled=read_cancel,life=alive;const auto key=detail_key;
        jobs.Run(cancelled,[this,cancelled,life,key,card] {
            std::shared_ptr<GdkPixbuf> picture;std::string message;
            try {
                const auto grant=packages->Inspect(ModulePlugin(Module::Capture));
                if(!grant.enabled || cancelled->load()) return;
                CaptureStore store(CaptureStore::DefaultRoot());const auto captured=store.Acquire(card.id,card.revision);
                ContentStore library(ContentStore::DefaultRoot());Record record;
                if(captured) {record.kind=captured->record.kind=="video"?Kind::Video:Kind::Image;record.content=captured->path.string();}
                else {std::string error;if(library.Get(card.id,&record,&error)!=StoreStatus::Ok || record.revision!=card.revision || RecordModule(record)!=Module::Capture) throw std::runtime_error("捕获已变化，请重新选择");}
                const auto preview=PreviewResource(record,library,*cancelled);message=preview.message;
                if(!preview.png.empty()) picture=ClipboardThumbnail(ClipboardArchive{{{"image/png",preview.png}}},1000);
                const auto after=packages->Inspect(grant.id);if(!after.enabled || after.grant!=grant.grant) return;
            } catch(const std::exception& e) {message=e.what();}
            Post([this,life,cancelled,key,picture,message] {
                if(!life->load() || cancelled->load() || key!=detail_key || state.module!="capture" || state.password_field || !state.visible) return;
                capture_image=picture;gtk_widget_queue_draw(capture_picture);
                gtk_label_set_text(GTK_LABEL(capture_meta),(state.selected_preview+"\n"+message).c_str());
            });
        });
    }
    void NewNote() {
        if(state.module=="passwords") {if(password_view)password_view->New();return;}
        if(state.module=="capture") {if(open_capture) open_capture();return;}
        if ((state.module!="notes" && state.module!="resources") || !Discard()) return;
        ClearDraft(); draft.kind=Kind::Note;
        if (state.module == "resources") {
            draft.kind = (state.filter == "reference" || state.filter == "all") ? Kind::Pdf : state.filter == "skills" ? Kind::Skill : Kind::Resource;
            draft.resource_type = state.filter == "document" || state.filter == "project" ? state.filter : "other";
            FillDraft();
        }
        dirty=true; Controls();
        gtk_label_set_text(GTK_LABEL(status),state.module=="resources"?"新建资源":"新建笔记"); if(state.module=="notes") gtk_widget_grab_focus(title);
    }
    void Detail(const Card* card) {
        if (!card) { if (!dirty) ClearDraft(); return; }
        const auto key=card->id+":"+card->revision;
        if (key==detail_key || dirty || writing) return;
        ClearDraft(); detail_key=key;
        if (card->module == Module::Temporary) {
            ClipboardDetail(*card);
            return;
        }
        if (card->module == Module::Capture) {CaptureDetail(*card);return;}
        if (card->module == Module::Passwords) {
            gtk_label_set_text(GTK_LABEL(status),card->preview.c_str()); return;
        }
        if (preview) {
            draft.id=card->id; draft.kind=card->kind; draft.title=card->title; draft.content=card->payload.empty()?card->preview:card->payload;
            if (card->kind == Kind::Pdf) {
                draft.reference.kind="journalArticle";
                draft.reference.fields={{"authors","Ada Lovelace; Alan Turing"},{"year","2024"},{"container","Journal of Test Fixtures"},{"volume","12"},{"pages","10–20"},{"doi","10.1000/example"}};
            }
            FillDraft();
            gtk_label_set_text(GTK_LABEL(status),(std::to_string(state.count)+" 条本地记录").c_str()); return;
        }
        read_cancel=std::make_shared<std::atomic<bool>>(false);
        const auto cancelled=read_cancel; const auto id=card->id; const auto revision=card->revision;
        const auto storage=ContentStore::DefaultRoot(); auto life=alive;
        jobs.Run(cancelled,[this,life,cancelled,id,revision,key,storage] {
            ContentStore store(storage); Record value; std::string error;
            const auto result=store.Get(id,&value,&error);
            if (!life->load() || cancelled->load()) return;
            Post([this,life,cancelled,key,revision,result,value=std::move(value)] {
                if (!life->load() || cancelled->load() || !state.visible || !state.managing || state.password_field || detail_key!=key || dirty) return;
                if (result!=StoreStatus::Ok || value.revision!=revision) { gtk_label_set_text(GTK_LABEL(status),"条目已变动，请重新选择"); return; }
                draft=value; FillDraft(); gtk_label_set_text(GTK_LABEL(status),
                                   !value.format_issue.empty() ? value.format_issue.c_str()
                                   : value.projection_conflict ? "Markdown 副本已被修改"
                                   : value.note_format == "rich-text"
                                       ? (rich_supported ? "富文本与附件已载入"
                                                         : "此富文本格式仅可查看，原始附件已保留")
                                       :"本机条目"); Controls();
            });
        });
    }
    void Save() {
        if (preview || writing || (state.module!="notes" && state.module!="resources") || state.password_field || !state.visible) return;
        Record value=CaptureDraft();
        if (draft.projection_conflict) {
            gtk_label_set_text(GTK_LABEL(status), "请先显式导入 Markdown 副本，草稿已保留");
            return;
        }
        if (value.note_format == "rich-text" && value.rich_asset.empty()) {
            gtk_label_set_text(GTK_LABEL(status), "富文本资产超过 4 MiB 上限，草稿已保留"); return;
        }
        if (value.title.empty() || value.content.empty()) { gtk_label_set_text(GTK_LABEL(status),"请填写标题和内容"); return; }
        Write(std::move(value),false);
    }
    void Remove() {
        if (preview || writing || draft.id.empty() || !Discard() ||
            !ConfirmCurrent("删除这条内容？","仅删除当前条目，附件文件仍保留。")) return;
        Write(draft,true);
    }
    void Write(Record value,bool removing) {
        const auto storage=ContentStore::DefaultRoot(); auto life=alive;
        const auto module = RecordModule(value);
        const bool import_requested = module == Module::Resources && resource_editor->ImportRequested();
        const auto authority=packages->Inspect(ModulePlugin(module));
        if (!authority.enabled) { gtk_label_set_text(GTK_LABEL(status),"模块插件已停用，草稿已保留"); return; }
        write_cancel=std::make_shared<std::atomic<bool>>(false); const auto cancelled=write_cancel;
        writing=true; Controls(); gtk_label_set_text(GTK_LABEL(status),"正在保存…");
        auto complete=[this,life,removing,module](StoreStatus result,Record saved,std::string error = {}) {
            if (!life->load()) return;
            Post([this,life,result,saved=std::move(saved),removing,module,error=std::move(error)] {
                if (!life->load()) return;
                writing=false;
                if (result==StoreStatus::Ok) {
                    dirty=false; if (module == Module::Notes) suspended_draft.reset(); else suspended_resource.reset();
                    suspended_rich.reset();
                    ClearDraft();
                    if (state.visible && state.managing && !state.password_field) SendCommand("refresh",removing?"":saved.id);
                } else {
                    if (suspended_draft && state.visible && state.managing && !state.password_field && state.module=="notes") {
                        draft=*suspended_draft; detail_key=suspended_key; FillDraft();
                        if (suspended_rich) {
                            note_editor->Load(*suspended_rich);
                            rich_supported = true;
                            suspended_rich.reset();
                            Controls();
                        }
                        dirty=true; suspended_draft.reset();
                    }
                    if (suspended_resource && state.visible && state.managing && !state.password_field && state.module == "resources" && module == Module::Resources) {
                        draft = *suspended_resource; detail_key = suspended_resource_key; FillDraft(); resource_editor->SetImportRequested(suspended_resource_import); dirty = true; suspended_resource.reset();
                    }
                    gtk_label_set_text(GTK_LABEL(status),result==StoreStatus::RevisionConflict?"条目已被修改，草稿已保留；重新读取后再保存":error.empty()?"保存失败，草稿已保留":error.c_str());
                }
                Controls();
            });
        };
        auto work=[this,storage,value=std::move(value),removing,cancelled,module,import_requested,grant=authority.grant,complete]() mutable {
            ContentStore store(storage); Record saved; std::string error;
            auto result=StoreStatus::Invalid;
            try {
                packages->WithEnabledGrant(ModulePlugin(module),grant,[&] {
                    if (cancelled->load()) return;
                    if (!removing && module == Module::Resources) {
                        if (value.attachment.empty() && !ValidateResource(value, &error)) return;
                        if (import_requested && value.attachment.empty() && store.ImportAttachment(value.content, &value.attachment, &error) != StoreStatus::Ok) return;
                        if (!import_requested) value.attachment.clear();
                    }
                    if (!cancelled->load()) result=removing?store.Remove(value.id,value.revision,&error):store.Put(value,value.revision,&saved,&error);
                });
            } catch (const std::exception&) { /* Revoked operations retain the draft. */ }
            complete(result,std::move(saved),std::move(error));
        };
        if(!jobs.Run(cancelled,std::move(work),[complete]{complete(StoreStatus::Invalid,{});})) complete(StoreStatus::Invalid,{});
    }

    void Apply(const Snapshot& snapshot) {
        if(password_view)password_view->Apply(snapshot);
        const bool notes_enabled=std::find(snapshot.modules.begin(),snapshot.modules.end(),Module::Notes)!=snapshot.modules.end();
        if (dirty && state.module=="notes" && (!snapshot.visible || !snapshot.managing || snapshot.password_field || snapshot.module!="notes" || !notes_enabled)) {
            suspended_draft=CaptureDraft(); suspended_key=detail_key;
            if (gtk_combo_box_get_active(GTK_COMBO_BOX(format)) == 2 && rich_supported)
                suspended_rich = note_editor->Capture();
        }
        const bool resources_enabled = std::find(snapshot.modules.begin(),snapshot.modules.end(),Module::Resources)!=snapshot.modules.end();
        if (dirty && state.module == "resources" && (!snapshot.visible || !snapshot.managing || snapshot.password_field || snapshot.module != "resources" || !resources_enabled)) {
            suspended_resource = CaptureDraft(); suspended_resource_key = detail_key;
            suspended_resource_import = resource_editor->ImportRequested();
        }
        if (confirmation && (!snapshot.visible || !snapshot.managing || snapshot.password_field ||
                             snapshot.module != state.module || (state.module=="notes"?!notes_enabled:!resources_enabled)))
            gtk_dialog_response(GTK_DIALOG(confirmation), GTK_RESPONSE_CANCEL);
        const bool changed_module = state.module != snapshot.module;
        state=snapshot;
        if (changed_module)
            ClearDraft();
        if (theme!=state.theme) SetTheme(state.theme);
        if (!state.visible || !state.managing || state.password_field) {
            // Ordinary close/navigation has already asked about drafts. A
            // protection/revocation transition must conceal immediately.
            if (write_cancel) write_cancel->store(true);
            ClearDraft(); gtk_widget_hide(window); return;
        }
        const bool enabled=std::any_of(state.modules.begin(),state.modules.end(),[&](auto module){return state.module==ModuleRaw(module);});
        if (!enabled || (state.module != "notes" && state.module != "temporary" && state.module != "resources" && state.module != "capture" && state.module != "passwords")) {
            if(write_cancel) write_cancel->store(true);
            ClearDraft();
        } else if (state.module == "notes" && suspended_draft && !writing) {
            draft=*suspended_draft; detail_key=suspended_key; FillDraft();
            if (suspended_rich) {
                note_editor->Load(*suspended_rich);
                rich_supported = true;
                suspended_rich.reset();
                Controls();
            }
            dirty=true;
            suspended_draft.reset(); gtk_label_set_text(GTK_LABEL(status),"已恢复未保存的草稿");
        }
        if (state.module == "resources" && resources_enabled && suspended_resource && !writing) {
            draft=*suspended_resource; detail_key=suspended_resource_key; FillDraft(); resource_editor->SetImportRequested(suspended_resource_import); dirty=true;
            suspended_resource.reset(); gtk_label_set_text(GTK_LABEL(status), "已恢复未保存的草稿");
        }
        gtk_entry_set_placeholder_text(GTK_ENTRY(search), state.module == "passwords" ? "仅搜索密码标题" : "搜索标题或内容");
        updating=true;
        if (state.query!=gtk_entry_get_text(GTK_ENTRY(search))) gtk_entry_set_text(GTK_ENTRY(search),state.query.c_str());
        updating=false;
        gtk_label_set_text(GTK_LABEL(count),CountText(state.count).c_str()); gtk_label_set_text(GTK_LABEL(hint),state.hint.c_str());
        Clear(modules);
        for (auto module : state.modules) {
            const auto& definition=ModuleDefinition(module); const bool selected=state.module==ModuleRaw(module);
            auto* button=Button(definition.at("title"),[this,module] { Navigate({{"v",1},{"op","module"},{"text",ModuleRaw(module)}}); },"capsule-module");
            gtk_button_set_image(GTK_BUTTON(button),Icon(definition.at("icon"),11)); gtk_button_set_always_show_image(GTK_BUTTON(button),TRUE);
            if(selected) Class(button,"selected");
            gtk_widget_set_halign(button,GTK_ALIGN_START); Add(modules,button);
            if(selected) for(const auto& filter:definition.at("filters")) {
                auto* choice=Button(filter.at("title"),[this,id=filter.at("id").get<std::string>()] { Navigate({{"v",1},{"op","filter"},{"text",id}}); },"capsule-filter");
                if(state.filter==filter.at("id").get<std::string>()) Class(choice,"selected");
                gtk_widget_set_halign(choice,GTK_ALIGN_START); Add(modules,choice);
            }
        }
        auto* focused=gtk_window_get_focus(GTK_WINDOW(window));
        const bool list_focused=focused && gtk_widget_is_ancestor(focused,list);
        Clear(list);
        const Card* selected=nullptr;
        GtkWidget* selected_row=nullptr;
        for(const auto& card:state.cards) {
            auto* row=Button("",[this,id=card.id] { Navigate({{"v",1},{"op","select"},{"id",id}}); },"capsule-entry");
            auto* column = Column(4);
            auto* row_heading = Row(7);
            auto* name = Label(card.title, "name");
            auto* summary =
                Label(card.module == Module::Temporary ? card.source : card.preview, "caption");
            std::string kind=card.kind==Kind::Pdf?"文献":card.kind==Kind::Resource?"资源":KindRaw(card.kind); std::transform(kind.begin(),kind.end(),kind.begin(),[](unsigned char c){return std::toupper(c);});
            Add(row_heading,Label(kind,"capsule-kind")); Add(row_heading,name,true);
            gtk_widget_set_valign(column,GTK_ALIGN_CENTER); gtk_widget_set_size_request(row,-1,56);
            for(auto* label:{name,summary}) { gtk_label_set_xalign(GTK_LABEL(label),0); gtk_label_set_ellipsize(GTK_LABEL(label),PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(label),1); }
            Add(column,row_heading); Add(column,summary);
            gtk_widget_destroy(gtk_bin_get_child(GTK_BIN(row))); gtk_container_add(GTK_CONTAINER(row),column);
            if(card.id==(dirty?draft.id:state.selected_id)) Class(row,"selected");
            if(card.id==state.selected_id) { selected=&card; selected_row=row; }
            Add(list,row);
        }
        if(state.cards.empty()) Add(list,Label(state.loading?"正在读取…":"暂无条目","caption"));
        Detail(selected); Controls();
        const bool opening=!gtk_widget_get_visible(window);
        gtk_widget_show_all(window); if(opening) gtk_window_present(GTK_WINDOW(window));
        if(list_focused && selected_row) gtk_widget_grab_focus(selected_row);
    }
};
ManagerWindow::ManagerWindow(Send send,bool preview,CopyFile file,CopyText text,std::function<void()> capture):impl_(std::make_unique<Impl>(std::move(send),preview,std::move(file),std::move(text),std::move(capture))){}
ManagerWindow::~ManagerWindow()=default;
void ManagerWindow::Apply(const Snapshot& snapshot){impl_->Apply(snapshot);}
void ManagerWindow::ClipboardStatus(const std::string& status) {
    if (impl_->state.module == "temporary" && !impl_->state.password_field)
        gtk_label_set_text(GTK_LABEL(impl_->clipboard_status), status.c_str());
    else if (impl_->state.module == "resources" && !impl_->state.password_field)
        gtk_label_set_text(GTK_LABEL(impl_->status), status.c_str());
}
GtkWidget* ManagerWindow::Window() const{return impl_->window;}
GtkWidget* ManagerWindow::Root() const{return impl_->root;}
nlohmann::json ManagerWindow::PreviewGeometry() const {
    Json controls = Json::object();
    gboolean dark=FALSE;
    g_object_get(gtk_settings_get_for_screen(gtk_widget_get_screen(impl_->window)),
                 "gtk-application-prefer-dark-theme",&dark,nullptr);
    const std::vector<std::pair<const char*, GtkWidget*>> fields = {
        {"search", impl_->search}, {"new", impl_->create},
        {"list", impl_->list_scroll}, {"editor", impl_->editor},
        {"title", impl_->title}, {"summary", impl_->summary},
        {"format", impl_->format},
        {"body", gtk_widget_get_ancestor(impl_->body, GTK_TYPE_SCROLLED_WINDOW)},
        {"save", impl_->save}, {"remove", impl_->remove}
    };
    for (const auto& [name, widget] : fields) {
        if (!widget) continue;
        int x = 0, y = 0;
        gtk_widget_translate_coordinates(widget, impl_->root, 0, 0, &x, &y);
        controls[name] = {{"x", x}, {"y", y},
                         {"width", gtk_widget_get_allocated_width(widget)},
                         {"height", gtk_widget_get_allocated_height(widget)}};
    }
    return {{"width", gtk_widget_get_allocated_width(impl_->root)},
            {"height", gtk_widget_get_allocated_height(impl_->root)},
            {"keyWindow", bool(gtk_window_is_active(GTK_WINDOW(impl_->window)))},
            {"appearance", dark?"dark":"light"},
            {"scale", gtk_widget_get_scale_factor(impl_->root)},
            {"selectedRow", impl_->state.selected}, {"controls", controls}};
}
} // namespace rimes::capsule
