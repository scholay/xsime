#include "session/protected_input.hpp"
#include "native_buffer_metrics.hpp"
#include "buffer_service.hpp"
#include "buffer_application.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>

#include "buffer_process.hpp"
#include "engine/rime_key.hpp"

namespace fcitx {
namespace {

FCITX_DEFINE_LOG_CATEGORY(rimes_buffer_log, "rimes.buffer");

constexpr int kHoldTickUs = 50000;
constexpr int kDefaultFocusGraceMs = 5000;
constexpr int kDragHardTimeoutMs = 30000;

rimes::buffer::CaretRect CaretFromIc(InputContext* ic) {
    rimes::buffer::CaretRect caret;
    if (ic == nullptr) {
        return caret;
    }
    const auto& rect = ic->cursorRect();
    caret.x = rect.left();
    caret.y = rect.top();
    caret.width = rect.width();
    caret.height = rect.height();
    // A Wayland/RelativeRect cursor is surface-local. It is never an absolute
    // monitor anchor, and the client scale does not turn it into one.
    caret.valid = ic->display().rfind("x11:", 0) == 0 &&
                  !ic->capabilityFlags().test(CapabilityFlag::RelativeRect) &&
                  std::abs(static_cast<std::int64_t>(caret.x)) < 1000000 &&
                  std::abs(static_cast<std::int64_t>(caret.y)) < 1000000 &&
                  caret.width >= 0 && caret.width < 100000 && caret.height >= 0 && caret.height < 100000 &&
                  (caret.width > 0 || caret.height > 0 || caret.x != 0 || caret.y != 0);
    return caret;
}

int FocusGraceMsFromEnv() {
    const char* value = std::getenv("RIMES_BUFFER_FOCUS_GRACE_MS");
    if (value == nullptr || value[0] == '\0') {
        return kDefaultFocusGraceMs;
    }
    char* end = nullptr;
    const auto parsed = std::strtol(value, &end, 10);
    if (end == value || parsed < 50 || parsed > 30000) {
        return kDefaultFocusGraceMs;
    }
    return static_cast<int>(parsed);
}

std::string DefaultSocketPath() {
    if (const char* override_path = std::getenv("RIMES_BUFFER_SOCKET")) {
        if (override_path[0] != '\0') {
            return override_path;
        }
    }
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR")) {
        return std::string(runtime) + "/rimes-buffer.sock";
    }
    return "/tmp/rimes-buffer-" + std::to_string(geteuid()) + ".sock";
}

bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

int SetCloexec(int fd) {
    const int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    const int status = fcntl(fd, F_GETFL, 0);
    if (status >= 0) {
        fcntl(fd, F_SETFL, status | O_NONBLOCK);
    }
    return fd;
}

std::string FindUiBinary() {
    if (const char* override_path = std::getenv("RIMES_BUFFER_UI")) {
        if (override_path[0] != '\0') {
            return override_path;
        }
    }
    const char* candidates[] = {
        "/usr/libexec/rimes/rimes-buffer",
        "/usr/lib/rimes/rimes-buffer",
        "/usr/bin/rimes-buffer",
        "/usr/local/bin/rimes-buffer",
        nullptr,
    };
    for (const char* const* path = candidates; *path != nullptr; ++path) {
        if (access(*path, X_OK) == 0) {
            return *path;
        }
    }
    if (const char* path_env = std::getenv("PATH")) {
        std::string remaining = path_env;
        while (!remaining.empty()) {
            const auto split = remaining.find(':');
            const std::string dir = remaining.substr(0, split);
            const std::string candidate = dir + "/rimes-buffer";
            if (access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
            if (split == std::string::npos) {
                break;
            }
            remaining = remaining.substr(split + 1);
        }
    }
    return {};
}

}  // namespace

BufferService::BufferService(Instance* instance, PostFn post)
    : instance_(instance),
      post_(std::move(post)),
      socket_path_(DefaultSocketPath()),
      dump_path_(std::getenv("RIMES_BUFFER_DUMP") ? std::getenv("RIMES_BUFFER_DUMP") : ""),
      focus_grace_ms_(FocusGraceMsFromEnv()),
      auto_capture_(EnvFlag("RIMES_BUFFER_AUTO_CAPTURE")),
      headless_(EnvFlag("RIMES_BUFFER_HEADLESS")) {
    translation_.SetSegmenter([this](std::string_view text) { return model_.SplitSegments(text); });
    if (const char* close_after = std::getenv("RIMES_BUFFER_CLOSE_AFTER_LAST")) {
        model_.set_close_after_last(std::strcmp(close_after, "0") != 0);
    }
    StartSocket();
    Publish();
}

BufferService::~BufferService() {
    auto_send_timer_.reset();
    alive_->store(false);
    CancelDerived();
    CancelHoldTimer();
    CancelFocusGraceTimer();
    CancelDragHardTimer();
    ui_respawn_timer_.reset();
    StopSocket();
    ReapUi();
}

std::string BufferService::TokenFor(InputContext* ic) {
    if (ic == nullptr) {
        return {};
    }
    return std::to_string(reinterpret_cast<std::uintptr_t>(ic));
}

bool BufferService::IsToggleHotkey(const KeyEvent& event) {
    if (event.isRelease()) {
        return false;
    }
    const auto& key = event.rawKey();
    const bool shift = key.states().test(KeyState::Shift);
    const bool ctrl = key.states().test(KeyState::Ctrl);
    const bool super = key.states().test(KeyState::Super);
    const bool letter_b = key.check(FcitxKey_B) || key.check(FcitxKey_b) ||
                          key.sym() == FcitxKey_B || key.sym() == FcitxKey_b;
    return letter_b && shift && (ctrl || super) && !(ctrl && super);
}

bool BufferService::IsReturnKey(const Key& key) {
    return key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter);
}

bool BufferService::IsBackspaceKey(const Key& key) {
    return key.check(FcitxKey_BackSpace);
}

bool BufferService::IsPrintableAscii(const KeyEvent& event, char* out) {
    if (event.isRelease() || out == nullptr) {
        return false;
    }
    const auto states = event.rawKey().states();
    if (states.test(KeyState::Ctrl) || states.test(KeyState::Alt) ||
        states.test(KeyState::Super)) {
        return false;
    }
    const auto sym = static_cast<std::uint32_t>(event.key().sym());
    if (sym < 0x20 || sym > 0x7e) {
        return false;
    }
    *out = static_cast<char>(sym);
    return true;
}

