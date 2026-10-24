#include "ai_choices.hpp"
#include "../core/product_brand.hpp"
#include "rimes_version.hpp"
#include "window.hpp"

#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../broker/autostart.hpp"
#include "../broker/default_paths.hpp"
#include "../ui/buffer_layout.hpp"
#include "../ui/layered_frame.hpp"
#include "../ui/buffer_paint.hpp"
#include "../ui/icons.hpp"
#include "../ui/menu_draw.hpp"
#include "../ui/theme.hpp"
#include "buffer_hotkey.hpp"
#include "buffer_anchor.hpp"
#include "settings_ui.hpp"
#include "user_lexicon.hpp"
#include "translation_speech.hpp"
#include "../peripherals/window.hpp"

#pragma comment(lib, "comctl32.lib")

namespace rimes::windows::workbench {
namespace {
constexpr UINT kChanged = WM_APP + 80, kTray = WM_APP + 81;
constexpr UINT kOpenSettings = WM_APP + 82;
constexpr UINT kUpdateBufferZOrder = WM_APP + 83;
constexpr UINT kMaintenanceExit = WM_APP + 84;
constexpr int kToggle = 100, kSettings = 101, kDeploy = 102, kStartup = 103,
              kAbout = 105, kExit = 104, kPasteMenu = 106;
constexpr int kModeInput = 110, kModeGenerate = 111, kModeTranslate = 112;
constexpr int kCapsule = 113, kMailbox = 114;

std::wstring BufferHotkeyTitle(const Settings& settings) {
  return ShortcutTitle(settings.hotkey_key, settings.hotkey_modifiers);
}

DWORD ForegroundProcess() {
  DWORD process = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process);
  return process;
}

BufferRect DipFrame(const RECT& rect, double scale) {
  return {rect.left / scale, rect.top / scale,
          (static_cast<double>(rect.right) - rect.left) / scale,
          (static_cast<double>(rect.bottom) - rect.top) / scale};
}
BufferRect DipFrame(BufferRect rect, double scale) {
  return {rect.x / scale, rect.y / scale, rect.width / scale, rect.height / scale};
}
struct BufferMonitor {
  RECT work{};
  double scale = 1;
};
RECT BufferMonitorWorkArea(HMONITOR monitor) {
  MONITORINFO info{sizeof(info)};
  RECT work{};
  if (monitor && GetMonitorInfoW(monitor, &info)) work = info.rcWork;
  else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  return work;
}
BufferMonitor BufferMonitorInfo(HMONITOR monitor) {
  BufferMonitor result;
  result.work = BufferMonitorWorkArea(monitor);
  // GetDpiForWindow on our own invisible PMv2 window measures the destination
  // display, even when the host or previous Buffer is on a different DPI.
  const HWND probe = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"STATIC", L"", WS_POPUP, result.work.left, result.work.top, 1, 1,
      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (probe) {
    const auto dpi = GetDpiForWindow(probe);
    if (dpi) result.scale = static_cast<double>(dpi) / 96;
    DestroyWindow(probe);
  }
  return result;
}
HMONITOR BufferFallbackMonitor() {
  POINT mouse{};
  GetCursorPos(&mouse);
  return MonitorFromPoint(mouse, MONITOR_DEFAULTTOPRIMARY);
}
ui::BufferLayout WorkbenchLayout(const ui::BufferPaintState& paint, float width) {
  ui::BufferMetrics metrics;
  // Match macOS on work areas narrower than the normal readable minimum.
  metrics.min_width_dip = (std::min)(metrics.min_width_dip,
                                    (std::max)(1, static_cast<int>(width)));
  return ui::LayoutBuffer(paint, width, metrics);
}

struct Window {
  std::shared_ptr<ui::SmoothShapesSession> smooth_shapes = ui::AcquireSmoothShapes();
  Runtime& runtime;
  std::function<void()> stop, deploy;
  UiCommands* commands = nullptr;
  UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  HWND window = nullptr;
  bool destroying = false;
  std::unique_ptr<SettingsUiHost> settings;
  std::unique_ptr<peripherals::Windows> peripherals;
  std::uint64_t peripheral_tick = 0;
  std::atomic<HWND> notification{nullptr};
  ID2D1Factory* factory = nullptr;
  IDWriteFactory* write = nullptr;
  ID2D1DCRenderTarget* render = nullptr;
  ui::LayeredFrame paint_frame;
  ID2D1SolidColorBrush* brush = nullptr;
  IDWriteTextFormat* format = nullptr;
  IDWriteTextFormat* label_format = nullptr;
  NOTIFYICONDATAW tray{};
  float scroll[2] = {0, 0};
  ui::BufferMode mode = ui::BufferMode::kInput;
  ui::ThemeId theme = ui::ThemeId::kNight;
  ui::BufferLayout last_layout{};
  ui::BufferHitKind pressed_hit = ui::BufferHitKind::kNone;
  int hover = -1;
  HWND tooltip = nullptr;
  std::wstring tooltip_text;
  TranslationSpeech speech;
  std::uint64_t pressed_speech_block = 0;
  std::string speech_language;
  std::jthread maintenance;
  std::atomic_bool deploying = false;
  HICON product_icon = nullptr;
  HFONT menu_font = nullptr;
  std::map<UINT, std::wstring> menu_labels;
  std::unique_ptr<BufferHotkeyRegistration> buffer_hotkey;
  std::array<std::unique_ptr<BufferHotkeyRegistration>, 3> global_hotkeys;
  std::unique_ptr<BufferHotkeyRegistration> send_hotkey;
  std::array<std::unique_ptr<BufferHotkeyRegistration>, 2> plugin_hotkeys;
  std::array<bool, 2> plugin_hotkey_warning{};
  unsigned held_send_key = 0;
  std::uint64_t shown_revision = 0;
  std::size_t shown_insertion = 0;
  std::wstring shown_preedit;
  unsigned shown_preedit_caret = 0;
  float last_result_width = 0;
  HICON target_icon = nullptr;
  DWORD target_pid = 0;
  unsigned target_icon_dpi = 0;
  std::uint64_t placed_opening_revision = 0;
  std::uint64_t placed_geometry_revision = 0;
  BufferOpeningSide opening_side = BufferOpeningSide::kBottomFallback;
  bool positioning = false;
  double positioning_scale = 1;
  RECT positioning_work{};

  Window(Runtime& r, std::function<void()> s, std::function<void()> d,
         UiCommands* c)
      : runtime(r), stop(std::move(s)), deploy(std::move(d)), commands(c) {}
  ~Window() {
    if (target_icon) DestroyIcon(target_icon);
    if (maintenance.joinable()) maintenance.join();
    if (product_icon) DestroyIcon(product_icon);
    if (menu_font) DeleteObject(menu_font);
    if (label_format) label_format->Release();
    if (format) format->Release();
    if (brush) brush->Release();
    if (render) render->Release();
    if (write) write->Release();
    if (factory) factory->Release();
  }

