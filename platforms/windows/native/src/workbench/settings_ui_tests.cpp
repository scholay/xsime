#include "settings_ui.hpp"
#include "buffer_hotkey.hpp"
#include "window.hpp"
#include "user_lexicon.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using namespace rimes::windows;
int failures = 0;
void Check(bool value, const char* message) {
  if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

LPARAM Point(HWND window, float x, float y) {
  const float dpi = static_cast<float>(GetDpiForWindow(window));
  return MAKELPARAM(static_cast<WORD>(x * dpi / 96.0f + 0.5f),
                    static_cast<WORD>(y * dpi / 96.0f + 0.5f));
}
void Click(HWND window, const ui::DipRect& box) {
  const auto point = Point(window, (box.left + box.right) / 2,
                          (box.top + box.bottom) / 2);
  SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
  SendMessageW(window, WM_LBUTTONUP, 0, point);
}
ui::SettingsLayout Layout(HWND window, const ui::SettingsDraft& draft) {
  RECT client{};
  GetClientRect(window, &client);
  const float scale = 96.0f / static_cast<float>(GetDpiForWindow(window));
  return ui::LayoutSettings(static_cast<float>(client.right) * scale,
                             static_cast<float>(client.bottom) * scale, draft);
}

bool IsVisibleWithinFixture(HWND control, HWND fixture) {
  if (!control || !fixture) return false;
  for (HWND ancestor = control; ancestor; ancestor = GetParent(ancestor)) {
    if ((GetWindowLongPtrW(ancestor, GWL_STYLE) & WS_VISIBLE) == 0)
      return false;
    if (ancestor == fixture) return true;
  }
  return false;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, const char* message, ULONGLONG timeout = 3000) {
  const auto start = GetTickCount64();
  bool ready = predicate();
  while (!ready && GetTickCount64() - start < timeout) {
    // Focus/activation on the production UI thread can synchronously contact
    // a fixture window owned by this thread. Service those sent messages;
    // preserve posted input for the fixture's explicit dispatch barriers.
    MSG pending{};
    PeekMessageW(&pending, nullptr, 0, 0, PM_NOREMOVE);
    Sleep(2);
    ready = predicate();
  }
  Check(ready, message);
  return ready;
}

struct FixtureWindows {
  HWND buffer = nullptr, settings = nullptr;
};
FixtureWindows WindowsOnFixtureThread(DWORD thread) {
  FixtureWindows windows;
  if (!thread) return windows;
  EnumThreadWindows(thread, [](HWND window, LPARAM context) -> BOOL {
    auto& found = *reinterpret_cast<FixtureWindows*>(context);
    wchar_t kind[64]{};
    GetClassNameW(window, kind, 64);
    if (std::wstring(kind) == L"Rimes.Workbench") found.buffer = window;
    if (std::wstring(kind) == L"Rimes.SettingsHost") found.settings = window;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&windows));
  return windows;
}

bool BufferPrecedesSettings(const FixtureWindows& windows) {
  if (!windows.settings || !windows.buffer) return false;
  for (HWND next = GetWindow(windows.buffer, GW_HWNDNEXT); next;
       next = GetWindow(next, GW_HWNDNEXT))
    if (next == windows.settings) return true;
  return false;
}

constexpr UINT kFixturePosition = WM_APP + 1;
constexpr UINT kFixtureSettingsClose = WM_APP + 2;
constexpr UINT kFixtureSettingsCloseBarrier = WM_APP + 3;
constexpr UINT kFixtureStopForegroundTimer = WM_APP + 4;
struct NativePositionRequest {
  FixtureWindows windows;
  HWND insert_after = HWND_TOP;
  int x = 0, y = 0, width = 0, height = 0;
  UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
  bool positioned = false, ordered = false, inspected = false;
  HWND active = nullptr;
  LONG_PTR buffer_style = 0;
  RECT rectangle{};
};
struct NativeSettingsCloseRequest {
  FixtureWindows windows, observed;
  workbench::UiCommands* commands = nullptr;
  bool reopen = false, reopen_queued = false, closed = false;
  bool barrier_posted = false, ordered = false, inspected = false;
  LONG_PTR buffer_style = 0;
  HWND active = nullptr;
  std::atomic<bool> completed{false};
};
LRESULT CALLBACK FixturePositionProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
  if (message == kFixtureStopForegroundTimer) {
    const auto buffer = reinterpret_cast<HWND>(lparam);
    if (!buffer || GetWindowThreadProcessId(buffer, nullptr) != GetCurrentThreadId())
      return FALSE;
    const BOOL stopped = KillTimer(buffer, 1);
    // KillTimer does not remove an already queued WM_TIMER. Drain only this
    // fixture's timer messages on its owning UI thread before rebinding.
    MSG pending{};
    while (PeekMessageW(&pending, buffer, WM_TIMER, WM_TIMER, PM_REMOVE)) {}
    return stopped;
  }
  if (message == kFixtureSettingsClose) {
    auto* request = reinterpret_cast<NativeSettingsCloseRequest*>(lparam);
    if (!request) return FALSE;
    if (GetWindowThreadProcessId(request->windows.buffer, nullptr) != GetCurrentThreadId() ||
        GetWindowThreadProcessId(request->windows.settings, nullptr) != GetCurrentThreadId()) {
      request->completed = true;
      return FALSE;
    }
    // Both actions run in this dispatch. Queue reopen before closing so it
    // executes before the close callback's deferred Buffer-order message.
    if (request->reopen && request->commands)
      request->reopen_queued = request->commands->RequestSettings();
    SendMessageW(request->windows.settings, WM_CLOSE, 0, 0);
    request->closed = !IsWindow(request->windows.settings);
    request->barrier_posted =
        PostMessageW(window, kFixtureSettingsCloseBarrier, 0, lparam) != FALSE;
    if (!request->barrier_posted) request->completed = true;
    return TRUE;
  }
  if (message == kFixtureSettingsCloseBarrier) {
    auto* request = reinterpret_cast<NativeSettingsCloseRequest*>(lparam);
    if (!request) return FALSE;
    // Posted FIFO places this observation after the deferred restore. In the
    // reopen case, its Runtime notification is appended after this barrier;
    // it cannot repair an incorrect stale restoration before we observe it.
    request->observed = WindowsOnFixtureThread(GetCurrentThreadId());
    request->ordered = BufferPrecedesSettings(request->observed);
    request->buffer_style = GetWindowLongPtrW(request->observed.buffer, GWL_EXSTYLE);
    GUITHREADINFO state{sizeof(state)};
    request->inspected = GetGUIThreadInfo(GetCurrentThreadId(), &state) != FALSE;
    request->active = state.hwndActive;
    request->completed = true;
    return TRUE;
  }
  if (message == kFixturePosition) {
    auto* request = reinterpret_cast<NativePositionRequest*>(lparam);
    if (!request ||
        GetWindowThreadProcessId(request->windows.buffer, nullptr) != GetCurrentThreadId() ||
        GetWindowThreadProcessId(request->windows.settings, nullptr) != GetCurrentThreadId())
      return FALSE;
    request->positioned = SetWindowPos(request->windows.buffer, request->insert_after,
        request->x, request->y, request->width, request->height, request->flags) != FALSE;
    // Sample in the same UI dispatch as SetWindowPos. A later queued Runtime
    // update cannot repair an incorrect native order before this observation.
    request->ordered = BufferPrecedesSettings(request->windows);
    request->buffer_style = GetWindowLongPtrW(request->windows.buffer, GWL_EXSTYLE);
    GetWindowRect(request->windows.buffer, &request->rectangle);
    GUITHREADINFO state{sizeof(state)};
    request->inspected = GetGUIThreadInfo(GetCurrentThreadId(), &state) != FALSE;
    request->active = state.hwndActive;
    return TRUE;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void CloseSettingsAtUiBarrier(HWND driver, const FixtureWindows& windows,
                              workbench::UiCommands& commands,
                              NativeSettingsCloseRequest& request, bool reopen) {
  request.windows = windows;
  request.commands = &commands;
  request.reopen = reopen;
  // Posting the action first drains preexisting Runtime notifications before
  // closing. No Runtime method is called between a plain close and its barrier.
  const bool posted = driver &&
      PostMessageW(driver, kFixtureSettingsClose, 0,
                   reinterpret_cast<LPARAM>(&request));
  Check(posted, "owned UI driver queues the settings-close lifecycle action");
  if (!posted || !WaitUntil([&] { return request.completed.load(); },
          "settings close reaches its posted UI queue barrier")) return;
  Check(request.closed && request.barrier_posted,
        "settings destruction completes before the deferred-placement barrier");
  if (reopen) {
    Check(request.reopen_queued && request.observed.settings && request.ordered &&
              (request.buffer_style & WS_EX_TOPMOST),
          "reopening Settings preserves the floating Buffer before a Runtime update");
  } else {
    Check(!request.observed.settings && (request.buffer_style & WS_EX_TOPMOST),
          "settings close restores Buffer topmost at the UI barrier without a Runtime update");
  }
  Check(request.inspected && request.active != windows.buffer &&
            (request.buffer_style & WS_EX_NOACTIVATE),
        "deferred settings placement preserves Buffer no-activation");
}

void CheckSynchronousBufferPosition(HWND driver, const FixtureWindows& windows) {
  NativePositionRequest request;
  request.windows = windows;
  for (const HWND placement : {HWND_TOP, HWND_TOPMOST}) {
    request.insert_after = placement;
    Check(driver && SendMessageW(driver, kFixturePosition, 0,
                                 reinterpret_cast<LPARAM>(&request)) && request.positioned,
          "owned UI driver executes a native Buffer raise synchronously");
    Check(request.ordered && (request.buffer_style & WS_EX_TOPMOST),
          "native Buffer raise stays floating while Settings is open");
    Check(request.inspected && request.active != windows.buffer &&
              (request.buffer_style & WS_EX_NOACTIVATE),
          "native no-activation raise keeps Buffer inactive");
  }

  const RECT original = request.rectangle;
  request.x = original.left + 12;
  request.y = original.top + 8;
  request.width = original.right - original.left + 16;
  request.height = original.bottom - original.top + 10;
  request.flags = SWP_NOZORDER | SWP_NOACTIVATE;
  Check(driver && SendMessageW(driver, kFixturePosition, 0,
                               reinterpret_cast<LPARAM>(&request)) && request.positioned &&
            request.rectangle.left == request.x && request.rectangle.top == request.y &&
            request.rectangle.right - request.rectangle.left == request.width &&
            request.rectangle.bottom - request.rectangle.top == request.height &&
            request.ordered && (request.buffer_style & WS_EX_TOPMOST),
        "Buffer movement and no-Z-order resizing retain floating placement and geometry");
  request.x = original.left;
  request.y = original.top;
  request.width = original.right - original.left;
  request.height = original.bottom - original.top;
  Check(driver && SendMessageW(driver, kFixturePosition, 0,
                               reinterpret_cast<LPARAM>(&request)) && request.positioned,
        "owned UI driver restores the Buffer geometry without changing its order");
}

void CheckBufferRestored(const FixtureWindows& windows, bool visible) {
  WaitUntil([&] {
    return windows.buffer &&
        (GetWindowLongPtrW(windows.buffer, GWL_EXSTYLE) &
         (WS_EX_TOPMOST | WS_EX_NOACTIVATE)) ==
            (WS_EX_TOPMOST | WS_EX_NOACTIVATE);
  }, "closing settings restores the Buffer's topmost and no-activation styles");
  Check(windows.buffer &&
            IsVisibleWithinFixture(windows.buffer, windows.buffer) == visible,
        "restoring Buffer topmost preserves its previous visibility");
  GUITHREADINFO state{sizeof(state)};
  Check(windows.buffer &&
            GetGUIThreadInfo(GetWindowThreadProcessId(windows.buffer, nullptr), &state) &&
            state.hwndActive != windows.buffer,
        "restoring Buffer topmost does not activate the Buffer");
}

void CheckProductionWindowPlacement(const FixtureWindows& windows) {
  if (!windows.buffer || !windows.settings) {
    Check(false, "production Buffer and settings windows exist for placement checks");
    return;
  }
  WaitUntil([&] {
    return IsVisibleWithinFixture(windows.settings, windows.settings);
  }, "settings finishes showing before its native placement is checked");
  WaitUntil([&] {
    return (GetWindowLongPtrW(windows.buffer, GWL_EXSTYLE) & WS_EX_TOPMOST) &&
           BufferPrecedesSettings(windows);
  }, "Buffer remains above ordinary Settings in the topmost window band");
  const auto buffer_style = GetWindowLongPtrW(windows.buffer, GWL_EXSTYLE);
  Check((buffer_style & (WS_EX_TOPMOST | WS_EX_NOACTIVATE)) ==
            (WS_EX_TOPMOST | WS_EX_NOACTIVATE),
        "opening settings preserves Buffer floating and no-activation roles");
  Check((GetWindowLongPtrW(windows.settings, GWL_EXSTYLE) &
         (WS_EX_TOPMOST | WS_EX_NOACTIVATE)) == 0 &&
            GetWindow(windows.settings, GW_OWNER) == nullptr,
        "production settings is a normal activatable top-level window without a topmost owner");
  Check((GetWindowLongPtrW(windows.settings, GWL_STYLE) &
         (WS_CAPTION | WS_THICKFRAME)) == (WS_CAPTION | WS_THICKFRAME),
        "settings retains native caption dragging and resizing");

  RECT original{}, client{};
  GetWindowRect(windows.settings, &original);
  RECT original_buffer{}, overlap{}, overlapped_buffer{};
  GetWindowRect(windows.buffer, &original_buffer);
  Check(MoveWindow(windows.buffer, original.left + 20, original.top + 100,
                   original_buffer.right - original_buffer.left,
                   original_buffer.bottom - original_buffer.top, TRUE) != FALSE,
        "fixture positions the retained Buffer over settings content");
  GetWindowRect(windows.buffer, &overlapped_buffer);
  Check(IntersectRect(&overlap, &original, &overlapped_buffer) &&
            BufferPrecedesSettings(windows),
        "Buffer remains floating when its native rectangle overlaps Settings");
  GUITHREADINFO state{sizeof(state)};
  Check(GetGUIThreadInfo(GetWindowThreadProcessId(windows.buffer, nullptr), &state) &&
            state.hwndActive != windows.buffer,
        "floating Buffer placement does not activate it");
  MoveWindow(windows.buffer, original_buffer.left, original_buffer.top,
             original_buffer.right - original_buffer.left,
             original_buffer.bottom - original_buffer.top, TRUE);
  GetClientRect(windows.settings, &client);
  POINT client_origin{client.left, client.top};
  ClientToScreen(windows.settings, &client_origin);
  const LONG x = original.left + (original.right - original.left) / 2;
  const LONG y = original.top + (client_origin.y - original.top) / 2;
  Check(SendMessageW(windows.settings, WM_NCHITTEST, 0,
                    MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y))) == HTCAPTION,
        "the settings title bar delegates native dragging to Windows");

  // Own disposable windows only. This exercises normal OS positioning in
  // headless CTest; it does not claim an interactive multi-monitor drag.
  Check(MoveWindow(windows.settings, original.left + 20, original.top + 20,
                   original.right - original.left, original.bottom - original.top,
                   TRUE) != FALSE,
        "production settings accepts an ordinary window move");
  RECT moved{};
  GetWindowRect(windows.settings, &moved);
  Check(moved.left == original.left + 20 && moved.top == original.top + 20,
        "settings does not clamp an ordinary move to its initial placement");
  MoveWindow(windows.settings, original.left, original.top,
             original.right - original.left, original.bottom - original.top, TRUE);
}