InputContext* BufferService::LiveTarget() const {
    if (!instance_) return nullptr;
    InputContext* found = nullptr;
    instance_->inputContextManager().foreachFocused([&](InputContext* ic) {
        if (model_.captures(TokenFor(ic))) {
            found = ic;
            return false;
        }
        return true;
    });
    return found;
}

void BufferService::RefreshCaret(InputContext* ic) {
    if (ic == nullptr) {
        return;
    }
    model_.set_caret(CaretFromIc(ic));
    if (!ic->program().empty()) {
        model_.set_target_name(ic->program());
        target_token_ = TokenFor(ic);
    }
}

void BufferService::EndDrag() {
    // Once release is reported, another activation may be a different field
    // in Firefox's shared IC. Never retain a time window that consumes it.
    dragging_ = false;
    CancelDragHardTimer();
    if (model_.capture_enabled() && !LiveTarget()) {
        DropCaptureForSwitch("drag-ended-without-target");
        Publish();
    }
}

void BufferService::ConsumeDragBlip() {
    dragging_ = false;
    CancelDragHardTimer();
}

void BufferService::ArmDragHardTimer() {
    if (instance_ == nullptr) {
        return;
    }
    const uint64_t delay_us = static_cast<uint64_t>(kDragHardTimeoutMs) * 1000;
    drag_hard_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + delay_us, 0,
        [this](EventSourceTime*, uint64_t) {
            if (dragging_) {
                EndDrag();
            }
            return true;
        });
}

void BufferService::CancelDragHardTimer() {
    drag_hard_timer_.reset();
}

void BufferService::ClearClientPreedit(InputContext* ic) {
    if (ic == nullptr) {
        return;
    }
    ic->inputPanel().setClientPreedit(Text());
    ic->inputPanel().setPreedit(Text());
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
}

bool BufferService::OnCommit(InputContext* ic, std::string_view text) {
    if (ic == nullptr || text.empty()) {
        return false;
    }
    if (ProtectedInput(ic->capabilityFlags()) || model_.secure() ||
        model_.password_field()) {
        return false;
    }
    if (!model_.captures(TokenFor(ic))) {
        return false;
    }
    model_.finish_direct_run();
    model_.append(text, rimes::buffer::Origin::Rime);
    model_.set_preedit({});
    raw_input_.clear();
    if (!model_.visible()) {
        model_.set_visible(true);
    }
    RefreshCaret(ic);
    EnsureUi();
    Publish();
    FCITX_LOGC(rimes_buffer_log, Info) << "commit captured blocks=" << model_.pending_count();
    return true;
}

bool BufferService::HandleEarlyKey(KeyEvent& event, bool composing) {
    auto* ic = event.inputContext();
    if (ic == nullptr) {
        return false;
    }
    if (gesture_.pending() && !event.isRelease() && !IsReturnKey(event.rawKey()) && !IsReturnKey(event.key())) {
        gesture_.cancel(); CancelHoldTimer(); model_.set_hold_progress(0);
    }
    if (!managed_hotkey_ && IsToggleHotkey(event)) {
        Toggle(ic);
        event.filterAndAccept();
        return true;
    }

    if ((IsReturnKey(event.rawKey()) || IsReturnKey(event.key())) &&
        eat_return_until_release_) {
        if (event.isRelease()) {
            eat_return_until_release_ = false;
            ApplyReturnAction(gesture_.on_release(std::chrono::steady_clock::now()), ic,
                              composing);
            CancelHoldTimer();
        }
        event.filterAndAccept();
        return true;
    }

    const bool password = ProtectedInput(ic->capabilityFlags());
    OnPasswordField(ic, password);
    if (password || model_.secure()) {
        return false;
    }

    if (event.key().check(FcitxKey_Escape) && !event.isRelease() && !composing &&
        model_.captures(TokenFor(ic))) {
        CloseAndPause();
        event.filterAndAccept();
        return true;
    }

    if (!model_.captures(TokenFor(ic))) {
        return false;
    }

    if (IsReturnKey(event.rawKey()) || IsReturnKey(event.key())) {
        const auto now = std::chrono::steady_clock::now();
        const bool is_repeat = event.rawKey().states().test(KeyState::Repeat);
        if (composing) {
            if (event.isRelease()) {
                gesture_.on_release(now);
            } else if (!is_repeat) {
                eat_return_until_release_ = true;
                gesture_.on_press(true, is_repeat, now);
            }
            // Let Rime settle this physical Return into a commit. The same
            // press must not send; on_press(settle) keeps the hold disarmed.
            return false;
        }
        if (event.isRelease()) {
            eat_return_until_release_ = false;
            ApplyReturnAction(gesture_.on_release(now), ic, composing);
        } else {
            if (!is_repeat) {
                eat_return_until_release_ = true;
            }
            SynchronizeDerived();
            const bool derived = plugins_ && !plugins_->SelectedBuffer().empty();
            if (derived && !CanDeliverDerived()) {
                if (!is_repeat) {
                    gesture_.cancel(); CancelHoldTimer();
                    // Return requests generation only when there is no ready
                    // output to send. A missing target or a commit conflict is
                    // not permission to replace an existing result.
                    if (!delivery_conflict_ && derived_.Phase() != rimes::buffer::DerivedPhase::Ready &&
                        !(Translating() && translation_.Head())) Generate();
                }
                event.filterAndAccept(); return true;
            }
            ApplyReturnAction(gesture_.on_press(false, is_repeat, now, derived), ic, composing);
            return_generation_ = model_.generation();
            if (!is_repeat && gesture_.pending() && !gesture_.settle_only()) {
                ArmHoldTimer();
            }
        }
        event.filterAndAccept();
        return true;
    }

    if ((IsBackspaceKey(event.rawKey()) || IsBackspaceKey(event.key())) &&
        !event.isRelease()) {
        if (composing) {
            return false;
        }
        if (!model_.delete_backward_direct(TokenFor(ic))) {
            model_.remove_last_block();
        }
        Publish();
        event.filterAndAccept();
        return true;
    }

    const auto& key = event.rawKey();
    if (!event.isRelease() &&
        (key.check(FcitxKey_A, KeyState::Ctrl) || key.check(FcitxKey_a, KeyState::Ctrl))) {
        model_.select_all();
        Publish();
        event.filterAndAccept();
        return true;
    }
    if (!event.isRelease() &&
        (key.check(FcitxKey_V, KeyState::Ctrl) || key.check(FcitxKey_v, KeyState::Ctrl))) {
        WriteAll(R"({"v":1,"type":"request_clipboard"})");
        event.filterAndAccept();
        return true;
    }

    return false;
}

