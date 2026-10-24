#include "user_lexicon.hpp"
#include "provider.hpp"
#include "../ui/settings_paint.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <optional>
#include <thread>

namespace rimes::windows::workbench {
namespace {
constexpr wchar_t kClass[] = L"Rimes.UserLexicon";
constexpr UINT kCompleted = WM_APP + 271;
constexpr UINT_PTR kPoll = 1;
constexpr int kChinese = 7101, kStatus = 7110, kResult = 7111;
constexpr int kExport = 7120, kImport = 7121, kRefresh = 7122, kClose = 7123;
HWND current_window = nullptr;
enum class Action { Refresh, Export, Import };
struct Outcome {
  std::array<UserLexiconStatus, 3> status{};
  std::wstring message;
};
struct Completion {
  std::mutex mutex;
  std::optional<Outcome> result;
};
struct View {
  HWND window = nullptr, heading = nullptr, explanation = nullptr, status = nullptr, result = nullptr;
  std::array<HWND, 3> choices{};
  std::array<HWND, 4> actions{};
  std::array<UserLexiconStatus, 3> statuses{};
  std::shared_ptr<UserLexiconService> service;
  std::shared_ptr<Completion> completion = std::make_shared<Completion>();
  std::jthread worker;
  ui::SettingsFonts fonts;
  std::shared_ptr<ui::SmoothShapesSession> shapes = ui::AcquireSmoothShapes();
  ui::ThemeId theme = ui::DefaultThemeId();
  HBRUSH background = nullptr;
  unsigned dpi = 96, selected = 0;
  bool busy = false, choosing = false, closing = false, destroyed = false;

