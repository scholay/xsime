#include "configuration.hpp"
#include "shortcuts.hpp"
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace rimes::runtime;
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void Reject(F operation) {
  bool failed = false;
  try {
    operation();
  } catch (...) {
    failed = true;
  }
  Check(failed, "invalid configuration accepted");
}
int main() {
  char path[] = "/tmp/xsime-configuration-XXXXXX";
  Check(mkdtemp(path), "fixture failed");
  const std::filesystem::path root(path);
  try {
    ConfigurationStore store(root / "settings");
    const auto first = store.Load();
    Check(first.at("resetOnAppSwitch") == false,
          "fresh install must preserve staged content across applications");
    auto next = first;
    const auto id = RandomID();
    auto provider = FieldDefaults(ProviderFields());
    provider["id"] = id;
    provider["name"] = "Local fixture";
    provider["baseURL"] = "http://127.0.0.1:12345/v1";
    provider["model"] = "fixture-model";
    next["providers"].push_back(provider);
    next["activeProvider"] = id;
    next["plugins"]["builtin.apple-translation"] = {
        {"sourceLanguage", "zh-Hans"}, {"targetLanguage", "en"}};
    const std::string secret = "fixture-credential-not-for-diagnostics";
    store.Save(next, first.at("revision"), {{id, secret}});
    const auto snapshot = store.Freeze("builtin.openai-compatible");
    Check(snapshot.credential == secret &&
              snapshot.provider.at("model") == "fixture-model",
          "credential did not reach frozen request");
    PrivateDirectory prefs(root / "settings");
    auto legacy = store.Load();
    legacy.erase("autoSendSeconds");
    legacy.erase("shortcuts"); legacy.erase("globalShortcuts");
    prefs.Write("preferences.json", legacy.dump());
    Check(store.Load().at("autoSendSeconds") == 1 &&
          store.Load().at("revision") == legacy.at("revision"), "legacy auto-send migration lost revision");
    Check(store.Load().at("shortcuts") == DefaultShortcuts() && store.Load().at("globalShortcuts") == true,
          "legacy shortcut migration failed");
    ValidateShortcuts(DefaultShortcuts());
    for (const auto* chord : {"b", "SHIFT+b", "CTRL+a", "CTRL+v", "LOGO+v", "CTRL+CTRL+b", "CTRL+SHIFT+B", "SHIFT+CTRL+b", "CTRL+bad", "CTRL+b\n"})
      Reject([&] { ParseShortcut(chord); });
    Check(!ParseShortcut("") && ParseShortcut("CTRL+ALT+F12")->Fcitx() == "Control+Alt+F12", "shortcut encoding failed");
    auto duplicate = store.Load(); duplicate["shortcuts"]["capsule"] = duplicate["shortcuts"]["buffer"];
    Reject([&] { store.Save(duplicate, duplicate.at("revision")); });
    auto invalid_lifetime = store.Load();
    invalid_lifetime["autoSendSeconds"] = 4;
    Reject([&] { store.Save(invalid_lifetime, invalid_lifetime.at("revision")); });
    Check(prefs.Read("preferences.json", 1048576)->find(secret) ==
              std::string::npos,
          "credential leaked to ordinary preferences");
    Check(!snapshot.provider.contains("credential"),
          "credential reference leaked to provider request");
    Reject([&] { store.Save(next, first.at("revision")); });
    auto current = store.Load();
    next = current;
    next["providers"][0]["model"] = "next-model";
    store.Save(next, current.at("revision"), {{id, "replacement-credential"}});
    Check(snapshot.credential == secret &&
              snapshot.provider.at("model") == "fixture-model",
          "save mutated in-flight snapshot");
    ConfigurationStore restarted(root / "settings");
    Check(restarted.Freeze("builtin.openai-compatible").credential ==
              "replacement-credential",
          "restart lost credential");
    current = store.Load();
    next = current;
    next["providers"][0]["credential"] = RandomID();
    Reject([&] { store.Save(next, current.at("revision")); });
    Check(store.Load() == current, "rejected save changed configuration");
    next = current;
    next["providers"][0]["apiKey"] = secret;
    Reject([&] { store.Save(next, current.at("revision")); });
    for (auto url :
         {"http://remote.example/v1", "https://user:pass@example.org/v1",
          "https://example.org/v1?key=secret", "file:///tmp/key"})
      Check(!ValidEndpoint(url), "unsafe endpoint accepted");
    Check(ValidEndpoint("https://example.org/v1") &&
              ValidEndpoint("http://[::1]:1234/v1"),
          "supported endpoint rejected");
    auto translated = ConfigurationStore::NormalizePlugin(
        "builtin.apple-translation", {{"provider", "old-cloud"}});
    Check(!translated.contains("provider") &&
              translated.at("continuous") == true,
          "translation retained cloud channel");
    Check(ConfigurationStore::NormalizePlugin("builtin.codex-cli",
                                              {{"model", "follow-default"}})
                  .at("model") == "gpt-6-sol",
          "CLI defaults migration missing");
    Check(ConfigurationStore::NormalizePlugin(
              "builtin.stream-input",
              {{"debounceSeconds", 0.35}, {"maximumWaitSeconds", 1.2}})
                  .at("latency") == "stable",
          "stream pace migration missing");
    Reject([&] {
      ConfigurationStore::NormalizePlugin("builtin.stream-input",
                                          {{"candidateCount", 6}});
    });
    Reject([&] {
      ConfigurationStore::NormalizePlugin(
          "builtin.apple-translation",
          {{"sourceLanguage", "en"}, {"targetLanguage", "en"}});
    });
    // An unsafe retired blob must not turn an already committed preferences
    // transaction into a claimed rollback, nor follow/delete the other link.
    const auto retired =
        root / "settings" / "credentials" /
        (id + "." +
         current.at("providers")[0].at("credential").get<std::string>());
    const auto other_link = root / "retired-link";
    std::filesystem::create_hard_link(retired, other_link);
    Check(!store.Save(current, current.at("revision"), {{id, ""}}),
          "credential cleanup failure was hidden");
    Check(std::filesystem::exists(other_link),
          "cleanup touched an unsafe linked file");
    Check(store.Freeze("builtin.openai-compatible").credential.empty(),
          "explicit credential deletion failed");
    const auto intact = store.Load();
    prefs.Write("preferences.json", "invalid");
    Reject([&] { store.Load(); });
    Reject([&] { store.Save(intact, intact.at("revision")); });
    Check(prefs.Read("preferences.json", 1024) == "invalid",
          "corrupt settings silently reset");
    prefs.Write("preferences.json", intact.dump());
    Check(store.Load() == intact, "backup restoration failed");
    chmod((root / "settings").c_str(), 0755);
    Reject([&] { store.Freeze("builtin.openai-compatible"); });
    chmod((root / "settings").c_str(), 0700);
    std::cout << "Configuration transactions, immutable requests, credential "
                 "isolation and migration passed\n";
  } catch (...) {
    std::filesystem::remove_all(root);
    throw;
  }
  std::filesystem::remove_all(root);
}
