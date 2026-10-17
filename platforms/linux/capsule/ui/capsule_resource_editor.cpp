#include "background_jobs.hpp"
#include "capsule_resource_editor.hpp"
#include "capsule_text.hpp"
#include "desktop.hpp"
#include "official_catalog.hpp"
#include "private_files.hpp"
#include <algorithm>
namespace rimes::capsule {
using namespace rimes::desktop;
namespace {
void Post(std::function<void()> fn) {
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        +[](gpointer raw) -> gboolean {
            (*static_cast<std::function<void()>*>(raw))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(fn)),
        +[](gpointer raw) { delete static_cast<std::function<void()>*>(raw); });
}
GtkWidget* Field(GtkWidget* form, const char* label, GtkWidget* widget) {
    auto* row = Column(5);
    Add(row, Label(label, "capsule-field-label"));
    Add(row, widget);
    Add(form, row);
    return row;
}
} // namespace
struct ResourceEditor::Impl {
    GtkWindow* owner;
    GtkWidget *root, *form, *title, *classification, *path, *reference_type, *reference_form,
        *citation_style, *citation, *citation_copy, *import, *file_copy, *preview_image,
        *preview_text, *notice, *heading, *classification_row, *choose, *folder, *chooser = nullptr;
    std::map<std::string, GtkWidget*> fields;
    std::function<void()> changed;
    std::function<void(const Record&)> copy_file;
    std::function<void(const std::string&)> copy_text;
    Record record;
    bool updating = false, enabled = true, fixture = false;
    std::shared_ptr<GdkPixbuf> picture;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true), cancelled;
    runtime::BackgroundJobs jobs;
    std::unique_ptr<plugins::PackageStore> packages;
    Impl(GtkWindow* window, std::function<void()> edit, std::function<void(const Record&)> file,
         std::function<void(const std::string&)> text, bool sample)
        : owner(window), changed(std::move(edit)), copy_file(std::move(file)),
          copy_text(std::move(text)), fixture(sample) {
        if (!fixture)
            packages = std::make_unique<plugins::PackageStore>(
                runtime::UserRoot() / "plugins", plugins::OfficialCatalog(),
                plugins::BundledPackages(), plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
        root = Class(gtk_scrolled_window_new(nullptr, nullptr), "capsule-resource-form");
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(root), GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        form = Column(10);
        gtk_container_add(GTK_CONTAINER(root), form);
        for (auto setter : {gtk_widget_set_margin_start, gtk_widget_set_margin_end,
                            gtk_widget_set_margin_top, gtk_widget_set_margin_bottom})
            setter(form, 16);
        heading = Label("编辑资源", "capsule-form-heading");
        Add(form, heading);
        auto entry = [&](const char* key, const char* caption, const char* placeholder,
                         GtkWidget* parent) {
            auto* widget = Class(gtk_entry_new(), "capsule-field");
            gtk_widget_set_name(widget, (std::string("capsule-resource-") + key).c_str());
            gtk_entry_set_placeholder_text(GTK_ENTRY(widget), placeholder);
            Field(parent, caption, widget);
            g_signal_connect(widget, "changed", G_CALLBACK(+[](GtkEditable*, gpointer raw) {
                                 static_cast<Impl*>(raw)->Changed();
                             }),
                             this);
            return widget;
        };
        title = entry("title", "标题", "标题", form);
        classification = gtk_combo_box_text_new();
        for (const auto* name : {"文档", "项目", "其他"})
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(classification), name);
        classification_row = Field(form, "资源类型", classification);
        gtk_widget_set_name(classification, "capsule-resource-type");
        auto* row = Row(8);
        path = Class(gtk_entry_new(), "capsule-field");
        gtk_widget_set_name(path, "capsule-resource-path");
        gtk_entry_set_placeholder_text(GTK_ENTRY(path), "/home/name/Documents/example.pdf");
        Add(row, path, true);
        choose = Button("选择…", [this] { Choose(false); });
        Add(row, choose);
        folder = Button("文件夹…", [this] { Choose(true); });
        Add(row, folder);
        file_copy = Button("复制文件/文件夹", [this] {
            if (enabled)
                copy_file(Capture());
        });
        gtk_widget_set_name(file_copy, "capsule-resource-copy");
        Add(row, file_copy);
        Field(form, "ABSOLUTE PATH", row);
        import = gtk_check_button_new_with_label("保存附件副本（仅文件，随 Capsule 备份）");
        gtk_widget_set_name(import, "capsule-resource-import");
        reference_form = Column(10);
        Add(form, reference_form);
        reference_type = gtk_combo_box_text_new();
        for (const auto* name : {"图书", "期刊论文", "会议论文", "报告"})
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(reference_type), name);
        Field(reference_form, "文献类型", reference_type);
        gtk_widget_set_name(reference_type, "capsule-resource-reference-type");
        const char* labels[] = {"作者", "年份", "期刊 / 会议", "出版社 / 机构", "卷",
                                "期",   "页码", "DOI",         "URL",           "ISBN"};
        const char* hints[] = {"按文献顺序填写，多个作者用 ; 分隔",
                               "例如 2024",
                               "期刊名或会议名",
                               "图书、报告必填",
                               "可选",
                               "可选",
                               "例如 12–25",
                               "例如 10.1000/xyz",
                               "可选",
                               "可选"};
        for (std::size_t i = 0; i < kReferenceFields.size(); ++i)
            fields[kReferenceFields[i]] =
                entry(kReferenceFields[i], labels[i], hints[i], reference_form);
        citation_style = gtk_combo_box_text_new();
        for (const auto* name : {"GB/T 7714", "APA 7", "MLA 9", "Chicago A-D", "Vancouver", "IEEE"})
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(citation_style), name);
        gtk_widget_set_name(citation_style, "capsule-resource-style");
        Field(reference_form, "引用格式", citation_style);
        gtk_combo_box_set_active(GTK_COMBO_BOX(citation_style), 0);
        if (!fixture)
            try {
                const auto preference = runtime::PrivateDirectory(ContentStore::DefaultRoot())
                                            .Read("reference-style-v1", 64);
                if (preference)
                    for (std::size_t i = 0; i < kCitationStyles.size(); ++i)
                        if (*preference == kCitationStyles[i])
                            gtk_combo_box_set_active(GTK_COMBO_BOX(citation_style), i);
            } catch (const std::exception&) {
            }
        citation = Label("", "caption");
        gtk_label_set_line_wrap(GTK_LABEL(citation), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(citation), 1);
        gtk_label_set_xalign(GTK_LABEL(citation), 0);
        gtk_widget_set_name(citation, "capsule-resource-citation");
        Field(reference_form, "预览", citation);
        citation_copy = Button("复制参考文献", [this] {
            const auto value = Capture();
            const auto formatted = FormatReference(value.title, value.reference, Style());
            if (enabled && formatted)
                copy_text(*formatted);
        });
        gtk_widget_set_name(citation_copy, "capsule-resource-citation-copy");
        Add(reference_form, citation_copy);
        auto* refresh = Button("预览文件", [this] { Preview(); });
        gtk_widget_set_name(refresh, "capsule-resource-preview");
        Add(form, refresh);
        preview_image = gtk_drawing_area_new();
        gtk_widget_set_size_request(preview_image, -1, 220);
        g_signal_connect(
            preview_image, "draw",
            G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer raw) -> gboolean {
                auto* self = static_cast<Impl*>(raw);
                if (!self->picture)
                    return FALSE;
                const auto w = gdk_pixbuf_get_width(self->picture.get());
                const auto h = gdk_pixbuf_get_height(self->picture.get());
                const auto scale = std::min(double(gtk_widget_get_allocated_width(widget)) / w,
                                            double(gtk_widget_get_allocated_height(widget)) / h);
                cairo_save(cr);
                cairo_translate(cr, (gtk_widget_get_allocated_width(widget) - w * scale) / 2,
                                (gtk_widget_get_allocated_height(widget) - h * scale) / 2);
                cairo_scale(cr, scale, scale);
                gdk_cairo_set_source_pixbuf(cr, self->picture.get(), 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
                return FALSE;
            }),
            this);
        Add(form, preview_image);
        gtk_widget_set_no_show_all(preview_image, TRUE);
        preview_text = Label("", "caption");
        gtk_label_set_line_wrap(GTK_LABEL(preview_text), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(preview_text), 1);
        gtk_label_set_xalign(GTK_LABEL(preview_text), 0);
        gtk_label_set_selectable(GTK_LABEL(preview_text), TRUE);
        Add(form, preview_text);
        Add(form, import);
        notice = Label("", "capsule-status");
        gtk_label_set_line_wrap(GTK_LABEL(notice), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(notice), 1);
        Add(form, notice);
        gtk_widget_set_name(notice, "capsule-resource-preview-status");
        for (auto* widget : {reference_type, classification})
            g_signal_connect(widget, "changed", G_CALLBACK(+[](GtkComboBox*, gpointer raw) {
                                 static_cast<Impl*>(raw)->Changed();
                             }),
                             this);
        g_signal_connect(path, "changed", G_CALLBACK(+[](GtkEditable*, gpointer raw) {
                             auto* self = static_cast<Impl*>(raw);
                             if (!self->updating) {
                                 self->record.attachment.clear();
                                 self->ClearPreview();
                             }
                             self->Changed();
                         }),
                         this);
        g_signal_connect(import, "toggled", G_CALLBACK(+[](GtkToggleButton*, gpointer raw) {
                             static_cast<Impl*>(raw)->Changed();
                         }),
                         this);
        g_signal_connect(citation_style, "changed", G_CALLBACK(+[](GtkComboBox*, gpointer raw) {
                             auto* self = static_cast<Impl*>(raw);
                             self->Citation();
                             if (!self->fixture)
                                 try {
                                     runtime::PrivateDirectory(ContentStore::DefaultRoot())
                                         .Write("reference-style-v1", self->Style());
                                 } catch (const std::exception&) {
                                 }
                         }),
                         this);
        gtk_widget_show_all(root);
        for (auto* item : {reference_form, classification_row, folder})
            gtk_widget_set_no_show_all(item, TRUE);
        gtk_widget_hide(preview_image);
        Load({});
    }
    ~Impl() {
        alive->store(false);
        if (cancelled)
            cancelled->store(true);
        jobs.Shutdown();
    }
    std::string Style() const {
        const auto index = gtk_combo_box_get_active(GTK_COMBO_BOX(citation_style));
        return kCitationStyles[std::clamp(index, 0, 5)];
    }
    Record Capture() const {
        Record value = record;
        value.title = gtk_entry_get_text(GTK_ENTRY(title));
        value.content = gtk_entry_get_text(GTK_ENTRY(path));
        const int type = gtk_combo_box_get_active(GTK_COMBO_BOX(classification));
        value.kind =
            record.kind == Kind::Pdf || record.kind == Kind::Skill ? record.kind : Kind::Resource;
        value.resource_type = type == 0 ? "document" : type == 1 ? "project" : "other";
        const char* kinds[] = {"book", "journalArticle", "conferencePaper", "report"};
        value.reference.kind =
            kinds[std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(reference_type)), 0, 3)];
        for (const auto& [name, widget] : fields)
            value.reference.fields[name] = gtk_entry_get_text(GTK_ENTRY(widget));
        return value;
    }
    void Citation() {
        const auto value = Capture();
        const auto text = FormatReference(value.title, value.reference, Style());
        std::string missing;
        for (const auto& field : value.reference.Missing()) {
            if (!missing.empty())
                missing += "、";
            missing += field;
        }
        gtk_label_set_text(
            GTK_LABEL(citation),
            text.value_or("补齐" + (missing.empty() ? "标题" : missing) + "后可生成引用").c_str());
        gtk_widget_set_sensitive(citation_copy, enabled && text.has_value());
    }
    void Changed() {
        if (updating)
            return;
        Citation();
        gtk_widget_set_visible(reference_form, Capture().kind == Kind::Pdf);
        changed();
    }
    void ClearPreview() {
        if (cancelled)
            cancelled->store(true);
        picture.reset();
        gtk_widget_hide(preview_image);
        gtk_label_set_text(GTK_LABEL(preview_text), "");
        gtk_label_set_text(GTK_LABEL(notice), "");
    }
    void Load(const Record& value) {
        ClearPreview();
        record = value;
        updating = true;
        gtk_entry_set_text(GTK_ENTRY(title), value.title.c_str());
        gtk_entry_set_text(GTK_ENTRY(path), value.content.c_str());
        const auto filter = RecordFilter(value);
        gtk_combo_box_set_active(GTK_COMBO_BOX(classification), filter == "document"  ? 0
                                                                : filter == "project" ? 1
                                                                                      : 2);
        gtk_widget_set_visible(classification_row, value.kind == Kind::Resource);
        gtk_widget_set_visible(folder, value.kind != Kind::Pdf);
        gtk_button_set_label(GTK_BUTTON(choose), value.kind == Kind::Pdf ? "选择 PDF…" : "选择…");
        gtk_button_set_label(GTK_BUTTON(file_copy),
                             value.kind == Kind::Pdf ? "复制文件" : "复制文件/文件夹");
        const auto label = value.kind == Kind::Pdf     ? "文献"
                           : value.kind == Kind::Skill ? "Skills"
                                                       : "资源";
        gtk_label_set_text(GTK_LABEL(heading),
                           ((value.id.empty() ? "新建" : "编辑") + std::string(label)).c_str());
        const auto kind = value.reference.kind;
        gtk_combo_box_set_active(GTK_COMBO_BOX(reference_type), kind == "journalArticle"    ? 1
                                                                : kind == "conferencePaper" ? 2
                                                                : kind == "report"          ? 3
                                                                                            : 0);
        for (const auto& [name, widget] : fields)
            gtk_entry_set_text(GTK_ENTRY(widget), value.reference.Get(name).c_str());
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(import), !value.attachment.empty());
        updating = false;
        gtk_widget_set_visible(reference_form, value.kind == Kind::Pdf);
        Citation();
        if (!value.content.empty())
            Preview();
    }
    void Choose(bool directory) {
        if (!enabled)
            return;
        chooser = gtk_file_chooser_dialog_new(
            directory ? "选择文件夹" : "选择文件", owner, GTK_FILE_CHOOSER_ACTION_OPEN, "取消",
            GTK_RESPONSE_CANCEL, "选择", GTK_RESPONSE_ACCEPT, nullptr);
        if (directory)
            gtk_file_chooser_set_action(GTK_FILE_CHOOSER(chooser),
                                        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
        const auto response = gtk_dialog_run(GTK_DIALOG(chooser));
        if (response == GTK_RESPONSE_ACCEPT && enabled) {
            auto* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
            if (filename) {
                gtk_entry_set_text(GTK_ENTRY(path), filename);
                if (std::string(gtk_entry_get_text(GTK_ENTRY(title))).empty())
                    gtk_entry_set_text(GTK_ENTRY(title),
                                       std::filesystem::path(filename).stem().c_str());
                g_free(filename);
                Preview();
            }
        }
        gtk_widget_destroy(chooser);
        chooser = nullptr;
    }
    void Preview() {
        ClearPreview();
        if (fixture || !enabled)
            return;
        const auto value = Capture();
        if (value.content.empty())
            return;
        cancelled = std::make_shared<std::atomic<bool>>(false);
        const auto cancel = cancelled, life = alive;
        gtk_label_set_text(GTK_LABEL(notice), "正在预览…");
        if (!jobs.Run(cancel, [this, value, cancel, life] {
                ResourcePreview result;
                try {
                    const auto grant = packages->Inspect(ModulePlugin(Module::Resources));
                    if (cancel->load())
                        return;
                    if (!grant.enabled)
                        throw std::runtime_error("disabled");
                    result =
                        PreviewResource(value, ContentStore(ContentStore::DefaultRoot()), *cancel);
                    const auto now = packages->Inspect(grant.id);
                    if (!now.enabled || now.grant != grant.grant) {
                        result = {};
                        result.message = "资源插件已停用";
                    }
                } catch (const std::exception&) {
                    result.message = "资源预览不可用";
                }
                Post([this, life, cancel, result = std::move(result)] {
                    if (!life->load() || cancel->load() || !enabled)
                        return;
                    gtk_label_set_text(GTK_LABEL(preview_text), result.text.c_str());
                    gtk_label_set_text(GTK_LABEL(notice), result.message.c_str());
                    if (!result.png.empty()) {
                        auto* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
                        const bool wrote =
                            loader &&
                            gdk_pixbuf_loader_write(
                                loader, reinterpret_cast<const guchar*>(result.png.data()),
                                result.png.size(), nullptr);
                        const bool closed = loader && gdk_pixbuf_loader_close(loader, nullptr);
                        auto* image =
                            wrote && closed ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
                        if (image) {
                            g_object_ref(image);
                            picture = std::shared_ptr<GdkPixbuf>(
                                image, [](GdkPixbuf* item) { g_object_unref(item); });
                            gtk_widget_show(preview_image);
                            gtk_widget_queue_draw(preview_image);
                        }
                        if (loader)
                            g_object_unref(loader);
                    }
                });
            }))
            gtk_label_set_text(GTK_LABEL(notice), "预览正忙，请重试");
    }
    void SetEditable(bool value) {
        enabled = value;
        gtk_widget_set_sensitive(form, value);
        if (!value) {
            ClearPreview();
            if (chooser)
                gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_CANCEL);
        }
    }
};
ResourceEditor::ResourceEditor(GtkWindow* window, std::function<void()> changed,
                               std::function<void(const Record&)> file,
                               std::function<void(const std::string&)> text, bool fixture)
    : impl_(std::make_unique<Impl>(window, std::move(changed), std::move(file), std::move(text),
                                   fixture)) {}
ResourceEditor::~ResourceEditor() = default;
GtkWidget* ResourceEditor::Widget() const { return impl_->root; }
Record ResourceEditor::Capture() const { return impl_->Capture(); }
void ResourceEditor::Load(const Record& record) { impl_->Load(record); }
void ResourceEditor::Conceal() {
    impl_->SetEditable(false);
    impl_->Load({});
}
void ResourceEditor::SetEditable(bool enabled) { impl_->SetEditable(enabled); }
bool ResourceEditor::ImportRequested() const {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(impl_->import));
}
void ResourceEditor::SetImportRequested(bool value) {
    impl_->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl_->import), value);
    impl_->updating = false;
}
} // namespace rimes::capsule