void TestNestedHitTargets() {
  ui::SettingsDraft draft;
  draft.page = ui::SettingsPage::kAppearance;
  for (const float width : {860.0f, 980.0f, 1300.0f}) {
    for (int index = 0; index < 4; ++index) {
      draft.theme_detail = index;
      const auto layout = ui::LayoutSettings(width, 600, draft);
      const auto& detail = layout.theme_details[static_cast<std::size_t>(index)];
      Check(ui::HitTestSettings(layout, detail.left + 1, detail.top + 1) == 400 + index,
            "details own their hit region before the parent card");
      const auto& popup = layout.theme_popover;
      Check(popup.left > layout.content.left && popup.right < width &&
                popup.bottom < layout.body.bottom,
            "theme details remain inside content and clear Save");
      Check(ui::HitTestSettings(layout, popup.left + 3, popup.top + 3) == 600,
            "popover shields underlying card clicks");
    }
  }
  draft.subpage = 1;
  const auto hidden = ui::LayoutSettings(980, 680, draft);
  Check(hidden.theme_details.empty() && hidden.theme_popover.width() == 0,
        "size subpage has no stale theme hit targets");
}

void TestOwnedClicksAndTransientDetails() {
  int saves = 0;
  int previews = 0;
  ui::ThemeId preview = ui::ThemeId::kNight;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [] { workbench::Settings s; s.close_after_send = false; return s; };
  callbacks.load_theme = [] { return ui::ThemeId::kNight; };
  callbacks.save = [&](workbench::Settings, const std::wstring&, bool, std::string*) {
    ++saves;
    return true;
  };
  callbacks.on_theme_preview = [&](ui::ThemeId value) { preview = value; ++previews; };
  // Own, disposable windows only. No Runtime, preferences, credentials, input
  // registration or installed Broker. CTest runs in the isolated build session.
  workbench::SettingsUiHost host(std::move(callbacks));
  host.Open(nullptr);
  HWND window = host.hwnd();
  Check(window != nullptr, "isolated settings host opens");
  if (!window) return;
  ui::SettingsDraft draft;
  auto layout = Layout(window, draft);
  SendMessageW(window, WM_LBUTTONUP, 0,
               Point(window, layout.body.right + 2, layout.body.bottom + 2));
  Check(host.IsOpen() && saves == 0, "unowned mouse release cannot Save");
  if (!host.IsOpen()) return;
  Click(window, layout.nav[1]);
  draft.page = ui::SettingsPage::kAppearance;
  layout = Layout(window, draft);
  Click(window, layout.theme_details[1]);
  Check(previews == 0, "theme details do not select or preview a theme");
  SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
  Check(host.IsOpen(), "Escape dismisses details before the settings window");
  if (!host.IsOpen()) return;

  const auto a = Point(window, layout.theme_cards[0].left + 4,
                       layout.theme_cards[0].top + 4);
  const auto b = Point(window, layout.theme_cards[1].left + 4,
                       layout.theme_cards[1].top + 4);
  SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, a);
  SendMessageW(window, WM_LBUTTONUP, 0, b);
  Check(previews == 0, "dragging between cards never selects the release card");
  SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, b);
  SendMessageW(window, WM_CANCELMODE, 0, 0);
  SendMessageW(window, WM_LBUTTONUP, 0, b);
  Check(previews == 0, "cancelled capture revokes a late release");

  Click(window, layout.theme_details[0]);
  Click(window, layout.theme_cards[3]);
  Check(previews == 0, "outside click dismisses details without choosing another theme");
  Click(window, layout.theme_cards[3]);
  Check(preview == ui::ThemeId::kRasta && previews == 1,
        "normal owned card click still previews exactly once");
  host.Close();
  Check(preview == ui::ThemeId::kRasta && saves == 1,
        "closing settings retains the immediately applied theme");
}
void TestCandidateSettingsControls() {
  workbench::Settings saved;
  int saves = 0;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [] { workbench::Settings s; s.candidate_count = 5; return s; };
  callbacks.save = [&](workbench::Settings s, const std::wstring&, bool, std::string*) { saved = s; ++saves; return true; };
  workbench::SettingsUiHost host(std::move(callbacks));
  host.Open(nullptr);
  const auto window = host.hwnd();
  if (!window) { Check(false, "candidate settings fixture opens"); return; }
  ShowWindow(window, SW_SHOWNOACTIVATE);
  ui::SettingsDraft draft;
  auto layout = Layout(window, draft);
  Click(window, layout.nav[1]);
  draft.page = ui::SettingsPage::kAppearance;
  layout = Layout(window, draft);
  Click(window, layout.subpage_tabs[1]);
  draft.subpage = 1;
  layout = Layout(window, draft);
  const auto vertical = GetDlgItem(window, 404);
  Check(IsVisibleWithinFixture(vertical, window), "vertical candidate option is exposed in appearance size settings");
  SendMessageW(window, WM_COMMAND, MAKEWPARAM(404, BN_CLICKED), reinterpret_cast<LPARAM>(vertical));
  host.Close();
  Check(saves == 1 && saved.candidate_count == 5 && saved.vertical_candidates,
        "candidate controls save the requested count and arrangement");
}

void TestBufferHotkeyRegistrationLifecycle() {
  // Real Win32 registration, disposable message-only windows, no synthesized
  // input. The native suite runs in the isolated build session, not a desktop
  // user session. Discover free letter chords rather than assuming availability.
  struct Fixture {
    HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, nullptr, nullptr);
    HWND contender = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
                                      HWND_MESSAGE, nullptr, nullptr, nullptr);
    ~Fixture() {
      for (int id : {100, 101, 102, 103, 104, 200})
        UnregisterHotKey(contender, id);
      if (owner) DestroyWindow(owner);
      if (contender) DestroyWindow(contender);
    }
  } fixture;
  Check(fixture.owner && fixture.contender, "hotkey fixture owns two isolated windows");
  if (!fixture.owner || !fixture.contender) return;
  struct Chord { unsigned modifiers, key; };
  std::vector<Chord> chords;
  for (unsigned key = 'A'; key <= 'Z' && chords.size() < 5; ++key) {
    const unsigned modifiers = chords.empty() ? MOD_CONTROL | MOD_ALT
        : chords.size() == 1 ? MOD_CONTROL | MOD_SHIFT : MOD_ALT | MOD_SHIFT;
    const int id = 100 + static_cast<int>(chords.size());
    if (RegisterHotKey(fixture.contender, id, modifiers | MOD_NOREPEAT, key))
      chords.push_back({modifiers, key});
  }
  Check(chords.size() == 5, "five real letter chords available in the build session");
  if (chords.size() != 5) return;
  const auto old = chords[0], busy = chords[1], next = chords[2];
  auto can_claim = [&](Chord chord) {
    const bool claimed = RegisterHotKey(fixture.contender, 200,
        chord.modifiers | MOD_NOREPEAT, chord.key) != FALSE;
    if (claimed) UnregisterHotKey(fixture.contender, 200);
    return claimed;
  };
  workbench::BufferHotkeyRegistration hotkey(fixture.owner);
  const Chord bare{0, old.key};
  const bool bare_was_free = can_claim(bare);
  Check(!hotkey.RegisterInitial(bare.modifiers, bare.key) && !hotkey.Registered() &&
            (!bare_was_free || can_claim(bare)),
        "invalid startup settings never reserve a bare typing key");
  Check(!hotkey.RegisterInitial(old.modifiers, old.key) && !hotkey.Registered(),
        "startup conflict reports unavailable rather than a fictitious active chord");
  UnregisterHotKey(fixture.contender, 100);
  Check(hotkey.RegisterInitial(old.modifiers, old.key) && !can_claim(old),
        "startup retry owns the requested real chord");
  int saves = 0;
  const Chord invalid[] = {
      {0, old.key}, {MOD_SHIFT, old.key}, {MOD_CONTROL, 'V'},
      {MOD_ALT, VK_F4},
      {MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, old.key},
      {MOD_CONTROL | MOD_SHIFT, 0}, {MOD_CONTROL | MOD_SHIFT, 'b'},
      {MOD_CONTROL | MOD_SHIFT, VK_SHIFT},
      {MOD_CONTROL | MOD_SHIFT, static_cast<unsigned>(-1)}};
  for (const auto chord : invalid) {
    const bool was_free = can_claim(chord);
    const auto result = hotkey.Update(chord.modifiers, chord.key,
                                     [&] { ++saves; return true; });
    Check(result == workbench::HotkeyUpdate::kInvalid && saves == 0 &&
              !can_claim(old) && (!was_free || can_claim(chord)),
          "unsupported modifiers/letters skip Save and registration while retaining the old chord");
  }
  const auto conflict = hotkey.Update(busy.modifiers, busy.key, [&] { ++saves; return true; });
  Check(conflict == workbench::HotkeyUpdate::kUnavailable && saves == 0 && !can_claim(old),
        "new chord conflict skips Save and leaves the old chord registered");
  Check(hotkey.Update(next.modifiers, next.key, [&] { ++saves; return true; }) ==
            workbench::HotkeyUpdate::kUnavailable && saves == 0 && !can_claim(old),
        "Alt+Shift conflict also preserves the old registration and saved settings");
  UnregisterHotKey(fixture.contender, 102);
  const auto failed = hotkey.Update(next.modifiers, next.key, [&] {
    ++saves;
    Check(!can_claim(old) && !can_claim(next), "both chords stay owned while Save runs");
    return false;
  });
  Check(failed == workbench::HotkeyUpdate::kSaveFailed && !can_claim(old) && can_claim(next),
        "failed Save releases the candidate and retains the original real registration");
  const auto saved = hotkey.Update(next.modifiers, next.key, [&] { ++saves; return true; });
  Check(saved == workbench::HotkeyUpdate::kSaved && can_claim(old) && !can_claim(next),
        "successful Save releases the old chord and owns the new chord");
  const auto next_message = MAKELPARAM(next.modifiers, next.key);
  const WPARAM current_id = hotkey.Matches(1, next_message) ? 1 : 2;
  const auto same = hotkey.Update(next.modifiers, next.key, [&] {
    ++saves;
    Check(!can_claim(next), "unchanged chord is continuously registered during Save");
    return true;
  });
  Check(same == workbench::HotkeyUpdate::kSaved && saves == 3 &&
            hotkey.Matches(current_id, next_message),
        "unchanged settings save retains the original registration id");
  Check(!hotkey.Matches(current_id == 1 ? 2 : 1, next_message) &&
            !hotkey.Matches(current_id, MAKELPARAM(old.modifiers, old.key)),
        "queued old ids and mismatched chord messages do not toggle Buffer");
  hotkey.Reset();
  Check(!hotkey.Registered() && can_claim(next), "closing the owner releases its real chord");
  workbench::BufferHotkeyRegistration previous_plugin(fixture.owner, 4);
  workbench::BufferHotkeyRegistration next_plugin(fixture.owner, 6);
  Check(previous_plugin.Sync(true, old.modifiers, old.key) && next_plugin.Sync(true, next.modifiers, next.key) &&
        !can_claim(old) && !can_claim(next), "both native plugin chords are reserved while Buffer owns input");
  Check(previous_plugin.Matches(5, MAKELPARAM(old.modifiers, old.key)) &&
        next_plugin.Matches(7, MAKELPARAM(next.modifiers, next.key)), "plugin directions own distinct native dispatch IDs");
  Check(!previous_plugin.Sync(true, busy.modifiers, busy.key) && !previous_plugin.Registered() && can_claim(old),
        "committed shortcut replacement failure releases the stale plugin chord");
  Check(next_plugin.Sync(false, next.modifiers, next.key) && !next_plugin.Registered() && can_claim(next),
        "focus loss or Buffer closure restores the plugin chord to the host");
  Check(previous_plugin.Sync(true, old.modifiers, old.key) && !can_claim(old),
        "new exact input lease can reserve its configured plugin chord again");
  previous_plugin.Reset();
  UnregisterHotKey(fixture.contender, 103); UnregisterHotKey(fixture.contender, 104);
  workbench::BufferHotkeyRegistration first_global(fixture.owner, 19), second_global(fixture.owner, 23);
  Check(first_global.RegisterInitial(old.modifiers, old.key) && second_global.RegisterInitial(next.modifiers, next.key), "global group starts with both old bindings");
  std::array<workbench::GlobalHotkeyBinding, 2> bindings{{{&first_global, chords[3].modifiers, chords[3].key}, {&second_global, busy.modifiers, busy.key}}};
  std::size_t failed_index = 0; int group_saves = 0;
  Check(workbench::UpdateGlobalHotkeys(bindings, [&] { ++group_saves; return true; }, &failed_index) == workbench::HotkeyUpdate::kUnavailable && failed_index == 1 && group_saves == 0 &&
        !can_claim(old) && !can_claim(next) && can_claim(chords[3]), "later global conflict rolls back earlier candidate and retains every old binding");
  bindings[1] = {&second_global, chords[4].modifiers, chords[4].key};
  Check(workbench::UpdateGlobalHotkeys(bindings, [] { return false; }) == workbench::HotkeyUpdate::kSaveFailed &&
        !can_claim(old) && !can_claim(next) && can_claim(chords[3]) && can_claim(chords[4]), "failed configuration write rolls back all global candidates");
  Check(workbench::UpdateGlobalHotkeys(bindings, [&] { ++group_saves; return true; }) == workbench::HotkeyUpdate::kSaved && group_saves == 1 &&
        can_claim(old) && can_claim(next) && !can_claim(chords[3]) && !can_claim(chords[4]), "successful global group saves once and activates every new binding");
  workbench::BufferHotkeyRegistration startup_conflict(fixture.owner, 29);
  Check(!startup_conflict.RegisterInitial(busy.modifiers, busy.key), "fixture keeps its startup chord unavailable");
  std::array<workbench::GlobalHotkeyBinding, 1> unchanged{{{&startup_conflict, busy.modifiers, busy.key, false}}};
  Check(workbench::UpdateGlobalHotkeys(unchanged, [&] { ++group_saves; return true; }) == workbench::HotkeyUpdate::kSaved &&
      group_saves == 2 && !startup_conflict.Registered() && !can_claim(busy),
      "unrelated setting saves without changing or stealing an unavailable startup binding");
  unchanged[0] = {&startup_conflict, old.modifiers, old.key, true};
  Check(workbench::UpdateGlobalHotkeys(unchanged, [] { return false; }) == workbench::HotkeyUpdate::kSaveFailed &&
      !startup_conflict.Registered() && can_claim(old), "explicit correction rolls back its reservation if persistence fails");
  Check(workbench::UpdateGlobalHotkeys(unchanged, [] { return true; }) == workbench::HotkeyUpdate::kSaved &&
      startup_conflict.Registered() && !can_claim(old), "explicit correction of an unavailable binding activates the chosen chord");
}

