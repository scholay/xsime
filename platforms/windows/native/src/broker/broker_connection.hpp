#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bitset>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "../core/broker_protocol.hpp"
#include "../engine/rime_engine.hpp"
#include "../workbench/runtime.hpp"
#include "named_pipe_server.hpp"

#include "../workbench/chord_profile.hpp"

namespace rimes::windows::broker {

// Owns all librime sessions opened by one already-verified pipe client. The
// process-level RimeEngine outlives every BrokerConnection and remains the sole
// owner of the librime runtime.
class BrokerConnection final {
 public:
  BrokerConnection(DWORD broker_session_id, engine::RimeEngine* engine,
                   workbench::Runtime* runtime = nullptr,
                   std::function<bool()> open_settings = {},
                   bool input_only = false) noexcept;
  ~BrokerConnection();

  BrokerConnection(const BrokerConnection&) = delete;
  BrokerConnection& operator=(const BrokerConnection&) = delete;

  ClientAction Handle(const core::Frame& request,
                      DWORD verified_client_process_id, core::Frame* response);

 private:
  friend struct CandidatePagingRegressionProbe;
  struct SessionState {
    std::uint64_t context_id = 0;
    std::uint64_t last_sequence_id = 0;
    std::uint64_t revision = 0;
    engine::RimeEngine::SessionId engine_session_id = 0;
    workbench::ChordComposer chord;
    std::optional<workbench::ChordProfile> chord_profile;
    engine::EngineSnapshot chord_snapshot;
    unsigned chord_interval_ms = 50;
    bool composing = false;
    workbench::Target target;
    std::uint64_t settings_revision = 0;
    bool capture = false;
    unsigned input_route = 0;
    std::optional<std::uint64_t> candidate_guard;
    std::string schema_id;
    std::bitset<256> handled_key_downs;
  };

  ClientAction HandleHello(const core::Frame& request,
                           DWORD verified_client_process_id,
                           core::Frame* response);
  ClientAction HandleOpenSession(const core::Frame& request,
                                 core::Frame* response);
  ClientAction HandleCloseSession(const core::Frame& request,
                                  core::Frame* response);
  ClientAction HandleKeyEvent(const core::Frame& request,
                              core::Frame* response);
  ClientAction RespondWithInputState(const core::Frame& request,
                                     core::InputState state,
                                     core::Frame* response);
  ClientAction RespondPassThrough(const core::Frame& request,
                                  const core::KeyEvent& key,
                                  const SessionState& session,
                                  core::Frame* response);
  std::uint64_t AllocateSessionId() noexcept;
  void CloseAllSessions() noexcept;

  static void MakeResponse(const core::Frame& request, core::MessageType type,
                           std::vector<std::byte> payload,
                           core::Frame* response);
  static void MakeError(const core::Frame& request, core::BrokerErrorCode code,
                        std::string message, core::Frame* response);

  DWORD broker_session_id_;
  engine::RimeEngine* engine_;
  bool hello_received_ = false;
  bool modifier_snapshots_ = false;
  bool key_routing_ = false;
  bool pending_chord_ = false;
  bool candidate_paging_ = false;
  workbench::Runtime* runtime_;
  workbench::Runtime* settings_;
  std::function<bool()> open_settings_;
  DWORD peer_process_ = 0;
  std::uint64_t next_session_id_ = 1;
  std::unordered_map<std::uint64_t, SessionState> sessions_;
};

}  // namespace rimes::windows::broker
