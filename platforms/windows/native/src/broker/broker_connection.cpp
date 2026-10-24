#include "rimes_version.hpp"
#include "broker_connection.hpp"
#include "../core/buffer_keys.hpp"

#include <array>
#include <atomic>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

#include "../engine/rime_snapshot.hpp"
#include "key_translation.hpp"

namespace rimes::windows::broker {
namespace {

inline constexpr std::string_view kBrokerVersion = kProductVersion;
inline constexpr std::size_t kMaxSessionsPerConnection = 64;

// Only the isolated CI Broker records phase durations. Never log a key code,
// composition, candidate, committed text, or a user's host application.
class KeyLatencyTrace {
 public:
  void Mark(const char* phase) {
#ifdef RIMES_E2E_TEST_IDENTITY
    if (count_ < phases_.size()) phases_[count_++] = {phase, GetTickCount64()};
#else
    (void)phase;
#endif
  }
  ~KeyLatencyTrace() {
#ifdef RIMES_E2E_TEST_IDENTITY
    const auto ended = GetTickCount64();
    if (ended - started_ < 25) return;
    std::clog << "RIMES key latency total_ms=" << ended - started_;
    auto previous = started_;
    for (std::size_t i = 0; i < count_; ++i) {
      std::clog << ' ' << phases_[i].first << "_ms="
                << phases_[i].second - previous;
      previous = phases_[i].second;
    }
    std::clog << " reply_ms=" << ended - previous << '\n';
#endif
  }

