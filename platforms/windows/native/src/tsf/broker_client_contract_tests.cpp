#include "BrokerClient.h"
#include "key_routing.hpp"
#include "../core/buffer_keys.hpp"

#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

// Deterministic console contract checks for BrokerClient while disconnected.
// Does not launch or wait on a Broker: unavailable keys must fail open
// immediately, must not be marked consumed, and must not invent a snapshot
// that could be applied later as delayed replay.

namespace {

int g_failures = 0;

void Fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

using rimes::windows::tsf::BrokerClient;
using rimes::windows::tsf::BrokerInputState;
using rimes::windows::tsf::BrokerKeyEvent;
using rimes::windows::tsf::BrokerKeyPhase;
using rimes::windows::tsf::BrokerKeyResult;
using rimes::windows::tsf::CreateBrokerClient;

void ExpectUnavailable(BrokerClient* client, BrokerKeyPhase phase,
                       WPARAM virtual_key, LPARAM key_data,
                       std::string_view label) {
  BrokerInputState state;
  state.has_snapshot = true;  // poison; Unavailable must clear/replace
  state.composing = true;
  state.composition = L"stale";
  state.commit_text = L"stale_commit";
  const BrokerKeyResult result =
      client->HandleKey({phase, virtual_key, key_data}, &state);
  Expect(result == BrokerKeyResult::kUnavailable, label);
  Expect(!state.has_snapshot,
         "unavailable must not leave a snapshot for later apply/replay");
  Expect(!state.composing,
         "unavailable must not retain a stale composing flag");
  Expect(state.composition.empty(),
         "unavailable must not retain composition text");
  Expect(state.commit_text.empty(),
         "unavailable must not retain commit text");
}

int RunContractTests() {
  using namespace rimes::windows;
  const auto send_mods = core::ShortcutModifiers(MOD_ALT | MOD_SHIFT);
  Expect(core::IsBufferSendChord(VK_RETURN, send_mods), "default send is Alt+Shift+Enter");
  Expect(core::IsBufferSendChord(VK_RETURN, send_mods | unsigned(core::KeyModifiers::kCapsLock) |
                                  unsigned(core::KeyModifiers::kNumLock)), "lock state does not affect send prediction");
  for (const auto extra : {core::KeyModifiers::kControl, core::KeyModifiers::kWindows, core::KeyModifiers::kAltGr})
    Expect(!core::IsBufferSendChord(VK_RETURN, send_mods | unsigned(extra)), "extra modifiers do not alias send");
  Expect(!core::IsBufferSendChord(VK_RETURN, unsigned(core::KeyModifiers::kControl)), "old Ctrl+Enter is not a default alias");
  Expect(core::IsBufferSendChord('J', core::ShortcutModifiers(MOD_CONTROL | MOD_SHIFT), 'J', MOD_CONTROL | MOD_SHIFT),
         "custom key and modifiers use the same matcher as Broker");
  Expect(!core::IsBufferSendChord('J', 0, 'J', MOD_CONTROL | MOD_SHIFT), "bare letter never aliases a configured modified send");
  Expect(core::BufferPluginCycleDirection(VK_UP, send_mods) == -1 && core::BufferPluginCycleDirection(VK_DOWN, send_mods) == 1, "default plugin directions");
  Expect(core::BufferPluginCycleDirection('K', core::ShortcutModifiers(6), 'K', 6, 'J', 3) == -1 &&
         core::BufferPluginCycleDirection('J', core::ShortcutModifiers(3), 'K', 6, 'J', 3) == 1, "custom plugin matcher");
  Expect(!core::BufferPluginCycleDirection(VK_DOWN, send_mods, 'K', 6, 'J', 3), "removed defaults not aliases");
  Expect(!core::BufferPluginCycleDirection(VK_UP, send_mods | unsigned(core::KeyModifiers::kAltGr)), "AltGr never aliases plugin switching");
  using rimes::windows::tsf::detail::RoutePrintableKey;
  const auto shift = static_cast<std::uint32_t>(
      rimes::windows::core::KeyModifiers::kShift);
  for (const auto key : {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'}) {
    Expect(!RoutePrintableKey(key, 0, false, false, false, false, false),
           "idle number row is never claimed before engine pass-through");
    Expect(RoutePrintableKey(key, shift, false, false, false, false, false),
           "Shift+digit retains schema punctuation");
    Expect(RoutePrintableKey(key, 0, true, false, false, false, false),
           "number selection during composition remains an IME key");
    Expect(RoutePrintableKey(key, 0, false, true, true, false, false),
           "Buffer still captures idle ASCII numbers");
  }
  for (WPARAM key = VK_NUMPAD0; key <= VK_DIVIDE; ++key) {
    for (const auto modifiers : {std::uint32_t{0}, shift}) {
      Expect(!RoutePrintableKey(key, modifiers, false, false, false, false, false),
             "idle keypad digits, decimal and operators stay with the native editor, including Shift");
      Expect(RoutePrintableKey(key, modifiers, true, false, false, false, false),
             "keypad commands during composition remain an IME key");
      Expect(RoutePrintableKey(key, modifiers, false, true, true, false, false),
             "Buffer still captures keypad input in ASCII mode");
    }
  }
  for (const WPARAM key : std::vector<WPARAM>{'A', 'Z', VK_SPACE, VK_OEM_2}) {
    Expect(!RoutePrintableKey(key, 0, false, false, true, false, false),
           "authoritative ASCII mode does not claim host printable keys");
    Expect(RoutePrintableKey(key, 0, false, true, true, false, false),
           "Buffer retains ASCII letters and punctuation");
  }
  Expect(RoutePrintableKey('N', 0, false, false, false, false, false),
         "Chinese letters still start a composition");
  // ascii_punct hands ASCII punctuation back to the host, so TSF must not claim
  // a key the engine is configured to decline; letters stay claimed either way.
  Expect(!RoutePrintableKey(VK_OEM_COMMA, 0, false, false, false, true, false),
         "ascii_punct releases ASCII punctuation to the host");
  Expect(RoutePrintableKey(VK_OEM_COMMA, 0, false, false, false, false, false),
         "punctuation stays an IME key when ascii_punct is off");
  Expect(!RoutePrintableKey(VK_OEM_2, shift, false, false, false, true, false),
         "ascii_punct releases Shift+punctuation too");
  Expect(RoutePrintableKey('A', 0, false, false, false, true, false),
         "ascii_punct keeps Latin letters claimed for composition");
  // Caps Lock belongs to the host: the editor already produces the upper-case
  // letter, so the key must not be claimed once no composition is in flight.
  Expect(!RoutePrintableKey('A', 0, false, false, false, true, true),
         "Caps Lock lets the host emit the upper-case letter");
  Expect(RoutePrintableKey('A', 0, true, false, false, true, true),
         "Caps Lock does not interrupt an active composition");
  for (const bool ascii : {false, true}) {
    Expect(!RoutePrintableKey(VK_SPACE, 0, false, false, ascii, false, false),
           "idle space stays with the editor in both Chinese and ASCII modes");
    Expect(RoutePrintableKey(VK_SPACE, 0, true, false, ascii, false, false),
           "composition space still confirms a candidate");
  }
  auto client = CreateBrokerClient();
  Expect(client != nullptr, "CreateBrokerClient returns a client");
  if (!client) {
    return EXIT_FAILURE;
  }

  Expect(!client->IsConnected(), "fresh client is disconnected");
  Expect(!client->AsciiMode().has_value(), "disconnected client never invents an input mode");
  ExpectUnavailable(client.get(), BrokerKeyPhase::kInputModeToggle, VK_SHIFT, 0,
                    "disconnected indicator click never queues a delayed toggle");

  ExpectUnavailable(client.get(), BrokerKeyPhase::kTestKeyDown, 'N', 0,
                    "disconnected TestKeyDown is unavailable (fail-open)");
  ExpectUnavailable(client.get(), BrokerKeyPhase::kKeyDown, 'N', 1,
                    "disconnected KeyDown is unavailable (fail-open)");
  ExpectUnavailable(client.get(), BrokerKeyPhase::kTestKeyDown, 'I', 0,
                    "second disconnected letter is also unavailable");
  ExpectUnavailable(client.get(), BrokerKeyPhase::kKeyDown, 'I', 1,
                    "second disconnected KeyDown is unavailable");

  // Matching releases while never connected must not be consumed either:
  // nothing was processed, so the host keeps the key.
  BrokerInputState up_state;
  Expect(client->HandleKey({BrokerKeyPhase::kKeyUp, 'N', 0}, &up_state) ==
             BrokerKeyResult::kPassThrough,
         "KeyUp with no prior consumed KeyDown passes through");
  Expect(!up_state.has_snapshot, "unhandled KeyUp has no snapshot");

  Expect(client->HandleKey({BrokerKeyPhase::kTestKeyUp, 'N', 0}, nullptr) ==
             BrokerKeyResult::kUnavailable,
         "disconnected TestKeyUp is unavailable");

  // Never BeginConnect here: CTest must not launch Session Broker or wait.
  client->Disconnect();
  Expect(!client->IsConnected(), "Disconnect leaves the client disconnected");
  ExpectUnavailable(client.get(), BrokerKeyPhase::kKeyDown, 'A', 1,
                    "after Disconnect, keys remain unavailable");

  if (g_failures != 0) {
    std::cerr << g_failures << " broker-client contract failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cerr << "broker-client unavailable/fail-open contract ok\n";
  return EXIT_SUCCESS;
}

}  // namespace

int wmain() { return RunContractTests(); }
