#include "../core/product_brand.hpp"
#include "settings_ui.hpp"
#include "application_update.hpp"

#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "../ui/icons.hpp"
#include "../ui/theme.hpp"
#include "buffer_hotkey_config.hpp"

namespace rimes::windows::workbench {
namespace {

constexpr const wchar_t* kSchemas[] = {L"rime_ice", L"double_pinyin",
                                       L"double_pinyin_flypy", L"wubi86", L"lingxi_wubi",
                                       L"english", L"my_combo"};

RECT DipToPx(const ui::DipRect& r, unsigned dpi) {
  RECT out{};
  out.left = static_cast<LONG>(r.left * dpi / 96.0f + 0.5f);
  out.top = static_cast<LONG>(r.top * dpi / 96.0f + 0.5f);
  out.right = static_cast<LONG>(r.right * dpi / 96.0f + 0.5f);
  out.bottom = static_cast<LONG>(r.bottom * dpi / 96.0f + 0.5f);
  return out;
}

void Place(HWND hwnd, const ui::DipRect& r, unsigned dpi, bool show) {
  if (!hwnd) return;
  const RECT px = DipToPx(r, dpi);
  SetWindowPos(hwnd, nullptr, px.left, px.top, px.right - px.left,
               px.bottom - px.top, SWP_NOZORDER | SWP_NOACTIVATE |
                                       (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

std::wstring GetText(HWND hwnd) {
  if (!hwnd) return {};
  std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(hwnd)) + 1,
                    L'\0');
  GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
  text.resize(wcslen(text.c_str()));
  return text;
}

}  // namespace

SettingsUiHost::SettingsUiHost(SettingsUiCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {
  draft_.about_text = callbacks_.about_text;
}

void SettingsUiHost::OpenInputHabits() {
  if (!callbacks_.load || !callbacks_.save || !callbacks_.input_options || !CommitSave()) return;
  HMENU menu = nullptr;
  try {
    auto config = callbacks_.load();
    auto options = callbacks_.input_options(config.schema);
    options.erase(std::remove_if(options.begin(), options.end(), [](const auto& option) {
      return option.name == "ascii_punct"; // Existing native punctuation checkbox owns this choice.
    }), options.end());
    if (options.empty()) {
      MessageBoxW(hwnd_, L"当前方案未提供可配置的双状态开关；原方案文件保持不变。", L"输入习惯", MB_OK);
      return;
    }
    menu = CreatePopupMenu();
    const auto scope = config.input_scope + ":" + config.schema;
    const auto saved = config.InputOptionsFor(config.schema);
    for (std::size_t i = 0; i < options.size(); ++i) {
      const auto& option = options[i];
      HMENU choices = CreatePopupMenu();
      const auto found = saved.find(option.name);
      const int selected = found == saved.end() ? 0 : found->second ? 2 : 1;
      const std::wstring titles[] = {L"跟随方案", Wide(option.off), Wide(option.on)};
      for (int value = 0; value < 3; ++value)
        AppendMenuW(choices, MF_STRING | (selected == value ? MF_CHECKED : 0), 1000 + i * 3 + value, titles[value].c_str());
      AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(choices), Wide(option.off + " / " + option.on).c_str());
    }
    RECT bounds{}; GetWindowRect(input_habits_, &bounds);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, bounds.left, bounds.bottom, 0, hwnd_, nullptr);
    DestroyMenu(menu); menu = nullptr;
    if (command < 1000 || (command - 1000) / 3 >= options.size()) return;
    const auto& name = options[(command - 1000) / 3].name;
    const auto choice = (command - 1000) % 3;
    // Reload after the menu closes; retain changes in all other settings.
    config = callbacks_.load();
    if (!config.input_options.contains(scope)) config.input_options[scope] = core::Json::object();
    if (choice == 0) config.input_options[scope].erase(name);
    else config.input_options[scope][name] = choice == 2;
    std::string error;
    if (!callbacks_.save(config, L"", false, &error)) SaveError(Wide(error));
  } catch (...) {
    if (menu) DestroyMenu(menu);
    SaveError(L"输入习惯未保存，原设置保持不变。");
  }
}

SettingsUiHost::~SettingsUiHost() {
  if (hwnd_) DestroyWindow(hwnd_);
  if (settings_icon_) DestroyIcon(settings_icon_);
  fonts_.Release();
  if (edit_brush_) DeleteObject(edit_brush_);
}

void SettingsUiHost::SyncDraftFromConfig(const Settings& config) {
  draft_.imported_schema = config.imported_scheme ? Wide(config.schema) : L"";
  draft_.schema_index = 0;
  for (int i = 0; i < ui::kSettingsSchemeCount; ++i) {
    if (config.schema == Utf8(std::wstring(kSchemas[i]))) draft_.schema_index = i;
  }
  draft_.theme = callbacks_.load_theme ? callbacks_.load_theme()
                                       : ui::ThemeIdOrDefault(config.theme);
  draft_.preview_theme = draft_.theme;
  draft_.theme_index = static_cast<int>(ui::ThemeIdOrDefault(config.theme));
  draft_.theme_detail = -1;
  opened_theme_ = draft_.theme;
  draft_.ascii = config.ascii;
  draft_.traditional = config.traditional;
  draft_.ascii_punctuation = config.ascii_punctuation;
  draft_.font_size = config.font_size;
  draft_.candidate_count = config.candidate_count;
  draft_.vertical_candidates = config.vertical_candidates;
  draft_.auto_send_seconds = config.auto_send_seconds;
  draft_.close_after_send = config.close_after_send;
  draft_.align_to_input_box = config.align_to_input_box;
  draft_.reset_on_app_switch = config.reset_on_app_switch;
  draft_.global_keys = {config.settings_key, config.capsule_key, config.capture_key};
  draft_.global_modifiers = {config.settings_modifiers, config.capsule_modifiers, config.capture_modifiers};
  draft_.translation_provider_index = config.translation_provider == "ai" ? 1 : 0;
  draft_.plugin_keys = {config.previous_plugin_key, config.next_plugin_key};
  draft_.plugin_modifiers = {config.previous_plugin_modifiers, config.next_plugin_modifiers};
  draft_.hotkey_modifiers = config.hotkey_modifiers;
  draft_.hotkey = static_cast<wchar_t>(config.hotkey_key);
  draft_.send_key = config.send_key;
  draft_.send_modifiers = config.send_modifiers;
  draft_.base_url = Wide(config.base_url);
  draft_.model = Wide(config.model);
  draft_.ai_connector_index = config.ai_connector == "codex-cli" ? 1 : 0;
  draft_.codex_path = Wide(config.codex_path);
  draft_.codex_model = Wide(config.codex_model);
  draft_.target_language = Wide(config.target_language);
  draft_.source_language = Wide(config.source_language);
  draft_.api_key.clear();
  draft_.page = ui::SettingsPage::kHome;
  draft_.subpage = 0;
  draft_.keyboard_focus = false;
  draft_.hover = -1;
  if (!callbacks_.about_text.empty()) draft_.about_text = callbacks_.about_text;
}

void SettingsUiHost::OpenPage(HWND owner, ui::SettingsPage page, int subpage, bool visible) {
  Open(owner, visible);
  draft_.page = page;
  draft_.subpage = subpage;
  draft_.focus = static_cast<int>(page);
  Relayout();
}
void SettingsUiHost::Toggle(HWND owner) {
  if (hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    StopRecording();
    if (extensions_) extensions_->Deactivate();
    if (pending_save_) CommitSave();
    ShowWindow(hwnd_, SW_HIDE);  // Keep the current page and editor state.
    return;
  }
  Open(owner);
}
void SettingsUiHost::Open(HWND owner, bool visible) {
  owner_ = owner;
  if (hwnd_) {
    if (visible) {
      ShowWindow(hwnd_, SW_SHOWNORMAL);
      // The process's first ShowWindow may honor a launcher's SW_HIDE instead.
      SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
      SetForegroundWindow(hwnd_);
    }
    return;
  }
  if (callbacks_.load) SyncDraftFromConfig(callbacks_.load());
  ready_ = false;
  pending_save_ = false;
  draft_.save_error.clear();
  WNDCLASSW wc{};
  wc.lpfnWndProc = Procedure;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"Rimes.SettingsHost";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  if (!settings_icon_) settings_icon_ = ui::CreateSettingsIcon(GetSystemMetrics(SM_CXSMICON));
  wc.hIcon = nullptr;
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  wc.style = CS_HREDRAW | CS_VREDRAW;
  RegisterClassW(&wc);

  dpi_ = GetDpiForSystem();
  if (owner) dpi_ = GetDpiForWindow(owner);
  RECT client{0, 0, MulDiv(980, static_cast<int>(dpi_), 96),
              MulDiv(680, static_cast<int>(dpi_), 96)};
  AdjustWindowRectExForDpi(&client, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
  const int width = (std::min)(client.right - client.left,
                               monitor.rcWork.right - monitor.rcWork.left);
  const int height = (std::min)(client.bottom - client.top,
                                monitor.rcWork.bottom - monitor.rcWork.top);
  const int left = monitor.rcWork.left +
                   (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
  const int top = monitor.rcWork.top +
                  (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
  hwnd_ = CreateWindowExW(
      // Present the shell and native children together during navigation.
      // Buffering only the parent can expose a frame of old child pixels.
      WS_EX_COMPOSITED, wc.lpszClassName, L"设置", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      left, top, width, height,
      // Use the Buffer only to select initial placement. A native owner would
      // make Settings inherit its topmost state. SettingsUiHost owns this
      // ordinary top-level window and destroys it when the Broker closes.
      nullptr, nullptr, wc.hInstance, this);
  if (!hwnd_) return;
  SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(settings_icon_));
  SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(settings_icon_));
  CreateOrUpdateChildren();
  parity_ = std::make_unique<ParitySettings>();
  parity_->Create(hwnd_, callbacks_.load ? callbacks_.load() : Settings{}, callbacks_.connectors);
  if (callbacks_.metrics) extensions_ = std::make_unique<ExtensionPages>(hwnd_, ExtensionCallbacks{
      callbacks_.metrics, callbacks_.load, callbacks_.plugins, callbacks_.manage_plugin,
      [this](ui::SettingsPage page) { OpenPage(owner_, page); }});
  for (const auto id : {5112, 6000, 6007, 6011, 6023, 6032})
    SetWindowSubclass(GetDlgItem(hwnd_, id), ComboProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  SetWindowSubclass(GetDlgItem(hwnd_, 6070), ConnectorScrollProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  ready_ = true;
  SetTimer(hwnd_, 11, 500, nullptr);
  Relayout();
  if (visible) {
      ShowWindow(hwnd_, SW_SHOWNORMAL);
      // The process's first ShowWindow may honor a launcher's SW_HIDE instead.
      SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
      SetForegroundWindow(hwnd_);
    }
  draft_.focus = static_cast<int>(draft_.page);
  if (visible) SetFocus(hwnd_);
}

void SettingsUiHost::Close() {
  if (!hwnd_) return;
  if (pending_save_) CommitSave();
  ready_ = false;
  DestroyWindow(hwnd_);
}

void SettingsUiHost::StopRecording() {
  recording_shortcut_ = -1;
  SetWindowTextW(record_hotkey_, L"录入快捷键");
  SetWindowTextW(record_send_, L"录入快捷键");
  for (const auto control : record_plugin_) SetWindowTextW(control, L"录入快捷键");
  for (const auto control : record_global_) SetWindowTextW(control, L"录入快捷键");
}

void SettingsUiHost::RecordShortcut(unsigned key, unsigned modifiers) {
  if (!RecordingShortcut()) return;
  const bool send = recording_shortcut_ == 1;
  if (key == VK_DELETE && !modifiers) {
    const unsigned defaults[] = {'B', VK_RETURN, VK_UP, VK_DOWN, 'S', 'V', '4'};
    key = defaults[recording_shortcut_];
    modifiers = MOD_ALT | MOD_SHIFT;
  }
  if (!core::ValidShortcut(key, modifiers, send)) {
    SetWindowTextW(shortcut_feedback_, L"该组合不安全或是系统编辑快捷键，请换一个组合。无修饰键仅允许发送键使用 Enter 或 F6–F12。");
    return;
  }
  if (key == 'M' && modifiers == (MOD_ALT | MOD_SHIFT)) {
    SetWindowTextW(shortcut_feedback_, L"Alt+Shift+M 保留给 Mailbox，请使用另一个组合。");
    return;
  }
  const HWND edits[] = {edit_hotkey_, edit_send_, edit_plugin_[0], edit_plugin_[1], edit_global_[0], edit_global_[1], edit_global_[2]};
  const HWND combos[] = {combo_hotkey_modifiers_, combo_send_modifiers_, combo_plugin_modifiers_[0], combo_plugin_modifiers_[1], combo_global_modifiers_[0], combo_global_modifiers_[1], combo_global_modifiers_[2]};
  for (int i = 0; i < 7; ++i) {
    if (i == recording_shortcut_) continue;
    const auto choice = SendMessageW(combos[i], CB_GETCURSEL, 0, 0);
    const unsigned other_modifiers = choice >= 0 && choice < static_cast<LRESULT>(kBufferHotkeyChoices.size())
        ? kBufferHotkeyChoices[static_cast<std::size_t>(choice)].modifiers : 0U;
    if (key == ParseShortcutKey(GetText(edits[i])) && modifiers == other_modifiers) {
      SetWindowTextW(shortcut_feedback_, L"该组合已用于另一项快捷键，请先修改冲突项。");
      return;
    }
  }
  SetWindowTextW(edits[recording_shortcut_], ShortcutKeyTitle(key).c_str());
  SendMessageW(combos[recording_shortcut_], CB_SETCURSEL,
      modifiers ? static_cast<WPARAM>(BufferHotkeyChoiceIndex(modifiers)) : kBufferHotkeyChoices.size(), 0);
  SetWindowTextW(shortcut_feedback_, (L"已录入 " + ShortcutTitle(key, modifiers) + L"；完成修改后立即生效。").c_str());
  StopRecording();
  QueueSave(true);
}

bool SettingsUiHost::HandleDialogMessage(MSG* message) {
  if (!hwnd_ || (message->hwnd != hwnd_ && !IsChild(hwnd_, message->hwnd)))
    return false;
  if (extensions_ && extensions_->Owns(message->hwnd)) {
    // The practice editor owns Return, Tab, arrows and composition. Dialog
    // navigation must never turn a typing key into Save or a sidebar command.
    if (GetDlgCtrlID(message->hwnd) == 7004) return false;
    return IsDialogMessageW(extensions_->window(), message) != FALSE;
  }
  const bool down = message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN;
  const bool up = message->message == WM_KEYUP || message->message == WM_SYSKEYUP;
  if ((down || up) && recorded_key_ && message->wParam == recorded_key_) {
    if (up) recorded_key_ = 0;
    return true;
  }
  if (RecordingShortcut() && down) {
    unsigned modifiers = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
    const auto key = static_cast<unsigned>(message->wParam);
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN)
      return true;
    if (message->lParam & (1LL << 30)) return true;
    if (key == VK_ESCAPE) { StopRecording(); return true; }
    if ((GetKeyState(VK_RMENU) & 0x8000) && (modifiers & MOD_CONTROL)) {
      SetWindowTextW(shortcut_feedback_, L"AltGr 保留给键盘布局输入，请使用左 Alt 或其他修饰键录入快捷键。");
      return true;
    }
    RecordShortcut(key, modifiers);
    recorded_key_ = key;
    return true;
  }
  if (message->message == WM_KEYDOWN) {
    draft_.keyboard_focus = true;
    const auto key = message->wParam;
    const int control_id = GetDlgCtrlID(GetParent(message->hwnd) == hwnd_ ? message->hwnd : GetParent(message->hwnd));
    if ((message->hwnd == combo_hotkey_modifiers_ || message->hwnd == combo_send_modifiers_ || message->hwnd == combo_plugin_modifiers_[0] || message->hwnd == combo_plugin_modifiers_[1] || message->hwnd == combo_connector_ ||
         (control_id >= 433 && control_id <= 435) || control_id == 5112 || control_id == 6000 || control_id == 6005 || control_id == 6007 || control_id == 6011 || control_id == 6023 || control_id == 6032 || control_id == 6050 ||
         message->hwnd == combo_auto_send_ || message->hwnd == combo_translation_) &&
        (key == VK_RETURN || key == VK_ESCAPE) &&
        SendMessageW(GetParent(message->hwnd) == hwnd_ ? message->hwnd : GetParent(message->hwnd), CB_GETDROPPEDSTATE, 0, 0))
      return false;  // Confirm/cancel the native popup before window commands.
    if (key == VK_ESCAPE ||
        (key >= '1' && key <= '9' && (GetKeyState(VK_CONTROL) & 0x8000))) {
      SendMessageW(hwnd_, WM_KEYDOWN, key, message->lParam);
      return true;
    }
    if (key == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
      draft_.subpage = (draft_.subpage + 1) % ui::SettingsTabCount(draft_.page);
      draft_.focus = static_cast<int>(draft_.page);
      Relayout();
      SetFocus(hwnd_);
      return true;
    }
    if (key == VK_RETURN && GetFocus() != hwnd_) {
      wchar_t kind[20]{}; GetClassNameW(GetFocus(),kind,20);
      if (_wcsicmp(kind,L"BUTTON")==0) {
        SendMessageW(GetFocus(), BM_CLICK, 0, 0);
        return true;
      }
      if (pending_save_) CommitSave();
      return true;
    }
    if (key == VK_TAB) {
      std::vector<HWND> controls;
      for (const auto control : {schemes_button_, input_habits_, lexicon_button_, check_ascii_, check_trad_, check_punct_,
                                edit_font_, edit_candidate_count_, check_vertical_, combo_hotkey_modifiers_, edit_hotkey_, record_hotkey_,
                                combo_send_modifiers_, edit_send_, record_send_,
                                combo_plugin_modifiers_[0], edit_plugin_[0], record_plugin_[0],
                                combo_plugin_modifiers_[1], edit_plugin_[1], record_plugin_[1], reset_shortcuts_, edit_base_,
                                edit_model_, edit_key_, edit_lang_, edit_source_lang_, combo_connector_, edit_codex_path_, edit_codex_model_,
                                combo_auto_send_, check_close_after_, combo_translation_})
        if (control && IsWindowVisible(control)) controls.push_back(control);
      if (parity_) { const auto added=parity_->VisibleControls(); controls.insert(controls.end(),added.begin(),added.end()); }
      for (int i = 0; i < 3; ++i)
        for (const auto control : {combo_global_modifiers_[i], edit_global_[i], record_global_[i]})
          if (control && IsWindowVisible(control) && IsWindowEnabled(control)) controls.push_back(control);
      for (const auto control : {check_align_, check_reset_app_, move_buffer_})
        if (control && IsWindowVisible(control) && IsWindowEnabled(control)) controls.push_back(control);
      for (const auto control : maintenance_actions_)
        if (control && IsWindowVisible(control) && IsWindowEnabled(control)) controls.push_back(control);
      for (const auto control : {plugin_filter_, plugin_previous_, plugin_next_, clipboard_recording_, peripheral_open_, home_action_})
        if (control && IsWindowVisible(control) && IsWindowEnabled(control)) controls.push_back(control);
      for (std::size_t i = 0; i < plugin_labels_.size(); ++i)
        for (const auto control : {plugin_install_[i], plugin_enable_[i], plugin_remove_[i]})
          if (control && IsWindowVisible(control) && IsWindowEnabled(control)) controls.push_back(control);
      const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      const int sidebar = static_cast<int>(draft_.page);
      int grid = sidebar;
      if (draft_.subpage == 0 && draft_.page == ui::SettingsPage::kInput && draft_.imported_schema.empty())
        grid = 100 + draft_.schema_index;
      if (draft_.subpage == 0 && draft_.page == ui::SettingsPage::kAppearance)
        grid = 200 + draft_.theme_index;
      auto focus_shell = [&](int next) {
        draft_.focus = next;
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
      };
      if (GetFocus() == hwnd_) {
        const int current = draft_.focus;
        if (!back) {
          if (current < ui::kSettingsPageCount && grid != sidebar) focus_shell(grid);
          else if (current >= 200 && current < 204) focus_shell(400 + current - 200);
          else if (!controls.empty()) SetFocus(controls.front());
          else focus_shell(sidebar);
        } else {
          if (current >= 400 && current < 404) focus_shell(200 + current - 400);
          else if (current >= 100) focus_shell(sidebar);
          else if (!controls.empty()) SetFocus(controls.back());
          else if (grid >= 200 && grid < 204) focus_shell(400 + grid - 200);
          else focus_shell(grid);
        }
        return true;
      }
      if (!controls.empty() && GetFocus() ==
          (back ? controls.front() : controls.back())) {
        focus_shell(back ? (grid >= 200 && grid < 204 ? 400 + grid - 200 : grid) : sidebar);
        return true;
      }
    }
    // IsDialogMessage eats arrow/space keys for dialog navigation; the painted
    // sidebar/cards own these keys while focus is on the shell.
    if (GetFocus() == hwnd_) return false;
  }
  return IsDialogMessageW(hwnd_, message) != FALSE;
}

void SettingsUiHost::CreateOrUpdateChildren() {
  auto make_edit = [&](HWND* slot, bool secret) {
    if (*slot) return;
    *slot = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (secret ? ES_PASSWORD : 0), 0,
        0, 10, 10, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  };
  auto make_check = [&](HWND* slot, const wchar_t* text, int id) {
    if (*slot) return;
    *slot = CreateWindowExW(0, L"BUTTON", text,
                            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10,
                            10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                            nullptr);
  };
  make_edit(&edit_font_, false);
  make_edit(&edit_candidate_count_, false);
  make_edit(&edit_hotkey_, false);
  make_edit(&edit_send_, false);
  SetWindowLongPtrW(edit_send_, GWLP_ID, 407);
  if (!combo_hotkey_modifiers_) {
    combo_hotkey_modifiers_ = CreateWindowExW(
        0, L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 130, 130, hwnd_, reinterpret_cast<HMENU>(405),
        GetModuleHandleW(nullptr), nullptr);
    for (const auto& choice : kBufferHotkeyChoices)
      SendMessageW(combo_hotkey_modifiers_, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(choice.label));
  }
  if (!combo_send_modifiers_) {
    combo_send_modifiers_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 160, 130, hwnd_, reinterpret_cast<HMENU>(408), GetModuleHandleW(nullptr), nullptr);
    for (const auto& choice : kBufferHotkeyChoices)
      SendMessageW(combo_send_modifiers_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
    SendMessageW(combo_send_modifiers_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"无修饰键"));
  }
  auto make_button = [&](HWND& control, const wchar_t* title, int id) {
    if (!control) control = CreateWindowExW(0, L"BUTTON", title, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
  };
  make_button(schemes_button_, L"导入与管理方案…", 415);
  make_button(input_habits_, L"当前方案的输入习惯…", 425);
  make_button(lexicon_button_, L"学习词库：导入、导出与恢复…", 455);
  EnableWindow(lexicon_button_, static_cast<bool>(callbacks_.open_lexicon));
  EnableWindow(input_habits_, static_cast<bool>(callbacks_.input_options));
  make_button(peripheral_open_, L"打开独立窗口…", 416);
  make_button(home_action_, L"", 417);
  EnableWindow(schemes_button_, static_cast<bool>(callbacks_.open_schemes));
  if (!imported_label_) imported_label_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT,
      0, 0, 1, 1, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  SetWindowTextW(imported_label_, (L"当前使用导入方案：" + draft_.imported_schema + rimes::branding::Text(L"\n可在方案管理中启用其他方案或恢复内置方案。重启 {brand} 后生效。").c_str()).c_str());
  make_button(record_hotkey_, L"录入快捷键", 409);
  make_button(record_send_, L"录入快捷键", 410);
  for (int i = 0; i < 2; ++i) {
    make_edit(&edit_plugin_[i], false);
    SetWindowLongPtrW(edit_plugin_[i], GWLP_ID, 420 + i);
    if (!combo_plugin_modifiers_[i]) {
      combo_plugin_modifiers_[i] = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
          0, 0, 160, 130, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(422 + i)), GetModuleHandleW(nullptr), nullptr);
      for (const auto& choice : kBufferHotkeyChoices)
        SendMessageW(combo_plugin_modifiers_[i], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
    }
    make_button(record_plugin_[i], L"录入快捷键", 418 + i);
    SetWindowTextW(edit_plugin_[i], ShortcutKeyTitle(draft_.plugin_keys[i]).c_str());
    SendMessageW(combo_plugin_modifiers_[i], CB_SETCURSEL, BufferHotkeyChoiceIndex(draft_.plugin_modifiers[i]), 0);
  }
  make_button(reset_shortcuts_, L"恢复全部默认快捷键", 411);
  for (int i = 0; i < 3; ++i) {
    make_edit(&edit_global_[i], false);
    SetWindowLongPtrW(edit_global_[i], GWLP_ID, 430 + i);
    if (!combo_global_modifiers_[i]) {
      combo_global_modifiers_[i] = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
          0, 0, 160, 130, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(433 + i)), GetModuleHandleW(nullptr), nullptr);
      for (const auto& choice : kBufferHotkeyChoices)
        SendMessageW(combo_global_modifiers_[i], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
    }
    make_button(record_global_[i], L"录入快捷键", 436 + i);
    SetWindowTextW(edit_global_[i], ShortcutKeyTitle(draft_.global_keys[i]).c_str());
    SendMessageW(combo_global_modifiers_[i], CB_SETCURSEL, BufferHotkeyChoiceIndex(draft_.global_modifiers[i]), 0);
    SetWindowSubclass(combo_global_modifiers_[i], ComboProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  }
  make_check(&check_align_, L"对齐目标输入框（关闭后保留窗口位置）", 439);
  make_check(&check_reset_app_, L"切换应用时清空本地缓冲（保留外部来源内容）", 440);
  make_button(move_buffer_, L"移到当前屏幕", 441);
  EnableWindow(move_buffer_, static_cast<bool>(callbacks_.move_buffer_to_screen));
  const wchar_t* maintenance_titles[] = {L"打开运行日志目录", L"打开输入法数据目录", L"打开设置与历史目录", L"重启输入法进程", L"重新安装 · 暂不可用"};
  for (int i = 0; i < 5; ++i) make_button(maintenance_actions_[i], maintenance_titles[i], 450 + i);
  EnableWindow(maintenance_actions_[3], static_cast<bool>(callbacks_.restart_broker));
  EnableWindow(maintenance_actions_[4], FALSE);
  if (!shortcut_feedback_) shortcut_feedback_ = CreateWindowExW(0, L"STATIC",
      L"录入时 Esc 取消，Delete 恢复该项默认；完成修改后立即生效。\n切换按已启用插件的工具栏顺序循环，保留原文；仅 Buffer 捕获时生效。\n发送键轻按逐块发送，长按默认 0.6 秒全部发送。",
      WS_CHILD | SS_LEFT, 0, 0, 1, 1, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  make_edit(&edit_base_, false);
  make_edit(&edit_codex_path_, false);
  make_edit(&edit_codex_model_, false);
  if (!combo_connector_) {
    combo_connector_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 200, 130, hwnd_, reinterpret_cast<HMENU>(406), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(combo_connector_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"通用 OpenAI API"));
    SendMessageW(combo_connector_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Codex CLI（本机登录）"));
  }
  make_edit(&edit_model_, false);
  make_edit(&edit_key_, true);
  make_edit(&edit_lang_, false);
  make_edit(&edit_source_lang_, false);
  make_check(&check_ascii_, L"英文直输", 401);
  make_check(&check_trad_, L"繁体转换", 402);
  make_check(&check_punct_, L"英文标点", 403);
  make_check(&check_vertical_, L"竖向排列候选", 404);
  make_check(&check_close_after_, L"全部发送完成后关闭 Buffer", 412);
  make_check(&clipboard_recording_, L"自动记录剪贴板历史", 424);
  auto make_combo = [&](HWND& slot, int id, std::initializer_list<const wchar_t*> labels) {
    if (slot) return;
    slot = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 200, 160, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    for (const auto* label : labels) SendMessageW(slot, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  };
  make_combo(combo_auto_send_, 413, {L"手动发送", L"每 1 秒发送下一块", L"每 2 秒发送下一块", L"每 3 秒发送下一块", L"每 5 秒发送下一块"});
  make_combo(combo_translation_, 414, {L"本地离线翻译 · 中文 / English", L"AI 翻译 · 使用连接器"});
  SendMessageW(combo_auto_send_, CB_SETCURSEL, draft_.auto_send_seconds == 5 ? 4 : draft_.auto_send_seconds, 0);
  SendMessageW(combo_translation_, CB_SETCURSEL, draft_.translation_provider_index, 0);
  for (const HWND combo : {combo_hotkey_modifiers_, combo_send_modifiers_, combo_plugin_modifiers_[0], combo_plugin_modifiers_[1], combo_connector_, combo_auto_send_, combo_translation_})
    SetWindowSubclass(combo, ComboProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  SetWindowTextW(edit_font_, std::to_wstring(draft_.font_size).c_str());
  SetWindowTextW(edit_candidate_count_, std::to_wstring(draft_.candidate_count).c_str());
  SetWindowTextW(edit_hotkey_, ShortcutKeyTitle(draft_.hotkey).c_str());
  SetWindowTextW(edit_send_, ShortcutKeyTitle(draft_.send_key).c_str());
  SendMessageW(combo_send_modifiers_, CB_SETCURSEL, draft_.send_modifiers
      ? BufferHotkeyChoiceIndex(draft_.send_modifiers) : kBufferHotkeyChoices.size(), 0);
  SendMessageW(combo_hotkey_modifiers_, CB_SETCURSEL,
               BufferHotkeyChoiceIndex(draft_.hotkey_modifiers), 0);
  SetWindowTextW(edit_base_, draft_.base_url.c_str());
  SetWindowTextW(edit_model_, draft_.model.c_str());
  SendMessageW(combo_connector_, CB_SETCURSEL, draft_.ai_connector_index, 0);
  SetWindowTextW(edit_codex_path_, draft_.codex_path.c_str());
  SetWindowTextW(edit_codex_model_, draft_.codex_model.c_str());
  SetWindowTextW(edit_key_, L"");
  SetWindowTextW(edit_lang_, draft_.target_language.c_str());
  SetWindowTextW(edit_source_lang_, draft_.source_language.c_str());
  for (std::size_t i = 0; i < plugin_labels_.size(); ++i) {
    auto make = [&](HWND& control, const wchar_t* kind, const wchar_t* title, DWORD style, int action) {
      control = CreateWindowExW(0, kind, title, WS_CHILD | style, 0, 0, 1, 1, hwnd_,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(action < 0 ? 0 : 5100 + static_cast<int>(i) * 3 + action)), GetModuleHandleW(nullptr), nullptr);
    };
    make(plugin_labels_[i], L"STATIC", L"", SS_LEFT, -1);
    make(plugin_install_[i], L"BUTTON", L"安装", WS_TABSTOP | BS_OWNERDRAW, 0);
    make(plugin_enable_[i], L"BUTTON", L"启用", WS_TABSTOP | BS_OWNERDRAW, 1);
    make(plugin_remove_[i], L"BUTTON", L"卸载", WS_TABSTOP | BS_OWNERDRAW, 2);
  }
  plugin_status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 0, 0, 1, 1, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  make_combo(plugin_filter_, 5112, {L"全部", L"缓冲插件", L"Capsule 插件", L"Mailbox 插件", L"内置扩展"});
  SendMessageW(plugin_filter_, CB_SETCURSEL, 0, 0);
  plugin_offset_ = 0;
  auto plugin_navigation = [&](int id, const wchar_t* title, bool label = false) {
    return CreateWindowExW(0, label ? L"STATIC" : L"BUTTON", title,
        WS_CHILD | (label ? SS_CENTER : WS_TABSTOP | BS_OWNERDRAW), 0, 0, 1, 1,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
  };
  plugin_previous_ = plugin_navigation(5113, L"上一页");
  plugin_next_ = plugin_navigation(5114, L"下一页");
  plugin_page_ = plugin_navigation(5115, L"", true);
  UpdatePlugins(); ThemeEdits();
}

void SettingsUiHost::UpdatePlugins() {
  // The extension gallery owns this page when available. Updating the hidden
  // legacy labels still sends repaint notifications through the shared host.
  if (extensions_) return;
  const auto set_text = [](HWND h, const std::wstring& text) {
    if (GetText(h) != text) SetWindowTextW(h, text.c_str());
  };
  if (callbacks_.plugins) plugin_rows_ = callbacks_.plugins();
  constexpr const char* kinds[] = {"", "buffer", "capsule", "mailbox", "extension"};
  const auto filter = SendMessageW(plugin_filter_, CB_GETCURSEL, 0, 0);
  if (filter > 0 && filter < static_cast<LRESULT>(std::size(kinds)))
    std::erase_if(plugin_rows_, [&](const auto& item) { return item.kind != kinds[filter]; });
  const auto page_size = plugin_labels_.size();
  const auto count = plugin_rows_.size();
  plugin_offset_ = (std::min)(plugin_offset_, count ? ((count - 1) / page_size) * page_size : 0);
  EnableWindow(plugin_previous_, plugin_offset_ > 0);
  EnableWindow(plugin_next_, plugin_offset_ + page_size < count);
  const auto page_text = count ? std::to_wstring(plugin_offset_ / page_size + 1) + L" / " +
      std::to_wstring((count + page_size - 1) / page_size) : L"暂无插件";
  set_text(plugin_page_, page_text);
  for (std::size_t i = 0; i < plugin_labels_.size(); ++i) {
    if (plugin_offset_ + i >= plugin_rows_.size()) continue;
    const auto& item = plugin_rows_[plugin_offset_ + i];
    const auto name = item.id == "builtin.apple-translation" ? std::string("翻译") : item.name;
    const std::string kind = item.kind == "capsule" ? "Capsule 插件" : item.kind == "mailbox" ? "Mailbox 插件" : item.kind == "extension" ? "内置扩展" : "缓冲插件";
    const auto label = Wide(name + "  " + item.version + "\n" + kind + " · " + (item.installed ? item.enabled ? "已启用" : "已停用" : "未安装"));
    set_text(plugin_labels_[i], label);
    set_text(plugin_install_[i], item.bundled ? L"恢复安装" : L"下载安装");
    EnableWindow(plugin_install_[i], !item.installed);
    set_text(plugin_enable_[i], item.enabled ? L"停用" : L"启用");
    EnableWindow(plugin_enable_[i], item.installed);
  }
  if (callbacks_.plugin_status) set_text(plugin_status_, Wide(callbacks_.plugin_status()));
}

void SettingsUiHost::ThemeEdits() {
  fonts_.Ensure(dpi_);
  const auto& p = ui::Palette(draft_.preview_theme);
  // Documented Windows 11 attributes; older systems keep the native fallback.
  const BOOL dark = p.dark ? TRUE : FALSE;
  const COLORREF caption = ui::ToColorRef(p.settings_background);
  const COLORREF text = ui::ToColorRef(p.text_primary);
  DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
  DwmSetWindowAttribute(hwnd_, DWMWA_TEXT_COLOR, &text, sizeof(text));
  if (edit_brush_) DeleteObject(edit_brush_);
  edit_brush_ = CreateSolidBrush(ui::ToColorRef(p.surface));
  HWND edits[] = {edit_font_, edit_candidate_count_, check_vertical_, combo_hotkey_modifiers_, edit_hotkey_,
                  combo_send_modifiers_, edit_send_, record_hotkey_, record_send_, combo_plugin_modifiers_[0], edit_plugin_[0], record_plugin_[0], combo_plugin_modifiers_[1], edit_plugin_[1], record_plugin_[1], reset_shortcuts_, shortcut_feedback_, edit_base_,
                  edit_model_, edit_key_, edit_lang_, edit_source_lang_, combo_connector_, edit_codex_path_, edit_codex_model_,
                  combo_auto_send_, check_close_after_, combo_translation_};
  for (HWND edit : edits) {
    if (!edit) continue;
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.control),
                 TRUE);
  }
  for (int i = 0; i < 3; ++i)
    for (const auto control : {edit_global_[i], combo_global_modifiers_[i], record_global_[i]})
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.control), TRUE);
  for (const auto control : {check_align_, check_reset_app_, move_buffer_}) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  for (const auto control : maintenance_actions_) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  for (std::size_t i = 0; i < plugin_labels_.size(); ++i) {
    for (const auto control : {plugin_labels_[i], plugin_install_[i], plugin_enable_[i], plugin_remove_[i]})
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  }
  SendMessageW(plugin_status_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  for (const auto control : {plugin_filter_, plugin_previous_, plugin_next_, plugin_page_})
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  HWND checks[] = {check_ascii_, check_trad_, check_punct_, check_vertical_};
  for (HWND check : checks) {
    if (!check) continue;
    SendMessageW(check, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  }
}

void SettingsUiHost::Relayout() {
  if (!hwnd_) return;
  const bool was_updating = updating_controls_;
  updating_controls_ = true;
  if (callbacks_.plugins && extensions_) {
    for (const auto& item : callbacks_.plugins()) {
      if (item.id == "builtin.fly-chord-learning") draft_.extensions_enabled[0] = item.enabled;
      if (item.id == "builtin.typing-speed") draft_.extensions_enabled[1] = item.enabled;
      if (item.id == "builtin.statistics") draft_.extensions_enabled[2] = item.enabled;
    }
    if (!ui::SettingsPageAvailable(draft_, draft_.page)) { draft_.page = ui::SettingsPage::kPlugins; draft_.subpage = 4; }
  }
  draft_.subpage = (std::clamp)(draft_.subpage, 0, ui::SettingsTabCount(draft_.page)-1);
  pressed_hit_ = -2;
  if (GetCapture() == hwnd_) ReleaseCapture();
  if (draft_.page != ui::SettingsPage::kAppearance || draft_.subpage != 0)
    draft_.theme_detail = -1;
  RECT client{};
  GetClientRect(hwnd_, &client);
  dpi_ = GetDpiForWindow(hwnd_);
  fonts_.Ensure(dpi_);
  const float width = static_cast<float>(client.right) * 96.0f /
                      static_cast<float>(dpi_);
  const float height = static_cast<float>(client.bottom) * 96.0f /
                       static_cast<float>(dpi_);
  layout_ = ui::LayoutSettings(width, height, draft_);
  ThemeEdits();

  const bool input = draft_.page == ui::SettingsPage::kInput && draft_.subpage == 1;
  const bool appearance = draft_.page == ui::SettingsPage::kAppearance && draft_.subpage == 1;
  const bool buffer = draft_.page == ui::SettingsPage::kBuffer && draft_.subpage == 0;
  const bool behavior = draft_.page == ui::SettingsPage::kBuffer && draft_.subpage == 1;
  const bool global = draft_.page == ui::SettingsPage::kBuffer && draft_.subpage == 2;
  if (!buffer && !global) StopRecording();
  const bool api = false; // Native multi-profile content owns this page.
  auto place_edit = [&](HWND edit, ui::DipRect box, bool show) {
    if (show) {
      box.left += 8;
      box.right -= 8;
      box.top += 5;
      box.bottom -= 4;
    }
    Place(edit, box, dpi_, show);
  };

  place_edit(edit_font_, layout_.font_edit, appearance);
  place_edit(edit_candidate_count_, layout_.candidate_count_edit, appearance);
  auto hotkey_dropdown = layout_.hotkey_modifiers;
  hotkey_dropdown.bottom += 96.0f;  // Native combo's height also owns its popup.
  Place(combo_hotkey_modifiers_, hotkey_dropdown, dpi_, buffer);
  place_edit(edit_hotkey_, layout_.hotkey_edit, buffer);
  auto send_dropdown = layout_.send_modifiers;
  send_dropdown.bottom += 160.0f;
  Place(combo_send_modifiers_, send_dropdown, dpi_, buffer);
  place_edit(edit_send_, layout_.send_edit, buffer);
  Place(record_hotkey_, layout_.hotkey_record, dpi_, buffer);
  Place(record_send_, layout_.send_record, dpi_, buffer);
  for (int i = 0; i < 2; ++i) {
    auto dropdown = layout_.plugin_modifiers[i]; dropdown.bottom += 160;
    Place(combo_plugin_modifiers_[i], dropdown, dpi_, buffer);
    place_edit(edit_plugin_[i], layout_.plugin_edit[i], buffer);
    Place(record_plugin_[i], layout_.plugin_record[i], dpi_, buffer);
  }
  for (int i = 0; i < 3; ++i) {
    auto dropdown = layout_.global_modifiers[i]; dropdown.bottom += 160;
    Place(combo_global_modifiers_[i], dropdown, dpi_, global);
    place_edit(edit_global_[i], layout_.global_edit[i], global);
    Place(record_global_[i], layout_.global_record[i], dpi_, global);
  }
  Place(check_align_, {layout_.body.left, layout_.body.top + 214, layout_.body.right, layout_.body.top + 240}, dpi_, global);
  Place(check_reset_app_, {layout_.body.left, layout_.body.top + 246, layout_.body.right, layout_.body.top + 272}, dpi_, global);
  Place(move_buffer_, {layout_.body.left, layout_.body.top + 282, layout_.body.left + 190, layout_.body.top + 312}, dpi_, global);
  Place(reset_shortcuts_, layout_.shortcut_reset, dpi_, buffer || global);
  Place(shortcut_feedback_, layout_.shortcut_feedback, dpi_, buffer || global);
  for (int i = 0; i < 5; ++i) {
    const bool show = draft_.page == ui::SettingsPage::kAbout && (draft_.subpage == 1 ? i < 3 : i >= 3);
    const auto y = layout_.body.top + 112 + (i < 3 ? i : i - 3) * 44.f;
    Place(maintenance_actions_[i], {layout_.body.left, y, layout_.body.left + 240, y + 32}, dpi_, show);
  }
  place_edit(edit_base_, layout_.base_edit, api);
  auto connector_dropdown = layout_.connector_combo;
  connector_dropdown.bottom += 96.0f;
  Place(combo_connector_, connector_dropdown, dpi_, api);
  place_edit(edit_codex_path_, layout_.codex_path_edit, api);
  place_edit(edit_codex_model_, layout_.codex_model_edit, api);
  place_edit(edit_model_, layout_.model_edit, api);
  place_edit(edit_key_, layout_.key_edit,
             false);
  place_edit(edit_lang_, behavior ? layout_.buffer_language : layout_.lang_edit, api || behavior);
  place_edit(edit_source_lang_, layout_.buffer_source_language, behavior);
  auto auto_dropdown = layout_.auto_send_combo; auto_dropdown.bottom += 180;
  auto translation_dropdown = layout_.translation_combo; translation_dropdown.bottom += 100;
  Place(combo_auto_send_, auto_dropdown, dpi_, behavior);
  Place(combo_translation_, translation_dropdown, dpi_, behavior);
  Place(check_close_after_, layout_.close_after_check, dpi_, behavior);
  const bool schemes = draft_.page == ui::SettingsPage::kInput && draft_.subpage == 0;
  const float schemes_top = layout_.scheme_cards.empty()
      ? layout_.body.top + 170 : layout_.scheme_cards.back().bottom + 20;
  Place(schemes_button_, {layout_.body.left, schemes_top, layout_.body.left + 225, schemes_top + 34}, dpi_, schemes);
  Place(input_habits_, {layout_.body.left + 240, schemes_top, layout_.body.left + 490, schemes_top + 34}, dpi_, schemes);
  Place(lexicon_button_, {layout_.body.left, schemes_top + 44, layout_.body.left + 300, schemes_top + 78}, dpi_, schemes);
  SendMessageW(lexicon_button_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  SendMessageW(input_habits_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  Place(imported_label_, {layout_.body.left, layout_.body.top, layout_.body.right, layout_.body.top + 100}, dpi_, schemes && !draft_.imported_schema.empty());
  SendMessageW(schemes_button_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  SendMessageW(imported_label_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  Place(check_ascii_, layout_.check_ascii, dpi_, input);
  Place(check_trad_, layout_.check_trad, dpi_, input);
  Place(check_punct_, layout_.check_punct, dpi_, input);
  Place(check_vertical_, layout_.check_vertical, dpi_, appearance);
  const bool plugins = !extensions_ && draft_.page == ui::SettingsPage::kPlugins && draft_.subpage == 0;
  UpdatePlugins();
  Place(plugin_filter_, {layout_.body.left, layout_.body.top, layout_.body.left+220, layout_.body.top+180}, dpi_, plugins);
  for (std::size_t i = 0; i < plugin_labels_.size(); ++i) {
    const float y = layout_.body.top + 48 + static_cast<float>(i) * 66.0f;
    const float x = layout_.body.left;
    const bool visible = plugins && plugin_offset_ + i < plugin_rows_.size();
    const bool installable = visible && plugin_rows_[plugin_offset_ + i].can_install;
    Place(plugin_labels_[i], {x,y,x+285,y+52}, dpi_, visible);
    Place(plugin_install_[i], {x+300,y+6,x+414,y+38}, dpi_, installable);
    Place(plugin_enable_[i], {x+426,y+6,x+510,y+38}, dpi_, visible);
    Place(plugin_remove_[i], {x+522,y+6,x+606,y+38}, dpi_, installable);
  }
  Place(plugin_previous_, {layout_.body.left,layout_.body.top+320,layout_.body.left+90,layout_.body.top+350}, dpi_, plugins);
  Place(plugin_page_, {layout_.body.left+100,layout_.body.top+324,layout_.body.left+220,layout_.body.top+350}, dpi_, plugins);
  Place(plugin_next_, {layout_.body.left+230,layout_.body.top+320,layout_.body.left+320,layout_.body.top+350}, dpi_, plugins);
  Place(plugin_status_, {layout_.body.left,layout_.body.top+364,layout_.body.right,layout_.body.top+398}, dpi_, plugins);
  const bool peripheral = draft_.page == ui::SettingsPage::kCapsule || draft_.page == ui::SettingsPage::kMailbox;
  const bool capsule = draft_.page == ui::SettingsPage::kCapsule;
  if (capsule && callbacks_.load_clipboard_recording)
    clipboard_recording_enabled_ = callbacks_.load_clipboard_recording();
  Place(clipboard_recording_, {layout_.body.left,layout_.body.top+154,layout_.body.right,layout_.body.top+186}, dpi_, capsule);
  EnableWindow(clipboard_recording_, static_cast<bool>(callbacks_.save_clipboard_recording));
  SendMessageW(clipboard_recording_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  const auto peripheral_y = layout_.body.top + (capsule ? 204 : 150);
  Place(peripheral_open_, {layout_.body.left,peripheral_y,layout_.body.left+220,peripheral_y+34}, dpi_, peripheral);
  SendMessageW(peripheral_open_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  const auto* home_link = draft_.page == ui::SettingsPage::kHome ? ui::HomeLink(draft_.subpage) : nullptr;
  Place(home_action_, layout_.home_action, dpi_, home_link != nullptr || (draft_.page == ui::SettingsPage::kHome && draft_.subpage == 0));
  if (draft_.page == ui::SettingsPage::kHome && draft_.subpage == 0) SetWindowTextW(home_action_, L"检查更新…");
  if (home_link) SetWindowTextW(home_action_, home_link->action);
  SendMessageW(home_action_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
  if (parity_) parity_->Layout(draft_.page, draft_.subpage, layout_.body, dpi_, fonts_.body);
  if (extensions_) extensions_->Layout(draft_.page, draft_.subpage, layout_.body, dpi_, draft_.preview_theme);
  // WS_CLIPCHILDREN excludes native labels/controls from the shell repaint.
  // They must also redraw after navigation, resizing or a theme preview.
  // Coalesce child layout/notification changes into the next complete frame.
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
  updating_controls_ = was_updating;
}

LRESULT CALLBACK SettingsUiHost::ComboProcedure(HWND hwnd, UINT message, WPARAM wparam,
    LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
  auto* self = reinterpret_cast<SettingsUiHost*>(data);
  if (message == WM_NCDESTROY) { RemoveWindowSubclass(hwnd, ComboProcedure, id); return DefSubclassProc(hwnd, message, wparam, lparam); }
  if (message != WM_PAINT && message != WM_PRINTCLIENT) {
    const auto result = DefSubclassProc(hwnd, message, wparam, lparam);
    if (message == CB_SETCURSEL || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == CB_SHOWDROPDOWN)
      InvalidateRect(hwnd, nullptr, FALSE);
    return result;
  }
  PAINTSTRUCT ps{};
  HDC dc = message == WM_PAINT ? BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wparam);
  if (dc) {
    const auto& p = ui::Palette(self->draft_.preview_theme);
    RECT client{}; GetClientRect(hwnd, &client);
    self->PaintChildBackground(hwnd, dc);
    ui::FillRoundRect(dc, client, MulDiv(6, static_cast<int>(self->dpi_), 96), ui::ToColorRef(p.surface));
    ui::StrokeRoundRect(dc, client, MulDiv(6, static_cast<int>(self->dpi_), 96), ui::ToColorRef(GetFocus() == hwnd || IsChild(hwnd, GetFocus()) ? p.accent : p.border));
    RECT arrow = client; arrow.left = arrow.right - MulDiv(26, static_cast<int>(self->dpi_), 96);
    ui::DrawFluentIcon(dc, ui::FluentIcon::kChevronDown, arrow, ui::ToColorRef(p.text_secondary));
    RECT label = client; label.left += MulDiv(8, static_cast<int>(self->dpi_), 96); label.right = arrow.left;
    // Editable combos own a native EDIT child. Painting the same string here
    // underneath it leaves duplicate glyphs when editing or selecting text.
    if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & 3) != CBS_DROPDOWN) {
      const auto text = GetText(hwnd); const auto old = SelectObject(dc, self->fonts_.control);
      SetBkMode(dc, TRANSPARENT); SetTextColor(dc, ui::ToColorRef(p.text_primary));
      DrawTextW(dc, text.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      SelectObject(dc, old);
    }
  }
  if (message == WM_PAINT) EndPaint(hwnd, &ps);
  return 0;
}
LRESULT CALLBACK SettingsUiHost::ConnectorScrollProcedure(HWND h, UINT message, WPARAM w, LPARAM l,
                                                          UINT_PTR id, DWORD_PTR data) {
  auto* self = reinterpret_cast<SettingsUiHost*>(data);
  if (message == WM_NCDESTROY) RemoveWindowSubclass(h, ConnectorScrollProcedure, id);
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_PAINT || message == WM_PRINT || message == WM_PRINTCLIENT) {
    PAINTSTRUCT paint{};
    const auto dc = message == WM_PAINT ? BeginPaint(h, &paint) : reinterpret_cast<HDC>(w);
    RECT r{}; GetClientRect(h, &r);
    const auto& p = ui::Palette(self->draft_.preview_theme);
    const auto px = [self](int n) { return MulDiv(n, self->dpi_, 96); };
    ui::FillRectColor(dc, r, ui::ToColorRef(p.settings_background));
    SCROLLBARINFO info{sizeof(info)};
    if (GetScrollBarInfo(h, OBJID_CLIENT, &info)) {
      const bool pressed = (info.rgstate[3] & STATE_SYSTEM_PRESSED) != 0;
      POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(h, &cursor);
      const bool hover = PtInRect(&r, cursor) != FALSE;
      const int width = px(hover || pressed ? 7 : 5);
      RECT thumb{(r.right-width)/2, info.xyThumbTop, (r.right+width)/2, info.xyThumbBottom};
      if (thumb.bottom > thumb.top) ui::FillRoundRect(dc, thumb, px(3), ui::ToColorRef(
          pressed ? p.text_secondary : ui::Blend(p.settings_background,p.text_secondary,hover ? .65f : .45f)));
      for (bool up : {true,false}) {
        RECT arrow{0, up ? 0 : r.bottom-info.dxyLineButton, r.right, up ? info.dxyLineButton : r.bottom};
        const int saved = SaveDC(dc);
        if (up) {
          SetGraphicsMode(dc,GM_ADVANCED);
          XFORM rotate{-1,0,0,-1,static_cast<FLOAT>(arrow.right),static_cast<FLOAT>(arrow.bottom)};
          ModifyWorldTransform(dc,&rotate,MWT_LEFTMULTIPLY);
        }
        ui::DrawFluentIcon(dc,ui::FluentIcon::kChevronDown,arrow,ui::ToColorRef(p.text_muted));
        RestoreDC(dc,saved);
      }
    }
    if (message == WM_PAINT) EndPaint(h,&paint);
    return 0;
  }
  if (message == WM_MOUSEMOVE) { TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0}; TrackMouseEvent(&track); }
  const auto result = DefSubclassProc(h,message,w,l);
  if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE || message == WM_LBUTTONUP || message == WM_ENABLE)
    InvalidateRect(h,nullptr,FALSE);
  return result;
}
void SettingsUiHost::ApplyControlTheme() { ThemeEdits(); }

void SettingsUiHost::SaveError(const std::wstring& message) {
  draft_.save_error = message;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsUiHost::QueueSave(bool immediate) {
  if (!ready_ || updating_controls_) return;
  pending_save_ = true;
  KillTimer(hwnd_, 12);
  if (immediate) CommitSave();
  else SetTimer(hwnd_, 12, 400, nullptr);
}

bool SettingsUiHost::CommitSave() {
  KillTimer(hwnd_, 12);
  if (!pending_save_) return true;
  if (!callbacks_.save && !callbacks_.save_connections) { pending_save_ = false; return true; }
  try {
    Settings config = callbacks_.load ? callbacks_.load() : Settings{};
    if (draft_.imported_schema.empty())
      config.schema = Utf8(std::wstring(kSchemas[draft_.schema_index]));
    config.theme = ui::ThemeIdString(
        static_cast<ui::ThemeId>(draft_.theme_index));
    config.base_url = Utf8(GetText(edit_base_));
    config.model = Utf8(GetText(edit_model_));
    const auto connector = SendMessageW(combo_connector_, CB_GETCURSEL, 0, 0);
    if (connector != 0 && connector != 1) throw std::runtime_error("connector choice");
    config.ai_connector = connector == 1 ? "codex-cli" : "openai-compatible";
    config.codex_path = Utf8(GetText(edit_codex_path_));
    config.codex_model = Utf8(GetText(edit_codex_model_));
    config.target_language = Utf8(GetText(edit_lang_));
    config.source_language = Utf8(GetText(edit_source_lang_));
    const auto automatic = SendMessageW(combo_auto_send_, CB_GETCURSEL, 0, 0);
    const auto translation = SendMessageW(combo_translation_, CB_GETCURSEL, 0, 0);
    if (automatic < 0 || automatic > 4 || translation < 0 || translation > 1) throw std::runtime_error("Buffer behavior choice");
    config.auto_send_seconds = automatic == 4 ? 5U : static_cast<unsigned>(automatic);
    config.close_after_send = draft_.close_after_send;
    config.align_to_input_box = draft_.align_to_input_box;
    config.reset_on_app_switch = draft_.reset_on_app_switch;
    config.translation_provider = translation == 0 ? "local" : "ai";
    const auto font_text = GetText(edit_font_);
    std::size_t parsed = 0;
    config.font_size = static_cast<unsigned>(std::stoul(font_text, &parsed));
    if (parsed != font_text.size()) throw std::runtime_error("font range");
    const auto count_text = GetText(edit_candidate_count_);
    config.candidate_count = static_cast<unsigned>(std::stoul(count_text, &parsed));
    auto hotkey = GetText(edit_hotkey_);
    if (parsed != count_text.size() || config.candidate_count < 1 || config.candidate_count > 9 || config.font_size < 10 ||
        config.font_size > 40)
      throw std::runtime_error("range");
    config.hotkey_key = ParseShortcutKey(hotkey);
    const auto modifier_choice = SendMessageW(combo_hotkey_modifiers_, CB_GETCURSEL, 0, 0);
    if (modifier_choice < 0 || modifier_choice >= static_cast<LRESULT>(kBufferHotkeyChoices.size()))
      throw std::runtime_error("hotkey modifier choice");
    config.hotkey_modifiers = kBufferHotkeyChoices[static_cast<std::size_t>(modifier_choice)].modifiers;
    config.send_key = ParseShortcutKey(GetText(edit_send_));
    const auto send_choice = SendMessageW(combo_send_modifiers_, CB_GETCURSEL, 0, 0);
    if (send_choice < 0 || send_choice > static_cast<LRESULT>(kBufferHotkeyChoices.size()))
      throw std::runtime_error("send modifier choice");
    config.send_modifiers = send_choice == static_cast<LRESULT>(kBufferHotkeyChoices.size()) ? 0U
        : kBufferHotkeyChoices[static_cast<std::size_t>(send_choice)].modifiers;
    for (int i = 0; i < 2; ++i) {
      auto& key = i ? config.next_plugin_key : config.previous_plugin_key;
      auto& modifiers = i ? config.next_plugin_modifiers : config.previous_plugin_modifiers;
      key = ParseShortcutKey(GetText(edit_plugin_[i]));
      const auto choice = SendMessageW(combo_plugin_modifiers_[i], CB_GETCURSEL, 0, 0);
      modifiers = choice >= 0 && choice < static_cast<LRESULT>(kBufferHotkeyChoices.size())
          ? kBufferHotkeyChoices[static_cast<std::size_t>(choice)].modifiers : 0U;
    }
    unsigned* global_keys[] = {&config.settings_key, &config.capsule_key, &config.capture_key};
    unsigned* global_modifiers[] = {&config.settings_modifiers, &config.capsule_modifiers, &config.capture_modifiers};
    for (int i = 0; i < 3; ++i) {
      *global_keys[i] = ParseShortcutKey(GetText(edit_global_[i]));
      const auto choice = SendMessageW(combo_global_modifiers_[i], CB_GETCURSEL, 0, 0);
      *global_modifiers[i] = choice >= 0 && choice < static_cast<LRESULT>(kBufferHotkeyChoices.size())
          ? kBufferHotkeyChoices[static_cast<std::size_t>(choice)].modifiers : 0U;
    }
    if (!ValidBufferShortcuts(config)) {
      SetWindowTextW(shortcut_feedback_, L"快捷键无效、占用保留快捷键或与另一项重复；尚未保存。无修饰键仅允许发送键使用 Enter 或 F6–F12。");
      SaveError(L"快捷键无效或重复，原设置保持不变。");
      return false;
    }
    config.ascii = draft_.ascii;
    config.traditional = draft_.traditional;
    config.ascii_punctuation = draft_.ascii_punctuation;
    config.vertical_candidates = draft_.vertical_candidates;
    auto key = GetText(edit_key_);
    std::string error;
    if (parity_ && !parity_->Read(&config, &error)) {
      SaveError(Wide(error)); return false;
    }
    const bool ok = callbacks_.save_connections && parity_
        ? callbacks_.save_connections(config, parity_->Secrets(), &error)
        : callbacks_.save(config, key, !key.empty(), &error);
    if (!key.empty())
      SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
    if (!ok) {
      SaveError(Wide(error));
      return false;
    }
    const auto previous_theme = draft_.theme;
    draft_.theme = config.follow_system_theme && callbacks_.load_theme
        ? callbacks_.load_theme() : static_cast<ui::ThemeId>(draft_.theme_index);
    draft_.preview_theme = draft_.theme;
    opened_theme_ = draft_.theme;
    pending_save_ = false;
    draft_.save_error.clear();
    updating_controls_ = true;
    if (parity_) parity_->Saved();
    if (!GetText(edit_key_).empty()) SetWindowTextW(edit_key_, L"");
    updating_controls_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
    // Ordinary autosaves do not change fonts/colors. Resetting every native
    // control here makes the whole page blink after each typing pause.
    if (previous_theme != draft_.theme) {
      ThemeEdits();
      if (callbacks_.on_theme_preview) callbacks_.on_theme_preview(draft_.theme);
      RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    return true;
  } catch (...) {
    SaveError(L"请输入有效的候选数（1–9）、字号或快捷键；原设置保持不变。");
    return false;
  }
}

void SettingsUiHost::PaintChildBackground(HWND child, HDC dc) {
  // Transparent STATICs and rounded owner-drawn controls cannot rely on the
  // parent's pixels: the parent deliberately clips every visible child HWND.
  // Render the current shell at the child's exact origin, including card
  // backgrounds. This also makes corners independent of stale/default pixels.
  RECT client{};
  GetClientRect(child, &client);
  POINT origin{};
  MapWindowPoints(child, hwnd_, &origin, 1);
  const int saved = SaveDC(dc);
  IntersectClipRect(dc, client.left, client.top, client.right, client.bottom);
  OffsetViewportOrgEx(dc, -origin.x, -origin.y, nullptr);
  ui::PaintSettingsShell(dc, layout_, draft_, fonts_, dpi_);
  if (parity_) parity_->PaintFields(dc, draft_.preview_theme, dpi_);
  RestoreDC(dc, saved);
}

void SettingsUiHost::Paint(HDC dc) {
  RECT client{};
  if (!GetClientRect(hwnd_, &client) || client.right <= 0 || client.bottom <= 0)
    return;
  HDC frame = CreateCompatibleDC(dc);
  HBITMAP bitmap = frame
      ? CreateCompatibleBitmap(dc, client.right, client.bottom) : nullptr;
  if (!bitmap) {
    if (frame) DeleteDC(frame);
    ui::PaintSettingsShell(dc, layout_, draft_, fonts_, dpi_);
    if (parity_) parity_->PaintFields(dc, draft_.preview_theme, dpi_);
    return;
  }
  const auto previous = SelectObject(frame, bitmap);
  // Background clearing, text and highlights must reach the window together.
  // WM_ERASEBKGND alone cannot prevent a direct GDI clear/draw sequence from
  // exposing an empty intermediate frame when the mouse crosses navigation.
  ui::PaintSettingsShell(frame, layout_, draft_, fonts_, dpi_);
  if (parity_) parity_->PaintFields(frame, draft_.preview_theme, dpi_);
  BitBlt(dc, 0, 0, client.right, client.bottom, frame, 0, 0, SRCCOPY);
  SelectObject(frame, previous);
  DeleteObject(bitmap);
  DeleteDC(frame);
}

void SettingsUiHost::InvalidateHover(int hit) {
  const ui::DipRect* bounds = nullptr;
  if (hit >= 0 && hit < ui::kSettingsPageCount)
    bounds = &layout_.nav[static_cast<std::size_t>(hit)];
  else if (hit >= 100 && hit < 100 + static_cast<int>(layout_.scheme_cards.size()))
    bounds = &layout_.scheme_cards[static_cast<std::size_t>(hit - 100)];
  else if (hit >= 200 && hit < 200 + static_cast<int>(layout_.theme_cards.size()))
    bounds = &layout_.theme_cards[static_cast<std::size_t>(hit - 200)];
  else if (hit >= 400 && hit < 400 + static_cast<int>(layout_.theme_details.size()))
    bounds = &layout_.theme_details[static_cast<std::size_t>(hit - 400)];
  if (!bounds) return;
  auto rect = DipToPx(*bounds, dpi_);
  InflateRect(&rect, 2, 2);
  InvalidateRect(hwnd_, &rect, FALSE);
}

void SettingsUiHost::SelectPage(ui::SettingsPage page) {
  remembered_subpages_[static_cast<int>(draft_.page)] = draft_.subpage;
  draft_.page = page;
  draft_.subpage = remembered_subpages_[static_cast<int>(page)];
  draft_.focus = static_cast<int>(page);
  Relayout();
}
void SettingsUiHost::ActivateHit(int hit) {
  if (hit >= 0 && hit < ui::kSettingsPageCount) {
    SelectPage(static_cast<ui::SettingsPage>(hit));
  } else if (hit >= 500 && hit < 500 + ui::SettingsTabCount(draft_.page)) {
    draft_.subpage = hit - 500;
    draft_.focus = static_cast<int>(draft_.page);
    Relayout();
  } else if (hit >= 100 && hit < 100 + ui::kSettingsSchemeCount &&
             draft_.page == ui::SettingsPage::kInput && draft_.subpage == 0 && draft_.imported_schema.empty()) {
    draft_.schema_index = hit - 100;
    draft_.focus = hit;
    QueueSave(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
  } else if (hit >= 200 && hit < 204 &&
             draft_.page == ui::SettingsPage::kAppearance && draft_.subpage == 0) {
    draft_.theme_index = hit - 200;
    if (parity_) SendMessageW(GetDlgItem(hwnd_,6032), CB_SETCURSEL, 0, 0);
    draft_.theme_detail = -1;
    draft_.preview_theme = static_cast<ui::ThemeId>(draft_.theme_index);
    draft_.focus = hit;
    QueueSave(true);
    Relayout();
  } else if (hit >= 400 && hit < 404 &&
             draft_.page == ui::SettingsPage::kAppearance && draft_.subpage == 0) {
    draft_.theme_detail = draft_.theme_detail == hit - 400 ? -1 : hit - 400;
    draft_.focus = hit;
    Relayout();

  }
}

LRESULT CALLBACK SettingsUiHost::Procedure(HWND hwnd, UINT message,
                                           WPARAM wparam, LPARAM lparam) {
  auto* self =
      reinterpret_cast<SettingsUiHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<SettingsUiHost*>(
        reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  }
  if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);

  switch (message) {
    case WM_PRINTCLIENT:
      if (wparam) self->Paint(reinterpret_cast<HDC>(wparam));
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC dc = BeginPaint(hwnd, &ps);
      self->Paint(dc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_TIMER:
      if (wparam == 12) { self->CommitSave(); return 0; }
      self->updating_controls_ = true;
      if (self->parity_ && self->parity_->Tick()) self->Relayout();
      self->updating_controls_ = false;
      if (wparam == 11 && self->draft_.page == ui::SettingsPage::kPlugins) self->UpdatePlugins();
      return 0;
    case WM_APP + 73:
      self->Relayout();
      return 0;
    case WM_COMMAND:
      if (!self->ready_ || self->updating_controls_) return 0;
      if (LOWORD(wparam) == 424 && HIWORD(wparam) == BN_CLICKED) {
        std::string error;
        const bool enabled = !self->clipboard_recording_enabled_;
        if (self->callbacks_.save_clipboard_recording &&
            self->callbacks_.save_clipboard_recording(enabled, &error))
          self->clipboard_recording_enabled_ = enabled;
        else if (!error.empty()) self->SaveError(Wide(error));
        InvalidateRect(self->clipboard_recording_, nullptr, FALSE);
        return 0;
      }
      if (LOWORD(wparam) == 417 && HIWORD(wparam) == BN_CLICKED) {
        if (self->draft_.page == ui::SettingsPage::kHome) {
          if (self->draft_.subpage == 0) { ShowApplicationUpdate(hwnd); return 0; }
          if (const auto* link = ui::HomeLink(self->draft_.subpage)) {
            const bool opened = self->callbacks_.open_brand_link
                ? self->callbacks_.open_brand_link(link->destination)
                : reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", link->destination, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
            if (!opened) MessageBoxW(hwnd, (std::wstring(L"无法打开，请使用浏览器或邮件应用访问：\n\n") + link->destination).c_str(),
                                    L"打开链接", MB_OK | MB_ICONINFORMATION);
          }
        }
        return 0;
      }
      if (LOWORD(wparam) == 416 && HIWORD(wparam) == BN_CLICKED) {
        if (self->draft_.page == ui::SettingsPage::kCapsule && self->callbacks_.open_capsule) self->callbacks_.open_capsule();
        if (self->draft_.page == ui::SettingsPage::kMailbox && self->callbacks_.open_mailbox) self->callbacks_.open_mailbox();
        return 0;
      }
      if (self->parity_) {
        self->updating_controls_ = true;
        const bool handled = self->parity_->Command(LOWORD(wparam), HIWORD(wparam));
        self->updating_controls_ = false;
        if (handled) {
          const auto id = LOWORD(wparam);
          if ((HIWORD(wparam) == BN_CLICKED &&
               (id == 6001 || id == 6002 || id == 6003 || id == 6013 || id == 6016 ||
                id == 6024 || id == 6025 || id == 6041 || id == 6042 || id == 6050 || id == 6060 || id == 6062)))
            self->QueueSave(true);
          else if (id == 6000 && HIWORD(wparam) == CBN_SELCHANGE && self->pending_save_)
            self->CommitSave();
          if (HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == BN_CLICKED) self->Relayout();
          return 0;
        }
      }
      // Mapping editor and preview fields become a setting only through their
      // Add/Update action. Read-only gateway snippets and plugin filters do not.
      if (HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == CBN_EDITCHANGE ||
          HIWORD(wparam) == EN_KILLFOCUS || HIWORD(wparam) == CBN_SELCHANGE) {
        const auto id = LOWORD(wparam);
        if (id < 5100 || (id >= 6004 && id <= 6015) || (id >= 6030 && id <= 6035) || id == 6063) {
          if (HIWORD(wparam) == EN_KILLFOCUS) {
            if (self->pending_save_) self->CommitSave();
          } else self->QueueSave(HIWORD(wparam) == CBN_SELCHANGE);
        }
      }
      if (LOWORD(wparam) == 415 && HIWORD(wparam) == BN_CLICKED) {
        if (!self->callbacks_.open_schemes || !self->callbacks_.open_schemes())
          MessageBoxW(hwnd, L"无法打开方案管理。", rimes::branding::Text(L"{brand} 设置").c_str(), MB_OK | MB_ICONERROR);
        return 0;
      }
      if (LOWORD(wparam) == 425 && HIWORD(wparam) == BN_CLICKED) { self->OpenInputHabits(); return 0; }
      if (LOWORD(wparam) == 455 && HIWORD(wparam) == BN_CLICKED) {
        if (self->callbacks_.open_lexicon) self->callbacks_.open_lexicon(hwnd, self->draft_.preview_theme);
        return 0;
      }
      if (LOWORD(wparam) == 441 && HIWORD(wparam) == BN_CLICKED) {
        if (self->callbacks_.move_buffer_to_screen) self->callbacks_.move_buffer_to_screen();
        return 0;
      }
      if (LOWORD(wparam) >= 450 && LOWORD(wparam) <= 454 && HIWORD(wparam) == BN_CLICKED) {
        const auto action = LOWORD(wparam) - 450;
        if (action < 3) {
          if (!self->callbacks_.open_maintenance_location || !self->callbacks_.open_maintenance_location(action))
            self->SaveError(L"目录暂不可用，请检查安装及数据路径。");
        } else if (action == 3) {
          if (self->pending_save_ && !self->CommitSave()) return 0;
          std::string error;
          if (!self->callbacks_.restart_broker || !self->callbacks_.restart_broker(&error))
            self->SaveError(error.empty() ? L"无法启动新输入法进程，当前进程保持运行。" : Wide(error));
        }
        return 0;
      }
      if (HIWORD(wparam) == BN_CLICKED && (LOWORD(wparam) == 409 || LOWORD(wparam) == 410 || LOWORD(wparam) == 418 || LOWORD(wparam) == 419 || (LOWORD(wparam) >= 436 && LOWORD(wparam) <= 438))) {
        const int recording = LOWORD(wparam) >= 436 ? LOWORD(wparam) - 432 : LOWORD(wparam) >= 418 ? LOWORD(wparam) - 416 : LOWORD(wparam) - 409;
        const bool stop = self->recording_shortcut_ == recording;
        self->StopRecording();
        if (!stop) {
          self->recording_shortcut_ = recording;
          SetWindowTextW(recording >= 4 ? self->record_global_[recording - 4] : recording >= 2 ? self->record_plugin_[recording - 2] : recording ? self->record_send_ : self->record_hotkey_, L"请按快捷键…");
          SetWindowTextW(self->shortcut_feedback_, L"按下新的组合键；Esc 取消，Delete 恢复此项默认。完成修改后立即生效。");
        }
        return 0;
      }
      if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == 411) {
        self->StopRecording();
        SetWindowTextW(self->edit_hotkey_, L"B");
        SetWindowTextW(self->edit_send_, L"Enter");
        SetWindowTextW(self->edit_plugin_[0], L"Up");
        SetWindowTextW(self->edit_plugin_[1], L"Down");
        SetWindowTextW(self->edit_global_[0], L"S");
        SetWindowTextW(self->edit_global_[1], L"V");
        SetWindowTextW(self->edit_global_[2], L"4");
        for (const HWND combo : {self->combo_hotkey_modifiers_, self->combo_send_modifiers_, self->combo_plugin_modifiers_[0], self->combo_plugin_modifiers_[1], self->combo_global_modifiers_[0], self->combo_global_modifiers_[1], self->combo_global_modifiers_[2]})
          SendMessageW(combo, CB_SETCURSEL, BufferHotkeyChoiceIndex(MOD_ALT | MOD_SHIFT), 0);
        SetWindowTextW(self->shortcut_feedback_, L"已恢复默认 Alt+Shift+B / Enter / ↑ / ↓ / S / V / 4。");
        self->QueueSave(true);
        return 0;
      }
      if (LOWORD(wparam) == 405 && HIWORD(wparam) == CBN_SELCHANGE) {
        const auto choice = SendMessageW(self->combo_hotkey_modifiers_, CB_GETCURSEL, 0, 0);
        if (choice >= 0 && choice < static_cast<LRESULT>(kBufferHotkeyChoices.size()))
          self->draft_.hotkey_modifiers = kBufferHotkeyChoices[static_cast<std::size_t>(choice)].modifiers;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (LOWORD(wparam) == 5112 && HIWORD(wparam) == CBN_SELCHANGE) {
        self->plugin_offset_ = 0;
        self->Relayout();
        return 0;
      }
      if (HIWORD(wparam) == BN_CLICKED && (LOWORD(wparam) == 5113 || LOWORD(wparam) == 5114)) {
        const auto size = self->plugin_labels_.size();
        if (LOWORD(wparam) == 5113 && self->plugin_offset_ >= size) self->plugin_offset_ -= size;
        if (LOWORD(wparam) == 5114 && self->plugin_offset_ + size < self->plugin_rows_.size()) self->plugin_offset_ += size;
        self->Relayout();
        return 0;
      }
      if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) >= 5100 && LOWORD(wparam) < 5112) {
        const auto index = self->plugin_offset_ + static_cast<std::size_t>((LOWORD(wparam) - 5100) / 3);
        const int action = (LOWORD(wparam) - 5100) % 3;
        if (self->callbacks_.manage_plugin && index < self->plugin_rows_.size()) {
          const auto item = self->plugin_rows_[index]; std::string error;
          const std::string operation = action == 0 ? "install" : action == 2 ? "uninstall" : item.enabled ? "disable" : "enable";
          if (!self->callbacks_.manage_plugin(item.id, operation, &error)) MessageBoxW(hwnd, Wide(error).c_str(), rimes::branding::Text(L"{brand}").c_str(), MB_OK);
          self->Relayout();
        }
        return 0;
      }
      if (HIWORD(wparam) == BN_CLICKED) {
        switch (LOWORD(wparam)) {
          case 401: self->draft_.ascii = !self->draft_.ascii; break;
          case 402: self->draft_.traditional = !self->draft_.traditional; break;
          case 403: self->draft_.ascii_punctuation = !self->draft_.ascii_punctuation; break;
          case 404: self->draft_.vertical_candidates = !self->draft_.vertical_candidates; break;
          case 412: self->draft_.close_after_send = !self->draft_.close_after_send; break;
          case 439: self->draft_.align_to_input_box = !self->draft_.align_to_input_box; break;
          case 440: self->draft_.reset_on_app_switch = !self->draft_.reset_on_app_switch; break;
          default: return 0;
        }
        self->QueueSave(true);
        InvalidateRect(reinterpret_cast<HWND>(lparam), nullptr, FALSE);
        return 0;
      }
      break;
    case WM_DRAWITEM: {
      const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (self->parity_ && self->parity_->DrawControl(*draw, self->draft_.preview_theme, self->dpi_, self->fonts_.body)) return TRUE;
      if (draw->CtlType != ODT_BUTTON && draw->CtlType != ODT_COMBOBOX) break;
      const auto& p = ui::Palette(self->draft_.preview_theme);
      if (draw->CtlType == ODT_COMBOBOX || !(draw->CtlID >= 401 && draw->CtlID <= 404 || draw->CtlID == 412 || draw->CtlID == 424 || draw->CtlID == 439 || draw->CtlID == 440)) {
        const bool selected = (draw->itemState & ODS_SELECTED) != 0 ||
            GetPropW(draw->hwndItem, L"RimesSelectedSegment") != nullptr;
        const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
        if (draw->CtlType == ODT_BUTTON) {
          self->PaintChildBackground(draw->hwndItem, draw->hDC);
          ui::FillRoundRect(draw->hDC, draw->rcItem, MulDiv(6, static_cast<int>(self->dpi_), 96),
              ui::ToColorRef(selected ? p.surface_tertiary : p.surface));
          ui::StrokeRoundRect(draw->hDC, draw->rcItem, MulDiv(6, static_cast<int>(self->dpi_), 96), ui::ToColorRef(p.border));
        } else {
          ui::FillRectColor(draw->hDC, draw->rcItem,
              ui::ToColorRef(selected ? p.surface_tertiary : p.surface));
        }
        std::wstring text;
        if (draw->CtlType == ODT_COMBOBOX && draw->itemID != static_cast<UINT>(-1)) {
          const auto length = SendMessageW(draw->hwndItem, CB_GETLBTEXTLEN, draw->itemID, 0);
          if (length >= 0 && length < 1024) { text.resize(length + 1); SendMessageW(draw->hwndItem, CB_GETLBTEXT, draw->itemID, reinterpret_cast<LPARAM>(text.data())); text.resize(length); }
        } else text = GetText(draw->hwndItem);
        const auto old = SelectObject(draw->hDC, self->fonts_.control);
        SetBkMode(draw->hDC, TRANSPARENT); SetTextColor(draw->hDC, ui::ToColorRef(disabled ? p.text_muted : p.text_primary));
        RECT label = draw->rcItem; InflateRect(&label, -8, 0);
        DrawTextW(draw->hDC, text.c_str(), -1, &label, (draw->CtlType == ODT_BUTTON ? DT_CENTER : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(draw->hDC, old);
        if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC, &label);
        return TRUE;
      }
      ui::FillRectColor(draw->hDC, draw->rcItem,
                       ui::ToColorRef(draw->CtlID == 412 ? p.surface_secondary : p.settings_background));
      const bool checked = draw->CtlID == 401 ? self->draft_.ascii
          : draw->CtlID == 439 ? self->draft_.align_to_input_box
          : draw->CtlID == 440 ? self->draft_.reset_on_app_switch
          : draw->CtlID == 424 ? self->clipboard_recording_enabled_
          : draw->CtlID == 402 ? self->draft_.traditional
                               : draw->CtlID == 403 ? self->draft_.ascii_punctuation
                                : draw->CtlID == 412 ? self->draft_.close_after_send : self->draft_.vertical_candidates;
      RECT box = draw->rcItem;
      const int size = MulDiv(14, static_cast<int>(self->dpi_), 96);
      box.right = box.left + size;
      box.top += (box.bottom - box.top - size) / 2;
      box.bottom = box.top + size;
      ui::FillRoundRect(draw->hDC, box, MulDiv(3, static_cast<int>(self->dpi_), 96),
                        ui::ToColorRef(checked ? p.accent : p.surface));
      ui::StrokeRoundRect(draw->hDC, box, MulDiv(3, static_cast<int>(self->dpi_), 96),
                          ui::ToColorRef(checked ? p.accent : p.border_strong));
      if (checked) ui::DrawFluentIcon(draw->hDC, ui::FluentIcon::kCheck, box,
                                    ui::ToColorRef(p.accent_foreground));
      RECT text_box = draw->rcItem;
      text_box.left = box.right + MulDiv(8, static_cast<int>(self->dpi_), 96);
      const auto label = GetText(draw->hwndItem);
      const auto old = SelectObject(draw->hDC, self->fonts_.body);
      SetBkMode(draw->hDC, TRANSPARENT);
      SetTextColor(draw->hDC, ui::ToColorRef(p.text_primary));
      DrawTextW(draw->hDC, label.c_str(), -1, &text_box,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      SelectObject(draw->hDC, old);
      if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC, &draw->rcItem);
      return TRUE;
    }
    case WM_GETMINMAXINFO: {
      auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
      const int dpi = static_cast<int>(GetDpiForWindow(hwnd));
      RECT min_client{0, 0, MulDiv(860, dpi, 96), MulDiv(600, dpi, 96)};
      AdjustWindowRectExForDpi(&min_client, WS_OVERLAPPEDWINDOW, FALSE, 0,
                               dpi);
      limits->ptMinTrackSize = {min_client.right - min_client.left,
                                min_client.bottom - min_client.top};
      return 0;
    }
    case WM_SIZE:
      self->Relayout();
      return 0;
    case WM_VSCROLL:
    case WM_MOUSEWHEEL:
      if (self->parity_ && self->parity_->Scroll(message, wparam)) { self->Relayout(); return 0; }
      break;
    case WM_DPICHANGED: {
      auto* rect = reinterpret_cast<RECT*>(lparam);
      SetWindowPos(hwnd, nullptr, rect->left, rect->top,
                   rect->right - rect->left, rect->bottom - rect->top,
                   SWP_NOZORDER);
      self->dpi_ = HIWORD(wparam);
      self->Relayout();
      return 0;
    }
    case WM_MEASUREITEM: {
      auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
      if (measure->CtlType == ODT_COMBOBOX) { measure->itemHeight = MulDiv(24, static_cast<int>(self->dpi_), 96); return TRUE; }
      break;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
      const auto& p = ui::Palette(self->draft_.preview_theme);
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetBkColor(dc, ui::ToColorRef(p.surface));
      SetTextColor(dc, ui::ToColorRef(p.text_primary));
      if (message == WM_CTLCOLORSTATIC) {
        const auto child = reinterpret_cast<HWND>(lparam);
        wchar_t kind[16]{};
        GetClassNameW(child, kind, 16);
        // Disabled/read-only edits also use WM_CTLCOLORSTATIC, but still need
        // the field surface instead of the label's parent background.
        if (_wcsicmp(kind, L"Static") == 0) {
          self->PaintChildBackground(child, dc);
          SetBkMode(dc, TRANSPARENT);
          SetTextColor(dc, ui::ToColorRef(!IsWindowEnabled(child) || GetPropW(child, L"RimesMuted") ? p.text_muted :
              GetPropW(child, L"RimesError") ? p.danger_text : GetPropW(child, L"RimesAccent") ? p.accent_text :
              GetPropW(child, L"RimesSecondary") ? p.text_secondary : p.text_primary));
          return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
        }
      }
      if (!self->edit_brush_)
        self->edit_brush_ = CreateSolidBrush(ui::ToColorRef(p.surface));
      return reinterpret_cast<LRESULT>(self->edit_brush_);
    }
    case WM_LBUTTONDOWN: {
      SetFocus(hwnd);
      self->draft_.keyboard_focus = false;
      const float x = static_cast<float>(static_cast<short>(LOWORD(lparam))) *
                      96.0f / self->dpi_;
      const float y = static_cast<float>(static_cast<short>(HIWORD(lparam))) *
                      96.0f / self->dpi_;
      self->pressed_hit_ = ui::HitTestSettings(self->layout_, x, y);
      SetCapture(hwnd);
      return 0;
    }
    case WM_LBUTTONUP: {
      const int pressed = self->pressed_hit_;
      self->pressed_hit_ = -2;
      if (GetCapture() == hwnd) ReleaseCapture();
      if (pressed == -2) return 0;
      const float x = static_cast<float>(static_cast<short>(LOWORD(lparam))) *
                      96.0f / self->dpi_;
      const float y = static_cast<float>(static_cast<short>(HIWORD(lparam))) *
                      96.0f / self->dpi_;
      const int hit = ui::HitTestSettings(self->layout_, x, y);
      if (hit != pressed) return 0;
      if (self->draft_.theme_detail >= 0 && hit != 600 &&
          !(hit >= 400 && hit < 404)) {
        self->draft_.theme_detail = -1;
        self->Relayout();
        return 0;
      }
      self->ActivateHit(hit);
      return 0;
    }
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE && self->extensions_) self->extensions_->Deactivate();
      if (LOWORD(wparam) == WA_INACTIVE) self->StopRecording();
      if (LOWORD(wparam) == WA_INACTIVE && self->draft_.theme_detail >= 0) {
        self->draft_.theme_detail = -1;
        self->Relayout();
      }
      break;
    case WM_CANCELMODE:
      if (GetCapture() == hwnd) ReleaseCapture();
      [[fallthrough]];
    case WM_CAPTURECHANGED:
      self->pressed_hit_ = -2;
      return 0;
    case WM_MOUSEMOVE: {
      const float x = static_cast<float>(static_cast<short>(LOWORD(lparam))) *
                      96.0f / self->dpi_;
      const float y = static_cast<float>(static_cast<short>(HIWORD(lparam))) *
                      96.0f / self->dpi_;
      const int hover = ui::HitTestSettings(self->layout_, x, y);
      if (self->draft_.hover != hover) {
        const int previous = self->draft_.hover;
        self->draft_.hover = hover;
        self->InvalidateHover(previous);
        self->InvalidateHover(hover);
      }
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&track);
      return 0;
    }
    case WM_MOUSELEAVE:
      self->InvalidateHover(self->draft_.hover);
      self->draft_.hover = -1;
      return 0;
    case WM_SETCURSOR:
      if (reinterpret_cast<HWND>(wparam) == hwnd && LOWORD(lparam) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        const float x = static_cast<float>(point.x) * 96.0f / self->dpi_;
        const float y = static_cast<float>(point.y) * 96.0f / self->dpi_;
        const int hit = ui::HitTestSettings(self->layout_, x, y);
        const bool actionable = hit >= 0 && hit != 600;
        SetCursor(LoadCursorW(nullptr, actionable ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        if (self->draft_.theme_detail >= 0) {
          self->draft_.theme_detail = -1;
          self->Relayout();
          return 0;
        }
        self->Close();
        return 0;
      }
      if (wparam >= '1' && wparam <= '9' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        self->SelectPage(ui::kSettingsNavOrder[wparam - '1']);
        return 0;
      }
      if (wparam == VK_UP || wparam == VK_DOWN || wparam == VK_LEFT ||
          wparam == VK_RIGHT) {
        auto& focus = self->draft_.focus;
        if (focus < 0) focus = static_cast<int>(self->draft_.page);
        if (focus < ui::kSettingsPageCount) {
          auto index = ui::SettingsNavIndex(static_cast<ui::SettingsPage>(focus));
          do {
            const int step = (wparam == VK_DOWN || wparam == VK_RIGHT) ? 1 : ui::kSettingsPageCount - 1;
            index = (index + step) % ui::kSettingsPageCount;
          } while (!ui::SettingsPageAvailable(self->draft_, ui::kSettingsNavOrder[index]));
          focus = static_cast<int>(ui::kSettingsNavOrder[index]);
          self->SelectPage(static_cast<ui::SettingsPage>(focus));
          return 0;
        }
        if (self->draft_.page == ui::SettingsPage::kInput &&
            self->draft_.subpage == 0 && focus >= 100 && focus < 100 + ui::kSettingsSchemeCount) {
          int idx = focus - 100;
          if (wparam == VK_RIGHT || wparam == VK_DOWN) idx = (idx + 1) % ui::kSettingsSchemeCount;
          else idx = (idx + ui::kSettingsSchemeCount - 1) % ui::kSettingsSchemeCount;
          focus = 100 + idx;
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        if (self->draft_.page == ui::SettingsPage::kAppearance &&
            self->draft_.subpage == 0 &&
            ((focus >= 200 && focus < 204) || (focus >= 400 && focus < 404))) {
          const int base = focus >= 400 ? 400 : 200;
          int idx = focus - base;
          if (wparam == VK_RIGHT || wparam == VK_DOWN) idx = (idx + 1) % 4;
          else idx = (idx + 3) % 4;
          focus = base + idx;
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
      }
      if (wparam == VK_SPACE || wparam == VK_RETURN) {
        self->ActivateHit(self->draft_.focus);
        return 0;
      }
      break;
    case WM_CLOSE:
      self->Close();
      return 0;
    case WM_DESTROY:
      self->ready_ = false;
      KillTimer(hwnd, 12);
      self->extensions_.reset();
      self->parity_.reset();
      self->recording_shortcut_ = -1; self->recorded_key_ = 0;
      self->edit_send_ = self->combo_send_modifiers_ = nullptr;
      self->edit_plugin_ = {}; self->combo_plugin_modifiers_ = {}; self->record_plugin_ = {};
      self->edit_global_ = {}; self->combo_global_modifiers_ = {}; self->record_global_ = {};
      self->check_align_ = self->check_reset_app_ = self->move_buffer_ = nullptr;
      self->maintenance_actions_ = {};
      self->record_hotkey_ = self->record_send_ = self->reset_shortcuts_ = self->shortcut_feedback_ = nullptr;
      KillTimer(hwnd, 11);
      self->hwnd_ = nullptr;
      self->schemes_button_ = self->imported_label_ = self->input_habits_ = nullptr;
      self->lexicon_button_ = nullptr;
      self->peripheral_open_ = self->home_action_ = nullptr;
      self->clipboard_recording_ = nullptr;
      self->plugin_filter_ = self->plugin_previous_ = self->plugin_next_ = self->plugin_page_ = nullptr;
      self->edit_font_ = self->edit_candidate_count_ = self->edit_hotkey_ = self->edit_base_ = nullptr;
      self->combo_hotkey_modifiers_ = nullptr;
      self->edit_model_ = self->edit_key_ = self->edit_lang_ = nullptr;
      self->edit_source_lang_ = nullptr;
      self->combo_auto_send_ = self->check_close_after_ = self->combo_translation_ = nullptr;
      self->combo_connector_ = self->edit_codex_path_ = self->edit_codex_model_ = nullptr;
      self->check_vertical_ = self->check_ascii_ = self->check_trad_ = self->check_punct_ = nullptr;

      if (self->callbacks_.on_closed) self->callbacks_.on_closed();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace rimes::windows::workbench