void BufferService::AfterRime(InputContext* ic, KeyEvent& event, bool rime_handled,
                              bool composing, std::string_view preedit,
                              std::string_view raw_input) {
    if (ic == nullptr) {
        return;
    }
    if (!model_.captures(TokenFor(ic))) {
        if (model_.preedit() != preedit && !model_.capture_enabled()) {
            model_.set_preedit({});
            raw_input_.clear();
        }
        return;
    }
    model_.set_preedit(std::string(preedit));
    raw_input_ = std::string(raw_input);
    RefreshCaret(ic);
    if (!rime_handled && !event.isRelease() && !composing) {
        char printable = 0;
        if (IsPrintableAscii(event, &printable)) {
            const char text[] = {printable, '\0'};
            model_.append_direct_fragment(text, TokenFor(ic));
            event.filterAndAccept();
        }
    }
    if (rime_handled && !preedit.empty()) {
        model_.finish_direct_run();
    }
    Publish();
}

void BufferService::Configure(bool close_after_last, bool reset_on_app_switch, int width,
                              std::string revision, std::string theme) {
    configuration_revision_ = std::move(revision);
    theme_ = std::move(theme);
    if (!std::getenv("RIMES_BUFFER_CLOSE_AFTER_LAST")) model_.set_close_after_last(close_after_last);
    reset_on_app_switch_ = reset_on_app_switch;
    width_ = std::clamp(width, rimes::native::bufferMinimumWidth, rimes::native::bufferMaximumWidth);
    Publish();
}

void BufferService::ConfigureAutoSend(bool enabled, int seconds) {
    if (seconds != 1 && seconds != 2 && seconds != 3 && seconds != 5) return;
    if (auto_send_ != enabled || auto_send_seconds_ != seconds) auto_send_clock_.Reset();
    auto_send_ = enabled; auto_send_seconds_ = seconds;
    Publish();
}

void BufferService::SynchronizeAutoSend() {
    const auto plugin = plugins_ ? plugins_->SelectedBuffer() : std::string{};
    auto_send_clock_.Synchronize(plugin, model_.blocks());
    if (model_.secure() || model_.password_field() || model_.last_reason() == rimes::buffer::MutationReason::PrivacyDiscard)
        auto_send_clock_.Reset();
    const bool active = auto_send_ && plugin.empty() && model_.visible() && model_.processing_active() &&
        model_.capture_enabled() && !model_.secure() && !model_.password_field() && !model_.empty() && LiveTarget();
    if (!active) { auto_send_clock_.Pause(); return; }
    if (!auto_send_timer_ && instance_) {
        auto_send_timer_ = instance_->eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 100000, 0,
            [this](EventSourceTime* timer, uint64_t) {
                (void)timer;
                auto_send_scheduled_ = false;
                AutoSendTick();
                return true;
            });
    }
    if (auto_send_timer_ && !auto_send_scheduled_) {
        auto_send_timer_->setTime(now(CLOCK_MONOTONIC) + 100000); auto_send_timer_->setOneShot();
        auto_send_scheduled_ = true;
    }
}

void BufferService::AutoSendTick() {
    SynchronizeAutoSend();
    const bool active = auto_send_ && (!plugins_ || plugins_->SelectedBuffer().empty()) &&
        model_.visible() && model_.processing_active() && model_.capture_enabled() &&
        !model_.secure() && !model_.password_field() && LiveTarget();
    const auto head = auto_send_clock_.Tick(static_cast<double>(now(CLOCK_MONOTONIC)) / 1000000.0,
                                           active, auto_send_seconds_);
    // Stable earlier blocks may age while composing; no timer settles preedit
    // or competes with a physical Return/drag operation.
    if (head && model_.preedit().empty() && !gesture_.pending() && !dragging_ &&
        !model_.blocks().empty() && model_.blocks().front().id == *head) Deliver(false);
    else if (active && !model_.empty()) Publish();
}

void BufferService::BindPlugins(rimes::plugins::Registry& registry,
    std::function<bool(const rimes::plugins::Lease&)> validate,
    std::function<rimes::runtime::RequestConfiguration(std::string_view)> freeze) {
    CancelDerived();
    plugins_ = &registry;
    validate_ = std::move(validate);
    freeze_ = std::move(freeze);
}

void BufferService::RegisterDerivedAdapter(std::string id, std::string capability, RunDerived run, bool live_expand) {
    if (!plugins_ || !run || !validate_ || !freeze_) throw std::runtime_error("Unbound Buffer runtime");
    const auto* state = plugins_->Find(id);
    if (!state || state->descriptor.kind != rimes::plugins::Kind::Buffer ||
        !state->descriptor.capabilities.count(capability) || derived_adapters_.count(id))
        throw std::runtime_error("Invalid compiled Buffer adapter");
    derived_adapters_.emplace(std::move(id), DerivedAdapter{std::move(capability), std::move(run), live_expand});
}

void BufferService::CancelDerived() {
    CancelTranslation(false);
    if (derived_cancel_) derived_cancel_->store(true);
    derived_cancel_.reset();
    derived_request_.reset();
    derived_.Cancel();
    result_pending_.clear(); result_pending_id_.clear();
    derived_logical_blocks_.clear(); derived_error_.clear();
}

void BufferService::SynchronizeDerived() {
    // A permission check may refresh configuration and publish a new snapshot.
    if (synchronizing_derived_) return;
    synchronizing_derived_ = true;
    if (model_.last_reason() == rimes::buffer::MutationReason::PrivacyDiscard) translation_.Clear();
    bool grant = false;
    try { grant = derived_request_ && validate_ && validate_(derived_request_->lease); }
    catch (const std::exception&) { grant = false; }
    derived_.Synchronize(model_.blocks(), plugins_ ? plugins_->SelectedBuffer() : "",
        configuration_revision_, {model_.capture_token(), focus_epoch_},
        model_.visible() && model_.processing_active() && !focus_suspended_, model_.secure() || model_.password_field());
    if (Translating()) {
        SynchronizeTranslation();
        synchronizing_derived_ = false;
        return;
    }
    CancelTranslation(true);
    if (derived_request_ && !derived_.Owns(*derived_request_, grant)) CancelDerived();
    SynchronizeResultDelivery();
    if (derived_.Phase() != rimes::buffer::DerivedPhase::Failed) derived_error_.clear();
    synchronizing_derived_ = false;
}

