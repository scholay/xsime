#pragma once
#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>

#include "model.hpp"
#include "../core/control.hpp"
#include <map>
namespace rimes::windows::workbench {
struct ConnectionProfile {
  std::string id, name, kind = "openai-compatible", base_url, model, executable;
  std::string documentation_url;
};
struct Settings {
  std::vector<ConnectionProfile> connections;
  std::string active_connection;
  unsigned buffer_width = 760, send_hold_ms = 600;
  bool follow_system_theme = false;
  bool align_to_input_box = true, reset_on_app_switch = false;
  bool gateway_enabled = true;
  unsigned candidate_width = 460, candidate_height = 34, candidate_label_size = 10;
  bool chord_enabled = false;
  unsigned chord_interval_ms = 50;
  core::Json chord_profile; // Applied portable keymap snapshot, never a draft.
  bool imported_scheme = false;  // Runtime-only; never persisted to settings.json.
  std::string input_scope = "builtin"; // Package identity, runtime-only.
  core::Json input_options = core::Json::object();
  std::map<std::string, bool> InputOptionsFor(const std::string& id) const {
    const auto scope = input_scope + ":" + id;
    std::map<std::string, bool> result;
    if (input_options.is_object() && input_options.contains(scope) && input_options.at(scope).is_object())
      for (const auto& [key, value] : input_options.at(scope).items()) if (value.is_boolean()) result[key] = value.get<bool>();
    return result;
  }
  std::uint64_t revision = 1;
  std::string schema = "rime_ice", base_url, model, target_language = "English";
  std::string ai_connector = "openai-compatible", codex_path, codex_model;
  std::string chatgpt_model = "gpt-6-sol", chatgpt_effort = "medium";
  std::string translation_provider = "local";
  std::string source_language = "Chinese";
  bool translation_speech = false;
  unsigned auto_send_seconds = 0;
  bool close_after_send = true;
  // Persisted appearance colorway: night|day|quiet|rasta. Default night.
  std::string theme = "night";
  bool ascii = false, traditional = false, ascii_punctuation = false;
  unsigned candidate_count = 9;
  bool vertical_candidates = false;
  unsigned font_size = 16, hotkey_modifiers = MOD_ALT | MOD_SHIFT,
           hotkey_key = 'B';
  unsigned send_modifiers = MOD_ALT | MOD_SHIFT, send_key = VK_RETURN;
  unsigned previous_plugin_modifiers = MOD_ALT | MOD_SHIFT, previous_plugin_key = VK_UP;
  unsigned next_plugin_modifiers = MOD_ALT | MOD_SHIFT, next_plugin_key = VK_DOWN;
  unsigned settings_modifiers = MOD_ALT | MOD_SHIFT, settings_key = 'S';
  unsigned capsule_modifiers = MOD_ALT | MOD_SHIFT, capsule_key = 'V';
  unsigned capture_modifiers = MOD_ALT | MOD_SHIFT, capture_key = '4';
};
void MigrateConnections(Settings* settings);
bool SelectConnection(Settings* settings, const std::string& id);
bool ValidConnections(const Settings& settings);
std::string EffectiveTheme(const Settings& settings);
std::string CredentialIdentity(const Settings& settings);
std::wstring Wide(const std::string& text);
std::string Utf8(const std::wstring& text);
bool LoadSettings(Settings* value, std::string* error);
bool ValidSettings(const Settings& value);
bool ValidBufferShortcuts(const Settings& value);
bool SaveSettings(const Settings& value, std::string* error);
bool SaveSecret(const std::wstring& key, const std::string& endpoint,
                std::string* error, const std::string& identity = "");
bool HasSecret();
bool SaveConnectionSecrets(const Settings&, const std::map<std::string, std::wstring>&, std::string*);
// Internal dependency seam used by the loopback transport tests; no UI supplies
// a key here.
bool GenerateWithKey(const Settings& config, const Generation& job,
                     std::wstring key,
                     const std::function<bool(const std::string&)>& chunk,
                     const std::function<bool()>& cancelled,
                     std::string* error);
bool GenerateAPI(const Settings& config, const Generation& job,
                 const std::function<bool(const std::string&)>& chunk,
                 const std::function<bool()>& cancelled, std::string* error);
// Common text-connector seam. Both transports obey the frozen Generation and
// cancellation contract; only Runtime may publish or deliver their output.
bool GenerateText(const Settings& config, const Generation& job,
                  const std::function<bool(const std::string&)>& chunk,
                  const std::function<bool()>& cancelled, std::string* error);
}  // namespace rimes::windows::workbench
