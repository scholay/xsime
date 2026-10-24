#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "CandidateWindow.h"
#include "ModuleState.h"
#include "TextService.h"
#include "fake_tsf.hpp"

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

}  // namespace
#include "focus_tests.hpp"
namespace {

void DumpDocument(std::string_view label,
                  const rimes::windows::e2e::FakeDocument& document) {
  rimes::windows::tsf::CandidateSnapshot snapshot;
  rimes::windows::tsf::CandidateWindow::GetLastSnapshot(&snapshot);
  std::cerr << label << ": composing=" << document.composing
            << " preedit_units=" << document.composition.size()
            << " text_units=" << document.text.size()
            << " candidates=" << snapshot.items.size()
            << " visible=" << snapshot.visible << '\n';
}

// Change only this test thread's logical state. Never synthesize desktop input
// or change the user's hardware modifiers when exercising TSF callbacks.
class LogicalKeyboardState {
 public:
  LogicalKeyboardState() { GetKeyboardState(saved_); }
  ~LogicalKeyboardState() { SetKeyboardState(saved_); }
  void Clear() {
    BYTE state[256]{};
    SetKeyboardState(state);
  }
  void Shift(bool down) {
    BYTE state[256]{};
    GetKeyboardState(state);
    state[VK_SHIFT] = state[VK_LSHIFT] = down ? 0x80 : 0;
    SetKeyboardState(state);
  }
  void RightShift(bool down) {
    BYTE state[256]{};
    GetKeyboardState(state);
    state[VK_RSHIFT] = down ? 0x80 : 0;
    state[VK_SHIFT] = (down || (state[VK_LSHIFT] & 0x80)) ? 0x80 : 0;
    SetKeyboardState(state);
  }
  void Command(int key, bool down) {
    BYTE state[256]{};
    GetKeyboardState(state);
    state[key] = down ? 0x80 : 0;
    SetKeyboardState(state);
  }
 private:
  BYTE saved_[256]{};
};

void TapLeftShift(rimes::windows::tsf::TextService* service, ITfContext* context) {
  LogicalKeyboardState keyboard;
  keyboard.Shift(true);
  BOOL eaten = FALSE;
  service->OnTestKeyDown(context, VK_SHIFT, 0x2a0000, &eaten);
  Expect(eaten, "Shift test callback offers the real event to the engine");
  if (eaten) {
    service->OnKeyDown(context, VK_SHIFT, 0x2a0000, &eaten);
    Expect(!eaten, "unhandled Shift down remains a host modifier");
  }
  keyboard.Shift(false);
  service->OnTestKeyUp(context, VK_SHIFT, 0x2a0000, &eaten);
  Expect(eaten, "unconsumed Shift down still receives its real release");
  if (eaten) {
    service->OnKeyUp(context, VK_SHIFT, 0x2a0000, &eaten);
    Expect(!eaten, "Shift release mutation never consumes the host modifier");
  }
}

void TypeVirtualKey(rimes::windows::tsf::TextService* service,
                    ITfContext* context, WPARAM virtual_key, bool require_eaten,
                    rimes::windows::e2e::FakeDocument* document = nullptr,
                    bool dump_after_key_down = false) {
  using rimes::windows::tsf::TextService;
  BOOL eaten = FALSE;
  service->OnTestKeyDown(context, virtual_key, 0, &eaten);
  if (!eaten) {
    if (require_eaten)
      Fail("expected test key down to be consumed: vk=" + std::to_string(virtual_key));
    return;
  }
  eaten = FALSE;
  const HRESULT down = service->OnKeyDown(context, virtual_key, 0, &eaten);
  if (FAILED(down)) {
    Fail("OnKeyDown failed");
    return;
  }
  if (require_eaten && eaten == FALSE) {
    Fail("expected key down to be consumed: vk=" + std::to_string(virtual_key));
  }
  if (dump_after_key_down && document != nullptr) {
    DumpDocument("after keydown vk=" + std::to_string(virtual_key), *document);
  }
  eaten = FALSE;
  service->OnTestKeyUp(context, virtual_key, 0, &eaten);
  if (eaten) service->OnKeyUp(context, virtual_key, 0, &eaten);
}

void TypeLatin(rimes::windows::tsf::TextService* service, ITfContext* context,
               std::string_view letters,
               rimes::windows::e2e::FakeDocument* document = nullptr) {
  bool first = true;
  for (const char letter : letters) {
    const WPARAM virtual_key =
        static_cast<WPARAM>(static_cast<unsigned char>(letter) & ~0x20U);
    TypeVirtualKey(service, context, virtual_key, true, document, first);
    first = false;
  }
}

bool WaitForBroker(rimes::windows::tsf::TextService* service,
                   DWORD timeout_millis) {
  const DWORD started = GetTickCount();
  while (GetTickCount() - started < timeout_millis) {
    if (service->IsBrokerConnected()) {
      return true;
    }
    Sleep(50);
  }
  return service->IsBrokerConnected();
}

std::vector<std::wstring> CandidateTexts(bool visible_only = false) {
  rimes::windows::tsf::CandidateSnapshot snapshot;
  rimes::windows::tsf::CandidateWindow::GetLastSnapshot(&snapshot);
  std::vector<std::wstring> texts;
  texts.reserve(snapshot.items.size());
  if (visible_only) {
    for (const auto index : snapshot.visible_indices)
      if (index < snapshot.items.size()) texts.push_back(snapshot.items[index].text);
  } else
    for (const auto& item : snapshot.items) texts.push_back(item.text);
  return texts;
}

bool ContainsText(const std::vector<std::wstring>& texts,
                  std::wstring_view expected) {
  for (const std::wstring& text : texts) {
    if (text == expected) {
      return true;
    }
  }
  return false;
}

void ResetDocument(rimes::windows::e2e::FakeDocument* document) {
  *document = rimes::windows::e2e::FakeDocument{};
}

// SetContext has a bounded wait while the connection/control worker
// owns its mutex. Transport readiness alone does not mean a focus binding has
// completed. Wait for that setup boundary before sending test keys; never
// retry or replay an input event.
bool WaitForContext(rimes::windows::tsf::BrokerClient* client,
                    std::uint64_t context) {
  const auto started = GetTickCount64();
  while (GetTickCount64() - started < 2000) {
    if (client->IsConnected() && client->SetContext(context)) return true;
    Sleep(10);
  }
  return false;
}

// Documents the intentional cold/unavailable contract: keys before the Broker
// is ready fail open immediately, are never marked consumed without processing,
// and are never replayed after a later successful connect.
void CheckUnavailablePassThroughAndNoReplay(bool legacy_broker) {
  using namespace rimes::windows::tsf;
  auto client = CreateBrokerClient();
  Expect(client != nullptr, "unavailable-contract client created");
  if (!client) {
    return;
  }

  Expect(!client->IsConnected(), "contract client starts disconnected");
  BrokerInputState cold;
  Expect(client->HandleKey({BrokerKeyPhase::kTestKeyDown, 'N', 0}, nullptr) ==
             BrokerKeyResult::kUnavailable,
         "cold TestKeyDown fails open");
  Expect(client->HandleKey({BrokerKeyPhase::kKeyDown, 'N', 1}, &cold) ==
             BrokerKeyResult::kUnavailable,
         "cold KeyDown fails open");
  Expect(!cold.has_snapshot, "cold KeyDown must not invent a snapshot");
  Expect(client->HandleKey({BrokerKeyPhase::kKeyDown, 'I', 1}, &cold) ==
             BrokerKeyResult::kUnavailable,
         "second cold KeyDown fails open");
  Expect(!cold.has_snapshot, "second cold KeyDown has no snapshot to replay");

  client->BeginConnect();
  const auto connected_at = GetTickCount64();
  while (!client->IsConnected() && GetTickCount64() - connected_at < 15000) {
    Sleep(50);
  }
  Expect(client->IsConnected(), "contract client connects for positive control");
  if (!client->IsConnected()) {
    client->Disconnect();
    return;
  }
  Expect(WaitForContext(client.get(), 1001), "connected context bound");

  BrokerInputState live;
  const auto first_key_started = GetTickCount64();
  const auto first_key_result =
      client->HandleKey({BrokerKeyPhase::kKeyDown, 'N', 1}, &live);
  if (first_key_result != BrokerKeyResult::kConsumed)
    std::cerr << "first connected key result=" << static_cast<int>(first_key_result)
              << " elapsed_ms=" << GetTickCount64() - first_key_started
              << " connected=" << client->IsConnected()
              << " has_snapshot=" << live.has_snapshot << '\n';
  Expect(first_key_result == BrokerKeyResult::kConsumed,
         "connected KeyDown is processed");
  Expect(live.has_snapshot && live.composing,
         "connected KeyDown yields a live snapshot");
  Expect(live.composition == L"n" && live.commit_text.empty(),
         "first connected key has no replay of unavailable NI");
  client->HandleKey({BrokerKeyPhase::kKeyUp, 'N', 0}, &live);
  // Cancel any preedit so the next reconnect assertion is unambiguous.
  BrokerInputState cancelled;
  client->HandleKey({BrokerKeyPhase::kKeyDown, VK_ESCAPE, 1}, &cancelled);
  client->HandleKey({BrokerKeyPhase::kKeyUp, VK_ESCAPE, 0}, &cancelled);

  LogicalKeyboardState keyboard;
  keyboard.Shift(true);
  Expect(client->HandleKey({BrokerKeyPhase::kTestKeyDown, VK_SHIFT, 0x2a0001}, nullptr) ==
             (legacy_broker ? BrokerKeyResult::kPassThrough : BrokerKeyResult::kConsumed),
         "independent Shift observation follows the peer capability");
  BrokerInputState armed_shift;
  Expect(client->HandleKey({BrokerKeyPhase::kKeyDown, VK_SHIFT, 0x2a0001}, &armed_shift) ==
             BrokerKeyResult::kPassThrough && !armed_shift.has_snapshot,
         "physical Shift down remains local and never owns a host key");
  client->Disconnect();
  keyboard.Shift(false);
  Expect(!client->IsConnected(), "Disconnect returns to unavailable");

  BrokerInputState dropped;
  Expect(client->HandleKey({BrokerKeyPhase::kKeyDown, 'X', 1}, &dropped) ==
             BrokerKeyResult::kUnavailable,
         "post-disconnect KeyDown fails open");
  Expect(client->HandleKey({BrokerKeyPhase::kKeyDown, 'Y', 1}, &dropped) ==
             BrokerKeyResult::kUnavailable,
         "second post-disconnect KeyDown fails open");
  Expect(!dropped.has_snapshot,
         "disconnected letters are not retained for replay");

  client->BeginConnect();
  const auto reconnected_at = GetTickCount64();
  while (!client->IsConnected() && GetTickCount64() - reconnected_at < 15000) {
    Sleep(50);
  }
  Expect(client->IsConnected(), "contract client reconnects");
  if (!client->IsConnected()) {
    client->Disconnect();
    return;
  }
  Expect(client->SetContext(1002), "fresh context after reconnect");
  BrokerInputState old_shift;
  Expect(client->HandleKey({BrokerKeyPhase::kTestKeyUp, VK_SHIFT, 0x2a0000}, nullptr) ==
             BrokerKeyResult::kPassThrough,
         "reconnected client never offers the previous session's Shift release");
  Expect(client->HandleKey({BrokerKeyPhase::kKeyUp, VK_SHIFT, 0x2a0000}, &old_shift) ==
             BrokerKeyResult::kPassThrough && !old_shift.has_snapshot,
         "armed Shift retired by disconnect is never replayed on its late release");

  BrokerInputState after;
  Expect(client->HandleKey({BrokerKeyPhase::kKeyDown, 'N', 1}, &after) ==
             BrokerKeyResult::kConsumed,
         "first key after reconnect is processed fresh");
  Expect(after.has_snapshot, "reconnect KeyDown has a snapshot");
  Expect(after.composition == L"n" && after.commit_text.empty(),
         "no delayed replay of unavailable or prior cancelled input");
  client->HandleKey({BrokerKeyPhase::kKeyUp, 'N', 0}, &after);
  Expect(client->HandleKey({BrokerKeyPhase::kKeyDown, 'I', 1}, &after) ==
             BrokerKeyResult::kConsumed,
         "second fresh key after reconnect is processed");
  Expect(after.composition == L"ni" && after.commit_text.empty(),
         "fresh reconnect composition contains exactly the new NI");
  client->HandleKey({BrokerKeyPhase::kKeyUp, 'I', 0}, &after);
  client->HandleKey({BrokerKeyPhase::kKeyDown, VK_ESCAPE, 1}, &after);
  client->HandleKey({BrokerKeyPhase::kKeyUp, VK_ESCAPE, 0}, &after);
  client->Disconnect();
}

void CheckCandidateGuardAfterKeyRelease() {
  using namespace rimes::windows::tsf;
  auto client = CreateBrokerClient();
  Expect(client != nullptr, "candidate regression client created");
  if (!client) return;
  client->BeginConnect();
  const auto started = GetTickCount64();
  while (!client->IsConnected() && GetTickCount64() - started < 15000)
    Sleep(50);
  Expect(client->IsConnected(), "candidate regression client connected");
  if (!client->IsConnected()) return;
  Expect(WaitForContext(client.get(), 1), "candidate regression context bound");
  BrokerInputState shown;
  for (const auto key : {'N', 'I'}) {
    BrokerInputState down, up;
    Expect(client->HandleKey({BrokerKeyPhase::kKeyDown,
                             static_cast<WPARAM>(key), 1}, &down) ==
               BrokerKeyResult::kConsumed,
           "candidate regression letter consumed");
    if (down.has_snapshot) shown = down;
    Expect(client->HandleKey({BrokerKeyPhase::kKeyUp,
                             static_cast<WPARAM>(key), 0}, &up) ==
               BrokerKeyResult::kConsumed,
           "candidate regression matching release consumed");
    if (up.has_snapshot) shown = up;
  }
  Expect(shown.composing && !shown.candidates.empty(),
         "candidate remains displayed after letter release");
  Expect(client->Control({{"op", "candidate_guard"},
                          {"revision", shown.revision}, {"index", 0}}),
         "current mouse candidate accepted after unhandled KeyUp");
  BrokerInputState selected, released;
  const auto selection_started = GetTickCount64();
  const auto selection_result =
      client->HandleKey({BrokerKeyPhase::kKeyDown, '1', 1}, &selected);
  if (selection_result != BrokerKeyResult::kConsumed)
    std::cerr << "candidate selection result=" << static_cast<int>(selection_result)
              << " elapsed_ms=" << GetTickCount64() - selection_started
              << " connected=" << client->IsConnected()
              << " has_snapshot=" << selected.has_snapshot << '\n';
  Expect(selection_result == BrokerKeyResult::kConsumed,
         "guarded candidate selection consumed");
  if (!shown.candidates.empty())
    Expect(selected.commit_text == shown.candidates[0].text,
           "guarded candidate commits exactly the displayed item");
  client->HandleKey({BrokerKeyPhase::kKeyUp, '1', 0}, &released);
  Expect(!client->Control({{"op", "candidate_guard"},
                           {"revision", shown.revision}, {"index", 0}}),
         "old candidate rejected after commit");
  client->Disconnect();
}

#include "chord_tests.hpp"

int RunTypingScenarios(bool legacy_broker, bool chord = false) {
  using rimes::windows::e2e::FakeContext;
  using rimes::windows::e2e::FakeDocument;
  using rimes::windows::e2e::FakeThreadMgr;
  using rimes::windows::tsf::CandidateSnapshot;
  using rimes::windows::tsf::CandidateWindow;
  using rimes::windows::tsf::TextService;
  using rimes::windows::tsf::module::SetInstance;

  SetInstance(GetModuleHandleW(nullptr));
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
    std::cerr << "CoInitializeEx failed\n";
    return EXIT_FAILURE;
  }

