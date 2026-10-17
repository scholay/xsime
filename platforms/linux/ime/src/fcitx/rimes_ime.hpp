#pragma once

#include <atomic>
#include <memory>

#include <fcitx-config/configuration.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/handlertable.h>
#include <fcitx/addonfactory.h>
#include <fcitx/event.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

#include "buffer/buffer_service.hpp"
#include "capsule/capsule_service.hpp"
#include "engine/rime_engine.hpp"
#include "plugin_registry.hpp"
#include "package_binding.hpp"
#include "configuration.hpp"
#include "local_translation.hpp"
#include "ai_api.hpp"
#include "session/session_monitor.hpp"
#include "desktop/workspace_launcher.hpp"
#include "desktop/desktop_shortcuts.hpp"

namespace fcitx {

FCITX_CONFIGURATION(RimesConfiguration,
    Option<bool> verticalCandidates{this, "VerticalCandidates", "竖向排列候选", true};
    Option<bool> capsuleNotes{this, "CapsuleNotes", "启用 Capsule 笔记", true};
    ExternalOption settings{this, "Settings", "XSIME 设置", "rimes-settings"};
    ExternalOption schemes{this, "Schemes", "导入与管理输入方案", "rimes-schemes"};)
class RimesState;

class RimesIme final : public InputMethodEngineV2 {
public:
    explicit RimesIme(Instance* instance);
    ~RimesIme() override;

    void keyEvent(const InputMethodEntry& entry, KeyEvent& keyEvent) override;
    void activate(const InputMethodEntry& entry, InputContextEvent& event) override;
    void deactivate(const InputMethodEntry& entry, InputContextEvent& event) override;
    void reset(const InputMethodEntry& entry, InputContextEvent& event) override;
    std::string subMode(const InputMethodEntry& entry, InputContext& inputContext) override;

    void reloadConfig() override;
    void setConfig(const RawConfig& config) override;
    bool verticalCandidates() const { return *config_.verticalCandidates; }
    const Configuration* getConfig() const override { return &config_; }
    const Configuration* getConfigForInputMethod(const InputMethodEntry&) const override { return &config_; }
    const std::string& initialSchema() const { return imported_schema_; }
    Instance* instance() { return instance_; }
    rimes::linuxime::RimeEngine& engine() { return engine_; }
    FactoryFor<RimesState>& factory() { return factory_; }
    BufferService& buffer() { return *buffer_; }
    CapsuleService& capsule() { return *capsule_; }
    rimes::plugins::Registry& plugins() { return plugins_; }
    bool acceptsPluginResult(const rimes::plugins::Lease& lease);

    // Single commit path. Buffer intercepts here the way macOS intercepts
    // before Delivery.insert (see rime_hooks.hpp).
    void commitText(InputContext* ic, std::string_view text);

    void applySnapshot(InputContext* ic, const rimes::linuxime::EngineSnapshot& snapshot);

private:
    void HandleDeployReady();
    void OnDeployReady();
    void WatchPluginPackages();
    void WatchConfiguration();
    void ApplyConfiguration();

    RimesConfiguration config_;
    std::string imported_schema_;
    Instance* instance_;
    rimes::linuxime::RimeEngine engine_;
    FactoryFor<RimesState> factory_;
    EventDispatcher dispatcher_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::unique_ptr<BufferService> buffer_;
    std::unique_ptr<CapsuleService> capsule_;
    std::unique_ptr<HandlerTableEntry<EventHandler>> destroy_watch_;
    std::unique_ptr<HandlerTableEntry<EventHandler>> capability_watch_;
    std::unique_ptr<HandlerTableEntry<EventHandler>> focus_watch_;
    std::unique_ptr<SessionMonitor> session_monitor_;
    std::unique_ptr<WorkspaceLauncher> launcher_;
    std::unique_ptr<DesktopShortcuts> desktop_shortcuts_;
    bool deploy_announced_ = false;
    rimes::plugins::Registry plugins_;
    std::unique_ptr<rimes::plugins::PackageStore> plugin_store_;
    std::unique_ptr<rimes::plugins::PackageBinding> plugin_binding_;
    std::unique_ptr<EventSourceIO> plugin_watch_;
    int plugin_watch_fd_ = -1;
    std::unique_ptr<rimes::runtime::ConfigurationStore> configuration_;
    std::unique_ptr<EventSourceIO> configuration_watch_;
    int configuration_watch_fd_ = -1;
    std::string configuration_revision_;
    rimes::runtime::LocalTranslationWorker translation_worker_;
    rimes::runtime::AIWorker ai_worker_;
};

class RimesImeFactory : public AddonFactory {
public:
    AddonInstance* create(AddonManager* manager) override;
};

}  // namespace fcitx
