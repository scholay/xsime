#include "session/protected_input.hpp"
#include "rimes_ime.hpp"
#include <glib.h>

#include <filesystem>
#include <functional>
#include <array>
#include <cerrno>
#include <sys/inotify.h>
#include <unistd.h>

#include <fcitx-utils/log.h>
#include <fcitx-config/iniparser.h>
#include <fcitx/event.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/userinterface.h>

#include "engine/rime_hooks.hpp"
#include "engine/rime_paths.hpp"
#include "rimes_state.hpp"
#include "official_catalog.hpp"

#ifndef RIMES_INSTALL_SHARED_DIR
#define RIMES_INSTALL_SHARED_DIR ""
#endif

namespace fcitx {
namespace {

FCITX_DEFINE_LOG_CATEGORY(rimes_log, "rimes");

}  // namespace

RimesIme::RimesIme(Instance* instance)
    : instance_(instance),
      factory_([this](InputContext& ic) { return new RimesState(this, &ic); }),
      alive_(std::make_shared<std::atomic<bool>>(true)) {
    reloadConfig();
    instance_->inputContextManager().registerProperty("rimesState", &factory_);
    dispatcher_.attach(&instance_->eventLoop());

    // fcitx5 time events are one-shot (sd_event / libuv). A setTime() poll
    // without setOneShot() fires once at +200 ms and never again — that left
    // status stuck on "Deploying". Notify from the maintenance thread instead.
    engine_.SetDeployReadyCallback([this, alive = alive_]() {
        dispatcher_.schedule([this, alive]() {
            if (alive && alive->load()) {
                HandleDeployReady();
            }
        });
    });

    auto paths = rimes::linuxime::ResolveEnginePaths(
        std::filesystem::path(RIMES_INSTALL_SHARED_DIR));
    rimes::linuxime::RimeEngineOptions options;
    options.shared_data_dir = paths.shared_data_dir;
    options.user_data_dir = paths.user_data_dir;
    options.log_dir = paths.log_dir;
    options.wait_for_maintenance = false;
    imported_schema_ = paths.imported_schema;

    std::string error;
    if (!engine_.Start(options, &error)) {
        FCITX_LOGC(rimes_log, Error) << "librime start failed: " << error;
    } else if (engine_.IsDeploying()) {
        FCITX_LOGC(rimes_log, Info)
            << "librime background deploy started; keys pass through until ready";
    }

    auto post = [this, alive = alive_](std::function<void()> work) {
        if (!alive->load()) return;
        dispatcher_.schedule([this, alive, work = std::move(work)]() {
            if (alive && alive->load()) {
                work();
            }
        });
    };
    buffer_ = std::make_unique<BufferService>(instance_, post);
    capsule_ = std::make_unique<CapsuleService>(instance_, post);
    launcher_ = std::make_unique<WorkspaceLauncher>(*instance_, *buffer_, *capsule_, [] {
        const char* program = access(RIMES_INSTALLED_SETTINGS, X_OK) == 0 ? RIMES_INSTALLED_SETTINGS : RIMES_BUILD_SETTINGS;
        gchar* arguments[] = {const_cast<gchar*>(program), nullptr};
        GError* error = nullptr;
        const bool launched = g_spawn_async(nullptr, arguments, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &error);
        g_clear_error(&error); return launched;
    });
    buffer_->SetManagedHotkey(); capsule_->SetManagedHotkey();
    desktop_shortcuts_ = std::make_unique<DesktopShortcuts>(instance_->eventLoop(),
        [this](const std::string& action) { return launcher_->Open(action); },
        [this] { return launcher_->LastResult(); });
    launcher_->SetDesktopStatus([this] { return desktop_shortcuts_ ? desktop_shortcuts_->Status() : rimes::runtime::Json::object(); });
    // Register only adapters which exist in this host. Catalog entries for
    // future Linux ports never become executable just by being downloaded.
    for (auto module : rimes::capsule::kModules) {
        const auto id = rimes::capsule::ModulePlugin(module);
        plugins_.Register(rimes::plugins::CompiledAdapters().at(id),
            {[this, module] { capsule_->SetModuleEnabled(module, true); },
             [this, module] { capsule_->SetModuleEnabled(module, false); }});
        capsule_->SetModuleEnabled(module, false);
    }
    plugins_.Register(rimes::plugins::CompiledAdapters().at("builtin.apple-translation"),
        {[] {}, [this] { translation_worker_.CancelAll(); }});
    plugins_.Register(rimes::plugins::CompiledAdapters().at("builtin.openai-compatible"),
        {[] {}, [this] { ai_worker_.CancelAll(); }});
    WatchPluginPackages();
    WatchConfiguration();
    capsule_->BindModuleValidation([this](auto module) {
        const auto lease = plugins_.Authorize(rimes::capsule::ModulePlugin(module), "capsule.navigate");
        return lease && acceptsPluginResult(*lease);
    });
    buffer_->BindAutoSend([this] {
        if (!configuration_) throw std::runtime_error("Configuration unavailable");
        auto config = configuration_->Load();
        const bool enabled = config.at("autoSend");
        const int current = config.at("autoSendSeconds");
        config["autoSend"] = !enabled || current != 5;
        config["autoSendSeconds"] = !enabled ? 1 : current == 1 ? 2 : current == 2 ? 3 : current == 3 ? 5 : 1;
        configuration_->Save(config, config.at("revision"));
        ApplyConfiguration();
    });
    buffer_->BindCloseAfterLast([this] {
        if (!configuration_) throw std::runtime_error("Configuration unavailable");
        auto config = configuration_->Load();
        config["closeAfterLast"] = !config.at("closeAfterLast").get<bool>();
        configuration_->Save(config, config.at("revision"));
        ApplyConfiguration();
    });
    buffer_->BindPlugins(plugins_, [this](const auto& lease) { return acceptsPluginResult(lease); },
        [this](std::string_view id) {
            if (!configuration_) throw std::runtime_error("Configuration unavailable");
            return configuration_->Freeze(id);
        });
    buffer_->RegisterDerivedAdapter("builtin.apple-translation", "translation.generate",
        [this](auto request, auto configuration, auto cancelled, auto receive) {
            translation_worker_.Run(std::move(request.text), configuration.plugin.at("sourceLanguage").template get<std::string>(),
                configuration.plugin.at("targetLanguage").template get<std::string>(), std::move(cancelled),
                [receive = std::move(receive)](std::string text, bool success) { receive({0, std::move(text), true, success}); });
        });
    buffer_->RegisterDerivedAdapter("builtin.openai-compatible", "ai.generate",
        [this](auto request, auto configuration, auto cancelled, auto receive) {
            ai_worker_.Run(std::move(request.text), std::move(configuration), std::move(cancelled),
                [receive = std::move(receive)](rimes::runtime::AIUpdate update) {
                    receive({0, std::move(update.text), update.finished, update.error == rimes::runtime::AIError::None,
                             rimes::runtime::AIErrorMessage(update.error), std::move(update.blocks)});
                });
        });

    destroy_watch_ = instance_->watchEvent(
        EventType::InputContextDestroyed, EventWatcherPhase::Default,
        [this](Event& event) {
            auto& ic_event = static_cast<InputContextEvent&>(event);
            if (buffer_) {
                buffer_->OnInputContextDestroyed(ic_event.inputContext());
            }
            if (capsule_) {
                capsule_->OnInputContextDestroyed(ic_event.inputContext());
            }
        });
    capability_watch_ = instance_->watchEvent(
        EventType::InputContextCapabilityChanged, EventWatcherPhase::Default,
        [this](Event& event) {
            auto& cap = static_cast<CapabilityChangedEvent&>(event);
            const bool password = ProtectedInput(cap.newFlags());
            if (password) {
                if (auto* state = cap.inputContext()->propertyFor(&factory_)) state->reset();
            }
            if (!cap.inputContext()->hasFocus()) return;
            if (password && launcher_) launcher_->Revoke();
            if (buffer_) {
                buffer_->OnPasswordField(cap.inputContext(), password);
            }
            if (capsule_) {
                capsule_->OnPasswordField(cap.inputContext(), password);
            }
        });
    // Password fields can disable the IM entirely, so engine activate() is
    // not a reliable notification path (notably Firefox on Linux).
    focus_watch_ = instance_->watchEvent(EventType::InputContextFocusIn, EventWatcherPhase::Default,
        [this](Event& event) {
            auto* ic = static_cast<InputContextEvent&>(event).inputContext();
            const bool protected_field = ProtectedInput(ic->capabilityFlags());
            buffer_->OnPasswordField(ic, protected_field);
            capsule_->OnPasswordField(ic, protected_field);
        });
    session_monitor_ = std::make_unique<SessionMonitor>(instance_->eventLoop(), [this](bool protected_session) {
        if (protected_session && launcher_) launcher_->Revoke();
        buffer_->OnSessionProtection(protected_session);
        capsule_->OnSessionProtection(protected_session);
        if (protected_session) instance_->inputContextManager().foreach([this](InputContext* ic) {
            if (auto* state = ic->propertyFor(&factory_)) state->reset();
            return true;
        });
    });
}

void RimesIme::reloadConfig() {
    readAsIni(config_, "conf/rimes.conf");
    if (plugin_binding_) plugin_binding_->Refresh();
    if (capsule_ && (!plugin_binding_ || !plugin_binding_->Manages("builtin.capsule.notes")))
        plugins_.ApplyState("builtin.capsule.notes", true, *config_.capsuleNotes);
}
void RimesIme::setConfig(const RawConfig& config) {
    config_.load(config, true);
    safeSaveAsIni(config_, "conf/rimes.conf");
    if (!plugin_binding_ || !plugin_binding_->Manages("builtin.capsule.notes"))
        plugins_.ApplyState("builtin.capsule.notes", true, *config_.capsuleNotes);
    instance_->inputContextManager().foreachFocused([this](InputContext* ic) {
        if (auto* state = ic->propertyFor(&factory_)) state->refreshCandidates();
        return true;
    });
}

RimesIme::~RimesIme() {
    // Closing a companion UI can synchronously reactivate a client. Retire
    // every observer before stopping the engine or destroying either service.
    focus_watch_.reset();
    capability_watch_.reset();
    destroy_watch_.reset();
    desktop_shortcuts_.reset();
    launcher_.reset();
    session_monitor_.reset();
    configuration_watch_.reset();
    if (configuration_watch_fd_ >= 0) close(configuration_watch_fd_);
    plugin_watch_.reset();
    if (plugin_watch_fd_ >= 0) close(plugin_watch_fd_);
    plugin_binding_.reset();
    plugins_.Shutdown();
    if (alive_) {
        alive_->store(false);
    }
    // Workers may already have passed the alive check. Join while the event
    // dispatcher and service callbacks still exist, before detaching it.
    translation_worker_.Shutdown();
    ai_worker_.Shutdown();
    engine_.SetDeployReadyCallback(nullptr);
    engine_.Stop();
    // Socket readers also post to this dispatcher; join them before detach.
    buffer_.reset();
    capsule_.reset();
    dispatcher_.detach();
}

void RimesIme::WatchPluginPackages() {
    try {
        plugin_store_ = std::make_unique<rimes::plugins::PackageStore>(
            rimes::runtime::UserRoot() / "plugins", rimes::plugins::OfficialCatalog(),
            rimes::plugins::BundledPackages(), rimes::plugins::CompiledAdapters(), RIMES_PRODUCT_VERSION);
        rimes::runtime::PrivateDirectory migration(rimes::runtime::UserRoot() / "configuration");
        if (!migration.Read("capsule-modules-v1", 64)) {
            if (!*config_.capsuleNotes) {
                const auto notes = plugin_store_->Inspect("builtin.capsule.notes");
                if (notes.enabled) plugin_store_->Enable(notes.id, false, notes.grant);
            }
            migration.Write("capsule-modules-v1", "migrated\n");
        }
        plugin_watch_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (plugin_watch_fd_ < 0 || inotify_add_watch(plugin_watch_fd_, plugin_store_->Path().c_str(),
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB) < 0)
            throw std::runtime_error("Plugin observer unavailable");
        plugin_binding_ = std::make_unique<rimes::plugins::PackageBinding>(*plugin_store_, plugins_);
        plugin_watch_ = instance_->eventLoop().addIOEvent(plugin_watch_fd_, IOEventFlag::In,
            [this](EventSourceIO*, int fd, IOEventFlags) {
                alignas(inotify_event) std::array<char, 16384> events{};
                bool valid = true;
                for (;;) {
                    const auto count = read(fd, events.data(), events.size());
                    if (count < 0 && errno == EINTR) continue;
                    if (count < 0 && errno == EAGAIN) break;
                    if (count <= 0) { valid = false; break; }
                    for (std::size_t offset = 0; offset < static_cast<std::size_t>(count);) {
                        const auto* event = reinterpret_cast<const inotify_event*>(events.data() + offset);
                        if (event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) valid = false;
                        offset += sizeof(inotify_event) + event->len;
                    }
                }
                if (valid) plugin_binding_->Refresh();
                else plugin_binding_->Revoke();
                if (buffer_) buffer_->Publish();
                if (capsule_) capsule_->Publish();
                return valid;
            });
        if (!plugin_watch_) throw std::runtime_error("Plugin observer unavailable");
    } catch (const std::exception&) {
        if (plugin_binding_) plugin_binding_->Revoke();
        plugin_watch_.reset();
        if (plugin_watch_fd_ >= 0) { close(plugin_watch_fd_); plugin_watch_fd_ = -1; }
        FCITX_LOGC(rimes_log, Error) << "Official plugins unavailable; check private data permissions";
    }
}

bool RimesIme::acceptsPluginResult(const rimes::plugins::Lease& lease) {
    ApplyConfiguration(); // Close the same notification-order race as receipts.
    return plugin_binding_ && plugin_binding_->Accepts(lease);
}

void RimesIme::ApplyConfiguration() {
    try {
        if (!configuration_) throw std::runtime_error("Configuration unavailable");
        const auto config = configuration_->Load();
        const auto revision = config.at("revision").get<std::string>();
        if (revision == configuration_revision_) return;
        configuration_revision_ = revision;
        for (const auto& state : plugins_.Entries()) plugins_.Invalidate(state.descriptor.id);
        buffer_->Configure(config.at("closeAfterLast"), config.at("resetOnAppSwitch"), config.at("bufferWidth"), revision, config.at("theme"));
        buffer_->ConfigureAutoSend(config.at("autoSend"), config.at("autoSendSeconds"));
        capsule_->ConfigureTheme(config.at("theme"));
        capsule_->ConfigureClipboard(rimes::runtime::ConfigurationStore::NormalizePlugin(
                                         "builtin.capsule.temporary",
                                         config.at("plugins").value("builtin.capsule.temporary",
                                                                    rimes::runtime::Json::object()))
                                         .at("captureEnabled"));
        if (launcher_) launcher_->Configure(config.at("shortcuts"), config.at("globalShortcuts"));
        if (desktop_shortcuts_) desktop_shortcuts_->Configure(config.at("shortcuts"), config.at("globalShortcuts"));
    } catch (const std::exception&) {
        configuration_revision_.clear();
        buffer_->ConfigureAutoSend(false, 1);
        capsule_->ConfigureClipboard(false);
        if (launcher_) launcher_->Revoke();
        for (const auto& state : plugins_.Entries()) plugins_.Invalidate(state.descriptor.id);
    }
}

void RimesIme::WatchConfiguration() {
    try {
        configuration_ = std::make_unique<rimes::runtime::ConfigurationStore>(rimes::runtime::UserRoot() / "configuration");
        configuration_watch_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (configuration_watch_fd_ < 0 || inotify_add_watch(configuration_watch_fd_, configuration_->Path().c_str(),
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB) < 0)
            throw std::runtime_error("Configuration observer unavailable");
        configuration_watch_ = instance_->eventLoop().addIOEvent(configuration_watch_fd_, IOEventFlag::In,
            [this](EventSourceIO*, int fd, IOEventFlags) {
                alignas(inotify_event) std::array<char, 16384> events{};
                bool valid = true;
                for (;;) {
                    const auto count = read(fd, events.data(), events.size());
                    if (count < 0 && errno == EINTR) continue;
                    if (count < 0 && errno == EAGAIN) break;
                    if (count <= 0) { valid = false; break; }
                    for (std::size_t offset = 0; offset < static_cast<std::size_t>(count);) {
                        const auto* event = reinterpret_cast<const inotify_event*>(events.data() + offset);
                        if (event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) valid = false;
                        offset += sizeof(inotify_event) + event->len;
                    }
                }
                if (!valid) configuration_.reset();
                ApplyConfiguration();
                return valid;
            });
        if (!configuration_watch_) throw std::runtime_error("Configuration observer unavailable");
        ApplyConfiguration();
    } catch (const std::exception&) {
        configuration_.reset();
        configuration_watch_.reset();
        if (configuration_watch_fd_ >= 0) { close(configuration_watch_fd_); configuration_watch_fd_ = -1; }
        FCITX_LOGC(rimes_log, Error) << "Plugin configuration unavailable; check private data permissions";
    }
}

void RimesIme::HandleDeployReady() {
    std::string error;
    if (!engine_.PollMaintenance(&error)) {
        if (engine_.IsDeploying()) {
            return;
        }
        FCITX_LOGC(rimes_log, Error) << "librime deploy failed: " << error;
        return;
    }
    OnDeployReady();
}

void RimesIme::OnDeployReady() {
    if (deploy_announced_) {
        return;
    }
    deploy_announced_ = true;
    FCITX_LOGC(rimes_log, Info) << "librime deploy finished";
    instance_->inputContextManager().foreachFocused([this](InputContext* ic) {
        if (instance_->inputMethod(ic) == "rimes") {
            instance_->showInputMethodInformation(ic);
            ic->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
        return true;
    });
}

void RimesIme::keyEvent(const InputMethodEntry& entry, KeyEvent& keyEvent) {
    FCITX_UNUSED(entry);
    if ((session_monitor_ && session_monitor_->protectedSession()) ||
        ProtectedInput(keyEvent.inputContext()->capabilityFlags())) return;
    auto* state = keyEvent.inputContext()->propertyFor(&factory_);
    if (state == nullptr) {
        return;
    }
    if (buffer_ && buffer_->HandleEarlyKey(keyEvent, state->composing())) {
        return;
    }
    if (capsule_ && capsule_->HandleEarlyKey(keyEvent)) {
        return;
    }
    state->keyEvent(keyEvent);
}

void RimesIme::activate(const InputMethodEntry& entry, InputContextEvent& event) {
    FCITX_UNUSED(entry);
    if ((session_monitor_ && session_monitor_->protectedSession()) ||
        ProtectedInput(event.inputContext()->capabilityFlags())) {
        buffer_->OnPasswordField(event.inputContext(), ProtectedInput(event.inputContext()->capabilityFlags()));
        capsule_->OnPasswordField(event.inputContext(), ProtectedInput(event.inputContext()->capabilityFlags()));
        return;
    }
    auto* state = event.inputContext()->propertyFor(&factory_);
    if (state != nullptr) {
        state->activate();
    }
    if (buffer_) {
        buffer_->OnActivate(event.inputContext());
    }
    if (capsule_) {
        capsule_->OnActivate(event.inputContext());
    }
}

void RimesIme::deactivate(const InputMethodEntry& entry, InputContextEvent& event) {
    FCITX_UNUSED(entry);
    // Drop capture and wipe client preedit before RimesState::reset paints
    // the input panel, so focus-out cannot commit a leftover guard.
    if (buffer_) {
        buffer_->OnDeactivate(event.inputContext(),
                              event.type() == EventType::InputContextSwitchInputMethod);
    }
    if (capsule_) {
        capsule_->OnDeactivate(event.inputContext(),
                               event.type() == EventType::InputContextSwitchInputMethod);
    }
    auto* state = event.inputContext()->propertyFor(&factory_);
    if (state != nullptr) {
        state->deactivate(event);
    }
}

void RimesIme::reset(const InputMethodEntry& entry, InputContextEvent& event) {
    FCITX_UNUSED(entry);
    auto* state = event.inputContext()->propertyFor(&factory_);
    if (state != nullptr) {
        state->reset();
    }
}

std::string RimesIme::subMode(const InputMethodEntry& entry, InputContext& inputContext) {
    FCITX_UNUSED(entry);
    if (engine_.IsDeploying()) {
        return "Deploying";
    }
    auto* state = inputContext.propertyFor(&factory_);
    if (state == nullptr) {
        return {};
    }
    return state->schemaId();
}

void RimesIme::commitText(InputContext* ic, std::string_view text) {
    static_cast<void>(rimes::linuxime::kCommitHookNote);
    if (ic == nullptr || text.empty() || !ic->hasFocus() || ProtectedInput(ic->capabilityFlags()) ||
        (session_monitor_ && session_monitor_->protectedSession())) {
        return;
    }
    if (buffer_ && buffer_->OnCommit(ic, text)) {
        return;
    }
    ic->commitString(std::string(text));
}

void RimesIme::applySnapshot(InputContext* ic,
                             const rimes::linuxime::EngineSnapshot& snapshot) {
    if (ic == nullptr) {
        return;
    }
    if (!snapshot.commit_text.empty()) {
        commitText(ic, snapshot.commit_text);
    }
}

AddonInstance* RimesImeFactory::create(AddonManager* manager) {
    return new RimesIme(manager->instance());
}

}  // namespace fcitx

FCITX_ADDON_FACTORY(fcitx::RimesImeFactory);