void TestBufferHotkeyModifierControl() {
  for (const unsigned modifiers : {MOD_CONTROL | MOD_SHIFT,
                                   MOD_CONTROL | MOD_ALT,
                                   MOD_ALT | MOD_SHIFT}) {
    workbench::Settings current;
    current.hotkey_modifiers = modifiers;
    current.hotkey_key = 'J';
    workbench::Settings saved;
    int saves = 0;
    workbench::SettingsUiCallbacks callbacks;
    callbacks.load = [&] { return current; };
    callbacks.save = [&](workbench::Settings value, const std::wstring&, bool,
                         std::string*) {
      saved = value;
      ++saves;
      return true;
    };
    workbench::SettingsUiHost host(std::move(callbacks));
    auto open_buffer_page = [&]() -> HWND {
      host.Open(nullptr);
      const auto window = host.hwnd();
      Check(window != nullptr, "shortcut settings fixture opens");
      if (!window) return nullptr;
      ShowWindow(window, SW_SHOWNOACTIVATE);
      ui::SettingsDraft draft;
      Click(window, Layout(window, draft).nav[2]);
      const HWND choices = GetDlgItem(window, 405);
      Check(IsVisibleWithinFixture(choices, window),
            "native modifier selector is exposed only on the shortcut page");
      Check(SendMessageW(choices, CB_GETCOUNT, 0, 0) == static_cast<LRESULT>(workbench::kBufferHotkeyChoices.size()) &&
                SendMessageW(choices, CB_GETCURSEL, 0, 0) ==
                    workbench::BufferHotkeyChoiceIndex(modifiers),
            "selector displays the complete configured chord without migration");
      return choices;
    };
    HWND choices = open_buffer_page();
    if (!choices) continue;
    for (const WPARAM key : {VK_RETURN, VK_ESCAPE}) {
      SendMessageW(choices, CB_SHOWDROPDOWN, TRUE, 0);
      Check(SendMessageW(choices, CB_GETDROPPEDSTATE, 0, 0) != 0,
            "native shortcut dropdown opens for keyboard interaction");
      MSG message{};
      message.hwnd = choices;
      message.message = WM_KEYDOWN;
      message.wParam = key;
      const bool handled = host.HandleDialogMessage(&message);
      Check(!handled && host.IsOpen() && saves == 0,
            "Return and Escape in an open combo do not Save or close settings");
      if (!handled) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      Check(SendMessageW(choices, CB_GETDROPPEDSTATE, 0, 0) == 0 &&
                host.IsOpen() && saves == 0,
            "native combo handles popup confirmation/cancellation before the settings window");
    }
    // Saving an unrelated preference must keep the legacy modifier and letter.
    host.Close();
    Check(saves == 0 && current.hotkey_modifiers == modifiers && current.hotkey_key == 'J',
          "opening and closing without edits never rewrites settings");
    choices = open_buffer_page();
    if (!choices) continue;
    const auto selected = (workbench::BufferHotkeyChoiceIndex(modifiers) + 1) % 3;
    SendMessageW(choices, CB_SETCURSEL, selected, 0);
    SendMessageW(host.hwnd(), WM_COMMAND, MAKEWPARAM(405, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(choices));
    host.Close();
    Check(saves == 1 && saved.hotkey_modifiers == workbench::kBufferHotkeyChoices[static_cast<std::size_t>(selected)].modifiers,
          "modifier selection applies before settings closes");
    choices = open_buffer_page();
    if (!choices) continue;
    SendMessageW(choices, CB_SETCURSEL, selected, 0);
    SendMessageW(host.hwnd(), WM_COMMAND, MAKEWPARAM(405, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(choices));
    host.Close();
    Check(saves == 2 && saved.hotkey_modifiers ==
              workbench::kBufferHotkeyChoices[static_cast<std::size_t>(selected)].modifiers &&
              saved.hotkey_key == 'J',
          "explicit modifier migration keeps the user's custom letter");
  }
}

void TestSendShortcutRecorder() {
  workbench::Settings current;
  current.hotkey_modifiers = MOD_CONTROL | MOD_ALT;
  current.hotkey_key = 'J';
  int saves = 0;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [&] { return current; };
  callbacks.save = [&](workbench::Settings value, const std::wstring&, bool, std::string*) {
    current = value; ++saves; return true;
  };
  workbench::SettingsUiHost host(std::move(callbacks));
  auto open = [&] {
    host.Open(nullptr);
    ui::SettingsDraft draft;
    Click(host.hwnd(), Layout(host.hwnd(), draft).nav[2]);
  };
  auto record = [&] { SendMessageW(GetDlgItem(host.hwnd(), 410), BM_CLICK, 0, 0); };
  open();
  const HWND send_key = GetDlgItem(host.hwnd(), 407);
  Check(IsVisibleWithinFixture(send_key, host.hwnd()), "send shortcut has an editable field on the same page");
  record();
  host.RecordShortcut('J', MOD_CONTROL | MOD_ALT);
  Check(host.RecordingShortcut() && saves == 0, "recorder rejects a duplicate without saving");
  host.RecordShortcut('V', MOD_CONTROL);
  Check(host.RecordingShortcut(), "recorder preserves host editing shortcuts");
  BYTE saved_keys[256]{}; GetKeyboardState(saved_keys);
  BYTE keys[256]{}; keys[VK_CONTROL] = keys[VK_SHIFT] = 0x80;
  SetKeyboardState(keys);  // Test-thread state only; never sends desktop input.
  MSG message{}; message.hwnd = GetDlgItem(host.hwnd(), 410);
  message.message = WM_KEYDOWN; message.wParam = VK_F8;
  Check(host.HandleDialogMessage(&message) && !host.RecordingShortcut() && saves == 1,
        "keyboard-first recorder applies the complete shortcut immediately");
  message.lParam = 1LL << 30;
  Check(host.HandleDialogMessage(&message) && saves == 1, "recorded-key repeats cannot activate settings controls");
  message.message = WM_KEYUP;
  Check(host.HandleDialogMessage(&message), "recorder consumes the captured release");
  SetKeyboardState(saved_keys);
  host.Close();
  Check(saves == 1 && current.hotkey_key == 'J' && current.hotkey_modifiers == (MOD_CONTROL | MOD_ALT) &&
        current.send_key == VK_F8 && current.send_modifiers == (MOD_CONTROL | MOD_SHIFT),
        "Save persists the send key independently and preserves the old global shortcut");
  open(); record();
  message = {}; message.hwnd = GetDlgItem(host.hwnd(), 410);
  message.message = WM_KEYDOWN; message.wParam = VK_ESCAPE;
  Check(host.HandleDialogMessage(&message) && host.IsOpen() && !host.RecordingShortcut(),
        "Escape cancels recording rather than closing settings");
  record(); host.RecordShortcut(VK_DELETE, 0);
  host.Close();
  Check(current.send_key == VK_RETURN && saves == 2, "closing settings retains a single shortcut reset");
  open();
  SendMessageW(GetDlgItem(host.hwnd(), 411), BM_CLICK, 0, 0);
  host.Close();
  Check(saves == 3 && current.hotkey_key == 'B' && current.hotkey_modifiers == (MOD_ALT | MOD_SHIFT) &&
        current.send_key == VK_RETURN && current.send_modifiers == (MOD_ALT | MOD_SHIFT),
        "explicit reset restores both requested Alt+Shift defaults");
}

void TestPluginShortcutControls() {
  workbench::Settings current; int saves = 0;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [&] { return current; };
  callbacks.save = [&](workbench::Settings value, const std::wstring&, bool, std::string*) { current = value; ++saves; return true; };
  workbench::SettingsUiHost host(std::move(callbacks));
  const auto open = [&] { host.OpenPage(nullptr, ui::SettingsPage::kBuffer); };
  open();
  for (int id : {418,419,420,421,422,423}) Check(IsVisibleWithinFixture(GetDlgItem(host.hwnd(), id), host.hwnd()), "plugin shortcut native controls visible");
  SendMessageW(GetDlgItem(host.hwnd(), 418), BM_CLICK, 0, 0);
  host.RecordShortcut(VK_DOWN, 5); Check(host.RecordingShortcut(), "previous cannot duplicate next");
  host.RecordShortcut('K', 6); Check(!host.RecordingShortcut() && saves == 1 && current.previous_plugin_key == 'K', "previous recording applies immediately");
  SendMessageW(GetDlgItem(host.hwnd(), 419), BM_CLICK, 0, 0);
  host.RecordShortcut('M', 5); Check(host.RecordingShortcut(), "reject reserved Mailbox shortcut");
  host.RecordShortcut('4', 5); Check(host.RecordingShortcut(), "reject reserved screenshot shortcut");
  host.RecordShortcut('J', 3); host.Close();
  Check(saves == 2 && current.previous_plugin_key == 'K' && current.previous_plugin_modifiers == 6 &&
        current.next_plugin_key == 'J' && current.next_plugin_modifiers == 3, "save independent plugin shortcuts");
  open(); wchar_t title[20]{}; GetWindowTextW(GetDlgItem(host.hwnd(), 420), title, 20);
  Check(std::wstring(title) == L"K", "reopen restores custom previous key");
  SetWindowTextW(GetDlgItem(host.hwnd(), 421), L"K");
  SendMessageW(GetDlgItem(host.hwnd(), 423), CB_SETCURSEL, workbench::BufferHotkeyChoiceIndex(6), 0);
  SendMessageW(host.hwnd(), WM_TIMER, 12, 0); Check(host.IsOpen() && saves == 2, "manual duplicate rejected without saving");
  SendMessageW(GetDlgItem(host.hwnd(), 418), BM_CLICK, 0, 0); host.RecordShortcut(VK_DELETE, 0); host.Close();
  Check(current.previous_plugin_key == VK_UP && saves == 3, "single reset is applied immediately");
  open(); SendMessageW(GetDlgItem(host.hwnd(), 411), BM_CLICK, 0, 0); host.Close();
  Check(saves == 4 && current.previous_plugin_key == VK_UP && current.next_plugin_key == VK_DOWN &&
        current.previous_plugin_modifiers == 5 && current.next_plugin_modifiers == 5, "reset includes both cycle shortcuts");
  ui::SettingsDraft draft; draft.page = ui::SettingsPage::kBuffer;
  const auto layout = ui::LayoutSettings(860, 600, draft);
  Check(layout.shortcut_feedback.bottom < layout.body.bottom, "four shortcut rows fit minimum height");
}
void TestHoverRepaintScopeAndResources() {
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [] { return workbench::Settings{}; };
  workbench::SettingsUiHost host(std::move(callbacks));
  host.Open(nullptr);
  const auto window = host.hwnd();
  Check(window != nullptr, "hover repaint fixture opens");
  if (!window) return;
  ShowWindow(window, SW_SHOWNOACTIVATE);
  UpdateWindow(window);
  ui::SettingsDraft draft;
  const auto layout = Layout(window, draft);
  auto move = [&](int row) {
    const auto& rect = layout.nav[static_cast<std::size_t>(row)];
    SendMessageW(window, WM_MOUSEMOVE, 0,
                 Point(window, (rect.left + rect.right) / 2,
                       (rect.top + rect.bottom) / 2));
  };
  auto sidebar_only = [&] {
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    const int type = GetUpdateRgn(window, region, FALSE);
    RECT dirty{};
    GetRgnBox(region, &dirty);
    const float scale = GetDpiForWindow(window) / 96.0f;
    const bool result = type != ERROR && type != NULLREGION &&
        dirty.right <= static_cast<LONG>(layout.sidebar.right * scale) + 2 &&
        !PtInRegion(region, static_cast<int>((layout.heading.left + 30) * scale),
                    static_cast<int>((layout.heading.top + 10) * scale));
    DeleteObject(region);
    return result;
  };
  // Session 0 discards HWND update regions. Do not claim these desktop
  // assertions ran there; the actual GDI bitmap assertions below still run.
  if (IsWindowVisible(window)) {
    for (const int row : {0, 1, 2, 3}) {
      move(row);
      Check(sidebar_only(), "navigation hover excludes content from the HWND dirty region");
      UpdateWindow(window);
      move(row);
      Check(!GetUpdateRect(window, nullptr, FALSE), "same hovered row does not invalidate again");
    }
    SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    Check(sidebar_only(), "mouse leave invalidates only the previous navigation highlight");
    UpdateWindow(window);
    SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    Check(!GetUpdateRect(window, nullptr, FALSE), "repeated mouse leave does not invalidate again");
    std::cout << "Desktop HWND hover dirty-region assertions executed\n";
  } else {
    std::cout << "NOT EXECUTED: desktop HWND hover dirty-region assertions (invisible Session 0 window)\n";
  }
  SendMessageW(window, WM_MOUSELEAVE, 0, 0);

  RECT client{};
  GetClientRect(window, &client);
  HDC screen = GetDC(window);
  HDC capture = CreateCompatibleDC(screen);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = client.right;
  info.bmiHeader.biHeight = -client.bottom;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr;
  HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  ReleaseDC(window, screen);
  Check(capture && bitmap && pixels, "native offscreen GDI capture allocates its bitmap and DC");
  if (!capture || !bitmap || !pixels) {
    if (bitmap) DeleteObject(bitmap);
    if (capture) DeleteDC(capture);
    host.Close();
    return;
  }
  const auto selected = SelectObject(capture, bitmap);
  const auto count = static_cast<std::size_t>(client.right) * client.bottom;
  const auto bits = static_cast<std::uint32_t*>(pixels);
  auto render = [&] {
    std::fill(bits, bits + count, 0x00112233U);
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(capture), PRF_CLIENT);
    GdiFlush();
    return std::vector<std::uint32_t>(bits, bits + count);
  };
  auto nav_bounds = [&](int row) {
    RECT rect{};
    if (row >= 0) {
      const auto& dip = layout.nav[static_cast<std::size_t>(row)];
      const auto scale = GetDpiForWindow(window) / 96.0f;
      rect = {static_cast<LONG>(dip.left * scale) - 2,
              static_cast<LONG>(dip.top * scale) - 2,
              static_cast<LONG>(dip.right * scale) + 2,
              static_cast<LONG>(dip.bottom * scale) + 2};
    }
    return rect;
  };
  auto previous = render();
  Check(std::none_of(previous.begin(), previous.end(),
                     [](auto pixel) { return pixel == 0x00112233U; }),
        "buffered settings renderer covers the entire client with a completed frame");
  int old_row = -1;
  for (const int row : {1, 2, 3, 0, -1}) {
    if (row < 0) SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    else move(row);
    const auto current = render();
    const auto old_bounds = nav_bounds(old_row), new_bounds = nav_bounds(row);
    std::size_t changed = 0;
    bool outside_changed = false;
    for (std::size_t i = 0; i < count; ++i) {
      if (current[i] == previous[i]) continue;
      ++changed;
      const POINT point{static_cast<LONG>(i % client.right),
                        static_cast<LONG>(i / client.right)};
      if (!PtInRect(&old_bounds, point) && !PtInRect(&new_bounds, point))
        outside_changed = true;
    }
    // Leaving the already-selected row need not alter its selected styling.
    if (row >= 0) Check(changed > 0, "native hover GDI output changes its navigation highlight");
    Check(!outside_changed, "hover GDI pixel changes stay inside old and new navigation rows");
    if (row < 0) SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    else move(row);
    Check(render() == current, "repeated hover state produces an identical native frame");
    previous = current;
    old_row = row;
  }

  const DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  Check(resources > 0, "GDI instrumentation observes the fixture's fonts, bitmap and DC");
  for (int i = 0; i < 64; ++i) {
    move(i % 4);
    render();  // Forces actual buffered drawing even when Session 0 is hidden.
  }
  Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= resources + 1,
        "64 actual buffered hover renders do not retain GDI bitmaps or DCs");
  std::cout << "Native GDI hover frame coverage, bounded pixel changes and resource assertions executed\n";
  SelectObject(capture, selected);
  DeleteObject(bitmap);
  DeleteDC(capture);
  host.Close();
}
void TestCodexConnectorSelector() {
  workbench::Settings original, saved;
  original.ai_connector = "codex-cli";
  original.codex_path = "C:/Program Files/Codex/codex.exe";
  original.codex_model = "fixture-model";
  original.model = "keep-api-model";
  original.hotkey_modifiers = MOD_ALT | MOD_SHIFT;
  int saves = 0;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [&] { return original; };
  callbacks.save = [&](workbench::Settings value, const std::wstring&, bool replace_key, std::string*) {
    Check(!replace_key, "CLI selection never changes API credentials");
    saved = value; ++saves; return true;
  };
  workbench::SettingsUiHost host(std::move(callbacks));
  host.Open(nullptr);
  const auto window = host.hwnd();
  ui::SettingsDraft draft;
  Click(window, Layout(window, draft).nav[3]);
  Check(!GetDlgItem(window, 6005) && IsVisibleWithinFixture(GetDlgItem(window, 6060), window) &&
      IsVisibleWithinFixture(GetDlgItem(window, 6062), window), "connector selection uses three cards");
  Check(!IsWindowEnabled(GetDlgItem(window, 6061)), "unimplemented Claude connector stays disabled");
  Check(IsVisibleWithinFixture(GetDlgItem(window,6040),window) &&
      !IsVisibleWithinFixture(GetDlgItem(window,6008),window) &&
      !IsVisibleWithinFixture(GetDlgItem(window,6006),window), "Codex detail prioritizes authorization and hides API secrets");
  SendMessageW(GetDlgItem(window,6067), BM_CLICK, 0, 0);
  Check(IsVisibleWithinFixture(GetDlgItem(window,6006),window) && saves == 0,
      "advanced CLI fields expand without changing the selected connector");
  host.Close();
  Check(saves == 0, "opening and closing the connector selector does not rewrite settings");
}

