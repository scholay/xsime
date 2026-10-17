#include "background_jobs.hpp"
#include "clipboard_controller.hpp"
#include "clipboard_desktop.hpp"
#include "capsule_resources.hpp"
#include "capsule_capture.hpp"
#include "configuration.hpp"
#include "official_catalog.hpp"
#include <algorithm>
#include <gtk/gtk.h>

namespace rimes::capsule {
namespace {
void Post(std::function<void()> work) {
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        +[](gpointer raw) -> gboolean {
            (*static_cast<std::function<void()>*>(raw))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(work)),
        +[](gpointer raw) { delete static_cast<std::function<void()>*>(raw); });
}
} // namespace
struct ClipboardController::Impl {
    Send send;
    Status status;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<std::uint64_t> scope{0};
    runtime::BackgroundJobs jobs;
    std::unique_ptr<DesktopClipboard> desktop;
    std::unique_ptr<plugins::PackageStore> packages;
    Snapshot state;
    bool seen_restore = false;
    std::uint64_t restore_seq = 0;
    std::string action_scope;
    explicit Impl(Send callback, Status notice)
        : send(std::move(callback)), status(std::move(notice)) {
        packages = std::make_unique<plugins::PackageStore>(
            runtime::UserRoot() / "plugins", plugins::OfficialCatalog(), plugins::BundledPackages(),
            plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
        desktop = std::make_unique<DesktopClipboard>(
            [this, life = alive](ClipboardArchive archive, ClipboardSource source,
                                 std::uint64_t epoch) {
                if (!life->load())
                    return;
                // X11 application metadata is optional and untrusted. A malformed
                // WM_CLASS must not lose an otherwise valid clipboard item.
                if (source.application.size() > 512 ||
                    !g_utf8_validate(source.application.data(), source.application.size(), nullptr))
                    source.application.clear();
                auto cancelled = std::make_shared<std::atomic<bool>>(false);
                if (!jobs.Run(cancelled, [this, life, epoch, archive = std::move(archive),
                                          source = std::move(source)] {
                        if (!life->load() || !desktop->Current(epoch))
                            return;
                        try {
                            runtime::ConfigurationStore config(runtime::UserRoot() /
                                                               "configuration");
                            auto current = [&] {
                                return life->load() && desktop->Current(epoch) &&
                                       config.Plugin(ModulePlugin(Module::Temporary))
                                           .at("captureEnabled")
                                           .get<bool>();
                            };
                            if (!current())
                                return;
                            const auto grant = packages->Inspect(ModulePlugin(Module::Temporary));
                            packages->WithEnabledGrant(grant.id, grant.grant, [&] {
                                ClipboardHistoryStore store(ClipboardHistoryStore::DefaultRoot());
                                store.Capture(archive, source, current);
                            });
                            Post([this, life, epoch] {
                                if (life->load() && desktop->Current(epoch))
                                    send(plugins::Json{
                                        {"v", 1}, {"op", "refresh"}, {"text", "clipboard"}}
                                             .dump());
                            });
                        } catch (const std::exception&) {
                            Notice("此条剪贴板内容未收录；请检查容量、格式或数据目录权限", life,
                                   epoch);
                        }
                    }))
                    Notice("剪贴板正忙，此条内容未收录", life, epoch);
            },
            [this, life = alive](std::string text) {
                Post([this, life, text = std::move(text)] {
                    if (life->load() && state.module == "temporary" && !state.password_field)
                        status(text);
                });
            });
    }
    ~Impl() {
        alive->store(false);
        scope.fetch_add(1);
        desktop->Configure(false, true);
        jobs.Shutdown();
        desktop.reset();
    }
    void Notice(std::string text, const std::shared_ptr<std::atomic<bool>>& life,
                std::uint64_t epoch) {
        Post([this, life, epoch, text = std::move(text)] {
            if (life->load() && desktop->Current(epoch) && state.module == "temporary")
                status(text);
        });
    }
    ClipboardArchive FileArchive(const Record& record) {
        const auto path = ResolveResource(record, ContentStore(ContentStore::DefaultRoot()));
        const auto uri = ResourceUri(path);
        return {{{"text/uri-list", uri + "\r\n"}, {"x-special/gnome-copied-files", "copy\n" + uri},
                  {"application/x-kde-cutselection", "0"}, {"text/plain;charset=utf-8", path.string()}, {"UTF8_STRING", path.string()}}};
    }
    void CopyResource(const Record& record) {
        if (!state.visible || state.password_field || state.module != "resources") return;
        const auto epoch = scope.load(); const auto life = alive;
        if (!jobs.Run(std::make_shared<std::atomic<bool>>(false), [this, life, epoch, record] {
            auto current = [&] {return life->load() && scope.load() == epoch;};
            bool queued = false;
            try {
                const auto grant = packages->Inspect(ModulePlugin(Module::Resources));
                if (!current()) return;
                if (!grant.enabled) throw std::runtime_error("resource grant revoked");
                auto archive = FileArchive(record);
                packages->WithEnabledGrant(grant.id, grant.grant, [&] {
                    if (current()) queued = desktop->Restore(std::move(archive), false, [this, life, epoch](bool written) {
                        Post([this, life, epoch, written] {
                            if (life->load() && scope.load() == epoch) status(written?"已复制文件，可在目标应用中粘贴":"复制文件失败，请重试");
                        });
                    });
                });
            } catch (const std::exception&) {}
            if (!queued) Post([this, life, epoch] {if (life->load() && scope.load() == epoch) status("文件不可用、授权已变化，或桌面未开放剪贴板接口");});
        })) status("剪贴板正忙，请稍后重试复制");
    }
    void Restore(const Snapshot& snapshot) {
        const auto epoch = scope.load(), sequence = snapshot.restore_seq;
        const auto id = snapshot.restore_id, revision = snapshot.restore_revision;
        const bool close = snapshot.restore_close;
        const auto module = ModuleFromRaw(snapshot.module).value_or(Module::Temporary);
        auto life = alive;
        if (!jobs.Run(std::make_shared<std::atomic<bool>>(false), [this, life, epoch, sequence, id,
                                                                   revision, close, module] {
                auto current = [&] { return life->load() && scope.load() == epoch; };
                bool queued = false;
                try {
                    if (!current())
                        return;
                    const auto grant = packages->Inspect(ModulePlugin(module));
                    packages->WithEnabledGrant(grant.id, grant.grant, [&] {
                        if (!current())
                            return;
                        std::optional<ClipboardArchive> archive;
                        if (module == Module::Temporary) {
                            ClipboardHistoryStore store(ClipboardHistoryStore::DefaultRoot()); archive = store.Read(id, revision);
                        } else if (module == Module::Capture) {
                            CaptureStore store(CaptureStore::DefaultRoot());
                            const auto captured=store.Acquire(id,revision); Record value;
                            if(captured) {
                                value.kind=captured->record.kind=="video"?Kind::Video:Kind::Image; value.content=captured->path.string();
                                archive=FileArchive(value);archive->resource_lease=captured->lease;
                                if(value.kind==Kind::Image && captured->record.bytes<=32*1024*1024) {
                                    std::atomic<bool> cancelled{false};
                                    archive->representations.push_back({"image/png",ReadResource(value.content,32*1024*1024,cancelled)});
                                }
                            } else {
                                ContentStore library(ContentStore::DefaultRoot());std::string error;
                                if(library.Get(id,&value,&error)==StoreStatus::Ok && value.revision==revision && RecordModule(value)==Module::Capture) archive=FileArchive(value);
                            }
                        } else if (module == Module::Resources) {
                            ContentStore store(ContentStore::DefaultRoot()); Record value; std::string error;
                            if (store.Get(id, &value, &error) == StoreStatus::Ok && value.revision == revision && RecordModule(value) == Module::Resources) archive = FileArchive(value);
                        }
                        if (!archive || !current())
                            return;
                        queued = desktop->Restore(
                            std::move(*archive), false,
                            [this, life, epoch, sequence, close](bool written) {
                                Post([this, life, epoch, sequence, close, written] {
                                    if (!life->load() || scope.load() != epoch ||
                                        restore_seq != sequence)
                                        return;
                                    status(written ? "已复制，可在目标应用中粘贴"
                                                   : "剪贴板恢复失败，请重试");
                                    if (written && close)
                                        send(plugins::Json{{"v", 1}, {"op", "close"}}.dump());
                                });
                            });
                    });
                } catch (const std::exception&) {
                }
                if (!queued)
                    Post([this, life, epoch, sequence] {
                        if (life->load() && scope.load() == epoch && restore_seq == sequence)
                            status("未恢复剪贴板：条目已变化，或桌面未开放剪贴板接口");
                    });
            }))
            status("剪贴板正忙，请稍后重试复制");
    }
    void Apply(const Snapshot& snapshot) {
        const bool enabled = std::find(snapshot.modules.begin(), snapshot.modules.end(),
                                       Module::Temporary) != snapshot.modules.end();
        const bool action_enabled = std::any_of(snapshot.modules.begin(), snapshot.modules.end(), [&](Module module) {return ModuleRaw(module) == snapshot.module;});
        const auto next = std::string(snapshot.visible ? "visible" : "hidden") + "/" +
                          (snapshot.password_field ? "protected" : "open") + "/" +
                          (action_enabled ? "enabled" : "disabled") + "/" + snapshot.module + "/" +
                          snapshot.selected_id + "/" + std::to_string(snapshot.restore_seq);
        if (next != action_scope) {
            scope.fetch_add(1);
            action_scope = next;
            desktop->CancelWrites();
        }
        state = snapshot;
        desktop->Configure(snapshot.clipboard_capture && enabled, snapshot.password_field);
        if (snapshot.password_field) {
            seen_restore = false;
            return;
        }
        const bool edge = seen_restore && restore_seq != snapshot.restore_seq;
        seen_restore = true;
        restore_seq = snapshot.restore_seq;
        if (edge && snapshot.visible && !snapshot.password_field && action_enabled &&
            (snapshot.module == "temporary" || snapshot.module == "resources" || snapshot.module == "capture") && !snapshot.restore_id.empty())
            Restore(snapshot);
    }
    void CopyText(const std::string& text) {
        if (!state.visible || state.password_field || text.empty())
            return;
        if (desktop->Available()) {
            auto life = alive;
            const auto epoch = scope.load();
            if (desktop->Restore(
                    ClipboardArchive{{{"text/plain;charset=utf-8", text}, {"UTF8_STRING", text}}},
                    false, [this, life, epoch](bool success) {
                        if (!success)
                            Post([this, life, epoch] {
                                if (life->load() && scope.load() == epoch)
                                    status("复制失败，请重试");
                            });
                    }))
                return;
        }
        // Focused GTK selection is the explicit-copy fallback on compositors
        // without data-control. It never enables a global history observer.
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text.data(),
                               text.size());
        status("已交给当前窗口剪贴板；若切换应用后无法粘贴，请先聚焦 Capsule 再复制");
    }
};
ClipboardController::ClipboardController(Send send, Status status)
    : impl_(std::make_unique<Impl>(std::move(send), std::move(status))) {}
ClipboardController::~ClipboardController() = default;
void ClipboardController::Apply(const Snapshot& state) { impl_->Apply(state); }
void ClipboardController::CopyText(const std::string& text) { impl_->CopyText(text); }
void ClipboardController::CopyResource(const Record& record) { impl_->CopyResource(record); }
} // namespace rimes::capsule
