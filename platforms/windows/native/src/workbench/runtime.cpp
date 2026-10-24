#include "ai_choices.hpp"
#include "runtime.hpp"
#include "local_translation.hpp"
#include "translation_languages.hpp"
#include "official_features.hpp"
#include "connector_ui.hpp"
#include "codex_cli.hpp"
#include "../core/buffer_keys.hpp"

#include <algorithm>
#include <chrono>
namespace rimes::windows::workbench {
using core::Json;
Runtime::Runtime(APIGenerator generate, TranslationResources resources,
                 std::function<std::uint64_t()> clock, std::function<std::uint32_t()> foreground,
                 ReadinessProbe readiness)
    : translation_resources_(std::move(resources)), generate_(std::move(generate)),
      readiness_probe_(std::move(readiness)), foreground_(std::move(foreground)), clock_(std::move(clock)) {
  // Warm ICU/NLS while the Broker starts, before the first commit enters its
  // bounded synchronous key-response path. Statistics remain best effort.
  try { (void)metrics::Characters("a"); } catch (...) {}
  using NativeGenerator = bool (*)(const Settings&, const Generation&,
      const std::function<bool(const std::string&)>&, const std::function<bool()>&, std::string*);
  const auto native = generate_.target<NativeGenerator>();
  // An injected generator is a complete transport fixture. Production always
  // probes its real connector off the UI/engine threads before enabling send.
  if (!readiness_probe_ && native && *native == GenerateText) {
    readiness_probe_ = [](const Settings& config, const std::function<bool()>& cancelled) -> Readiness {
      if (config.ai_connector == "codex-cli") {
        try {
          const auto exe = ResolveCodex(config.codex_path);
          std::string error;
          if (RunCodexControl(exe, {L"login", L"status"}, cancelled, &error)) return {true, {}};
          return {false, "请在设置 > 连接器中登录 ChatGPT。"};
        } catch (...) { return {false, "未找到 Codex CLI，请在设置 > 连接器中配置。"}; }
      }
      if (!ValidConnectionURL(config.base_url) || config.model.empty())
        return {false, "请在设置 > 连接器中配置地址和模型。"};
      ConnectionProfile profile{CredentialIdentity(config), {}, "openai-compatible", config.base_url, config.model, {}};
      if (!config.base_url.starts_with("http://") && !HasConnectionSecret(profile))
        return {false, "请在设置 > 连接器中填写此 Provider 的 API Key。"};
      return {true, {}};
    };
  }
  std::string error;
  settings_valid_ = LoadSettings(&settings_, &error);
  if (!settings_valid_) model_.status = error;
  remembered_buffer_plugin_ = plugins_.LoadBufferSelection();
  metrics_->Enable(!plugins_.Grant("builtin.statistics").empty(), !plugins_.Grant("builtin.typing-speed").empty());
  api_worker_ = std::jthread([this] { RunAPI(); });
  std::lock_guard lock(mutex_);
  PrepareTranslationResources();
}
Runtime::~Runtime() {
  Stop();
  readiness_worker_.request_stop();
  if (readiness_worker_.joinable()) readiness_worker_.join();
  if (api_worker_.joinable()) api_worker_.join();
  if (plugin_worker_.joinable()) plugin_worker_.join();
  if (translation_worker_.joinable()) translation_worker_.join();
}
void Runtime::PrepareTranslationResources() {
  if (stopping_ || !settings_valid_ || !translation_resources_.ready ||
      !translation_resources_.prepare || translation_ready_ || translation_preparing_ ||
      settings_.translation_provider != "local" ||
      plugins_.Grant(official::kTranslation).empty() || clock_() < translation_retry_at_)
    return;
  if (translation_resources_.ready()) {
    translation_ready_ = true;
    translation_status_ = "本地中英翻译资源已就绪";
    return;
  }
  translation_preparing_ = true;
  translation_status_ = "本地中英翻译资源正在后台下载（约 182 MiB）";
  translation_worker_ = std::jthread([this](std::stop_token stop) {
    std::string error;
    bool ok = false;
    try {
      ok = translation_resources_.prepare([stop] { return stop.stop_requested(); }, &error) &&
           translation_resources_.ready();
    } catch (...) { error = "本地翻译资源准备失败，将在后台重试。"; }
    std::lock_guard lock(mutex_);
    translation_preparing_ = false;
    translation_ready_ = ok;
    translation_retry_at_ = ok ? 0 : clock_() + 60000;
    translation_status_ = ok ? "本地中英翻译资源已就绪" : error;
    Changed();
  });
}
void Runtime::Changed() {
  auto_clock_.Synchronize(model_.source);
  if (notify_) notify_();
}
void Runtime::SetNotify(std::function<void()> notify) {
  std::lock_guard lock(mutex_);
  notify_ = std::move(notify);
}
bool Runtime::Stopping() {
  std::lock_guard lock(mutex_);
  return stopping_;
}
Target Runtime::Register(std::uint32_t process, std::uint64_t session,
                         std::uint64_t context) {
  std::lock_guard lock(mutex_);
  Target target{process, session, context};
  sessions_.emplace(session, Entry{target, {}});
  return target;
}
void Runtime::Remove(Target target) {
  std::lock_guard lock(mutex_);
  if (model_.live == target) ++peripheral_focus_revision_;
  const bool affects_capture = model_.live == target ||
      (model_.capture && model_.bound == target) || peripheral_target_ == target;
  if (peripheral_target_ == target) RevokePeripheral();
  model_.Revoke(target);
  sessions_.erase(target.session);
  // A retiring background session does not own the current host's held keys
  // or focus proof. In particular, do not reset them halfway through a cycle.
  if (affects_capture) CaptureChanged();
  event_.notify_all();
  if (affects_capture) Changed();
}
void Runtime::Focus(Target target) {
  std::lock_guard lock(mutex_);
  FocusLocked(target);
}
void Runtime::FocusLocked(Target target) {
  if (target.process == GetCurrentProcessId()) target = {};
  ObserveAppSwitchLocked(target.process);
  if (model_.live != target) ++peripheral_focus_revision_;
  if (model_.live == target && model_.capture) return;
  if (peripheral_target_ != target) RevokePeripheral();
  model_.Focus(target);
  CaptureChanged(true);
  Changed();
}
bool Runtime::Capturing(Target target) {
  std::lock_guard lock(mutex_);
  return model_.live == target &&
      ((peripheral_target_ && peripheral_target_ == target) ||
       (model_.capture && model_.bound == target));
}
bool Runtime::IsSendChord(const core::KeyEvent& key) {
  std::lock_guard lock(mutex_);
  if (peripheral_target_) return false;
  return core::IsBufferSendChord(key.virtual_key, key.modifiers,
                                  settings_.send_key, settings_.send_modifiers);
}
unsigned Runtime::InputRoute(Target target) {
  std::lock_guard lock(mutex_);
  if (model_.live != target) return 0;
  if (peripheral_target_ && peripheral_target_ == target) return peripheral_physical_ ? 3 : 2;
  return model_.capture && model_.bound == target ? 1 : 0;
}
void Runtime::CaptureChanged(bool follow_focus) {
  foreground_suspended_ = {};
  focus_request_.reset();
  if (!model_.visible) focus_follow_ = false;
  if (!focus_follow_ || model_.capture) {
    folded_ = false;
    fold_at_.reset();
  } else if (!folded_ && !fold_at_) {
    // Match macOS's focus-churn grace period; never postpone an existing fold.
    fold_at_ = clock_() + 600;
  }
  const auto focused = sessions_.find(model_.live.session);
  if (follow_focus && focus_follow_ && model_.visible && !model_.capture &&
      !peripheral_target_ && model_.live && focused != sessions_.end() &&
      focused->second.target == model_.live) {
    focus_request_ = FocusRequest{model_.live, ++next_focus_request_, clock_() + 1500};
  }
  ClearBufferTransitionLocked();
  model_.CancelSendAll();
  cycle_held_.reset(); cycle_target_ = {};
  // A focus/configuration/visibility transition cancels delivery, including
  // leaving and returning to the same target before the held key is released.
  return_sent_ = true;
  return_progress_ = false;
  auto_clock_.Pause(); auto_countdown_active_ = false;
  clipboard_action_ = 0;
  if (!model_.visible) {
    api_job_.reset(); translation_pending_ = false;
  }
  if (!model_.visible || !model_.capture) {
    translation_waiting_since_ = 0;
    speech_request_ = {};
    speech_after_delivery_ = false;
    if (model_.bound) translation_paused_ = true;
  }
  buffer_caret_ = {};
  for (auto& [id, entry] : sessions_) {
    (void)id;
    // Replace obsolete capture notifications but never replace a delivery.
    std::erase_if(entry.events, [](const Json& e) {
      return e.value("kind", "") == "capture";
    });
    entry.events.push_back(
        {{"kind", "capture"},
         {"context", entry.target.context},
         {"openingRevision", opening_revision_},
         {"focusRequest", focus_request_ && focus_request_->target == entry.target ? focus_request_->request : 0},
         {"focusFollow", focus_follow_ && model_.visible},
         {"font", settings_.font_size},
         {"candidateWidth", settings_.candidate_width}, {"candidateHeight", settings_.candidate_height},
         {"candidateLabelSize", settings_.candidate_label_size},
         {"verticalCandidates", settings_.vertical_candidates},
         {"theme", EffectiveTheme(settings_)},
         {"sendKey", settings_.send_key},
         {"sendModifiers", settings_.send_modifiers},
         {"previousPluginKey", settings_.previous_plugin_key},
         {"previousPluginModifiers", settings_.previous_plugin_modifiers},
         {"nextPluginKey", settings_.next_plugin_key},
         {"nextPluginModifiers", settings_.next_plugin_modifiers},
         {"inputRoute", peripheral_target_ == entry.target && peripheral_target_ ? (peripheral_physical_ ? 3 : 2) : 1},
         {"enabled", model_.live == entry.target &&
             ((peripheral_target_ && peripheral_target_ == entry.target) ||
              (model_.capture && model_.bound == entry.target))}});
  }
  event_.notify_all();
}
void Runtime::Queue(const std::optional<Delivery>& delivery) {
  if (!delivery) return;
  auto found = sessions_.find(delivery->target.session);
  if (found == sessions_.end()) {
    model_.LostAcknowledgement();
    return;
  }
  found->second.events.push_back({{"kind", "deliver"}, {"guard", true},
                                  {"request", delivery->request},
                                  {"context", delivery->target.context},
                                  {"text", delivery->block.text}});
  pending_since_ = clock_();
  delivery_opening_revision_ = opening_revision_;
  event_.notify_all();
  Changed();
}
int Runtime::PluginCycleDirection(const core::KeyEvent& key) {
  std::lock_guard lock(mutex_);
  return core::BufferPluginCycleDirection(key.virtual_key, key.modifiers,
      settings_.previous_plugin_key, settings_.previous_plugin_modifiers,
      settings_.next_plugin_key, settings_.next_plugin_modifiers);
}
bool Runtime::PluginShortcut(std::uint32_t foreground_process, unsigned key,
                             unsigned modifiers) {
  std::lock_guard lock(mutex_);
  // WM_HOTKEY does not carry a TSF context. Use only the existing exact lease;
  // never bind/rebind a target or foreground any window to process this action.
  if (!foreground_process || foreground_process != model_.live.process ||
      !model_.visible || !model_.capture || model_.bound != model_.live ||
      peripheral_target_ || !model_.preedit.empty() || model_.Pending()) return false;
  const int direction = core::BufferPluginCycleDirection(key, core::ShortcutModifiers(modifiers),
      settings_.previous_plugin_key, settings_.previous_plugin_modifiers,
      settings_.next_plugin_key, settings_.next_plugin_modifiers);
  return direction && CycleBufferPluginLocked(direction);
}
bool Runtime::BeforeKey(Target target, const core::KeyEvent& key,
                        bool composing) {
  std::lock_guard lock(mutex_);
  const bool down = (key.event_flags &
                     static_cast<unsigned>(core::KeyEventFlags::kKeyDown)) != 0;
  if (down && model_.visible && model_.capture && model_.bound == target && model_.live == target &&
      !ai_mode_ && !model_.translate && !peripheral_target_ && metrics::TypingKey(key.virtual_key, key.modifiers))
    live_metrics_.Key(clock_(), (key.event_flags & static_cast<unsigned>(core::KeyEventFlags::kRepeat)) != 0,
        key.virtual_key == VK_BACK || key.virtual_key == VK_DELETE);
  if (peripheral_target_ && peripheral_target_ == target && model_.live == target) {
    if (peripheral_delivery_) {
      if (key.virtual_key == VK_ESCAPE && down && peripheral_input_.size() < 256)
        peripheral_input_.push_back({{"key", VK_ESCAPE}, {"down", true}});
      Changed(); return true;
    }
    const auto modifiers = key.modifiers & ~static_cast<unsigned>(core::KeyModifiers::kCapsLock);
    const bool ctrl = modifiers == static_cast<unsigned>(core::KeyModifiers::kControl);
    const bool command = peripheral_physical_ || (!composing &&
        (key.virtual_key == VK_ESCAPE || key.virtual_key == VK_RETURN ||
         key.virtual_key == VK_BACK || key.virtual_key == VK_DELETE ||
         key.virtual_key == VK_LEFT || key.virtual_key == VK_RIGHT ||
         key.virtual_key == VK_UP || key.virtual_key == VK_DOWN ||
         key.virtual_key == VK_TAB || key.virtual_key == VK_HOME ||
         key.virtual_key == VK_END || (ctrl &&
         (key.virtual_key == 'A' || key.virtual_key == 'C' ||
          key.virtual_key == 'V' || key.virtual_key == 'F' || key.virtual_key == 'N'))));
    if (command) {
      if (peripheral_input_.size() < 256)
        peripheral_input_.push_back({{"key", key.virtual_key}, {"down", down},
            {"modifiers", key.modifiers}, {"physical", peripheral_physical_}});
      else { RevokePeripheral(); CaptureChanged(); }
      Changed();
      return true;
    }
    // Host shortcuts relinquish the lease before passing through. Plain
    // composing keys continue through the same per-client Rime session.
    if (down && core::HasHostCommandModifier(key.modifiers) &&
        key.virtual_key != VK_CONTROL && key.virtual_key != VK_LCONTROL && key.virtual_key != VK_RCONTROL &&
        key.virtual_key != VK_MENU && key.virtual_key != VK_LMENU && key.virtual_key != VK_RMENU &&
        key.virtual_key != VK_LWIN && key.virtual_key != VK_RWIN) {
      RevokePeripheral(); CaptureChanged(); Changed();
    }
    return false;
  }
  if (key.virtual_key < cycle_held_.size() && cycle_target_ == target && cycle_held_.test(key.virtual_key)) {
    if (!down) cycle_held_.reset(key.virtual_key);
    return true;
  }
  const int direction = core::BufferPluginCycleDirection(key.virtual_key, key.modifiers,
      settings_.previous_plugin_key, settings_.previous_plugin_modifiers,
      settings_.next_plugin_key, settings_.next_plugin_modifiers);
  if (model_.visible && model_.capture && model_.bound == target && model_.live == target && direction) {
    if (down && !(key.event_flags & static_cast<unsigned>(core::KeyEventFlags::kRepeat))) {
      cycle_target_ = target; cycle_held_.set(key.virtual_key);
      if (!composing && !model_.Pending()) {
        CycleBufferPluginLocked(direction);
      }
    }
    return true;
  }
  if (return_held_ && key.virtual_key == return_key_ &&
      return_target_ == target) {
    if (!down) {
      if (!return_sent_ && model_.capture && model_.bound == target)
        Queue(SendAuthorized(clock_() - pressed_at_ >= settings_.send_hold_ms));
      return_held_ = false;
      return_progress_ = false;
      Changed();
    }
    return true;
  }
  const bool send = core::IsBufferSendChord(key.virtual_key, key.modifiers,
                                            settings_.send_key, settings_.send_modifiers);
  const auto modifiers = key.modifiers & ~static_cast<unsigned>(core::KeyModifiers::kCapsLock);
  if (model_.capture && model_.bound == target && model_.live == target &&
      modifiers == static_cast<unsigned>(core::KeyModifiers::kControl) &&
      (key.virtual_key == 'A' || key.virtual_key == 'V' || key.virtual_key == 'C')) {
    if (down && !composing) {
      if (key.virtual_key == 'A') model_.SelectAll();
      else { clipboard_action_ = key.virtual_key; clipboard_target_ = target; }
      Changed();
    }
    return true;
  }
  if (core::HasHostCommandModifier(key.modifiers) && !send) return false;
  if (!model_.capture || model_.bound != target || model_.live != target)
    return false;
  if (!composing && !core::HasHostCommandModifier(key.modifiers) &&
      (key.virtual_key == VK_LEFT || key.virtual_key == VK_RIGHT ||
       key.virtual_key == VK_HOME || key.virtual_key == VK_END || key.virtual_key == VK_DELETE)) {
    if (down) {
      if (key.virtual_key == VK_DELETE) model_.DeleteForward();
      else if (key.virtual_key == VK_HOME) model_.SetInsertion(0);
      else if (key.virtual_key == VK_END) model_.SetInsertion(model_.source.size());
      else model_.MoveInsertion(key.virtual_key == VK_LEFT ? -1 : 1);
      Changed();
    }
    return true;
  }
  if (!send && key.virtual_key != VK_ESCAPE && key.virtual_key != VK_RETURN &&
      key.virtual_key != VK_BACK &&
      (model_.SourceText().size() > Model::kLimit - core::kMaxCommitTextBytes ||
       (model_.Pending() && !model_.result.empty()))) {
    model_.status =
        "Buffer is full or a result is being delivered. Input paused.";
    Changed();
    return true;
  }
  if (!send && key.virtual_key == VK_ESCAPE) {
    if (down && composing) {
      model_.status = "请先完成当前组字。"; Changed(); return true;
    }
    if (down) {
      model_.Close();
      CaptureChanged();
      Changed();
    }
    return true;
  }
  if (!send && key.virtual_key == VK_BACK && !composing) {
    if (down) {
      model_.Backspace();
      edited_at_ = clock_();
      Changed();
    }
    return true;
  }
  if (send) {
    if (down && !(key.event_flags &
                  static_cast<unsigned>(core::KeyEventFlags::kRepeat))) {
      return_held_ = true;
      return_key_ = key.virtual_key;
      return_sent_ = composing;
      return_progress_ = false;
      pressed_at_ = clock_();
      return_target_ = target;
      // Settle composition through Rime, then suppress repeats/release for
      // this physical press even when the configured key is not Enter.
      if (composing) return false;
      if ((ai_mode_ || model_.translate) && (model_.busy || model_.result.empty())) {
        // Generation owns this entire press. Never send the retained source
        // on key-up/hold, even if the connector finishes during that press.
        return_sent_ = true;
        if (!model_.busy) StartGeneration(model_.translate);
      }
      return_progress_ = !return_sent_ && !model_.Pending() && !model_.uncertain &&
          (!model_.source.empty() || !model_.result.empty());
      Changed();
    }
    return true;
  }
  // Ordinary Return only settles composition. It cannot send or submit the
  // host field while Buffer owns input; sending uses the configured chord.
  if (key.virtual_key == VK_RETURN && !composing) return true;
  return false;
}
void Runtime::Capture(Target target, engine::EngineSnapshot* snapshot,
                      bool direct_input) {
  std::lock_guard lock(mutex_);
  CaptureLocked(target, snapshot, direct_input);
}
void Runtime::CaptureLocked(Target target, engine::EngineSnapshot* snapshot, bool direct_input) {
  if (peripheral_target_ && peripheral_target_ == target && model_.live == target) {
    peripheral_preedit_ = snapshot->composition;
    if (!snapshot->commit_text.empty()) {
      if (peripheral_input_.size() < 256)
        peripheral_input_.push_back({{"text", snapshot->commit_text}});
      snapshot->commit_text.clear();
    }
    Changed();
    return;
  }
  if (!model_.capture || model_.bound != target || model_.live != target)
    return;
  if (ai_mode_ && (!snapshot->composition.empty() || !snapshot->commit_text.empty()) &&
      (model_.busy || !model_.result.empty())) model_.ReturnToEdit();
  model_.preedit = snapshot->composition;
  preedit_caret_ = snapshot->caret_utf16;
  if (!snapshot->commit_text.empty()) {
    if (!model_.Append(snapshot->commit_text,
                       direct_input ? InputKind::kDirect : InputKind::kCommit))
      throw std::runtime_error("Buffer capacity invariant");
    if (!ai_mode_ && !model_.translate)
      live_metrics_.Commit(clock_(), static_cast<unsigned>(metrics::Characters(snapshot->commit_text).size()));
    snapshot->commit_text.clear();
    edited_at_ = clock_();
  }
  Changed();
}
void Runtime::ClearBufferTransitionLocked() {
  buffer_transition_.reset();
  for (auto& [id, entry] : sessions_) {
    (void)id;
    std::erase_if(entry.events, [](const Json& e) { return e.value("kind", "") == "buffer_transition"; });
  }
}
bool Runtime::QueueBufferTransitionLocked(bool close, const std::string& plugin) {
  ClearBufferTransitionLocked();
  const auto found = sessions_.find(model_.bound.session);
  if (stopping_ || !model_.visible || !model_.capture || model_.bound != model_.live ||
      peripheral_target_ || model_.Pending() || found == sessions_.end() || found->second.target != model_.bound ||
      model_.SourceText().size() > Model::kLimit - 2 * core::kMaxCommitTextBytes) {
    model_.status = "请先完成当前组字。"; Changed(); return false;
  }
  buffer_transition_ = BufferTransition{model_.bound, ++next_transition_, opening_revision_,
      model_.revision, clock_() + 1500, model_.preedit, plugin, close};
  found->second.events.push_back({{"kind", "buffer_transition"}, {"context", model_.bound.context},
      {"request", next_transition_}, {"openingRevision", opening_revision_}, {"close", close}});
  model_.status = "正在确认当前组字…";
  event_.notify_all(); Changed(); return true;
}
bool Runtime::CompleteBufferTransition(Target target, std::uint64_t request,
    const std::function<bool(engine::EngineSnapshot*)>& settle) {
  std::lock_guard lock(mutex_);
  if (!buffer_transition_ || !request || buffer_transition_->request != request ||
      buffer_transition_->target != target) return false;
  const auto action = *buffer_transition_;
  ClearBufferTransitionLocked();
  if (stopping_ || !settle || clock_() > action.deadline || !model_.visible || !model_.capture ||
      model_.bound != target || model_.live != target || peripheral_target_ || model_.Pending() ||
      action.opening != opening_revision_ || action.revision != model_.revision || action.preedit != model_.preedit ||
      model_.SourceText().size() > Model::kLimit - 2 * core::kMaxCommitTextBytes) return false;
  // Revalidate plugin enablement before resolving composition, not after it.
  if (!action.close) {
    const auto entries = BufferPluginsLocked();
    if (std::none_of(entries.begin(), entries.end(), [&](const auto& p) { return p.id == action.plugin; })) return false;
  }
  engine::EngineSnapshot snapshot;
  const bool settled = settle(&snapshot);
  // Preserve commits even if the second engine operation failed after flushing
  // a chord. Never stage the raw spelling as a substitute for Rime's commit.
  if (settled || !snapshot.commit_text.empty()) CaptureLocked(target, &snapshot, false);
  if (!settled || snapshot.composing || !model_.preedit.empty()) {
    model_.status = "组字尚未完成，请选择候选后重试。"; Changed(); return false;
  }
  if (action.close) model_.Close();
  else if (!SelectBufferPluginLocked(action.plugin)) return false;
  CaptureChanged(); Changed(); return true;
}
Json Runtime::Control(const Json& message, std::uint32_t process) {
  std::unique_lock lock(mutex_);
  const auto id = message.value("session", 0ULL);
  auto found = sessions_.find(id);
  if (found == sessions_.end() || found->second.target.process != process)
    return {{"kind", "disconnected"}};
  const auto target = found->second.target;
  const auto op = message.value("op", "");
  if (op == "delivery_guard") {
    const auto request = message.value("request", 0ULL);
    const bool peripheral = request && peripheral_delivery_ == request && peripheral_target_ == target && model_.live == target;
    const bool buffer = model_.visible && model_.capture && model_.bound == target && model_.live == target &&
        delivery_opening_revision_ == opening_revision_ && model_.OwnsDelivery(request, target);
    return {{"kind", peripheral || buffer ? "ok" : "stale"}};
  }
  if (op == "geometry") {
    // Geometry is presentation only, and only an answer to this explicit
    // opening from its authenticated exact session can reposition the panel.
    const auto rect = message.find("caret");
    if (!opening_at_ || clock_() - opening_at_ > 1000 || opening_caret_ ||
        !model_.visible || !model_.capture || model_.bound != target || model_.live != target ||
        model_.revision != opening_source_revision_ || !model_.preedit.empty() ||
        message.value("context", 0ULL) != target.context ||
        message.value("openingRevision", 0ULL) != opening_revision_ ||
        rect == message.end() || !rect->is_array() || rect->size() != 4 ||
        !std::all_of(rect->begin(), rect->end(), [](const Json& n) { return n.is_number(); }))
      return {{"kind", "stale"}};
    BufferRect caret{(*rect)[0].get<double>(), (*rect)[1].get<double>(),
                     (*rect)[2].get<double>(), (*rect)[3].get<double>()};
    if (!FiniteBufferRect(caret) || std::abs(caret.x) > 1000000 ||
        std::abs(caret.y) > 1000000 || caret.width < 0 || caret.width > 2000 ||
        caret.height <= 2 || caret.height >= 600) return {{"kind", "error"}};
    opening_caret_ = caret;
    ++placement_revision_;
    Changed();
    return {{"kind", "ok"}};
  }
  if (op == "focus") {
    // TSF can report delayed document focus from a background window (or our
    // own popup thread). That is not evidence that the user's host changed.
    // A real foreground departure is revoked by the host or ObserveForeground.
    if (target.process == GetCurrentProcessId() || target.process != foreground_())
      return {{"kind", "stale"}};
    FocusLocked(target);
    return {{"kind", "ok"}};
  }
  if (op == "focus_ready") {
    // A focus notification alone cannot grant capture. The exact TSF session
    // must answer this challenge after a granted read lock has verified an
    // editable selection and a fresh caret in the current foreground host.
    const auto rect = message.find("caret");
    if (!focus_request_ || focus_request_->target != target ||
        focus_request_->request != message.value("request", 0ULL) ||
        clock_() > focus_request_->deadline || !focus_follow_ || !model_.visible ||
        model_.capture || peripheral_target_ || model_.live != target ||
        target.process == GetCurrentProcessId() || foreground_() != target.process ||
        message.value("context", 0ULL) != target.context ||
        rect == message.end() || !rect->is_array() || rect->size() != 4 ||
        !std::all_of(rect->begin(), rect->end(), [](const Json& n) { return n.is_number(); }))
      return {{"kind", "stale"}};
    const BufferRect caret{(*rect)[0].get<double>(), (*rect)[1].get<double>(),
                           (*rect)[2].get<double>(), (*rect)[3].get<double>()};
    if (!FiniteBufferRect(caret) || std::abs(caret.x) > 1000000 ||
        std::abs(caret.y) > 1000000 || caret.width < 0 || caret.width > 2000 ||
        caret.height <= 2 || caret.height >= 600) return {{"kind", "stale"}};
    BindLocked(target.process);
    opening_caret_ = caret;
    ++placement_revision_;
    CaptureChanged();
    Changed();
    return {{"kind", "ok"}};
  }
  if (op == "ack") {
    if (peripheral_delivery_ && peripheral_delivery_ == message.value("request", 0ULL) && peripheral_target_ == target) {
      peripheral_input_.push_back({{"delivered", message.value("accepted", false)}});
      peripheral_delivery_ = 0;
      Changed();
      return {{"kind", "ok"}};
    }
    CheckPluginAuthorization();
    const bool was_pending = model_.Pending();
    const bool same_opening = delivery_opening_revision_ == opening_revision_ &&
        model_.visible && model_.capture && model_.bound == target && model_.live == target;
    const auto spoken = !model_.result.empty() ? model_.result.front() : Block{};
    Queue(model_.Acknowledge(message.value("request", 0ULL), target,
                             message.value("accepted", false)));
    const bool speak_delivered = was_pending && same_opening && spoken.id && model_.translate &&
        settings_.translation_speech && message.value("accepted", false) &&
        std::none_of(model_.result.begin(), model_.result.end(),
                     [&](const Block& block) { return block.id == spoken.id; });
    CheckPluginAuthorization();
    if (was_pending && same_opening && settings_.close_after_send && !model_.Pending() && !model_.busy &&
        model_.source.empty() && model_.result.empty() && model_.preedit.empty()) {
      model_.Close(); CaptureChanged();
    }
    if (speak_delivered) {
      speech_request_ = {{"text", spoken.text}, {"language", spoken.language}};
      speech_after_delivery_ = !model_.visible;
    }
    Changed();
    return {{"kind", "ok"}};
  }
  if (op == "wait") {
    event_.wait_for(lock, std::chrono::seconds(10), [&] {
      auto i = sessions_.find(id);
      return stopping_ || i == sessions_.end() || !i->second.events.empty();
    });
    found = sessions_.find(id);
    if (stopping_ || found == sessions_.end())
      return {{"kind", "disconnected"}};
    if (found->second.events.empty()) return {{"kind", "heartbeat"}};
    Json value = std::move(found->second.events.front());
    found->second.events.pop_front();
    return value;
  }
  return {{"kind", "error"}};
}
Json Runtime::Snapshot() {
  std::lock_guard lock(mutex_);
  if (model_.visible && (ai_mode_ || model_.translate)) RefreshReadinessLocked(model_.translate);
  const auto readiness = GenerationReadinessLocked(model_.translate);
  Json source = Json::array(), result = Json::array(), streaming = Json::array();
  for (const auto& b : model_.source)
    source.push_back({{"id", b.id}, {"text", b.text}, {"age_ms", auto_clock_.Age(b.id)}});
  for (const auto& b : model_.result)
    result.push_back({{"id", b.id}, {"text", b.text}});
  for (const auto& b : model_.streaming) streaming.push_back({{"id", b.id}, {"text", b.text}});
  return {{"generation_ready", readiness.ready}, {"generation_unavailable", readiness.message},
          {"waiting_seconds", model_.busy ? (clock_() - generation_started_) / 1000 : 0},
          {"stream_blocks", streaming}, {"reasoning_summary", model_.reasoning_summary},
          {"source_blocks", source},
          {"result_blocks", result},
          {"visible", model_.visible},
          {"folded", folded_},
          {"capture", model_.capture},
          {"busy", model_.busy},
          {"uncertain", model_.uncertain},
          {"source", model_.SourceText()},
          {"result", model_.ResultText()},
          {"preview", model_.preview},
          {"preedit", model_.preedit},
          {"preedit_caret", preedit_caret_},
          {"insertion", model_.insertion},
          {"selected", model_.selected},
          {"revision", model_.revision},
          {"generation", model_.GenerationID()},
          {"pending", model_.Pending()},
          {"complete_result", model_.CompleteResult()},
          {"send_hold_progress", return_progress_ && return_held_ && model_.visible &&
              model_.capture && model_.bound == return_target_ && model_.live == return_target_ &&
              !model_.uncertain && model_.preedit.empty()
              ? (std::min)(1.0, static_cast<double>(clock_() - pressed_at_) /
                  (std::max)(1U, settings_.send_hold_ms)) : -1.0},
          {"mode", ai_mode_ ? 1 : model_.translate ? 2 : 0},
          {"plugin", SelectedBufferPluginLocked()},
          {"auto_send_seconds", settings_.auto_send_seconds},
          {"close_after_send", settings_.close_after_send},
          {"chatgpt_model", settings_.chatgpt_model}, {"chatgpt_effort", settings_.chatgpt_effort},
          {"source_language", settings_.source_language},
          {"target_language", settings_.target_language},
          {"translation_speech", settings_.translation_speech},
          {"speech_allowed", model_.visible || speech_after_delivery_},
          {"translation_controls_enabled", !model_.Pending()},
          {"live_metrics", live_metrics_.Line()},
          {"auto_remaining_ms", auto_countdown_active_ && !model_.source.empty()
              ? static_cast<long long>(settings_.auto_send_seconds * 1000ULL -
                  (std::min)(settings_.auto_send_seconds * 1000ULL, auto_clock_.Age(model_.source.front().id))) : 0LL},
          {"status", model_.status},
          {"translate", model_.translate},
          {"opening_revision", opening_revision_},
          {"placement_revision", placement_revision_},
          {"target_pid", model_.capture && model_.bound == model_.live
                             ? model_.bound.process : 0}};
}
Target Runtime::PlacementTarget() {
  std::lock_guard lock(mutex_);
  const auto found = sessions_.find(model_.live.session);
  if (!model_.visible || !model_.capture || model_.bound != model_.live ||
      found == sessions_.end() || found->second.target != model_.bound) return {};
  return model_.bound;
}
std::optional<BufferRect> Runtime::PlacementCaret(Target target) {
  std::lock_guard lock(mutex_);
  if (!target || !model_.capture || !model_.visible || model_.bound != target ||
      model_.live != target || model_.revision != opening_source_revision_ ||
      !model_.preedit.empty()) return {};
  return opening_caret_;
}
void Runtime::CancelOpeningPlacement() {
  std::lock_guard lock(mutex_);
  opening_at_ = 0;
  opening_caret_.reset();
}
void Runtime::SetImportedSchema(std::string schema, std::string package) {
  std::lock_guard lock(mutex_); imported_schema_ = std::move(schema); imported_package_ = std::move(package);
}
std::uint64_t Runtime::ConfigurationRevision() { std::lock_guard lock(mutex_); return settings_.revision; }
void Runtime::AppearanceChanged() { std::lock_guard lock(mutex_); CaptureChanged(); Changed(); }
Settings Runtime::Configuration() {
  std::lock_guard lock(mutex_);
  auto effective = settings_;
  if (effective.schema == official::kChordSchema && plugins_.Grant(official::kChord).empty()) effective.schema = "rime_ice";
  if (!imported_schema_.empty()) { effective.schema = imported_schema_; effective.imported_scheme = true; effective.input_scope = imported_package_; }
  return effective;
}
bool Runtime::Configure(Settings value, const std::wstring& key,
                        bool replace_key, std::string* error, const std::map<std::string, std::wstring>& secrets) {
  std::lock_guard lock(mutex_);
  if (model_.Pending()) {
    if (error) *error = "正在等待发送确认，请稍后保存设置。";
    return false;
  }
  if (!settings_valid_) {
    if (error)
      *error =
          "Settings file is unreadable. Preserve it and resolve before saving.";
    return false;
  }
  if (!imported_schema_.empty()) value.schema = settings_.schema;
  MigrateConnections(&value);
  if (!SelectConnection(&value, value.active_connection)) {
    if (error) *error = "连接器配置无效。";
    return false;
  }
  value.revision = settings_.revision + 1;
  if (!SaveSettings(value, error)) return false;
  auto updates = secrets;
  if (replace_key) updates[CredentialIdentity(value)] = key;
  auto credential_profiles = value;
  // Removed providers retire their endpoint-bound credentials in the same
  // rollback-capable transaction as other credential updates.
  for (const auto& prior : settings_.connections) {
    if (prior.kind == "openai-compatible" && std::none_of(value.connections.begin(), value.connections.end(),
        [&](const auto& current) { return current.id == prior.id; })) {
      credential_profiles.connections.push_back(prior); updates[prior.id] = L"";
    }
  }
  const bool secrets_saved = SaveConnectionSecrets(credential_profiles, updates, error);
  for (auto& [id, secret] : updates) if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
  if (!secrets_saved) {
    if (!SaveSettings(settings_, nullptr)) {
      settings_valid_ = false;
      model_.Cancel();
    }
    return false;
  }
  settings_ = std::move(value);
  readiness_worker_.request_stop();
  readiness_cache_.clear();
  RevokePeripheral();
  PrepareTranslationResources();
  if (model_.translate) {
    model_.ResetTranslation();
    failed_translation_revision_ = 0;
  } else {
    model_.InvalidatePluginResults();
    result_plugin_.clear(); result_grant_.clear();
    result_connector_.clear(); result_connector_grant_.clear();
  }
  model_.capture = false;
  CaptureChanged();
  Changed();
  return true;
}
void Runtime::BindLocked(std::uint32_t foreground_process) {
  RevokePeripheral();
  // A tray/menu or delayed TSF focus notification is not authority to capture
  // input from a background process. Never revive the previous bound target.
  const auto found = sessions_.find(model_.live.session);
  if (!foreground_process || foreground_process == GetCurrentProcessId() ||
      model_.live.process != foreground_process || found == sessions_.end() ||
      found->second.target != model_.live)
    model_.Focus({});
  const bool reopening = !model_.visible;
  model_.Open();
  focus_follow_ = true;
  if (reopening) {
    live_metrics_.Reset();
    // Restore only a still-enabled Buffer plugin, and never grant focus or
    // resurrect transient text/results merely because a preference exists.
    const auto preferred = remembered_buffer_plugin_;
    if (!SelectBufferPluginLocked(preferred) && !preferred.empty())
      SelectBufferPluginLocked("");
  }
  translation_paused_ = false;
  translation_waiting_since_ = 0;
  ++opening_revision_;
  opening_caret_.reset();
  opening_at_ = clock_();
  opening_source_revision_ = model_.revision;
}
void Runtime::Bind(std::uint32_t foreground_process) {
  std::lock_guard lock(mutex_);
  BindLocked(foreground_process);
  CaptureChanged();
  Changed();
}
void Runtime::Toggle(std::uint32_t foreground_process) {
  std::lock_guard lock(mutex_);
  if (model_.visible) {
    if (!model_.preedit.empty()) { QueueBufferTransitionLocked(true); return; }
    model_.Close();
  } else
    BindLocked(foreground_process);
  CaptureChanged();
  Changed();
}
void Runtime::PauseCapture() {
  std::lock_guard lock(mutex_);
  foreground_suspended_ = {};
  focus_request_.reset();
  if (peripheral_target_) { RevokePeripheral(); CaptureChanged(); Changed(); }
  translation_paused_ = true;
  translation_waiting_since_ = 0;
  if (!model_.capture) return;
  model_.capture = false;
  model_.preedit.clear();
  CaptureChanged();
  Changed();
}
void Runtime::ObserveAppSwitchLocked(std::uint32_t process) {
  if (!process || process == GetCurrentProcessId()) return;
  const auto previous = last_external_process_;
  last_external_process_ = process;
  if (!settings_.reset_on_app_switch || !previous || previous == process ||
      std::any_of(model_.source.begin(), model_.source.end(), [](const auto& block) { return block.external; })) return;
  const bool translation = model_.translate;
  DiscardBufferLocked();
  model_.translate = translation;
  Changed();
}
void Runtime::ObserveForeground(std::uint32_t process) {
  std::lock_guard lock(mutex_);
  ObserveAppSwitchLocked(process);
  if (model_.live && model_.live.process != process) ++peripheral_focus_revision_;
  if (model_.capture && model_.bound == model_.live && model_.bound.process != process) {
    const auto target = model_.bound;
    model_.capture = false;
    model_.preedit.clear();
    CaptureChanged();
    // Keep only a resumption intent. Even a transient null foreground revokes
    // delivery immediately; returning to this PID cannot itself grant capture.
    foreground_suspended_ = target;
    Changed();
  } else if (foreground_suspended_ && foreground_suspended_.process == process &&
             foreground_suspended_ == model_.live) {
    // Menus/window transitions need not produce another TSF SetFocus for an
    // unchanged document. Ask that exact session for a fresh editable caret.
    CaptureChanged(true);
    Changed();
  }
}
void Runtime::RevokePeripheral() {
  if (peripheral_delivery_) {
    auto found = sessions_.find(peripheral_target_.session);
    if (found != sessions_.end()) std::erase_if(found->second.events, [&](const Json& e) {
      return e.value("kind", "") == "deliver" && e.value("request", 0ULL) == peripheral_delivery_;
    });
  }
  peripheral_delivery_ = 0;
  peripheral_target_ = {};
  peripheral_physical_ = false;
  peripheral_preedit_.clear();
  peripheral_input_ = Json::array();
}
bool Runtime::BorrowPeripheral(std::uint32_t foreground_process) {
  std::lock_guard lock(mutex_);
  RevokePeripheral();
  const auto found = sessions_.find(model_.live.session);
  if (!foreground_process || foreground_process == GetCurrentProcessId() ||
      model_.live.process != foreground_process || found == sessions_.end() ||
      found->second.target != model_.live || model_.Pending() ||
      !model_.preedit.empty()) { CaptureChanged(); return false; }
  peripheral_target_ = model_.live;
  model_.capture = false;
  translation_paused_ = true;
  ++opening_revision_;
  CaptureChanged();
  Changed();
  return true;
}
void Runtime::ReleasePeripheral() {
  std::lock_guard lock(mutex_);
  if (!peripheral_target_) return;
  RevokePeripheral();
  CaptureChanged();
  Changed();
}
void Runtime::PeripheralPhysical(bool enabled) {
  std::lock_guard lock(mutex_);
  const bool physical = enabled && static_cast<bool>(peripheral_target_);
  if (physical == peripheral_physical_) return;
  peripheral_physical_ = physical;
  peripheral_preedit_.clear();
  peripheral_input_ = Json::array();
  CaptureChanged();
  Changed();
}
Json Runtime::TakePeripheralInput() {
  std::lock_guard lock(mutex_);
  Json result{{"active", static_cast<bool>(peripheral_target_)},
      {"process", peripheral_target_.process}, {"preedit", peripheral_preedit_},
      {"events", std::move(peripheral_input_)}};
  peripheral_input_ = Json::array();
  return result;
}
void Runtime::PublishPeripheralCaret(const RECT& caret) {
  std::lock_guard lock(mutex_);
  if (!peripheral_target_ || peripheral_target_ != model_.live ||
      caret.bottom <= caret.top || EqualRect(&caret, &buffer_caret_)) return;
  auto found = sessions_.find(peripheral_target_.session);
  if (found == sessions_.end()) return;
  buffer_caret_ = caret;
  std::erase_if(found->second.events, [](const Json& e) { return e.value("kind", "") == "buffer_caret"; });
  found->second.events.push_back({{"kind", "buffer_caret"},
      {"context", peripheral_target_.context}, {"openingRevision", opening_revision_},
      {"rect", {caret.left, caret.top, caret.right, caret.bottom}}});
  event_.notify_all();
}
bool Runtime::SendPeripheral(const std::string& text) {
  std::lock_guard lock(mutex_);
  if (!peripheral_target_ || peripheral_target_ != model_.live || peripheral_delivery_ ||
      !peripheral_preedit_.empty() || text.empty() || text.size() > core::kMaxCommitTextBytes || model_.Pending()) return false;
  auto found = sessions_.find(peripheral_target_.session);
  if (found == sessions_.end()) return false;
  peripheral_delivery_ = model_.ReserveDeliveryRequest();
  found->second.events.push_back({{"kind", "deliver"}, {"guard", true}, {"request", peripheral_delivery_},
      {"context", peripheral_target_.context}, {"text", text}});
  event_.notify_all();
  return true;
}
Runtime::PeripheralPasteTicket Runtime::PeripheralPasteTarget() {
  std::lock_guard lock(mutex_);
  if (!peripheral_target_ || peripheral_target_ != model_.live || peripheral_delivery_ ||
      !peripheral_preedit_.empty() || !model_.preedit.empty() || model_.Pending()) return {};
  return {peripheral_target_, peripheral_focus_revision_};
}
bool Runtime::CanPastePeripheral(PeripheralPasteTicket ticket) {
  std::lock_guard lock(mutex_);
  const auto target = ticket.target;
  const auto found = sessions_.find(target.session);
  return target && ticket.focus_revision == peripheral_focus_revision_ &&
      target == model_.live && found != sessions_.end() &&
      found->second.target == target && !peripheral_target_ && !model_.capture &&
      model_.preedit.empty() && !model_.Pending();
}
void Runtime::Close() {
  std::lock_guard lock(mutex_);
  if (!model_.preedit.empty()) { QueueBufferTransitionLocked(true); return; }
  model_.Close();
  CaptureChanged();
  Changed();
}
void Runtime::Protect() {
  std::lock_guard lock(mutex_);
  ++peripheral_focus_revision_;
  RevokePeripheral();
  model_.Protect();
  CaptureChanged();
  Changed();
}
void Runtime::DiscardBufferLocked() {
  RevokePeripheral();
  api_job_.reset();
  model_.Discard();
  saved_plugin_results_.clear(); live_metrics_.Reset(); auto_clock_.Reset();
  for (auto& [id, entry] : sessions_) { (void)id; entry.events.clear(); }
  return_held_ = false; return_sent_ = false; return_target_ = {};
  pressed_at_ = 0; pending_since_ = 0;
  result_plugin_.clear(); result_grant_.clear();
  result_connector_.clear(); result_connector_grant_.clear();
  CaptureChanged();
}
void Runtime::DiscardBuffer() {
  std::lock_guard lock(mutex_);
  DiscardBufferLocked();
  Changed();
}
void Runtime::Paste(std::string text) {
  std::lock_guard lock(mutex_);
  if (model_.visible) {
    if (!model_.Append(std::move(text), InputKind::kExternal))
      model_.status = "Paste exceeds Buffer capacity or delivery is pending.";
    edited_at_ = clock_();
    Changed();
  }
}
bool Runtime::StageExternal(std::string text) {
  std::lock_guard lock(mutex_);
  if (text.empty() || model_.Pending() || !model_.preedit.empty() || model_.uncertain ||
      model_.SourceText().size() + text.size() > Model::kLimit) return false;
  RevokePeripheral();
  model_.selected = false;
  model_.SetInsertion(model_.source.size());
  if (!model_.Append(std::move(text), InputKind::kExternal)) return false;
  model_.capture = false;
  model_.bound = {};
  model_.visible = true;
  focus_follow_ = false; // Peripheral review still requires an explicit bind.
  translation_paused_ = true;
  edited_at_ = clock_();
  model_.status = "已加入暂存。请回到目标输入框并绑定 Buffer 后发送。";
  CaptureChanged();
  Changed();
  return true;
}
void Runtime::Send(bool all) {
  std::lock_guard lock(mutex_);
  Queue(SendAuthorized(all));
  Changed();
}
bool Runtime::ShortcutDown(std::uint32_t foreground_process) {
  Target target;
  core::KeyEvent key;
  {
    std::lock_guard lock(mutex_);
    if (!foreground_process || foreground_process != model_.live.process ||
        !model_.capture || model_.bound != model_.live || !model_.preedit.empty()) return false;
    target = model_.live;
    key.virtual_key = settings_.send_key;
    key.modifiers = core::ShortcutModifiers(settings_.send_modifiers);
    key.event_flags = static_cast<unsigned>(core::KeyEventFlags::kKeyDown);
  }
  return BeforeKey(target, key, false);
}
void Runtime::ShortcutUp() {
  Target target;
  core::KeyEvent key;
  {
    std::lock_guard lock(mutex_);
    if (!return_held_) return;
    target = return_target_; key.virtual_key = return_key_;
  }
  BeforeKey(target, key, false);
}
void Runtime::SetInsertion(std::size_t index) {
  std::lock_guard lock(mutex_);
  if (model_.preedit.empty() && model_.capture) {
    model_.SetInsertion(index); Changed();
  }
}
void Runtime::ReturnToEdit() {
  std::lock_guard lock(mutex_);
  model_.ReturnToEdit(); auto_clock_.Reset(); Changed();
}
void Runtime::SetAutoSend(unsigned seconds, bool close_after) {
  std::lock_guard lock(mutex_);
  auto next = settings_;
  next.auto_send_seconds = seconds; next.close_after_send = close_after;
  std::string error;
  if (SaveSettings(next, &error)) { settings_ = next; auto_clock_.Reset(); }
  else model_.status = error;
  Changed();
}
unsigned Runtime::TakeClipboardAction() {
  std::lock_guard lock(mutex_);
  const auto action = clipboard_action_; clipboard_action_ = 0;
  return model_.capture && model_.visible && clipboard_target_ == model_.live &&
      model_.bound == model_.live ? action : 0;
}
Json Runtime::CopyTicketLocked() {
  CheckPluginAuthorization();
  if (!model_.visible || model_.busy || model_.Pending() || model_.uncertain || !model_.preedit.empty()) return {};
  const bool result = !model_.result.empty();
  if (result && !model_.CompleteResult()) return {};
  if (!result && !model_.selected && model_.capture) return {};
  const auto text = result ? model_.ResultText() : model_.SourceText();
  if (text.empty()) return {};
  return {{"revision", model_.revision}, {"generation", model_.GenerationID()},
          {"text", text}, {"session", model_.bound.session},
          {"context", model_.bound.context}, {"capture", model_.capture}};
}
Json Runtime::PrepareCopy() { std::lock_guard lock(mutex_); return CopyTicketLocked(); }
bool Runtime::ValidateCopy(const Json& ticket) {
  std::lock_guard lock(mutex_);
  return !ticket.is_null() && ticket == CopyTicketLocked();
}
bool Runtime::CommitCopy(const Json& ticket, const std::function<bool()>& write) {
  std::lock_guard lock(mutex_);
  if (!ticket.is_null() && ticket == CopyTicketLocked() && write()) {
    model_.Close(); CaptureChanged(); Changed();
    return true;
  }
  return false;
}
void Runtime::PublishCaret(const RECT& caret) {
  std::lock_guard lock(mutex_);
  if (!model_.visible || !model_.capture || model_.bound != model_.live ||
      caret.bottom <= caret.top || EqualRect(&caret, &buffer_caret_)) return;
  auto found = sessions_.find(model_.bound.session);
  if (found == sessions_.end()) return;
  buffer_caret_ = caret;
  std::erase_if(found->second.events, [](const Json& e) { return e.value("kind", "") == "buffer_caret"; });
  found->second.events.push_back({{"kind", "buffer_caret"}, {"context", model_.bound.context},
      {"openingRevision", opening_revision_},
      {"rect", {caret.left, caret.top, caret.right, caret.bottom}}});
  event_.notify_all();
}
bool Runtime::EffectiveGenerationSettings(bool translation, Settings* effective) {
  *effective = settings_;
  if (translation || ai_plugin_.empty()) return true;
  const std::string kind = ai_plugin_ == official::kCodex ? "codex-cli" : "openai-compatible";
  if (effective->ai_connector == kind) return true;
  const auto found = std::find_if(effective->connections.begin(), effective->connections.end(),
      [&](const auto& p) { return p.kind == kind; });
  return found != effective->connections.end() && SelectConnection(effective, found->id);
}
Runtime::Readiness Runtime::GenerationReadinessLocked(bool translation) {
  if (!settings_valid_) return {false, "设置不可用，请先修复设置文件。"};
  if (translation && settings_.translation_provider == "local")
    return {!translation_resources_.ready || translation_ready_, translation_ready_ ? "" : translation_status_};
  Settings effective;
  if (!EffectiveGenerationSettings(translation, &effective))
    return {false, "请先在设置 > 连接器中配置该插件的模型。"};
  if (!readiness_probe_) return {true, {}};
  const auto key = std::to_string(settings_.revision) + ":" + effective.active_connection;
  const auto found = readiness_cache_.find(key);
  return found == readiness_cache_.end() ? Readiness{false, "正在检查连接器…"} : found->second.value;
}
void Runtime::RefreshReadinessLocked(bool translation) {
  if (!readiness_probe_ || readiness_running_ || stopping_ ||
      (translation && settings_.translation_provider == "local")) return;
  Settings effective;
  if (!EffectiveGenerationSettings(translation, &effective)) return;
  const auto key = std::to_string(settings_.revision) + ":" + effective.active_connection;
  const auto found = readiness_cache_.find(key);
  if (found != readiness_cache_.end() && clock_() < found->second.expires) return;
  readiness_running_ = true;
  readiness_worker_ = std::jthread([this, effective, key](std::stop_token stop) {
    Readiness result;
    try { result = readiness_probe_(effective, [stop] { return stop.stop_requested(); }); }
    catch (...) { result = {false, "连接器状态检查失败，请在设置中检查连接。"}; }
    std::lock_guard lock(mutex_);
    readiness_running_ = false;
    if (stopping_) return;
    // A configuration edit revokes the check as well as its cached result.
    // Notify again when a cancelled probe retires so the UI can check the new
    // connector without waiting for another user action.
    if (stop.stop_requested() || effective.revision != settings_.revision) {
      Changed();
      return;
    }
    readiness_cache_[key] = {std::move(result), clock_() + 30000};
    Changed();
  });
}
void Runtime::StartGeneration(bool translation, bool complete_sentence_only) {
  if (!settings_valid_ || stopping_ || model_.busy || model_.Pending() ||
      (!translation && !model_.result.empty()))
    return;
  CheckPluginAuthorization();
  const std::string plugin = translation ? official::kTranslation
      : !ai_plugin_.empty() ? ai_plugin_
      : settings_.ai_connector == "codex-cli" ? official::kCodex : official::kAI;
  auto effective = settings_;
  if (!translation && !ai_plugin_.empty()) {
    const std::string kind = plugin == official::kCodex ? "codex-cli" : "openai-compatible";
    if (effective.ai_connector != kind) {
      const auto found = std::find_if(effective.connections.begin(), effective.connections.end(),
          [&](const auto& p) { return p.kind == kind; });
      if (found == effective.connections.end()) {
        model_.status = "请先在设置 > 连接器中配置该插件的模型。"; Changed(); return;
      }
      SelectConnection(&effective, found->id);
    }
  }
  const auto grant = plugins_.Grant(plugin);
  if (grant.empty()) { failed_translation_revision_ = model_.revision; model_.status = "Install and enable the plugin in Settings > Official plugins."; Changed(); return; }
  const bool local = translation && settings_.translation_provider == "local";
  if (local && (!LocalTranslationLanguage(TranslationLanguageID(settings_.source_language)) ||
                !LocalTranslationLanguage(TranslationLanguageID(settings_.target_language)))) {
    failed_translation_revision_ = model_.revision;
    model_.status = "本地翻译支持中文与英语互译；其他语言请在设置中选择 AI 翻译。";
    Changed(); return;
  }
  if (local && translation_resources_.ready && !translation_ready_) {
    translation_pending_ = true;
    model_.status = "本地翻译准备中…";
    PrepareTranslationResources();
    Changed();
    return;
  }
  RefreshReadinessLocked(translation);
  const auto readiness = GenerationReadinessLocked(translation);
  if (!readiness.ready) { model_.status = readiness.message; Changed(); return; }
  translation_pending_ = false;
  const std::string connector = !local && effective.ai_connector == "codex-cli" ? official::kCodex : "";
  const auto connector_grant = connector.empty() ? std::string{} : plugins_.Grant(connector);
  if (!connector.empty() && connector_grant.empty()) {
    failed_translation_revision_ = model_.revision; model_.status = "Enable the Codex connector in Settings > Official plugins."; Changed(); return;
  }
  std::string instruction;
  try { instruction = official::Instruction(plugins_.Package(plugin), settings_.target_language); }
  catch (...) { model_.status = "Plugin package unavailable."; Changed(); return; }
  auto job = model_.Generate(settings_.revision, translation,
                             complete_sentence_only);
  if (!model_.busy) return;
  generation_started_ = clock_(); generation_elapsed_second_ = 0;
  if (local) model_.status = "正在本机翻译…";
  job.plugin_id = plugin; job.plugin_grant = grant; job.instruction = instruction;
  if (plugin == official::kCodex) { job.model = settings_.chatgpt_model; job.effort = settings_.chatgpt_effort; }
  job.connector_id = connector; job.connector_grant = connector_grant;
  job.target_language = settings_.target_language;
  if (translation && !local) job.instruction += "\nSource language: " + settings_.source_language;
  result_plugin_ = plugin; result_grant_ = grant;
  result_connector_ = connector; result_connector_grant_ = connector_grant;
  api_job_ = std::make_pair(effective, std::move(job));
  api_event_.notify_one();
  Changed();
}
void Runtime::Generate(bool translation) {
  std::lock_guard lock(mutex_);
  const std::string id = translation ? official::kTranslation : !ai_plugin_.empty() ? ai_plugin_ :
      settings_.ai_connector == "codex-cli" ? official::kCodex : official::kAI;
  if (!SelectBufferPluginLocked(id) || buffer_transition_) return;
  failed_translation_revision_ = 0;
  if (translation) translation_paused_ = false;
  StartGeneration(translation);
}
std::string Runtime::SelectedBufferPluginLocked() const {
  if (model_.translate) return official::kTranslation;
  return ai_mode_ ? (!ai_plugin_.empty() ? ai_plugin_ :
      settings_.ai_connector == "codex-cli" ? official::kCodex : official::kAI) : "";
}
std::vector<PluginView> Runtime::BufferPluginsLocked() {
  std::vector<PluginView> entries{{"", "Default", "", "", true, true, true}};
  for (const auto& entry : plugins_.Entries())
    if (entry.kind == "buffer" && entry.installed && entry.enabled && !plugins_.Grant(entry.id).empty())
      entries.push_back(entry);
  return entries;
}
std::vector<PluginView> Runtime::BufferPlugins() {
  std::lock_guard lock(mutex_); return BufferPluginsLocked();
}
void Runtime::RememberBufferPluginLocked(const std::string& id) {
  if (remembered_buffer_plugin_ == id) return;
  remembered_buffer_plugin_ = id;
  std::string error;
  if (!plugins_.SaveBufferSelection(id, &error))
    model_.status = "本次插件选择未能保存，重启后将恢复此前选择。";
}
void Runtime::SwitchPluginResultsLocked(const std::string& id) {
  auto previous = model_.ParkResults();
  if (!result_plugin_.empty() && !previous.blocks.empty())
    saved_plugin_results_[result_plugin_] = {std::move(previous), result_grant_, result_connector_,
        result_connector_grant_, settings_.revision};
  result_plugin_.clear(); result_grant_.clear(); result_connector_.clear(); result_connector_grant_.clear();
  const auto found = saved_plugin_results_.find(id);
  if (found == saved_plugin_results_.end()) return;
  auto saved = std::move(found->second); saved_plugin_results_.erase(found);
  if (plugins_.Grant(id) != saved.grant ||
      (!saved.connector.empty() && plugins_.Grant(saved.connector) != saved.connector_grant)) return;
  model_.RestoreResults(std::move(saved.results));
  if (saved.configuration_revision != settings_.revision) model_.ResetTranslation();
  result_plugin_ = id; result_grant_ = saved.grant;
  result_connector_ = saved.connector; result_connector_grant_ = saved.connector_grant;
}
bool Runtime::SelectBufferPluginLocked(const std::string& id) {
  if (stopping_ || model_.Pending()) return false;
  const auto entries = BufferPluginsLocked();
  if (std::none_of(entries.begin(), entries.end(), [&](const auto& p) { return p.id == id; })) return false;
  ClearBufferTransitionLocked();
  if (id == SelectedBufferPluginLocked()) { RememberBufferPluginLocked(id); return true; }
  if (!model_.preedit.empty()) return QueueBufferTransitionLocked(false, id);
  const bool was_busy = model_.busy;
  live_metrics_.Reset();
  api_job_.reset(); model_.Cancel();
  SwitchPluginResultsLocked(id);
  translation_pending_ = false; translation_waiting_since_ = 0;
  speech_request_ = {}; speech_after_delivery_ = false;
  ai_mode_ = !id.empty() && id != official::kTranslation;
  ai_plugin_ = ai_mode_ ? id : "";
  model_.generated_mode = ai_mode_;
  model_.translate = false;
  if (id == official::kTranslation) {
    model_.BeginTranslation(); translation_paused_ = false;
    failed_translation_revision_ = 0; edited_at_ = clock_();
  }
  auto_clock_.Reset();
  model_.status = was_busy && id.empty() ? "Processing stopped. Source retained." :
      model_.result.empty() ? (model_.capture ? "Buffer" : "Copy only - no input target") : "Ready";
  RememberBufferPluginLocked(id);
  Changed(); return true;
}
bool Runtime::CycleBufferPluginLocked(int direction) {
  const auto entries = BufferPluginsLocked();
  if (entries.empty() || (direction != -1 && direction != 1)) return false;
  const auto selected = SelectedBufferPluginLocked();
  const auto found = std::find_if(entries.begin(), entries.end(),
      [&](const auto& plugin) { return plugin.id == selected; });
  const auto index = found == entries.end() ? 0 : static_cast<int>(found - entries.begin());
  const auto count = static_cast<int>(entries.size());
  return SelectBufferPluginLocked(entries[(index + count + direction) % count].id);
}
bool Runtime::SelectBufferPlugin(const std::string& id) {
  std::lock_guard lock(mutex_); return SelectBufferPluginLocked(id);
}
void Runtime::SelectAIMode() {
  std::lock_guard lock(mutex_);
  SelectBufferPluginLocked(settings_.ai_connector == "codex-cli" ? official::kCodex : official::kAI);
}
void Runtime::SelectTranslationMode() {
  std::lock_guard lock(mutex_);
  SelectBufferPluginLocked(official::kTranslation);
}
bool Runtime::SetTranslationLanguages(std::string source, std::string target) {
  std::lock_guard lock(mutex_);
  if (!settings_valid_ || !model_.visible || model_.Pending()) return false;
  const auto source_id = TranslationLanguageID(source), target_id = TranslationLanguageID(target);
  if (source_id.empty() || target_id.empty() || source_id == target_id ||
      (settings_.translation_provider == "local" &&
       (!LocalTranslationLanguage(source_id) || !LocalTranslationLanguage(target_id)))) return false;
  if (source_id == TranslationLanguageID(settings_.source_language) &&
      target_id == TranslationLanguageID(settings_.target_language)) return true;
  auto next = settings_;
  next.source_language = std::move(source); next.target_language = std::move(target);
  ++next.revision;
  std::string error;
  if (!SaveSettings(next, &error)) { model_.status = error; Changed(); return false; }
  settings_ = std::move(next);
  model_.ResetTranslation(); api_job_.reset(); speech_request_ = {};
  failed_translation_revision_ = 0; translation_waiting_since_ = 0;
  edited_at_ = clock_(); auto_clock_.Reset();
  model_.status = "语言已切换，等待翻译…";
  Changed();
  return true;
}
bool Runtime::SetChatGPTInference(std::string model, std::string effort) {
  std::lock_guard lock(mutex_);
  if (stopping_ || !settings_valid_ || !ValidAIModel(model) || !ValidAIEffort(model, effort)) return false;
  auto next = settings_; next.chatgpt_model = std::move(model); next.chatgpt_effort = std::move(effort);
  std::string error;
  if (!SaveSettings(next, &error)) { model_.status = error; Changed(); return false; }
  // Already-running requests own their frozen model/effort. A preference edit
  // does not cancel them or invalidate completed output from this plugin.
  settings_ = std::move(next); Changed(); return true;
}
void Runtime::SetTranslationSpeech(bool enabled) {
  std::lock_guard lock(mutex_);
  if (!settings_valid_) return;
  auto next = settings_; next.translation_speech = enabled;
  std::string error;
  if (SaveSettings(next, &error)) settings_ = std::move(next);
  else model_.status = error;
  if (!enabled) { speech_request_ = {}; speech_after_delivery_ = false; }
  Changed();
}
void Runtime::RequestTranslationSpeech(std::uint64_t id) {
  std::lock_guard lock(mutex_);
  if (!model_.visible || !model_.translate || !settings_.translation_speech) return;
  for (const auto& block : model_.result) if (block.id == id) {
    speech_request_ = {{"text", block.text}, {"language", block.language}};
    Changed(); break;
  }
}
Json Runtime::TakeTranslationSpeech() {
  std::lock_guard lock(mutex_);
  auto request = std::move(speech_request_); speech_request_ = {};
  return (model_.visible || speech_after_delivery_) && model_.translate && settings_.translation_speech ? request : Json{};
}
void Runtime::ReturnToInput() {
  std::lock_guard lock(mutex_);
  SelectBufferPluginLocked("");
}
void Runtime::Cancel() {
  std::lock_guard lock(mutex_);
  ClearBufferTransitionLocked();
  api_job_.reset(); model_.Cancel();
  translation_paused_ = true; translation_pending_ = false; translation_waiting_since_ = 0;
  speech_request_ = {}; speech_after_delivery_ = false; auto_clock_.Reset();
  model_.status = "Processing stopped. Source retained.";
  Changed();
}
void Runtime::Tick() {
  if (clock_() - metrics_save_at_ > 5000) { metrics_->Save(); metrics_save_at_ = clock_(); }
  std::lock_guard lock(mutex_);
  CheckPluginAuthorization();
  PrepareTranslationResources();
  if (translation_pending_ && translation_ready_ && model_.translate && model_.visible && !translation_paused_)
    StartGeneration(true);
  const auto now = clock_();
  if (model_.visible && model_.busy && (now - generation_started_) / 1000 != generation_elapsed_second_) {
    generation_elapsed_second_ = (now - generation_started_) / 1000;
    Changed();
  }
  if (fold_at_ && now >= *fold_at_) {
    fold_at_.reset();
    folded_ = focus_follow_ && model_.visible && !model_.capture;
    Changed();
  }
  if (buffer_transition_ && now > buffer_transition_->deadline) {
    ClearBufferTransitionLocked();
    model_.status = "请先完成组字再重试；更新后请重启目标应用。";
    Changed();
  }
  if (return_held_ && !return_sent_ && now - pressed_at_ >= settings_.send_hold_ms) {
    return_sent_ = true;
    if (model_.capture && return_target_ == model_.bound) {
      const auto delivery = SendAuthorized(true);
      if (!delivery) return_progress_ = false;
      Queue(delivery);
    }
    Changed();
  }
  if (model_.Pending() && pending_since_ && now - pending_since_ > 10000) {
    model_.LostAcknowledgement();
    pending_since_ = 0;
    CaptureChanged();
    Changed();
  }
  if (model_.visible && !translation_paused_ &&
      model_.translate && !model_.busy && !model_.Pending() &&
      model_.revision != failed_translation_revision_ &&
      model_.NeedsTranslation()) {
    if (!translation_waiting_since_) translation_waiting_since_ = now;
    if (now - edited_at_ >= 300 || now - translation_waiting_since_ >= 900) {
      StartGeneration(true);
      translation_waiting_since_ = 0;
    }
  } else translation_waiting_since_ = 0;
  const bool ready = !ai_mode_ && !model_.translate && model_.result.empty() && settings_.auto_send_seconds &&
      model_.visible && model_.capture && model_.bound == model_.live && !model_.Pending() && !model_.uncertain &&
      !model_.source.empty() && !model_.busy && model_.preedit.empty();
  auto_countdown_active_ = ready;
  if (auto_clock_.Tick(model_.source, now, ready, settings_.auto_send_seconds * 1000)) {
    Queue(SendAuthorized(false));
    auto_clock_.Pause(); auto_countdown_active_ = false;
  }
  if (ready) Changed();
}
void Runtime::Stop() {
  std::lock_guard lock(mutex_);
  stopping_ = true;
  readiness_worker_.request_stop();
  translation_worker_.request_stop();
  DiscardBufferLocked();
  api_event_.notify_all();
  event_.notify_all();
  Changed();
}
void Runtime::CheckPluginAuthorization() {
  std::erase_if(saved_plugin_results_, [&](const auto& item) {
    return plugins_.Grant(item.first) != item.second.grant ||
        (!item.second.connector.empty() && plugins_.Grant(item.second.connector) != item.second.connector_grant);
  });
  if ((!result_plugin_.empty() && plugins_.Grant(result_plugin_) != result_grant_) ||
      (!result_connector_.empty() && plugins_.Grant(result_connector_) != result_connector_grant_)) {
    model_.InvalidatePluginResults();
    if (!model_.Pending()) { result_plugin_.clear(); result_grant_.clear(); result_connector_.clear(); result_connector_grant_.clear(); }
    Changed();
  }
}
std::optional<Delivery> Runtime::SendAuthorized(bool all) {
  const bool revoked = (!result_plugin_.empty() && plugins_.Grant(result_plugin_) != result_grant_) ||
      (!result_connector_.empty() && plugins_.Grant(result_connector_) != result_connector_grant_);
  CheckPluginAuthorization();
  return revoked ? std::nullopt : model_.Send(all);
}
std::vector<PluginView> Runtime::Plugins() { return plugins_.Entries(); }
std::string Runtime::PluginStatus() {
  std::lock_guard lock(mutex_);
  return plugin_status_ + (plugin_status_.empty() || translation_status_.empty() ? "" : "\n") + translation_status_;
}
bool Runtime::ManagePlugin(const std::string& id, const std::string& action, std::string* error) {
  std::lock_guard lock(mutex_);
  if (stopping_ || model_.Pending()) { if(error) *error = "Wait for the current insertion to finish."; return false; }
  if (action == "install") {
    if (plugin_installing_) { if(error) *error = "A plugin download is already running."; return false; }
    plugin_installing_ = true; plugin_status_ = "Downloading and verifying...";
    plugin_worker_ = std::jthread([this, id] {
      std::string failure; const bool ok = plugins_.Install(id, &failure);
      std::lock_guard guard(mutex_); plugin_installing_ = false;
      plugin_status_ = ok ? "Installed. Enable the plugin to use it." : failure; Changed();
    });
    Changed(); return true;
  }
  const bool ok = action == "uninstall" ? plugins_.Uninstall(id, error)
      : action == "enable" || action == "disable" ? plugins_.Enable(id, action == "enable", error) : false;
  if (ok) {
    const bool unavailable = action == "disable" || action == "uninstall";
    const bool fallback = unavailable && SelectedBufferPluginLocked() == id;
    if (unavailable && remembered_buffer_plugin_ == id) RememberBufferPluginLocked("");
    ++settings_.revision;
    metrics_->Enable(!plugins_.Grant("builtin.statistics").empty(), !plugins_.Grant("builtin.typing-speed").empty());
    CheckPluginAuthorization();
    plugin_status_ = action == "uninstall" ? "Uninstalled. User data retained." : action == "enable" ? "Enabled." : "Disabled.";
    CaptureChanged();
    // Queue settlement after publishing the grant change; CaptureChanged must
    // not immediately cancel the fallback action for an owned composition.
    if (fallback) {
      if (!model_.preedit.empty()) QueueBufferTransitionLocked(false);
      else SelectBufferPluginLocked("");
    }
    Changed();
  }
  return ok;
}
void Runtime::RunAPI() {
  for (;;) {
    std::unique_lock lock(mutex_);
    api_event_.wait(lock, [&] { return stopping_ || api_job_.has_value(); });
    if (stopping_) return;
    auto [config, job] = std::move(*api_job_);
    api_job_.reset();
    if (!model_.visible || !model_.Accepts(job, settings_.revision) ||
        plugins_.Grant(job.plugin_id) != job.plugin_grant ||
        (!job.connector_id.empty() && plugins_.Grant(job.connector_id) != job.connector_grant)) continue;
    lock.unlock();
    const Generation identity = job;
    const auto authorized = [this, identity] {
      return !stopping_ && plugins_.Grant(identity.plugin_id) == identity.plugin_grant &&
          (identity.connector_id.empty() || plugins_.Grant(identity.connector_id) == identity.connector_grant) &&
          model_.Accepts(identity, settings_.revision);
    };
    job.snapshot = [this, identity, authorized](const std::string& text) {
      std::lock_guard l(mutex_);
      if (!authorized()) return false;
      const bool accepted = model_.StreamSnapshot(identity, settings_.revision, text);
      Changed(); return accepted;
    };
    job.progress = [this, authorized](bool summary, const std::string& text) {
      std::lock_guard l(mutex_);
      if (!authorized() || text.size() > 16384 || text.find('\0') != std::string::npos) return false;
      try { (void)Wide(text); } catch (...) { return false; }
      if (summary) model_.reasoning_summary = text;
      else model_.status = text;
      Changed(); return true;
    };
    std::string error;
    const bool ok = generate_(
        config, job,
        [&](const std::string& text) {
          std::lock_guard l(mutex_);
          const bool accepted = plugins_.Grant(job.plugin_id) == job.plugin_grant &&
              (job.connector_id.empty() || plugins_.Grant(job.connector_id) == job.connector_grant) &&
              model_.Stream(job, settings_.revision, text);
          Changed();
          return accepted;
        },
        [&] {
          std::lock_guard l(mutex_);
          return stopping_ || plugins_.Grant(job.plugin_id) != job.plugin_grant ||
              (!job.connector_id.empty() && plugins_.Grant(job.connector_id) != job.connector_grant) ||
              !model_.Accepts(job, settings_.revision);
        },
        &error);
    lock.lock();
    if (plugins_.Grant(job.plugin_id) == job.plugin_grant &&
        (job.connector_id.empty() || plugins_.Grant(job.connector_id) == job.connector_grant) && model_.Accepts(job, settings_.revision)) {
      const bool finished = model_.Finish(job, settings_.revision, ok);
      if (!finished && job.translation) failed_translation_revision_ = model_.revision;
      if (!ok) {
        model_.status = error;
        if (job.translation) failed_translation_revision_ = model_.revision;
      }
      Changed();
    }
  }
}
}  // namespace rimes::windows::workbench