  ~View() {
    // Ordinary Close is deferred until completion. Forced owner/Broker teardown
    // must retire the worker before the referenced RimeEngine is destroyed.
    if (worker.joinable()) worker.join();
    fonts.Release();
    if (background) DeleteObject(background);
  }
  int Px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
  void Theme(ui::ThemeId value) {
    theme = value;
    const auto& p = ui::Palette(theme);
    if (background) DeleteObject(background);
    background = CreateSolidBrush(ui::ToColorRef(p.settings_background));
    BOOL dark = p.dark;
    const COLORREF caption = ui::ToColorRef(p.settings_background), text = ui::ToColorRef(p.text_primary);
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &text, sizeof(text));
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
  }
  void Layout() {
    fonts.Ensure(dpi);
    RECT client{}; GetClientRect(window, &client);
    const int width = MulDiv(client.right, 96, static_cast<int>(dpi));
    const int height = MulDiv(client.bottom, 96, static_cast<int>(dpi));
    const auto place = [&](HWND child, int x, int y, int w, int h, HFONT font) {
      SetWindowPos(child, nullptr, Px(x), Px(y), Px(w), Px(h), SWP_NOACTIVATE | SWP_NOZORDER);
      SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    };
    place(heading, 24, 22, width - 48, 30, fonts.heading);
    place(explanation, 24, 62, width - 48, 56, fonts.body);
    const int choice_width = (width - 64) / 3;
    for (int i = 0; i < 3; ++i) place(choices[i], 24 + i * (choice_width + 8), 132, choice_width, 36, fonts.body);
    place(status, 24, 190, width - 48, 54, fonts.body);
    place(result, 24, 252, width - 48, (std::max)(48, height - 332), fonts.body);
    const int bottom = height - 56;
    place(actions[0], 24, bottom, 112, 32, fonts.body);
    place(actions[1], 148, bottom, 140, 32, fonts.body);
    place(actions[2], 300, bottom, 82, 32, fonts.body);
    place(actions[3], width - 106, bottom, 82, 32, fonts.body);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
  }
  void Controls() {
    const bool available = !busy && !choosing;
    for (auto choice : choices) { EnableWindow(choice, available); InvalidateRect(choice, nullptr, FALSE); }
    const auto state = statuses[selected].state;
    EnableWindow(actions[0], available && state == UserLexiconState::DatabasePresent);
    EnableWindow(actions[1], available && state != UserLexiconState::Unavailable);
    EnableWindow(actions[2], available);
    EnableWindow(actions[3], !choosing);
    if (!busy) {
      const auto text = std::wstring(UserLexiconName(kUserLexicons[selected])) + L"\n" + Wide(statuses[selected].message);
      SetWindowTextW(status, text.c_str());
    }
  }
  void Start(Action action, std::filesystem::path path = {}) {
    if (busy || choosing) return;
    busy = true;
    SetWindowTextW(status, action == Action::Refresh ? L"正在读取学习词库状态…" : action == Action::Export ? L"正在导出学习词库…" : L"正在检查并导入学习词库…");
    SetWindowTextW(result, L"");
    Controls();
    const auto target = window;
    const auto state = completion;
    const auto backend = service;
    const auto kind = kUserLexicons[selected];
    try {
      worker = std::jthread([state, backend, target, action, kind, path = std::move(path)] {
        Outcome result;
        try {
          if (action != Action::Refresh) {
            const auto value = action == Action::Export ? backend->Export(kind, path) : backend->Import(kind, path);
            result.message = std::wstring(action == Action::Export ? L"已导出 " : value.snapshot ? L"已恢复 " : L"已导入 ") +
                std::to_wstring(value.entries) + L" 条学习记录。\n" + value.file.filename().wstring();
          }
        } catch (const std::exception& error) {
          result.message = L"操作未完成：" + Wide(error.what());
        } catch (...) {
          result.message = L"操作未完成，请稍后重试。";
        }
        try { result.status = backend->Status(); }
        catch (const std::exception& error) {
          for (std::size_t i = 0; i < result.status.size(); ++i)
            result.status[i] = {kUserLexicons[i], UserLexiconState::Unavailable, error.what()};
        } catch (...) {
          for (std::size_t i = 0; i < result.status.size(); ++i)
            result.status[i] = {kUserLexicons[i], UserLexiconState::Unavailable, "词库状态暂不可用。"};
        }
        { std::lock_guard lock(state->mutex); state->result = std::move(result); }
        PostMessageW(target, kCompleted, 0, 0);
      });
      // If the message queue is briefly full, completion still gets collected;
      // polling never changes text or invalidates controls while work continues.
      SetTimer(window, kPoll, 150, nullptr);
    } catch (...) {
      busy = false; SetWindowTextW(result, L"无法启动词库任务，请稍后重试。"); Controls();
    }
  }
  void Finish() {
    if (!busy) return;
    std::optional<Outcome> output;
    { std::lock_guard lock(completion->mutex); output.swap(completion->result); }
    if (!output) return;
    if (worker.joinable()) worker.join();
    KillTimer(window, kPoll); busy = false;
    if (closing) { DestroyWindow(window); return; }
    statuses = std::move(output->status);
    SetWindowTextW(result, output->message.c_str());
    Controls();
  }
  void Choose(Action action) {
    if (busy || choosing) return;
    const auto state = statuses[selected].state;
    if (state == UserLexiconState::Unavailable || (action == Action::Export && state != UserLexiconState::DatabasePresent)) return;
    choosing = true; Controls();
    wchar_t file[32768]{};
    if (action == Action::Export) {
      const auto name = Wide(UserLexiconDictionary(kUserLexicons[selected])) + L"-learning.txt";
      wcsncpy_s(file, name.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window; dialog.lpstrFile = file; dialog.nMaxFile = 32768;
    dialog.lpstrFilter = L"Rime 学习词库或快照 (*.txt;*.tsv;*.snapshot)\0*.txt;*.tsv;*.snapshot\0所有文件\0*.*\0\0";
    dialog.lpstrDefExt = L"txt";
    dialog.lpstrTitle = action == Action::Export ? L"导出学习词库" : L"导入学习词库或恢复快照";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT |
        (action == Action::Export ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const bool accepted = action == Action::Export ? GetSaveFileNameW(&dialog) != 0 : GetOpenFileNameW(&dialog) != 0;
    // The file dialog pumps messages. Broker shutdown can forcibly destroy
    // its owner while it is open; retire this view before touching controls.
    if (destroyed) { delete this; return; }
    const auto failure = accepted ? 0 : CommDlgExtendedError();
    choosing = false; Controls();
    if (accepted) Start(action, file);
    else if (failure) SetWindowTextW(result, L"文件选择器无法打开，请稍后重试。");
  }
};
struct Creation { View* view; bool adopted = false; };
LRESULT CALLBACK ChildProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR parent) {
  const auto owner = reinterpret_cast<HWND>(parent);
  if (message == WM_KEYDOWN && wparam == VK_ESCAPE) { PostMessageW(owner, WM_CLOSE, 0, 0); return 0; }
  if (message == WM_KEYDOWN && wparam == VK_TAB) {
    if (auto next = GetNextDlgTabItem(owner, window, (GetKeyState(VK_SHIFT) & 0x8000) != 0)) SetFocus(next);
    return 0;
  }
  if (message == WM_KEYDOWN && wparam == VK_RETURN && IsWindowEnabled(window)) {
    SendMessageW(window, BM_CLICK, 0, 0); return 0;
  }
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ChildProcedure, 1);
  return DefSubclassProc(window, message, wparam, lparam);
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* view = reinterpret_cast<View*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* creation = static_cast<Creation*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    view = creation->view; creation->adopted = true;
    view->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
  }
  if (!view) return DefWindowProcW(window, message, wparam, lparam);
  switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{}; auto dc = BeginPaint(window, &paint);
      FillRect(dc, &paint.rcPaint, view->background ? view->background : GetSysColorBrush(COLOR_WINDOW));
      EndPaint(window, &paint); return 0;
    }
    case WM_CTLCOLORSTATIC: {
      const auto dc = reinterpret_cast<HDC>(wparam); const auto& p = ui::Palette(view->theme);
      SetBkColor(dc, ui::ToColorRef(p.settings_background)); SetTextColor(dc, ui::ToColorRef(p.text_primary));
      return reinterpret_cast<LRESULT>(view->background);
    }
    case WM_DRAWITEM: {
      const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (!draw || draw->CtlType != ODT_BUTTON) break;
      const auto& p = ui::Palette(view->theme);
      const bool selected = draw->CtlID >= kChinese && draw->CtlID < kChinese + 3 && draw->CtlID - kChinese == view->selected;
      const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
      ui::FillRectColor(draw->hDC, draw->rcItem, ui::ToColorRef(p.settings_background));
      ui::FillRoundRect(draw->hDC, draw->rcItem, view->Px(6), ui::ToColorRef(selected || (draw->itemState & ODS_SELECTED) ? p.surface_tertiary : p.surface));
      ui::StrokeRoundRect(draw->hDC, draw->rcItem, view->Px(6), ui::ToColorRef(selected || (draw->itemState & ODS_FOCUS) ? p.accent : p.border));
      wchar_t text[96]{}; GetWindowTextW(draw->hwndItem, text, 96);
      auto old = SelectObject(draw->hDC, view->fonts.body);
      SetBkMode(draw->hDC, TRANSPARENT); SetTextColor(draw->hDC, ui::ToColorRef(disabled ? p.text_muted : p.text_primary));
      auto rect = draw->rcItem; DrawTextW(draw->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(draw->hDC, old); return TRUE;
    }
    case WM_COMMAND:
      if (HIWORD(wparam) != BN_CLICKED) break;
      if (LOWORD(wparam) >= kChinese && LOWORD(wparam) < kChinese + 3 && !view->busy && !view->choosing) {
        view->selected = LOWORD(wparam) - kChinese; view->Controls(); return 0;
      }
      if (LOWORD(wparam) == kExport) { view->Choose(Action::Export); return 0; }
      if (LOWORD(wparam) == kImport) { view->Choose(Action::Import); return 0; }
      if (LOWORD(wparam) == kRefresh) { view->Start(Action::Refresh); return 0; }
      if (LOWORD(wparam) == kClose) { SendMessageW(window, WM_CLOSE, 0, 0); return 0; }
      break;
    case kCompleted: view->Finish(); return 0;
    case WM_TIMER: if (wparam == kPoll) { view->Finish(); return 0; } break;
    case WM_SIZE: if (view->heading) view->Layout(); return 0;
    case WM_DPICHANGED: {
      view->dpi = HIWORD(wparam);
      const auto* rect = reinterpret_cast<RECT*>(lparam);
      SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOACTIVATE | SWP_NOZORDER);
      view->Layout(); return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* size = reinterpret_cast<MINMAXINFO*>(lparam);
      size->ptMinTrackSize = {view->Px(560), view->Px(440)}; return 0;
    }
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) { SendMessageW(window, WM_CLOSE, 0, 0); return 0; }
      if (wparam == VK_TAB) {
        if (auto next = GetNextDlgTabItem(window, nullptr, (GetKeyState(VK_SHIFT) & 0x8000) != 0)) SetFocus(next);
        return 0;
      }
      break;
    case WM_CLOSE:
      if (view->choosing) return 0;
      if (view->busy) { view->closing = true; ShowWindow(window, SW_HIDE); }
      else DestroyWindow(window);
      return 0;
    case WM_NCDESTROY:
      KillTimer(window, kPoll);
      if (current_window == window) current_window = nullptr;
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      view->destroyed = true; view->window = nullptr;
      if (!view->choosing) delete view;
      return DefWindowProcW(window, message, wparam, lparam);
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
}  // namespace
void ShowUserLexicon(HWND owner, std::shared_ptr<UserLexiconService> service, ui::ThemeId theme) {
  if (!service) return;
  if (current_window && IsWindow(current_window)) {
    auto* existing = reinterpret_cast<View*>(GetWindowLongPtrW(current_window, GWLP_USERDATA));
    existing->closing = false; existing->Theme(theme);
    ShowWindow(current_window, SW_SHOWNORMAL); SetForegroundWindow(current_window);
    return;
  }
  WNDCLASSW type{}; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = kClass;
  type.lpfnWndProc = Procedure; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&type);
  auto* view = new View;
  view->service = std::move(service); view->dpi = owner ? GetDpiForWindow(owner) : 96;
  if (!view->dpi) view->dpi = 96;
  RECT area{};
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
  area = monitor.rcWork;
  if (!area.right || !area.bottom) SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
  const int width = view->Px(640), height = view->Px(450);
  Creation creation{view};
  // From WM_NCCREATE onward the native window owns View, including a failed
  // creation's WM_NCDESTROY path.
  const auto window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kClass, L"学习词库",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
      area.left + (area.right - area.left - width) / 2, area.top + (area.bottom - area.top - height) / 2,
      width, height, owner, nullptr, type.hInstance, &creation);
  if (!window) { if (!creation.adopted) delete view; return; }
  current_window = window; view->dpi = GetDpiForWindow(window);
  const auto control = [&](const wchar_t* cls, const wchar_t* text, int id, DWORD style) {
    auto child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), type.hInstance, nullptr);
    if (style & WS_TABSTOP) SetWindowSubclass(child, ChildProcedure, 1, reinterpret_cast<DWORD_PTR>(window));
    return child;
  };
  view->heading = control(L"STATIC", L"学习词库", 7100, SS_NOPREFIX);
  view->explanation = control(L"STATIC", L"管理中文、五笔86和英文的个人学习记录。\n导入支持 Rime 用户词典文本；带词库标识的学习快照会自动识别并恢复。", 7104, SS_NOPREFIX);
  for (int i = 0; i < 3; ++i) view->choices[i] = control(L"BUTTON", UserLexiconName(kUserLexicons[i]), kChinese + i, BS_OWNERDRAW | WS_TABSTOP);
  view->status = control(L"STATIC", L"正在读取学习词库状态…", kStatus, SS_NOPREFIX);
  view->result = control(L"STATIC", L"", kResult, SS_NOPREFIX);
  const wchar_t* labels[] = {L"导出…", L"导入 / 恢复…", L"刷新", L"关闭"};
  for (int i = 0; i < 4; ++i) view->actions[i] = control(L"BUTTON", labels[i], kExport + i, BS_OWNERDRAW | WS_TABSTOP);
  view->Theme(theme); view->Layout(); view->Start(Action::Refresh);
  ShowWindow(window, SW_SHOWNORMAL); SetForegroundWindow(window);
}
}  // namespace rimes::windows::workbench
