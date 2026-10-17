#include "launcher_client.hpp"
#include <gtk/gtk.h>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using Json = rimes::plugins::Json;
Json Call(const std::string& method, const std::string& action = "") {
    auto* loop = g_main_loop_new(nullptr, FALSE);
    Json response;
    rimes::desktop::LauncherCall(method, action, nullptr, [&](Json value) {
        response = std::move(value); g_main_loop_quit(loop);
    });
    g_main_loop_run(loop); g_main_loop_unref(loop);
    return response;
}
}
int main(int argc, char** argv) {
    bool dialog = false;
    std::string action;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dialog") dialog = true;
        else if (action.empty()) action = arg;
        else action = "--help";
    }
    if (action != "buffer" && action != "capsule" && action != "mailbox" && action != "settings" &&
        action != "--status" && action != "--retry") {
        std::cout << "Usage: rimes-launch [--dialog] {buffer|capsule|mailbox|settings|--status|--retry}\n";
        return action == "--help" ? 0 : 2;
    }
    auto response = Call("GetStatus");
    if (!response.value("launcher", false) && action.front() != '-') {
        // Starting Fcitx is safe when already running: it owns the session's
        // unique service. No input group or login environment is rewritten.
        gchar program[] = "fcitx5", daemon[] = "-d";
        gchar* arguments[] = {program, daemon, nullptr};
        GError* error = nullptr;
        if (g_spawn_async(nullptr, arguments, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error)) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                response = Call("GetStatus");
                if (response.value("launcher", false)) break;
            }
        }
        g_clear_error(&error);
    }
    if (response.value("launcher", false)) {
        if (action == "--retry") response = Call("Retry");
        else if (action != "--status") {
            response = Call("Open", action);
            const auto request = response.value("request", uint64_t{0});
            // The host waits for activation after an IM switch before capture.
            for (unsigned i = 0; i < 25 && response.value("status", "") == "pending"; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                auto status = Call("GetStatus");
                response = status.value("lastAction", status);
                if (response.value("request", uint64_t{0}) != request) {
                    response = {{"ok", false}, {"status", "superseded"}, {"message", "另一次启动已替代当前请求，请重试。"}};
                    break;
                }
            }
        }
    }
    const bool ok = response.value("launcher", false) ||
        (response.value("ok", false) && response.value("status", "") != "pending");
    std::cout << response.dump() << '\n';
    if (!ok && dialog && gtk_init_check(nullptr, nullptr)) {
        auto* message = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
            "%s", response.value("message", "未能打开 XSIME，请重试。").c_str());
        gtk_dialog_run(GTK_DIALOG(message)); gtk_widget_destroy(message);
    }
    return ok ? 0 : 1;
}