  ui::ThemeId ActiveTheme() const {
    return ui::ThemeIdOrDefault(EffectiveTheme(runtime.Configuration()));
  }
  void EnsureSettings();
  bool SaveWithGlobalHotkeys(const Settings&, const std::function<bool()>&, std::string*);
  void MoveBufferToCurrentScreen();
  void OpenSettings();
  void OpenPeripheral(bool mailbox, bool toggle = false) {
    try {
      if (!peripherals) peripherals = std::make_unique<peripherals::Windows>(runtime);
      if (toggle) peripherals->Toggle(mailbox);
      else peripherals->Open(mailbox);
      UpdateBufferZOrder();
    } catch (const std::exception& e) { MessageBoxW(window, Wide(e.what()).c_str(), L"XSIME", MB_OK | MB_ICONERROR); }
  }
  void UpdateBufferZOrder();
  void Paint();
  void Update();
  void PositionForOpening(const core::Json& state);
  void Action(int index);
  void Copy();
  void Paste();
  void ApplyHit(ui::BufferHitKind hit);
  void PopupModeMenu();
  void PopupTranslationLanguage(bool source);
  std::uint64_t SpeechBlockAt(float x, float y);
  void PopupAIChoice(bool model);
  void PopupTrayMenu();
  void AddTrayIcon();
  void RefreshTrayIcon();
  HMENU BuildOwnerMenu(const std::vector<ui::OwnerMenuItem>& items);
  void UpdateTooltip(int hit);
  void UpdateTargetIcon(DWORD process);
  void PublishLogicalCaret(const ui::BufferPaintState& paint);
  bool folded = false;
  unsigned configured_width = 0;
  ui::BufferPaintState MakePaintState(const core::Json& state) const;
  static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
};

ui::BufferPaintState Window::MakePaintState(const core::Json& state) const {
  ui::BufferPaintState paint{};
  paint.theme = theme;
  paint.folded = folded;
  paint.mode = static_cast<ui::BufferMode>(state.value("mode", 0));
  for (const auto& plugin : runtime.BufferPlugins())
    if (plugin.id == state.value("plugin", "")) { paint.plugin_name = Wide(plugin.name); break; }
  paint.insertion = state.value("insertion", std::size_t{});
  paint.preedit_caret = state.value("preedit_caret", 0U);
  paint.selected = state.value("selected", false);
  paint.auto_send_seconds = state.value("auto_send_seconds", 0U);
  paint.close_after_send = state.value("close_after_send", false);
  paint.live_metrics = Wide(state.value("live_metrics", std::string()));
  paint.show_ai_choices = state.value("plugin", "") == "builtin.codex-cli";
  paint.ai_model = Wide(state.value("chatgpt_model", "gpt-6-sol"));
  for (const auto& choice : kChatGPTModels) if (state.value("chatgpt_model", "") == choice.id) paint.ai_model = choice.title;
  paint.ai_effort = Wide(state.value("chatgpt_effort", "medium"));
  paint.translation_speech = state.value("translation_speech", false);
  paint.translation_controls_enabled = state.value("translation_controls_enabled", true);
  auto language_title = [](const std::string& value) {
    const auto id = TranslationLanguageID(value);
    for (const auto& language : kTranslationLanguages)
      if (id == language.id) return std::wstring(language.title);
    return Wide(value);
  };
  paint.source_language = language_title(state.value("source_language", std::string("Chinese")));
  paint.target_language = language_title(state.value("target_language", std::string("English")));
  paint.spinner_phase = static_cast<float>(GetTickCount64() % 1000) * 0.006283f;
  paint.send_hold_progress = state.value("send_hold_progress", -1.0f);
  paint.busy = state.value("busy", false);
  paint.translate = state.value("translate", false);
  paint.capturing = state.value("capture", false);
  paint.bound = state.value("target_pid", 0) != 0;
  const auto status = state.value("status", std::string());
  static const std::map<std::string, std::wstring> messages = {
      {"Buffer", L"已绑定"}, {"Ready", L"结果就绪"},
      {"Copy only - no input target", L"未绑定：先选输入框，再点原文区"},
      {"Target changed. Rebind to send.", L"输入已暂停：点击原文区重新绑定"},
      {"Protected", L"已暂停"},
      {"Waiting for response...", L"等待响应…"},
      {"Receiving...", L"接收中…"},
      {"Processing stopped. Source retained.", L"已停止处理，原文已保留"},
      {"Request failed. Source retained.", L"请求失败，原文已保留"},
      {"Sending...", L"发送中…"}, {"Delivered", L"已发送"},
      {"Delivery failed. Content retained.", L"发送失败，内容已保留"},
      {"Delivery state changed. Check the target before retrying.",
       L"发送状态已变化，请先检查输入框"},
      {"Delivery unconfirmed. Check the target; automatic retry disabled.",
       L"发送未确认，请先检查输入框"},
      {"Paste exceeds Buffer capacity or delivery is pending.",
       L"粘贴内容过长，或正在发送"}};
  const auto message = messages.find(status);
  paint.status = message != messages.end() ? message->second : Wide(status);
  if (paint.status.empty())
    paint.status = paint.capturing ? L"已绑定" : L"等待输入框";
  const bool has_result = !state.value("result", std::string()).empty();
  const bool has_source = !state.value("source", std::string()).empty();
  if (has_result && !paint.translate)
    paint.status += L" · 待发结果已保留";
  paint.copy_enabled = !paint.busy && !state.value("pending", false) &&
      !state.value("uncertain", false) && state.value("preedit", std::string()).empty() &&
      (has_result ? state.value("complete_result", false) : (has_source && (paint.selected || !paint.bound)));
  paint.send_enabled = paint.capturing && paint.bound &&
                       !state.value("uncertain", false) &&
                       (has_result || has_source) &&
                       (!paint.busy || has_result) && status != "Sending...";
  if (paint.mode != ui::BufferMode::kInput && !has_result)
    paint.send_enabled = has_source && !paint.busy &&
        state.value("generation_ready", true) &&
        state.value("preedit", std::string()).empty() && status != "Sending...";
  paint.paste_enabled = status != "Sending...";
  paint.preedit = Wide(state.value("preedit", std::string()));
  paint.preview = Wide(state.value("preview", std::string()));
  paint.reasoning_summary = Wide(state.value("reasoning_summary", std::string()));
  if (paint.mode == ui::BufferMode::kTranslate && !has_result) {
    paint.result_hint = (status == "Buffer" || status == "Ready" || status == "Delivered")
        ? L"等待翻译…" : paint.status;
  }
  paint.scroll_source = scroll[0];
  paint.scroll_result = scroll[1];
  paint.hover = hover;
  paint.pressed = static_cast<int>(pressed_hit);
  paint.empty_hint = paint.capturing ? L"等待输入"
      : rimes::branding::Text(L"先点击宿主输入框，再点击这里开始输入（需选中 {brand}）").c_str();
  for (const auto& block : state["source_blocks"])
    paint.source_blocks.push_back(
        {Wide(block.value("text", std::string())), false,
         paint.mode == ui::BufferMode::kInput && paint.auto_send_seconds
             ? float(block.value("age_ms", 0ULL)) / (paint.auto_send_seconds * 1000) : 0});
  for (const auto& block : state["result_blocks"])
    paint.result_blocks.push_back(
        {Wide(block.value("text", std::string())), false});
  if (state.contains("stream_blocks") && !state["stream_blocks"].empty()) {
    paint.preview.clear();
    for (const auto& block : state["stream_blocks"])
      paint.result_blocks.push_back({Wide(block.value("text", std::string())), true});
  }
  if (paint.busy && paint.result_blocks.empty())
    paint.result_hint = paint.status + L" · " + std::to_wstring(state.value("waiting_seconds", 0ULL)) + L" 秒";
  else if (!paint.busy && paint.mode != ui::BufferMode::kInput && !has_result &&
           !state.value("generation_ready", true))
    paint.result_hint = Wide(state.value("generation_unavailable", std::string()));
  return paint;
}

void Window::EnsureSettings() {
  if (settings) return;
  SettingsUiCallbacks cb;
  cb.input_options = runtime.input_option_provider;
  cb.connectors = NativeConnectorUiServices();
  cb.metrics = runtime.Metrics();
  cb.open_schemes = [this] {
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, 32768);
    return count > 0 && count < 32768 && reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", path, L"--schemes", nullptr, SW_SHOWNORMAL)) > 32;
  };
  cb.load = [this] { return runtime.Configuration(); };
  cb.open_capsule = [this] { OpenPeripheral(false); };
  cb.open_mailbox = [this] { OpenPeripheral(true); };
  cb.load_clipboard_recording = [this] {
    try { return peripherals && peripherals->CollectClipboard(); } catch (...) { return false; }
  };
  cb.save_clipboard_recording = [this](bool enabled, std::string* error) {
    try {
      if (!peripherals) peripherals = std::make_unique<peripherals::Windows>(runtime);
      peripherals->SetCollectClipboard(enabled);
      return true;
    } catch (const std::exception& failure) { if (error) *error = failure.what(); return false; }
  };
  cb.save_connections = [this](Settings value, const std::map<std::string, std::wstring>& secrets, std::string* error) {
    if (!SaveWithGlobalHotkeys(value, [&] { return runtime.Configure(value, L"", false, error, secrets); }, error)) return false;
    RefreshTrayIcon(); InvalidateRect(window, nullptr, FALSE); return true;
  };
  cb.save = [this](Settings value, const std::wstring& key, bool replace,
                   std::string* error) {
    if (!SaveWithGlobalHotkeys(value, [&] { return runtime.Configure(value, key, replace, error); }, error)) return false;
    wcscpy_s(tray.szTip, rimes::branding::Text(L"{brand} 输入法与 Buffer").c_str());
    Shell_NotifyIconW(NIM_MODIFY, &tray);
    RefreshTrayIcon();
    InvalidateRect(window, nullptr, FALSE);
    return true;
  };
  cb.plugins = [this] {
    auto entries = runtime.Plugins();
    if (peripherals) {
      auto modules = peripherals->Plugins();
      entries.insert(entries.end(), modules.begin(), modules.end());
    }
    return entries;
  };
  cb.manage_plugin = [this](const std::string& id, const std::string& action, std::string* error) {
    if (id.starts_with("builtin.capsule.") || id.starts_with("builtin.mailbox.")) {
      if (peripherals && (action == "enable" || action == "disable"))
        return peripherals->SetPluginEnabled(id, action == "enable", error);
      if (error) *error = "此内置插件随宿主安装，可在这里启用或停用。";
      return false;
    }
    return runtime.ManagePlugin(id, action, error);
  };
  cb.plugin_status = [this] { return runtime.PluginStatus(); };
  cb.load_theme = [this] { return ActiveTheme(); };
  cb.on_theme_preview = [this](ui::ThemeId preview) {
    theme = preview;
    if (peripherals) peripherals->PreviewTheme(preview);
    InvalidateRect(window, nullptr, FALSE);
  };
  cb.on_closed = [this] {
    if (peripherals) peripherals->PreviewTheme(std::nullopt);
    // Settings' WM_DESTROY callback runs before DestroyWindow has finished.
    // Defer native placement until that operation has unwound; a reentrant
    // SetWindowPos can succeed without applying the topmost style change.
    if (!destroying && window)
      PostMessageW(window, kUpdateBufferZOrder, 0, 0);
  };
  cb.about_text = std::wstring(rimes::branding::Text(L"{brand} Windows ").c_str()) + kProductVersionWide + L"\nCommit: " + Wide(RIMES_BUILD_COMMIT) +
                  L"\n协议 v2\n词库：" + runtime.user_data_directory.wstring() +
                  L"\n日志：" + runtime.log_directory.wstring() + L"\n设置与历史：%LOCALAPPDATA%\\RIMES";
  cb.move_buffer_to_screen = [this] { MoveBufferToCurrentScreen(); };
  cb.restart_broker = runtime.restart_broker;
  if (runtime.user_lexicon) cb.open_lexicon = [service = runtime.user_lexicon](HWND owner, ui::ThemeId selected_theme) {
    ShowUserLexicon(owner, service, selected_theme);
  };
  cb.open_maintenance_location = [this](unsigned action) {
    auto path = action == 0 ? runtime.log_directory : action == 1 ? runtime.user_data_directory : std::filesystem::path{};
    if (action == 2) {
      wchar_t local[32768]{};
      const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
      if (!length || length >= 32768) return false;
      path = std::filesystem::path(local) / L"RIMES";
    }
    return !path.empty() && reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
  };
  settings = std::make_unique<SettingsUiHost>(std::move(cb));
}