void TestPluginManagementAndChordSelection() {
  Check(std::wstring(ui::kSettingsSchemeTitles[6]) == L"isaac2026",
        "chord scheme uses its current product name");
  int changes = 0;
  std::string saved_schema;
  workbench::PluginView fixture{"builtin.apple-translation", "Translation", "1.1.0", "grant", true, true, true};
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [] { return workbench::Settings{}; };
  callbacks.plugins = [&] { return std::vector<workbench::PluginView>{fixture}; };
  callbacks.manage_plugin = [&](const std::string& id, const std::string& action, std::string*) {
    Check(id == fixture.id && action == "disable", "plugin control passes the selected identity and action");
    fixture.enabled = false; ++changes; return true;
  };
  callbacks.save = [&](workbench::Settings value, const std::wstring&, bool, std::string*) {
    saved_schema = value.schema; return true;
  };
  workbench::SettingsUiHost host(std::move(callbacks));
  host.Open(nullptr);
  const HWND window = host.hwnd();
  Check(window != nullptr, "plugin fixture window opens");
  if (!window) return;
  // SSH/CTest can inherit STARTF_USESHOWWINDOW=SW_HIDE. Explicitly show the
  // disposable fixture after Open so visibility checks cover the controls.
  ShowWindow(window, SW_SHOWNOACTIVATE);
  ui::SettingsDraft draft;
  auto layout = Layout(window, draft);
  Check(layout.nav.size() == 12, "brand home, core hosts and three native extension pages");
  Click(window, layout.nav[4]);
  const HWND toggle = GetDlgItem(window, 5101);
  // SSH/CTest's Session 0 desktop can be hidden even when these owned windows
  // have WS_VISIBLE. Verify every owned ancestor and the child's full native
  // visibility relative to its shown parent. On a visible desktop this also
  // requires IsWindowVisible(toggle); Session 0 cannot prove actual rendering.
  const bool exposed = IsVisibleWithinFixture(toggle, window) &&
                       IsWindowEnabled(toggle) &&
                       IsWindowVisible(toggle) == IsWindowVisible(window);
  if (!exposed) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    GetStartupInfoW(&startup);
    RECT client{}, button{};
    GetClientRect(window, &client);
    GetWindowRect(toggle, &button);
    std::cerr << "Plugin fixture state: parent-visible=" << IsWindowVisible(window)
              << " parent-style=" << GetWindowLongPtrW(window, GWL_STYLE)
              << " toggle-exists=" << (toggle != nullptr)
              << " toggle-visible=" << IsWindowVisible(toggle)
              << " toggle-style=" << GetWindowLongPtrW(toggle, GWL_STYLE)
              << " toggle-enabled=" << IsWindowEnabled(toggle)
              << " client=" << client.right << 'x' << client.bottom
              << " toggle-size=" << button.right - button.left << 'x'
              << button.bottom - button.top
              << " dpi=" << GetDpiForWindow(window)
              << " startup-flags=" << startup.dwFlags
              << " startup-show=" << startup.wShowWindow
              << " desktop-visible=" << IsWindowVisible(GetDesktopWindow())
              << " desktop-style=" << GetWindowLongPtrW(GetDesktopWindow(), GWL_STYLE)
              << '\n';
  }
  Check(exposed, "installed plugin exposes its switch");
  SendMessageW(toggle, BM_CLICK, 0, 0);
  Check(changes == 1 && !fixture.enabled, "switch callback occurs exactly once");
  Click(window, layout.nav[0]);
  draft.page = ui::SettingsPage::kInput;
  Check(IsVisibleWithinFixture(window, window), "navigation keeps the fixture parent shown");
  Check((GetWindowLongPtrW(toggle, GWL_STYLE) & WS_VISIBLE) == 0 &&
            !IsWindowVisible(toggle),
        "plugin controls leave other pages clear");
  layout = Layout(window, draft);
  Check(layout.scheme_cards.size() == 7, "Wubi mixing and chording have separate scheme choices");
  Click(window, layout.scheme_cards[4]);

  Check(saved_schema == "lingxi_wubi", "scheme control saves XSIME Wubi identity");
  // Choosing another schema keeps the same settings window open.
  host.OpenPage(nullptr, ui::SettingsPage::kInput);
  const HWND reopened = host.hwnd();
  layout = Layout(reopened, draft);
  Click(reopened, layout.scheme_cards[6]);

  Check(saved_schema == "my_combo", "schema control saves chording identity");
  host.Close();
}

