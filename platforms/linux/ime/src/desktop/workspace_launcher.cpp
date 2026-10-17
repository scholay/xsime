#include "workspace_launcher.hpp"
#include "buffer/buffer_service.hpp"
#include "capsule/capsule_service.hpp"
#include "session/protected_input.hpp"
#include <fcitx/inputcontextmanager.h>

namespace fcitx {
namespace {
Key Normalized(Key key) {
    auto sym = key.sym();
    if (sym >= FcitxKey_A && sym <= FcitxKey_Z) sym = static_cast<KeySym>(sym + FcitxKey_a - FcitxKey_A);
    const auto masks = KeyStates{KeyState::Ctrl} | KeyState::Alt | KeyState::Shift | KeyState::Super;
    return Key(sym, key.states() & masks);
}
}
WorkspaceLauncher::WorkspaceLauncher(Instance& instance, BufferService& buffer, CapsuleService& capsule,
                                     std::function<bool()> open_settings)
    : instance_(instance), buffer_(buffer), capsule_(capsule), open_settings_(std::move(open_settings)) {
    for (auto event : {EventType::InputContextFocusIn, EventType::InputContextFocusOut,
                       EventType::InputContextSwitchInputMethod, EventType::InputContextDestroyed})
        watchers_.push_back(instance_.watchEvent(event, EventWatcherPhase::Default, [this](Event&) { Invalidate(); }));
    watchers_.push_back(instance_.watchEvent(EventType::InputContextKeyEvent, EventWatcherPhase::PreInputMethod,
        [this](Event& event) { HandleKey(static_cast<KeyEvent&>(event)); }));
}
WorkspaceLauncher::~WorkspaceLauncher() = default;
void WorkspaceLauncher::Invalidate() {
    ++epoch_; down_.clear(); pending_.reset();
    if (result_.value("status", "") == "pending")
        result_ = {{"ok", false}, {"request", request_id_}, {"status", "focus-changed"}, {"message", "焦点已变化，请重新打开工作台。"}};
}
void WorkspaceLauncher::Configure(const Json& shortcuts, bool global) {
    rimes::runtime::ValidateShortcuts(shortcuts);
    std::map<std::string, Key> next;
    for (auto it = shortcuts.begin(); it != shortcuts.end(); ++it)
        if (const auto value = rimes::runtime::ParseShortcut(it.value())) next.emplace(it.key(), Normalized(Key(value->Fcitx())));
    if (next == bindings_ && global_ == global) return;
    Invalidate(); bindings_ = std::move(next); global_ = global;
}
bool WorkspaceLauncher::HandleKey(KeyEvent& event) {
    const auto key = Normalized(event.rawKey());
    if (event.isRelease()) {
        if (down_.erase(key.sym())) { event.filterAndAccept(); return true; }
        return false;
    }
    auto* ic = event.inputContext();
    if (!ic || !ic->hasFocus() || ProtectedInput(ic->capabilityFlags()) || buffer_.model().secure() ||
        (!global_ && instance_.inputMethod(ic) != "rimes")) return false;
    for (const auto& [action, binding] : bindings_) {
        if (!key.check(binding) || action == "mailbox") continue; // registered when task 18 adds its host
        const auto status = desktop_status_ ? desktop_status_() : Json::object();
        if (status.value("recording", false)) return false;
        if (global_ && desktop_status_) {
            const auto state = status.value("actions", Json::object()).value(action, Json::object()).value("state", "");
            // The portal can assign a different shortcut in its own dialog.
            // Only its actual binding is authoritative once granted.
            if (status.value("backend", "") == "portal" && state == "registered") continue;
            if (instance_.inputMethod(ic) != "rimes" && (state == "conflict" || state == "not-granted")) continue;
        }
        if (!down_.insert(key.sym()).second) { event.filterAndAccept(); return true; }
        const auto response = Open(action);
        // Switching IM invalidates pending gestures; restore this press's
        // release ownership only after the synchronous switch completes.
        if (response.value("ok", false)) { down_.insert(key.sym()); event.filterAndAccept(); return true; }
        down_.erase(key.sym()); return false;
    }
    return false;
}
void WorkspaceLauncher::Show(const std::string& action, InputContext* ic) {
    if (action == "buffer") {
        if (!ic && buffer_.model().visible()) buffer_.CloseAndPause();
        else buffer_.Toggle(ic);
    }
    else if (action == "capsule") {
        if (ic) capsule_.Toggle(ic);
        else if (capsule_.model().visible()) capsule_.Close();
        else capsule_.ShowDetached();
    }
    result_ = {{"ok", true}, {"request", request_id_}, {"status", "opened"}};
}
WorkspaceLauncher::Json WorkspaceLauncher::Open(const std::string& action) {
    pending_.reset();
    ++request_id_;
    auto fail = [&](const char* status, const char* message) -> Json {
        result_ = {{"ok", false}, {"request", request_id_}, {"status", status}, {"message", message}}; return result_;
    };
    if (action != "buffer" && action != "capsule" && action != "settings")
        return fail("unavailable", "此功能尚未安装或当前版本不支持。");
    if (buffer_.model().secure()) return fail("protected", "会话受保护，请解锁后重试。");
    if (action == "capsule" && !capsule_.Enabled()) return fail("disabled", "胶囊插件已停用，请在设置中启用后重试。");
    if (action == "settings") {
        if (!open_settings_()) return fail("unavailable", "无法打开设置，请检查程序安装。");
        return result_ = {{"ok", true}, {"request", request_id_}, {"status", "opened"}};
    }
    InputContext* ic = nullptr;
    instance_.inputContextManager().foreachFocused([&](InputContext* value) { ic = value; return false; });
    if ((ic && ProtectedInput(ic->capabilityFlags())) || buffer_.model().password_field())
        return fail("protected", "当前输入框受保护，请切换到普通输入框。");
    if (!ic || instance_.inputMethod(ic) == "rimes") { Show(action, ic); return result_; }
    const auto uuid = ic->uuid();
    instance_.setCurrentInputMethod(ic, "rimes", true);
    ic = instance_.inputContextManager().findByUUID(uuid);
    if (!ic || !ic->hasFocus() || instance_.inputMethod(ic) != "rimes")
        return fail("input-method", "请先在 Fcitx5 中添加并启用 XSIME，然后重新打开。");
    const auto request = ++epoch_;
    result_ = {{"ok", true}, {"request", request_id_}, {"status", "pending"}};
    pending_ = instance_.eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 10000, 0,
        [this, uuid, request, action](EventSourceTime*, uint64_t) {
            auto* target = instance_.inputContextManager().findByUUID(uuid);
            if (request != epoch_ || !target || !target->hasFocus() || instance_.inputMethod(target) != "rimes" ||
                buffer_.model().secure() || ProtectedInput(target->capabilityFlags())) {
                result_ = {{"ok", false}, {"request", request_id_}, {"status", "focus-changed"}, {"message", "输入焦点已变化，请重新打开工作台。"}};
                return true;
            }
            Show(action, target); return true;
        });
    return result_;
}
} // namespace fcitx