  FakeDocument document;
  auto* thread_manager = new FakeThreadMgr();
  auto* context = new FakeContext(&document);
  auto* service = new TextService();

  const HRESULT activated = service->Activate(thread_manager, 1);
  if (FAILED(activated)) {
    std::cerr << "TextService::Activate failed\n";
    service->Release();
    context->Release();
    thread_manager->Release();
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    return EXIT_FAILURE;
  }

  if (!WaitForBroker(service, 15000)) {
    Fail("TSF client did not connect to the Broker within 15s");
    service->Deactivate();
    service->Release();
    context->Release();
    thread_manager->Release();
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    return EXIT_FAILURE;
  }

  CheckFocusRestoration();
  LogicalKeyboardState scenario_keyboard;
  scenario_keyboard.Clear();
  if (chord) {
    CheckFirstChord(service, context, document);
    service->Deactivate();
    service->Release();
    context->Release();
    thread_manager->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  std::cerr << "caps_lock=" << ((GetKeyState(VK_CAPITAL) & 1) != 0)
            << " shift=" << ((GetKeyState(VK_SHIFT) & 0x8000) != 0) << '\n';

  CheckUnavailablePassThroughAndNoReplay(legacy_broker);
  CheckCandidateGuardAfterKeyRelease();

  // The first probe may run under a host-owned text lock. Do not bypass a
  // declined probe in the fixture: TSF would deliver that letter to the app.
  for (int focus = 0; focus < 2; ++focus) {
    service->OnSetFocus(FALSE);
    ResetDocument(&document);
    context->refuse_read_edits = true;
    BOOL first_eaten = FALSE;
    service->OnTestKeyDown(context, 'N', 1, &first_eaten);
    context->refuse_read_edits = false;
    Expect(first_eaten, "locked first probe retains the first physical letter");
    if (first_eaten) {
      service->OnKeyDown(context, 'N', 1, &first_eaten);
      Expect(first_eaten && document.composing && document.composition == L"n",
             "real first letter starts marked text before the next key");
      service->OnTestKeyUp(context, 'N', 1, &first_eaten);
      if (first_eaten) service->OnKeyUp(context, 'N', 1, &first_eaten);
    }
    TypeLatin(service, context, "ihao");
    TypeVirtualKey(service, context, VK_SPACE, true);
    Expect(document.text == L"你好" && !document.composing,
           "real Rime commits the first word after a locked probe and refocus");
  }
  ResetDocument(&document);

  if (legacy_broker) {
    TypeLatin(service, context, "ni");
    const auto old_preedit = document.composition;
    LogicalKeyboardState keyboard;
    keyboard.Shift(true);
    BOOL eaten = TRUE;
    service->OnTestKeyDown(context, VK_SHIFT, 0x2a0000, &eaten);
    Expect(!eaten, "legacy Broker does not offer Shift down");
    service->OnKeyDown(context, VK_SHIFT, 0x2a0000, &eaten);
    Expect(!eaten, "legacy Broker does not consume physical Shift down");
    keyboard.Shift(false);
    service->OnTestKeyUp(context, VK_SHIFT, 0x2a0000, &eaten);
    Expect(!eaten, "legacy Broker does not offer Shift up");
    service->OnKeyUp(context, VK_SHIFT, 0x2a0000, &eaten);
    Expect(!eaten && document.text.empty() && document.composing &&
               document.composition == old_preedit,
           "legacy Broker keeps preedit and never receives new modifier requests");
    TypeLatin(service, context, "hao");
    TypeVirtualKey(service, context, VK_SPACE, true);
    Expect(document.text == L"你好" && !document.composing,
           "legacy peer commits subsequent Chinese input without a late Shift response");
    ResetDocument(&document);
    std::cout << "New TSF client / legacy Broker Shift compatibility passed\n";
  } else {
    // Product defaults intentionally configure right Shift as noop; preserve
    // the scheme's style rather than forcing both sides to toggle ASCII.
    TypeLatin(service, context, "ni");
    const auto right_preedit = document.composition;
    {
      LogicalKeyboardState keyboard;
      keyboard.RightShift(true);
      BOOL eaten = FALSE;
      service->OnTestKeyDown(context, VK_SHIFT, 0x360000, &eaten);
      Expect(eaten, "right Shift scan code observes a qualified physical tap");
      service->OnKeyDown(context, VK_SHIFT, 0x360000, &eaten);
      Expect(!eaten, "right Shift down remains a host modifier");
      keyboard.RightShift(false);
      service->OnTestKeyUp(context, VK_SHIFT, 0x360000, &eaten);
      Expect(eaten, "right Shift observes its matching unconsumed release");
      service->OnKeyUp(context, VK_SHIFT, 0x360000, &eaten);
      Expect(!eaten && document.text.empty() && document.composing &&
                 document.composition == right_preedit,
             "right Shift noop style preserves preedit without committing or toggling ASCII");
    }
    TypeLatin(service, context, "hao");
    TypeVirtualKey(service, context, VK_SPACE, true);
    Expect(document.text == L"你好", "right Shift noop preserves subsequent Chinese typing");
    ResetDocument(&document);
    TypeLatin(service, context, "ni");
    TapLeftShift(service, context);
    Expect(document.text == L"ni" && !document.composing,
           "Shift release commits existing raw code immediately, exactly once");
    CandidateSnapshot shifted;
    CandidateWindow::GetLastSnapshot(&shifted);
    Expect(!shifted.visible, "Shift raw-code commit retires candidates");
    BOOL ascii_eaten = TRUE;
    service->OnTestKeyDown(context, 'A', 0, &ascii_eaten);
    Expect(!ascii_eaten, "ASCII mode does not pre-claim a native edit letter");
    service->OnKeyDown(context, 'A', 0, &ascii_eaten);
    Expect(!ascii_eaten && document.text == L"ni",
           "ASCII mode passes the next letter through without delayed raw-code commit");
    service->OnKeyUp(context, 'A', 0, &ascii_eaten);
    for (const WPARAM key : {VK_SPACE, VK_OEM_COMMA, VK_OEM_PERIOD}) {
      service->OnTestKeyDown(context, key, 0, &ascii_eaten);
      Expect(!ascii_eaten, "ASCII spaces and punctuation stay with the host at OnTest");
      service->OnKeyDown(context, key, 0, &ascii_eaten);
      Expect(!ascii_eaten && document.text == L"ni",
             "ASCII spaces and punctuation cannot be swallowed by the IME");
      service->OnKeyUp(context, key, 0, &ascii_eaten);
    }
    TapLeftShift(service, context);
    ResetDocument(&document);
    const auto idle_writes = context->write_requests;
    context->refuse_write_edits = true;
    TapLeftShift(service, context);
    Expect(context->write_requests == idle_writes,
           "idle Shift mode toggle never asks the native editor for a write lock");
    service->OnTestKeyDown(context, 'A', 0, &ascii_eaten);
    Expect(!ascii_eaten, "idle Shift ASCII mode passes letters through at OnTest");
    TapLeftShift(service, context);
    context->refuse_write_edits = false;
    TypeLatin(service, context, "nihao");
    TypeVirtualKey(service, context, VK_SPACE, true);
    Expect(document.text == L"你好",
           "Chinese typing resumes in the same context after an idle Shift toggle");
    ResetDocument(&document);
  }

  if (!legacy_broker) {
    // OnTest=false can suppress the real callback. Filtered F1/commands must
    // cancel locally without sending those host shortcuts or a neutral key.
    for (const auto& [command, key] : std::vector<std::pair<int, WPARAM>>{
             {0, VK_F1}, {VK_CONTROL, 'A'}, {VK_CONTROL, VK_RETURN}}) {
      TypeLatin(service, context, "ni");
      const auto before = document.composition;
      LogicalKeyboardState keyboard;
      keyboard.Shift(true);
      BOOL eaten = FALSE;
      service->OnTestKeyDown(context, VK_SHIFT, 0x2a0000, &eaten);
      Expect(eaten, "Shift observation begins before a host shortcut");
      service->OnKeyDown(context, VK_SHIFT, 0x2a0000, &eaten);
      if (command) keyboard.Command(command, true);
      service->OnTestKeyDown(context, key, 0, &eaten);
      Expect(!eaten, "Shift host shortcut test passes through without IPC");
      if (command) keyboard.Command(command, false);
      keyboard.Shift(false);
      service->OnTestKeyUp(context, VK_SHIFT, 0x2a0000, &eaten);
      Expect(eaten, "canceled Shift still observes its matching release");
      service->OnKeyUp(context, VK_SHIFT, 0x2a0000, &eaten);
      Expect(!eaten && document.text.empty() && document.composing &&
                 document.composition == before,
             "canceled Shift shortcut cannot commit raw text or toggle ASCII");
      TypeLatin(service, context, "hao");
      TypeVirtualKey(service, context, VK_SPACE, true);
      Expect(document.text == L"你好", "typing continues after canceled Shift shortcut");
      ResetDocument(&document);
    }
    for (const int cancellation : {0, 1, 2}) {
      TypeLatin(service, context, "ni");
      const auto before = document.composition;
      LogicalKeyboardState keyboard;
      keyboard.Shift(true);
      BOOL eaten = FALSE;
      service->OnTestKeyDown(context, VK_SHIFT, 0x2a0000, &eaten);
      service->OnKeyDown(context, VK_SHIFT, 0x2a0000, &eaten);
      if (cancellation == 0) {
        // Only this test waits: physical holds must not become synthetic taps.
        Sleep(510);
      } else if (cancellation == 1) {
        service->OnTestKeyDown(context, VK_SHIFT, 0x402a0001, &eaten);
        service->OnKeyDown(context, VK_SHIFT, 0x402a0001, &eaten);
      } else {
        keyboard.RightShift(true);
        service->OnTestKeyDown(context, VK_SHIFT, 0x360000, &eaten);
        service->OnKeyDown(context, VK_SHIFT, 0x360000, &eaten);
        keyboard.RightShift(false);
        service->OnTestKeyUp(context, VK_SHIFT, 0x360000, &eaten);
        service->OnKeyUp(context, VK_SHIFT, 0x360000, &eaten);
      }
      keyboard.Shift(false);
      service->OnTestKeyUp(context, VK_SHIFT, 0x2a0000, &eaten);
      service->OnKeyUp(context, VK_SHIFT, 0x2a0000, &eaten);
      Expect(!eaten && document.text.empty() && document.composition == before,
             "held, repeated or dual Shift cannot become an ASCII tap");
      TypeLatin(service, context, "hao");
      TypeVirtualKey(service, context, VK_SPACE, true);
      Expect(document.text == L"你好", "typing continues after held/repeated/dual Shift");
      ResetDocument(&document);
    }
    {
      TypeLatin(service, context, "ni");
      LogicalKeyboardState keyboard;
      keyboard.Shift(true);
      BOOL eaten = FALSE;
      service->OnTestKeyDown(context, VK_SHIFT, 0x2a0000, &eaten);
      service->OnKeyDown(context, VK_SHIFT, 0x2a0000, &eaten);
      service->OnSetFocus(FALSE);
      ResetDocument(&document);
      service->OnSetFocus(TRUE);
      keyboard.Shift(false);
      TypeLatin(service, context, "nihao");
      service->OnTestKeyUp(context, VK_SHIFT, 0x2a0000, &eaten);
      Expect(!eaten, "retired context never owns the old Shift release");
      service->OnKeyUp(context, VK_SHIFT, 0x2a0000, &eaten);
      Expect(!eaten && document.text.empty(), "focus retirement never replays Shift into a new session");
      TypeVirtualKey(service, context, VK_SPACE, true);
      Expect(document.text == L"你好", "new context preserves normal Chinese input after old Shift release");
      ResetDocument(&document);
    }
    std::cout << "Qualified Shift gesture cancellation assertions passed\n";
  }

  TypeLatin(service, context, "nihao");
  const auto before_backspace = document.composition;
  TypeVirtualKey(service, context, VK_BACK, true);
  Expect(document.composing && document.composition != before_backspace &&
             document.text.empty(), "Backspace edits preedit without committing");
  TypeVirtualKey(service, context, VK_ESCAPE, true);
  ResetDocument(&document);
  TypeLatin(service, context, "nihao");
  TypeVirtualKey(service, context, VK_RETURN, true);
  Expect(document.text == L"nihao" && !document.composing,
         "fixture Return binding commits raw input once");
  ResetDocument(&document);
  for (const WPARAM key : {VK_BACK, VK_RETURN, VK_SPACE}) {
    BOOL idle_eaten = TRUE;
    service->OnTestKeyDown(context, key, 0, &idle_eaten);
    Expect(!idle_eaten, "idle Backspace/Return/Space stays with the host at OnTest");
    service->OnKeyDown(context, key, 0, &idle_eaten);
    Expect(!idle_eaten && document.text.empty(),
           "idle host key produces no IME insertion");
  }
  for (WPARAM key : std::vector<WPARAM>{'0', '1', '2', '3', '4', '5', '6',
                                      '7', '8', '9', VK_NUMPAD0, VK_NUMPAD9}) {
    BOOL idle_eaten = TRUE;
    service->OnTestKeyDown(context, key, 0, &idle_eaten);
    Expect(!idle_eaten, "idle digits are not claimed by the test callback");
    service->OnKeyDown(context, key, 0, &idle_eaten);
    Expect(!idle_eaten && document.text.empty(),
           "idle digit callback does not mutate the document or engine");
  }
  {
    LogicalKeyboardState keyboard;
    for (const bool shifted : {false, true}) {
      keyboard.Shift(shifted);
      for (WPARAM key = VK_NUMPAD0; key <= VK_DIVIDE; ++key) {
        BOOL keypad_eaten = TRUE;
        service->OnTestKeyDown(context, key, 0, &keypad_eaten);
        Expect(!keypad_eaten, "idle keypad digits, decimal and operators are not pre-claimed, even with Shift");
        service->OnKeyDown(context, key, 0, &keypad_eaten);
        Expect(!keypad_eaten && document.text.empty() && !document.composing,
               "idle keypad input leaves the editor and Rime session untouched");
        service->OnTestKeyUp(context, key, 0, &keypad_eaten);
        Expect(!keypad_eaten, "pass-through keypad input never owns a later release");
        service->OnKeyUp(context, key, 0, &keypad_eaten);
        Expect(!keypad_eaten, "keypad release remains with the host");
      }
    }
    keyboard.Shift(false);
    TypeLatin(service, context, "nihao");
    TypeVirtualKey(service, context, VK_SPACE, true);
    Expect(document.text == L"你好", "Chinese typing survives idle keypad pass-through");
    ResetDocument(&document);
  }
  {
    LogicalKeyboardState keyboard;
    for (const int modifier : {VK_CONTROL, VK_MENU, VK_LWIN}) {
      keyboard.Command(modifier, true);
      for (const WPARAM key : std::vector<WPARAM>{'A', 'C', 'V', VK_RETURN, VK_BACK, VK_ESCAPE}) {
        BOOL command_eaten = TRUE;
        service->OnTestKeyDown(context, key, 0, &command_eaten);
        Expect(!command_eaten, "host command is not claimed by OnTestKeyDown");
        service->OnKeyDown(context, key, 0, &command_eaten);
        Expect(!command_eaten && document.text.empty(),
               "host command cannot insert or delete IME text");
      }
      keyboard.Command(modifier, false);
    }
  }

  TypeLatin(service, context, "nihao");
  CandidateSnapshot navigation;
  CandidateWindow::GetLastSnapshot(&navigation);
  const auto inline_before_arrows = document.composition;
  Expect(navigation.items.size() > 1, "candidate navigation fixture has multiple words");
  const auto second_word = navigation.items.size() > 1 ? navigation.items[1].text : L"";
  TypeVirtualKey(service, context, VK_RIGHT, true);
  CandidateWindow::GetLastSnapshot(&navigation);
  Expect(navigation.highlighted == 1 && document.composition == inline_before_arrows,
         "Right selects the next candidate without moving the preedit caret");
  TypeVirtualKey(service, context, VK_SPACE, true);
  Expect(document.text == second_word && !document.composing,
         "Space commits the candidate selected with horizontal arrows");
  ResetDocument(&document);
  TypeLatin(service, context, "nihao");
  TypeVirtualKey(service, context, VK_DOWN, true);
  CandidateWindow::GetLastSnapshot(&navigation);
  Expect(navigation.matrix_rows.size() >= 2 && navigation.matrix_rows.size() <= 3,
         "Down expands consecutive engine pages into a matrix");
  TypeVirtualKey(service, context, VK_UP, true);
  CandidateWindow::GetLastSnapshot(&navigation);
  Expect(navigation.matrix_rows.empty() && document.composing,
         "Up at the first row collapses the matrix and retains inline preedit");
  TypeVirtualKey(service, context, VK_DOWN, true);
  CandidateWindow::GetLastSnapshot(&navigation);
  const auto second_row_start = navigation.matrix_rows.empty() ? 0 : navigation.matrix_rows[0];
  const auto second_row_word = second_row_start < navigation.items.size()
      ? navigation.items[second_row_start].text : L"";
  TypeVirtualKey(service, context, VK_DOWN, true);
  TypeVirtualKey(service, context, '1', true);
  Expect(document.text == second_row_word && !document.composing,
         "matrix number key commits from the active row, not the first page");
  ResetDocument(&document);

  TypeLatin(service, context, "nihao", &document);
  DumpDocument("after nihao", document);
  Expect(document.composing, "nihao should start an inline composition");
  Expect(!document.composition.empty(),
         "nihao should produce a non-empty preedit");
  const std::vector<std::wstring> after_nihao = CandidateTexts();
  Expect(!after_nihao.empty(), "nihao should show a candidate page");
  Expect(ContainsText(after_nihao, L"你好"),
         "nihao candidates should include 你好");
  CandidateSnapshot snapshot;
  CandidateWindow::GetLastSnapshot(&snapshot);
  Expect(snapshot.visible, "candidate window snapshot should be visible");
  Expect(snapshot.caret_rect.left == document.caret_rect.left &&
             snapshot.caret_rect.top == document.caret_rect.top,
         "candidate window should use ITfContextView::GetTextExt caret");

  TypeVirtualKey(service, context, VK_SPACE, true);
  Expect(document.last_commit == L"你好", "Space should commit 你好");
  Expect(document.text == L"你好", "document text should contain 你好");
  Expect(!document.composing, "Space should end the composition");
  CandidateWindow::GetLastSnapshot(&snapshot);
  Expect(!snapshot.visible, "candidate window should hide after commit");

  ResetDocument(&document);
  TypeLatin(service, context, "nihao");
  const std::vector<std::wstring> before_number = CandidateTexts();
  Expect(before_number.size() >= 2,
         "nihao should offer at least two candidates");
  const std::wstring second =
      before_number.size() >= 2 ? before_number[1] : std::wstring();
  TypeVirtualKey(service, context, static_cast<WPARAM>('2'), true);
  Expect(document.last_commit == second,
         "number 2 should commit the second candidate");
  Expect(!document.composing, "number selection should end the composition");

  ResetDocument(&document);
  TypeLatin(service, context, "nihao");
  const std::vector<std::wstring> page_one = CandidateTexts(true);
  TypeVirtualKey(service, context, VK_NEXT, true);
  const std::vector<std::wstring> page_two = CandidateTexts(true);
  Expect(!page_one.empty() && !page_two.empty(),
         "paging should keep a candidate page visible");
  Expect(page_one != page_two,
         "PageDown should replace the visible candidate subpage");
  Expect(document.composing, "paging should keep the composition active");
  TypeVirtualKey(service, context, VK_PRIOR, true);
  Expect(CandidateTexts(true) == page_one,
         "PageUp restores the preceding visible candidate subpage");
  TypeVirtualKey(service, context, VK_ESCAPE, true);
  Expect(document.text.empty(), "Escape after paging should not commit");
  Expect(!document.composing, "Escape should cancel the composition");
  CandidateWindow::GetLastSnapshot(&snapshot);
  Expect(!snapshot.visible, "Escape should hide the candidate window");

  ResetDocument(&document);
  TypeLatin(service, context, "nihao");
  TypeVirtualKey(service, context, VK_ESCAPE, true);
  Expect(document.text.empty() && document.last_commit.empty(),
         "Escape during preedit should not commit");
  Expect(!document.composing, "Escape should clear composing state");

  // Native Edit controls may terminate preedit before notifying focus loss.
  for (int iteration = 0; iteration < 25; ++iteration) {
    ResetDocument(&document);
    TypeLatin(service, context, "ni");
    context->TerminateComposition();
    Expect(document.text.empty() && document.composition.empty() &&
               !document.composing,
           "host termination must erase preedit before it becomes raw text");
    // The minimal E2E dictionary contains nihao and ni, not standalone hao.
    TypeLatin(service, context, "nihao");
    TypeVirtualKey(service, context, VK_SPACE, true);
    Expect(document.text == L"你好",
           "host termination must reset the old engine context too");
  }

  // A host that refuses every caret source must not move the popup to the
  // screen origin. QueryCaretRect yields an all-zero rectangle, which used to
  // be indistinguishable from a caret at (0, 0) and produced a visible jump to
  // the top-left corner before the real caret arrived on the next frame.
  ResetDocument(&document);
  TypeLatin(service, context, "ni");
  CandidateWindow::GetLastSnapshot(&snapshot);
  Expect(snapshot.visible, "candidate window is visible before the host refuses");
  const RECT known_caret = snapshot.caret_rect;
  Expect(known_caret.left != 0 || known_caret.top != 0,
         "the baseline caret is anchored next to the input field");
  document.refuse_caret = true;
  TypeLatin(service, context, "hao");
  CandidateWindow::GetLastSnapshot(&snapshot);
  Expect(snapshot.visible,
         "an unknown caret must not hide an otherwise valid candidate page");
  Expect(snapshot.caret_rect.left == known_caret.left &&
             snapshot.caret_rect.top == known_caret.top,
         "an unresolvable caret must reuse the previous placement, not (0, 0)");
  Expect(!(snapshot.caret_rect.left == 0 && snapshot.caret_rect.top == 0),
         "the candidate window must never be anchored at the screen origin");
  document.refuse_caret = false;
  TypeVirtualKey(service, context, VK_SPACE, true);
  Expect(document.last_commit == L"你好",
         "a host that briefly hides its caret must not break composition");

  // Same-thread document changes do not require a BOOL foreground-loss event.
  // Exercise the actual DocumentMgr/GetTop -> BindContext -> RevokeContext path,
  // and a key-context change arriving before a focus notification.
  {
    using rimes::windows::e2e::FakeDocumentMgr;
    FakeDocument next_document;
    next_document.caret_rect = {620, 430, 622, 454};
    auto* next_context = new FakeContext(&next_document);
    auto* first_manager = new FakeDocumentMgr(context);
    auto* next_manager = new FakeDocumentMgr(next_context);
    ITfDocumentMgr* owner = nullptr;
    Expect(SUCCEEDED(context->GetDocumentMgr(&owner)) && owner == first_manager,
           "caret focus fixture attaches the first context to its document stack");
    if (owner) owner->Release();
    thread_manager->SetFocus(first_manager);
    service->OnSetFocus(first_manager, nullptr);
    ResetDocument(&document);
    TypeLatin(service, context, "ni");
    CandidateWindow::GetLastSnapshot(&snapshot);
    const RECT first_caret = snapshot.caret_rect;
    Expect(snapshot.visible, "document A supplies a visible baseline caret");

    document.refuse_caret = true;
    service->OnSetFocus(first_manager, first_manager);
    SelectionEditRecord ordinary_selection;
    service->OnEndEdit(context, 0, &ordinary_selection);
    TypeLatin(service, context, "hao");
    CandidateWindow::GetLastSnapshot(&snapshot);
    Expect(snapshot.visible && EqualRect(&snapshot.caret_rect, &first_caret),
           "same-context focus and ordinary selection retain transient caret fallback");
    document.refuse_caret = false;
    TypeVirtualKey(service, context, VK_ESCAPE, true);
    ResetDocument(&document);
    TypeLatin(service, context, "ni");

    next_document.refuse_caret = true;
    thread_manager->SetFocus(next_manager);
    service->OnSetFocus(next_manager, first_manager);
    CandidateWindow::GetLastSnapshot(&snapshot);
    Expect(!snapshot.visible && snapshot.caret_rect.bottom == 0,
           "same-thread document focus change retires the prior cached caret immediately");
    TypeLatin(service, next_context, "ni");
    CandidateWindow::GetLastSnapshot(&snapshot);
    Expect(!snapshot.visible,
           "document B with an unresolved first caret cannot borrow document A placement");
    service->OnLayoutChange(context, TF_LC_CHANGE, nullptr);
    CandidateWindow::GetLastSnapshot(&snapshot);
    Expect(!snapshot.visible,
           "late layout callback from document A cannot republish its old caret in document B");
    next_document.refuse_caret = false;
    TypeLatin(service, next_context, "hao");
    CandidateWindow::GetLastSnapshot(&snapshot);
    Expect(snapshot.visible && EqualRect(&snapshot.caret_rect, &next_document.caret_rect),
           "document B recovers using its own caret geometry");
    TypeVirtualKey(service, next_context, VK_SPACE, true);
    Expect(next_document.last_commit == L"你好", "document B composition commits independently");

    thread_manager->SetFocus(first_manager);
    ResetDocument(&document);
    TypeLatin(service, context, "ni");  // Deliberately no OnSetFocus callback.
    CandidateWindow::GetLastSnapshot(&snapshot);
    Expect(snapshot.visible, "key-context switch binds a fresh visible document A");
    thread_manager->SetFocus(next_manager);
    ResetDocument(&next_document);
    next_document.refuse_caret = true;
    TypeLatin(service, next_context, "ni");  // BindContext is the safety boundary.
    CandidateWindow::GetLastSnapshot(&snapshot);
    Expect(!snapshot.visible,
           "key-context change without focus notification still retires the prior caret");
    next_document.refuse_caret = false;
    TypeLatin(service, next_context, "hao");
    CandidateWindow::GetLastSnapshot(&snapshot);
    Expect(snapshot.visible, "context is visible before its document stack is popped");
    next_manager->Pop(TF_POPF_ALL);
    service->OnPopContext(next_context);
    owner = nullptr;
    Expect(next_context->GetDocumentMgr(&owner) == S_FALSE && owner == nullptr,
           "popped context has no document-manager owner");
    CandidateWindow::GetLastSnapshot(&snapshot);
    Expect(!snapshot.visible && snapshot.caret_rect.bottom == 0,
           "context pop retires cached caret even while the thread remains foreground");
    next_manager->Push(next_context);
    service->OnPushContext(next_context);
    ResetDocument(&next_document);
    next_document.refuse_caret = true;
    TypeLatin(service, next_context, "ni");
    CandidateWindow::GetLastSnapshot(&snapshot);
    Expect(!snapshot.visible, "re-pushed context cannot reuse its popped caret");
    next_document.refuse_caret = false;
    TypeVirtualKey(service, next_context, VK_ESCAPE, true);
    thread_manager->SetFocus(nullptr);
    service->OnSetFocus(FALSE);
    first_manager->Release();
    next_manager->Release();
    next_context->Release();
    std::cout << "Same-thread document/key-context/pop caret isolation assertions passed\n";
  }

  // Revoked focus must retire the previous caret. A new context whose first
  // query is unavailable must stay hidden, rather than reuse another field.
  service->OnSetFocus(FALSE);
  ResetDocument(&document);
  document.refuse_caret = true;
  service->OnSetFocus(TRUE);
  TypeLatin(service, context, "ni");
  CandidateWindow::GetLastSnapshot(&snapshot);
  Expect(!snapshot.visible, "unresolved first caret after focus retirement stays hidden");
  document.refuse_caret = false;
  TypeVirtualKey(service, context, VK_ESCAPE, true);

  // An asynchronous edit accepted by RequestEditSession is still revocable.
  ResetDocument(&document);
  context->defer_edits = true;
  TypeLatin(service, context, "nihao");
  Expect(!context->delayed_edits.empty(),
         "host queued asynchronous composition edits");
  FakeDocument other_document;
  auto* other = new FakeContext(&other_document);
  TypeLatin(service, other, "nihao");
  context->DrainEdits();
  context->defer_edits = false;
  Expect(document.text.empty() && !document.composing,
         "old context edits revoked before execution");
  TypeVirtualKey(service, other, VK_SPACE, true);
  Expect(other_document.text == L"你好", "new context commits independently");
  other->read_only = true;
  BOOL protected_eaten = TRUE;
  service->OnKeyDown(other, 'N', 0, &protected_eaten);
  Expect(!protected_eaten, "read-only context never forwards input");
  other->Release();

  service->Deactivate();
  service->Release();
  context->Release();
  thread_manager->Release();
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const bool legacy = argc == 2 && std::wstring_view(argv[1]) == L"--legacy-broker";
  const bool chord = argc == 2 && std::wstring_view(argv[1]) == L"--chord";
  if (argc != 1 && !legacy && !chord) return EXIT_FAILURE;
  return RunTypingScenarios(legacy, chord);
}
