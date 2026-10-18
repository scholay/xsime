#include "capsule_store.hpp"
#include "mailbox_workspace.hpp"
#include "official_catalog.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace rimes::mailbox;
namespace {
std::string stage;
void Check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(stage + ": " + message);
}
void Wait(const std::function<bool()>& condition, int milliseconds = 6000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        while (gtk_events_pending())
            gtk_main_iteration();
        try {
            if (condition())
                return;
        } catch (const rimes::runtime::StorageBusy&) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < until);
    throw std::runtime_error(stage + ": timed out");
}
template <class F> auto Stable(F action) -> decltype(action()) {
    for (int attempt = 0;; ++attempt) {
        try {
            return action();
        } catch (const rimes::runtime::StorageBusy&) {
            if (attempt == 100)
                throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}
void Pump(int milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    Wait([&] { return std::chrono::steady_clock::now() >= until; }, milliseconds + 1000);
}
GtkWidget* Find(GtkWidget* root, const std::string& name) {
    if (name == gtk_widget_get_name(root))
        return root;
    if (!GTK_IS_CONTAINER(root))
        return nullptr;
    auto* children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget* found = nullptr;
    for (auto* c = children; c && !found; c = c->next)
        found = Find(GTK_WIDGET(c->data), name);
    g_list_free(children);
    return found;
}
GtkWidget* Accessible(GtkWidget* root, const std::string& name) {
    const auto* value = atk_object_get_name(gtk_widget_get_accessible(root));
    if (value && name == value)
        return root;
    if (!GTK_IS_CONTAINER(root))
        return nullptr;
    auto* children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget* found = nullptr;
    for (auto* c = children; c && !found; c = c->next)
        found = Accessible(GTK_WIDGET(c->data), name);
    g_list_free(children);
    return found;
}
std::string Text(GtkWidget* view) {
    GtkTextIter start{}, end{};
    auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    auto* raw = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    std::string text = raw;
    g_free(raw);
    return text;
}
void Click(GtkWidget* button) {
    Check(button && GTK_IS_BUTTON(button) && gtk_widget_is_sensitive(button), "button unavailable");
    gtk_button_clicked(GTK_BUTTON(button));
}
} // namespace
int main(int argc, char** argv) {
    const auto root =
        std::filesystem::temp_directory_path() / ("xsime-mailbox-ui-" + std::to_string(getpid()));
    try {
        Check(!std::filesystem::exists(root), "fixture already exists");
        setenv("RIMES_USER_DIR", root.c_str(), 1);
        gtk_init(&argc, &argv);
        const auto fixture = Json::parse(std::ifstream(RIMES_MAILBOX_FIXTURE));
        rimes::runtime::PrivateDirectory(root / "mailbox")
            .Write("mailbox.json", fixture.at("document").dump());
        Store store(root / "mailbox");
        const std::string inbox = fixture.at("selectedInbox"), chat = fixture.at("selectedChat");
        rimes::plugins::PackageStore packages(
            root / "plugins", rimes::plugins::OfficialCatalog(), rimes::plugins::BundledPackages(),
            rimes::plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
        {
            Workspace host;
            stage = "initial shield";
            Check(host.Protected() && !host.Open("inbox", inbox),
                  "initial unknown session exposed content");
            host.Protect(false);
            Wait([&] { return host.Inspect().at("ready") == true; });
            stage = "open existing receipt";
            Check(host.Open("inbox", inbox), "could not open inbox");
            Wait([&] { return gtk_window_is_active(GTK_WINDOW(host.Window())); });
            stage = "visible receipt mark read";
            Wait([&] { return store.Get(inbox).at("unread") == false; });
            auto* composer = Find(host.Content(), "mailbox-composer-input");
            Check(composer, "composer missing");
            stage = "wide layout";
            gtk_window_resize(GTK_WINDOW(host.Window()), 1000, 700);
            Wait([&] {
                return host.Inspect().at("narrow") == false &&
                       host.Inspect().at("sidebarVisible") == true;
            });
            stage = "compact drawer";
            gtk_window_resize(GTK_WINDOW(host.Window()), 680, 500);
            Wait([&] {
                return host.Inspect().at("narrow") == true &&
                       host.Inspect().at("sidebarVisible") == false;
            });
            Click(Accessible(host.Content(), "显示或隐藏列表"));
            Wait([&] { return host.Inspect().at("sidebarOverlay") == true; });
            Click(Accessible(host.Content(), "收起侧栏"));
            Wait([&] { return host.Inspect().at("sidebarVisible") == false; });
            stage = "independent drafts";
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer)),
                                     "公开的本地草稿🙂", -1);
            Click(Find(host.Content(), "mailbox-nav-chat"));
            Wait([&] { return host.Inspect().at("module") == "chat"; });
            Check(Text(composer).empty(), "inbox draft leaked into chat");
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer)),
                                     "独立的对话草稿", -1);
            Click(Find(host.Content(), "mailbox-nav-inbox"));
            Wait([&] { return Text(composer) == "公开的本地草稿🙂"; });
            stage = "local note save";
            Wait([&] { return gtk_widget_is_sensitive(Find(host.Content(), "mailbox-send")); });
            Click(Find(host.Content(), "mailbox-send"));
            Wait([&] { return Text(composer).empty(); });
            Check(Stable([&] { return store.Get(inbox); }).at("messages").back().at("body") ==
                      "公开的本地草稿🙂",
                  "note content changed");
            Check(Stable([&] { return store.Get(inbox); }).at("messages").back().at("kind") ==
                      "localNote",
                  "note became a remote reply");
            Wait([&] {
                return host.Inspect().at("revision") == store.Read().revision &&
                       host.Inspect().at("busy") == false;
            });
            stage = "committed draft cleared after navigation";
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer)),
                                     "已保存后立即切换", -1);
            Click(Find(host.Content(), "mailbox-send"));
            // Observe the durable write without dispatching its GTK completion.
            const auto durable_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (store.Get(inbox).at("messages").back().at("body") != "已保存后立即切换") {
                Check(std::chrono::steady_clock::now() < durable_deadline, "note did not persist");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            host.Hide();
            Pump(100);
            Check(host.Open("inbox", inbox), "could not reopen saved receipt");
            Check(Text(composer).empty(),
                  "published note remained as a duplicate draft after navigation");
            Wait([&] {
                return host.Inspect().at("revision") == store.Read().revision &&
                       host.Inspect().at("busy") == false;
            });
            stage = "saving preserves newer draft";
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer)),
                                     "正在保存的旧草稿", -1);
            Wait([&] { return gtk_widget_is_sensitive(Find(host.Content(), "mailbox-send")); });
            Click(Find(host.Content(), "mailbox-send"));
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer)),
                                     "保存期间继续写的新草稿", -1);
            Wait([&] {
                return store.Get(inbox).at("messages").back().at("body") == "正在保存的旧草稿" &&
                       gtk_widget_is_sensitive(Find(host.Content(), "mailbox-send"));
            });
            Check(Text(composer) == "保存期间继续写的新草稿", "completion discarded newer text");
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer)), "", 0);
            const std::string inbound_id = store.Get(inbox).at("review").at("messageID");
            const auto action = [&](const std::string& suffix) {
                return Find(host.Content(), "mailbox-message-" + inbound_id + "-" + suffix);
            };
            stage = "real X11 copy";
            Wait([&] { return action("copy") && gtk_widget_is_sensitive(action("copy")); });
            Click(action("copy"));
            Wait([&] { return gtk_widget_is_sensitive(action("copy")); });
            auto* copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
            Check(copied &&
                      copied == store.Get(inbox).at("messages")[0].at("body").get<std::string>(),
                  "copy differs from original receipt");
            g_free(copied);
            stage = "save receipt to Capsule";
            Click(action("capsule"));
            rimes::capsule::ContentStore library(root / "capsule");
            std::vector<rimes::capsule::Record> notes;
            std::string library_error;
            Wait([&] {
                return library.ListAll(&notes, &library_error) == rimes::capsule::StoreStatus::Ok &&
                       notes.size() == 1;
            });
            Wait([&] { return gtk_widget_is_sensitive(action("capsule")); });
            Click(action("capsule"));
            Wait([&] { return gtk_widget_is_sensitive(action("capsule")); });
            Check(library.ListAll(&notes, &library_error) == rimes::capsule::StoreStatus::Ok &&
                      notes.size() == 1,
                  "repeated save duplicated note");
            stage = "ignore keeps content and disables accept";
            Click(action("reject"));
            Wait([&] {
                return store.Get(inbox).at("review").at("state") == "rejected" &&
                       action("buffer") && !gtk_widget_is_sensitive(action("buffer"));
            });
            Check(!action("reject") &&
                      store.Get(inbox).at("messages")[0].at("body") == notes[0].content,
                  "ignore lost original or left repeat action");
            stage = "search no result";
            gtk_entry_set_text(GTK_ENTRY(Find(host.Content(), "mailbox-search")),
                               "无法匹配的测试查询");
            Pump(250);
            Check(!Find(Find(host.Content(), "mailbox-index"), "mailbox-thread-" + inbox),
                  "search kept unmatched receipt");
            gtk_entry_set_text(GTK_ENTRY(Find(host.Content(), "mailbox-search")), "");
            Pump(250);
            stage = "hide/reopen preserves draft";
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(composer)),
                                     "关闭不丢失草稿", -1);
            host.Hide();
            Check(!gtk_widget_get_visible(host.Window()), "hide kept window visible");
            Check(host.Open("inbox", inbox), "reopen failed");
            Check(Text(composer) == "关闭不丢失草稿", "close discarded draft");
            stage = "protected session hides without auto reopening";
            host.Protect(true);
            Check(!gtk_widget_get_visible(host.Window()) && !host.Open("chat", chat),
                  "protected deep link exposed window");
            host.Protect(false);
            Pump(120);
            Check(!gtk_widget_get_visible(host.Window()), "unlock reopened without a user action");
            Check(host.Open("inbox", inbox), "explicit reopen failed");
            stage = "permission revocation";
            auto permission = packages.Inspect("builtin.mailbox.inbox");
            const auto before = Stable([&] { return store.Get(inbox); });
            packages.Enable(permission.id, false, permission.grant);
            Wait([&] { return !Find(host.Content(), "mailbox-nav-inbox"); });
            Check(!host.Open("inbox", inbox), "disabled module opened");
            Check(Stable([&] { return store.Get(inbox); }) == before,
                  "disabling module modified content");
            permission = packages.Inspect(permission.id);
            packages.Enable(permission.id, true, permission.grant);
            Wait([&] { return Find(host.Content(), "mailbox-nav-inbox") != nullptr; });
            Check(host.Open("inbox", inbox), "re-enabled module failed");
            Check(Text(composer) == "关闭不丢失草稿", "module disable lost draft");
            stage = "image preview and export";
            std::ifstream png_file(std::filesystem::path(RIMES_MAILBOX_FIXTURE).parent_path() /
                                       "public-pixel.png",
                                   std::ios::binary);
            const std::string png((std::istreambuf_iterator<char>(png_file)), {});
            auto image_row = Stable([&] {
                return store.Create({{"kind", "http"},
                                     {"displayName", "公开图片来源"},
                                     {"replyCapability", "localNotesOnly"}},
                                    "独立图片", "公开测试图片", "inbox", png);
            });
            const std::string image_thread = image_row.at("id"),
                              image_message = image_row.at("review").at("messageID");
            Wait([&] { return Find(host.Content(), "mailbox-thread-" + image_thread) != nullptr; });
            Check(host.Open("inbox", image_thread), "image receipt did not open");
            Wait([&] { return Find(host.Content(), "mailbox-image-preview") != nullptr; });
            const auto image_action = [&](const char* suffix) {
                return Find(host.Content(), "mailbox-message-" + image_message + "-" + suffix);
            };
            const auto dialog = [&]() -> GtkWidget* {
                GtkWidget* found = nullptr;
                auto* windows = gtk_window_list_toplevels();
                for (auto* item = windows; item && !found; item = item->next)
                    if (GTK_IS_DIALOG(item->data) && gtk_window_get_transient_for(GTK_WINDOW(
                                                         item->data)) == GTK_WINDOW(host.Window()))
                        found = GTK_WIDGET(item->data);
                g_list_free(windows);
                return found;
            };
            Wait([&] {
                return image_action("save") && gtk_widget_is_sensitive(image_action("save"));
            });
            Click(image_action("save"));
            Wait([&] { return dialog() && GTK_IS_FILE_CHOOSER(dialog()); });
            gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_CANCEL);
            Check(!dialog(), "cancel left image chooser open");
            Click(image_action("save"));
            Wait([&] { return dialog() && GTK_IS_FILE_CHOOSER(dialog()); });
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog()), root.c_str());
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog()), "exported.png");
            Pump(150);
            gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
            Wait([&] { return std::filesystem::exists(root / "exported.png"); });
            Check(rimes::runtime::PrivateDirectory(root).Read("exported.png", 1048576) == png,
                  "chooser export changed PNG bytes");
            Wait([&] { return gtk_widget_is_sensitive(image_action("capsule")); });
            Click(image_action("capsule"));
            Wait([&] {
                return library.ListAll(&notes, &library_error) == rimes::capsule::StoreStatus::Ok &&
                       notes.size() == 2;
            });
            const auto saved_image =
                *std::find_if(notes.begin(), notes.end(), [](const auto& value) {
                    return value.kind == rimes::capsule::Kind::Image;
                });
            stage = "confirmed delete preserves Capsule image";
            Wait([&] { return gtk_widget_is_sensitive(image_action("capsule")); });
            Click(Accessible(host.Content(), "删除会话"));
            Wait([&] { return dialog() != nullptr; });
            gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_CANCEL);
            Check(store.Get(image_thread).at("id") == image_thread, "cancel deleted receipt");
            Click(Accessible(host.Content(), "删除会话"));
            Wait([&] { return dialog() != nullptr; });
            gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
            Wait([&] {
                try {
                    store.Get(image_thread);
                    return false;
                } catch (const std::runtime_error&) {
                    return true;
                }
            });
            Check(rimes::runtime::PrivateDirectory(
                      std::filesystem::path(saved_image.content).parent_path())
                          .Read(std::filesystem::path(saved_image.content).filename().string(),
                                1048576) == png,
                  "deletion removed independent Capsule image");
            Pump(200);
            Check(host.Open("inbox", inbox), "original receipt did not reopen");
            Check(Text(composer) == "关闭不丢失草稿",
                  "image interaction replaced another receipt draft");
            stage = "external update";
            auto receipt = Stable([&] {
                return store.Create({{"kind", "http"},
                                     {"displayName", "公开外部来源"},
                                     {"replyCapability", "localNotesOnly"}},
                                    "新的外部收件", std::string(12000, 'x') + "\n中文🙂");
            });
            const std::string new_id = receipt.at("id");
            Wait([&] { return Find(host.Content(), "mailbox-thread-" + new_id) != nullptr; });
            Check(host.Inspect().at("thread") == inbox,
                  "external arrival replaced current selection");
            stage = "corrupt document preservation";
            const auto durable = rimes::runtime::PrivateDirectory(root / "mailbox")
                                     .Read("mailbox.json", 16 * 1048576)
                                     .value();
            rimes::runtime::PrivateDirectory(root / "mailbox").Write("mailbox.json", "{invalid");
            Wait([&] { return !gtk_widget_is_sensitive(Find(host.Content(), "mailbox-send")); });
            Pump(150);
            Check(rimes::runtime::PrivateDirectory(root / "mailbox")
                          .Read("mailbox.json", 16 * 1048576)
                          .value() == "{invalid",
                  "unreadable store was overwritten");
            rimes::runtime::PrivateDirectory(root / "mailbox").Write("mailbox.json", durable);
            Wait([&] { return gtk_widget_is_sensitive(Find(host.Content(), "mailbox-send")); });
            stage = "geometry save";
            gtk_window_resize(GTK_WINDOW(host.Window()), 680, 500);
            Pump(500);
            auto prefs = Json::parse(rimes::runtime::PrivateDirectory(root / "mailbox")
                                         .Read("window.json", 4096)
                                         .value());
            Check(prefs.at("width") == 680 && prefs.at("height") == 500,
                  "minimum window size not saved");
            host.Hide();
        }
        {
            stage = "restarted window";
            Workspace restored;
            restored.Protect(false);
            Wait([&] { return restored.Inspect().at("ready") == true; });
            Check(restored.Open(), "restart could not open");
            Wait([&] { return restored.Inspect().at("narrow") == true; });
            int width = 0, height = 0;
            gtk_window_get_size(GTK_WINDOW(restored.Window()), &width, &height);
            Check(width == 680 && height == 500, "restart lost window dimensions");
            Check(!store.Get(inbox).at("messages").empty(), "restart lost messages");
        }
        std::filesystem::remove_all(root);
        std::cout << "Mailbox GTK: startup shield, read marker, modules/drafts, responsive drawer, "
                     "local notes, search, protection, revocation, external updates, corruption "
                     "and restart passed\n";
    } catch (const std::exception& error) {
        std::cerr << stage << ": " << error.what() << "\n";
        std::filesystem::remove_all(root);
        return 1;
    }
}