bool BufferService::SelectPlugin(const std::string& id) {
    if (!plugins_ || (!id.empty() && !derived_adapters_.count(id)) || !plugins_->SelectBuffer(id)) return false;
    CancelDerived();
    gesture_.cancel(); CancelHoldTimer();
    Publish();
    return true;
}

bool BufferService::Generate() {
    if (Translating()) return TranslationTick(true);
    SynchronizeDerived();
    if (!plugins_ || !freeze_ || !validate_) return false;
    const auto adapter = derived_adapters_.find(plugins_->SelectedBuffer());
    if (adapter == derived_adapters_.end()) return false;
    const auto lease = plugins_->Authorize(adapter->first, adapter->second.capability);
    bool started = false;
    try {
        if (!lease || !validate_(*lease)) return false;
        const auto configuration = freeze_(adapter->first);
        if (configuration.revision != configuration_revision_) return false;
        const auto request = derived_.Begin(*lease);
        if (!request) return false;
        started = true;
        derived_error_.clear(); derived_logical_blocks_.clear();
        if (derived_cancel_) derived_cancel_->store(true);
        derived_request_ = request;
        derived_cancel_ = std::make_shared<std::atomic<bool>>(false);
        auto receive = [this, alive = alive_, post = post_, request = *request](ResultUpdate update) {
            if (!alive->load()) return;
            post([this, alive, request, update = std::move(update)] {
                if (!alive->load()) return;
                const auto& [alternative, text, finished, success, message, blocks] = update;
                SynchronizeDerived();
                bool grant = false;
                try { grant = validate_ && validate_(request.lease); } catch (const std::exception&) {}
                if (!derived_.Accepts(request, grant)) { Publish(); return; }
                const bool valid = text.empty() || derived_.Update(request, grant, alternative, text);
                if (finished && success && !blocks.empty()) {
                    std::string joined;
                    for (const auto& block : blocks) joined += block;
                    if (blocks.size() <= 20 && joined == text) derived_logical_blocks_[alternative] = blocks;
                }
                if (finished || !valid) {
                    derived_.Finish(request, grant, success && valid);
                    if (derived_cancel_) derived_cancel_->store(true);
                    if (derived_.Phase() == rimes::buffer::DerivedPhase::Failed) {
                        derived_request_.reset();
                        derived_error_ = message.size() <= 512 && message.find('\0') == message.npos ? message : "生成失败，原文已保留";
                    }
                }
                Publish();
            });
        };
        adapter->second.run(*request, configuration, derived_cancel_, std::move(receive));
        Publish();
        return true;
    } catch (const std::exception&) {
        // A preflight failure must not erase an earlier ready result. Once
        // Begin has transferred ownership, only the new job is cancelled.
        if (started) CancelDerived();
        Publish();
        return false;
    }
}

void BufferService::OnActivate(InputContext* ic) {
    if (ic == nullptr) {
        return;
    }
    OnPasswordField(ic, ProtectedInput(ic->capabilityFlags()));
    if (model_.secure() || model_.password_field()) return;
    focus_suspended_ = false;
    const auto token = TokenFor(ic);
    FCITX_LOGC(rimes_buffer_log, Info)
        << "activate token=" << token << " capturing=" << model_.capture_enabled()
        << " auto=" << auto_capture_ << " drag=" << dragging_
        << " pending=" << pending_unfocus_token_;
    const bool same_ic = (!pending_unfocus_token_.empty() && pending_unfocus_token_ == token) ||
                         model_.captures(token);
    if (same_ic) {
        // Firefox/Chromium keep one IC per window. Keep capture only for an
        // explicit, still-active toolbar drag. Consume one WM activation;
        // after receiving drag_end every activation is a field switch.
        // Physical release can precede that command (GTK polls every 50ms).
        pending_unfocus_token_.clear();
        CancelFocusGraceTimer();
        if (dragging_) {
            ConsumeDragBlip();
            RefreshCaret(ic);
            OnPasswordField(ic, ProtectedInput(ic->capabilityFlags()));
            Publish();
            return;
        }
        DropCaptureForSwitch("same-ic-reactivate");
    }
    if (model_.capture_enabled() && !model_.captures(token)) {
        DropCaptureForSwitch("focus-changed");
    }
    // Only a known external application transition may clear local typing.
    // Companion settings/editors and mixed content staged for another app
    // preserve the previous external identity and the complete workspace.
    const auto program = ic->program();
    const bool external = !program.empty() && !rimes::buffer::IsCompanionApplication(program);
    if (external) {
        const bool staged_external = std::any_of(model_.blocks().begin(), model_.blocks().end(), [](const auto& block) {
            return block.origin != rimes::buffer::Origin::Rime && block.origin != rimes::buffer::Origin::Local;
        });
        if (reset_on_app_switch_ && !last_external_program_.empty() &&
            program != last_external_program_ && !staged_external) {
            model_.discard_for_privacy();
            raw_input_.clear();
        }
        last_external_program_ = program;
    }
    OnPasswordField(ic, ProtectedInput(ic->capabilityFlags()));
    if (auto_capture_ && !ProtectedInput(ic->capabilityFlags())) {
        ShowAndCapture(ic);
    }
    Publish();
}

void BufferService::OnDeactivate(InputContext* ic, bool switching_im) {
    FCITX_LOGC(rimes_buffer_log, Info)
        << "deactivate switch_im=" << switching_im << " capturing=" << model_.capture_enabled()
        << " drag=" << dragging_;
    if (switching_im && ic && ProtectedInput(ic->capabilityFlags())) {
        OnPasswordField(ic, true);
        ClearClientPreedit(ic);
        return;
    }
    if (switching_im && model_.captures(TokenFor(ic))) {
        CloseAndPause();
        return;
    }
    ClearClientPreedit(ic);
    if (!model_.captures(TokenFor(ic))) {
        return;
    }
    gesture_.cancel(); CancelHoldTimer();
    if (dragging_) {
        // A window-manager move temporarily deactivates the same IC. Keep
        // generation ownership until this drag ends or another IC activates;
        // native delivery still requires LiveTarget() on every commit.
        Publish();
        return;
    }
    focus_suspended_ = true;
    ++focus_epoch_;
    CancelDerived();
    // Do not drop capture yet. A toolbar drag or a brief WM grab will
    // activate this same IC again; a real field switch activates another.
    pending_unfocus_token_ = TokenFor(ic);
    ArmFocusGraceTimer();
    Publish();
}

