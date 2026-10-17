#include "capsule_password.hpp"
#include "capsule_password_view.hpp"
#include "clipboard_desktop.hpp"
#include "desktop.hpp"
#include "official_catalog.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace rimes::capsule;
using Clock = std::chrono::steady_clock;
std::string stage = "initialize";
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
GtkWidget* Find(GtkWidget* root, const std::string& name) {
    if (name == gtk_widget_get_name(root))
        return root;
    if (!GTK_IS_CONTAINER(root))
        return nullptr;
    auto* children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget* result = nullptr;
    for (auto* n = children; n && !result; n = n->next)
        result = Find(GTK_WIDGET(n->data), name);
    g_list_free(children);
    return result;
}
GtkWidget* Button(GtkWidget* root, const std::string& label) {
    if (GTK_IS_BUTTON(root) && gtk_button_get_label(GTK_BUTTON(root)) &&
        label == gtk_button_get_label(GTK_BUTTON(root)))
        return root;
    if (!GTK_IS_CONTAINER(root))
        return nullptr;
    auto* children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget* result = nullptr;
    for (auto* n = children; n && !result; n = n->next)
        result = Button(GTK_WIDGET(n->data), label);
    g_list_free(children);
    return result;
}
std::string Text(GtkWidget* view) {
    auto* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter a{}, z{};
    gtk_text_buffer_get_bounds(b, &a, &z);
    auto* raw = gtk_text_buffer_get_text(b, &a, &z, FALSE);
    std::string value = raw;
    g_free(raw);
    return value;
}
std::string VisibleText(GtkWidget* root) {
    if (!gtk_widget_get_visible(root))
        return "";
    std::string value;
    if (GTK_IS_LABEL(root))
        value += gtk_label_get_text(GTK_LABEL(root));
    if (GTK_IS_ENTRY(root))
        value += gtk_entry_get_text(GTK_ENTRY(root));
    if (GTK_IS_TEXT_VIEW(root))
        value += Text(root);
    if (GTK_IS_CONTAINER(root)) {
        auto* children = gtk_container_get_children(GTK_CONTAINER(root));
        for (auto* n = children; n; n = n->next)
            value += VisibleText(GTK_WIDGET(n->data));
        g_list_free(children);
    }
    return value;
}
void Wait(const std::function<bool()>& condition, int milliseconds = 8000) {
    const auto until = Clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        while (gtk_events_pending())
            gtk_main_iteration();
        if (condition())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (Clock::now() < until);
    throw std::runtime_error("vault GTK condition timed out: " + stage);
}
int main(int argc, char** argv) {
    const auto root =
        std::filesystem::temp_directory_path() / ("xsime-vault-ui-" + std::to_string(getpid()));
    try {
        Check(!std::filesystem::exists(root), "fixture path already exists");
        setenv("RIMES_USER_DIR", root.c_str(), 1);
        gtk_init(&argc, &argv);
        rimes::plugins::PackageStore packages(
            root / "plugins", rimes::plugins::OfficialCatalog(), rimes::plugins::BundledPackages(),
            rimes::plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
        PasswordStore store(ContentStore::DefaultRoot());
        PasswordRecord draft;
        draft.title = "公开测试账号";
        draft.summary = "仅供测试";
        draft.category = "login";
        draft.body =
            "- 用户名：public-user\n- 密码：public-fixture-secret\n- TOTP：JBSWY3DPEHPK3PXP";
        auto entry = store.Put(draft, "");
        Snapshot state;
        state.visible = true;
        state.managing = true;
        state.module = "passwords";
        state.modules = {Module::Notes, Module::Passwords};
        state.selected_id = entry.id;
        state.cards = {PasswordCard(entry)};
        auto* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_default_size(GTK_WINDOW(window), 460, 660);
        rimes::desktop::Theme("night");
        std::vector<std::string> commands;
        int refreshes = 0;
        PasswordView* active = nullptr;
        {
            PasswordView view(GTK_WINDOW(window), [&](std::string raw) {
                commands.push_back(raw);
                const auto command = nlohmann::json::parse(raw);
                if (command.at("op") == "refresh") {
                    ++refreshes;
                    state.cards.clear();
                    for (const auto& value : store.List())
                        state.cards.push_back(PasswordCard(value));
                    if (command.contains("id") && !command.at("id").get<std::string>().empty())
                        state.selected_id = command.at("id");
                    active->Apply(state);
                }
            });
            active = &view;
            gtk_container_add(GTK_CONTAINER(window), view.Widget());
            gtk_widget_show_all(window);
            view.Apply(state);
            auto get = [&](const char* name) {
                auto* value = Find(view.Widget(), name);
                Check(value, "missing vault control");
                return value;
            };
            auto click = [&](const char* name) {
                auto* widget = get(name);
                Check(gtk_widget_is_sensitive(widget), "vault control disabled");
                gtk_button_clicked(GTK_BUTTON(widget));
            };
            int focus_count = 0;
            auto focus = [&] {
                stage = "focus " + std::to_string(++focus_count);
                gtk_window_present(GTK_WINDOW(window));
                gdk_window_focus(gtk_widget_get_window(window), GDK_CURRENT_TIME);

                Wait([&] { return gtk_window_is_active(GTK_WINDOW(window)); });
            };
            focus();
            auto key = [&](unsigned code, bool down, guint modifiers = 0) {
                GdkEventKey event{};
                event.type = down ? GDK_KEY_PRESS : GDK_KEY_RELEASE;
                event.hardware_keycode = code;
                event.keyval = GDK_KEY_a;
                event.state = modifiers;
                gboolean handled = FALSE;
                g_signal_emit_by_name(window, down ? "key-press-event" : "key-release-event",
                                      &event, &handled);
                Check(handled, "physical chord event escaped into host input");
            };
            auto attempt = [&](const RevealCode& code) {
                gtk_widget_grab_focus(get("vault-chord-input"));
                auto* retry = Button(view.Widget(), "重新输入");
                gtk_button_clicked(GTK_BUTTON(retry));
                for (const auto& chord : code) {
                    for (char c : chord) {
                        unsigned hardware = 0;
                        for (unsigned i = 0; i < 128; ++i)
                            if (RevealPhysicalLetter(i) == c)
                                hardware = i;
                        Check(hardware, "physical key mapping missing");
                        key(hardware, true);
                    }
                    for (char c : chord) {
                        for (unsigned i = 0; i < 128; ++i)
                            if (RevealPhysicalLetter(i) == c)
                                key(i, false);
                    }
                }
            };
            auto unlock = [&](RevealCode code = DefaultRevealCode()) {
                focus();
                stage = "unlock " + std::to_string(focus_count);
                attempt(code);
                try {
                    Wait([&] { return gtk_widget_get_sensitive(get("vault-edit")); });
                } catch (...) {
                    throw std::runtime_error(stage + ": " + gtk_label_get_text(GTK_LABEL(get("vault-status"))));
                }
            };
            auto absent = [&] {
                return VisibleText(view.Widget()).find("public-fixture-secret") ==
                           std::string::npos &&
                       Text(get("vault-plaintext")).empty();
            };
            Check(absent() && Find(view.Widget(), "vault-row-0") == nullptr,
                  "locked view decrypted before authorization");
            gchar* im_module = nullptr;
            g_object_get(get("vault-plaintext"), "im-module", &im_module, nullptr);
            Check(im_module && std::string(im_module) == "gtk-im-context-simple",
                  "secret editor exposed its context to an external input service");
            g_free(im_module);
            stage = "wrong credential";
            attempt({"a", "b", "c", "d"});
            Wait([&] {
                return std::string(gtk_label_get_text(GTK_LABEL(get("vault-status"))))
                           .find("不正确") != std::string::npos;
            });
            Check(absent(), "wrong credential revealed plaintext");
            // Modifiers and autorepeated key presses cannot add or merge challenge slots.
            gtk_widget_grab_focus(get("vault-chord-input"));
            key(24, true, GDK_CONTROL_MASK);
            key(24, false, GDK_CONTROL_MASK);
            Check(absent(), "modifier bypassed challenge");
            unlock();
            Check(VisibleText(view.Widget()).find("public-user") != std::string::npos && absent(),
                  "fields were not masked after unlock");
            Check(VisibleText(view.Widget()).find("JBSWY3DPEHPK3PXP") == std::string::npos,
                  "TOTP seed visible in detail");
            click("vault-reveal-1");
            Check(VisibleText(view.Widget()).find("public-fixture-secret") != std::string::npos,
                  "single field reveal missing");
            stage = "copy";
            click("vault-row-1");
            Wait([&] {
                return std::string(gtk_label_get_text(GTK_LABEL(get("vault-status"))))
                           .find("已复制") != std::string::npos;
            });
            auto* copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
            Check(copied && std::string(copied) == "public-fixture-secret",
                  "vault explicit copy did not paste expected value");
            g_free(copied);
            Check(gtk_clipboard_wait_is_target_available(
                      gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
                      gdk_atom_intern("application/x-rimes-sensitive", FALSE)),
                  "vault clipboard marker missing");
            stage = "paste into password field";
            auto protected_field = state;
            protected_field.password_field = true;
            protected_field.visible = false;
            Check(DecodeSnapshot(EncodeSnapshot(protected_field), &protected_field), "password field wire snapshot");
            view.Apply(protected_field);
            Check(absent(), "password field retained a revealed editor");
            const auto copied_until = Clock::now() + std::chrono::milliseconds(100);
            Wait([&] { return Clock::now() >= copied_until; });
            copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
            Check(copied && std::string(copied) == "public-fixture-secret", "password field revoked accepted clipboard copy");
            g_free(copied);
            protected_field.session_protected = true;
            view.Apply(protected_field);
            Wait([&] { return !gtk_clipboard_wait_is_target_available(
                gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), gdk_atom_intern("application/x-rimes-sensitive", FALSE)); });
            view.Apply(state);
            unlock();
            click("vault-reveal-2");
            Check(VisibleText(view.Widget()).find("JBSWY3DPEHPK3PXP") == std::string::npos,
                  "TOTP reveal leaked seed");
            click("vault-edit");
            Check(Text(get("vault-plaintext")) == draft.body, "editor did not load decrypted body");
            stage = "category popup";
            gtk_combo_box_popup(GTK_COMBO_BOX(get("vault-category")));
            Wait([&] {
                gboolean shown = FALSE;
                g_object_get(get("vault-category"), "popup-shown", &shown, nullptr);
                return shown;
            });
            const auto popup_deadline = Clock::now() + std::chrono::milliseconds(120);
            while (Clock::now() < popup_deadline) {
                while (gtk_events_pending())
                    gtk_main_iteration();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            Check(Text(get("vault-plaintext")) == draft.body,
                  "owned category popup concealed plaintext");
            Check(g_spawn_command_line_sync("xdotool key Down Return", nullptr, nullptr, nullptr,
                                            nullptr),
                  "could not select category");
            Wait([&] {
                gboolean shown = TRUE;
                g_object_get(get("vault-category"), "popup-shown", &shown, nullptr);
                return !shown;
            });
            focus();
            Check(Text(get("vault-plaintext")) == draft.body,
                  "closing category popup concealed plaintext");
            stage = "editor copy";
            auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(get("vault-plaintext")));
            GtkTextIter first{}, last{};
            gtk_text_buffer_get_bounds(buffer, &first, &last);
            gtk_text_buffer_select_range(buffer, &first, &last);
            while (gtk_events_pending())
                gtk_main_iteration();
            copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY));
            Check(!copied || std::string(copied) != draft.body,
                  "selection leaked password through PRIMARY");
            g_free(copied);
            g_signal_emit_by_name(get("vault-plaintext"), "copy-clipboard");
            Wait([&] {
                auto* value =
                    gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
                bool ok = value && std::string(value) == draft.body;
                g_free(value);
                return ok;
            });
            Check(gtk_clipboard_wait_is_target_available(
                      gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
                      gdk_atom_intern("org.nspasteboard.ConcealedType", FALSE)),
                  "text editor copied without sensitive marker");
            stage = "save";
            gtk_entry_set_text(GTK_ENTRY(get("vault-title")), "已加密修改");
            click("vault-save");
            Wait([&] { return refreshes == 1; });
            entry = store.Inspect(entry.id);
            Check(entry.title == "已加密修改" && entry.category == "key" &&
                      store.Read(entry.id, entry.revision).body == draft.body,
                  "GUI encrypted save failed");
            Check(absent(), "save retained plaintext");
            unlock();
            click("vault-edit");
            stage = "external focus with category popup";
            gtk_text_buffer_set_text(buffer, "temporary unsaved secret", -1);
            gtk_combo_box_popup(GTK_COMBO_BOX(get("vault-category")));
            Wait([&] {
                gboolean shown = FALSE;
                g_object_get(get("vault-category"), "popup-shown", &shown, nullptr);
                return shown;
            });
            stage = "external window mapping";
            auto* other_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
            gtk_container_add(GTK_CONTAINER(other_window), gtk_entry_new());
            gtk_window_set_default_size(GTK_WINDOW(other_window), 200, 80);
            gtk_window_move(GTK_WINDOW(other_window), 900, 30);
            gtk_widget_show_all(other_window);
            Wait([&] { return gtk_widget_get_mapped(other_window); });
            stage = "outside menu dismissal";
            int other_x = 0, other_y = 0;
            gdk_window_get_origin(gtk_widget_get_window(other_window), &other_x, &other_y);
            auto outside = "xdotool mousemove " + std::to_string(other_x + 20) + " " +
                           std::to_string(other_y + 20) + " click 1";
            Check(g_spawn_command_line_sync(outside.c_str(), nullptr, nullptr, nullptr, nullptr),
                  "could not dismiss menu outside window");
            Wait([&] {
                gboolean shown = TRUE;
                g_object_get(get("vault-category"), "popup-shown", &shown, nullptr);
                return !shown;
            });
            stage = "external window active";
            gtk_window_present(GTK_WINDOW(other_window));
            Wait([&] { return gtk_window_is_active(GTK_WINDOW(other_window)); });
            stage = "external window conceal";
            Wait([&] { return absent(); });
            Check(Text(get("vault-plaintext")).empty() && absent(),
                  "focus loss retained decrypted editor");
            gtk_widget_destroy(other_window);
            unlock();
            click("vault-edit");
            {
                auto storage_lock =
                    rimes::runtime::PrivateDirectory(ContentStore::DefaultRoot()).Lock();
                gtk_text_buffer_set_text(buffer, "- 密码：cancelled-while-store-locked", -1);
                click("vault-save");
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                const auto before = Clock::now();
                view.Conceal();
                Check(Clock::now() - before < std::chrono::milliseconds(500),
                      "busy storage blocked immediate conceal");
            }
            Check(store.Inspect(entry.id).revision == entry.revision,
                  "concealed pending write changed encrypted record");
            unlock();
            auto external = store.Read(entry.id, entry.revision);
            external.body = "- 密码：externally-updated";
            entry = store.Put(external, entry.revision);
            Wait([&] { return !gtk_widget_get_sensitive(get("vault-edit")); });
            Check(absent(), "external record mutation did not revoke lease");
            state.cards = {PasswordCard(entry)};
            view.Apply(state);
            unlock();
            RevealCredential verifier(ContentStore::DefaultRoot());
            const RevealCode custom = {"ab", "cd", "ef", "gh"};
            verifier.Change(DefaultRevealCode(), custom, verifier.Revision());
            Wait([&] { return !gtk_widget_get_sensitive(get("vault-edit")); });
            Check(absent(), "credential replacement did not revoke old lease");
            unlock(custom);
            auto permission = packages.Inspect(ModulePlugin(Module::Passwords));
            packages.Enable(permission.id, false, permission.grant);
            Wait([&] { return !gtk_widget_get_sensitive(get("vault-edit")); });
            Check(absent(), "plugin revocation retained detail");
            permission = packages.Inspect(permission.id);
            packages.Enable(permission.id, true, permission.grant);
            // Change the current credential through the real four-chord UI, including mismatch
            // retry.
            auto* configure = Button(view.Widget(), "查看口令…");
            Check(configure, "credential configuration entry missing");
            gtk_button_clicked(GTK_BUTTON(configure));
            auto* change = Button(view.Widget(), "更改查看口令…");
            Check(change, "credential UI missing");
            gtk_button_clicked(GTK_BUTTON(change));
            attempt(custom);
            Wait([&] {
                return VisibleText(view.Widget()).find("输入新的四组") != std::string::npos;
            });
            const RevealCode next = {"qw", "er", "ty", "ui"};
            attempt(next);
            Check(VisibleText(view.Widget()).find("再次输入") != std::string::npos,
                  "credential confirmation missing");
            attempt({"a", "b", "c", "d"});
            Check(VisibleText(view.Widget()).find("不一致") != std::string::npos,
                  "mismatched credential accepted");
            attempt(next);
            attempt(next);
            Wait([&] { return verifier.Matches(next) && gtk_widget_is_sensitive(change); });
            Check(!verifier.Matches(custom), "old credential remained valid");
            auto* reset = Button(view.Widget(), "恢复默认口令…");
            gtk_button_clicked(GTK_BUTTON(reset));
            attempt(next);
            Wait([&] {
                return verifier.Matches(DefaultRevealCode()) && gtk_widget_is_sensitive(reset);
            });
            unlock();
            click("vault-edit");
            Wait([&] { return Text(get("vault-plaintext")).empty(); }, 17000);
            Check(absent(), "15-second editor expiry retained plaintext");
            // A new editor survives a harmless host refresh, then writes a distinct record.
            focus();
            view.New();
            view.Apply(state);
            Wait([&] {
                return gtk_widget_get_sensitive(get("vault-save")) &&
                       !Text(get("vault-plaintext")).empty();
            });
            gtk_entry_set_text(GTK_ENTRY(get("vault-title")), "新建密码");
            gtk_text_buffer_set_text(buffer, "- 密码：new-public-fixture", -1);
            const int before_new = refreshes;
            click("vault-save");
            Wait([&] { return refreshes == before_new + 1 && store.List().size() == 2; });
            Wait([&] { return refreshes >= 3; });
            unlock();
            stage = "delete confirmation";
            click("vault-remove");
            Check(store.List().size() == 2, "first delete click removed record");
            const int before_delete = refreshes;
            click("vault-remove");
            // Observe the UI acknowledgement before inspecting storage. Repeated
            // List() calls hold the same nonblocking lock as the delete worker.
            Wait([&] { return refreshes > before_delete; });
            Check(store.List().size() == 1, "confirmed delete did not remove the record");
            state.selected_id = entry.id;
            state.cards = {PasswordCard(entry)};
            view.Apply(state);
            unlock();
            state.password_field = true;
            state.session_protected = true;
            state.visible = false;
            view.Apply(state);
            Check(absent() && Text(get("vault-plaintext")).empty(),
                  "lock/sleep protection retained plaintext");
            for (const auto& command : commands)
                Check(command.find("public-fixture-secret") == std::string::npos &&
                          command.find("externally-updated") == std::string::npos,
                      "decrypted content reached host IPC");
        }
        gtk_widget_destroy(window);
        std::filesystem::remove_all(root);
        std::cout << "GTK vault: physical chords, masked detail, TOTP, explicit copy markers, "
                     "PRIMARY exclusion, encrypted CRUD, "
                     "focus/credential/record/plugin/15-second/lock revocation passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
