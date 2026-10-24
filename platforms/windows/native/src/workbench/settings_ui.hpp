#pragma once

#include <Windows.h>

#include <functional>
#include <string>

#include "../ui/settings_layout.hpp"
#include "../ui/settings_paint.hpp"
#include "provider.hpp"
#include "official_plugins.hpp"
#include "parity_settings.hpp"
#include "metrics_ui.hpp"
#include "../../../../../Shared/InputHabits/rime_options.hpp"

namespace rimes::windows::workbench {

struct SettingsUiCallbacks {
  std::function<std::vector<xsime::input::Switch>(const std::string&)> input_options;
  std::function<Settings()> load;
  std::function<bool(Settings, const std::wstring& key, bool replace_key,
                     std::string* error)>
      save;
  std::function<bool(Settings, const std::map<std::string, std::wstring>&, std::string*)> save_connections;
  // Notify the other surfaces after an appearance change is persisted.
  std::function<ui::ThemeId()> load_theme;
  std::function<void()> on_closed;
  std::function<void(ui::ThemeId)> on_theme_preview;
  std::wstring about_text;
  std::function<std::vector<PluginView>()> plugins;
  std::function<bool(const std::string&, const std::string&, std::string*)> manage_plugin;
  std::function<std::string()> plugin_status;
  std::function<bool()> open_schemes;
  std::function<void(HWND, ui::ThemeId)> open_lexicon;
  std::function<void()> open_capsule, open_mailbox;
  std::function<void()> move_buffer_to_screen;
  std::function<bool(std::string*)> restart_broker;
  std::function<bool(unsigned)> open_maintenance_location;
  std::function<bool()> load_clipboard_recording;
  std::function<bool(bool, std::string*)> save_clipboard_recording;
  std::shared_ptr<metrics::Service> metrics;
  // Optional fixture override; production opens these fixed destinations with the OS.
  std::function<bool(const std::wstring&)> open_brand_link;
  ConnectorUiServices connectors;
};

// Production and visual-preview share this host. Preview passes fixture
// callbacks that never touch Runtime, Credential Manager, or settings.json.
class SettingsUiHost {
 public:
  explicit SettingsUiHost(SettingsUiCallbacks callbacks);
  ~SettingsUiHost();

  void Open(HWND owner, bool visible = true);
  void Toggle(HWND owner);
  void OpenPage(HWND owner, ui::SettingsPage page, int subpage = 0, bool visible = true);
  void Close();
  [[nodiscard]] bool IsOpen() const noexcept { return hwnd_ != nullptr; }
  [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
  bool HandleDialogMessage(MSG* message);
  bool RecordingShortcut() const noexcept { return recording_shortcut_ >= 0; }
  void RecordShortcut(unsigned key, unsigned modifiers);

 private:
  void SyncDraftFromConfig(const Settings& config);
  void Relayout();
  void CreateOrUpdateChildren();
  void ApplyControlTheme();
  void ThemeEdits();
  void UpdatePlugins();
  bool CommitSave();
  void QueueSave(bool immediate);
  void SaveError(const std::wstring& message);
  void Paint(HDC dc);
  void PaintChildBackground(HWND child, HDC dc);
  void InvalidateHover(int hit);
  void ActivateHit(int hit);
  void SelectPage(ui::SettingsPage page);
  static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK ComboProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  static LRESULT CALLBACK ConnectorScrollProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

  std::shared_ptr<ui::SmoothShapesSession> smooth_shapes_ = ui::AcquireSmoothShapes();
  std::unique_ptr<ParitySettings> parity_;
  std::unique_ptr<ExtensionPages> extensions_;
  SettingsUiCallbacks callbacks_;
  ui::SettingsDraft draft_{};
  ui::SettingsLayout layout_{};
  ui::SettingsFonts fonts_{};
  ui::ThemeId opened_theme_ = ui::ThemeId::kNight;
  bool ready_ = false, updating_controls_ = false, pending_save_ = false;
  HICON settings_icon_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  HWND edit_font_ = nullptr;
  HWND edit_candidate_count_ = nullptr;
  HWND check_vertical_ = nullptr;
  HWND edit_hotkey_ = nullptr;
  HWND combo_hotkey_modifiers_ = nullptr;
  HWND edit_send_ = nullptr, combo_send_modifiers_ = nullptr;
  HWND record_hotkey_ = nullptr, record_send_ = nullptr, reset_shortcuts_ = nullptr, shortcut_feedback_ = nullptr;
  std::array<HWND, 2> edit_plugin_{}, combo_plugin_modifiers_{}, record_plugin_{};
  std::array<HWND, 3> edit_global_{}, combo_global_modifiers_{}, record_global_{};
  HWND check_align_ = nullptr, check_reset_app_ = nullptr, move_buffer_ = nullptr;
  std::array<HWND, 5> maintenance_actions_{};
  int recording_shortcut_ = -1;
  unsigned recorded_key_ = 0;
  void StopRecording();
  void OpenInputHabits();
  HWND input_habits_ = nullptr;
  HWND edit_base_ = nullptr;
  HWND combo_connector_ = nullptr, edit_codex_path_ = nullptr, edit_codex_model_ = nullptr;
  HWND edit_model_ = nullptr;
  HWND edit_key_ = nullptr;
  HWND edit_lang_ = nullptr;
  HWND edit_source_lang_ = nullptr;
  HWND combo_auto_send_ = nullptr, check_close_after_ = nullptr, combo_translation_ = nullptr;
  HWND schemes_button_ = nullptr, imported_label_ = nullptr;
  HWND lexicon_button_ = nullptr;
  HWND check_ascii_ = nullptr;
  HWND check_trad_ = nullptr;
  HWND check_punct_ = nullptr;
  std::array<HWND,4> plugin_labels_{}, plugin_install_{}, plugin_enable_{}, plugin_remove_{};
  HWND plugin_status_ = nullptr;
  HWND plugin_filter_ = nullptr, plugin_previous_ = nullptr,
       plugin_next_ = nullptr, plugin_page_ = nullptr;
  std::size_t plugin_offset_ = 0;
  HWND peripheral_open_ = nullptr;
  HWND clipboard_recording_ = nullptr;
  bool clipboard_recording_enabled_ = true;
  HWND home_action_ = nullptr;
  std::vector<PluginView> plugin_rows_;
  HBRUSH edit_brush_ = nullptr;
  unsigned dpi_ = 96;
  int pressed_hit_ = -2;
  std::array<int, ui::kSettingsPageCount> remembered_subpages_{};
};

}  // namespace rimes::windows::workbench