void TestSettingsPreserveRuntimeAndStreaming() {
  std::cerr << "Runtime UI: begin\n";
  const DWORD original_size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring original(original_size, L'\0');
  if (original_size) {
    GetEnvironmentVariableW(L"LOCALAPPDATA", original.data(), original_size);
    original.resize(wcslen(original.c_str()));
  }
  const auto root = std::filesystem::temp_directory_path() /
      (L"rimes-settings-ui-test-" + std::to_wstring(GetCurrentProcessId()) +
       L"-" + std::to_wstring(GetTickCount64()));
  std::filesystem::create_directories(root);
  if (!SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str())) {
    Check(false, "runtime fixture isolates its preferences and plugin state");
    std::filesystem::remove_all(root);
    return;
  }
  {
    std::atomic<unsigned> calls{0}, completed{0};
    std::atomic<bool> release{false}, cancelled_request{false}, accepted{false};
    // Real Runtime and the production window/dispatcher; only the network
    // transport is synthetic. No credentials, input registration or host text.
    workbench::Runtime runtime([&](const workbench::Settings&,
        const workbench::Generation&, const std::function<bool(const std::string&)>& chunk,
        const std::function<bool()>& cancelled, std::string*) {
      const bool initial = chunk("Stream");
      ++calls;
      const auto start = GetTickCount64();
      while (!release && !cancelled() && GetTickCount64() - start < 15000)
        Sleep(2);
      cancelled_request = cancelled();
      accepted = initial && release && !cancelled_request && chunk(" done.");
      ++completed;
      return accepted.load();
    });
    // The daily Broker may own the production defaults on a developer desktop.
    // Give this isolated fixture available global bindings of its own.
    auto fixture_shortcuts = runtime.Configuration();
    unsigned* fixture_global_keys[] = {&fixture_shortcuts.settings_key, &fixture_shortcuts.capsule_key, &fixture_shortcuts.capture_key};
    unsigned* fixture_global_modifiers[] = {&fixture_shortcuts.settings_modifiers, &fixture_shortcuts.capsule_modifiers, &fixture_shortcuts.capture_modifiers};
    unsigned candidate_key = VK_F13;
    for (int i = 0; i < 3; ++i) {
      bool available = false;
      while (candidate_key <= VK_F24) {
        const auto key = candidate_key++;
        if (!RegisterHotKey(nullptr, 731, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, key)) continue;
        UnregisterHotKey(nullptr, 731);
        *fixture_global_keys[i] = key; *fixture_global_modifiers[i] = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
        available = true; break;
      }
      Check(available, "production UI fixture has an available isolated global shortcut");
    }
    std::string fixture_error;
    Check(runtime.Configure(fixture_shortcuts, L"", false, &fixture_error), "production UI fixture saves its own global shortcuts");
    workbench::UiCommands commands;
    Check(commands.RequestSettings(), "one cold-start settings request is accepted before the window exists");
    // Posted request pointers remain valid until the UI thread is joined,
    // including timeout/error paths; each request is used only once.
    NativeSettingsCloseRequest close_requests[3];
    std::atomic<DWORD> ui_thread{0};
    std::atomic<HWND> position_driver{nullptr};
    std::jthread ui([&] {
      ui_thread = GetCurrentThreadId();
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
      WNDCLASSW driver_class{};
      driver_class.lpfnWndProc = FixturePositionProcedure;
      driver_class.hInstance = GetModuleHandleW(nullptr);
      driver_class.lpszClassName = L"Rimes.SettingsPositionFixture";
      RegisterClassW(&driver_class);
      const HWND driver = CreateWindowExW(0, driver_class.lpszClassName, L"", 0,
          0, 0, 0, 0, HWND_MESSAGE, nullptr, driver_class.hInstance, nullptr);
      position_driver = driver;
      std::cerr << "Runtime UI: native dispatcher starting\n";
      workbench::RunWindow(runtime, [] {}, [] {}, &commands);
      std::cerr << "Runtime UI: native dispatcher stopped\n";
      position_driver = nullptr;
      if (driver) DestroyWindow(driver);
      UnregisterClassW(driver_class.lpszClassName, driver_class.hInstance);
    });
    const auto windows = [&] { return WindowsOnFixtureThread(ui_thread.load()); };
    // Cold native/tray initialization shares the production command's bounded
    // 15-second startup allowance; no hotkey is posted before that first open.
    WaitUntil([&] { const auto h = windows().settings; return h && IsWindowVisible(h); },
              "settings command opens the production settings host", 15000);
    std::cerr << "Runtime UI: cold open checked\n";
    const auto initial_settings = windows().settings;
    const auto initial_buffer = windows().buffer;
    // Real WM_HOTKEY is queued, never a cross-thread sent message that can
    // interrupt Open() while it is still presenting the initial native children.
    PostMessageW(initial_buffer, WM_HOTKEY, 20, MAKELPARAM(fixture_shortcuts.settings_modifiers, fixture_shortcuts.settings_key));
    WaitUntil([&] { return !IsWindowVisible(initial_settings); }, "queued settings shortcut finishes");
    std::cerr << "Runtime UI: shortcut hide checked\n";
    Check(windows().settings == initial_settings && !IsWindowVisible(initial_settings),
          "configured settings shortcut hides settings without destroying its draft");
    SendMessageW(initial_buffer, WM_APP + 81, 0, WM_LBUTTONUP);
    std::cerr << "Runtime UI: tray request returned\n";
    Check(windows().settings == initial_settings && IsWindowVisible(initial_settings) &&
          !runtime.Snapshot().value("visible", true),
          "tray left click opens settings without opening Buffer");
    CheckProductionWindowPlacement(windows());
    Check(!runtime.Snapshot().value("visible", true), "settings do not open a closed Buffer");
    Check(!IsVisibleWithinFixture(windows().buffer, windows().buffer),
          "opening settings leaves a closed Buffer physically hidden");
    runtime.Toggle(0);  // Own tray/hotkey behavior with no authorized host.
    WaitUntil([&] {
      const auto current = windows();
      return IsVisibleWithinFixture(current.buffer, current.buffer) &&
             BufferPrecedesSettings(current);
    }, "Buffer opened after settings stays floating without a capture target");
    Check(!runtime.Snapshot().value("capture", true),
          "floating Buffer placement does not authorize host capture");
    runtime.Close();
    WaitUntil([&] { return !IsVisibleWithinFixture(windows().buffer, windows().buffer); },
              "closing Buffer while settings is open preserves its hidden state");
    CloseSettingsAtUiBarrier(position_driver.load(), windows(), commands,
                             close_requests[0], false);
    std::cerr << "Runtime UI: first close barrier returned\n";
    WaitUntil([&] { return windows().settings == nullptr; }, "closing settings retires its own window");
    CheckBufferRestored(windows(), false);

    const auto peer = GetCurrentProcessId() + 1;
    const auto target = runtime.Register(peer, 9001, 9001);
    runtime.Focus(target);
    runtime.Bind(peer);
    // This fixture intentionally has no real foreground host. First prove
    // that the production timer revokes the synthetic peer, then isolate the
    // settings/stream lifetime checks from that unrelated 25 ms timer race.
    WaitUntil([&] { return !runtime.Capturing(target); },
              "production foreground timer revokes the synthetic host");
    Check(SendMessageW(position_driver.load(), kFixtureStopForegroundTimer, 0,
                       reinterpret_cast<LPARAM>(windows().buffer)) != FALSE,
          "owned UI driver isolates synthetic host from the foreground timer");
    runtime.Focus(target);
    runtime.Bind(peer);
    runtime.Paste("Fixture source.");
    runtime.Generate(true);
    WaitUntil([&] { return calls == 1 && runtime.Snapshot().value("busy", false); },
              "synthetic streaming request is held in flight");
    const auto before = runtime.Snapshot();
    const auto revision = runtime.Configuration().revision;
    Check(before.value("visible", false) && before.value("capture", false) &&
              before.value("preview", std::string()) == "Stream",
          "real Runtime fixture has visible captured source and a stream preview");
    Check(commands.RequestSettings(), "settings request posts while Buffer is active");
    WaitUntil([&] { return windows().settings && !runtime.Capturing(target); },
              "settings opens and pauses the previous host capture");
    CheckProductionWindowPlacement(windows());
    auto after = runtime.Snapshot();
    Check(after["visible"] == before["visible"] &&
              after["source_blocks"] == before["source_blocks"] &&
              after["result_blocks"] == before["result_blocks"] &&
              after["busy"] == before["busy"] && after["preview"] == before["preview"] &&
              after["translate"] == before["translate"] && after["status"] == before["status"],
          "opening settings preserves Buffer visibility, block identities and active request state");
    Check(!after.value("capture", true) && after.value("target_pid", 1U) == 0 &&
              runtime.Configuration().revision == revision,
          "settings pause delivery authority without applying configuration");
    WaitUntil([&] { return IsVisibleWithinFixture(windows().buffer, windows().buffer); },
              "production Buffer window remains shown while settings are open");

    CheckSynchronousBufferPosition(position_driver.load(), windows());
    runtime.Focus({});  // The real host can report focus loss after activation.
    WaitUntil([&] { return BufferPrecedesSettings(windows()); },
              "streaming runtime refresh keeps the retained Buffer floating");
    Check(runtime.Snapshot().value("busy", false) &&
              runtime.Snapshot().value("preview", std::string()) == "Stream",
          "following host focus loss preserves the already-paused request");
    const HWND first_settings = windows().settings;
    SetWindowPos(windows().buffer, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Check(commands.RequestSettings(), "repeated settings command posts");
    WaitUntil([&] {
      return windows().settings == first_settings && BufferPrecedesSettings(windows());
    }, "repeated settings request reuses the host without demoting Buffer");
    CloseSettingsAtUiBarrier(position_driver.load(), windows(), commands,
                             close_requests[1], true);
    Check(runtime.Snapshot().value("busy", false) &&
              runtime.Snapshot().value("preview", std::string()) == "Stream" &&
              runtime.Snapshot()["source_blocks"] == before["source_blocks"],
          "closing and reopening settings preserves the in-flight Buffer request");
    CloseSettingsAtUiBarrier(position_driver.load(), windows(), commands,
                             close_requests[2], false);
    WaitUntil([&] { return windows().settings == nullptr; }, "settings can dismiss while streaming continues");
    CheckBufferRestored(windows(), true);
    Check(runtime.Snapshot().value("visible", false) &&
              runtime.Snapshot().value("busy", false) &&
              runtime.Snapshot()["source_blocks"] == before["source_blocks"],
          "dismissing settings preserves source and the active request");
    release = true;
    WaitUntil([&] { return completed == 1 && !runtime.Snapshot().value("busy", true); },
              "original stream finishes after settings is dismissed");
    Check(accepted && !cancelled_request && calls == 1 &&
              runtime.Snapshot().value("result", std::string()) == "Stream done.",
          "original callback remains authorized and completes exactly once");
    runtime.Focus(target);
    runtime.Send(false);
    Check(!runtime.Capturing(target) && runtime.Snapshot().value("target_pid", 1U) == 0 &&
              runtime.Snapshot().value("result", std::string()) == "Stream done.",
          "returning to the old input field does not revive capture or consume output");
    runtime.Bind(peer);
    Check(runtime.Capturing(target), "explicit binding is required to resume host capture");

    engine::EngineSnapshot preedit;
    preedit.composition = "pending preedit";
    runtime.Capture(target, &preedit);
    Check(commands.RequestSettings(), "settings request posts while composing");
    WaitUntil([&] { return windows().settings && !runtime.Capturing(target); },
              "settings pauses an explicitly rebound input context");
    CheckProductionWindowPlacement(windows());
    Check(runtime.Snapshot().value("preedit", std::string()) == "" &&
              runtime.Snapshot().value("result", std::string()) == "Stream done." &&
              runtime.Snapshot()["source_blocks"] == before["source_blocks"],
          "capture pause clears only transient preedit and retains committed work");

    release = false;
    cancelled_request = false;
    runtime.Paste("Next source.");
    runtime.Generate(true);
    WaitUntil([&] { return calls == 2 && runtime.Snapshot().value("busy", false); },
              "second synthetic request starts for the appended source");
    auto changed = runtime.Configuration();
    changed.theme = "day";
    std::string error;
    Check(runtime.Configure(changed, L"", false, &error), "explicit settings Apply writes isolated preferences");
    WaitUntil([&] { return completed == 2; }, "configuration change retires the old request");
    Check(cancelled_request && !accepted &&
              !runtime.Snapshot().value("busy", true) &&
              runtime.Snapshot().value("preview", std::string()) == "" &&
              runtime.Snapshot().value("result", std::string()) == "" &&
              runtime.Snapshot().value("source", std::string()) == "Fixture source.Next source.",
          "actual configuration changes still invalidate old API results and retain source");
    release = true;
    runtime.Stop();
    if (const HWND buffer = windows().buffer) PostMessageW(buffer, WM_CLOSE, 0, 0);
    ui.join();
    Check(!windows().buffer && !windows().settings,
          "Broker window shutdown also destroys its independent settings window");
  }
  Check(SetEnvironmentVariableW(L"LOCALAPPDATA", original_size ? original.c_str() : nullptr) != 0,
        "runtime fixture restores the original preferences path after joining its workers");
  std::filesystem::remove_all(root);
}
}  // namespace

