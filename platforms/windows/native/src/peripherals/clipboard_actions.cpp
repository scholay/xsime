#include "clipboard_actions.hpp"

#include "appearance.hpp"
#include "../ui/layered_frame.hpp"

namespace rimes::windows::peripherals {
ClipboardWriteResult PublishClipboardFormats(HWND owner, const std::vector<ClipboardFormat>& formats, DWORD expected_sequence) {
  ValidateClipboardFormats(formats);
  struct Memory {
    UINT format;
    HGLOBAL handle;
  };
  std::vector<Memory> values;
  struct Free {
    std::vector<Memory>& values;
    ~Free() { for (auto& value : values) if (value.handle) GlobalFree(value.handle); }
  } free{values};
  for (const auto& value : formats) {
    auto memory = GlobalAlloc(GMEM_MOVEABLE, value.bytes.size());
    if (!memory) return {};
    values.push_back({ClipboardFormatId(value.name), memory});
    auto* data = GlobalLock(memory);
    if (!data) return {};
    memcpy(data, value.bytes.data(), value.bytes.size());
    GlobalUnlock(memory);
  }
  if (!OpenClipboard(owner)) return {};
  struct Close { ~Close() { CloseClipboard(); } } close;
  // Loading a large archive can race a new copy. Check while owning the
  // clipboard so the new content cannot be overwritten between check and write.
  if (GetClipboardSequenceNumber() != expected_sequence) return {};
  if (!EmptyClipboard()) return {};
  for (auto& value : values) {
    if (!SetClipboardData(value.format, value.handle)) return {};
    value.handle = nullptr;  // Ownership transfers only after successful publication.
  }
  return {true, GetClipboardSequenceNumber()};
}
ClipboardWriteResult CopyClipboardArchive(HWND owner, Store& store, const Json& item, ClipboardPublisher publish,
                          std::function<DWORD()> sequence) {
  // Read, verify and allocate the whole snapshot before modifying the clipboard.
  const auto expected = sequence();
  return publish(owner, store.ClipboardFormats(item), expected);
}
bool ClipboardRequiresNativePaste(const Json& item) {
  if (!item.contains("clipboard")) return false;
  const auto& archive = item.at("clipboard");
  if (!archive.value("files", Json::array()).empty()) return false;
  const auto formats = archive.value("formats", Json::array());
  return formats.size() != 1 || formats.at(0).value("name", "") != "CF_UNICODETEXT";
}
PasteTarget CurrentPasteTarget() {
  PasteTarget result;
  result.foreground = GetForegroundWindow();
  result.thread = GetWindowThreadProcessId(result.foreground, &result.process);
  if (!result.thread || result.process == GetCurrentProcessId() ||
      !IsWindowVisible(result.foreground)) return {};
  GUITHREADINFO info{sizeof(info)};
  if (!GetGUIThreadInfo(result.thread, &info) || !info.hwndFocus ||
      (info.flags & (GUI_INMENUMODE | GUI_INMOVESIZE | GUI_POPUPMENUMODE))) return {};
  result.focus = info.hwndFocus;
  DWORD process = 0;
  GetWindowThreadProcessId(result.focus, &process);
  if (process != result.process ||
      GetAncestor(result.focus, GA_ROOT) != result.foreground) return {};
  wchar_t name[128]{};
  GetClassNameW(result.focus, name, 128);
  if (_wcsicmp(name, L"Edit") == 0 &&
      (GetWindowLongPtrW(result.focus, GWL_STYLE) & ES_PASSWORD)) return {};
  return result;
}
PasteResult PasteClipboardImage(PasteTarget target, DWORD sequence, PastePlatform api) {
  if (!api.interactive()) return PasteResult::Blocked;
  if (!target || api.target() != target) return PasteResult::TargetChanged;
  if (api.sequence() != sequence) return PasteResult::ClipboardChanged;
  if (!api.keys_released) api.keys_released = [] {
    for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN,
                    int('V'), VK_RETURN, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON})
      if (GetAsyncKeyState(key) & 0x8000) return false;
    return true;
  };
  if (!api.keys_released()) return PasteResult::Waiting;
  // Recheck after querying key state; never restore or activate another window.
  if (api.target() != target) return PasteResult::TargetChanged;
  if (api.sequence() != sequence) return PasteResult::ClipboardChanged;
  INPUT keys[4]{};
  for (auto& key : keys) key.type = INPUT_KEYBOARD;
  keys[0].ki.wVk = VK_CONTROL;
  keys[1].ki.wVk = 'V';
  keys[2].ki.wVk = 'V'; keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
  keys[3].ki.wVk = VK_CONTROL; keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
  const auto sent = api.send(4, keys, sizeof(INPUT));
  if (sent == 4) return PasteResult::Sent;
  // A partial batch is not retried: release only keys this batch pressed.
  if (sent > 0 && sent < 4) {
    INPUT release[2]{keys[2], keys[3]};
    api.send(sent == 2 ? 2U : 1U, sent == 2 ? release : release + 1, sizeof(INPUT));
  }
  return PasteResult::Blocked;
}
ClipboardFeedback::~ClipboardFeedback() { if (hwnd_) DestroyWindow(hwnd_); }
void ClipboardFeedback::Hide() {
  if (hwnd_) { KillTimer(hwnd_, 1); ShowWindow(hwnd_, SW_HIDE); }
}
LRESULT CALLBACK ClipboardFeedback::Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_TIMER) { KillTimer(hwnd, 1); ShowWindow(hwnd, SW_HIDE); return 0; }
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  return DefWindowProcW(hwnd, message, wp, lp);
}
void ClipboardFeedback::Show(const std::wstring& text, ui::ThemeId theme, HWND anchor) {
  if (!hwnd_) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"XSIME.Clipboard.Feedback";
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_TRANSPARENT | WS_EX_LAYERED, wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
  }
  if (!hwnd_) return;
  SetWindowTextW(hwnd_, text.c_str());
  const auto dpi = anchor ? GetDpiForWindow(anchor) : 96U;
  const auto width = std::clamp(56 + static_cast<int>(text.size()) * 12, 208, 520);
  ui::LayeredFrame frame;
  if (!frame.Resize(MulDiv(width, dpi, 96), MulDiv(34, dpi, 96))) return;
  appearance::Paint p(frame.dc(), dpi);
  const auto colors = appearance::Palette(theme, false);
  p.Fill(p.R(0, 0, width, 34), colors.background);
  appearance::Icon(p, appearance::Glyph::Copy, p.R(13, 9, 16, 16), colors.secondary);
  p.Text(text, p.R(38, 0, width - 50, 34), 12, colors.text);
  frame.RoundAlpha(static_cast<float>(p.S(10)));
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
  SetWindowPos(hwnd_, HWND_TOPMOST,
      (monitor.rcWork.left + monitor.rcWork.right - p.S(width)) / 2,
      monitor.rcWork.bottom - p.S(96 + 34), p.S(width), p.S(34), SWP_NOACTIVATE);
  frame.Present(hwnd_);
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  SetTimer(hwnd_, 1, 1400, nullptr);
}
}  // namespace rimes::windows::peripherals
