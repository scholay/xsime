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