void TestBufferBehaviorControls() {
  workbench::Settings saved; int saves = 0;
  int scheme_opens = 0;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [] { workbench::Settings s; s.close_after_send = false; return s; };
  callbacks.save = [&](workbench::Settings value, const std::wstring&, bool, std::string*) { saved = value; ++saves; return true; };
  callbacks.open_schemes = [&] { ++scheme_opens; return true; };
  workbench::SettingsUiHost host(callbacks);
  host.OpenPage(nullptr, ui::SettingsPage::kInput, 0, false);
  const HWND window = host.hwnd();
  const HWND schemes = GetDlgItem(window, 415);
  const HWND close_after = GetDlgItem(window, 412);
  Check(schemes && close_after && schemes != close_after && IsWindowEnabled(schemes),
        "scheme manager and Buffer close-after-send have distinct native controls");
  RECT import_bounds{};
  GetWindowRect(schemes, &import_bounds);
  MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&import_bounds), 2);
  ui::SettingsDraft scheme_draft; scheme_draft.page = ui::SettingsPage::kInput;
  const auto scheme_layout = Layout(window, scheme_draft);
  const float dpi_scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
  Check(!scheme_layout.scheme_cards.empty() &&
        import_bounds.top >= scheme_layout.scheme_cards.back().bottom * dpi_scale &&
        import_bounds.bottom < scheme_layout.body.bottom * dpi_scale,
        "scheme import button clears all scheme rows and the save footer");
  SendMessageW(schemes, BM_CLICK, 0, 0);
  Check(scheme_opens == 1, "scheme manager button dispatches its own callback");
  host.OpenPage(nullptr, ui::SettingsPage::kBuffer, 1, false);
  Check(window && GetDlgItem(window, 413) && GetDlgItem(window, 414), "Buffer behavior exposes real controls");
  SendMessageW(GetDlgItem(window, 413), CB_SETCURSEL, 4, 0);
  SendMessageW(GetDlgItem(window, 414), CB_SETCURSEL, 1, 0);
  SendMessageW(window, WM_COMMAND, MAKEWPARAM(412, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 412)));
  Check(scheme_opens == 1, "Buffer close-after-send does not dispatch the scheme manager");
  host.Close();
  Check(saves == 1 && saved.auto_send_seconds == 5 && saved.close_after_send && saved.translation_provider == "ai",
        "Buffer behavior controls persist their values");
}
void TestParityDraftsAndConnections() {
  workbench::Settings current;
  current.theme = "rasta"; current.follow_system_theme = true;
  std::map<std::string, std::wstring> vault;
  int saves = 0;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [&] { return current; };
  callbacks.load_theme = [] { return ui::ThemeId::kNight; };
  callbacks.connectors.has_secret = [&](const auto& c) { return vault.contains(c.id) && !vault[c.id].empty(); };
  callbacks.save_connections = [&](workbench::Settings value, const auto& secrets, std::string*) {
    current = std::move(value);
    for (const auto& [id, key] : secrets) vault[id] = key;
    ++saves; return true;
  };
  workbench::SettingsUiHost host(std::move(callbacks));
  host.OpenPage(nullptr, ui::SettingsPage::kApi, 0, false);
  auto window = host.hwnd();
  auto edit = [&](int id, const wchar_t* text) { SetWindowTextW(GetDlgItem(window,id), text); };
  auto press = [&](int id) { SendMessageW(GetDlgItem(window,id), BM_CLICK, 0, 0); };
  auto flush = [&] { SendMessageW(window, WM_TIMER, 12, 0); };
  edit(6004,L"API A"); edit(6006,L"https://a.invalid/v1"); edit(6007,L"model-a"); edit(6008,L"synthetic-a");
  flush();
  Check(saves == 1 && vault["legacy-api"] == L"synthetic-a" && host.IsOpen(),
        "connection and credential edits persist while settings stays open");
  Check(GetWindowTextLengthW(GetDlgItem(window,6008)) == 11,
        "automatic persistence keeps the masked credential editor intact");
  press(6001);
  edit(6004,L"API B"); edit(6006,L"https://b.invalid/v1"); edit(6007,L"model-b"); edit(6008,L"synthetic-b");
  flush();
  const auto second_id = current.connections.back().id;
  Check(current.connections.size() == 3 && current.active_connection == "legacy-api" &&
        current.connections.front().name == "API A" && current.connections.back().model == "model-b" &&
        vault["legacy-api"] == L"synthetic-a" && vault[second_id] == L"synthetic-b",
        "automatic settings saves isolate provider credentials and preserve the active model");
  Check(workbench::ValidConnections(current), "new provider has a valid persisted identity");
  Check(current.theme == "rasta" && current.follow_system_theme,
        "saving another page retains the fallback while following system appearance");
  press(6042);
  Check(current.active_connection == second_id, "selecting the text model applies immediately");
  host.Close();
  host.OpenPage(nullptr, ui::SettingsPage::kChord, 1, false); window = host.hwnd();
  edit(6021,L"dvi"); edit(6022,L"hao"); press(6024);
  bool mapping = false;
  for (const auto& row : current.chord_profile["mappings"])
    if (row["keys"] == "dvi" && row["output"] == "hao") mapping = true;
  Check(mapping, "adding a validated mapping is immediately applied");
  edit(6010,L"My keymap"); host.Close();
  Check(current.chord_profile["name"] == "My keymap", "closing flushes pending keymap text");
  host.OpenPage(nullptr, ui::SettingsPage::kAppearance, 1, false); window = host.hwnd();
  edit(6030,L"900"); edit(6031,L"1600"); edit(6033,L"640"); edit(6034,L"44"); edit(6035,L"12");
  flush();
  Check(current.buffer_width == 900 && current.send_hold_ms == 1600 &&
        current.candidate_width == 640 && current.candidate_height == 44 && current.candidate_label_size == 12,
        "numeric settings apply after the editor debounce without closing");
  const int valid_saves = saves;
  edit(6033,L""); flush();
  Check(saves == valid_saves && current.candidate_width == 640 && host.IsOpen(),
        "incomplete numbers retain the previous setting without a modal dialog");
  edit(6033,L"680"); flush();
  Check(current.candidate_width == 680 && saves == valid_saves + 1,
        "correcting an invalid field resumes automatic persistence");
  host.Close();
  host.OpenPage(nullptr, ui::SettingsPage::kApi, 0, false); window = host.hwnd();
  edit(6008,L"temporary-draft"); edit(6008,L""); flush();
  Check(vault[second_id] == L"synthetic-b", "blank credential edit preserves the stored secret");
  press(6041);
  Check(vault.contains(second_id) && vault[second_id].empty(), "explicit clear immediately reaches the credential transaction");
  host.Close();
}

void TestConnectorInteractions() {
  workbench::Settings current;
  int saves = 0, opens = 0, copies = 0, confirms = 0;
  bool confirm = false;
  std::atomic<int> login_attempts{0};
  std::atomic<bool> authorized{false}, allow_login{false};
  const std::string login_url = "https://auth.openai.com/oauth/authorize?state=fixture&code_challenge=fixture";
  workbench::SettingsUiCallbacks cb;
  cb.load = [&] { return current; };
  cb.save_connections = [&](auto value, const auto&, std::string*) { current = value; ++saves; return true; };
  cb.connectors.has_secret = [](const auto&) { return false; };
  cb.connectors.probe = [&](const auto&, const auto&) {
    return workbench::ConnectionProbeResult{authorized.load(), authorized ? "Ready" : "Not authorized", {}};
  };
  cb.connectors.login = [&](const auto&, const auto& cancelled, const auto& url, std::string*) {
    ++login_attempts; url(login_url);
    const auto start = GetTickCount64();
    while (!cancelled() && !allow_login && GetTickCount64() - start < 2500) Sleep(2);
    authorized = !cancelled() && allow_login; return authorized.load();
  };
  cb.connectors.copy_private = [&](const auto& url) { Check(workbench::Utf8(url) == login_url, "copy exact active login URL"); ++copies; return true; };
  cb.connectors.open_url = [&](const auto&) { ++opens; return true; };
  cb.connectors.confirm_delete = [&](HWND, const auto&) { ++confirms; return confirm; };
  workbench::SettingsUiHost host(std::move(cb));
  host.OpenPage(nullptr, ui::SettingsPage::kApi, 0, false);
  const auto window = host.hwnd();
  const auto h = [&](int id) { return GetDlgItem(window,id); };
  const auto visible = [&](int id) { return (GetWindowLongPtrW(h(id), GWL_STYLE) & WS_VISIBLE) != 0; };
  const auto press = [&](int id) { SendMessageW(h(id),BM_CLICK,0,0); };
  const auto tick = [&] { SendMessageW(window,WM_TIMER,13,0); };
  Check(!IsWindowEnabled(h(6009)) && !IsWindowEnabled(h(6042)), "missing API config gates discovery and activation");
  const int initial = saves; press(6061);
  Check(saves == initial, "disabled Claude cannot alter settings");
  press(6060);
  Check(current.ai_connector == "codex-cli" && saves == initial+1, "ChatGPT card activates immediately");
  WaitUntil([&] { tick(); return IsWindowEnabled(h(6040)); }, "authorization status check completes");
  press(6040);
  WaitUntil([&] { tick(); return visible(6065); }, "login publishes copy link during waiting");
  press(6065); Check(copies == 1, "login link copy uses its explicit action");
  press(6040);
  WaitUntil([&] { tick(); return IsWindowEnabled(h(6040)) && !visible(6065); }, "cancel retires login and removes URL");
  allow_login = true; press(6040);
  WaitUntil([&] { tick(); return authorized && IsWindowEnabled(h(6040)); }, "login can succeed after cancellation");
  Check(login_attempts == 2 && saves == initial+1, "authorization progress never rewrites settings");
  press(6062);
  Check(current.ai_connector == "openai-compatible", "API card reactivates saved provider immediately");
  press(6001);
  const auto created = current.connections.back().id;
  SetWindowTextW(h(6006), L"http://127.0.0.1:11434/v1");
  SetWindowTextW(h(6007), L"fixture-model");
  SetWindowTextW(h(6063), L"https://example.com/docs");
  SendMessageW(window,WM_TIMER,12,0);
  Check(IsWindowEnabled(h(6009)) && IsWindowEnabled(h(6042)), "configured loopback provider can work without a key");
  Check(current.connections.back().documentation_url == "https://example.com/docs", "documentation edits save immediately");
  press(6064); Check(opens == 1, "valid documentation opens through explicit action");
  press(6042); Check(current.active_connection == created, "current text model changes immediately");
  const int before_delete = saves; press(6003);
  Check(confirms == 1 && saves == before_delete && current.active_connection == created, "cancelled deletion preserves settings without saving");
  confirm = true; press(6003);
  Check(confirms == 2 && current.active_connection != created && current.connections.size() == 2, "confirmed deletion removes only selected provider");
  host.OpenPage(nullptr,ui::SettingsPage::kApi,1,false);
  const bool was_enabled = current.gateway_enabled; press(6050);
  Check(current.gateway_enabled != was_enabled && !IsWindowEnabled(h(6056)), "gateway switch applies immediately and disables copy when off");
  ui::SettingsDraft draft; draft.page = ui::SettingsPage::kApi; draft.subpage = 1;
  auto layout = Layout(window,draft); Click(window,layout.nav[0]); Click(window,layout.nav[3]);
  Check(visible(6050) && !visible(6060), "navigation remembers connector subpage");
  host.Close();
}

void TestPageControlBounds() {
  workbench::SettingsUiHost host({});
  host.Open(nullptr, false);
  const auto window = host.hwnd();
  const auto dpi = GetDpiForWindow(window);
  for (const auto size : {SIZE{860, 600}, SIZE{980, 680}}) {
    RECT outer{0, 0, MulDiv(size.cx, dpi, 96), MulDiv(size.cy, dpi, 96)};
    AdjustWindowRectExForDpi(&outer, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left,
                 outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    for (int page = 0; page < ui::kSettingsPageCount; ++page) {
      for (int subpage = 0; subpage < ui::SettingsTabCount(static_cast<ui::SettingsPage>(page)); ++subpage) {
        const auto selected = static_cast<ui::SettingsPage>(page);
        host.OpenPage(nullptr, selected, subpage, false);
        ui::SettingsDraft draft; draft.page = selected; draft.subpage = subpage;
        const auto layout = Layout(window, draft);
        std::vector<RECT> bounds;
        for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
          if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
          RECT rect{}; GetWindowRect(child, &rect);
          const auto region = CreateRectRgn(0,0,0,0);
          if (GetWindowRgn(child, region) != ERROR) {
            RECT clipped{}; GetRgnBox(region, &clipped);
            OffsetRect(&clipped, rect.left, rect.top); IntersectRect(&rect, &rect, &clipped);
          }
          DeleteObject(region);
          MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
          Check(rect.left >= layout.body.left * dpi / 96 &&
                rect.right <= layout.width_dip * dpi / 96 &&
                rect.top >= layout.body.top * dpi / 96 &&
                rect.bottom <= layout.body.bottom * dpi / 96 + 1,
                "every page keeps native controls inside the content and above the footer");
          for (const auto& previous : bounds) {
            RECT overlap{};
            Check(!IntersectRect(&overlap, &previous, &rect),
                  "visible native controls do not overlap at minimum and default window sizes");
          }
          bounds.push_back(rect);
        }
      }
    }
  }
  host.Close();
}

void TestNativeControlBackgrounds() {
  for (const auto theme : {ui::ThemeId::kNight, ui::ThemeId::kDay}) {
    workbench::SettingsUiCallbacks callbacks;
    callbacks.load = [theme] { workbench::Settings value; value.theme = ui::ThemeIdString(theme); return value; };
    workbench::SettingsUiHost host(std::move(callbacks));
    host.Open(nullptr, false);
    // Reuse the same native HWNDs across pages, as in the reported regression.
    host.OpenPage(nullptr, ui::SettingsPage::kApi, 0, false);
    const auto window = host.hwnd();
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateBitmap(1024, 768, 1, 32, nullptr);
    const auto old = SelectObject(dc, bitmap);
    constexpr COLORREF poison = RGB(255, 0, 255);
    for (const auto id : {6104, 6106, 6107, 6108, 6001, 6009}) {
      const auto child = GetDlgItem(window, id);
      RECT client{}; GetClientRect(child, &client);
      ui::FillRectColor(dc, client, poison);
      if (id >= 6100) {
        SendMessageW(window, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
                     reinterpret_cast<LPARAM>(child));
      } else {
        DRAWITEMSTRUCT draw{};
        draw.CtlType = ODT_BUTTON; draw.CtlID = id; draw.hwndItem = child;
        draw.hDC = dc; draw.rcItem = client; draw.itemAction = ODA_DRAWENTIRE;
        SendMessageW(window, WM_DRAWITEM, id, reinterpret_cast<LPARAM>(&draw));
      }
      Check(GetPixel(dc, 0, 0) == ui::ToColorRef(ui::Palette(theme).surface_secondary),
            "native label and rounded button corners repaint the current card background");
      Check(GetPixel(dc, client.right - 1, client.bottom - 1) != poison,
            "native controls clear stale pixels across their full client rectangle");
    }
    const auto disabled_edit = GetDlgItem(window, 6008);
    const auto brush = reinterpret_cast<HBRUSH>(SendMessageW(window, WM_CTLCOLORSTATIC,
        reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(disabled_edit)));
    Check(brush != GetStockObject(HOLLOW_BRUSH),
          "disabled credential edits keep an opaque field background");
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
    host.Close();
  }
}

