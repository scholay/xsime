#include "ai_choices.hpp"
#include "provider.hpp"
#include "translation_languages.hpp"
#include "scheme_import.hpp"

#include <wincred.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "../core/control.hpp"
#include "buffer_hotkey_config.hpp"
#include "codex_cli.hpp"
#include "local_translation.hpp"
#include "sse.hpp"
#include "chord_profile.hpp"

namespace rimes::windows::workbench {
using core::Json;
namespace {
constexpr wchar_t kCredential[] = L"RIMES.Windows.OpenAI";
std::filesystem::path ConfigPath() {
  wchar_t value[32768]{};
  const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", value, 32768);
  if (!size || size >= 32768)
    throw std::runtime_error("LOCALAPPDATA unavailable");
  return std::filesystem::path(value) / L"RIMES" / L"settings.json";
}
std::wstring CredentialName(const std::string& identity) {
  return identity.empty() || identity == "legacy-api" ? std::wstring(kCredential)
      : std::wstring(kCredential) + L"." + Wide(identity);
}
std::wstring Secret(const std::string& endpoint, const std::string& identity) {
  PCREDENTIALW credential = nullptr;
  if (!CredReadW(CredentialName(identity).c_str(), CRED_TYPE_GENERIC, 0, &credential)) return {};
  if (!credential->UserName || Wide(endpoint) != credential->UserName) {
    CredFree(credential);
    return {};
  }
  std::wstring value(reinterpret_cast<wchar_t*>(credential->CredentialBlob),
                     credential->CredentialBlobSize / sizeof(wchar_t));
  CredFree(credential);
  return value;
}
struct Wipe {
  std::wstring& value;
  ~Wipe() {
    if (!value.empty())
      SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
  }
};
struct CloseInternet {
  void operator()(void* value) const {
    if (value) WinHttpCloseHandle(value);
  }
};
using Internet = std::unique_ptr<void, CloseInternet>;
void Fail(std::string* error, const char* code) {
  if (error) *error = code;
}
}  // namespace
std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                 static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) throw std::runtime_error("Invalid UTF-8");
  std::wstring result(static_cast<std::size_t>(size), 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size);
  return result;
}
std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                 static_cast<int>(text.size()), nullptr, 0,
                                 nullptr, nullptr);
  if (size <= 0) throw std::runtime_error("Invalid UTF-16");
  std::string result(static_cast<std::size_t>(size), 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}