bool Window::SaveWithGlobalHotkeys(const Settings& value, const std::function<bool()>& save, std::string* error) {
  if (!ValidBufferShortcuts(value)) {
    if (error) *error = "快捷键无效或与另一项重复，原设置保持不变。";
    return false;
  }
  const unsigned keys[] = {value.hotkey_key, value.settings_key, value.capsule_key, value.capture_key};
  const unsigned mods[] = {value.hotkey_modifiers, value.settings_modifiers, value.capsule_modifiers, value.capture_modifiers};
  const auto previous = runtime.Configuration();
  const unsigned old_keys[] = {previous.hotkey_key, previous.settings_key, previous.capsule_key, previous.capture_key};
  const unsigned old_mods[] = {previous.hotkey_modifiers, previous.settings_modifiers, previous.capsule_modifiers, previous.capture_modifiers};
  BufferHotkeyRegistration* registrations[] = {buffer_hotkey.get(), global_hotkeys[0].get(), global_hotkeys[1].get(), global_hotkeys[2].get()};
  const wchar_t* names[] = {L"Buffer", L"设置", L"Capsule", L"截图"};
  std::array<GlobalHotkeyBinding, 4> bindings;
  for (std::size_t i = 0; i < bindings.size(); ++i)
    bindings[i] = {registrations[i], mods[i], keys[i], keys[i] != old_keys[i] || mods[i] != old_mods[i]};
  const bool was_aligned = previous.align_to_input_box;
  std::size_t failed = 0;
  const auto result = UpdateGlobalHotkeys(bindings, save, &failed);
  if (result != HotkeyUpdate::kSaved) {
    if ((result == HotkeyUpdate::kInvalid || result == HotkeyUpdate::kUnavailable) && error)
      *error = Utf8(std::wstring(names[failed]) + L"快捷键 " + ShortcutTitle(keys[failed], mods[failed]) + L" 不可用，修改未保存；原快捷键保持有效。");
    return false;
  }
  if (value.align_to_input_box && !was_aligned) placed_opening_revision = UINT64_MAX;
  return true;
}

void Window::MoveBufferToCurrentScreen() {
  const auto display = BufferMonitorInfo(BufferFallbackMonitor());
  RECT rect{}; GetWindowRect(window, &rect);
  const auto old_scale = (std::max)(1.0, static_cast<double>(GetDpiForWindow(window)) / 96);
  const auto frame = BufferCenteredOnScreen(DipFrame(rect, old_scale), DipFrame(display.work, display.scale));
  positioning = true; positioning_scale = display.scale; positioning_work = display.work;
  SetWindowPos(window, nullptr, static_cast<int>(std::lround(frame.x * display.scale)), static_cast<int>(std::lround(frame.y * display.scale)),
      static_cast<int>(std::lround(frame.width * display.scale)), static_cast<int>(std::lround(frame.height * display.scale)), SWP_NOZORDER | SWP_NOACTIVATE);
  positioning = false; opening_side = BufferOpeningSide::kBottomFallback;
  const auto state = runtime.Snapshot();
  placed_opening_revision = state.value("opening_revision", 0ULL);
  placed_geometry_revision = state.value("placement_revision", 0ULL);
  InvalidateRect(window, nullptr, FALSE);
}

void Window::OpenSettings() {
  // Settings revoke the old host's capture authority. They do not hide the
  // Buffer, consume its blocks, or cancel its source/configuration-frozen job.
  runtime.PauseCapture();
  EnsureSettings();
  // The host retains Settings' lifetime; this HWND is only a placement anchor.
  settings->Open(window);
  UpdateBufferZOrder();
}

void Window::UpdateBufferZOrder() {
  if (destroying || !window) return;
  constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
  const bool topmost =
      (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
  if (!topmost) {
    // Like macOS's floating NSPanel, Buffer has its own presentation lifetime.
    // Keeping Settings open must not demote it behind the external input host.
    // Placement neither activates Buffer nor grants text-delivery authority.
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, flags);
  }
}

void Window::AddTrayIcon() {
  if (!Shell_NotifyIconW(NIM_ADD, &tray))
    Shell_NotifyIconW(NIM_MODIFY, &tray);
}

void Window::RefreshTrayIcon() {
  auto icon = ui::CreateTrayIcon(GetSystemMetrics(SM_CXSMICON));
  if (!icon) return;
  const auto previous = product_icon;
  product_icon = icon;
  tray.hIcon = icon;
  Shell_NotifyIconW(NIM_MODIFY, &tray);
  if (previous) DestroyIcon(previous);
}

HMENU Window::BuildOwnerMenu(const std::vector<ui::OwnerMenuItem>& items) {
  HMENU menu = CreatePopupMenu();
  menu_labels.clear();
  for (const auto& item : items) {
    if (item.separator) {
      AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
      continue;
    }
    menu_labels[item.id] = item.text;
    UINT flags = MF_OWNERDRAW | (item.enabled ? 0 : MF_GRAYED);
    if (item.checked) flags |= MF_CHECKED;
    AppendMenuW(menu, flags, item.id, MAKEINTRESOURCEW(item.id));
  }
  return menu;
}

void Window::PopupModeMenu() {
  const auto plugins = runtime.BufferPlugins();
  const auto selected = runtime.Snapshot().value("plugin", "");
  std::vector<ui::OwnerMenuItem> items;
  for (std::size_t i = 0; i < plugins.size(); ++i)
    items.push_back({static_cast<UINT>(kModeInput + i), Wide(plugins[i].name), plugins[i].id == selected});
  auto menu = BuildOwnerMenu(items);
  POINT cursor{};
  GetCursorPos(&cursor);
  const auto command =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0,
                     window, nullptr);
  DestroyMenu(menu);
  if (command >= kModeInput && static_cast<std::size_t>(command - kModeInput) < plugins.size())
    runtime.SelectBufferPlugin(plugins[command - kModeInput].id);
  InvalidateRect(window, nullptr, FALSE);
}

void Window::PopupTranslationLanguage(bool source) {
  const auto config = runtime.Configuration();
  std::vector<ui::OwnerMenuItem> items;
  const auto selected = TranslationLanguageID(source ? config.source_language : config.target_language);
  for (std::size_t i = 0; i < kTranslationLanguages.size(); ++i) {
    const auto& language = kTranslationLanguages[i];
    if (config.translation_provider == "local" && !LocalTranslationLanguage(language.id)) continue;
    items.push_back({static_cast<UINT>(3000 + i), language.title, selected == language.id});
  }
  const auto menu = BuildOwnerMenu(items);
  POINT cursor{}; GetCursorPos(&cursor);
  const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
      cursor.x, cursor.y, 0, window, nullptr);
  DestroyMenu(menu);
  if (command < 3000 || static_cast<std::size_t>(command - 3000) >= kTranslationLanguages.size()) return;
  auto from = config.source_language, to = config.target_language;
  (source ? from : to) = kTranslationLanguages[command - 3000].name;
  // Selecting the other side's language swaps the pair, as on macOS.
  if (TranslationLanguageID(from) == TranslationLanguageID(to))
    (source ? to : from) = source ? config.source_language : config.target_language;
  if (!runtime.SetTranslationLanguages(from, to)) MessageBeep(MB_OK);
}

std::uint64_t Window::SpeechBlockAt(float x, float y) {
  const auto snapshot = runtime.Snapshot();
  const auto state = MakePaintState(snapshot);
  if (!snapshot.value("visible", false) || state.mode != ui::BufferMode::kTranslate ||
      !state.translation_speech || !last_layout.result_text.contains(x, y)) return 0;
  float left = last_layout.result_text.left - scroll[1];
  for (std::size_t i = 0; i < state.result_blocks.size(); ++i) {
    const auto width = ui::MeasureBufferContent(write, format, {state.result_blocks[i]}, L"", false);
    if (x >= left && x < left + width - 5)
      return state.result_blocks[i].streaming ? 0 : snapshot["result_blocks"][i].value("id", 0ULL);
    left += width;
  }
  return 0;
}

