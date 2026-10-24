#include "provider.hpp"
#include "buffer_hotkey.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace rimes::windows::workbench;
namespace {
void Check(bool ok, const char* reason) {
  if (!ok) throw std::runtime_error(reason);
}
bool SameSettings(const Settings& a, const Settings& b) {
  return a.input_options == b.input_options && a.revision == b.revision && a.schema == b.schema && a.send_hold_ms == b.send_hold_ms &&
         a.base_url == b.base_url && a.model == b.model && a.ai_connector == b.ai_connector &&
         a.codex_path == b.codex_path && a.codex_model == b.codex_model &&
         a.chatgpt_model == b.chatgpt_model && a.chatgpt_effort == b.chatgpt_effort &&
         a.target_language == b.target_language && a.source_language == b.source_language &&
         a.translation_speech == b.translation_speech && a.theme == b.theme &&
         a.ascii == b.ascii && a.traditional == b.traditional &&
         a.ascii_punctuation == b.ascii_punctuation &&
         a.font_size == b.font_size && a.candidate_count == b.candidate_count &&
         a.vertical_candidates == b.vertical_candidates &&
         a.hotkey_modifiers == b.hotkey_modifiers && a.hotkey_key == b.hotkey_key &&
         a.send_key == b.send_key && a.send_modifiers == b.send_modifiers &&
         a.previous_plugin_key == b.previous_plugin_key && a.previous_plugin_modifiers == b.previous_plugin_modifiers &&
         a.next_plugin_key == b.next_plugin_key && a.next_plugin_modifiers == b.next_plugin_modifiers &&
         a.settings_key == b.settings_key && a.settings_modifiers == b.settings_modifiers &&
         a.capsule_key == b.capsule_key && a.capsule_modifiers == b.capsule_modifiers &&
         a.capture_key == b.capture_key && a.capture_modifiers == b.capture_modifiers &&
         a.align_to_input_box == b.align_to_input_box && a.reset_on_app_switch == b.reset_on_app_switch &&
         a.chord_enabled == b.chord_enabled &&
         a.auto_send_seconds == b.auto_send_seconds && a.close_after_send == b.close_after_send &&
         a.translation_provider == b.translation_provider;
}

// Exercise the production load/save functions without reading real settings,
// touching credentials, or retaining API keys in any test artifact.
struct IsolatedSettings {
  std::filesystem::path root;
  std::wstring previous;
  bool had_previous = false;
  IsolatedSettings() {
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size) {
      previous.resize(size);
      GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), size);
      previous.resize(wcslen(previous.c_str()));
      had_previous = true;
    }
    root = std::filesystem::temp_directory_path() /
        (L"rimes-settings-test-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    Check(!std::filesystem::exists(root), "test directory must be new");
    std::filesystem::create_directories(root / L"RIMES");
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str()) != FALSE,
          "isolate LOCALAPPDATA");
  }
  ~IsolatedSettings() {
    SetEnvironmentVariableW(L"LOCALAPPDATA",
                             had_previous ? previous.c_str() : nullptr);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  std::filesystem::path path() const { return root / L"RIMES/settings.json"; }
};
}  // namespace