void BufferService::OnInputContextDestroyed(InputContext* ic) {
    if (ic == nullptr) {
        return;
    }
    const auto token = TokenFor(ic);
    FCITX_LOGC(rimes_buffer_log, Info) << "ic destroyed token=" << token;
    const bool ours = model_.captures(token) || pending_unfocus_token_ == token ||
                      target_token_ == token;
    if (!ours) {
        return;
    }
    if (model_.captures(token) || pending_unfocus_token_ == token) {
        DropCaptureForSwitch("ic-destroyed");
    }
    if (target_token_ == token) {
        model_.set_target_name({});
        target_token_.clear();
    }
    Publish();
}

void BufferService::OnPasswordField(InputContext* ic, bool password) {
    // Capability events from background ICs must not shield or unshield the
    // active workspace. null is used by the host's isolated policy tests.
    if (ic && !ic->hasFocus()) return;
    if (password) {
        eat_return_until_release_ = false;
        if (!model_.password_field()) {
            ++focus_epoch_;
            CancelDerived(); translation_.Clear();
            gesture_.cancel(); CancelHoldTimer(); CancelFocusGraceTimer();
            dragging_ = false; CancelDragHardTimer(); pending_unfocus_token_.clear();
            raw_input_.clear(); model_.set_preedit({});
            target_token_.clear(); model_.set_target_name({});
        }
        model_.set_password_field(true);
        if (model_.capture_enabled()) {
            model_.route_direct_preserving_content("password");
        }
        Publish();
        return;
    }
    if (model_.password_field()) {
        model_.set_password_field(false);
        Publish();
    }
}

void BufferService::OnSessionProtection(bool protected_session) {
    if (model_.secure() == protected_session) return;
    if (protected_session) {
        // Protected key events bypass Buffer, including a held Return's key-up.
        eat_return_until_release_ = false;
        restore_after_session_ = model_.visible();
        model_.set_secure(true);
        ++focus_epoch_;
        CancelDerived(); translation_.Clear();
        gesture_.cancel(); CancelHoldTimer(); CancelFocusGraceTimer(); CancelDragHardTimer();
        dragging_ = false; pending_unfocus_token_.clear(); raw_input_.clear();
        target_token_.clear(); model_.set_target_name({});
        auto_send_clock_.Reset(); focus_suspended_ = true;
        // Keep source blocks and geometry, but retire all target association,
        // preedit, results and automatic processing without calling an old IC.
        model_.pause_capture_preserving_content();
    } else {
        model_.set_secure(false);
        if (restore_after_session_) model_.set_visible(true);
        restore_after_session_ = false;
        // Showing chrome is not permission to reuse a previous input target.
    }
    Publish();
}

void BufferService::Toggle(InputContext* ic) {
    FCITX_LOGC(rimes_buffer_log, Info)
        << "toggle visible=" << model_.visible()
        << " capturing=" << model_.capture_enabled()
        << " token=" << model_.capture_token();
    if (model_.visible() && model_.capture_enabled()) {
        CloseAndPause();
        return;
    }
    ShowAndCapture(ic);
}

void BufferService::ShowAndCapture(InputContext* ic) {
    if (model_.secure() || model_.password_field()) return;
    if (ic == nullptr && instance_) {
        focus_suspended_ = false;
        instance_->inputContextManager().foreachFocused([&](InputContext* focused) {
            ic = focused;
            return false;
        });
    }
    if (ic == nullptr) {
        focus_suspended_ = false;
        model_.set_visible(true);
        model_.resume_workbench_processing();
        EnsureUi();
        Publish();
        return;
    }
    if (ProtectedInput(ic->capabilityFlags())) {
        return;
    }
    RefreshCaret(ic);
    focus_suspended_ = false;
    ++focus_epoch_;
    model_.activate_capture(TokenFor(ic));
    model_.set_visible(true);
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    EnsureUi();
    Publish();
}

void BufferService::CloseAndPause() {
    restore_after_session_ = false;
    ++focus_epoch_;
    CancelDerived();
    gesture_.cancel();
    CancelHoldTimer();
    CancelFocusGraceTimer();
    dragging_ = false;
    CancelDragHardTimer();
    pending_unfocus_token_.clear();
    raw_input_.clear();
    ClearClientPreedit(LiveTarget());
    model_.pause_capture_preserving_content();
    Publish();
}

void BufferService::StageOpenPreedit() {
    auto text = raw_input_;
    if (text.empty()) {
        text = model_.preedit();
    }
    raw_input_.clear();
    if (text.empty()) {
        return;
    }
    model_.append(text, rimes::buffer::Origin::Local);
    model_.set_preedit({});
}

void BufferService::DropCaptureForSwitch(std::string_view reason) {
    ++focus_epoch_;
    CancelDerived();
    StageOpenPreedit();
    gesture_.cancel();
    CancelHoldTimer();
    CancelFocusGraceTimer();
    dragging_ = false;
    CancelDragHardTimer();
    pending_unfocus_token_.clear();
    model_.route_direct_preserving_content(reason);
}

void BufferService::ArmFocusGraceTimer() {
    if (instance_ == nullptr) {
        return;
    }
    const uint64_t delay_us = static_cast<uint64_t>(focus_grace_ms_) * 1000;
    focus_grace_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + delay_us, 0,
        [this](EventSourceTime*, uint64_t) {
            if (!pending_unfocus_token_.empty()) {
                DropCaptureForSwitch("focus-out");
                Publish();
            }
            return true;
        });
}

void BufferService::CancelFocusGraceTimer() {
    focus_grace_timer_.reset();
}

bool BufferService::SendNext() { return Deliver(false); }

bool BufferService::SendAll() { return Deliver(true); }