void Window::PopupAIChoice(bool model) {
  const auto config = runtime.Configuration();
  std::vector<ui::OwnerMenuItem> items;
  if (model) {
    for (std::size_t i = 0; i < kChatGPTModels.size(); ++i)
      items.push_back({static_cast<UINT>(3100 + i), kChatGPTModels[i].title, config.chatgpt_model == kChatGPTModels[i].id});
  } else {
    for (std::size_t i = 0; i < kChatGPTEfforts.size(); ++i)
      items.push_back({static_cast<UINT>(3100 + i), Wide(std::string(kChatGPTEfforts[i])),
          config.chatgpt_effort == kChatGPTEfforts[i], false, ValidAIEffort(config.chatgpt_model, kChatGPTEfforts[i])});
  }
  const auto menu = BuildOwnerMenu(items);
  POINT cursor{}; GetCursorPos(&cursor);
  const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window, nullptr);
  DestroyMenu(menu);
  if (command < 3100 || static_cast<std::size_t>(command - 3100) >= (model ? kChatGPTModels.size() : kChatGPTEfforts.size())) return;
  auto selected_model = model ? std::string(kChatGPTModels[command - 3100].id) : config.chatgpt_model;
  auto selected_effort = model ? config.chatgpt_effort : std::string(kChatGPTEfforts[command - 3100]);
  if (!ValidAIEffort(selected_model, selected_effort)) selected_effort = "max";
  if (!runtime.SetChatGPTInference(selected_model, selected_effort)) MessageBeep(MB_OK);
}

void Window::PopupTrayMenu() {
  std::wstring startup_command;
  bool startup_enabled = false;
  broker::QueryBrokerAutostart(&startup_command, &startup_enabled);
  auto menu = BuildOwnerMenu(
      {{kToggle, L"打开 / 绑定 Buffer"},
       {kCapsule, L"Capsule 内容库    Alt+Shift+V"},
       {kMailbox, L"Mailbox 工作区    Alt+Shift+M"},
       {kSettings, L"设置"},
       {kDeploy, L"重新部署词库"},
       {kStartup, L"登录时启动", startup_enabled},
       {kAbout, L"版本与诊断"},
       {0, L"", false, true},
       {kExit, L"退出"}});
  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(window);
  const auto command =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0,
                     window, nullptr);
  DestroyMenu(menu);
  // Preserve the floating window role after the native tray-menu lifecycle.
  UpdateBufferZOrder();
  PostMessageW(window, WM_COMMAND, command, 0);
}

void Window::UpdateTooltip(int hit) {
  if (!tooltip) return;
  tooltip_text.clear();
  auto& text = tooltip_text;
  switch (static_cast<ui::BufferHitKind>(hit)) {
    case ui::BufferHitKind::kMode: {
      const auto config = runtime.Configuration();
      text = L"切换缓冲插件\n上一个：" + ShortcutTitle(config.previous_plugin_key, config.previous_plugin_modifiers) +
          L"\n下一个：" + ShortcutTitle(config.next_plugin_key, config.next_plugin_modifiers);
      break;
    }
    case ui::BufferHitKind::kReasoning: text = L"思考摘要\n" + Wide(runtime.Snapshot().value("reasoning_summary", std::string())); break;
    case ui::BufferHitKind::kAIModel: text = L"选择 ChatGPT 模型；仅影响新请求"; break;
    case ui::BufferHitKind::kAIEffort: text = L"选择推理深度；仅影响新请求"; break;
    case ui::BufferHitKind::kClose:
      text = L"关闭";
      break;
    case ui::BufferHitKind::kPaste:
      text = L"粘贴";
      break;
    case ui::BufferHitKind::kCopy:
      text = L"复制完整结果并关闭（Ctrl+C）";
      break;
    case ui::BufferHitKind::kSend: {
      const auto config = runtime.Configuration();
      const auto state = runtime.Snapshot();
      text = state.value("busy", false) ? L"正在处理…" :
          state.value("mode", 0) != 0 && state.value("result", std::string()).empty()
              ? (state.value("mode", 0) == 2 ? L"翻译原文" : L"生成结果") : L"发送下一块";
      text += L"（" + ShortcutTitle(config.send_key, config.send_modifiers) + L"；长按至进度条满，全部上屏）";
      break;
    }
    case ui::BufferHitKind::kEdit: text = L"返回编辑，放弃当前生成结果"; break;
    case ui::BufferHitKind::kAutoSend: text = L"自动发送：手动 / 1 / 2 / 3 / 5 秒"; break;
    case ui::BufferHitKind::kCloseAfter: text = L"发送最后一块后关闭 Buffer"; break;
    case ui::BufferHitKind::kSourceLanguage: text = L"原文语言"; break;
    case ui::BufferHitKind::kTargetLanguage: text = L"目标语言"; break;
    case ui::BufferHitKind::kSwapLanguages: text = L"交换原文语言和目标语言"; break;
    case ui::BufferHitKind::kSpeech:
      text = runtime.Configuration().translation_speech
          ? L"朗读已开启：点按译文块或发送时朗读该块；点按关闭"
          : L"朗读已关闭；点按开启";
      break;
    case ui::BufferHitKind::kBind:
      text = L"在当前输入框接收输入到 Buffer";
      break;
    default:
      break;
  }
  TOOLINFOW info{};
  info.cbSize = sizeof(info);
  info.hwnd = window;
  info.uId = 1;
  info.lpszText = text.data();
  SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
}

void Window::Copy() {
  auto ticket = runtime.PrepareCopy();
  if (ticket.is_null()) return;
  auto text = ticket.value("text", std::string());
  auto wide = Wide(text);
  if (!OpenClipboard(window)) return;
  HGLOBAL memory =
      GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
  if (memory) {
    auto* data = GlobalLock(memory);
    if (data) {
      memcpy(data, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
      GlobalUnlock(memory);
      if (!runtime.CommitCopy(ticket, [&] { return EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory); }))
        GlobalFree(memory);
    } else
      GlobalFree(memory);
  }
  CloseClipboard();
}

void Window::Paste() {
  if (!OpenClipboard(window)) return;
  auto memory = GetClipboardData(CF_UNICODETEXT);
  std::wstring text;
  if (memory) {
    const auto bytes = GlobalSize(memory);
    if (bytes <= Model::kLimit * 2) {
      auto* data = static_cast<const wchar_t*>(GlobalLock(memory));
      if (data) {
        const auto count = wcsnlen(data, bytes / sizeof(wchar_t));
        if (count < bytes / sizeof(wchar_t)) text.assign(data, count);
        GlobalUnlock(memory);
      }
    }
  }
  CloseClipboard();
  if (!text.empty()) runtime.Paste(Utf8(text));
}

void Window::Action(int index) {
  switch (index) {
    case 0:
      runtime.Toggle(ForegroundProcess());
      break;
    case 1:
      Paste();
      break;
    case 2:
      mode = ui::BufferMode::kGenerate;
      runtime.Generate(false);
      break;
    case 3:
      mode = ui::BufferMode::kTranslate;
      runtime.Generate(true);
      break;
    case 4:
      runtime.Cancel();
      break;
    case 5:
      runtime.Send(false);
      break;
    case 6:
      runtime.Send(true);
      break;
    case 7:
      Copy();
      break;
    case 8:
      OpenSettings();
      break;
    case 9:
      runtime.Close();
      break;
    default:
      break;
  }
}

void Window::ApplyHit(ui::BufferHitKind hit) {
  const auto state = MakePaintState(runtime.Snapshot());
  if ((hit == ui::BufferHitKind::kPaste && !state.paste_enabled) ||
      (hit == ui::BufferHitKind::kCopy && !state.copy_enabled) ||
      (hit == ui::BufferHitKind::kSend && !state.send_enabled))
    return;
  switch (hit) {
    case ui::BufferHitKind::kSourceLanguage:
    case ui::BufferHitKind::kTargetLanguage:
      if (state.translation_controls_enabled) PopupTranslationLanguage(hit == ui::BufferHitKind::kSourceLanguage);
      break;
    case ui::BufferHitKind::kSwapLanguages: {
      const auto config = runtime.Configuration();
      if (state.translation_controls_enabled)
        runtime.SetTranslationLanguages(config.target_language, config.source_language);
      break;
    }
    case ui::BufferHitKind::kSpeech:
      runtime.SetTranslationSpeech(!state.translation_speech);
      break;
    case ui::BufferHitKind::kEdit:
      runtime.ReturnToEdit();
      break;
    case ui::BufferHitKind::kAutoSend: {
      if (state.mode != ui::BufferMode::kInput) break;
      const auto seconds = state.auto_send_seconds;
      runtime.SetAutoSend(seconds == 0 ? 1 : seconds == 1 ? 2 : seconds == 2 ? 3 : seconds == 3 ? 5 : 0,
          state.close_after_send);
      break;
    }
    case ui::BufferHitKind::kCloseAfter:
      if (state.mode != ui::BufferMode::kInput) break;
      runtime.SetAutoSend(state.auto_send_seconds, !state.close_after_send);
      break;
    case ui::BufferHitKind::kBind:
      runtime.Bind(ForegroundProcess());
      break;
    case ui::BufferHitKind::kMode:
      PopupModeMenu();
      break;
    case ui::BufferHitKind::kAIModel: PopupAIChoice(true); break;
    case ui::BufferHitKind::kAIEffort: PopupAIChoice(false); break;
    case ui::BufferHitKind::kClose:
      runtime.Close();
      break;
    case ui::BufferHitKind::kPaste:
      Paste();
      break;
    case ui::BufferHitKind::kCopy:
      Copy();
      break;
    case ui::BufferHitKind::kSend:
      if (state.mode != ui::BufferMode::kInput && state.result_blocks.empty())
        runtime.Generate(state.mode == ui::BufferMode::kTranslate);
      else runtime.Send(false);
      break;
    default:
      break;
  }
}

