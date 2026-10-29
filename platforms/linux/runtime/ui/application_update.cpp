#include "application_update.hpp"
#include "http_download.hpp"
#include "private_files.hpp"
#include "update_operation.hpp"
#include "../../../../Shared/AppUpdate/authentication.hpp"
#include "../../../../Shared/AppUpdate/signature_verifier.hpp"
#include <curl/curl.h>
#include <atomic>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <ctime>
#include <chrono>
#include <sys/utsname.h>

namespace rimes::desktop {
namespace {
using namespace rimes::runtime;
struct DownloadContext {
    std::string bytes;
    std::size_t limit;
    std::atomic<bool>* cancelled;
    std::function<void(unsigned)> progress;
};
size_t Receive(char* data, size_t size, size_t count, void* context) {
    auto& state = *static_cast<DownloadContext*>(context);
    if (*state.cancelled || (size && count > state.limit / size)) return 0;
    const auto n = size * count;
    if (state.bytes.size() + n > state.limit) return 0;
    state.bytes.append(data, n);
    if (state.progress) state.progress(static_cast<unsigned>(100 * state.bytes.size() / state.limit));
    return n;
}
int TransferProgress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic<bool>*>(context)->load() ? 1 : 0;
}
std::string Download(std::string url, std::size_t limit, std::atomic<bool>& cancelled, std::function<void(unsigned)> progress = {}) {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) throw std::runtime_error("Network initialization failed");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
    for (int redirect = 0; redirect < 6; ++redirect) {
        CheckUpdateCancelled(cancelled);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("Update download timed out");
        if (!OfficialDownloadURL(url)) throw std::runtime_error("Untrusted update URL");
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> request(curl_easy_init(), curl_easy_cleanup);
        if (!request) throw std::runtime_error("Network unavailable");
        DownloadContext context{{}, limit, &cancelled, progress};
        curl_easy_setopt(request.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(request.get(), CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(request.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(request.get(), CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(request.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(remaining));
        curl_easy_setopt(request.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(request.get(), CURLOPT_XFERINFOFUNCTION, TransferProgress);
        curl_easy_setopt(request.get(), CURLOPT_XFERINFODATA, &cancelled);
        curl_easy_setopt(request.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(request.get(), CURLOPT_WRITEFUNCTION, Receive);
        curl_easy_setopt(request.get(), CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(request.get(), CURLOPT_USERAGENT, "XSIME-updater/1");
        if (curl_easy_perform(request.get()) != CURLE_OK) throw std::runtime_error("Download failed or cancelled; retry when connected");
        long status = 0; curl_easy_getinfo(request.get(), CURLINFO_RESPONSE_CODE, &status);
        CheckUpdateCancelled(cancelled);
        if (status == 200) return context.bytes;
        if (status >= 300 && status < 400) {
            char* location = nullptr; curl_easy_getinfo(request.get(), CURLINFO_REDIRECT_URL, &location);
            if (!location) throw std::runtime_error("Invalid update redirect");
            url = location; continue;
        }
        throw std::runtime_error("Update not yet available (HTTP " + std::to_string(status) + ")");
    }
    throw std::runtime_error("Too many update redirects");
}
bool SupportedSystem() {
    utsname info{}; if (uname(&info) != 0 || std::string(info.machine) != "x86_64") return false;
    std::ifstream stream("/etc/os-release"); std::string line, id, version;
    while (std::getline(stream, line)) {
        if (line == "ID=ubuntu" || line == "ID=\"ubuntu\"") id = "ubuntu";
        if (line == "VERSION_ID=\"24.04\"" || line == "VERSION_ID=24.04") version = "24.04";
        if (line == "VERSION_ID=\"26.04\"" || line == "VERSION_ID=26.04") version = "26.04";
    }
    return id == "ubuntu" && (version == "24.04" || version == "26.04");
}
std::string Run(std::vector<std::string> arguments) {
    std::vector<char*> argv; for (auto& arg : arguments) argv.push_back(arg.data()); argv.push_back(nullptr);
    gchar *output = nullptr, *errors = nullptr; gint status = 0; GError* error = nullptr;
    const auto started = g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, &output, &errors, &status, &error);
    std::string result = output ? output : "", detail = errors ? errors : "";
    g_free(output); g_free(errors);
    if (!started || !g_spawn_check_wait_status(status, nullptr)) {
        if (error) { detail = error->message; g_error_free(error); }
        if (detail.size() > 700) detail.resize(700);
        throw std::runtime_error("Installation command failed or was cancelled. " + detail);
    }
    return result;
}
updates::Asset Authenticate(const std::string& bytes) {
    return updates::ParseAuthenticated(bytes, "linux", static_cast<std::uint64_t>(std::time(nullptr)), updates::VerifyP256);
}
void CheckAndRecordVersion(const PrivateDirectory& directory, const updates::Asset& asset) {
    const auto saved = directory.Read("trusted-version.json", 4096);
    const auto highest = saved ? nlohmann::json::parse(*saved).at("version").get<std::string>() : "";
    updates::CheckVersionFloor(asset, RIMES_PRODUCT_VERSION, highest);
    if (highest.empty() || updates::Version(asset.version) > updates::Version(highest))
        directory.Write("trusted-version.json", nlohmann::json{{"version", asset.version}}.dump());
}
struct State {
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    bool busy = false, available = false, downloaded = false, installation_started = false;
    std::string manifest;
    std::string message = std::string("当前版本 / Current: ") + RIMES_PRODUCT_VERSION;
    updates::Asset asset;
};
struct View { GtkWidget *window, *status, *check, *download, *install; guint timer = 0; std::shared_ptr<State> state = std::make_shared<State>(); };
void Start(const std::shared_ptr<State>& state, unsigned action) {
    {
        std::lock_guard lock(state->mutex);
        if (state->busy || (action != 1 && !state->available) || (action == 3 && !state->downloaded)) return;
        state->busy = true; state->message = action == 1 ? "正在检查 / Checking…" : action == 2 ? "正在下载 / Downloading…" : "请完成系统安装授权 / Authorize the installation…";
    }
    std::thread([state, action] {
        try {
            if (!SupportedSystem()) throw std::runtime_error("自动更新目前支持 Ubuntu 24.04 / 26.04 amd64。其他发行版请使用相应的软件包或源码安装。 / This package targets Ubuntu 24.04 / 26.04 amd64.");
            PrivateDirectory directory(UserRoot() / "application-updates");
            auto lock = WaitForUpdateStorage(directory, state->cancelled);
            if (action == 1) {
                const auto bytes = Download(updates::kFeed, 1024 * 1024, state->cancelled);
                const auto asset = Authenticate(bytes);
                CheckUpdateCancelled(state->cancelled);
                CheckAndRecordVersion(directory, asset);
                directory.Write("candidate.json", bytes);
                std::lock_guard guard(state->mutex);
                state->asset = asset; state->manifest = bytes; state->downloaded = false;
                state->available = updates::Version(asset.version) > updates::Version(RIMES_PRODUCT_VERSION);
                state->message = state->available ? "发现新版本 / Available: " + asset.version : "已是最新版本 / Up to date";
            } else if (action == 2) {
                std::string manifest;
                { std::lock_guard guard(state->mutex); manifest = state->manifest; }
                const auto asset = Authenticate(manifest);
                CheckAndRecordVersion(directory, asset);
                const auto name = updates::PackageName(asset, "linux");
                const auto bytes = Download(asset.url, asset.size, state->cancelled, [state](unsigned percent) {
                    std::lock_guard guard(state->mutex); state->message = "正在下载 / Downloading… " + std::to_string(percent) + "%";
                });
                updates::Verify(asset, bytes.size(), Sha256(bytes));
                CheckUpdateCancelled(state->cancelled);
                directory.Write(name, bytes);
                std::lock_guard guard(state->mutex);
                state->downloaded = true; state->message = "下载及验证完成，点击安装 / Download verified. Ready to install.";
            } else {
                std::string manifest;
                { std::lock_guard guard(state->mutex); manifest = state->manifest; }
                const auto asset = Authenticate(manifest);
                CheckAndRecordVersion(directory, asset);
                const auto name = updates::PackageName(asset, "linux");
                const auto package = directory.Path() / name;
                const auto bytes = directory.Read(name, updates::kMaximumPackage);
                if (!bytes) throw std::runtime_error("Download the package again");
                updates::Verify(asset, bytes->size(), Sha256(*bytes));
                if (Run({"/usr/bin/dpkg-deb", "--field", package.string(), "Package"}) != "fcitx5-rimes\n" ||
                    Run({"/usr/bin/dpkg-deb", "--field", package.string(), "Version"}) != asset.version + "\n" ||
                    Run({"/usr/bin/dpkg-deb", "--field", package.string(), "Architecture"}) != "amd64\n") throw std::runtime_error("Debian package identity mismatch");
                directory.Write("pending.json", nlohmann::json{{"version", asset.version}}.dump());
                // Only the fixed system package manager runs elevated. It owns dependency
                // resolution and dpkg's transaction; no root updater or shell is installed.
                {
                    std::lock_guard guard(state->mutex);
                    CheckUpdateCancelled(state->cancelled);
                    (void)Authenticate(manifest);
                    state->installation_started = true;
                    state->message = "系统安装已开始，关闭窗口不会中断软件包事务。 / System installation started; closing this window will not interrupt it.";
                }
                Run({"/usr/bin/pkexec", "/usr/bin/apt-get", "--yes", "install", package.string()});
                if (Run({"/usr/bin/dpkg-query", "-W", "-f=${Status} ${Version}", "fcitx5-rimes"}) != "install ok installed " + asset.version)
                    throw std::runtime_error("Package manager has not confirmed the new version");
                std::lock_guard guard(state->mutex);
                state->available = false; state->downloaded = false;
                state->message = "安装完成。请保存工作后注销并重新登录，以加载新版输入法。 / Installed. Sign out and back in to activate the update.";
            }
            std::lock_guard guard(state->mutex); state->busy = false; state->installation_started = false;
        } catch (const std::exception& error) { std::lock_guard guard(state->mutex); state->busy = false; state->installation_started = false; state->message = std::string("更新未完成，可重试 / Update incomplete: ") + error.what(); }
    }).detach();
}
gboolean Refresh(gpointer data) {
    auto* view = static_cast<View*>(data); std::lock_guard guard(view->state->mutex);
    gtk_label_set_text(GTK_LABEL(view->status), view->state->message.c_str());
    gtk_widget_set_sensitive(view->check, !view->state->busy);
    gtk_widget_set_sensitive(view->download, !view->state->busy && view->state->available);
    gtk_widget_set_sensitive(view->install, !view->state->busy && view->state->downloaded);
    gtk_button_set_label(GTK_BUTTON(gtk_dialog_get_widget_for_response(GTK_DIALOG(view->window), GTK_RESPONSE_CLOSE)),
                         view->state->installation_started ? "关闭（安装继续） / Close (installation continues)" : "关闭 / Close");
    return G_SOURCE_CONTINUE;
}
} // namespace
void ShowApplicationUpdate(GtkWindow* parent) {
    auto* view = new View{};
    view->window = gtk_dialog_new_with_buttons("XSIME 应用更新 / Software update", parent, GTK_DIALOG_DESTROY_WITH_PARENT, "关闭 / Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(view->window), 600, 240);
    auto* content = gtk_dialog_get_content_area(GTK_DIALOG(view->window));
    gtk_container_set_border_width(GTK_CONTAINER(content), 20);
    view->status = gtk_label_new(""); gtk_label_set_line_wrap(GTK_LABEL(view->status), true);
    gtk_label_set_max_width_chars(GTK_LABEL(view->status), 70); gtk_widget_set_vexpand(view->status, true);
    gtk_container_add(GTK_CONTAINER(content), view->status);
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_container_add(GTK_CONTAINER(content), actions);
    auto button = [&](const char* label, unsigned action) {
        auto* widget = gtk_button_new_with_label(label); gtk_container_add(GTK_CONTAINER(actions), widget);
        g_object_set_data(G_OBJECT(widget), "update-action", GUINT_TO_POINTER(action));
        g_signal_connect(widget, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
            auto* v = static_cast<View*>(data);
            const auto id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(clicked), "update-action"));
            Start(v->state, id); Refresh(v);
        }), view);
        return widget;
    };
    view->check = button("检查更新 / Check", 1); view->download = button("下载 / Download", 2); view->install = button("安装 / Install", 3);
    try {
        PrivateDirectory directory(UserRoot() / "application-updates");
        if (const auto pending = directory.Read("pending.json", 4096)) {
            const auto target = nlohmann::json::parse(*pending).at("version").get<std::string>();
            if (updates::Version(RIMES_PRODUCT_VERSION) >= updates::Version(target)) {
                view->state->message = std::string("更新完成 / Updated to ") + RIMES_PRODUCT_VERSION + "\n请确认已注销并重新登录，以加载新版输入法。 / Start a new login session to load the updated input method.";
                directory.Remove("pending.json");
            } else view->state->message += "\n等待安装或重新登录，可重试 / Waiting for installation or a new session.";
        }
        if (const auto saved = directory.Read("candidate.json", 1024 * 1024)) {
            const auto asset = Authenticate(*saved);
            if (updates::Version(asset.version) > updates::Version(RIMES_PRODUCT_VERSION)) {
                view->state->asset = asset; view->state->manifest = *saved; view->state->available = true;
                view->state->downloaded = std::filesystem::is_regular_file(directory.Path() / updates::PackageName(asset, "linux"));
            }
        }
    } catch (...) { view->state->message += "\n请重新检查更新 / Check for updates again."; }
    g_signal_connect(view->window, "response", G_CALLBACK(+[](GtkDialog* dialog, gint, gpointer) { gtk_widget_destroy(GTK_WIDGET(dialog)); }), nullptr);
    g_signal_connect(view->window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
        auto* v = static_cast<View*>(data);
        { std::lock_guard guard(v->state->mutex);
          if (!v->state->installation_started) v->state->cancelled = true; }
        if (v->timer) g_source_remove(v->timer);
        delete v;
    }), view);
    view->timer = g_timeout_add(200, Refresh, view); Refresh(view); gtk_widget_show_all(view->window);
}
} // namespace rimes::desktop