void BufferService::ApplyReturnAction(rimes::buffer::ReturnGesture::Action action,
                                      InputContext* ic, bool composing) {
    (void)composing;
    switch (action) {
        case rimes::buffer::ReturnGesture::Action::SettleOnly:
            // The caller still needs Rime to see Return so composition becomes
            // a commit. RimesState processes that after we return false from
            // a dedicated settle path. Here we only mark the gesture.
            break;
        case rimes::buffer::ReturnGesture::Action::ArmHold:
            model_.set_hold_progress(0);
            Publish();
            break;
        case rimes::buffer::ReturnGesture::Action::SendNext:
            Deliver(false);
            model_.set_hold_progress(0);
            Publish();
            break;
        case rimes::buffer::ReturnGesture::Action::SendAll:
            Deliver(true);
            model_.set_hold_progress(0);
            CancelHoldTimer();
            Publish();
            break;
        case rimes::buffer::ReturnGesture::Action::Consume:
        case rimes::buffer::ReturnGesture::Action::None:
            model_.set_hold_progress(0);
            if (ic != nullptr) {
                RefreshCaret(ic);
            }
            Publish();
            break;
    }
}

bool BufferService::Deliver(bool all) {
    if (!model_.preedit().empty()) return false;
    if (plugins_ && !plugins_->SelectedBuffer().empty()) return DeliverDerived(all);
    auto* ic = LiveTarget();
    if (ic == nullptr) {
        FCITX_LOGC(rimes_buffer_log, Info) << "delivery rejected: no live target";
        return false;
    }
    if (ProtectedInput(ic->capabilityFlags()) || model_.secure() ||
        model_.password_field()) {
        return false;
    }
    const auto token = TokenFor(ic);
    std::vector<std::string> sent;
    const auto blocks = model_.blocks();
    for (const auto& block : blocks) {
        if (!model_.captures(token) || LiveTarget() != ic) {
            break;
        }
        ic->commitString(block.text);
        sent.push_back(block.id);
        if (!all) {
            break;
        }
    }
    if (sent.empty()) {
        return false;
    }
    model_.consume_delivered(sent);
    const bool drained = model_.empty();
    if (drained && model_.close_after_last()) {
        CloseAndPause();
    } else {
        Publish();
    }
    return true;
}

void BufferService::HandleCommand(const rimes::buffer::Command& command) {
    if ((model_.secure() || model_.password_field()) && command.op != rimes::buffer::CommandOp::Hello &&
        command.op != rimes::buffer::CommandOp::Query && command.op != rimes::buffer::CommandOp::Close) {
        Publish(); return;
    }
    InputContext* ic = LiveTarget();
    if (ic == nullptr) {
        instance_->inputContextManager().foreachFocused([&](InputContext* focused) {
            ic = focused;
            return false;
        });
    }
    switch (command.op) {
        case rimes::buffer::CommandOp::Hello:
        case rimes::buffer::CommandOp::Query:
            break;
        case rimes::buffer::CommandOp::Toggle:
            Toggle(ic);
            break;
        case rimes::buffer::CommandOp::Show:
            ShowAndCapture(ic);
            break;
        case rimes::buffer::CommandOp::Close:
            CloseAndPause();
            break;
        case rimes::buffer::CommandOp::SendNext:
            SendNext();
            break;
        case rimes::buffer::CommandOp::SendAll:
            SendAll();
            break;
        case rimes::buffer::CommandOp::CycleAutoSend:
            if ((!plugins_ || plugins_->SelectedBuffer().empty()) && !model_.secure() && cycle_auto_send_) {
                try { cycle_auto_send_(); }
                catch (const std::exception&) { WriteAll(rimes::buffer::EncodeError("settings", "自动发送设置未保存，请重新加载设置后重试")); }
            }
            break;
        case rimes::buffer::CommandOp::ToggleCloseAfterLast:
            if ((!plugins_ || plugins_->SelectedBuffer().empty()) && !model_.secure() && toggle_close_after_last_) {
                try { toggle_close_after_last_(); }
                catch (const std::exception&) { WriteAll(rimes::buffer::EncodeError("settings", "发送完关闭设置未保存，请重新加载设置后重试")); }
            }
            break;
        case rimes::buffer::CommandOp::RemoveLast:
            model_.remove_last_block();
            break;
        case rimes::buffer::CommandOp::SelectAll:
            model_.select_all();
            break;
        case rimes::buffer::CommandOp::Paste:
            model_.insert_pasted_text(command.text, rimes::buffer::Origin::Clipboard);
            if (!model_.visible()) {
                model_.set_visible(true);
            }
            EnsureUi();
            break;
        case rimes::buffer::CommandOp::SetInsertion:
            model_.set_insertion_point(command.insertion_index);
            break;
        case rimes::buffer::CommandOp::DragBegin:
            FCITX_LOGC(rimes_buffer_log, Info) << "toolbar drag_begin received";
            dragging_ = true;
            pending_unfocus_token_.clear();
            CancelFocusGraceTimer();
            ArmDragHardTimer();
            return;
        case rimes::buffer::CommandOp::DragEnd:
            FCITX_LOGC(rimes_buffer_log, Info) << "toolbar drag_end received";
            EndDrag();
            return;
        case rimes::buffer::CommandOp::SelectPlugin:
            SelectPlugin(command.id);
            break;
        case rimes::buffer::CommandOp::Generate:
            Generate();
            break;
        case rimes::buffer::CommandOp::CancelGeneration:
            CancelDerived();
            break;
        case rimes::buffer::CommandOp::ReturnToEdit:
            CancelDerived();
            delivery_conflict_ = false;
            if (Translating()) translation_.ReturnToEdit();
            break;
        case rimes::buffer::CommandOp::SelectResult:
            SynchronizeDerived();
            derived_.SelectResult(command.id);
            break;
        case rimes::buffer::CommandOp::CopyResult: {
            SynchronizeDerived();
            if (TranslationActive()) {
                if (translation_.Complete() && command.id == translation_result_id_) {
                    const auto text = translation_.Preview();
                    if (!text.empty()) WriteAll(rimes::runtime::Json{{"v", 1}, {"type", "copy_result"}, {"id", command.id}, {"text", text}}.dump());
                }
                break;
            }
            const auto request = derived_request_;
            bool grant = false;
            try { grant = request && validate_ && validate_(request->lease); }
            catch (const std::exception&) { CancelDerived(); break; }
            SynchronizeDerived();
            if (request && derived_.Owns(*request, grant) &&
                derived_.SelectResult(command.id)) {
                const auto text = derived_.CopyText();
                if (!text.empty()) WriteAll(rimes::runtime::Json{{"v", 1}, {"type", "copy_result"},
                    {"id", command.id}, {"text", text}}.dump());
            }
            break;
        }
        case rimes::buffer::CommandOp::Unknown:
            break;
    }
    Publish();
}

