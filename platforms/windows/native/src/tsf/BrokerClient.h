#pragma once

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../core/control.hpp"

namespace rimes::windows::tsf {

inline constexpr UINT kBrokerNotification = WM_APP + 72;
inline constexpr UINT kBrokerConnected = WM_APP + 73;

enum class BrokerKeyPhase {
  kTestKeyDown,
  kKeyDown,
  kTestKeyUp,
  kKeyUp,
  kPreservedKey,
  kInputModeToggle, // Native language-bar action; never an injected host key.
};

struct BrokerKeyEvent {
  BrokerKeyPhase phase;
  WPARAM virtual_key;
  LPARAM key_data;
};

enum class BrokerKeyResult {
  kUnavailable,
  kPassThrough,
  kConsumed,
};

struct BrokerCandidate {
  std::uint32_t id = 0;
  std::wstring text;
  std::wstring comment;
  std::wstring label;
};

// Text mutations returned atomically with one authoritative Broker snapshot. Wire
// strings are decoded and validated by the pipe client before they reach TSF.
// `has_snapshot` is false when the key was eaten without a new authoritative
// mutation (most KeyUp events). Applying an empty default state in that case
// would cancel an in-progress composition.
// A negotiated modifier snapshot may accompany kPassThrough: apply its actual
// text changes while preserving the host's physical modifier event.
struct BrokerInputState {
  bool has_snapshot = false;
  bool buffer_capture = false;
  // Physical chord keys are owned before their final release creates Rime
  // preedit. Keep the host composition lifetime separate from `composing`.
  bool pending_chord = false;
  bool composing = false;
  bool candidates_visible = false;
  std::uint64_t revision = 0;
  std::uint32_t caret_utf16 = 0;
  std::uint32_t selection_length_utf16 = 0;
  std::uint16_t highlighted_candidate = 0xffff;
  std::uint16_t page_start = 0;
  std::uint16_t page_size = 0;
  // False is also the conservative fallback for pre-capability brokers.
  bool last_page = false;
  std::uint32_t candidate_page_keys = 0;
  std::wstring composition;
  std::wstring commit_text;
  std::vector<BrokerCandidate> candidates;
};

// Boundary between the in-process TSF DLL and the future out-of-process
// broker. Implementations must never load librime in this process and must not
// block the application's input thread. Failure or disconnection must return
// kUnavailable so the caller can pass the key through unchanged.
class BrokerClient {
 public:
  virtual ~BrokerClient() = default;

  virtual void BeginConnect() noexcept = 0;
  virtual void Disconnect() noexcept = 0;
  // Called on the owning TSF thread. A new context never inherits composition.
  virtual bool SetContext(std::uint64_t context_id) noexcept = 0;
  virtual void SetNotificationWindow(HWND window) noexcept = 0;
  virtual std::optional<core::Json> TakeNotification() = 0;
  virtual bool Control(core::Json message) noexcept = 0;
  virtual bool Capturing() const noexcept = 0;
  virtual bool IsBufferSendKey(WPARAM) const noexcept { return false; }
  virtual std::optional<bool> AsciiMode() const noexcept { return std::nullopt; }
  virtual std::uint64_t ConnectionGeneration() const noexcept = 0;
  [[nodiscard]] virtual bool IsConnected() const noexcept = 0;
  virtual BrokerKeyResult HandleKey(const BrokerKeyEvent& event,
                                    BrokerInputState* state) noexcept = 0;
};

// Creates a client for the per-user, per-logon-session Broker endpoint. On
// first activation it will launch a sibling RimesBroker.exe when the pipe is
// missing. If the Broker cannot be authenticated or does not answer within the
// I/O budget, keys are passed through and a reconnect is scheduled.
std::unique_ptr<BrokerClient> CreateBrokerClient() noexcept;

}  // namespace rimes::windows::tsf