void TestPeerHostsAndPluginKinds() {
  std::vector<workbench::PluginView> entries;
  for (int i = 0; i < 7; ++i) {
    workbench::PluginView item;
    item.id = "fixture." + std::to_string(i);
    item.name = "Fixture " + std::to_string(i);
    item.installed = item.enabled = true;
    item.kind = i < 4 ? "buffer" : i < 6 ? "capsule" : "extension";
    item.can_install = i < 4;
    entries.push_back(item);
  }
  std::string changed;
  int mailbox_opens = 0;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [] { return workbench::Settings{}; };
  callbacks.plugins = [&] { return entries; };
  callbacks.open_mailbox = [&] { ++mailbox_opens; };
  callbacks.manage_plugin = [&](const std::string& id, const std::string&, std::string*) {
    changed = id;
    return true;
  };
  workbench::SettingsUiHost host(std::move(callbacks));
  host.OpenPage(nullptr, ui::SettingsPage::kPlugins);
  const auto window = host.hwnd();
  ShowWindow(window, SW_SHOWNOACTIVATE);
  ui::SettingsDraft draft;
  const auto layout = Layout(window, draft);
  Check(layout.nav[2].top < layout.nav[8].top && layout.nav[8].top < layout.nav[7].top &&
            layout.nav[7].top < layout.nav[3].top && layout.nav[5].top < layout.nav[6].top,
        "Buffer, Mailbox and Capsule are adjacent core hosts before Extensions");
  Check(IsWindowEnabled(GetDlgItem(window, 5114)), "plugin catalog exposes later pages");
  SendMessageW(GetDlgItem(window, 5114), BM_CLICK, 0, 0);
  SendMessageW(GetDlgItem(window, 5101), BM_CLICK, 0, 0);
  Check(changed == "fixture.4", "second-page switch addresses the selected plugin identity");
  Check(!IsVisibleWithinFixture(GetDlgItem(window, 5100), window) &&
            IsVisibleWithinFixture(GetDlgItem(window, 5101), window),
        "compiled host modules expose enablement without unsupported package actions");
  const auto filter = GetDlgItem(window, 5112);
  SendMessageW(filter, CB_SETCURSEL, 4, 0);
  SendMessageW(window, WM_COMMAND, MAKEWPARAM(5112, CBN_SELCHANGE), reinterpret_cast<LPARAM>(filter));
  SendMessageW(GetDlgItem(window, 5101), BM_CLICK, 0, 0);
  Check(changed == "fixture.6" && !IsWindowEnabled(GetDlgItem(window, 5114)),
        "extension filter resets pagination and retains identity-based actions");
  SendMessageW(filter, CB_SETCURSEL, 3, 0);
  SendMessageW(window, WM_COMMAND, MAKEWPARAM(5112, CBN_SELCHANGE), reinterpret_cast<LPARAM>(filter));
  Check(!IsVisibleWithinFixture(GetDlgItem(window, 5101), window),
        "empty host category cannot expose a stale plugin switch");
  host.OpenPage(nullptr, ui::SettingsPage::kBuffer);
  SendMessageW(window, WM_KEYDOWN, VK_DOWN, 0);
  // The first core host after Buffer is Mailbox, irrespective of enum ordinals.
  SendMessageW(GetDlgItem(window, 416), BM_CLICK, 0, 0);
  Check(mailbox_opens == 1, "keyboard navigation from Buffer opens the peer Mailbox host");
  host.Close();
}

void TestBrandHome() {
  std::vector<std::wstring> opened;
  int saves = 0;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [] { return workbench::Settings{}; };
  callbacks.open_brand_link = [&](const std::wstring& url) { opened.push_back(url); return true; };
  callbacks.save = [&](workbench::Settings, const std::wstring&, bool, std::string*) { ++saves; return true; };
  workbench::SettingsUiHost host(callbacks);
  host.Open(nullptr);
  const auto window = host.hwnd();
  ShowWindow(window, SW_SHOWNOACTIVATE);
  ui::SettingsDraft draft;
  const auto layout = Layout(window, draft);
  Check(draft.page == ui::SettingsPage::kHome && ui::kSettingsNavOrder.front() == ui::SettingsPage::kHome &&
        layout.nav[11].top < layout.nav[0].top && layout.nav[10].bottom < layout.body.bottom,
        "brand home is first, without crowding the last sidebar entry or renumbering existing pages");
  const auto shown = [&](int id) { return (GetWindowLongPtrW(GetDlgItem(window, id), GWL_STYLE) & WS_VISIBLE) != 0; };
  Check(!shown(415) && shown(417), "brand home exposes software updates without stale input controls");
  for (int opening = 0; opening < 2; ++opening) {
    SendMessageW(GetDlgItem(window, 417), BM_CLICK, 0, 0);
    const auto updater = FindWindowW(L"XSIMEApplicationUpdate", nullptr);
    Check(updater && IsWindowVisible(updater) && GetWindow(updater, GW_OWNER) == window,
          "settings opens and reopens its owned update window");
    Check(updater && IsWindowEnabled(GetDlgItem(updater, 1)) &&
              !IsWindowEnabled(GetDlgItem(updater, 2)) && !IsWindowEnabled(GetDlgItem(updater, 3)),
          "fresh updater requires checking and a verified download before installation");
    if (updater) SendMessageW(updater, WM_CLOSE, 0, 0);
    Check(!IsWindow(updater) && host.IsOpen() && saves == 0 && opened.empty(),
          "closing updates preserves settings without launching a browser or installer");
  }
  SendMessageW(window, WM_KEYDOWN, VK_DOWN, 0);
  Check(shown(415), "down from the default home selects input settings");
  SendMessageW(window, WM_KEYDOWN, VK_UP, 0);
  Check(!shown(415), "up from input returns to home");
  const wchar_t* expected[] = {L"https://xsime.com", L"mailto:pm@scholay.com", L"https://github.com/scholay/xsime",
      L"https://github.com/scholay/xsime/blob/main/README.md", L"https://github.com/scholay/xsime/blob/main/LICENSE"};
  for (int tab = 1; tab < 6; ++tab) {
    Click(window, layout.subpage_tabs[tab]);
    Check(shown(417) && opened.size() == static_cast<std::size_t>(tab - 1), "selecting a home tab exposes its button without opening anything");
    // Native button keeps Tab focus, accessibility text, and Enter activation.
    SetFocus(window);
    MSG message{}; message.hwnd = window; message.message = WM_KEYDOWN; message.wParam = VK_TAB;
    Check(host.HandleDialogMessage(&message) && GetFocus() == GetDlgItem(window, 417), "Tab reaches the home link button");
    message.hwnd = GetFocus(); message.wParam = VK_RETURN;
    host.HandleDialogMessage(&message);
    Check(opened.size() == static_cast<std::size_t>(tab) && opened.back() == expected[tab - 1] && saves == 0 && host.IsOpen(),
          "Enter opens exactly the selected home destination without saving or closing");
  }
  Click(window, layout.nav[0]);
  SendMessageW(GetDlgItem(window, 417), BM_CLICK, 0, 0);
  Check(!shown(417) && opened.size() == 5, "leaving home hides the button and prevents stale link activation");
  host.Close();
  host.Open(nullptr, false);
  Check(!((GetWindowLongPtrW(GetDlgItem(host.hwnd(), 415), GWL_STYLE)) & WS_VISIBLE), "reopening settings starts at home");
  host.Close();
}