void Window::PositionForOpening(const core::Json& state) {
  const auto revision = state.value("opening_revision", 0ULL);
  if (!runtime.Configuration().align_to_input_box) {
    placed_opening_revision = revision;
    placed_geometry_revision = state.value("placement_revision", 0ULL);
    return;
  }
  const auto target = runtime.PlacementTarget();
  auto anchor = ProbeBufferInputAnchor(target.process, runtime.PlacementCaret(target));
  if (target != runtime.PlacementTarget()) anchor = {};
  HMONITOR monitor = BufferFallbackMonitor();
  if (anchor.caret && FiniteBufferRect(*anchor.caret) &&
      anchor.caret->x >= LONG_MIN && anchor.caret->x <= LONG_MAX &&
      anchor.caret->y >= LONG_MIN && anchor.caret->y <= LONG_MAX) {
    const POINT point{static_cast<LONG>(anchor.caret->x),
                       static_cast<LONG>(anchor.caret->y)};
    const auto candidate = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
    if (candidate) monitor = candidate;
  }
  auto display = BufferMonitorInfo(monitor);
  auto work = DipFrame(display.work, display.scale);
  if (anchor.caret && !PlausibleBufferCaret(DipFrame(*anchor.caret, display.scale), work)) {
    anchor = {};
    display = BufferMonitorInfo(BufferFallbackMonitor());
    work = DipFrame(display.work, display.scale);
  }
  RECT current_pixels{};
  GetWindowRect(window, &current_pixels);
  const double old_scale = static_cast<double>(GetDpiForWindow(window)) / 96;
  auto current = DipFrame(current_pixels, old_scale > 0 ? old_scale : 1);
  auto paint = MakePaintState(state);
  current.height = WorkbenchLayout(paint, static_cast<float>(current.width)).height_dip;
  const auto placement = BufferOpeningPlacement(current,
      anchor.caret ? std::optional(DipFrame(*anchor.caret, display.scale)) : std::nullopt,
      anchor.box ? std::optional(DipFrame(*anchor.box, display.scale)) : std::nullopt,
      work);
  positioning = true;
  positioning_scale = display.scale;
  positioning_work = display.work;
  SetWindowPos(window, nullptr,
      static_cast<int>(std::lround(placement.frame.x * display.scale)),
      static_cast<int>(std::lround(placement.frame.y * display.scale)),
      static_cast<int>(std::lround(placement.frame.width * display.scale)),
      static_cast<int>(std::lround(placement.frame.height * display.scale)),
      SWP_NOZORDER | SWP_NOACTIVATE);
  positioning = false;
  opening_side = placement.side;
  placed_opening_revision = revision;
  placed_geometry_revision = state.value("placement_revision", 0ULL);
}

