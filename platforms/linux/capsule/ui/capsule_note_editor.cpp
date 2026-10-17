#include "capsule_note_editor.hpp"
#include "capsule_hash.hpp"
#include "capsule_structure.hpp"
#include "capsule_text.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
namespace rimes::capsule {
namespace {
std::string Text(GtkTextBuffer* buffer, GtkTextIter a, GtkTextIter b) {
    auto* text = gtk_text_buffer_get_text(buffer, &a, &b, TRUE);
    std::string out = text;
    g_free(text);
    return out;
}
std::string AssetName(GObject* object) {
    auto* name = object ? static_cast<const char*>(g_object_get_data(object, "capsule-note-asset"))
                        : nullptr;
    return name ? name : "";
}
void SetAsset(GObject* object, const std::string& name) {
    g_object_set_data_full(object, "capsule-note-asset", g_strdup(name.c_str()), g_free);
}
std::optional<std::string> ReadImage(const char* path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return {};
    struct Owner {
        int fd;
        ~Owner() { close(fd); }
    } owner{fd};
    struct stat info {};
    constexpr std::size_t limit = 3 * 1024 * 1024;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        info.st_size > static_cast<off_t>(limit))
        return {};
    std::string bytes;
    std::array<char, 65536> chunk{};
    while (bytes.size() < limit) {
        const auto n = read(fd, chunk.data(), std::min(chunk.size(), limit - bytes.size()));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return {};
        if (!n)
            break;
        bytes.append(chunk.data(), n);
    }
    struct stat after {};
    if (fstat(fd, &after) || bytes.size() != static_cast<std::size_t>(after.st_size) ||
        after.st_mtim.tv_sec != info.st_mtim.tv_sec ||
        after.st_mtim.tv_nsec != info.st_mtim.tv_nsec)
        return {};
    return bytes;
}
GdkPixbuf* Picture(const std::string& bytes, int* original_width = nullptr,
                   int* original_height = nullptr) {
    auto* loader = gdk_pixbuf_loader_new();
    struct Size {
        bool safe = true;
        int width = 0, height = 0;
    } size;
    g_signal_connect(
        loader, "size-prepared",
        reinterpret_cast<GCallback>(+[](GdkPixbufLoader* loader, int w, int h, gpointer raw) {
            auto* size = static_cast<Size*>(raw);
            size->width = w;
            size->height = h;
            size->safe =
                w > 0 && h > 0 && w <= 16384 && h <= 16384 && std::int64_t(w) * h <= 16000000;
            const double scale = std::min({1.0, 320.0 / std::max(1, w), 180.0 / std::max(1, h)});
            gdk_pixbuf_loader_set_size(loader, size->safe ? std::max(1, int(w * scale)) : 1,
                                       size->safe ? std::max(1, int(h * scale)) : 1);
        }),
        &size);
    const bool written = gdk_pixbuf_loader_write(
        loader, reinterpret_cast<const guchar*>(bytes.data()), bytes.size(), nullptr);
    const bool valid = gdk_pixbuf_loader_close(loader, nullptr) && written;
    auto* image = valid && size.safe ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
    if (image) {
        g_object_ref(image);
        if (original_width)
            *original_width = size.width;
        if (original_height)
            *original_height = size.height;
    }
    g_object_unref(loader);
    return image;
}
} // namespace
struct NoteEditor::Impl {
    GtkWidget* view;
    GtkTextBuffer* buffer;
    std::function<void()> changed;
    GtkTextTag *bold, *italic, *underline, *strike;
    std::map<std::string, std::string> assets;
    std::deque<RichDocument> undo, redo;
    bool rich = false, bullet = false, loading = false;
    RichRun typing;
    std::vector<GtkTextTag*> link_tags;
    GtkWidget* dialog = nullptr;
    std::uint64_t generation = 0;
    Impl(GtkWidget* widget, std::function<void()> callback)
        : view(widget), buffer(gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget))),
          changed(std::move(callback)) {
        bold = gtk_text_buffer_create_tag(buffer, "capsule-bold", "weight", PANGO_WEIGHT_BOLD,
                                          nullptr);
        italic = gtk_text_buffer_create_tag(buffer, "capsule-italic", "style", PANGO_STYLE_ITALIC,
                                            nullptr);
        underline = gtk_text_buffer_create_tag(buffer, "capsule-underline", "underline",
                                               PANGO_UNDERLINE_SINGLE, nullptr);
        strike =
            gtk_text_buffer_create_tag(buffer, "capsule-strike", "strikethrough", TRUE, nullptr);
        g_signal_connect(buffer, "begin-user-action",
                         reinterpret_cast<GCallback>(+[](GtkTextBuffer*, gpointer raw) {
                             auto* self = static_cast<Impl*>(raw);
                             if (!self->loading) {
                                 self->undo.push_back(self->Capture());
                                 self->Bound(self->undo);
                                 self->redo.clear();
                             }
                         }),
                         this);
        g_signal_connect_after(
            buffer, "insert-text",
            reinterpret_cast<GCallback>(
                +[](GtkTextBuffer*, GtkTextIter* end, gchar* text, gint bytes, gpointer raw) {
                    auto* self = static_cast<Impl*>(raw);
                    if (self->loading || !self->rich)
                        return;
                    auto start = *end;
                    gtk_text_iter_backward_chars(&start, g_utf8_strlen(text, bytes));
                    self->ApplyStyle(self->typing, start, *end);
                }),
            this);
        g_signal_connect(view, "key-press-event",
                         reinterpret_cast<GCallback>(
                             +[](GtkWidget*, GdkEventKey* key, gpointer raw) -> gboolean {
                                 auto* self = static_cast<Impl*>(raw);
                                 if (!gtk_text_view_get_editable(GTK_TEXT_VIEW(self->view)))
                                     return FALSE;
                                 if (key->state & GDK_CONTROL_MASK) {
                                     if (key->keyval == GDK_KEY_z || key->keyval == GDK_KEY_Z) {
                                         self->Undo(key->state & GDK_SHIFT_MASK);
                                         return TRUE;
                                     }
                                     if (self->rich && key->keyval == GDK_KEY_b) {
                                         self->Toggle(self->bold, self->typing.bold);
                                         return TRUE;
                                     }
                                     if (self->rich && key->keyval == GDK_KEY_i) {
                                         self->Toggle(self->italic, self->typing.italic);
                                         return TRUE;
                                     }
                                 }
                                 if (self->bullet && (key->keyval == GDK_KEY_Tab ||
                                                      key->keyval == GDK_KEY_ISO_Left_Tab)) {
                                     self->Indent((key->state & GDK_SHIFT_MASK) ||
                                                  key->keyval == GDK_KEY_ISO_Left_Tab);
                                     return TRUE;
                                 }
                                 return FALSE;
                             }),
                         this);
        g_signal_connect(
            view, "button-press-event",
            reinterpret_cast<GCallback>(+[](GtkWidget*, GdkEventButton* event,
                                            gpointer raw) -> gboolean {
                auto* self = static_cast<Impl*>(raw);
                if (!self->bullet || !gtk_text_view_get_editable(GTK_TEXT_VIEW(self->view)) ||
                    event->button != 1)
                    return FALSE;
                int x, y;
                gtk_text_view_window_to_buffer_coords(
                    GTK_TEXT_VIEW(self->view), GTK_TEXT_WINDOW_WIDGET, event->x, event->y, &x, &y);
                GtkTextIter at{};
                gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(self->view), &at, x, y);
                auto a = at, b = at;
                gtk_text_iter_set_line_offset(&a, 0);
                gtk_text_iter_forward_to_line_end(&b);
                auto line = Text(self->buffer, a, b);
                auto indentation = line.find_first_not_of(" \t");
                if (indentation == line.npos)
                    return FALSE;
                auto toggled = ToggleBullet(line);
                const auto offset = gtk_text_iter_get_line_offset(&at);
                if (!toggled || offset < int(indentation + 2) || offset > int(indentation + 5))
                    return FALSE;
                self->Replace(a, b, *toggled);
                return TRUE;
            }),
            this);
    }
    ~Impl() {
        g_signal_handlers_disconnect_by_data(buffer, this);
        g_signal_handlers_disconnect_by_data(view, this);
    }
    void Bound(std::deque<RichDocument>& history) {
        std::size_t size = 0;
        for (const auto& doc : history) {
            size += doc.PlainText().size();
            for (const auto& [name, bytes] : doc.assets)
                size += name.size() + bytes.size();
        }
        while (history.size() > 32 || (history.size() > 1 && size > 16 * 1024 * 1024)) {
            const auto& doc = history.front();
            size -= doc.PlainText().size();
            for (const auto& [name, bytes] : doc.assets)
                size -= name.size() + bytes.size();
            history.pop_front();
        }
    }
    void ApplyStyle(const RichRun& run, GtkTextIter a, GtkTextIter b) {
        for (const auto& [tag, on] :
             std::initializer_list<std::pair<GtkTextTag*, bool>>{{bold, run.bold},
                                                                 {italic, run.italic},
                                                                 {underline, run.underline},
                                                                 {strike, run.strike}})
            if (on)
                gtk_text_buffer_apply_tag(buffer, tag, &a, &b);
        if (!run.link.empty()) {
            auto* tag = gtk_text_buffer_create_tag(buffer, nullptr, "underline",
                                                   PANGO_UNDERLINE_SINGLE, nullptr);
            link_tags.push_back(tag);
            g_object_set_data_full(G_OBJECT(tag), "capsule-note-link", g_strdup(run.link.c_str()),
                                   g_free);
            gtk_text_buffer_apply_tag(buffer, tag, &a, &b);
        }
    }
    void InsertAsset(GtkTextIter& end, const std::string& name, int width = 2000,
                     int height = 2000) {
        auto found = assets.find(name);
        auto* image = found == assets.end() ? nullptr : Picture(found->second);
        if (image) {
            SetAsset(G_OBJECT(image), name);
            g_object_set_data(G_OBJECT(image), "capsule-width", GINT_TO_POINTER(width + 1));
            g_object_set_data(G_OBJECT(image), "capsule-height", GINT_TO_POINTER(height + 1));
            gtk_text_buffer_insert_pixbuf(buffer, &end, image);
            g_object_unref(image);
        } else {
            auto* anchor = gtk_text_buffer_create_child_anchor(buffer, &end);
            SetAsset(G_OBJECT(anchor), name);
            g_object_set_data(G_OBJECT(anchor), "capsule-width", GINT_TO_POINTER(width + 1));
            g_object_set_data(G_OBJECT(anchor), "capsule-height", GINT_TO_POINTER(height + 1));
            auto* label = gtk_label_new("[内嵌图片已保留]");
            gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(view), label, anchor);
            gtk_widget_show(label);
        }
    }
    void Load(const RichDocument& doc, bool reset = true) {
        loading = true;
        ++generation;
        if (dialog)
            gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        assets = doc.assets;
        gtk_text_buffer_set_text(buffer, "", 0);
        for (auto* tag : link_tags)
            gtk_text_tag_table_remove(gtk_text_buffer_get_tag_table(buffer), tag);
        link_tags.clear();
        typing = {};
        for (const auto& run : doc.runs) {
            GtkTextIter end{};
            gtk_text_buffer_get_end_iter(buffer, &end);
            const int offset = gtk_text_iter_get_offset(&end);
            if (!run.asset.empty())
                InsertAsset(end, run.asset, run.width_twips, run.height_twips);
            else
                gtk_text_buffer_insert(buffer, &end, run.text.data(), run.text.size());
            GtkTextIter start{};
            gtk_text_buffer_get_iter_at_offset(buffer, &start, offset);
            ApplyStyle(run, start, end);
        }
        loading = false;
        if (reset) {
            undo.clear();
            redo.clear();
        }
    }
    RichDocument Capture() const {
        RichDocument result;
        GtkTextIter at{};
        gtk_text_buffer_get_start_iter(buffer, &at);
        while (!gtk_text_iter_is_end(&at)) {
            RichRun run;
            auto* pixbuf = gtk_text_iter_get_pixbuf(&at);
            auto* anchor = gtk_text_iter_get_child_anchor(&at);
            if (pixbuf)
                run.asset = AssetName(G_OBJECT(pixbuf));
            if (anchor)
                run.asset = AssetName(G_OBJECT(anchor));
            if (!run.asset.empty()) {
                auto* object = pixbuf ? G_OBJECT(pixbuf) : G_OBJECT(anchor);
                run.width_twips = GPOINTER_TO_INT(g_object_get_data(object, "capsule-width")) - 1;
                run.height_twips = GPOINTER_TO_INT(g_object_get_data(object, "capsule-height")) - 1;
                auto found = assets.find(run.asset);
                if (found != assets.end())
                    result.assets.emplace(*found);
            } else {
                gchar text[8]{};
                const auto length = g_unichar_to_utf8(gtk_text_iter_get_char(&at), text);
                run.text.assign(text, length);
            }
            run.bold = gtk_text_iter_has_tag(&at, bold);
            run.italic = gtk_text_iter_has_tag(&at, italic);
            run.underline = gtk_text_iter_has_tag(&at, underline);
            run.strike = gtk_text_iter_has_tag(&at, strike);
            auto* tags = gtk_text_iter_get_tags(&at);
            for (auto* tag = tags; tag; tag = tag->next) {
                auto* url = static_cast<const char*>(
                    g_object_get_data(G_OBJECT(tag->data), "capsule-note-link"));
                if (url)
                    run.link = url;
            }
            g_slist_free(tags);
            if (!result.runs.empty() && run.asset.empty() && result.runs.back().SameStyle(run))
                result.runs.back().text += run.text;
            else
                result.runs.push_back(std::move(run));
            gtk_text_iter_forward_char(&at);
        }
        return result;
    }
    void Toggle(GtkTextTag* tag, bool& on) {
        if (!rich || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view)))
            return;
        GtkTextIter a{}, b{};
        if (gtk_text_buffer_get_selection_bounds(buffer, &a, &b)) {
            gtk_text_buffer_begin_user_action(buffer);
            gtk_text_buffer_apply_tag(buffer, tag, &a, &b);
            gtk_text_buffer_end_user_action(buffer);
        } else
            on = true;
        changed();
        gtk_widget_grab_focus(view);
    }
    void Replace(GtkTextIter a, GtkTextIter b, const std::string& text) {
        gtk_text_buffer_begin_user_action(buffer);
        gtk_text_buffer_delete(buffer, &a, &b);
        gtk_text_buffer_insert(buffer, &a, text.data(), text.size());
        gtk_text_buffer_place_cursor(buffer, &a);
        gtk_text_buffer_end_user_action(buffer);
        changed();
    }
    void Indent(bool outdent) {
        GtkTextIter a{}, b{};
        if (!gtk_text_buffer_get_selection_bounds(buffer, &a, &b)) {
            gtk_text_buffer_get_iter_at_mark(buffer, &a, gtk_text_buffer_get_insert(buffer));
            b = a;
        }
        gtk_text_iter_set_line_offset(&a, 0);
        gtk_text_iter_forward_to_line_end(&b);
        const auto text = Text(buffer, a, b);
        std::string result;
        std::size_t start = 0;
        for (;;) {
            const auto end = text.find('\n', start);
            result +=
                IndentBullet(text.substr(start, end == text.npos ? end : end - start), outdent);
            if (end == text.npos)
                break;
            result += '\n';
            start = end + 1;
        }
        Replace(a, b, result);
    }
    void Undo(bool again) {
        auto& from = again ? redo : undo;
        auto& to = again ? undo : redo;
        if (from.empty())
            return;
        to.push_back(Capture());
        Bound(to);
        auto doc = std::move(from.back());
        from.pop_back();
        Load(doc, false);
        changed();
    }
};
NoteEditor::NoteEditor(GtkWidget* view, std::function<void()> changed)
    : impl_(std::make_unique<Impl>(view, std::move(changed))) {}
