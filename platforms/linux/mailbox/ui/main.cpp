#include "capsule_hash.hpp"
#include "capsule_resources.hpp"
#include "capsule_text.hpp"
#include "desktop.hpp"
#include "mailbox_session.hpp"
#include "mailbox_workspace.hpp"
#include "terminal_fixtures.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>
using namespace rimes::mailbox;
namespace {
void Pump(int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        while (gtk_events_pending())
            gtk_main_iteration();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < end);
}
int RenderFixture(const std::filesystem::path& input, const std::filesystem::path& output) {
    auto sample = Json::parse(std::ifstream(input));
    if (sample.value("purpose", "") != "public-parity-fixture")
        throw std::runtime_error("render input must be a public fixture");
    char path[] = "/tmp/xsime-mailbox-render-XXXXXX";
    if (!mkdtemp(path))
        throw std::runtime_error("cannot create fixture directory");
    setenv("RIMES_USER_DIR", path, 1);
    try {
        gtk_init(nullptr, nullptr);
        g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, nullptr);
        std::filesystem::create_directories(output);
        auto document = sample.at("document");
        Validate(document);
        rimes::runtime::PrivateDirectory(std::filesystem::path(path) / "mailbox")
            .Write("mailbox.json", document.dump());
        if (sample.contains("images"))
            for (const auto& [name, digest] : sample.at("images").items()) {
                if (std::filesystem::path(name).filename().string() != name)
                    throw std::runtime_error("invalid fixture image");
                std::atomic<bool> cancelled{false};
                const auto bytes = rimes::capsule::ReadResource(
                    std::filesystem::absolute(input.parent_path() / "images" / name), 24 * 1048576,
                    cancelled);
                if (rimes::runtime::Sha256(bytes) != digest.get<std::string>())
                    throw std::runtime_error("fixture image digest differs");
                rimes::runtime::PrivateDirectory(std::filesystem::path(path) / "mailbox/images")
                    .Write(name, bytes);
            }
        Json result = Json::array();
        for (const auto& theme : {"night", "day", "quiet", "rasta"})
            for (const auto& scene : {"inbox", "chat", "terminal", "image", "accepted", "rejected",
                                      "chat-new", "chat-running", "chat-failed"})
                for (bool compact : {false, true}) {
                    Workspace workspace(nullptr, true);
                    const int width = compact ? 680 : 1000, height = compact ? 500 : 700;
                    gtk_widget_set_size_request(workspace.Window(), width, height);
                    gtk_window_set_default_size(GTK_WINDOW(workspace.Window()), width, height);
                    const std::string name = scene;
                    const std::string module = name.rfind("chat", 0) == 0 ? "chat"
                                               : name == "terminal"       ? "terminal"
                                                                          : "inbox";
                    const std::string thread =
                        name == "inbox"  ? sample.at("selectedInbox").get<std::string>()
                        : name == "chat" ? sample.at("selectedChat").get<std::string>()
                        : name == "chat-running"
                            ? sample.at("selectedChatRunning").get<std::string>()
                        : name == "chat-failed" ? sample.at("selectedChatFailed").get<std::string>()
                        : name == "image"       ? sample.at("selectedImage").get<std::string>()
                        : name == "accepted"    ? sample.at("selectedAccepted").get<std::string>()
                        : name == "rejected"    ? sample.at("selectedRejected").get<std::string>()
                                                : "";
                    workspace.Fixture({sample.at("document"), "fixture"}, theme, module, thread,
                                      sample.value("generationPreviews", Json::object()));
                    gtk_widget_show(workspace.Window());
                    Pump(350);
                    auto filename =
                        std::string(theme) + "-" + scene + (compact ? "-compact.png" : "-wide.png");
                    if (!rimes::desktop::Render(workspace.Content(), (output / filename).string(),
                                                2))
                        throw std::runtime_error("cannot render Mailbox");
                    auto inspection = workspace.Inspect();
                    inspection["image"] = filename;
                    inspection["logicalWidth"] =
                        gtk_widget_get_allocated_width(workspace.Content());
                    inspection["logicalHeight"] =
                        gtk_widget_get_allocated_height(workspace.Content());
                    result.push_back(inspection);
                }
        std::ofstream(output / "renders.json") << result.dump(2) << "\n";
    } catch (...) {
        std::filesystem::remove_all(path);
        throw;
    }
    std::filesystem::remove_all(path);
    return 0;
}
struct App {
    std::unique_ptr<Workspace> workspace;
    std::unique_ptr<rimes::runtime::FileLock> primary;
    std::unique_ptr<DesktopSession> session;
    guint open_timer = 0, notify_timer = 0;
    std::string notification;
    int notification_attempts = 0;
    std::string module, thread;
    int attempts = 0;
    bool initial_protection = true, failed = false;
    ~App() {
        if (notify_timer) g_source_remove(notify_timer);
        if (open_timer)
            g_source_remove(open_timer);
        session.reset();
        workspace.reset();
    }
    void NotifySoon(std::string target) {
        if (notify_timer) g_source_remove(notify_timer);
        notification = std::move(target);
        notification_attempts = 0;
        workspace->Refresh();
        notify_timer = g_timeout_add(50, +[](gpointer raw) -> gboolean {
            auto* self = static_cast<App*>(raw);
            ++self->notification_attempts;
            if ((!self->initial_protection && self->workspace->NotifyCompleted(self->notification)) ||
                self->notification_attempts >= 40) {
                self->notify_timer = 0;
                return G_SOURCE_REMOVE;
            }
            return G_SOURCE_CONTINUE;
        }, this);
    }
    void OpenSoon(std::string requested, std::string target) {
        if (open_timer)
            g_source_remove(open_timer);
        module = std::move(requested);
        thread = std::move(target);
        attempts = 0;
        workspace->Refresh();
        open_timer = g_timeout_add(
            50,
            +[](gpointer raw) -> gboolean {
                auto* self = static_cast<App*>(raw);
                ++self->attempts;
                if (self->workspace->Open(self->module, self->thread)) {
                    self->open_timer = 0;
                    return G_SOURCE_REMOVE;
                }
                if (self->attempts >= 40) {
                    self->open_timer = 0;
                    return G_SOURCE_REMOVE;
                }
                return G_SOURCE_CONTINUE;
            },
            this);
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << "XSIME Mailbox " << RIMES_PRODUCT_VERSION << "\n";
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--render-fixture" &&
            std::string(argv[3]) == "--render-dir")
            return RenderFixture(argv[2], argv[4]);
        if (argc == 3 && std::string(argv[1]) == "--render-terminal")
            return RenderTerminalFixture(argv[2]);
        // A profile has one primary application and one lock/sleep monitor. A
        // notification or second launch forwards options over the same bus name.
        const auto identity =
            "org.scholay.rimes.Mailbox.p" +
            rimes::runtime::Sha256(
                std::filesystem::weakly_canonical(rimes::runtime::UserRoot()).string())
                .substr(0, 24);
        auto* app = gtk_application_new(identity.c_str(), G_APPLICATION_HANDLES_COMMAND_LINE);
        g_application_add_main_option(G_APPLICATION(app), "module", 0, G_OPTION_FLAG_NONE,
                                      G_OPTION_ARG_STRING, "Mailbox module: inbox, chat, terminal",
                                      "MODULE");
        g_application_add_main_option(G_APPLICATION(app), "thread", 0, G_OPTION_FLAG_NONE,
                                      G_OPTION_ARG_STRING, "Open an existing Mailbox thread",
                                      "UUID");
        g_application_add_main_option(G_APPLICATION(app), "notify-thread", 0, G_OPTION_FLAG_NONE,
                                      G_OPTION_ARG_STRING, "Show a passive receipt for a completed Mailbox message", "UUID");
        g_application_add_main_option(G_APPLICATION(app), "show", 0, G_OPTION_FLAG_NONE,
                                      G_OPTION_ARG_NONE, "Show the existing Mailbox window",
                                      nullptr);
        g_application_add_main_option(G_APPLICATION(app), "quit", 0, G_OPTION_FLAG_NONE,
                                      G_OPTION_ARG_NONE, "Quit Mailbox", nullptr);
        App state;
        g_signal_connect(app, "startup", G_CALLBACK(+[](GApplication* application, gpointer raw) {
                             auto* state = static_cast<App*>(raw);
                             try {
                                 state->primary = std::make_unique<rimes::runtime::FileLock>(
                                     rimes::runtime::PrivateDirectory(rimes::runtime::UserRoot() /
                                                                      "mailbox" / "host")
                                         .Lock(false));
                             } catch (...) {
                                 state->failed = true;
                                 g_application_quit(application);
                                 return;
                             }
                             try {
                                 Store(rimes::runtime::UserRoot() / "mailbox").RecoverInterrupted();
                             } catch (const std::exception&) {
                             }
                             try {
                                 state->workspace =
                                     std::make_unique<Workspace>(GTK_APPLICATION(application));
                                 state->session =
                                     std::make_unique<DesktopSession>([state](bool value) {
                                         state->workspace->Protect(value);
                                         const bool initial = state->initial_protection;
                                         state->initial_protection = false;
                                         if (value && !initial && state->open_timer) {
                                             g_source_remove(state->open_timer);
                                             state->open_timer = 0;
                                         }
                                     });
                                 g_application_hold(application);
                             } catch (...) {
                                 state->failed = true;
                                 g_application_quit(application);
                             }
                         }),
                         &state);
        g_signal_connect(
            app, "command-line",
            G_CALLBACK((+[](GApplication* application, GApplicationCommandLine* command,
                            gpointer raw) -> gint {
                auto* state = static_cast<App*>(raw);
                if (state->failed || !state->workspace)
                    return 1;
                auto* options = g_application_command_line_get_options_dict(command);
                if (g_variant_dict_contains(options, "quit")) {
                    g_application_quit(application);
                    return 0;
                }
                const gchar *module = "", *thread = "", *notification = "";
                g_variant_dict_lookup(options, "notify-thread", "&s", &notification);
                g_variant_dict_lookup(options, "module", "&s", &module);
                g_variant_dict_lookup(options, "thread", "&s", &thread);
                if ((*module && std::string(module) != "inbox" && std::string(module) != "chat" &&
                     std::string(module) != "terminal") ||
                    (*thread && !rimes::capsule::LooksLikeUuid(thread)) ||
                    (*notification && (!rimes::capsule::LooksLikeUuid(notification) || *thread || *module))) {
                    g_application_command_line_printerr(command,
                                                        "Invalid Mailbox module or thread.\n");
                    return 2;
                }
                std::string target = thread;
                std::transform(target.begin(), target.end(), target.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (*notification) {
                    std::string receipt = notification;
                    std::transform(receipt.begin(), receipt.end(), receipt.begin(), [](unsigned char c) { return std::tolower(c); });
                    state->NotifySoon(receipt);
                } else state->OpenSoon(module, target);
                return 0;
            })),
            &state);
        auto* open_thread = g_simple_action_new("open-thread", G_VARIANT_TYPE_STRING);
        g_signal_connect(open_thread, "activate",
                         G_CALLBACK(+[](GSimpleAction*, GVariant* parameter, gpointer raw) {
                             auto* state = static_cast<App*>(raw);
                             std::string id = g_variant_get_string(parameter, nullptr);
                             std::transform(id.begin(), id.end(), id.begin(),
                                            [](unsigned char c) { return std::tolower(c); });
                             if (state->workspace && rimes::capsule::LooksLikeUuid(id))
                                 state->OpenSoon("", id);
                         }),
                         &state);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(open_thread));
        g_object_unref(open_thread);
        const int result = g_application_run(G_APPLICATION(app), argc, argv);
        if (state.open_timer) {
            g_source_remove(state.open_timer);
            state.open_timer = 0;
        }
        if (state.notify_timer) { g_source_remove(state.notify_timer); state.notify_timer = 0; }
        state.session.reset();
        state.workspace.reset();
        g_object_unref(app);
        return state.failed ? 1 : result;
    } catch (const std::exception&) {
        std::cerr
            << "Mailbox could not start; check the desktop session and private data directory.\n";
        return 1;
    }
}