void MigrateConnections(Settings* settings) {
  if (!settings->connections.empty()) return;
  settings->connections = {{"legacy-api", "AI API", "openai-compatible", settings->base_url, settings->model, ""},
                           {"codex", "Codex", "codex-cli", "", settings->codex_model, settings->codex_path}};
  settings->active_connection = settings->ai_connector == "codex-cli" ? "codex" : "legacy-api";
}
bool SelectConnection(Settings* settings, const std::string& id) {
  for (const auto& profile : settings->connections) {
    if (profile.id != id) continue;
    settings->active_connection = id;
    settings->ai_connector = profile.kind;
    if (profile.kind == "codex-cli") {
      settings->codex_path = profile.executable; settings->codex_model = profile.model;
    } else { settings->base_url = profile.base_url; settings->model = profile.model; }
    return true;
  }
  return false;
}
std::string CredentialIdentity(const Settings& settings) { return settings.active_connection; }
bool ValidConnections(const Settings& settings) {
  if (settings.connections.empty()) return settings.active_connection.empty();
  if (settings.connections.size() > 32) return false;
  std::vector<std::string> ids;
  bool active = false;
  for (const auto& p : settings.connections) {
    if (p.id.empty() || p.id.size() > 80 || p.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos ||
        std::find(ids.begin(), ids.end(), p.id) != ids.end() || p.name.empty() || p.name.size() > 160 ||
        (p.kind != "codex-cli" && p.kind != "openai-compatible") || p.base_url.size() > 2048 ||
        p.model.size() > 256 || p.executable.size() > 8192 || p.documentation_url.size() > 2048) return false;
    for (const auto* text : {&p.name, &p.base_url, &p.model, &p.executable, &p.documentation_url})
      if (text->find('\0') != std::string::npos || text->find('\n') != std::string::npos || text->find('\r') != std::string::npos) return false;
    ids.push_back(p.id); active |= settings.active_connection == p.id;
  }
  return active;
}
std::string EffectiveTheme(const Settings& settings) {
  if (!settings.follow_system_theme) return settings.theme;
  DWORD light = 0, size = sizeof(light);
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW)};
  if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
    const auto background = GetSysColor(COLOR_WINDOW);
    light = GetRValue(background) + GetGValue(background) + GetBValue(background) > 384;
  }
  return light ? "day" : "night";
}
bool ValidTheme(const std::string& theme) {
  return theme == "night" || theme == "day" || theme == "quiet" ||
         theme == "rasta";
}
bool ValidBufferShortcuts(const Settings& value) {
  const unsigned keys[] = {value.hotkey_key, value.send_key, value.previous_plugin_key, value.next_plugin_key,
      value.settings_key, value.capsule_key, value.capture_key};
  const unsigned modifiers[] = {value.hotkey_modifiers, value.send_modifiers,
      value.previous_plugin_modifiers, value.next_plugin_modifiers,
      value.settings_modifiers, value.capsule_modifiers, value.capture_modifiers};
  for (int i = 0; i < 7; ++i) {
    if (!core::ValidShortcut(keys[i], modifiers[i], i == 1)) return false;
    if (modifiers[i] == (MOD_ALT | MOD_SHIFT) && keys[i] == 'M') return false;
    for (int j = 0; j < i; ++j)
      if (keys[i] == keys[j] && modifiers[i] == modifiers[j]) return false;
  }
  return true;
}
bool ValidSettings(const Settings& value) {
  if (!value.input_options.is_object() || value.input_options.size() > 256 || value.input_options.dump().size() > 262144) return false;
  for (const auto& [scope, options] : value.input_options.items()) {
    if (scope.empty() || scope.size() > 256 || !options.is_object() || options.size() > 128) return false;
    for (const auto& [name, enabled] : options.items())
      if (name.empty() || name.size() > 96 || !enabled.is_boolean()) return false;
  }
  try { if (!value.chord_profile.is_null()) (void)ChordProfile::Parse(value.chord_profile); } catch (...) { return false; }
  const bool schema = rimes::schemes::ValidID(value.schema);
  return ValidConnections(value) && value.buffer_width >= 520 && value.buffer_width <= 1100 &&
         value.send_hold_ms >= 500 && value.send_hold_ms <= 3000 &&
         value.candidate_width >= 240 && value.candidate_width <= 1200 &&
         value.candidate_height >= 28 && value.candidate_height <= 72 &&
         value.candidate_label_size >= 8 && value.candidate_label_size <= 24 &&
         value.chord_interval_ms >= 10 && value.chord_interval_ms <= 300 &&
         (value.chord_profile.is_null() || value.chord_profile.dump().size() <= 2 * 1024 * 1024) && schema && value.candidate_count >= 1 && value.candidate_count <= 9 && ValidTheme(value.theme) && value.font_size >= 10 &&
         (value.auto_send_seconds == 0 || value.auto_send_seconds == 1 || value.auto_send_seconds == 2 ||
          value.auto_send_seconds == 3 || value.auto_send_seconds == 5) &&
         (value.translation_provider == "local" || value.translation_provider == "ai") &&
         value.font_size <= 40 &&
         ValidBufferShortcuts(value) &&
         value.base_url.size() <= 2048 && value.model.size() <= 256 &&
         !value.source_language.empty() && value.source_language.size() <= 128 &&
         !value.target_language.empty() && value.target_language.size() <= 128 &&
         TranslationLanguageID(value.source_language) != TranslationLanguageID(value.target_language) &&
         (value.ai_connector == "codex-cli" || value.ai_connector == "openai-compatible") &&
         value.codex_path.size() <= 8192 && value.codex_path.find('\0') == std::string::npos &&
         ValidAIModel(value.chatgpt_model) && ValidAIEffort(value.chatgpt_model, value.chatgpt_effort) &&
         value.codex_model.size() <= 256 && value.codex_model.find('\0') == std::string::npos;
}
bool LoadSettings(Settings* value, std::string* error) {
  try {
    auto path = ConfigPath();
    if (!std::filesystem::exists(path)) { value->ai_connector = "codex-cli"; MigrateConnections(value); return true; }
    if (std::filesystem::is_symlink(path) ||
        std::filesystem::file_size(path) > 3 * 1024 * 1024)
      throw std::runtime_error("Invalid settings file");
    std::ifstream file(path);
    auto j = Json::parse(file, [](int depth, Json::parse_event_t, Json&) {
      if (depth > 16) throw std::runtime_error("Settings nesting limit");
      return true;
    });
    // Do not expose partial or invalid values to startup hotkey registration.
    // A failed read retains the caller's previous trusted configuration.
    Settings candidate;
    candidate.revision = j.value("revision", 1ULL);
    candidate.schema = j.value("schema", "rime_ice");
    candidate.input_options = j.value("input_options", core::Json::object());
    candidate.base_url = j.value("base_url", "");
    candidate.model = j.value("model", "");
    candidate.ai_connector = j.value("ai_connector", "openai-compatible");
    candidate.codex_path = j.value("codex_path", "");
    candidate.codex_model = j.value("codex_model", "");
    candidate.chatgpt_model = j.value("chatgpt_model", candidate.codex_model.empty() ? "gpt-6-sol" : candidate.codex_model);
    candidate.chatgpt_effort = j.value("chatgpt_effort", "medium");
    candidate.active_connection = j.value("active_connection", "");
    candidate.gateway_enabled = j.value("gateway_enabled", true);
    if (j.contains("connections")) for (const auto& p : j.at("connections")) {
      candidate.connections.push_back({p.at("id"), p.at("name"), p.at("kind"), p.value("base_url", ""), p.value("model", ""), p.value("executable", ""), p.value("documentation_url", "")});
    }
    if (candidate.connections.empty()) MigrateConnections(&candidate);
    if (!SelectConnection(&candidate, candidate.active_connection)) throw std::runtime_error("Missing connection");
    candidate.buffer_width = j.value("buffer_width", 760U);
    candidate.send_hold_ms = j.value("send_hold_ms", 600U);
    candidate.follow_system_theme = j.value("follow_system_theme", false);
    candidate.align_to_input_box = j.value("align_to_input_box", true);
    candidate.reset_on_app_switch = j.value("reset_on_app_switch", false);
    candidate.candidate_width = j.value("candidate_width", 460U);
    candidate.candidate_height = j.value("candidate_height", 34U);
    candidate.candidate_label_size = j.value("candidate_label_size", 10U);
    // Missing fields in an existing profile retain the old effective default.
    // Only a new profile starts with the extension disabled.
    candidate.chord_enabled = j.value("chord_enabled", true);
    candidate.chord_interval_ms = j.value("chord_interval_ms", 50U);
    candidate.chord_profile = j.value("chord_profile", Json());
    candidate.target_language = j.value("target_language", "English");
    candidate.source_language = j.value("source_language", OppositeTranslationLanguage(candidate.target_language));
    candidate.translation_speech = j.value("translation_speech", false);
    candidate.translation_provider = j.value("translation_provider", "local");
    candidate.auto_send_seconds = j.value("auto_send_seconds", 0U);
    candidate.close_after_send = j.value("close_after_send", true);
    candidate.ascii = j.value("ascii", false);
    candidate.traditional = j.value("traditional", false);
    candidate.ascii_punctuation = j.value("ascii_punctuation", false);
    candidate.font_size = j.value("font_size", 16U);
    candidate.candidate_count = j.value("candidate_count", 9U);
    candidate.vertical_candidates = j.value("vertical_candidates", false);
    candidate.theme = j.value("theme", "night");
    if (!ValidTheme(candidate.theme)) candidate.theme = "night";
    // Preserve every explicitly saved legacy chord, even Ctrl+Alt+B: an old
    // default and a deliberate user choice cannot be distinguished. Early
    // files with only a letter also used Ctrl+Alt. New/absent shortcut fields
    // use Alt+Shift. Key-only v2 files retain their old Ctrl+Shift meaning.
    const unsigned default_modifiers =
        j.contains("hotkey_key") && j.value("version", 1U) < 2U
            ? MOD_CONTROL | MOD_ALT
            : j.contains("hotkey_key") && j.value("version", 1U) == 2U
                ? MOD_CONTROL | MOD_SHIFT : MOD_ALT | MOD_SHIFT;
    candidate.hotkey_modifiers = j.value("hotkey_modifiers", default_modifiers);
    candidate.hotkey_key = j.value("hotkey_key", static_cast<unsigned>('B'));
    candidate.send_modifiers = j.value("send_modifiers", unsigned(MOD_ALT | MOD_SHIFT));
    candidate.send_key = j.value("send_key", unsigned(VK_RETURN));
    // A pre-existing global custom binding wins over a newly added default.
    if (!j.contains("send_key") && !j.contains("send_modifiers") &&
        candidate.hotkey_key == candidate.send_key &&
        candidate.hotkey_modifiers == candidate.send_modifiers)
      candidate.send_modifiers = MOD_CONTROL | MOD_SHIFT;
    candidate.previous_plugin_key = j.value("previous_plugin_key", unsigned(VK_UP));
    candidate.previous_plugin_modifiers = j.value("previous_plugin_modifiers", unsigned(MOD_ALT | MOD_SHIFT));
    candidate.next_plugin_key = j.value("next_plugin_key", unsigned(VK_DOWN));
    candidate.next_plugin_modifiers = j.value("next_plugin_modifiers", unsigned(MOD_ALT | MOD_SHIFT));
    candidate.settings_key = j.value("settings_key", unsigned('S'));
    candidate.settings_modifiers = j.value("settings_modifiers", unsigned(MOD_ALT | MOD_SHIFT));
    candidate.capsule_key = j.value("capsule_key", unsigned('V'));
    candidate.capsule_modifiers = j.value("capsule_modifiers", unsigned(MOD_ALT | MOD_SHIFT));
    candidate.capture_key = j.value("capture_key", unsigned('4'));
    candidate.capture_modifiers = j.value("capture_modifiers", unsigned(MOD_ALT | MOD_SHIFT));
    // Existing explicit bindings win when adding a missing plugin shortcut.
    for (int i = 0; i < 2; ++i) {
      auto& key = i ? candidate.next_plugin_key : candidate.previous_plugin_key;
      auto& mods = i ? candidate.next_plugin_modifiers : candidate.previous_plugin_modifiers;
      if (j.contains(i ? "next_plugin_key" : "previous_plugin_key") ||
          j.contains(i ? "next_plugin_modifiers" : "previous_plugin_modifiers")) continue;
      const auto occupied = [&](unsigned m) {
        return (key == candidate.hotkey_key && m == candidate.hotkey_modifiers) ||
            (key == candidate.send_key && m == candidate.send_modifiers) ||
            (i && key == candidate.previous_plugin_key && m == candidate.previous_plugin_modifiers) ||
            (!i && key == candidate.next_plugin_key && m == candidate.next_plugin_modifiers);
      };
      if (occupied(mods)) for (const auto& choice : kBufferHotkeyChoices)
        if (!occupied(choice.modifiers)) { mods = choice.modifiers; break; }
    }
    // Newly editable defaults must never displace an existing custom binding.
    unsigned* global_keys[] = {&candidate.settings_key, &candidate.capsule_key, &candidate.capture_key};
    unsigned* global_modifiers[] = {&candidate.settings_modifiers, &candidate.capsule_modifiers, &candidate.capture_modifiers};
    const char* global_names[] = {"settings", "capsule", "capture"};
    for (int i = 0; i < 3; ++i) {
      if (j.contains(std::string(global_names[i]) + "_key") || j.contains(std::string(global_names[i]) + "_modifiers")) continue;
      const auto occupied = [&](unsigned mods) {
        const unsigned keys[] = {candidate.hotkey_key, candidate.send_key, candidate.previous_plugin_key, candidate.next_plugin_key};
        const unsigned modifiers[] = {candidate.hotkey_modifiers, candidate.send_modifiers, candidate.previous_plugin_modifiers, candidate.next_plugin_modifiers};
        for (int k = 0; k < 4; ++k) if (*global_keys[i] == keys[k] && mods == modifiers[k]) return true;
        for (int k = 0; k < 3; ++k) if (k != i && *global_keys[i] == *global_keys[k] && mods == *global_modifiers[k]) return true;
        return false;
      };
      if (occupied(*global_modifiers[i])) for (const auto& choice : kBufferHotkeyChoices)
        if (!occupied(choice.modifiers)) { *global_modifiers[i] = choice.modifiers; break; }
    }
    // Old versions incorrectly accepted Mailbox's fixed chord for Buffer or
    // send. Repair that invalid persisted overlap using the action's defined
    // default key, without displacing any other explicit binding. This is a
    // legacy-file migration, never a fallback for a runtime OS conflict.
    unsigned* keys[] = {&candidate.hotkey_key, &candidate.send_key, &candidate.previous_plugin_key,
        &candidate.next_plugin_key, &candidate.settings_key, &candidate.capsule_key, &candidate.capture_key};
    unsigned* modifiers[] = {&candidate.hotkey_modifiers, &candidate.send_modifiers, &candidate.previous_plugin_modifiers,
        &candidate.next_plugin_modifiers, &candidate.settings_modifiers, &candidate.capsule_modifiers, &candidate.capture_modifiers};
    for (int i = 0; i < 2; ++i) {
      if (*keys[i] != 'M' || *modifiers[i] != (MOD_ALT | MOD_SHIFT)) continue;
      *keys[i] = i == 0 ? unsigned('B') : unsigned(VK_RETURN);
      bool restored = false;
      for (const unsigned choice : {MOD_ALT | MOD_SHIFT, MOD_CONTROL | MOD_SHIFT, MOD_CONTROL | MOD_ALT}) {
        bool occupied = false;
        for (int other = 0; other < 7; ++other)
          if (other != i && *keys[other] == *keys[i] && *modifiers[other] == choice) occupied = true;
        if (!occupied) { *modifiers[i] = choice; restored = true; break; }
      }
      if (!restored) {
        Fail(error, i == 0 ? "旧 Buffer 快捷键与 Mailbox 冲突，B 键默认组合均已占用；原设置文件已保留。"
                            : "旧发送快捷键与 Mailbox 冲突，Enter 默认组合均已占用；原设置文件已保留。");
        return false;
      }
    }
    if (!ValidSettings(candidate))
      throw std::runtime_error("Invalid settings values");
    *value = std::move(candidate);
    return true;
  } catch (...) {
    Fail(error, "Settings unreadable; original file preserved.");
    return false;
  }
}
bool SaveSettings(const Settings& value, std::string* error) {
  try {
    if (!ValidSettings(value)) throw std::runtime_error("invalid settings");
    auto path = ConfigPath();
    std::filesystem::create_directories(path.parent_path());
    Json j = {{"version", 6},
              {"revision", value.revision},
              {"schema", value.schema},
              {"ascii", value.ascii},
              {"traditional", value.traditional},
              {"ascii_punctuation", value.ascii_punctuation},
              {"input_options", value.input_options},
              {"font_size", value.font_size},
              {"candidate_count", value.candidate_count},
              {"vertical_candidates", value.vertical_candidates},
              {"theme", value.theme},
              {"hotkey_modifiers", value.hotkey_modifiers},
              {"hotkey_key", value.hotkey_key},
              {"send_modifiers", value.send_modifiers},
              {"send_key", value.send_key},
              {"previous_plugin_key", value.previous_plugin_key},
              {"previous_plugin_modifiers", value.previous_plugin_modifiers},
              {"next_plugin_key", value.next_plugin_key},
              {"next_plugin_modifiers", value.next_plugin_modifiers},
              {"base_url", value.base_url},
              {"model", value.model},
              {"ai_connector", value.ai_connector},
              {"codex_path", value.codex_path},
              {"codex_model", value.codex_model},
              {"chatgpt_model", value.chatgpt_model}, {"chatgpt_effort", value.chatgpt_effort},
              {"target_language", value.target_language}};
    j["connections"] = Json::array();
    for (const auto& p : value.connections) j["connections"].push_back({{"id",p.id},{"name",p.name},{"kind",p.kind},{"base_url",p.base_url},{"model",p.model},{"executable",p.executable},{"documentation_url",p.documentation_url}});
    j["active_connection"] = value.active_connection;
    j["gateway_enabled"] = value.gateway_enabled;
    j["buffer_width"] = value.buffer_width; j["send_hold_ms"] = value.send_hold_ms;
    j["follow_system_theme"] = value.follow_system_theme;
    j["align_to_input_box"] = value.align_to_input_box;
    j["reset_on_app_switch"] = value.reset_on_app_switch;
    j["settings_key"] = value.settings_key; j["settings_modifiers"] = value.settings_modifiers;
    j["capsule_key"] = value.capsule_key; j["capsule_modifiers"] = value.capsule_modifiers;
    j["capture_key"] = value.capture_key; j["capture_modifiers"] = value.capture_modifiers;
    j["candidate_width"] = value.candidate_width; j["candidate_height"] = value.candidate_height;
    j["candidate_label_size"] = value.candidate_label_size;
    j["chord_enabled"] = value.chord_enabled; j["chord_interval_ms"] = value.chord_interval_ms;
    j["chord_profile"] = value.chord_profile;
    j["translation_provider"] = value.translation_provider;
    j["source_language"] = value.source_language;
    j["translation_speech"] = value.translation_speech;
    j["auto_send_seconds"] = value.auto_send_seconds;
    j["close_after_send"] = value.close_after_send;
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << j.dump(2);
    file.close();
    if (!file) throw std::runtime_error("write failed");
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("replace failed");
    return true;
  } catch (...) {
    Fail(error, "Could not save settings.");
    return false;
  }
}
bool SaveSecret(const std::wstring& key, const std::string& endpoint,
                std::string* error, const std::string& identity) {
  const auto credential_name = CredentialName(identity);
  if (key.empty()) {
    if (CredDeleteW(credential_name.c_str(), CRED_TYPE_GENERIC, 0) ||
        GetLastError() == ERROR_NOT_FOUND)
      return true;
  } else if (key.size() <= 2048 &&
             key.find_first_of(L"\r\n") == std::wstring::npos && key.find(L'\0') == std::wstring::npos) {
    auto endpoint_name = Wide(endpoint);
    CREDENTIALW credential{};
    credential.UserName = endpoint_name.data();
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(credential_name.c_str());
    credential.CredentialBlobSize =
        static_cast<DWORD>(key.size() * sizeof(wchar_t));
    credential.CredentialBlob =
        reinterpret_cast<BYTE*>(const_cast<wchar_t*>(key.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (CredWriteW(&credential, 0)) return true;
  }
  Fail(error, "Could not save API key in Windows Credential Manager.");
  return false;
}
bool HasSecret() {
  PCREDENTIALW value = nullptr;
  if (!CredReadW(kCredential, CRED_TYPE_GENERIC, 0, &value)) return false;
  CredFree(value);
  return true;
}
bool GenerateAPI(const Settings& config, const Generation& job,
                 const std::function<bool(const std::string&)>& chunk,
                 const std::function<bool()>& cancelled, std::string* error) {
  try {
    return GenerateWithKey(config, job, Secret(config.base_url, CredentialIdentity(config)), chunk,
                           cancelled, error);
  } catch (...) {
    Fail(error, "Credential unavailable.");
    return false;
  }
}
bool GenerateText(const Settings& config, const Generation& job,
                  const std::function<bool(const std::string&)>& chunk,
                  const std::function<bool()>& cancelled, std::string* error) {
  std::string translated;
  auto receive = [&](const std::string& text) {
    if (!job.translation) return chunk(text);
    if (cancelled() || translated.size() + text.size() > Model::kLimit) return false;
    translated += text; return true;
  };
  const bool ok = [&] {
    if (job.translation && config.translation_provider == "local")
      return GenerateLocalTranslation(config, job, receive, cancelled, error);
    if (config.ai_connector == "codex-cli")
      return GenerateCodex(config, job, receive, cancelled, error);
    if (config.ai_connector == "openai-compatible")
      return GenerateAPI(config, job, receive, cancelled, error);
    Fail(error, "Unknown AI connector. Source retained.");
    return false;
  }();
  if (!ok || !job.translation) return ok;
  const auto text = NormalizeTranslation(translated, job.source, config.target_language);
  return !cancelled() && !text.empty() && text.size() <= Model::kLimit && chunk(text);
}
bool GenerateWithKey(const Settings& config, const Generation& job,
                     std::wstring key,
                     const std::function<bool(const std::string&)>& chunk,
                     const std::function<bool()>& cancelled,
                     std::string* error) {
  try {
    Wipe wipe_key{key};
    if (key.empty() || config.model.empty()) {
      Fail(error, "Configure an API model and key first.");
      return false;
    }
    std::wstring url = Wide(config.base_url);
    while (!url.empty() && url.back() == L'/') url.pop_back();
    url += L"/chat/completions";
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength =
            static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0,
                         &parts)) {
      Fail(error, "Invalid API URL.");
      return false;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength),
        path(parts.lpszUrlPath, parts.dwUrlPathLength);
    const bool loopback = host == L"localhost" || host == L"127.0.0.1" ||
                          host == L"[::1]" || host == L"::1";
    if (parts.dwUserNameLength || parts.dwPasswordLength ||
        parts.dwExtraInfoLength ||
        (parts.nScheme != INTERNET_SCHEME_HTTPS &&
         !(parts.nScheme == INTERNET_SCHEME_HTTP && loopback))) {
      Fail(error, "Remote API endpoints require HTTPS and no URL credentials.");
      return false;
    }
    Internet session(
        WinHttpOpen(L"RIMES/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) throw std::runtime_error("session");
    WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 5000);
    Internet connection(
        WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) throw std::runtime_error("connect");
    Internet request(WinHttpOpenRequest(
        connection.get(), L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) throw std::runtime_error("request");
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects,
                     sizeof(redirects));
    auto headers =
        L"Content-Type: application/json\r\nAccept: "
        L"text/event-stream\r\nAuthorization: Bearer " +
        key + L"\r\n";
    Wipe wipe_headers{headers};
    if (job.instruction.empty() || job.instruction.size() > 32768) {
      Fail(error, "Plugin instruction unavailable."); return false;
    }
    const auto& instruction = job.instruction;
    Json body = {{"model", config.model},
                 {"stream", true},
                 {"messages",
                  Json::array({{{"role", "system"}, {"content", instruction}},
                               {{"role", "user"}, {"content", job.source}}})}};
    auto data = body.dump();
    if (cancelled()) return false;
    if (!WinHttpSendRequest(request.get(), headers.c_str(),
                            static_cast<DWORD>(headers.size()), data.data(),
                            static_cast<DWORD>(data.size()),
                            static_cast<DWORD>(data.size()), 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
      throw std::runtime_error("transport");
    SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(
        request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    wchar_t type[256]{};
    size = sizeof(type);
    if (status < 200 || status >= 300 ||
        !WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_TYPE,
                             WINHTTP_HEADER_NAME_BY_INDEX, type, &size,
                             WINHTTP_NO_HEADER_INDEX) ||
        std::wstring(type).find(L"text/event-stream") == std::wstring::npos) {
      Fail(error, "API must return a successful SSE response.");
      return false;
    }
    SseDecoder decoder(chunk, job.progress);
    const auto started = GetTickCount64();
    char buffer[8192];
    while (!cancelled() && GetTickCount64() - started < 120000) {
      DWORD received = 0;
      if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &received))
        throw std::runtime_error("read");
      if (!received) break;
      if (!decoder.Feed(std::string_view(buffer, received)))
        throw std::runtime_error("SSE");
      if (decoder.Done()) return !cancelled();
    }
    Fail(error,
         cancelled() ? "Cancelled." : "API stream ended before completion.");
    return false;
  } catch (...) {
    Fail(error, "API request failed or exceeded its limits. Source retained.");
    return false;
  }
}
}  // namespace rimes::windows::workbench