void BufferService::EnsureUi() {
    if (headless_ || !model_.visible()) {
        return;
    }
    ReapUi();
    if (ui_pid_ > 0) {
        return;
    }
    const auto binary = FindUiBinary();
    if (binary.empty()) {
        FCITX_LOGC(rimes_buffer_log, Info) << "rimes-buffer UI is not installed";
        return;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        execl(binary.c_str(), "rimes-buffer", "--socket", socket_path_.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    ui_pid_ = pid;
}

void BufferService::ReapUi() {
    if (ui_pid_ <= 0) {
        return;
    }
    if (rimes::buffer::UiProcessGone(ui_pid_)) {
        ui_pid_ = 0;
    }
}

void BufferService::ReplaceDroppedUi() {
    if (!rimes::buffer::ShouldForceUiRespawn(ui_pid_, dropped_ui_pid_)) {
        return;
    }
    rimes::buffer::DiscardUiProcess(ui_pid_);
    ui_pid_ = 0;
}

int BufferService::UiClientCount() {
    std::lock_guard<std::recursive_mutex> lock(clients_mu_);
    return static_cast<int>(clients_.size());
}

void BufferService::StartSocket() {
    unlink(socket_path_.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        FCITX_LOGC(rimes_buffer_log, Error) << "buffer socket() failed";
        return;
    }
    SetCloexec(listen_fd_);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    std::strncpy(address.sun_path, socket_path_.c_str(), sizeof(address.sun_path) - 1);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listen_fd_, 8) != 0) {
        FCITX_LOGC(rimes_buffer_log, Error) << "buffer bind/listen failed: " << socket_path_;
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    chmod(socket_path_.c_str(), 0600);
    running_.store(true);
    socket_thread_ = std::thread([this] { SocketLoop(); });
}

void BufferService::StopSocket() {
    running_.store(false);
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        for (auto& client : clients_) {
            if (client.fd >= 0) {
                shutdown(client.fd, SHUT_RDWR);
            }
        }
    }
    if (socket_thread_.joinable()) {
        socket_thread_.join();
    }
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        for (auto& client : clients_) {
            if (client.fd >= 0) {
                close(client.fd);
            }
        }
        clients_.clear();
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    unlink(socket_path_.c_str());
}

void BufferService::SocketLoop() {
    while (running_.load()) {
        std::vector<pollfd> fds;
        {
            std::lock_guard<std::recursive_mutex> lock(clients_mu_);
            if (listen_fd_ >= 0) {
                fds.push_back(pollfd{listen_fd_, POLLIN, 0});
            }
            for (const auto& client : clients_) {
                fds.push_back(pollfd{client.fd, POLLIN, 0});
            }
        }
        if (fds.empty()) {
            break;
        }
        const int ready = poll(fds.data(), fds.size(), 250);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        if (fds[0].fd == listen_fd_ && (fds[0].revents & POLLIN) != 0) {
            AcceptClient();
        }
        std::vector<int> readable;
        {
            std::lock_guard<std::recursive_mutex> lock(clients_mu_);
            for (const auto& item : fds) {
                if (item.fd == listen_fd_) {
                    continue;
                }
                if ((item.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                    readable.push_back(item.fd);
                }
            }
        }
        for (int fd : readable) {
            std::lock_guard<std::recursive_mutex> lock(clients_mu_);
            for (auto& client : clients_) {
                if (client.fd == fd) {
                    ReadClient(&client);
                    break;
                }
            }
        }
    }
}

void BufferService::AcceptClient() {
    const int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
        return;
    }
    SetCloexec(fd);
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        clients_.push_back(Client{fd, {}});
    }
    post_([this] { Publish(); });
}

void BufferService::ReadClient(Client* client) {
    char chunk[4096];
    const auto got = read(client->fd, chunk, sizeof(chunk));
    if (got <= 0) {
        CloseClient(client->fd);
        return;
    }
    client->incoming.append(chunk, static_cast<std::size_t>(got));
    while (client->incoming.size() >= 4) {
        std::uint32_t length = 0;
        if (!rimes::buffer::DecodeFrameHeader(client->incoming.data(), &length)) {
            CloseClient(client->fd);
            return;
        }
        if (client->incoming.size() < 4 + length) {
            return;
        }
        const std::string payload = client->incoming.substr(4, length);
        client->incoming.erase(0, 4 + length);
        rimes::buffer::Command command;
        std::string error;
        if (!rimes::buffer::ParseCommand(payload, &command, &error)) {
            WriteAll(rimes::buffer::EncodeError("bad_command", error));
            continue;
        }
        post_([this, command] { HandleCommand(command); });
    }
}

void BufferService::CloseClient(int fd) {
    bool clients_empty = false;
    {
        std::lock_guard<std::recursive_mutex> lock(clients_mu_);
        for (auto iterator = clients_.begin(); iterator != clients_.end(); ++iterator) {
            if (iterator->fd == fd) {
                close(fd);
                clients_.erase(iterator);
                clients_empty = clients_.empty();
                break;
            }
        }
    }
    if (clients_empty && !headless_) {
        post_([this] { ScheduleUiRespawn(); });
    }
}

bool BufferService::HasUiClients() {
    std::lock_guard<std::recursive_mutex> lock(clients_mu_);
    return !clients_.empty();
}

void BufferService::ScheduleUiRespawn() {
    if (headless_ || !model_.visible()) {
        return;
    }
    CancelDerived();
    dropped_ui_pid_ = ui_pid_;
    ui_respawn_attempt_ = 0;
    ArmUiRespawnTimer(50000);
}

