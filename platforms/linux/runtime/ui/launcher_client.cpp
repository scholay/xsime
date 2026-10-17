#include "launcher_client.hpp"
#include "launcher_protocol.hpp"
#include <cstring>
#include <memory>

namespace rimes::desktop {
namespace {
using Json = plugins::Json;
Json Unavailable() {
    return {{"ok", false}, {"status", "unavailable"},
            {"message", "无法连接 XSIME。请启动 Fcitx5，并在输入法配置中添加 XSIME 后重试。"}};
}
struct Request {
    std::string method, action;
    GCancellable* cancel;
    std::function<void(Json)> done;
    ~Request() { if (cancel) g_object_unref(cancel); }
};
struct SharedConnection {
    GDBusConnection* bus = nullptr;
    ~SharedConnection() { if (bus) g_object_unref(bus); }
};
}
void LauncherCall(const std::string& method, const std::string& action, GCancellable* cancel,
                  std::function<void(Json)> completion) {
    if (method != "Open" && method != "GetStatus" && method != "Retry" && method != "BeginRecording" && method != "EndRecording") {
        completion(Unavailable()); return;
    }
    auto* request = new Request{method, action, cancel ? G_CANCELLABLE(g_object_ref(cancel)) : nullptr, std::move(completion)};
    g_bus_get(G_BUS_TYPE_SESSION, cancel, +[](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Request> request(static_cast<Request*>(data));
        GError* error = nullptr;
        auto* bus = g_bus_get_finish(result, &error);
        g_clear_error(&error);
        if (!bus) { request->done(Unavailable()); return; }
        // Keep the same unique bus name while recording, including between
        // calls. A process exit still releases the recorder automatically.
        static SharedConnection connection;
        if (!connection.bus) connection.bus = G_DBUS_CONNECTION(g_object_ref(bus));
        const bool argument = request->method == "Open" || request->method == "BeginRecording" || request->method == "EndRecording";
        auto* parameters = argument ? g_variant_new("(s)", request->action.c_str()) : nullptr;
        auto* pending = request.release();
        using namespace runtime;
        g_dbus_connection_call(bus, kLauncherName, kLauncherPath, kLauncherInterface, pending->method.c_str(),
            parameters, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1500, pending->cancel,
            +[](GObject* object, GAsyncResult* result, gpointer data) {
                std::unique_ptr<Request> request(static_cast<Request*>(data));
                GError* error = nullptr;
                auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
                g_clear_error(&error);
                Json value = Unavailable();
                if (reply) {
                    const char* text = nullptr; g_variant_get(reply, "(&s)", &text);
                    if (text && std::strlen(text) < 65536) {
                        auto parsed = Json::parse(text, nullptr, false);
                        if (parsed.is_object()) value = std::move(parsed);
                    }
                    g_variant_unref(reply);
                }
                request->done(std::move(value));
            }, pending);
        g_object_unref(bus);
    }, request);
}
}