NoteEditor::~NoteEditor() = default;
void NoteEditor::Load(const RichDocument& doc) { impl_->Load(doc); }
void NoteEditor::LoadPlain(const std::string& text) {
    RichDocument doc;
    doc.runs.push_back({text});
    impl_->Load(doc);
}
RichDocument NoteEditor::Capture() const { return impl_->Capture(); }
void NoteEditor::SetFormat(bool rich, bool bullet) {
    impl_->rich = rich;
    impl_->bullet = bullet;
    if (!rich)
        impl_->typing = {};
}
void NoteEditor::Bold() { impl_->Toggle(impl_->bold, impl_->typing.bold); }
void NoteEditor::Italic() { impl_->Toggle(impl_->italic, impl_->typing.italic); }
void NoteEditor::Undo(bool again) { impl_->Undo(again); }
void NoteEditor::AddBullet() {
    auto doc = impl_->Capture();
    const auto text = doc.PlainText();
    GtkTextIter a{}, b{};
    if (!gtk_text_buffer_get_selection_bounds(impl_->buffer, &a, &b)) {
        gtk_text_buffer_get_iter_at_mark(impl_->buffer, &a,
                                         gtk_text_buffer_get_insert(impl_->buffer));
        b = a;
    }
    impl_->Replace(a, b, (text.empty() || text.back() == '\n' ? "" : "\n") + std::string("- [ ] "));
    gtk_widget_grab_focus(impl_->view);
}
void NoteEditor::Link(GtkWindow* parent) {
    GtkTextIter a{}, b{};
    if (!gtk_text_buffer_get_selection_bounds(impl_->buffer, &a, &b))
        return;
    const auto generation = impl_->generation;
    auto* dialog =
        gtk_dialog_new_with_buttons("添加链接", parent, GTK_DIALOG_MODAL, "取消",
                                    GTK_RESPONSE_CANCEL, "添加", GTK_RESPONSE_ACCEPT, nullptr);
    auto* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), "https://");
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), entry);
    gtk_widget_show_all(dialog);
    impl_->dialog = dialog;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT &&
        generation == impl_->generation && gtk_widget_get_visible(GTK_WIDGET(parent)) &&
        gtk_text_view_get_editable(GTK_TEXT_VIEW(impl_->view))) {
        const std::string link = gtk_entry_get_text(GTK_ENTRY(entry));
        if (link.size() <= 4096 &&
            (link.rfind("https://", 0) == 0 || link.rfind("http://", 0) == 0 ||
             link.rfind("mailto:", 0) == 0) &&
            link.find_first_of("\r\n\"") == link.npos) {
            RichRun run;
            run.link = link;
            gtk_text_buffer_begin_user_action(impl_->buffer);
            impl_->ApplyStyle(run, a, b);
            gtk_text_buffer_end_user_action(impl_->buffer);
            impl_->changed();
        }
    }
    impl_->dialog = nullptr;
    gtk_widget_destroy(dialog);
}
void NoteEditor::Image(GtkWindow* parent) {
    const auto generation = impl_->generation;
    auto* dialog =
        gtk_file_chooser_dialog_new("添加图片", parent, GTK_FILE_CHOOSER_ACTION_OPEN, "取消",
                                    GTK_RESPONSE_CANCEL, "添加", GTK_RESPONSE_ACCEPT, nullptr);
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_add_pixbuf_formats(filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    impl_->dialog = dialog;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT &&
        generation == impl_->generation && gtk_widget_get_visible(GTK_WIDGET(parent)) &&
        gtk_text_view_get_editable(GTK_TEXT_VIEW(impl_->view))) {
        auto* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        auto bytes = ReadImage(path);
        auto extension = std::filesystem::path(path).extension().string();
        g_free(path);
        if (extension.size() > 10 ||
            extension.find_first_not_of(".abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO"
                                        "PQRSTUVWXYZ0123456789") != extension.npos)
            extension = ".image";
        int original_width = 0, original_height = 0;
        auto* picture = bytes ? Picture(*bytes, &original_width, &original_height) : nullptr;
        if (picture) {
            const auto name = "image-" + Sha256Hex(*bytes) + extension;
            impl_->assets[name] = std::move(*bytes);
            GtkTextIter at{};
            gtk_text_buffer_get_iter_at_mark(impl_->buffer, &at,
                                             gtk_text_buffer_get_insert(impl_->buffer));
            gtk_text_buffer_begin_user_action(impl_->buffer);
            GtkTextIter selection_start{}, selection_end{};
            if (gtk_text_buffer_get_selection_bounds(impl_->buffer, &selection_start,
                                                     &selection_end)) {
                gtk_text_buffer_delete(impl_->buffer, &selection_start, &selection_end);
                at = selection_start;
            }
            impl_->InsertAsset(at, name, original_width * 20, original_height * 20);
            gtk_text_buffer_end_user_action(impl_->buffer);
            impl_->changed();
            g_object_unref(picture);
        } else {
            auto* error = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                                 GTK_BUTTONS_CLOSE,
                                                 "图片无法读取或超过 3 MiB / 1600 万像素限制");
            impl_->dialog = error;
            gtk_dialog_run(GTK_DIALOG(error));
            gtk_widget_destroy(error);
        }
    }
    impl_->dialog = nullptr;
    gtk_widget_destroy(dialog);
}
} // namespace rimes::capsule