void Window::UpdateTargetIcon(DWORD process) {
  const auto dpi = GetDpiForWindow(window);
  if (target_pid == process && target_icon_dpi == dpi) return;
  target_pid = process; target_icon_dpi = dpi;
  if (target_icon) { DestroyIcon(target_icon); target_icon = nullptr; }
  if (!process) return;
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
  if (!handle) return;
  wchar_t path[32768]{}; DWORD length = static_cast<DWORD>(std::size(path));
  if (QueryFullProcessImageNameW(handle, 0, path, &length)) {
    SHFILEINFOW info{};
    const auto size = MulDiv(18, static_cast<int>(dpi), 96);
    SHDefExtractIconW(path, 0, 0, &target_icon, nullptr, MAKELONG(size, 0));
    if (!target_icon && SHGetFileInfoW(path, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON))
      target_icon = info.hIcon;
  }
  CloseHandle(handle);
}
void Window::PublishLogicalCaret(const ui::BufferPaintState& paint) {
  if (!paint.capturing || !last_layout.show_source || !format || !write) return;
  // Candidate placement follows the start of preedit, while the drawn caret
  // and editing scroll continue to use the actual position within preedit.
  const float x = last_layout.source_text.left - scroll[0] + ui::MeasureBufferCaret(write, format, paint, true);
  if (x < last_layout.source_text.left || x > last_layout.source_text.right) return;
  const float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
  POINT point{static_cast<LONG>(std::lround(x * scale)),
              static_cast<LONG>(std::lround((last_layout.source_rail.top + 6) * scale))};
  ClientToScreen(window, &point);
  RECT caret{point.x, point.y, point.x + 1,
      point.y + static_cast<LONG>(std::lround((last_layout.source_rail.height() - 12) * scale))};
  runtime.PublishCaret(caret);
}
void Window::Update() {
  if (runtime.Stopping()) {
    PostMessageW(window, WM_CLOSE, 0, 0);
    return;
  }
  // Keep unsaved settings theme preview while the settings host is open.
  if (!settings || !settings->IsOpen()) theme = ActiveTheme();
  auto state = runtime.Snapshot();
  const auto language = state.value("target_language", std::string());
  if (!state.value("speech_allowed", false) || !state.value("translate", false) ||
      !state.value("translation_speech", false) || language != speech_language) speech.Stop();
  speech_language = language;
  const auto request = runtime.TakeTranslationSpeech();
  if (request.is_object() && !speech.Speak(Wide(request.value("text", std::string())),
                                         request.value("language", language))) MessageBeep(MB_OK);
  // Only a validated focus lease reveals the rail. Background output or a
  // retained draft changing is not authority to expand into the active app.
  folded = state.value("folded", false);
  mode = static_cast<ui::BufferMode>(state.value("mode", 0));
  UpdateTargetIcon(state.value("target_pid", 0U));
  const auto config = runtime.Configuration();
  if (configured_width != config.buffer_width) {
    configured_width = config.buffer_width;
    RECT frame{}; GetWindowRect(window, &frame);
    const auto work = BufferMonitorWorkArea(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
    const auto scale = static_cast<double>(GetDpiForWindow(window)) / 96;
    const auto width = (std::min)(static_cast<int>(config.buffer_width * scale), static_cast<int>(work.right - work.left - 16));
    const auto left = (std::clamp)(static_cast<int>(frame.left), static_cast<int>(work.left), static_cast<int>(work.right) - width);
    SetWindowPos(window, nullptr, left, frame.top, width, frame.bottom-frame.top, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (send_hotkey) {
    if (state.value("capture", false) && state.value("visible", false) &&
        state.value("preedit", std::string()).empty() &&
        core::ValidShortcut(config.send_key, config.send_modifiers))
      send_hotkey->RegisterInitial(config.send_modifiers, config.send_key);
    else send_hotkey->Reset();
  }
  // Own the complete chord at the native dispatcher while Buffer has a live
  // lease. Older DLLs and host accelerators must not receive the arrow first.
  // In-progress composition stays on the existing TSF settle-and-cycle path.
  const bool plugin_shortcuts = state.value("visible", false) && state.value("capture", false) &&
      !state.value("pending", false) && state.value("preedit", std::string()).empty() &&
      state.value("target_pid", 0U) == ForegroundProcess();
  const unsigned plugin_keys[] = {config.previous_plugin_key, config.next_plugin_key};
  const unsigned plugin_modifiers[] = {config.previous_plugin_modifiers, config.next_plugin_modifiers};
  for (std::size_t i = 0; i < plugin_hotkeys.size(); ++i) {
    if (!plugin_hotkeys[i]) continue;
    const bool available = plugin_hotkeys[i]->Sync(plugin_shortcuts, plugin_modifiers[i], plugin_keys[i]);
    if (plugin_shortcuts && !available && !plugin_hotkey_warning[i]) {
      const auto notice = ShortcutTitle(plugin_keys[i], plugin_modifiers[i]) +
          L" 已被占用。请修改插件切换快捷键，或使用 Buffer 的插件按钮。";
      wcscpy_s(tray.szInfoTitle, L"插件切换快捷键未启用");
      wcscpy_s(tray.szInfo, notice.c_str());
      tray.dwInfoFlags = NIIF_WARNING; tray.uFlags |= NIF_INFO;
      Shell_NotifyIconW(NIM_MODIFY, &tray); tray.uFlags &= ~NIF_INFO;
    }
    plugin_hotkey_warning[i] = plugin_shortcuts && !available;
  }
  const auto clipboard_action = runtime.TakeClipboardAction();
  if (clipboard_action == 'V') { Paste(); return; }
  if (clipboard_action == 'C') { Copy(); return; }
  if (state.value("visible", false) &&
      (placed_opening_revision != state.value("opening_revision", 0ULL) ||
       placed_geometry_revision != state.value("placement_revision", 0ULL)))
    PositionForOpening(state);
  ShowWindow(window,
             state.value("visible", false) ? SW_SHOWNOACTIVATE : SW_HIDE);
  if (state.value("visible", false)) {
    RECT rect{};
    GetClientRect(window, &rect);
    const float dpi = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
    auto paint = MakePaintState(state);
    last_layout = WorkbenchLayout(paint, static_cast<float>(rect.right) / dpi);
    const float source_content = ui::MeasureBufferContent(
        write, format, paint.source_blocks, paint.preedit);
    const float result_content = ui::MeasureBufferContent(
        write, format, paint.result_blocks, paint.preview, false);
    scroll[0] = ui::ClampScroll(scroll[0], source_content,
                                last_layout.source_text.width());
    scroll[1] = ui::ClampScroll(scroll[1], result_content,
                                last_layout.result_text.width());
    const auto revision = state.value("revision", 0ULL);
    if (revision != shown_revision || paint.insertion != shown_insertion ||
        paint.preedit != shown_preedit || paint.preedit_caret != shown_preedit_caret) {
      if (last_layout.show_source) {
        const float caret = ui::MeasureBufferCaret(write, format, paint);
        const float viewport = last_layout.source_text.width();
        if (caret < scroll[0] + 12) scroll[0] = (std::max)(0.0f, caret - 12);
        if (caret > scroll[0] + viewport - 16) scroll[0] = (std::max)(0.0f, caret - viewport + 16);
      }
      shown_revision = revision; shown_insertion = paint.insertion;
      shown_preedit = paint.preedit; shown_preedit_caret = paint.preedit_caret;
    }
    if (last_layout.show_result && result_content != last_result_width &&
        scroll[1] >= (std::max)(0.0f, last_result_width - last_layout.result_text.width()) - 2)
      scroll[1] = (std::max)(0.0f, result_content - last_layout.result_text.width());
    last_result_width = result_content;
    RECT window_rect{};
    GetWindowRect(window, &window_rect);
    const int height =
        static_cast<int>(last_layout.height_dip * dpi + 0.5f);
    const int width = window_rect.right - window_rect.left;
    if (std::abs((window_rect.bottom - window_rect.top) - height) > 2) {
      const auto work = BufferMonitorWorkArea(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
      const auto frame = BufferResizedOutward(DipFrame(window_rect, dpi),
          static_cast<double>(height) / dpi, opening_side,
          DipFrame(work, dpi));
      SetWindowPos(window, nullptr, window_rect.left,
                   static_cast<int>(std::lround(frame.y * dpi)), width,
                   static_cast<int>(std::lround(frame.height * dpi)),
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  UpdateBufferZOrder();
  PublishLogicalCaret(MakePaintState(state));
  InvalidateRect(window, nullptr, FALSE);
}

void Window::Paint() {
  PAINTSTRUCT paint{};
  BeginPaint(window, &paint);
  RECT rect{};
  GetClientRect(window, &rect);
  if (!render) {
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    factory->CreateDCRenderTarget(&properties, &render);
    if (render) {
      const auto dpi = static_cast<float>(GetDpiForWindow(window));
      render->SetDpi(dpi, dpi);
      render->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush);
    }
  }
  if (render && brush && format && label_format && paint_frame.Resize(rect.right, rect.bottom) &&
      SUCCEEDED(render->BindDC(paint_frame.dc(), &rect))) {
    const auto dpi = static_cast<float>(GetDpiForWindow(window));
    const float width = static_cast<float>(rect.right) * 96.0f / dpi;
    auto state = runtime.Snapshot();
    UpdateTargetIcon(state.value("target_pid", 0U));
    auto paint_state = MakePaintState(state);
    last_layout = WorkbenchLayout(paint_state, width);
    render->SetDpi(dpi, dpi);
    render->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    render->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    render->BeginDraw();
    render->Clear(ui::ColorF(0, 0));
    ui::DrawBufferWorkbench(
        ui::BufferPaintContext{render, write, format, label_format, brush, dpi},
        paint_state, last_layout);
    const auto drawn = render->EndDraw();
    if (drawn == D2DERR_RECREATE_TARGET) {
      if (brush) brush->Release();
      brush = nullptr;
      render->Release();
      render = nullptr;
    }
    if (SUCCEEDED(drawn) && target_icon && paint_state.bound) {
      const auto box = last_layout.association;
      const float scale = dpi / 96.0f;
      const LONG x = static_cast<LONG>((box.left + 2) * scale), y = static_cast<LONG>((box.top + 2) * scale);
      const LONG size = static_cast<LONG>(18 * scale);
      paint_frame.DrawIcon(target_icon, {x, y, x + size, y + size});
    }
    if (SUCCEEDED(drawn)) paint_frame.Present(window);
  }
  EndPaint(window, &paint);
}

LRESULT CALLBACK Window::Procedure(HWND hwnd, UINT message, WPARAM wparam,
                                   LPARAM lparam) {
  auto* self =
      reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<Window*>(
        reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->window = hwnd;
  }
  if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
  try {
    if (self->taskbar_created && message == self->taskbar_created) {
      self->RefreshTrayIcon();
      self->AddTrayIcon();
      return 0;
    }
    switch (message) {
      case WM_SETTINGCHANGE:
      case WM_THEMECHANGED:
        self->runtime.AppearanceChanged();
        self->RefreshTrayIcon();
        return 0;
      case WM_NCCALCSIZE:
        // Keep resize semantics without a native frame subtracting pixels
        // from the 73/105-DIP workbench client area.
        return 0;
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
      case WM_PAINT:
        self->Paint();
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_HOTKEY:
        if (wparam == 22 && self->settings && self->settings->RecordingShortcut()) {
          self->settings->RecordShortcut(HIWORD(lparam), LOWORD(lparam) & ~MOD_NOREPEAT);
          return 0;
        }
        for (std::size_t i = 0; i < self->global_hotkeys.size(); ++i) {
          if (!self->global_hotkeys[i] || !self->global_hotkeys[i]->Matches(wparam, lparam)) continue;
          if (self->settings && self->settings->RecordingShortcut())
            self->settings->RecordShortcut(HIWORD(lparam), LOWORD(lparam) & ~MOD_NOREPEAT);
          else if (i == 0) {
            self->runtime.PauseCapture(); self->EnsureSettings(); self->settings->Toggle(hwnd); self->UpdateBufferZOrder();
          } else if (i == 1) self->OpenPeripheral(false, true);
          else if (self->peripherals) self->peripherals->CaptureArea();
          return 0;
        }
        if (wparam == 22) { self->OpenPeripheral(true, true); return 0; }
        for (const auto& hotkey : self->plugin_hotkeys) {
          if (!hotkey || !hotkey->Matches(wparam, lparam)) continue;
          const unsigned key = HIWORD(lparam), modifiers = LOWORD(lparam) & ~MOD_NOREPEAT;
          if (self->settings && self->settings->RecordingShortcut())
            self->settings->RecordShortcut(key, modifiers);
          else self->runtime.PluginShortcut(ForegroundProcess(), key, modifiers);
          return 0;
        }
        if (self->send_hotkey && self->send_hotkey->Matches(wparam, lparam)) {
          if (self->runtime.ShortcutDown(ForegroundProcess())) self->held_send_key = HIWORD(lparam);
          return 0;
        }
        if (self->buffer_hotkey && self->buffer_hotkey->Matches(wparam, lparam)) {
          if (self->settings && self->settings->RecordingShortcut())
            self->settings->RecordShortcut(HIWORD(lparam), LOWORD(lparam) & ~MOD_NOREPEAT);
          else self->runtime.Toggle(ForegroundProcess());
        }
        return 0;
      case kChanged:
        self->Update();
        return 0;
      case kOpenSettings:
        self->OpenSettings();
        return 0;
      case kMaintenanceExit:
        // Buffer is disposable IME UI. Stop transient provider work and the
        // input engine through the same shutdown path as the tray's Exit.
        DestroyWindow(hwnd);
        return 0;
      case kUpdateBufferZOrder:
        // Recheck the current lifetime: Settings may have reopened while this
        // close notification was queued, and shutdown must not restore it.
        self->UpdateBufferZOrder();
        return 0;
      case WM_TIMER:
        if (self->peripherals && GetTickCount64() >= self->peripheral_tick) {
          self->peripheral_tick = GetTickCount64() + 100;
          self->peripherals->Tick();
        }
        if (self->held_send_key && !(GetAsyncKeyState(static_cast<int>(self->held_send_key)) & 0x8000)) {
          self->held_send_key = 0; self->runtime.ShortcutUp();
        }
        {
          const auto state = self->runtime.Snapshot();
          self->runtime.ObserveForeground(ForegroundProcess());
          if (state.value("busy", false) || state.value("send_hold_progress", -1.0f) >= 0)
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        self->runtime.Tick();
        return 0;
      case WM_CLIPBOARDUPDATE:
        if (self->peripherals) { try { self->peripherals->ClipboardChanged(); } catch (...) {} }
        return 0;
      case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (measure->CtlType == ODT_MENU) {
          const auto found = self->menu_labels.find(measure->itemID);
          const std::wstring text =
              found == self->menu_labels.end() ? L"" : found->second;
          return ui::MeasureOwnerMenu(measure, GetDpiForWindow(hwnd), text,
                                      measure->itemID == 0 && text.empty());
        }
        break;
      }
      case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (draw->CtlType == ODT_MENU) {
          const auto found = self->menu_labels.find(draw->itemID);
          const std::wstring text =
              found == self->menu_labels.end() ? L"" : found->second;
          const bool separator = draw->itemID == 0 && text.empty();
          return ui::DrawOwnerMenu(draw, self->theme, text,
                                   (draw->itemState & ODS_CHECKED) != 0,
                                   separator, self->menu_font);
        }
        break;
      }
      case WM_DPICHANGED: {
        if (self->render)
          self->render->SetDpi(static_cast<float>(HIWORD(wparam)),
                               static_cast<float>(HIWORD(wparam)));
        auto* rect = reinterpret_cast<RECT*>(lparam);
        if (!self->positioning) SetWindowPos(hwnd, nullptr, rect->left, rect->top,
                     rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (self->menu_font) DeleteObject(self->menu_font);
        self->menu_font = CreateFontW(
            -MulDiv(12, HIWORD(wparam), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_ENTERSIZEMOVE:
        self->runtime.CancelOpeningPlacement();
        self->placed_geometry_revision = self->runtime.Snapshot().value("placement_revision", 0ULL);
        // Manual placement takes precedence until the next explicit opening.
        self->opening_side = BufferOpeningSide::kBottomFallback;
        return 0;
      case WM_MOUSEMOVE: {
        const float dpi = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
        const float x =
            static_cast<float>(static_cast<short>(LOWORD(lparam))) / dpi;
        const float y =
            static_cast<float>(static_cast<short>(HIWORD(lparam))) / dpi;
        const auto hit = ui::HitTestBuffer(self->last_layout, x, y);
        const int next = static_cast<int>(hit);
        if (next != self->hover) {
          self->hover = next;
          self->UpdateTooltip(next);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        return 0;
      }
      case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
          POINT cursor{};
          GetCursorPos(&cursor);
          ScreenToClient(hwnd, &cursor);
          const float dpi = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
          const auto hit = ui::HitTestBuffer(self->last_layout,
              static_cast<float>(cursor.x) / dpi,
              static_cast<float>(cursor.y) / dpi);
          SetCursor(LoadCursorW(nullptr, hit == ui::BufferHitKind::kBind
              ? IDC_IBEAM : hit == ui::BufferHitKind::kReasoning ? IDC_ARROW : hit == ui::BufferHitKind::kNone &&
                  !self->SpeechBlockAt(static_cast<float>(cursor.x) / dpi, static_cast<float>(cursor.y) / dpi)
                  ? IDC_ARROW : IDC_HAND));
          return TRUE;
        }
        break;
      case WM_MOUSELEAVE:
        if (self->hover >= 0 || self->pressed_hit != ui::BufferHitKind::kNone) {
          self->hover = -1;
          self->pressed_hit = ui::BufferHitKind::kNone;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      case WM_LBUTTONDOWN: {
        const float dpi = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
        const float x =
            static_cast<float>(static_cast<short>(LOWORD(lparam))) / dpi;
        const float y =
            static_cast<float>(static_cast<short>(HIWORD(lparam))) / dpi;
        self->pressed_hit = ui::HitTestBuffer(self->last_layout, x, y);
        self->pressed_speech_block = self->pressed_hit == ui::BufferHitKind::kNone ? self->SpeechBlockAt(x, y) : 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_LBUTTONUP: {
        const float dpi = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
        const float x =
            static_cast<float>(static_cast<short>(LOWORD(lparam))) / dpi;
        const float y =
            static_cast<float>(static_cast<short>(HIWORD(lparam))) / dpi;
        const auto hit = ui::HitTestBuffer(self->last_layout, x, y);
        const auto pressed = self->pressed_hit;
        const auto spoken = self->pressed_speech_block;
        self->pressed_speech_block = 0;
        self->pressed_hit = ui::BufferHitKind::kNone;
        InvalidateRect(hwnd, nullptr, FALSE);
        if (hit == ui::BufferHitKind::kBind && hit == pressed) {
          const auto state = self->MakePaintState(self->runtime.Snapshot());
          if (!state.capturing) self->runtime.Bind(ForegroundProcess());
          float at = self->last_layout.source_text.left - self->scroll[0];
          std::size_t index = 0;
          for (const auto& block : state.source_blocks) {
            const auto width = ui::MeasureBufferContent(self->write, self->format, {block}, L"");
            if (x < at + width * .5f) break;
            at += width; ++index;
          }
          self->runtime.SetInsertion(index);
        } else if (spoken && hit == ui::BufferHitKind::kNone && spoken == self->SpeechBlockAt(x, y))
          self->runtime.RequestTranslationSpeech(spoken);
        else if (hit != ui::BufferHitKind::kNone && hit == pressed)
          self->ApplyHit(hit);
        return 0;
      }
      case WM_MOUSEWHEEL: {
        POINT point{static_cast<short>(LOWORD(lparam)),
                    static_cast<short>(HIWORD(lparam))};
        ScreenToClient(hwnd, &point);
        const float dpi = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
        const float x = static_cast<float>(point.x) / dpi;
        const float y = static_cast<float>(point.y) / dpi;
        const int lane = self->last_layout.show_result &&
                                 self->last_layout.result_rail.contains(x, y)
                             ? 1
                             : 0;
        auto state = self->runtime.Snapshot();
        auto paint = self->MakePaintState(state);
        const float content =
            lane == 0 ? ui::MeasureBufferContent(self->write, self->format,
                                                 paint.source_blocks,
                                                 paint.preedit)
                      : ui::MeasureBufferContent(self->write, self->format,
                                                 paint.result_blocks,
                                                 paint.preview, false);
        const float visible = lane == 0 ? self->last_layout.source_text.width()
                                        : self->last_layout.result_text.width();
        self->scroll[lane] = ui::ClampScroll(
            self->scroll[lane] -
                static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) /
                    WHEEL_DELTA * 40.0f,
            content, visible);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        const int dpi = self->positioning
            ? static_cast<int>(std::lround(self->positioning_scale * 96))
            : static_cast<int>(GetDpiForWindow(hwnd));
        const auto work = self->positioning ? self->positioning_work
            : BufferMonitorWorkArea(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
        const int available = static_cast<int>(BufferSafeWorkArea(
            DipFrame(work, static_cast<double>(dpi) / 96)).width);
        limits->ptMinTrackSize = {MulDiv((std::min)(520, available), dpi, 96), MulDiv(35, dpi, 96)};
        limits->ptMaxTrackSize = {MulDiv(1100, dpi, 96), MulDiv(400, dpi, 96)};
        return 0;
      }
      case WM_MOVE:
        self->PublishLogicalCaret(self->MakePaintState(self->runtime.Snapshot()));
        break;
      case WM_SIZE: {
        if (self->tooltip) {
          TOOLINFOW info{};
          info.cbSize = sizeof(info);
          info.hwnd = hwnd;
          info.uId = 1;
          GetClientRect(hwnd, &info.rect);
          SendMessageW(self->tooltip, TTM_NEWTOOLRECTW, 0,
                       reinterpret_cast<LPARAM>(&info));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_NCHITTEST: {
        POINT point{static_cast<short>(LOWORD(lparam)),
                    static_cast<short>(HIWORD(lparam))};
        ScreenToClient(hwnd, &point);
        const float dpi = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
        const float x = static_cast<float>(point.x) / dpi;
        const float y = static_cast<float>(point.y) / dpi;
        RECT client{};
        GetClientRect(hwnd, &client);
        if (x < 5.0f) return HTLEFT;
        if (x >= static_cast<float>(client.right) / dpi - 5.0f) return HTRIGHT;
        if (ui::BufferHitIsCaptionExcluded(
                ui::HitTestBuffer(self->last_layout, x, y)))
          return HTCLIENT;
        if (self->last_layout.toolbar.contains(x, y) &&
            !self->last_layout.mode_chip.contains(x, y))
          return HTCAPTION;
        return HTCLIENT;
      }
      case kTray:
        if (lparam == WM_LBUTTONUP) {
          self->OpenSettings();
          return 0;
        }
        if (lparam == WM_RBUTTONUP) {
          self->PopupTrayMenu();
          return 0;
        }
        break;
      case WM_COMMAND:
        switch (LOWORD(wparam)) {
          case kToggle:
            self->runtime.Bind(ForegroundProcess());
            break;
          case kSettings:
            self->OpenSettings();
            break;
          case kCapsule: case kMailbox:
            self->OpenPeripheral(LOWORD(wparam) == kMailbox);
            break;
          case kDeploy:
            if (!self->deploying.exchange(true)) {
              self->runtime.DiscardBuffer();
              self->maintenance = std::jthread([self] {
                self->deploy();
                self->deploying.store(false);
              });
            }
            break;
          case kStartup: {
            broker::DefaultBrokerPaths paths;
            std::wstring error, command;
            bool enabled = false;
            if (broker::QueryBrokerAutostart(&command, &enabled, &error)) {
              if (enabled)
                broker::RemoveBrokerAutostart(&error);
              else if (broker::ResolveDefaultBrokerPaths(&paths, &error))
                broker::InstallBrokerAutostart(paths.broker_exe.wstring(),
                                               &error);
            }
            if (!error.empty())
              MessageBoxW(hwnd, error.c_str(), rimes::branding::Text(L"{brand}").c_str(), MB_OK);
            break;
          }
          case kAbout: {
            const auto about_text =
                std::wstring(rimes::branding::Text(L"{brand} Windows ").c_str()) + kProductVersionWide + L"\nCommit: " + Wide(RIMES_BUILD_COMMIT) +
                L"\n协议 v2\n词库：%APPDATA%\\RIMES\n设置与日志：%LOCALAPPDATA%"
                L"\\RIMES";
            MessageBoxW(hwnd, about_text.c_str(), L"版本与诊断", MB_OK);
            break;
          }
          case kExit:
            DestroyWindow(hwnd);
            break;
          default:
            break;
        }
        return 0;
      case WM_WTSSESSION_CHANGE:
        if (wparam == WTS_SESSION_LOCK || wparam == WTS_SESSION_LOGOFF ||
            wparam == WTS_REMOTE_DISCONNECT || wparam == WTS_CONSOLE_DISCONNECT) {
          self->runtime.Protect();
          if (self->peripherals) self->peripherals->Protect();
        } else if (wparam == WTS_SESSION_UNLOCK || wparam == WTS_CONSOLE_CONNECT ||
                   wparam == WTS_REMOTE_CONNECT) {
          if (self->peripherals) self->peripherals->Resume();
        }
        return 0;
      case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND) { self->runtime.Protect(); if (self->peripherals) self->peripherals->Protect(); }
        else if (wparam == PBT_APMRESUMESUSPEND || wparam == PBT_APMRESUMEAUTOMATIC) {
          if (self->peripherals) self->peripherals->Resume();
        }
        return TRUE;
      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY:
        self->destroying = true;
        if (self->commands) self->commands->Attach(nullptr);
        self->notification.store(nullptr);
        self->runtime.SetNotify({});
        if (self->buffer_hotkey) self->buffer_hotkey->Reset();
        if (self->send_hotkey) self->send_hotkey->Reset();
        for (const auto& hotkey : self->plugin_hotkeys) if (hotkey) hotkey->Reset();
        for (const auto& hotkey : self->global_hotkeys) if (hotkey) hotkey->Reset();
        UnregisterHotKey(hwnd, 22);
        RemoveClipboardFormatListener(hwnd);
        WTSUnRegisterSessionNotification(hwnd);
        Shell_NotifyIconW(NIM_DELETE, &self->tray);
        self->settings.reset();
        self->runtime.Stop();
        self->stop();
        PostQuitMessage(0);
        return 0;
      default:
        break;
    }
  } catch (...) {
    self->runtime.Close();
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
}  // namespace

void UiCommands::Attach(HWND window) {
  std::lock_guard lock(mutex_);
  window_ = window;
  if (!window) { retired_ = true; pending_settings_ = false; return; }
  retired_ = false;
  if (pending_settings_ && PostMessageW(window_, kOpenSettings, 0, 0))
    pending_settings_ = false;
}

bool UiCommands::RequestSettings() {
  std::lock_guard lock(mutex_);
  if (retired_) return false;
  if (!window_) { pending_settings_ = true; return true; }
  return PostMessageW(window_, kOpenSettings, 0, 0) != FALSE;
}

bool UiCommands::RequestExit() {
  std::lock_guard lock(mutex_);
  return window_ && PostMessageW(window_, kMaintenanceExit, 0, 0);
}

void RunWindow(Runtime& runtime, const std::function<void()>& stop,
               const std::function<void()>& deploy, UiCommands* commands,
               bool open_settings) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  struct Apartment {
    ~Apartment() { CoUninitialize(); }
  } apartment;
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
  InitCommonControlsEx(&icc);
  Window ui(runtime, stop, deploy, commands);
  if (FAILED(
          D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &ui.factory)) ||
      FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&ui.write)))) {
    runtime.Stop();
    stop();
    return;
  }
  ui.write->CreateTextFormat(
      L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"zh-CN",
      &ui.format);
  ui.write->CreateTextFormat(
      L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10, L"zh-CN",
      &ui.label_format);
  if (ui.format) ui.format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  if (ui.label_format)
    ui.label_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  WNDCLASSW wc{};
  wc.lpfnWndProc = Window::Procedure;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"Rimes.Workbench";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_RIMES));
  RegisterClassW(&wc);
  RECT area{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
  const int dpi = [] {
    HDC dc = GetDC(nullptr);
    int v = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return v ? v : 96;
  }();
  ui.menu_font =
      CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  HWND window = CreateWindowExW(
      WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, wc.lpszClassName,
      rimes::branding::Text(L"{brand} Buffer").c_str(), WS_POPUP | WS_THICKFRAME,
      area.left + ((area.right - area.left) - MulDiv(760, dpi, 96)) / 2,
      area.bottom - MulDiv(120 + 73, dpi, 96), MulDiv(760, dpi, 96),
      MulDiv(73, dpi, 96), nullptr, nullptr, wc.hInstance, &ui);
  if (!window) {
    runtime.Stop();
    stop();
    return;
  }
  ui.notification.store(window);
  runtime.SetNotify([&ui] {
    auto handle = ui.notification.load();
    if (handle) PostMessageW(handle, kChanged, 0, 0);
  });
  auto config = runtime.Configuration();
  ui.theme = ui::ThemeIdOrDefault(config.theme);
  ui.buffer_hotkey = std::make_unique<BufferHotkeyRegistration>(window);
  ui.send_hotkey = std::make_unique<BufferHotkeyRegistration>(window, 8);
  for (std::size_t i = 0; i < ui.plugin_hotkeys.size(); ++i)
    ui.plugin_hotkeys[i] = std::make_unique<BufferHotkeyRegistration>(window, 4 + static_cast<int>(i) * 2);
  ui.buffer_hotkey->RegisterInitial(config.hotkey_modifiers, config.hotkey_key);
  const unsigned global_keys[] = {config.settings_key, config.capsule_key, config.capture_key};
  const unsigned global_modifiers[] = {config.settings_modifiers, config.capsule_modifiers, config.capture_modifiers};
  const int global_offsets[] = {19, 23, 25};
  std::wstring global_conflicts;
  const wchar_t* global_names[] = {L"设置", L"Capsule", L"截图"};
  for (std::size_t i = 0; i < ui.global_hotkeys.size(); ++i) {
    ui.global_hotkeys[i] = std::make_unique<BufferHotkeyRegistration>(window, global_offsets[i]);
    if (!ui.global_hotkeys[i]->RegisterInitial(global_modifiers[i], global_keys[i])) {
      if (!global_conflicts.empty()) global_conflicts += L"、";
      global_conflicts += global_names[i];
    }
  }
  RegisterHotKey(window, 22, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'M');
  try { ui.peripherals = std::make_unique<peripherals::Windows>(runtime); AddClipboardFormatListener(window); } catch (...) {}
  ui.product_icon = ui::CreateTrayIcon(GetSystemMetrics(SM_CXSMICON));
  ui.tray.cbSize = sizeof(ui.tray);
  ui.tray.hWnd = window;
  ui.tray.uID = 1;
  ui.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  ui.tray.uCallbackMessage = kTray;
  ui.tray.hIcon =
      ui.product_icon ? ui.product_icon : LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(ui.tray.szTip, rimes::branding::Text(L"{brand} 输入法与 Buffer").c_str());
  if (!ui.buffer_hotkey->Registered())
    wcscpy_s(ui.tray.szTip, rimes::branding::Text(L"{brand} · Buffer 快捷键不可用，请使用托盘").c_str());
  ui.AddTrayIcon();
  if (!global_conflicts.empty()) {
    wcscpy_s(ui.tray.szInfoTitle, L"部分快捷键未启用");
    wcscpy_s(ui.tray.szInfo, (global_conflicts + L"快捷键被占用。可从托盘打开设置，在 Buffer › 窗口与全局快捷键中修改。").c_str());
    ui.tray.dwInfoFlags = NIIF_WARNING; ui.tray.uFlags |= NIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &ui.tray); ui.tray.uFlags &= ~NIF_INFO;
  }
  if (!ui.buffer_hotkey->Registered()) {
    const auto notice = BufferHotkeyTitle(config) +
        L" 未能注册。仍可从托盘打开 Buffer，在设置中选择其他组合或字母。";
    wcscpy_s(ui.tray.szInfoTitle, L"Buffer 快捷键未启用");
    wcscpy_s(ui.tray.szInfo, notice.c_str());
    ui.tray.dwInfoFlags = NIIF_WARNING;
    ui.tray.uFlags |= NIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &ui.tray);
    ui.tray.uFlags &= ~NIF_INFO;
    OutputDebugStringW(notice.c_str());
  }
  ui.tooltip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0,
                               0, window, nullptr, wc.hInstance, nullptr);
  if (ui.tooltip) {
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_SUBCLASS;
    info.hwnd = window;
    info.uId = 1;
    GetClientRect(window, &info.rect);
    info.lpszText = const_cast<wchar_t*>(L"");
    SendMessageW(ui.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
  }
  WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
  SetTimer(window, 1, 25, nullptr);
  if (commands) commands->Attach(window);
  if (open_settings) ui.OpenSettings();
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (ui.peripherals && ui.peripherals->HandleMessage(&message)) continue;
    if (ui.settings && ui.settings->HandleDialogMessage(&message)) continue;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}
}  // namespace rimes::windows::workbench