 private:
#ifdef RIMES_E2E_TEST_IDENTITY
  ULONGLONG started_ = GetTickCount64();
  std::array<std::pair<const char*, ULONGLONG>, 9> phases_{};
  std::size_t count_ = 0;
#endif
};

constexpr std::uint32_t Flag(core::KeyEventFlags flag) noexcept {
  return static_cast<std::uint32_t>(flag);
}

constexpr std::uint32_t StateFlag(core::InputStateFlags flag) noexcept {
  return static_cast<std::uint32_t>(flag);
}

constexpr bool HasFlag(std::uint32_t value, std::uint32_t flag) noexcept {
  return (value & flag) != 0;
}

void ObserveCommit(workbench::Runtime* runtime, DWORD process,
                   const std::string& text, KeyLatencyTrace* latency = nullptr) noexcept {
  if (!runtime || text.empty()) return;
  try {
    const auto count = static_cast<unsigned>(metrics::Characters(text).size());
    if (latency) latency->Mark("unicode");
    runtime->Metrics()->ObserveCommit(process, count);
    if (latency) latency->Mark("aggregate");
  } catch (...) {
    // A best-effort aggregate must never affect actual text delivery.
  }
}

void LogEngineFailure(std::string_view operation,
                      std::uint64_t broker_session_id, std::string_view error) {
  std::clog << "RIMES broker " << operation << " failed for session "
            << broker_session_id;
  if (!error.empty()) {
    std::clog << ": " << error;
  }
  std::clog << '\n';
}

}  // namespace

BrokerConnection::BrokerConnection(DWORD broker_session_id,
                                   engine::RimeEngine* engine,
                                   workbench::Runtime* runtime,
                                   std::function<bool()> open_settings,
                                   bool input_only) noexcept
    : broker_session_id_(broker_session_id),
      engine_(engine),
      runtime_(input_only ? nullptr : runtime),
      settings_(runtime),
      open_settings_(input_only ? std::function<bool()>{} : std::move(open_settings)) {}

BrokerConnection::~BrokerConnection() { CloseAllSessions(); }

ClientAction BrokerConnection::Handle(const core::Frame& request,
                                      DWORD verified_client_process_id,
                                      core::Frame* response) {
  if (response == nullptr) {
    return ClientAction::kCloseAfterResponse;
  }
  if (request.header.request_id == 0 || request.header.flags != 0) {
    MakeError(request, core::BrokerErrorCode::kMalformedPayload,
              "requests require a non-zero request_id and zero flags",
              response);
    return ClientAction::kCloseAfterResponse;
  }
  if (!hello_received_) {
    if (request.header.message_type != core::MessageType::kClientHello) {
      MakeError(request, core::BrokerErrorCode::kUnauthorizedClient,
                "ClientHello must be the first frame", response);
      return ClientAction::kCloseAfterResponse;
    }
    return HandleHello(request, verified_client_process_id, response);
  }

  if (request.header.message_type == core::MessageType::kControl) {
    auto message = core::DecodeControl(request.payload);
    if (!message) {
      MakeError(request, core::BrokerErrorCode::kMalformedPayload,
                "Invalid control frame", response);
      return ClientAction::kCloseAfterResponse;
    }
    try {
      if (message->value("op", "") == "open_settings") {
        // The named-pipe server verifies SID/logon identity, and the Hello
        // above verifies the peer PID. A UI command does not own an input
        // context: never manufacture a Rime session or publish a focus target.
        if (verified_client_process_id != peer_process_ ||
            message->size() != 1 || !open_settings_) {
          MakeError(request, core::BrokerErrorCode::kUnsupportedMessage,
                    "settings require the authenticated interactive Broker",
                    response);
          return ClientAction::kCloseAfterResponse;
        }
        MakeResponse(request, core::MessageType::kControlState,
                     core::EncodeControl({{"kind", open_settings_() ? "ok" : "starting"}}),
                     response);
        return ClientAction::kContinue;
      }
      if (message->value("op", "") == "candidate_guard") {
        const auto session = sessions_.find(message->value("session", 0ULL));
        const bool valid =
            session != sessions_.end() && session->second.composing &&
            session->second.revision == message->value("revision", 0ULL) &&
            message->value("index", 99U) < 9;
        if (valid)
          session->second.candidate_guard = session->second.settings_revision;
        MakeResponse(request, core::MessageType::kControlState,
                     core::EncodeControl({{"kind", valid ? "ok" : "stale"}}),
                     response);
        return ClientAction::kContinue;
      }
      // Candidate validation belongs to the engine session and is available
      // in the headless integration Broker as well. Other controls need UI.
      if (!runtime_) {
        MakeError(request, core::BrokerErrorCode::kUnsupportedMessage,
                  "workbench controls require an interactive Broker", response);
        return ClientAction::kCloseAfterResponse;
      }
      if (message->value("op", "") == "buffer_transition") {
        const auto found = sessions_.find(message->value("session", 0ULL));
        const bool valid = verified_client_process_id == peer_process_ && engine_ && found != sessions_.end() &&
            found->second.engine_session_id && runtime_->CompleteBufferTransition(found->second.target,
                message->value("request", 0ULL), [&](engine::EngineSnapshot* output) {
          auto& session = found->second;
          if (!session.composing && !session.chord.Pending()) return false;
          if (session.chord.Pending() && session.chord_profile) {
            const auto flush = session.chord.Flush(*session.chord_profile);
            bool replaced = false;
            if (!engine_->ProcessChordText(session.engine_session_id, flush.text,
                flush.replace ? flush.raw : "", session.chord_snapshot, output, &replaced, nullptr)) return false;
            ObserveCommit(settings_, peer_process_, output->commit_text);
            session.composing = output->composing;
            session.chord.Reset(); session.chord_snapshot = {};
          }
          if (session.composing) {
            engine::EngineSnapshot settled;
            if (!engine_->CommitComposition(session.engine_session_id, &settled, nullptr)) return false;
            ObserveCommit(settings_, peer_process_, settled.commit_text);
            settled.commit_text = output->commit_text + settled.commit_text;
            *output = std::move(settled);
            session.composing = output->composing;
          }
          ++session.revision;
          return !session.composing;
        });
        MakeResponse(request, core::MessageType::kControlState,
                     core::EncodeControl({{"kind", valid ? "ok" : "stale"}}), response);
        return ClientAction::kContinue;
      }
      auto output = runtime_->Control(*message, verified_client_process_id);
      MakeResponse(request, core::MessageType::kControlState,
                   core::EncodeControl(output), response);
      return ClientAction::kContinue;
    } catch (...) {
      MakeError(request, core::BrokerErrorCode::kMalformedPayload,
                "Invalid control fields", response);
      return ClientAction::kCloseAfterResponse;
    }
  }
  switch (request.header.message_type) {
    case core::MessageType::kPing:
      if (!request.payload.empty()) {
        MakeError(request, core::BrokerErrorCode::kMalformedPayload,
                  "Ping payload must be empty", response);
        return ClientAction::kCloseAfterResponse;
      }
      MakeResponse(request, core::MessageType::kPong, {}, response);
      return ClientAction::kContinue;

    case core::MessageType::kOpenInputSession:
      return HandleOpenSession(request, response);

    case core::MessageType::kCloseInputSession:
      return HandleCloseSession(request, response);

    case core::MessageType::kKeyEvent:
      return HandleKeyEvent(request, response);

    case core::MessageType::kClientHello:
      MakeError(request, core::BrokerErrorCode::kMalformedPayload,
                "ClientHello may only be sent once", response);
      return ClientAction::kCloseAfterResponse;

    default:
      MakeError(request, core::BrokerErrorCode::kUnsupportedMessage,
                "message type is not a broker request", response);
      return ClientAction::kCloseAfterResponse;
  }
}

ClientAction BrokerConnection::HandleHello(const core::Frame& request,
                                           DWORD verified_client_process_id,
                                           core::Frame* response) {
  core::ClientHello hello;
  std::string error;
  if (!core::DecodeClientHello(request.payload, &hello, &error) ||
      hello.process_id != verified_client_process_id ||
      hello.session_id != broker_session_id_ || hello.client_name.empty()) {
    MakeError(request, core::BrokerErrorCode::kUnauthorizedClient,
              "ClientHello identity does not match the verified pipe client",
              response);
    return ClientAction::kCloseAfterResponse;
  }

  core::BrokerHello broker_hello;
  broker_hello.process_id = GetCurrentProcessId();
  broker_hello.session_id = broker_session_id_;
  broker_hello.capabilities = (runtime_ ? 1ULL : 0ULL) |
      core::kModifierSnapshotsCapability | core::kKeyRoutingCapability | core::kPendingChordCapability |
      core::kCandidatePagingCapability;
  modifier_snapshots_ =
      (hello.capabilities & core::kModifierSnapshotsCapability) != 0;
  key_routing_ = (hello.capabilities & core::kKeyRoutingCapability) != 0;
  pending_chord_ = (hello.capabilities & core::kPendingChordCapability) != 0;
  candidate_paging_ = (hello.capabilities & core::kCandidatePagingCapability) != 0;
  peer_process_ = verified_client_process_id;
  broker_hello.broker_version = std::string(kBrokerVersion);
  std::vector<std::byte> payload;
  if (!core::EncodeBrokerHello(broker_hello, &payload)) {
    MakeError(request, core::BrokerErrorCode::kInternalError,
              "failed to encode BrokerHello", response);
    return ClientAction::kCloseAfterResponse;
  }
  hello_received_ = true;
  MakeResponse(request, core::MessageType::kBrokerHello, std::move(payload),
               response);
  return ClientAction::kContinue;
}

ClientAction BrokerConnection::HandleOpenSession(const core::Frame& request,
                                                 core::Frame* response) {
  core::OpenInputSession open;
  if (!core::DecodeOpenInputSession(request.payload, &open)) {
    MakeError(request, core::BrokerErrorCode::kMalformedPayload,
              "invalid OpenInputSession payload", response);
    return ClientAction::kCloseAfterResponse;
  }
  if (sessions_.size() >= kMaxSessionsPerConnection) {
    MakeError(request, core::BrokerErrorCode::kMalformedPayload,
              "input session limit reached", response);
    return ClientAction::kContinue;
  }
  if (!open.schema_id.empty()) {
    MakeError(
        request, core::BrokerErrorCode::kUnsupportedMessage,
        "explicit schema selection is not available at this engine boundary",
        response);
    return ClientAction::kContinue;
  }
  if (engine_ == nullptr || !engine_->IsHealthy()) {
    MakeError(request, core::BrokerErrorCode::kInternalError,
              "Rime engine is unavailable", response);
    return ClientAction::kContinue;
  }

  std::string engine_error;
  const engine::RimeEngine::SessionId engine_session =
      engine_->CreateSession(&engine_error);
  if (engine_session == 0) {
    LogEngineFailure("CreateSession", 0, engine_error);
    MakeError(request, core::BrokerErrorCode::kInternalError,
              "failed to create the Rime input session", response);
    return ClientAction::kContinue;
  }

  const std::uint64_t session_id = AllocateSessionId();
  if (session_id == 0) {
    engine_->DestroySession(engine_session, nullptr);
    MakeError(request, core::BrokerErrorCode::kInternalError,
              "could not allocate a broker input session identifier", response);
    return ClientAction::kContinue;
  }

  try {
    SessionState state;
    state.context_id = open.context_id;
    state.engine_session_id = engine_session;
    state.schema_id = open.schema_id;
    if (settings_) {
      const auto config = settings_->Configuration();
      if (!engine_->Configure(engine_session, pending_chord_ && config.schema == "my_combo" ? "rime_ice" : config.schema, config.ascii,
                              config.traditional, config.ascii_punctuation,
                              nullptr, config.candidate_count, config.InputOptionsFor(config.schema)))
        throw std::runtime_error("Schema unavailable");
      if (runtime_) state.target =
          runtime_->Register(peer_process_, session_id, open.context_id);
      if (pending_chord_ && config.schema == "my_combo" && config.chord_enabled) {
        state.chord_profile = workbench::ChordProfile::Parse(config.chord_profile.is_null() ? workbench::BuiltinChordProfile() : config.chord_profile);
        state.chord_interval_ms = config.chord_interval_ms;
      }
      state.schema_id = config.schema;
      state.settings_revision = config.revision;
    }
    sessions_.emplace(session_id, std::move(state));
  } catch (...) {
    engine_->DestroySession(engine_session, nullptr);
    MakeError(request, core::BrokerErrorCode::kInternalError,
              "could not retain the new input session", response);
    return ClientAction::kCloseAfterResponse;
  }

  core::InputSessionOpened opened;
  opened.session_id = session_id;
  // RimeEngine does not yet expose schema selection/query APIs. Empty is an
  // honest "engine default" response; never echo an unverified requested ID.
  opened.active_schema_id = sessions_.at(session_id).schema_id;
  opened.has_key_routing = key_routing_;
  opened.ascii_mode = engine_->IsAsciiMode(engine_session);
  std::vector<std::byte> payload;
  if (!core::EncodeInputSessionOpened(opened, &payload)) {
    const auto session = sessions_.find(session_id);
    if (session != sessions_.end()) {
      engine_->DestroySession(session->second.engine_session_id, nullptr);
      sessions_.erase(session);
    }
    MakeError(request, core::BrokerErrorCode::kInternalError,
              "failed to encode InputSessionOpened", response);
    return ClientAction::kCloseAfterResponse;
  }
  MakeResponse(request, core::MessageType::kInputSessionOpened,
               std::move(payload), response);
  return ClientAction::kContinue;
}

ClientAction BrokerConnection::HandleCloseSession(const core::Frame& request,
                                                  core::Frame* response) {
  core::CloseInputSession close;
  if (!core::DecodeCloseInputSession(request.payload, &close)) {
    MakeError(request, core::BrokerErrorCode::kMalformedPayload,
              "invalid CloseInputSession payload", response);
    return ClientAction::kCloseAfterResponse;
  }
  const auto session = sessions_.find(close.session_id);
  if (session == sessions_.end()) {
    MakeError(request, core::BrokerErrorCode::kMalformedPayload,
              "unknown input session", response);
    return ClientAction::kContinue;
  }

  const engine::RimeEngine::SessionId engine_session =
      session->second.engine_session_id;
  if (runtime_) runtime_->Remove(session->second.target);
  sessions_.erase(session);
  std::string engine_error;
  if (engine_ == nullptr ||
      !engine_->DestroySession(engine_session, &engine_error)) {
    LogEngineFailure("DestroySession", close.session_id, engine_error);
    MakeError(request, core::BrokerErrorCode::kInternalError,
              "failed to destroy the Rime input session", response);
    return ClientAction::kContinue;
  }

  core::InputSessionClosed closed{close.session_id};
  std::vector<std::byte> payload;
  if (!core::EncodeInputSessionClosed(closed, &payload)) {
    MakeError(request, core::BrokerErrorCode::kInternalError,
              "failed to encode InputSessionClosed", response);
    return ClientAction::kCloseAfterResponse;
  }
  MakeResponse(request, core::MessageType::kInputSessionClosed,
               std::move(payload), response);
  return ClientAction::kContinue;
}

ClientAction BrokerConnection::HandleKeyEvent(const core::Frame& request,
                                              core::Frame* response) {
  KeyLatencyTrace latency;
  core::KeyEvent key;
  if (!core::DecodeKeyEvent(request.payload, &key)) {
    MakeError(request, core::BrokerErrorCode::kMalformedPayload,
              "invalid KeyEvent payload", response);
    return ClientAction::kCloseAfterResponse;
  }
  const auto session_iterator = sessions_.find(key.session_id);
  if (session_iterator == sessions_.end()) {
    MakeError(request, core::BrokerErrorCode::kMalformedPayload,
              "KeyEvent references an unknown input session", response);
    return ClientAction::kContinue;
  }
  SessionState& session = session_iterator->second;
  if (key.sequence_id <= session.last_sequence_id) {
    MakeError(request, core::BrokerErrorCode::kMalformedPayload,
              "KeyEvent sequence_id is stale or duplicated", response);
    return ClientAction::kCloseAfterResponse;
  }
  session.last_sequence_id = key.sequence_id;
  if (session.candidate_guard) {
    // A pending preference edit does not replace the visible composition. Its
    // candidate click still belongs to the configuration applied to this session.
    const bool valid = *session.candidate_guard == session.settings_revision;
    session.candidate_guard.reset();
    if (!valid) return RespondPassThrough(request, key, session, response);
  }
  if (settings_) {
    latency.Mark("request");
    const auto settings_revision = settings_->ConfigurationRevision();
    const bool capture = runtime_ && runtime_->Capturing(session.target);
    const auto input_route = runtime_ ? runtime_->InputRoute(session.target) : 0;
    if ((session.settings_revision != settings_revision && !session.composing) ||
        session.capture != capture || session.input_route != input_route) {
      const auto config = settings_->Configuration();
      if (!engine_->Configure(session.engine_session_id, (pending_chord_ && config.schema == "my_combo" ? "rime_ice" : config.schema),
                              config.ascii, config.traditional,
                              config.ascii_punctuation, nullptr, config.candidate_count, config.InputOptionsFor(config.schema)))
        return RespondPassThrough(request, key, session, response);
      session.chord.Reset(); session.chord_snapshot = {}; session.chord_profile.reset();
      if (pending_chord_ && config.schema == "my_combo" && config.chord_enabled) {
        try { session.chord_profile = workbench::ChordProfile::Parse(config.chord_profile.is_null()
            ? workbench::BuiltinChordProfile() : config.chord_profile); } catch (...) {}
      }
      session.chord_interval_ms = config.chord_interval_ms;
      session.settings_revision = config.revision;
      session.composing = false;
      session.capture = capture;
      session.input_route = input_route;
    }
  }
  latency.Mark("settings");

  const bool key_down =
      HasFlag(key.event_flags, Flag(core::KeyEventFlags::kKeyDown));
  const bool test_only =
      HasFlag(key.event_flags, Flag(core::KeyEventFlags::kTestOnly));
  const bool preserved =
      HasFlag(key.event_flags, Flag(core::KeyEventFlags::kPreservedKey));

  if (test_only) {
    core::InputState state;
    state.session_id = key.session_id;
    state.sequence_id = key.sequence_id;
    state.revision = session.revision;
    const bool likely_handled =
        key_down ? (IsLikelyHandledForTest(key, session.composing || session.chord.Pending()) ||
                    (runtime_ && runtime_->InputRoute(session.target) == 1 &&
                     runtime_->PluginCycleDirection(key) != 0))
                 : session.handled_key_downs.test(key.virtual_key);
    if (likely_handled) {
      state.state_flags = StateFlag(core::InputStateFlags::kHandled);
    }
    return RespondWithInputState(request, std::move(state), response);
  }

  // Resolve the current Rime composition before a Buffer source edit. This
  // Observation is after the actual-key/privacy gate, never OnTestKey*.
  if (settings_) settings_->Metrics()->ObserveKey(peer_process_, key, session.composing || session.chord.Pending());
  latency.Mark("metrics");
  // is an internal engine operation, never a synthesized host key/paste.
  const auto command_modifiers = key.modifiers & ~static_cast<unsigned>(core::KeyModifiers::kCapsLock);
  const auto physical_key = TranslateWindowsKey(key);
  const bool physical_chord_key = session.chord_profile && physical_key &&
      !core::HasHostCommandModifier(key.modifiers) &&
      !(key.modifiers & static_cast<unsigned>(core::KeyModifiers::kShift)) &&
      physical_key->keycode >= 0 && physical_key->keycode <= 127 &&
      (session.chord_profile->left + session.chord_profile->right).find(static_cast<char>(physical_key->keycode)) != std::string::npos;
  if (runtime_ && runtime_->Capturing(session.target) && key_down &&
      session.chord_profile && session.chord.Pending() &&
      !physical_chord_key) {
    const auto flush = session.chord.Flush(*session.chord_profile);
    if (settings_ && flush.settled) settings_->Metrics()->ObserveChord(peer_process_);
    engine::EngineSnapshot settled; bool replaced = false;
    if (!engine_->ProcessChordText(session.engine_session_id, flush.text,
        flush.replace ? flush.raw : "", session.chord_snapshot, &settled, &replaced, nullptr))
      return RespondPassThrough(request, key, session, response);
    ObserveCommit(settings_, peer_process_, settled.commit_text);
    runtime_->Capture(session.target, &settled);
    session.composing = settled.composing;
    session.chord.Reset(); session.chord_snapshot = {};
  }
  const bool workbench_transition = runtime_ && runtime_->InputRoute(session.target) == 1 &&
      (runtime_->PluginCycleDirection(key) != 0 ||
       (key.virtual_key == VK_ESCAPE && !core::HasHostCommandModifier(key.modifiers)));
  if (runtime_ && runtime_->Capturing(session.target) && session.composing && key_down &&
      ((command_modifiers == static_cast<unsigned>(core::KeyModifiers::kControl) &&
        (key.virtual_key == 'A' || key.virtual_key == 'V')) || workbench_transition)) {
    engine::EngineSnapshot settled;
    const bool resolved = workbench_transition
        ? engine_->CommitComposition(session.engine_session_id, &settled, nullptr)
        : engine_->ProcessKey(session.engine_session_id, 0xff0d, 0, &settled, nullptr);
    if (!resolved) return RespondPassThrough(request, key, session, response);
    ObserveCommit(settings_, peer_process_, settled.commit_text);
    runtime_->Capture(session.target, &settled);
    session.composing = settled.composing;
  }
  if (!test_only && runtime_ &&
      runtime_->BeforeKey(session.target, key, session.composing)) {
    session.chord.Reset(); session.chord_snapshot = {};
    // Paused Buffer input must not run a Shift tap through librime: it could
    // commit preedit into a full draft or an in-flight result. Physical Shift
    // down belongs to the host, so its release must also remain unclaimed.
    if (key.virtual_key == VK_SHIFT || key.virtual_key == VK_LSHIFT ||
        key.virtual_key == VK_RSHIFT)
      return RespondPassThrough(request, key, session, response);
    core::InputState state;
    state.session_id = key.session_id;
    state.sequence_id = key.sequence_id;
    state.revision = ++session.revision;
    state.state_flags = StateFlag(core::InputStateFlags::kHandled);
    if (runtime_->Capturing(session.target))
      state.state_flags |= StateFlag(core::InputStateFlags::kBufferCapture);
    if (key_down)
      session.handled_key_downs.set(key.virtual_key);
    else
      session.handled_key_downs.reset(key.virtual_key);
    return RespondWithInputState(request, std::move(state), response);
  }
  if (preserved) {
    if (!key_down) {
      session.handled_key_downs.reset(key.virtual_key);
    }
    return RespondPassThrough(request, key, session, response);
  }
  auto engine_key = key;
  if (runtime_ && runtime_->Capturing(session.target) && session.composing &&
      runtime_->IsSendChord(key)) {
    engine_key.virtual_key = VK_RETURN;
    engine_key.modifiers = 0;
  }
  // A composing send chord settles Rime only; its same physical press cannot
  // deliver, including when the user assigned a letter or function key.
  const std::optional<RimeKeyEvent> translated = TranslateWindowsKey(engine_key);
  if (!translated.has_value() || engine_ == nullptr || !engine_->IsHealthy()) {
    if (!key_down) {
      session.handled_key_downs.reset(key.virtual_key);
    }
    return RespondPassThrough(request, key, session, response);
  }
  const bool shift = translated->keycode == 0xffe1 || translated->keycode == 0xffe2;
  const bool shift_tap = HasFlag(key.event_flags, Flag(core::KeyEventFlags::kShiftTap));
  if (shift && (!modifier_snapshots_ || !shift_tap))
    return RespondPassThrough(request, key, session, response);
  // Refuse the event before touching librime if the wire revision cannot be
  // advanced. Checking after process_key would violate fail-open by mutating
  // an engine state that TSF can no longer identify.
  if (session.revision == std::numeric_limits<std::uint64_t>::max()) {
    LogEngineFailure("revision overflow", key.session_id, {});
    return RespondPassThrough(request, key, session, response);
  }

  latency.Mark("before_engine");
  engine::EngineSnapshot snapshot;
  std::string engine_error;
  bool processed = false;
  const bool chord_key = session.chord_profile && !engine_->IsAsciiMode(session.engine_session_id) &&
      !core::HasHostCommandModifier(key.modifiers) &&
      !(key.modifiers & static_cast<unsigned>(core::KeyModifiers::kShift)) &&
      translated->keycode >= 0 && translated->keycode <= 127 &&
      (session.chord_profile->left + session.chord_profile->right).find(static_cast<char>(translated->keycode)) != std::string::npos;
  if (chord_key) {
    const auto chord = session.chord.Key(*session.chord_profile, static_cast<char>(translated->keycode),
                                        key_down, GetTickCount64(), session.chord_interval_ms);
    if (settings_ && chord.settled) settings_->Metrics()->ObserveChord(peer_process_);
    bool replaced = false;
    if (chord.handled) {
      processed = engine_->ProcessChordText(session.engine_session_id, chord.text,
          chord.replace ? chord.raw : "", session.chord_snapshot, &snapshot, &replaced, &engine_error);
      if (!snapshot.commit_text.empty() || (chord.replace && !replaced)) session.chord.RevokePair();
      session.chord_snapshot = snapshot;
    } else processed = engine_->ProcessKey(session.engine_session_id, translated->keycode, translated->modifiers, &snapshot, &engine_error);
  } else {
    // Editing, selection, modifiers and punctuation end adjacent-batch authority.
    // A pending physical batch settles before the command, never in the host.
    std::string prefix_commit;
    if (session.chord_profile && session.chord.Pending()) {
      const auto flush = session.chord.Flush(*session.chord_profile); bool replaced = false;
      if (settings_ && flush.settled) settings_->Metrics()->ObserveChord(peer_process_);
      if (!engine_->ProcessChordText(session.engine_session_id, flush.text,
          flush.replace ? flush.raw : "", session.chord_snapshot, &snapshot, &replaced, &engine_error))
        return RespondPassThrough(request, key, session, response);
      prefix_commit = snapshot.commit_text;
    }
    session.chord.Reset(); session.chord_snapshot = {};
    processed = shift_tap
        ? engine_->ProcessShiftTap(session.engine_session_id, translated->keycode, translated->modifiers, &snapshot, &engine_error)
        : engine_->ProcessKey(session.engine_session_id, translated->keycode, translated->modifiers, &snapshot, &engine_error);
    snapshot.commit_text = prefix_commit + snapshot.commit_text;
  }
  latency.Mark("engine");
  if (!processed) {
    LogEngineFailure("ProcessKey", key.session_id, engine_error);
    if (!key_down) {
      session.handled_key_downs.reset(key.virtual_key);
    }
    return RespondPassThrough(request, key, session, response);
  }

  if (runtime_ && runtime_->Capturing(session.target)) {
    // In ASCII mode librime passes printable keys through; Buffer still owns
    // them.
    const bool direct_input = key_down && !snapshot.handled &&
        snapshot.commit_text.empty() && !snapshot.composing &&
        !core::HasHostCommandModifier(key.modifiers) && translated->keycode >= 32 &&
        translated->keycode <= 126;
    if (direct_input) {
      snapshot.handled = true;
      snapshot.commit_text.assign(1, static_cast<char>(translated->keycode));
    }
    ObserveCommit(settings_, peer_process_, snapshot.commit_text, &latency);
    runtime_->Capture(session.target, &snapshot, direct_input);
  } else if (settings_ && !snapshot.commit_text.empty()) {
    ObserveCommit(settings_, peer_process_, snapshot.commit_text, &latency);
  }
  latency.Mark("commit");
  std::uint64_t next_revision = session.revision;
  if (snapshot.handled || snapshot.modifier_snapshot) {
    ++next_revision;
  }

  core::InputState state;
  std::string mapping_error;
  if (!engine::MapSnapshotToInputState(key.session_id, key.sequence_id,
                                       next_revision, snapshot, &state,
                                       &mapping_error)) {
    LogEngineFailure("MapSnapshotToInputState", key.session_id, mapping_error);
    return RespondPassThrough(request, key, session, response);
  }

  if (runtime_ && runtime_->Capturing(session.target))
    state.state_flags |= StateFlag(core::InputStateFlags::kBufferCapture);
  session.revision = next_revision;
  // An ordinary unhandled event has no authoritative snapshot (letter KeyUp).
  // Preserve the displayed composition just as BrokerClient does, otherwise
  // candidate_guard rejects every mouse selection after the final key release.
  if (snapshot.handled || snapshot.modifier_snapshot)
    session.composing = snapshot.composing;
  if (key_down && snapshot.handled) {
    session.handled_key_downs.set(key.virtual_key);
  } else if (!key_down) {
    session.handled_key_downs.reset(key.virtual_key);
  }
  latency.Mark("state");
  return RespondWithInputState(request, std::move(state), response);
}

ClientAction BrokerConnection::RespondWithInputState(const core::Frame& request,
                                                     core::InputState state,
                                                     core::Frame* response) {
  if (!candidate_paging_)
    state.state_flags &= ~core::kCandidatePagingStateFlags;
  if (pending_chord_) {
    const auto session = sessions_.find(state.session_id);
    if (session != sessions_.end() && session->second.chord.Pending())
      state.state_flags |= StateFlag(core::InputStateFlags::kPendingChord);
  }
  if (key_routing_ && engine_) {
    const auto session = sessions_.find(state.session_id);
    if (session != sessions_.end() &&
        engine_->IsAsciiMode(session->second.engine_session_id))
      state.state_flags |= StateFlag(core::InputStateFlags::kAsciiMode);
  }
  std::vector<std::byte> payload;
  if (!core::EncodeInputState(state, &payload)) {
    MakeError(request, core::BrokerErrorCode::kInternalError,
              "failed to encode InputState", response);
    return ClientAction::kCloseAfterResponse;
  }
  MakeResponse(request, core::MessageType::kInputState, std::move(payload),
               response);
  return ClientAction::kContinue;
}

ClientAction BrokerConnection::RespondPassThrough(const core::Frame& request,
                                                  const core::KeyEvent& key,
                                                  const SessionState& session,
                                                  core::Frame* response) {
  core::InputState state;
  state.session_id = key.session_id;
  state.sequence_id = key.sequence_id;
  state.revision = session.revision;
  return RespondWithInputState(request, std::move(state), response);
}

std::uint64_t BrokerConnection::AllocateSessionId() noexcept {
  static std::atomic_uint64_t next{1};
  const auto id = next.fetch_add(1);
  return id == 0 || id == UINT64_MAX ? 0 : id;
}

void BrokerConnection::CloseAllSessions() noexcept {
  if (engine_ != nullptr) {
    for (const auto& [broker_session_id, session] : sessions_) {
      if (runtime_) runtime_->Remove(session.target);
      std::string error;
      if (!engine_->DestroySession(session.engine_session_id, &error)) {
        LogEngineFailure("DestroySession during disconnect", broker_session_id,
                         error);
      }
    }
  }
  sessions_.clear();
}

void BrokerConnection::MakeResponse(const core::Frame& request,
                                    core::MessageType type,
                                    std::vector<std::byte> payload,
                                    core::Frame* response) {
  response->header.message_type = type;
  response->header.flags =
      static_cast<std::uint32_t>(core::FrameFlags::kResponse);
  response->header.request_id = request.header.request_id;
  response->payload = std::move(payload);
}

void BrokerConnection::MakeError(const core::Frame& request,
                                 core::BrokerErrorCode code,
                                 std::string message, core::Frame* response) {
  core::ErrorResponse error_dto{code, std::move(message)};
  std::vector<std::byte> payload;
  core::EncodeErrorResponse(error_dto, &payload);
  MakeResponse(request, core::MessageType::kError, std::move(payload),
               response);
  response->header.flags |=
      static_cast<std::uint32_t>(core::FrameFlags::kError);
}

}  // namespace rimes::windows::broker
