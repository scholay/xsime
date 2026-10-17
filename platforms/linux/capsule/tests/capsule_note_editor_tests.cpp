#include "capsule_note_editor.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
using namespace rimes::capsule;
void Check(bool test, const char* message) {
    if (!test)
        throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        gtk_init(&argc, &argv);
        std::ifstream stream(NATIVE_NOTES_FIXTURES);
        const auto fixtures = nlohmann::json::parse(stream);
        auto* view = gtk_text_view_new();
        g_object_ref_sink(view);
        int changes = 0;
        {
            NoteEditor editor(view, [&] { ++changes; });
            editor.SetFormat(true, false);
            for (const auto& fixture : fixtures.at("rich")) {
                const auto bytes = Base64Decode(fixture.at("asset").get<std::string>());
                const auto rich = ReadRichText(*bytes);
                editor.Load(*rich);
                if (!rich->assets.empty()) {
                    GtkTextIter at{};
                    gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                                                   &at);
                    int images = 0;
                    do {
                        if (gtk_text_iter_get_pixbuf(&at))
                            ++images;
                    } while (gtk_text_iter_forward_char(&at));
                    Check(images == 1,
                          "native image must render as a pixbuf, not a fallback label");
                }
                const auto captured = editor.Capture();
                Check(captured.PlainText() == rich->PlainText() &&
                          captured.Markdown() == rich->Markdown() &&
                          captured.assets == rich->assets,
                      "GTK rich text or attachment round trip");
            }
            const auto original = ReadRichText(
                *Base64Decode(fixtures.at("rich").at(1).at("asset").get<std::string>()));
            const auto captured = editor.Capture();
            Check(captured.runs.size() == original->runs.size(), "GTK attachment runs changed");
            for (std::size_t i = 0; i < captured.runs.size(); ++i)
                Check(captured.runs[i].SameStyle(original->runs[i]),
                      "GTK image geometry/style changed");
            editor.LoadPlain("中文 text");
            auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
            GtkTextIter a{}, b{};
            gtk_text_buffer_get_iter_at_offset(buffer, &a, 0);
            gtk_text_buffer_get_iter_at_offset(buffer, &b, 2);
            gtk_text_buffer_select_range(buffer, &a, &b);
            editor.Bold();
            Check(editor.Capture().Markdown() == "**中文** text", "GTK bold range");
            editor.Undo();
            Check(editor.Capture().Markdown() == "中文 text", "GTK rich undo");
            editor.Undo(true);
            Check(editor.Capture().Markdown() == "**中文** text", "GTK rich redo");
            editor.LoadPlain("- [ ] first\n- [x] second");
            editor.SetFormat(false, true);
            gtk_text_buffer_get_iter_at_offset(buffer, &a, 0);
            gtk_text_buffer_place_cursor(buffer, &a);
            GdkEventKey key{};
            key.type = GDK_KEY_PRESS;
            key.keyval = GDK_KEY_Tab;
            gboolean handled = FALSE;
            g_signal_emit_by_name(view, "key-press-event", &key, &handled);
            Check(handled && editor.Capture().PlainText() == "  - [ ] first\n- [x] second",
                  "GTK Bullet Tab");
            key.keyval = GDK_KEY_ISO_Left_Tab;
            handled = FALSE;
            g_signal_emit_by_name(view, "key-press-event", &key, &handled);
            Check(handled && editor.Capture().PlainText() == "- [ ] first\n- [x] second",
                  "GTK Bullet Shift Tab");
            Check(changes >= 5, "editor changes were not reported");
        }
        gtk_widget_destroy(view);
        g_object_unref(view);
        std::cout << "GTK rich formatting, native assets, undo/redo and Bullet "
                     "editing passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