void TestImmediateSettingsAndIcons() {
  workbench::Settings current;
  int saves = 0;
  bool fail_save = false;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [&] { return current; };
  callbacks.save_connections = [&](workbench::Settings value, const auto&, std::string* error) {
    if (fail_save) { *error = "Fixture write failed"; return false; }
    current = std::move(value); ++saves; return true;
  };
  workbench::SettingsUiHost host(std::move(callbacks));
  host.OpenPage(nullptr, ui::SettingsPage::kBuffer, 1, false);
  auto window = host.hwnd();
  wchar_t title[32]{}; GetWindowTextW(window, title, 32);
  Check(std::wstring(title) == L"设置" && SendMessageW(window, WM_GETICON, ICON_SMALL, 0),
        "settings caption is a gear and Settings, without the product title");
  Check(ui::EnsureFluentIcons(), "official Fluent icon font is embedded in the production UI fixture");
  const auto dc = GetDC(window);
  const auto font = CreateFontW(-20,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
      OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"FluentSystemIcons-Regular");
  const auto old = SelectObject(dc,font);
  for (auto icon : ui::kSettingsPageIcons) {
    const wchar_t character = static_cast<wchar_t>(icon); WORD glyph = 0;
    Check(GetGlyphIndicesW(dc,&character,1,&glyph,GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && glyph != 0xffff,
          "every sidebar symbol resolves to an upstream font glyph");
  }
  SelectObject(dc,old); DeleteObject(font); ReleaseDC(window,dc);
  ui::SettingsDraft draft; draft.page = ui::SettingsPage::kBuffer; draft.subpage = 1;
  const auto layout = Layout(window,draft);
  Check(ui::HitTestSettings(layout,layout.width_dip-160,layout.height_dip-26) == -1 &&
        ui::HitTestSettings(layout,layout.width_dip-60,layout.height_dip-26) == -1,
        "removed footer buttons have no hidden hit targets");
  Check(saves == 0, "opening settings never writes preferences");
  const auto combo = GetDlgItem(window,413);
  SendMessageW(combo,CB_SETCURSEL,3,0);
  SendMessageW(window,WM_COMMAND,MAKEWPARAM(413,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
  Check(saves == 1 && current.auto_send_seconds == 3 && host.IsOpen(), "output countdown is immediately applied");
  SendMessageW(GetDlgItem(window,412),BM_CLICK,0,0);
  Check(saves == 2 && !current.close_after_send, "close-after-send switch applies immediately");
  host.OpenPage(nullptr,ui::SettingsPage::kAppearance,1,false);
  auto field = GetDlgItem(window,6033);
  ShowWindow(window, SW_SHOWNOACTIVATE);
  SetFocus(field);
  SetWindowTextW(field,L"720");
  SendMessageW(field, EM_SETSEL, 1, 2);
  Check(saves == 2, "text changes wait for a short idle interval instead of rewriting every keystroke");
  const auto start = GetTickCount64();
  while (saves == 2 && GetTickCount64() - start < 1500) {
    MSG message{};
    while (PeekMessageW(&message,window,0,0,PM_REMOVE)) DispatchMessageW(&message);
    Sleep(2);
  }
  Check(current.candidate_width == 720 && saves == 3 && host.IsOpen(), "real debounce timer applies text while settings stays open");
  DWORD selection_start = 0, selection_end = 0;
  SendMessageW(field, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start), reinterpret_cast<LPARAM>(&selection_end));
  Check(GetFocus() == field && selection_start == 1 && selection_end == 2,
        "automatic persistence keeps editor focus and selection");
  fail_save = true;
  SetWindowTextW(field,L"800"); SendMessageW(window,WM_TIMER,12,0);
  Check(saves == 3 && current.candidate_width == 720 && host.IsOpen(), "failed persistence keeps the last working configuration without a modal prompt");
  fail_save = false;
  SetWindowTextW(field,L"840");
  SendMessageW(window,WM_CLOSE,0,0);
  Check(!host.IsOpen() && current.candidate_width == 840 && saves == 4, "native close flushes a pending valid edit");
  host.OpenPage(nullptr,ui::SettingsPage::kAppearance,1,false); window=host.hwnd();
  SetWindowTextW(GetDlgItem(window,6033),L"");
  SendMessageW(window,WM_CLOSE,0,0);
  Check(!host.IsOpen() && current.candidate_width == 840 && saves == 4, "closing an incomplete field preserves the last valid value");
}

void TestCapsuleRecordingSettings() {
  bool enabled = true;
  int saves = 0;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [] { return workbench::Settings{}; };
  callbacks.load_clipboard_recording = [&] { return enabled; };
  callbacks.save_clipboard_recording = [&](bool value, std::string*) { enabled = value; ++saves; return true; };
  workbench::SettingsUiHost host(callbacks);
  host.OpenPage(nullptr, ui::SettingsPage::kCapsule);
  const auto toggle = GetDlgItem(host.hwnd(), 424);
  Check(IsVisibleWithinFixture(toggle, host.hwnd()) && IsWindowEnabled(toggle) && enabled,
        "Capsule settings expose the default-on recording preference");
  SendMessageW(toggle, BM_CLICK, 0, 0);
  Check(!enabled && saves == 1 && host.IsOpen(), "recording changes apply immediately without closing settings");
  host.Close(); host.OpenPage(nullptr, ui::SettingsPage::kCapsule);
  Check(!enabled && saves == 1, "reopening settings does not restore a default over the saved preference");
  SendMessageW(GetDlgItem(host.hwnd(), 424), BM_CLICK, 0, 0);
  Check(enabled && saves == 2, "recording can resume immediately");
  host.OpenPage(nullptr, ui::SettingsPage::kMailbox);
  Check(!IsVisibleWithinFixture(GetDlgItem(host.hwnd(), 424), host.hwnd()), "recording belongs only to Capsule settings");
}
void TestGlobalShortcutsAndWindowPreferences() {
  workbench::Settings current; int saves = 0, moves = 0, restarts = 0, lexicons = 0;
  std::vector<unsigned> locations;
  workbench::SettingsUiCallbacks callbacks;
  callbacks.load = [&] { return current; };
  callbacks.save = [&](workbench::Settings value, const std::wstring&, bool, std::string*) { current = value; ++saves; return true; };
  callbacks.move_buffer_to_screen = [&] { ++moves; };
  callbacks.restart_broker = [&](std::string* error) { ++restarts; *error = "fixture restart unavailable"; return false; };
  callbacks.open_maintenance_location = [&](unsigned action) { locations.push_back(action); return true; };
  callbacks.open_lexicon = [&](HWND owner, ui::ThemeId) { Check(IsWindow(owner), "lexicon callback receives its settings owner"); ++lexicons; };
  workbench::SettingsUiHost host(callbacks);
  host.OpenPage(nullptr, ui::SettingsPage::kBuffer, 2);
  for (int id = 430; id <= 441; ++id) Check(IsVisibleWithinFixture(GetDlgItem(host.hwnd(), id), host.hwnd()), "global shortcuts and window controls are visible");
  SendMessageW(GetDlgItem(host.hwnd(), 436), BM_CLICK, 0, 0);
  host.RecordShortcut('V', MOD_ALT | MOD_SHIFT);
  Check(host.RecordingShortcut() && saves == 0, "settings shortcut cannot duplicate Capsule");
  host.RecordShortcut('M', MOD_ALT | MOD_SHIFT);
  Check(host.RecordingShortcut() && saves == 0, "Mailbox binding remains reserved");
  host.RecordShortcut(VK_F9, MOD_CONTROL | MOD_SHIFT);
  Check(!host.RecordingShortcut() && saves == 1 && current.settings_key == VK_F9, "settings shortcut applies immediately");
  SendMessageW(GetDlgItem(host.hwnd(), 437), BM_CLICK, 0, 0); host.RecordShortcut(VK_F10, MOD_ALT | MOD_SHIFT);
  SendMessageW(GetDlgItem(host.hwnd(), 438), BM_CLICK, 0, 0); host.RecordShortcut(VK_F11, MOD_ALT | MOD_SHIFT);
  Check(saves == 3 && current.capsule_key == VK_F10 && current.capture_key == VK_F11, "Capsule and screenshot record independently");
  SendMessageW(GetDlgItem(host.hwnd(), 439), BM_CLICK, 0, 0);
  SendMessageW(GetDlgItem(host.hwnd(), 440), BM_CLICK, 0, 0);
  SendMessageW(GetDlgItem(host.hwnd(), 441), BM_CLICK, 0, 0);
  Check(!current.align_to_input_box && current.reset_on_app_switch && saves == 5 && moves == 1, "window preferences apply immediately and move action is distinct from saving");
  host.Close(); host.OpenPage(nullptr, ui::SettingsPage::kBuffer, 2);
  wchar_t text[32]{}; GetWindowTextW(GetDlgItem(host.hwnd(), 430), text, 32);
  Check(std::wstring(text) == L"F9" && saves == 5, "global shortcut survives reopening");
  SendMessageW(GetDlgItem(host.hwnd(), 438), BM_CLICK, 0, 0); host.RecordShortcut(VK_DELETE, 0);
  Check(current.capture_key == '4' && current.settings_key == VK_F9 && saves == 6, "reset one global action preserves other choices");
  host.OpenPage(nullptr, ui::SettingsPage::kCapsule, 1);
  Check(ui::SettingsTabCount(ui::SettingsPage::kCapsule) == 1 && ui::SettingsTabCount(ui::SettingsPage::kMailbox) == 2,
        "Capsule duplicate route removed while Mailbox remains unchanged");
  host.OpenPage(nullptr, ui::SettingsPage::kAbout, 0);
  Check(IsVisibleWithinFixture(GetDlgItem(host.hwnd(), 453), host.hwnd()) && !IsWindowEnabled(GetDlgItem(host.hwnd(), 454)), "maintenance exposes restart and explicitly disables reinstall");
  SendMessageW(GetDlgItem(host.hwnd(), 453), BM_CLICK, 0, 0);
  Check(restarts == 1 && host.IsOpen(), "failed restart keeps the existing settings process open");
  host.OpenPage(nullptr, ui::SettingsPage::kAbout, 1);
  for (int id = 450; id <= 452; ++id) {
    Check(IsVisibleWithinFixture(GetDlgItem(host.hwnd(), id), host.hwnd()), "maintenance exposes local log and data locations");
    SendMessageW(GetDlgItem(host.hwnd(), id), BM_CLICK, 0, 0);
  }
  Check(locations == std::vector<unsigned>({0, 1, 2}), "maintenance resolves each actual configured location through its host");
  host.OpenPage(nullptr, ui::SettingsPage::kInput, 0);
  Check(IsWindowEnabled(GetDlgItem(host.hwnd(), 455)), "learning lexicon entry is enabled when its service exists");
  SendMessageW(GetDlgItem(host.hwnd(), 455), BM_CLICK, 0, 0);
  Check(lexicons == 1, "learning lexicon entry invokes service once");
  host.Close();
}

void TestUserLexiconWindowLifecycle() {
  const auto directory = std::filesystem::temp_directory_path() / (L"rimes-lexicon-ui-" + std::to_wstring(GetCurrentProcessId()));
  std::atomic<bool> release{false}, unavailable{false};
  std::atomic<unsigned> calls{0};
  workbench::UserLexiconBackend backend;
  backend.dictionaries = [&](std::vector<std::string>* names, std::string* error) {
    ++calls;
    while (!release) Sleep(2);
    if (unavailable) { *error = "fixture engine unavailable"; return false; }
    *names = {"rime_ice", "english"}; return true;
  };
  auto service = std::make_shared<workbench::UserLexiconService>(std::move(backend), directory);
  workbench::SettingsUiCallbacks callbacks; callbacks.load = [] { return workbench::Settings{}; };
  workbench::SettingsUiHost host(callbacks); host.Open(nullptr);
  workbench::ShowUserLexicon(host.hwnd(), service, ui::ThemeId::kNight);
  HWND window = FindWindowW(L"Rimes.UserLexicon", nullptr);
  Check(window && GetWindow(window, GW_OWNER) == host.hwnd(), "learning lexicon is owned by settings");
  if (!window) { release = true; return; }
  WaitUntil([&] { return calls == 1; }, "lexicon status runs off the UI thread");
  Check(!IsWindowEnabled(GetDlgItem(window, 7120)) && !IsWindowEnabled(GetDlgItem(window, 7121)) &&
        !IsWindowEnabled(GetDlgItem(window, 7122)), "pending status cannot export import or start another query");
  SendMessageW(window, WM_COMMAND, MAKEWPARAM(7122, BN_CLICKED), 0);
  SendMessageW(window, WM_COMMAND, MAKEWPARAM(7121, BN_CLICKED), 0);
  SendMessageW(window, WM_CLOSE, 0, 0);
  Check(IsWindow(window) && !IsWindowVisible(window), "close returns promptly and defers disposal of an active lexicon task");
  workbench::ShowUserLexicon(host.hwnd(), service, ui::ThemeId::kDay);
  Check(FindWindowW(L"Rimes.UserLexicon", nullptr) == window && calls == 1, "reopening reuses the same pending task");
  const auto pump = [] {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
  };
  release = true;
  WaitUntil([&] { pump(); return IsWindowEnabled(GetDlgItem(window, 7122)) != FALSE; }, "async status returns to the native UI");
  Check(IsWindowEnabled(GetDlgItem(window, 7120)) && IsWindowEnabled(GetDlgItem(window, 7121)), "existing Chinese database permits export and import");
  SendMessageW(GetDlgItem(window, 7102), BM_CLICK, 0, 0);
  Check(!IsWindowEnabled(GetDlgItem(window, 7120)) && IsWindowEnabled(GetDlgItem(window, 7121)), "missing Wubi database can import but cannot pretend to export");
  SendMessageW(GetDlgItem(window, 7103), BM_CLICK, 0, 0);
  Check(IsWindowEnabled(GetDlgItem(window, 7120)), "English has independent real database status");
  HDC dc = GetDC(window);
  SendMessageW(window, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(GetDlgItem(window, 7110)));
  Check(GetTextColor(dc) == ui::ToColorRef(ui::Palette(ui::ThemeId::kDay).text_primary), "reopened lexicon inherits the current settings theme");
  ReleaseDC(window, dc);
  RECT rect{}; GetWindowRect(window, &rect); rect.right = rect.left + 960; rect.bottom = rect.top + 675;
  SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&rect));
  LOGFONTW font{}; GetObjectW(reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(window, 7120), WM_GETFONT, 0, 0)), sizeof(font), &font);
  Check(font.lfHeight == -MulDiv(13, 144, 96), "lexicon buttons follow monitor DPI");
  unavailable = true;
  SendMessageW(GetDlgItem(window, 7122), BM_CLICK, 0, 0);
  WaitUntil([&] { pump(); return IsWindowEnabled(GetDlgItem(window, 7122)) != FALSE; }, "unavailable status completes without blocking UI");
  Check(!IsWindowEnabled(GetDlgItem(window, 7120)) && !IsWindowEnabled(GetDlgItem(window, 7121)), "unavailable engine disables both mutations");
  release = false;
  SendMessageW(GetDlgItem(window, 7122), BM_CLICK, 0, 0);
  WaitUntil([&] { return calls == 3; }, "final lexicon refresh runs");
  SendMessageW(window, WM_CLOSE, 0, 0); release = true;
  WaitUntil([&] { pump(); return !IsWindow(window); }, "closed lexicon retires itself after worker completion");
  host.Close();
}

int main(int argc, char** argv) {
  std::cerr << "Settings UI fixture start\n";
  if (argc == 2 && std::string(argv[1]) == "--runtime-lifecycle") {
    TestSettingsPreserveRuntimeAndStreaming();
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
  }
  if (argc == 2 && std::string(argv[1]) == "--first-show") {
    workbench::SettingsUiCallbacks callbacks;
    callbacks.load = [] { return workbench::Settings{}; };
    workbench::SettingsUiHost host(callbacks);
    host.Open(nullptr);
    Check(host.hwnd() && IsWindowVisible(host.hwnd()),
          "first settings invocation is visible even when the process starts hidden");
    host.OpenPage(nullptr, ui::SettingsPage::kInput);
    const auto draft_field = FindWindowExW(host.hwnd(), nullptr, L"EDIT", nullptr);
    SetWindowTextW(draft_field, L"19");
    const auto window = host.hwnd();
    host.Toggle(nullptr);
    Check(host.hwnd() == window && !IsWindowVisible(window), "toggle hides without destroying settings");
    host.Toggle(nullptr);
    Check(host.hwnd() == window && IsWindowVisible(window), "toggle restores settings");
    wchar_t draft[32]{}; GetWindowTextW(draft_field, draft, 32);
    Check(std::wstring(draft) == L"19", "toggle preserves unsaved settings text");
    host.Close();
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
  }
  wchar_t executable[32768]{};
  GetModuleFileNameW(nullptr, executable, 32768);
  std::wstring command = L"\"" + std::wstring(executable) + L"\" --first-show";
  STARTUPINFOW startup{sizeof(startup)};
  startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  const bool started = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                                     0, nullptr, nullptr, &startup, &process) != FALSE;
  Check(started, "hidden-start settings fixture launches");
  if (started) {
    const auto waited = WaitForSingleObject(process.hProcess, 10000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000); }
    Check(waited == WAIT_OBJECT_0 && code == 0, "hidden-start settings fixture passes");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
  }
  const auto run = [](const char* name, auto test) {
    const auto started = GetTickCount64();
    std::cerr << "RUN " << name << '\n';
    test();
    std::cerr << "DONE " << name << " ms=" << GetTickCount64() - started << '\n';
  };
  run("immediate settings/icons", TestImmediateSettingsAndIcons);
  run("home", TestBrandHome);
  run("peer hosts/plugins", TestPeerHostsAndPluginKinds);
  run("connectors", TestConnectorInteractions);
  run("page control bounds", TestPageControlBounds);
  run("native backgrounds", TestNativeControlBackgrounds);
  run("parity drafts", TestParityDraftsAndConnections);
  run("Buffer controls", TestBufferBehaviorControls);
  run("nested hit targets", TestNestedHitTargets);
  run("owned clicks", TestOwnedClicksAndTransientDetails);
  run("candidate controls", TestCandidateSettingsControls);
  run("Capsule recording", TestCapsuleRecordingSettings);
  run("global shortcuts", TestGlobalShortcutsAndWindowPreferences);
  run("lexicon lifecycle", TestUserLexiconWindowLifecycle);
  run("hotkey registration", TestBufferHotkeyRegistrationLifecycle);
  run("hotkey modifiers", TestBufferHotkeyModifierControl);
  run("plugin shortcuts", TestPluginShortcutControls);
  run("send recorder", TestSendShortcutRecorder);
  run("Codex selector", TestCodexConnectorSelector);
  run("hover/resources", TestHoverRepaintScopeAndResources);
  run("plugin management/chords", TestPluginManagementAndChordSelection);
  run("runtime/streaming", TestSettingsPreserveRuntimeAndStreaming);
  if (failures) return EXIT_FAILURE;
  std::cout << "Settings details, mouse ownership and Runtime preservation tests passed\n";
  return EXIT_SUCCESS;
}
