#include <Windows.h>

#include <cstdlib>
#include <iostream>

#include "CandidateWindow.h"
#include "Guids.h"

namespace {
bool ime_show = false, ime_change = false, ime_hide = false;
void CALLBACK OnImeEvent(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
  if (event == EVENT_OBJECT_IME_SHOW) ime_show = true;
  if (event == EVENT_OBJECT_IME_CHANGE) ime_change = true;
  if (event == EVENT_OBJECT_IME_HIDE) ime_hide = true;
}
void Check(bool value, const char* description) {
  if (!value) {
    std::cerr << "FAIL: " << description << '\n';
    std::exit(1);
  }
}
}  // namespace

int main() {
  using namespace rimes::windows::tsf;
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HWND foreground = GetForegroundWindow();
  CandidateWindow candidate;
  const auto hook = SetWinEventHook(EVENT_OBJECT_IME_SHOW, EVENT_OBJECT_IME_CHANGE,
      nullptr, OnImeEvent, GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
  Check(hook != nullptr, "observe only this test's IME light-dismiss events");
  HWND owner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
      0, 0, 20, 20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Check(owner != nullptr, "isolated host owner created");
  candidate.SetOwner(owner);
  int selected = 0;
  std::size_t expected_index = 0;
  candidate.SetSelect([&](std::size_t index) {
    Check(index == expected_index, "mouse selects the visible logical item");
    ++selected;
  });
  CandidateSnapshot snapshot;
  snapshot.visible = true;
  snapshot.composition = L"nihao";
  snapshot.caret_rect = {100, 100, 100, 124};
  snapshot.items = {{L"1", L"你好", L""}};
  candidate.Update(snapshot);
  HWND popup = nullptr;
  DWORD process = 0;
  while ((popup = FindWindowExW(nullptr, popup, kCandidateWindowClass, nullptr))) {
    GetWindowThreadProcessId(popup, &process);
    if (process == GetCurrentProcessId()) break;
  }
  Check(popup && process == GetCurrentProcessId(), "test owns its isolated popup");
  UpdateWindow(popup);
  Check(candidate.IsVisible(), "layered candidate paints and remains visible");
  Check(GetWindow(popup, GW_OWNER) == owner, "candidate is owned by its input host");
  Check(GetForegroundWindow() == foreground, "candidate does not activate the host");
  const unsigned dpi = GetDpiForWindow(popup);
  const auto px = [dpi](int dip) { return MulDiv(dip, static_cast<int>(dpi), 96); };
  RECT initial_rect{};
  GetWindowRect(popup, &initial_rect);
  Check(initial_rect.bottom - initial_rect.top == px(34),
        "composition does not add a row above candidates");
  snapshot.composition.clear();
  candidate.Update(snapshot);
  RECT no_composition_rect{};
  GetWindowRect(popup, &no_composition_rect);
  Check(EqualRect(&initial_rect, &no_composition_rect),
        "popup geometry is independent of inline composition");
  snapshot.composition = std::wstring(200, L'n');
  candidate.Update(snapshot);
  RECT long_composition_rect{};
  GetWindowRect(popup, &long_composition_rect);
  Check(EqualRect(&initial_rect, &long_composition_rect),
        "long inline composition does not widen the candidate popup");
  HRGN region = CreateRectRgn(0, 0, 0, 0);
  Check(GetWindowRgn(popup, region) == ERROR, "alpha popup has no binary region clipping antialiased corners");
  Check((GetWindowLongPtrW(popup, GWL_EXSTYLE) & WS_EX_LAYERED) != 0, "candidate uses per-pixel alpha composition");
  DeleteObject(region);
  const LPARAM click = MAKELPARAM(px(10), px(17));
  SendMessageW(popup, WM_LBUTTONUP, 0, click);
  Check(selected == 0, "late mouse release cannot select a candidate");
  SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, click);
  SendMessageW(popup, WM_LBUTTONUP, 0, click);
  Check(selected == 1, "owned down/up selects once");
  SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, click);
  snapshot.composition = L"ni";
  candidate.Update(snapshot);
  SendMessageW(popup, WM_LBUTTONUP, 0, click);
  Check(selected == 1, "composition change invalidates the old mouse press");
  SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, click);
  candidate.Hide();
  SendMessageW(popup, WM_LBUTTONUP, 0, click);
  Check(selected == 1, "hidden popup invalidates the old mouse press");
  Check(GetCapture() != popup, "hiding releases mouse capture");
  snapshot.items.assign(9, {L"1", L"候选词内容偏长", L""});
  snapshot.highlighted = 0;
  candidate.Update(snapshot);
  RECT full_page_rect{};
  GetWindowRect(popup, &full_page_rect);
  Check(full_page_rect.right - full_page_rect.left <= px(460) &&
        full_page_rect.bottom - full_page_rect.top == px(34),
        "ordinary horizontal candidates stay in one row within the configured width");
  Check(candidate.CompactDigitIndex(0) == std::size_t{0} && candidate.CompactPageIndex(1).has_value() &&
      !candidate.CanPage(-1), "first subpage starts at one and disables previous");
  int page_clicks = 0;
  candidate.SetPage([&](int direction) {
    const auto index = candidate.CompactPageIndex(direction);
    Check(index.has_value(), "pager only invokes enabled visible subpages");
    snapshot.highlighted = static_cast<std::uint16_t>(*index);
    candidate.Update(snapshot);
    ++page_clicks;
  });
  std::vector<std::size_t> starts{0};
  while (candidate.CompactPageIndex(1)) {
    const auto next = *candidate.CompactPageIndex(1);
    RECT rect{}; GetClientRect(popup, &rect);
    const auto page_click = MAKELPARAM(rect.right - px(14), px(17));
    SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, page_click);
    SendMessageW(popup, WM_LBUTTONUP, 0, page_click);
    Check(candidate.CompactDigitIndex(0) == next,
        "mouse next page renumbers its first candidate while preserving the engine index");
    starts.push_back(next);
  }
  Check(page_clicks > 0 && !candidate.CanPage(1), "last subpage disables next without engine neighbours");
  expected_index = starts.back();
  SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, click);
  SendMessageW(popup, WM_LBUTTONUP, 0, click);
  Check(selected == 2, "clicking first visible item on a later page selects its engine index");
  for (auto i = starts.size() - 1; i > 0; --i) {
    RECT rect{}; GetClientRect(popup, &rect);
    const auto page_click = MAKELPARAM(rect.right - px(35), px(17));
    SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, page_click);
    SendMessageW(popup, WM_LBUTTONUP, 0, page_click);
    Check(candidate.CompactDigitIndex(0) == starts[i - 1], "reverse paging revisits stable boundaries");
  }
  SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, click);
  snapshot.highlighted = static_cast<std::uint16_t>(starts.back());
  candidate.Update(snapshot);
  SendMessageW(popup, WM_LBUTTONUP, 0, click);
  Check(selected == 2, "changing visible subpage invalidates a pending selection press");
  snapshot.highlighted = 0;
  snapshot.items.resize(2);
  candidate.Update(snapshot);
  candidate.SetVertical(true);
  RECT vertical_rect{};
  GetWindowRect(popup, &vertical_rect);
  Check(vertical_rect.bottom - vertical_rect.top == px(68),
        "vertical popup contains candidate rows only");
  Check(vertical_rect.top == snapshot.caret_rect.top,
        "first vertical candidate aligns with caret instead of the last candidate");
  snapshot.items.clear();
  candidate.Update(snapshot);
  Check(!IsWindowVisible(popup) && !candidate.snapshot().visible,
        "inline composition without candidates does not show a duplicate popup");
  candidate.SetVertical(false);
  snapshot.items.assign(5, {L"1", L"候选", L""});
  snapshot.matrix_rows = {3, 2};
  candidate.Update(snapshot);
  RECT matrix_rect{};
  GetWindowRect(popup, &matrix_rect);
  Check(matrix_rect.bottom - matrix_rect.top == px(71),
        "matrix adds only explicit candidate rows and their spacing");
  expected_index = 3;
  const LPARAM matrix_click = MAKELPARAM(px(10), px(54));
  SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, matrix_click);
  SendMessageW(popup, WM_LBUTTONUP, 0, matrix_click);
  Check(selected == 3, "second matrix row keeps its flattened mouse index");
  Check(GetForegroundWindow() == foreground, "click ownership preserves no-activation");
  DestroyWindow(owner);
  Check(!candidate.IsVisible() && !candidate.snapshot().visible,
        "owner destruction retires the candidate HWND and snapshot");
  candidate.Update(snapshot);
  Check(candidate.IsVisible(), "next context can recreate its candidate window");
  candidate.Hide();
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  Check(ime_show && ime_change && ime_hide, "show/change/hide notify light-dismiss hosts");
  UnhookWinEvent(hook);
  std::cout << "Candidate alpha surface, click ownership and no-activation passed\n";
}