int main() {
  try {
    // No native registrations: a conflicting startup chord is represented by
    // an unregistered owner, and an unrelated save must not try to claim it.
    BufferHotkeyRegistration unavailable(nullptr);
    std::array<GlobalHotkeyBinding, 1> unchanged{{{&unavailable, MOD_ALT | MOD_SHIFT, 'V', false}}};
    int unrelated_saves = 0;
    Check(UpdateGlobalHotkeys(unchanged, [&] { ++unrelated_saves; return true; }) == HotkeyUpdate::kSaved &&
        unrelated_saves == 1 && !unavailable.Registered(), "unavailable unchanged shortcut does not block unrelated settings save");
    Check(UpdateGlobalHotkeys(unchanged, [] { return false; }) == HotkeyUpdate::kSaveFailed && !unavailable.Registered(),
        "unrelated settings save still reports its own persistence failure");
    std::array<GlobalHotkeyBinding, 2> changed{{unchanged[0], {&unavailable, 0, 'B', true}}};
    std::size_t failed_index = 0;
    Check(UpdateGlobalHotkeys(changed, [&] { ++unrelated_saves; return true; }, &failed_index) == HotkeyUpdate::kInvalid &&
        failed_index == 1 && unrelated_saves == 1 && !unavailable.Registered(),
        "explicit changed shortcut is still validated and prevents saving on failure");
    IsolatedSettings test;
    Settings settings;
    Check(LoadSettings(&settings, nullptr) && settings.theme == "night" && settings.ai_connector == "codex-cli" &&
              settings.hotkey_modifiers == (MOD_ALT | MOD_SHIFT) &&
              settings.hotkey_key == 'B' && settings.send_key == VK_RETURN &&
              settings.send_modifiers == (MOD_ALT | MOD_SHIFT),
          "absent file uses Alt+Shift+B and Alt+Shift+Enter");
    Check(!settings.chord_enabled && settings.align_to_input_box && !settings.reset_on_app_switch,
          "fresh profile disables chord and preserves Buffer across apps with alignment enabled");
    Check(settings.settings_key == 'S' && settings.capsule_key == 'V' && settings.capture_key == '4', "fresh global shortcut defaults");
    for (const auto* theme : {"night", "day", "quiet", "rasta"}) {
      settings.theme = theme;
      settings.connections.front().model = "fixture-model";
      Check(SelectConnection(&settings, "legacy-api"), "select saved API profile");
      Check(SaveSettings(settings, nullptr), "save theme");
      Settings restored;
      Check(LoadSettings(&restored, nullptr) && restored.theme == theme &&
                restored.model == settings.model,
            "round trip retains theme and unrelated settings");
    }
    Check(settings.chatgpt_model == "gpt-6-sol" && settings.chatgpt_effort == "medium", "ChatGPT defaults match macOS");
    settings.input_options["builtin:wubi86"] = {{"rimes_wubi_manual_commit", true}};
    settings.input_options["fixture:wubi86"] = {{"rimes_wubi_fifth_code", false}};
    Check(settings.InputOptionsFor("wubi86").at("rimes_wubi_manual_commit"), "built-in scheme override");
    settings.input_scope = "fixture";
    Check(settings.InputOptionsFor("wubi86").size() == 1 && !settings.InputOptionsFor("wubi86").at("rimes_wubi_fifth_code"), "same schema in another package is isolated");
    settings.input_scope = "builtin";
    settings.chatgpt_model = "gpt-6-astra"; settings.chatgpt_effort = "high";
    settings.candidate_count = 5;
    settings.auto_send_seconds = 3; settings.close_after_send = true; settings.translation_provider = "ai";
    settings.source_language = "Japanese"; settings.translation_speech = true;
    settings.connections[1].executable = "C:/Program Files/Codex/codex.exe";
    settings.connections[1].model = "fixture-model";
    Check(SelectConnection(&settings, "codex"), "select saved Codex profile");
    Check(SaveSettings(settings, nullptr), "save Codex connector without API credentials");
    Settings codex;
    Check(LoadSettings(&codex, nullptr) && SameSettings(codex, settings), "Codex connector round trip");
    settings.ai_connector = "unknown";
    Check(!SaveSettings(settings, nullptr), "reject unknown connector");
    settings.ai_connector = "codex-cli";
    settings.vertical_candidates = true;
    Check(SaveSettings(settings, nullptr), "save candidate options");
    Settings candidate_options;
    Check(LoadSettings(&candidate_options, nullptr) && candidate_options.candidate_count == 5 &&
          candidate_options.vertical_candidates, "candidate options persist");
    for (unsigned invalid : {0U, 10U}) {
      settings.candidate_count = invalid;
      Check(!SaveSettings(settings, nullptr), "reject invalid candidate counts");
    }
    settings.candidate_count = 5;
    settings.theme = "neon";
    Check(!SaveSettings(settings, nullptr), "reject unknown theme on save");
    Settings retained;
    Check(LoadSettings(&retained, nullptr) && retained.theme == "rasta",
          "failed save preserves previous file");
    for (const auto* json : {"{}", "{\"theme\":\"unknown\"}"}) {
      std::ofstream(test.path(), std::ios::trunc) << json;
      Settings legacy;
      Check(LoadSettings(&legacy, nullptr) && legacy.theme == "night" && legacy.candidate_count == 9 && !legacy.vertical_candidates &&
                legacy.chord_enabled &&
                legacy.hotkey_modifiers == (MOD_ALT | MOD_SHIFT) &&
                legacy.hotkey_key == 'B' && legacy.send_hold_ms == 600,
            "legacy and unknown themes fall back safely");
    }
    Check(Settings{}.send_hold_ms == 600, "new Buffer hold default matches macOS 0.6 seconds");
    for (const bool enabled : {false, true}) {
      std::ofstream(test.path(), std::ios::trunc) << "{\"chord_enabled\":" << (enabled ? "true" : "false") << "}";
      Settings saved;
      Check(LoadSettings(&saved, nullptr) && saved.chord_enabled == enabled && SaveSettings(saved, nullptr), "explicit chord choice is retained");
    }
    std::ofstream(test.path(), std::ios::trunc) << "{\"hotkey_key\":83,\"hotkey_modifiers\":5}";
    Settings legacy_settings_key;
    Check(LoadSettings(&legacy_settings_key, nullptr) && legacy_settings_key.hotkey_key == 'S' && legacy_settings_key.hotkey_modifiers == 5 &&
        legacy_settings_key.settings_key == 'S' && legacy_settings_key.settings_modifiers != 5,
        "new editable settings default yields to a legacy Buffer binding");
    auto global_roundtrip = legacy_settings_key;
    global_roundtrip.settings_key = VK_F9; global_roundtrip.settings_modifiers = MOD_CONTROL | MOD_SHIFT;
    global_roundtrip.capsule_key = VK_F10; global_roundtrip.capture_key = VK_F11;
    global_roundtrip.align_to_input_box = false; global_roundtrip.reset_on_app_switch = true;
    Check(SaveSettings(global_roundtrip, nullptr), "save global shortcuts and window preferences");
    Settings global_loaded;
    Check(LoadSettings(&global_loaded, nullptr) && SameSettings(global_roundtrip, global_loaded), "global shortcuts and window preferences survive restart");
    global_roundtrip.capture_key = global_roundtrip.capsule_key;
    Check(!SaveSettings(global_roundtrip, nullptr), "global shortcuts cannot collide");
    global_roundtrip = global_loaded; global_roundtrip.settings_key = 'M'; global_roundtrip.settings_modifiers = 5;
    Check(!SaveSettings(global_roundtrip, nullptr), "Mailbox chord stays reserved");
    for (int action = 0; action < 7; ++action) {
      auto reserved = global_loaded;
      unsigned* keys[] = {&reserved.hotkey_key, &reserved.send_key, &reserved.previous_plugin_key, &reserved.next_plugin_key,
          &reserved.settings_key, &reserved.capsule_key, &reserved.capture_key};
      unsigned* modifiers[] = {&reserved.hotkey_modifiers, &reserved.send_modifiers, &reserved.previous_plugin_modifiers,
          &reserved.next_plugin_modifiers, &reserved.settings_modifiers, &reserved.capsule_modifiers, &reserved.capture_modifiers};
      *keys[action] = 'M'; *modifiers[action] = MOD_ALT | MOD_SHIFT;
      Check(!SaveSettings(reserved, nullptr), "every configurable action rejects the reserved Mailbox chord");
    }
    for (const bool send : {false, true}) {
      const auto json = std::string("{\"theme\":\"quiet\",\"font_size\":21,\"") +
          (send ? "send" : "hotkey") + "_key\":77,\"" + (send ? "send" : "hotkey") + "_modifiers\":5}";
      std::ofstream(test.path(), std::ios::trunc) << json;
      Settings repaired;
      Check(LoadSettings(&repaired, nullptr) && repaired.theme == "quiet" && repaired.font_size == 21 &&
          repaired.hotkey_key == 'B' && repaired.hotkey_modifiers == 5 && repaired.send_key == VK_RETURN && repaired.send_modifiers == 5,
          "legacy Mailbox overlap restores only the invalid action to its defined default");
      std::ifstream original(test.path());
      Check(std::string(std::istreambuf_iterator<char>(original), std::istreambuf_iterator<char>()) == json, "legacy overlap load never rewrites the source file");
      original.close();
      Check(SaveSettings(repaired, nullptr), "explicit next save persists the repaired legacy default");
    }
    std::ofstream(test.path(), std::ios::trunc) << "{\"hotkey_key\":77,\"hotkey_modifiers\":5,\"settings_key\":66,\"settings_modifiers\":5}";
    Settings overlapping_default;
    Check(LoadSettings(&overlapping_default, nullptr) && overlapping_default.hotkey_key == 'B' && overlapping_default.hotkey_modifiers == 6 &&
        overlapping_default.settings_key == 'B' && overlapping_default.settings_modifiers == 5,
        "legacy repair yields to an explicit custom binding occupying its default");
    const std::string exhausted = "{\"hotkey_key\":77,\"hotkey_modifiers\":5,\"settings_key\":66,\"settings_modifiers\":5,\"capsule_key\":66,\"capsule_modifiers\":6,\"capture_key\":66,\"capture_modifiers\":3}";
    std::ofstream(test.path(), std::ios::trunc) << exhausted;
    auto retained_overlap = global_loaded;
    std::string overlap_error;
    Check(!LoadSettings(&retained_overlap, &overlap_error) && SameSettings(retained_overlap, global_loaded) &&
        overlap_error.find("Buffer") != std::string::npos && overlap_error.find("Mailbox") != std::string::npos,
        "exhausted legacy defaults report the affected action and preserve the trusted configuration");
    {
      std::ifstream original(test.path());
      Check(std::string(std::istreambuf_iterator<char>(original), std::istreambuf_iterator<char>()) == exhausted, "exhausted legacy defaults preserve the original file verbatim");
    }
    for (unsigned hold : {600U, 1200U, 2300U}) {
      std::ofstream(test.path(), std::ios::trunc) << "{\"send_hold_ms\":" << hold << "}";
      Settings saved;
      Check(LoadSettings(&saved, nullptr) && saved.send_hold_ms == hold && SaveSettings(saved, nullptr),
            "explicit old or custom hold duration is preserved");
      Settings reloaded;
      Check(LoadSettings(&reloaded, nullptr) && reloaded.send_hold_ms == hold, "hold duration survives save and reload");
    }
    for (const unsigned modifiers : {MOD_CONTROL | MOD_ALT,
                                     MOD_CONTROL | MOD_SHIFT,
                                     MOD_ALT | MOD_SHIFT}) {
      for (const unsigned key : {'B', 'J'}) {
        std::ofstream(test.path(), std::ios::trunc)
            << "{\"version\":1,\"hotkey_modifiers\":" << modifiers
            << ",\"hotkey_key\":" << key << ",\"model\":\"retained-model\"}";
        Settings legacy;
        Check(LoadSettings(&legacy, nullptr) &&
                  legacy.hotkey_modifiers == modifiers && legacy.hotkey_key == key &&
                  legacy.model == "retained-model",
              "explicit legacy and new chords are never silently reassigned");
        Check(SaveSettings(legacy, nullptr), "save a preserved legacy chord");
        Settings restored;
        Check(LoadSettings(&restored, nullptr) &&
                  restored.hotkey_modifiers == modifiers && restored.hotkey_key == key &&
                  restored.model == "retained-model",
              "saving another preference retains the complete chord");
      }
    }
    for (const char* json : {"{\"hotkey_key\":74}",
                             "{\"version\":1,\"hotkey_key\":74}"}) {
      std::ofstream(test.path(), std::ios::trunc) << json;
      Settings legacy;
      Check(LoadSettings(&legacy, nullptr) &&
                legacy.hotkey_modifiers == (MOD_CONTROL | MOD_ALT) &&
                legacy.hotkey_key == 'J',
            "early key-only files retain their Ctrl+Alt shortcut");
    }
    std::ofstream(test.path(), std::ios::trunc)
        << "{\"version\":2,\"hotkey_key\":74}";
    Settings modern;
    Check(LoadSettings(&modern, nullptr) &&
              modern.hotkey_modifiers == (MOD_CONTROL | MOD_SHIFT) &&
              modern.hotkey_key == 'J',
          "modern key-only files use Ctrl+Shift");
    modern.hotkey_modifiers = MOD_CONTROL | MOD_ALT;
    Check(SaveSettings(modern, nullptr), "retain legacy before explicit migration");
    modern.hotkey_modifiers = MOD_CONTROL | MOD_SHIFT;
    Check(SaveSettings(modern, nullptr), "user explicitly chooses Ctrl+Shift");
    Settings migrated;
    Check(LoadSettings(&migrated, nullptr) &&
              migrated.hotkey_modifiers == (MOD_CONTROL | MOD_SHIFT) &&
              migrated.hotkey_key == 'J',
          "explicit migration preserves the custom letter");
    const unsigned invalid_modifiers[] = {
        0U, MOD_SHIFT, MOD_NOREPEAT, 16U, 0xffffffffU};
    for (unsigned modifiers : invalid_modifiers) {
      migrated.hotkey_modifiers = modifiers;
      Check(!SaveSettings(migrated, nullptr), "unsupported modifier sets are rejected");
    }
    Settings after_rejection;
    Check(LoadSettings(&after_rejection, nullptr) &&
              after_rejection.hotkey_modifiers == (MOD_CONTROL | MOD_SHIFT) &&
              after_rejection.hotkey_key == 'J',
          "rejected modifier changes preserve the saved shortcut");
    std::ofstream(test.path(), std::ios::trunc) << "{\"version\":5,\"hotkey_key\":38,\"hotkey_modifiers\":5,\"send_key\":40,\"send_modifiers\":5}";
    Settings migrated_cycle;
    Check(LoadSettings(&migrated_cycle, nullptr) && migrated_cycle.hotkey_key == VK_UP && migrated_cycle.send_key == VK_DOWN &&
        migrated_cycle.hotkey_modifiers == 5 && migrated_cycle.send_modifiers == 5 &&
        migrated_cycle.previous_plugin_modifiers != 5 && migrated_cycle.next_plugin_modifiers != 5,
        "adding missing cycle shortcuts preserves explicit legacy arrow bindings");
    auto duplicate = migrated_cycle;
    duplicate.next_plugin_key = duplicate.previous_plugin_key; duplicate.next_plugin_modifiers = duplicate.previous_plugin_modifiers;
    Check(!SaveSettings(duplicate, nullptr), "duplicate cycle shortcuts rejected");
    duplicate = migrated_cycle; duplicate.next_plugin_key = 'S'; duplicate.next_plugin_modifiers = 5;
    Check(!SaveSettings(duplicate, nullptr), "reserved settings shortcut rejected");
    duplicate.next_plugin_key = '4';
    Check(!SaveSettings(duplicate, nullptr), "reserved screenshot shortcut rejected");
    Settings trusted;
    Settings custom;
    custom.hotkey_key = VK_F8; custom.hotkey_modifiers = MOD_CONTROL | MOD_SHIFT;
    custom.send_key = 'J'; custom.send_modifiers = MOD_ALT | MOD_SHIFT;
    custom.previous_plugin_key = 'K'; custom.previous_plugin_modifiers = 6;
    custom.next_plugin_key = 'L'; custom.next_plugin_modifiers = 3;
    Check(SaveSettings(custom, nullptr), "save independently configurable non-letter toggle and non-Enter send");
    Settings custom_loaded;
    Check(LoadSettings(&custom_loaded, nullptr) && SameSettings(custom, custom_loaded), "custom shortcuts survive restart");
    custom.send_key = custom.hotkey_key; custom.send_modifiers = custom.hotkey_modifiers;
    Check(!SaveSettings(custom, nullptr), "duplicate actions cannot share a chord");
    for (unsigned key : {'A', 'V', 'C', 'X'}) {
      custom.send_key = key; custom.send_modifiers = MOD_CONTROL;
      Check(!SaveSettings(custom, nullptr), "editing shortcuts cannot become send actions");
    }
    custom.send_key = VK_F6; custom.send_modifiers = 0;
    Check(SaveSettings(custom, nullptr), "safe bare function key can be a local send key");
    custom.send_key = 'J';
    Check(!SaveSettings(custom, nullptr), "bare typing key cannot send");
    std::ofstream(test.path(), std::ios::trunc) << "{\"version\":3,\"hotkey_key\":74}";
    Check(LoadSettings(&custom, nullptr) && custom.hotkey_modifiers == (MOD_ALT | MOD_SHIFT),
          "v3 key-only settings use the requested Windows default");
    trusted.revision = 77;
    trusted.schema = "english";
    trusted.base_url = "https://example.invalid/v1";
    trusted.model = "trusted-model";
    trusted.target_language = "Chinese";
    trusted.source_language = "English";
    trusted.theme = "quiet";
    trusted.ascii = trusted.traditional = trusted.ascii_punctuation = true;
    trusted.font_size = 21;
    trusted.candidate_count = 5;
    trusted.vertical_candidates = true;
    trusted.hotkey_modifiers = MOD_CONTROL | MOD_ALT;
    trusted.hotkey_key = 'J';
    for (const char* bad_json : {
             "{\"revision\":55,\"model\":\"partial\",\"hotkey_modifiers\":0,\"hotkey_key\":66}",
             "{\"hotkey_modifiers\":-1,\"hotkey_key\":66}",
             "{\"hotkey_modifiers\":6,\"hotkey_key\":-1}",
             "{\"revision\":55,\"font_size\":-1}",
             "{\"model\":\"partial\",\"theme\":5}",
             "{\"hotkey_modifiers\":\"6\"}", "[1,2]", "{"}) {
      std::ofstream(test.path(), std::ios::trunc) << bad_json;
      Settings current = trusted;
      std::string error;
      Check(!LoadSettings(&current, &error) && !error.empty() &&
                SameSettings(current, trusted),
            "invalid or partially parsed settings leave the complete trusted caller state intact");
      Settings defaults, initial_defaults = defaults;
      Check(!LoadSettings(&defaults, nullptr) && SameSettings(defaults, initial_defaults),
            "failed startup load cannot leak a bare or invalid global hotkey");
      std::ifstream original(test.path());
      const std::string original_json{std::istreambuf_iterator<char>(original),
                                      std::istreambuf_iterator<char>()};
      Check(original_json == bad_json, "failed load preserves the original file verbatim");
    }
    std::ofstream(test.path(), std::ios::trunc) << "{\"theme\":5}";
    Check(!LoadSettings(&retained, nullptr), "wrong JSON type fails load");
    std::cout << "Settings theme persistence and legacy compatibility passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