void BufferService::ArmUiRespawnTimer(int delay_us) {
    if (instance_ == nullptr) {
        return;
    }
    ui_respawn_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + delay_us, 0,
        [this](EventSourceTime*, uint64_t) {
            ReapUi();
            // Only replace the pid that dropped the socket. A child this
            // retry already started gets several seconds to connect.
            ReplaceDroppedUi();
            ++ui_respawn_attempt_;
            if (model_.visible()) {
                EnsureUi();
            }
            if (model_.visible() && !HasUiClients() && ui_respawn_attempt_ < 5) {
                const int delays[] = {2000000, 2000000, 2000000, 2000000};
                const int index = std::min(ui_respawn_attempt_ - 1, 3);
                ArmUiRespawnTimer(delays[index]);
            }
            return true;
        });
}

void BufferService::WriteAll(const std::string& payload) {
    std::string frame;
    if (!rimes::buffer::EncodeFrame(payload, &frame)) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(clients_mu_);
    for (auto iterator = clients_.begin(); iterator != clients_.end();) {
        const auto written = send(iterator->fd, frame.data(), frame.size(), MSG_NOSIGNAL);
        if (written != static_cast<ssize_t>(frame.size())) {
            close(iterator->fd);
            iterator = clients_.erase(iterator);
            continue;
        }
        ++iterator;
    }
}

void BufferService::Publish() {
    SynchronizeDerived();
    SynchronizeAutoSend();
    if (model_.visible()) {
        EnsureUi();
    }
    auto snapshot = rimes::buffer::MakeSnapshot(model_);
    snapshot.width = width_;
    snapshot.theme = theme_;
    snapshot.auto_send_seconds = auto_send_ ? auto_send_seconds_ : 0;
    if (auto_send_ && (!plugins_ || plugins_->SelectedBuffer().empty()) && model_.visible() &&
        model_.processing_active() && !model_.secure() && LiveTarget())
        for (const auto& block : snapshot.blocks)
            snapshot.auto_send_progress[block.id] = std::clamp(auto_send_clock_.Age(block.id) / auto_send_seconds_, 0.0, 1.0);
    snapshot.active_plugin = derived_.Plugin();
    const auto adapter = derived_adapters_.find(snapshot.active_plugin);
    snapshot.live_expand = adapter == derived_adapters_.end() || adapter->second.live_expand;
    snapshot.phase = derived_.Phase();
    snapshot.message = derived_error_;
    snapshot.can_generate = derived_.CanGenerate();
    snapshot.results = derived_.Results();
    snapshot.result_blocks = result_pending_;
    if (const auto* selected = derived_.SelectedResult()) snapshot.selected_result = selected->id;
    if (TranslationActive()) {
        const bool running = translation_.Running() || (translation_.HasPending() && !translation_.Paused() && translation_configuration_ &&
            (translation_configuration_->plugin.value("continuous", true) || translation_explicit_));
        snapshot.phase = translation_.Complete() ? rimes::buffer::DerivedPhase::Ready :
            translation_.Failed() || translation_source_invalid_ ? rimes::buffer::DerivedPhase::Failed : running ? rimes::buffer::DerivedPhase::Running : rimes::buffer::DerivedPhase::Idle;
        if (translation_source_invalid_) snapshot.message = "原文过长，请分段翻译";
        else if (translation_.Failed()) snapshot.message = "本地翻译未完成，请检查语言资源或重试";
        else if (translation_.Paused()) snapshot.message = "翻译已暂停";
        snapshot.can_generate = !translation_.All().empty() && !translation_.Running();
        snapshot.results.clear(); snapshot.selected_result.clear();
        snapshot.result_blocks.clear();
        auto append_blocks = [&](const auto& units) {
            for (const auto& unit : units) {
                if (unit.phase != rimes::translation::Phase::Ready) break;
                for (const auto& block : unit.children) {
                    if (snapshot.result_blocks.size() < 200)
                        snapshot.result_blocks.push_back({block.id, block.text, rimes::buffer::Origin::Local, 0});
                    else { snapshot.result_blocks.back().id = "overflow-" + translation_result_id_; snapshot.result_blocks.back().text += block.text; }
                }
            }
        };
        append_blocks(translation_.Retired()); append_blocks(translation_.All());
        const auto preview = translation_.Preview();
        if (!preview.empty()) {
            snapshot.results.push_back({translation_result_id_, preview, 0, translation_.Complete()});
            snapshot.selected_result = translation_result_id_;
        }
    }
    if (plugins_) for (const auto& state : plugins_->Entries()) {
        if (state.descriptor.kind == rimes::plugins::Kind::Buffer && state.installed && state.enabled &&
            derived_adapters_.count(state.descriptor.id))
            snapshot.plugins.push_back({state.descriptor.id, state.descriptor.name});
    }
    snapshot.can_deliver = !snapshot.active_plugin.empty() && CanDeliverDerived();
    if (delivery_conflict_) snapshot.message = "上屏状态未确认，请返回编辑后继续";
    snapshot.ui_clients = UiClientCount();
    const auto json = rimes::buffer::EncodeSnapshot(snapshot);
    WriteAll(json);
    if (dump_path_.empty()) {
        return;
    }
    const auto tmp = dump_path_ + ".tmp";
    FILE* file = std::fopen(tmp.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    std::fwrite(json.data(), 1, json.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
    std::rename(tmp.c_str(), dump_path_.c_str());
}

void BufferService::ArmHoldTimer() {
    if (instance_ == nullptr) {
        return;
    }
    hold_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + kHoldTickUs, kHoldTickUs,
        [this](EventSourceTime* source, uint64_t) {
            // Rearm explicitly only while this gesture is live. Retain the
            // disabled source until release instead of destroying its callback
            // from inside an aborted tick.
            source->setEnabled(false);
            if (model_.generation() != return_generation_ || !model_.preedit().empty()) {
                gesture_.cancel(); model_.set_hold_progress(0); Publish(); return true;
            }
            const auto now_tp = std::chrono::steady_clock::now();
            const auto action = gesture_.on_tick(now_tp);
            model_.set_hold_progress(gesture_.progress(now_tp));
            if (action == rimes::buffer::ReturnGesture::Action::SendAll) {
                Deliver(true);
                model_.set_hold_progress(0);
                Publish();
                return true;
            }
            if (gesture_.pending() && !gesture_.settle_only()) {
                source->setTime(now(CLOCK_MONOTONIC) + kHoldTickUs);
                source->setOneShot();
                Publish();
                return true;
            }
            return true;
        });
}

void BufferService::CancelHoldTimer() {
    hold_timer_.reset();
    model_.set_hold_progress(0);
}

}  // namespace fcitx
