#include "capsule_capture.hpp"
#include "capsule_manager.hpp"
#include "capsule_resources.hpp"
#include "capture_controller.hpp"
#include "desktop.hpp"
#include "official_catalog.hpp"
#include "private_files.hpp"
#include <chrono>
#include <iostream>
#include <thread>
using namespace rimes::capsule;
using Json = nlohmann::json;
namespace {
void Check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
GtkWidget* Find(GtkWidget* root, const char* name) {
    if (std::string(gtk_widget_get_name(root)) == name)
        return root;
    if (!GTK_IS_CONTAINER(root))
        return nullptr;
    auto* children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget* result = nullptr;
    for (auto* node = children; node && !result; node = node->next)
        result = Find(GTK_WIDGET(node->data), name);
    g_list_free(children);
    return result;
}
void Wait(const std::function<bool()>& condition) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do {
        while (gtk_events_pending())
            gtk_main_iteration();
        if (condition())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < until);
    throw std::runtime_error("capture UI timed out");
}
void Delay(int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    Wait([&] { return std::chrono::steady_clock::now() >= until; });
}
} // namespace
int main(int argc, char** argv) {
    const auto root =
        std::filesystem::temp_directory_path() / ("xsime-capture-ui-" + rimes::runtime::RandomID());
    try {
        setenv("RIMES_USER_DIR", root.c_str(), 1);
        gtk_init(&argc, &argv);
        rimes::plugins::PackageStore packages(
            root / "plugins", rimes::plugins::OfficialCatalog(), rimes::plugins::BundledPackages(),
            rimes::plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
        auto grant = packages.Inspect(ModulePlugin(Module::Capture));
        if (!grant.enabled)
            packages.Enable(grant.id, true, grant.grant);
        CaptureStore store(CaptureStore::DefaultRoot());
        ContentStore library(ContentStore::DefaultRoot());
        std::vector<Json> commands;
        CaptureController capture([&](const auto& raw) { commands.push_back(Json::parse(raw)); });
        Snapshot state;
        state.visible = true;
        state.module = "capture";
        state.modules = {Module::Capture};
        capture.Apply(state);
        capture.Open();
        auto button = [&](const char* name) {
            auto* value = Find(capture.Window(), name);
            Check(value, "missing capture control");
            return value;
        };
        auto status = [&] {
            return std::string(gtk_label_get_text(GTK_LABEL(button("capture-status"))));
        };
        gtk_button_clicked(GTK_BUTTON(button("capture-screenshot")));
        Wait([&] { return commands.size() == 1; });
        auto images = store.List();
        Check(images.size() == 1 && images[0].kind == "image", "screenshot not archived");
        const auto shot = images[0];
        Check(commands.back().at("id") == shot.id && commands.back().at("op") == "refresh",
              "capture refresh identity");
        gtk_button_clicked(GTK_BUTTON(button("capture-record")));
        Wait([&] { return status().find("正在录制") != std::string::npos; });
        Delay(1100);
        gtk_button_clicked(GTK_BUTTON(button("capture-stop")));
        Wait([&] { return commands.size() == 2; });
        auto records = store.List();
        Check(records.size() == 2 && records[0].kind == "video", "recording not archived");
        Record video;
        video.kind = Kind::Video;
        video.content = store.File(records[0]).string();
        std::atomic<bool> cancelled{false};
        Check(!PreviewResource(video, library, cancelled).png.empty(),
              "recording first frame preview failed");
        gtk_button_clicked(GTK_BUTTON(button("capture-record")));
        Wait([&] { return status().find("正在录制") != std::string::npos; });
        gtk_button_clicked(GTK_BUTTON(button("capture-cancel")));
        Wait([&] { return gtk_widget_get_sensitive(button("capture-record")); });
        Check(store.List().size() == 2 && commands.size() == 2 &&
                  status().find("未保存文件") != std::string::npos,
              "cancel published capture or retained busy status");
        {
            int copied = 0, launched = 0;
            ManagerWindow manager(
                [&](const auto& raw) {
                    const auto command = Json::parse(raw);
                    Check(command.at("id") == shot.id && command.at("revision") == shot.revision,
                          "manager capture identity changed");
                    if (command.at("op") == "copy")
                        ++copied;
                },
                false, {}, {}, [&] { ++launched; });
            state.managing = true;
            state.cards = {CaptureCard(shot)};
            state.selected_id = shot.id;
            state.selected_preview = state.cards[0].preview;
            state.count = 1;
            manager.Apply(state);
            auto get = [&](const char* name) {
                auto* value = Find(manager.Root(), name);
                Check(value, "missing capture manager control");
                return value;
            };
            Wait([&] {
                return std::string(gtk_label_get_text(GTK_LABEL(get("capsule-capture-meta"))))
                           .find("\n") != std::string::npos;
            });
            Check(gtk_widget_get_visible(get("capsule-capture-form")) &&
                      !gtk_widget_is_visible(get("capsule-body")),
                  "capture uses note form");
            gtk_button_clicked(GTK_BUTTON(get("capsule-capture-copy")));
            Check(copied == 1, "capture manager copy disconnected");
            gtk_button_clicked(GTK_BUTTON(get("capsule-new")));
            Check(launched == 1, "capture launch disconnected");
            auto favorite = store.Collect(shot.id, shot.revision);
            state.cards = {CaptureCard(favorite)};
            manager.Apply(state);
            Check(!gtk_widget_get_sensitive(get("capsule-capture-remove")) &&
                      !gtk_widget_get_sensitive(get("capsule-capture-collect")),
                  "favorite removable in history");
            state.password_field = true;
            manager.Apply(state);
            Check(!gtk_widget_get_visible(manager.Window()) &&
                      std::string(gtk_label_get_text(GTK_LABEL(get("capsule-capture-title"))))
                          .empty(),
                  "capture details survive conceal");
            gtk_button_clicked(GTK_BUTTON(get("capsule-capture-copy")));
            Check(copied == 1, "protected capture copied");
        }
        state.password_field = false;
        state.managing = false;
        capture.Apply(state);
        gtk_button_clicked(GTK_BUTTON(button("capture-record")));
        Wait([&] { return status().find("正在录制") != std::string::npos; });
        grant = packages.Inspect(grant.id);
        packages.Enable(grant.id, false, grant.grant);
        state.modules.clear();
        capture.Apply(state);
        Check(!gtk_widget_get_visible(capture.Window()), "revoked capture window stays visible");
        Delay(1000);
        Check(store.List().size() == 2 && commands.size() == 2, "revoked recording published");
        for (const auto& entry : std::filesystem::directory_iterator(ContentStore::DefaultRoot()))
            Check(entry.path().filename().string().find(".capture-") != 0,
                  "capture staging directory leaked");
        std::cout << "Capture launcher PNG, recording, first-frame preview, cancel, grant "
                     "revocation, manager actions and protected conceal passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
    std::filesystem::remove_all(root);
    return 0;
}
