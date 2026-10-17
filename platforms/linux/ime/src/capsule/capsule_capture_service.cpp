#include "capsule_capture.hpp"
#include "capsule_service.hpp"
#include "official_catalog.hpp"
namespace fcitx {
bool CapsuleService::CaptureAction(const std::string& action) {
    using namespace rimes::capsule;
    if (!AllowsModule() || !model_.visible() || model_.module() != Module::Capture ||
        model_.loading() || !model_.Selected())
        return false;
    const auto card = *model_.Selected();
    Publish();
    if (history_action_cancel_)
        history_action_cancel_->store(true);
    history_action_cancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = history_action_cancel_, alive = alive_;
    return storage_jobs_.Run(cancelled, [this, alive, cancelled, post = post_, card, action] {
        std::string message;
        try {
            rimes::plugins::PackageStore packages(
                rimes::runtime::UserRoot() / "plugins", rimes::plugins::OfficialCatalog(),
                rimes::plugins::BundledPackages(), rimes::plugins::CompiledAdapters(),
                RIMES_PRODUCT_VERSION);
            const auto grant = packages.Inspect(ModulePlugin(Module::Capture));
            packages.WithEnabledGrant(grant.id, grant.grant, [&] {
                if (!alive->load() || cancelled->load())
                    return;
                CaptureStore store(CaptureStore::DefaultRoot());
                if (action == "favorite")
                    store.Collect(card.id, card.revision);
                else if (!store.Remove(card.id, card.revision))
                    throw std::runtime_error("捕获已变化，请重新选择");
            });
        } catch (const std::exception& e) {
            message = e.what();
        }
        if (!alive->load() || cancelled->load())
            return;
        post([this, alive, cancelled, card, action, message] {
            if (!alive->load() || cancelled->load() || !AllowsModule() ||
                model_.module() != Module::Capture)
                return;
            capture_error_ = message;
            ReloadNotes(action == "favorite" ? card.id : "");
            Publish();
        });
    });
}
} // namespace fcitx
