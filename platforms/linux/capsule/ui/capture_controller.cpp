#include "background_jobs.hpp"
#include "capsule_capture.hpp"
#include "capture_controller.hpp"
#include "capture_process.hpp"
#include "desktop.hpp"
#include "official_catalog.hpp"
#include "private_files.hpp"
#include <algorithm>
namespace rimes::capsule {
namespace {
void Post(std::function<void()> work) {
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        +[](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(work)),
        +[](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}
} // namespace
struct CaptureController::Impl {
    std::function<void(std::string)> send;
    GtkWidget *window, *image, *video, *stop, *cancel, *status;
    Snapshot state;
    bool busy = false;
    runtime::BackgroundJobs jobs;
    std::shared_ptr<CaptureControl> control;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    Impl(std::function<void(std::string)> callback) : send(std::move(callback)) {
        using namespace rimes::desktop;
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "捕获");
        gtk_window_set_default_size(GTK_WINDOW(window), 766, 100);
        gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
        gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
        gtk_window_set_role(GTK_WINDOW(window), "rimes-capture");
        Class(window, "rimes-panel");
        auto* column = Column(8);
        gtk_container_set_border_width(GTK_CONTAINER(column), 12);
        gtk_container_add(GTK_CONTAINER(window), column);
        auto* row = Row(10);
        Add(column, row);
        Add(row, Label("捕获", "section-title"));
        image = Button("截图", [this] { Start(false); });
        video = Button("录屏", [this] { Start(true); });
        stop = Button("停止并保存", [this] {
            if (control) {
                control->stop.store(true);
                gtk_widget_set_sensitive(stop, FALSE);
            }
        });
        cancel = Button("取消", [this] { Cancel(); });
        for (auto* item : {image, video, stop, cancel})
            Add(row, item);
        status = Label("PNG 截图 · WebM 录屏（无声音，最长 10 分钟）", "caption");
        Add(column, status);
        gtk_widget_set_name(image, "capture-screenshot");
        gtk_widget_set_name(video, "capture-record");
        gtk_widget_set_name(stop, "capture-stop");
        gtk_widget_set_name(cancel, "capture-cancel");
        gtk_widget_set_name(status, "capture-status");
        g_signal_connect(window, "delete-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
                             auto* self = static_cast<Impl*>(data);
                             self->Cancel();
                             gtk_widget_hide(self->window);
                             return TRUE;
                         }),
                         this);
        g_signal_connect(window, "key-press-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer data) -> gboolean {
                             if (event->keyval != GDK_KEY_Escape)
                                 return FALSE;
                             static_cast<Impl*>(data)->Cancel();
                             return TRUE;
                         }),
                         this);
        Refresh();
    }
    ~Impl() {
        alive->store(false);
        Cancel();
        jobs.Shutdown();
        gtk_widget_destroy(window);
    }
    bool Allowed() const {
        return !state.password_field && std::find(state.modules.begin(), state.modules.end(),
                                                  Module::Capture) != state.modules.end();
    }
    void Refresh() {
        gtk_widget_set_sensitive(image, Allowed() && !busy);
        gtk_widget_set_sensitive(video, Allowed() && !busy);
        gtk_widget_set_sensitive(stop, Allowed() && busy);
        gtk_widget_set_sensitive(cancel, busy);
    }
    void Cancel() {
        if (control)
            control->Cancel();
        if (busy)
            gtk_label_set_text(GTK_LABEL(status), "已取消，正在释放捕获资源…");
    }
    void Start(bool recording) {
        if (busy || !Allowed() || !state.visible)
            return;
        busy = true;
        control = std::make_shared<CaptureControl>();
        Refresh();
        gtk_label_set_text(GTK_LABEL(status), "正在请求桌面捕获…");
        gtk_widget_hide(window);
        const auto run = control;
        const auto life = alive;
        const bool portal =
            g_getenv("WAYLAND_DISPLAY") || (g_getenv("XDG_SESSION_TYPE") &&
                                            std::string(g_getenv("XDG_SESSION_TYPE")) == "wayland");
        // Let the desktop remove the launcher before taking the first frame.
        struct Delayed {
            Impl* self;
            std::shared_ptr<CaptureControl> run;
            std::shared_ptr<std::atomic<bool>> life;
            bool video, portal;
        };
        auto* delayed = new Delayed{this, run, life, recording, portal};
        g_timeout_add_full(
            G_PRIORITY_DEFAULT, 180,
            +[](gpointer raw) -> gboolean {
                auto& d = *static_cast<Delayed*>(raw);
                if (!d.life->load())
                    return G_SOURCE_REMOVE;
                auto* self = d.self;
                const auto run = d.run;
                const auto life = d.life;
                const bool recording = d.video, portal = d.portal;
                const bool accepted =
                    self->jobs.Run(std::make_shared<std::atomic<bool>>(false), [self, run, life,
                                                                                recording, portal] {
                        std::string message;
                        std::string selected;
                        const auto work =
                            ContentStore::DefaultRoot() / (".capture-" + runtime::RandomID());
                        try {
                            runtime::PrivateDirectory folder(work);
                            plugins::PackageStore packages(
                                runtime::UserRoot() / "plugins", plugins::OfficialCatalog(),
                                plugins::BundledPackages(), plugins::CompiledAdapters(),
                                RIMES_PRODUCT_VERSION);
                            const auto grant = packages.Inspect(ModulePlugin(Module::Capture));
                            if (!grant.enabled || !life->load() || run->cancel.load())
                                throw std::runtime_error("捕获已取消或插件停用");
                            const auto file = work / (recording ? "output.webm" : "output.png");
                            const auto result = RunCapture(
                                file, recording, portal, *run,
                                [self, life, run, recording](const auto& event) {
                                    if (!recording || event.value("event", "") != "started")
                                        return;
                                    Post([self, life, run] {
                                        if (life->load() && !run->cancel.load() &&
                                            self->control == run) {
                                            gtk_label_set_text(
                                                GTK_LABEL(self->status),
                                                "正在录制 · 无声音 · 停止后保存，取消则丢弃");
                                            gtk_widget_show_all(self->window);
                                            self->Refresh();
                                        }
                                    });
                                });
                            if (result.value("event", "") != "finished")
                                throw std::runtime_error(result.value("message", "捕获已取消"));
                            CaptureRecord metadata;
                            metadata.kind = recording ? "video" : "image";
                            metadata.source = result.at("source");
                            metadata.width = result.at("width");
                            metadata.height = result.at("height");
                            metadata.duration = result.at("duration");
                            CaptureStore store(CaptureStore::DefaultRoot());
                            const auto saved =
                                store.Import(file, metadata, run->cancel, [&](const auto& publish) {
                                    bool committed = false;
                                    packages.WithEnabledGrant(grant.id, grant.grant, [&] {
                                        committed = run->Commit(publish);
                                    });
                                    return committed;
                                });
                            selected = saved.id;
                            if (selected.empty())
                                throw std::runtime_error("插件授权已变化，捕获未保存");
                            message = recording ? "录屏已保存到捕获历史" : "截图已保存到捕获历史";
                        } catch (const std::exception& e) {
                            message = e.what();
                        }
                        try {
                            runtime::PrivateDirectory folder(work);
                            folder.Remove("output.png");
                            folder.Remove("output.webm");
                            std::filesystem::remove(work);
                        } catch (const std::exception&) {
                        }
                        Post([self, run, life, message, selected] {
                            if (!life->load() || self->control != run)
                                return;
                            self->busy = false;
                            self->Refresh();
                            if (run->cancel.load() && self->Allowed())
                                gtk_label_set_text(GTK_LABEL(self->status),
                                                   "捕获已取消，未保存文件");
                            if (!run->cancel.load() && self->Allowed()) {
                                gtk_label_set_text(GTK_LABEL(self->status), message.c_str());
                                gtk_widget_show_all(self->window);
                                if (!selected.empty())
                                    self->send(nlohmann::json{
                                        {"v", 1}, {"op", "refresh"}, {"id", selected}}
                                                   .dump());
                            }
                        });
                    });
                if (!accepted) {
                    self->busy = false;
                    self->Refresh();
                    gtk_label_set_text(GTK_LABEL(self->status), "捕获任务正忙，请稍后重试");
                    gtk_widget_show_all(self->window);
                }
                return G_SOURCE_REMOVE;
            },
            delayed, +[](gpointer raw) { delete static_cast<Delayed*>(raw); });
    }
    void Apply(const Snapshot& snapshot) {
        state = snapshot;
        if (!Allowed()) {
            Cancel();
            gtk_widget_hide(window);
            gtk_label_set_text(GTK_LABEL(status), "受保护内容");
        }
        rimes::desktop::Theme(state.theme, gtk_widget_get_screen(window));
        Refresh();
    }
    void Open() {
        if (!Allowed() || !state.visible)
            return;
        gtk_widget_show_all(window);
        gtk_window_present(GTK_WINDOW(window));
        Refresh();
    }
};
CaptureController::CaptureController(std::function<void(std::string)> send)
    : impl_(std::make_unique<Impl>(std::move(send))) {}
CaptureController::~CaptureController() = default;
void CaptureController::Apply(const Snapshot& state) { impl_->Apply(state); }
void CaptureController::Open() { impl_->Open(); }
GtkWidget* CaptureController::Window() const { return impl_->window; }
} // namespace rimes::capsule
