// Copyright 2026 scholay. SPDX-License-Identifier: Apache-2.0
#include <objbase.h>

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>

#include "metrics.hpp"
#include "metrics_ui.hpp"
#include "settings_ui.hpp"

using namespace rimes::windows;
namespace {
int failures = 0;
void Check(bool condition, const char* reason) {
  if (!condition) {
    std::cerr << "FAIL: " << reason << "\n";
    ++failures;
  }
}
metrics::Article Article(std::string text) {
  return {"test", "chinese", "Test", "", "", std::move(text), 1};
}
metrics::Json Context(bool chord = true) {
  return {{"chordAvailable", chord},
          {"schema", "my_combo"},
          {"comparison", "test"}};
}
void Core() {
  using namespace metrics;
  Check(Articles().size() == 8, "same eight versioned articles");
  for (const auto& a : Articles()) {
    const auto size = Characters(a.text).size();
    Check(a.version == 1 && size <= 2048 && a.text.size() <= 16384,
          "article bounds");
    if (a.language == "chinese")
      Check(size >= 400 && size <= 600, "Chinese corpus length matches macOS");
  }
  Check(Characters("有👨‍👩‍👧‍👦🇨🇳é").size() == 4,
        "Unicode grapheme clusters");
  Check(Characters("é") == Characters("é"),
        "Swift Character canonical equivalence");
  Check(Normalize("a\r\nb\rc") == "abc", "visual track and paragraph breaks are excluded from scoring");
  Check(Characters("甲\n乙") == Characters("甲乙"), "native row breaks preserve scoring offsets");
  Session s(Article("甲乙丙丁"), Context());
  s.Key(1, false, false, false);
  s.Chord(1.1);
  Check(s.Reconcile("甲丙丁", 2), "missing middle reconciliation");
  Check(s.Snapshot()["typedCorrectness"] == Json::array({true, true, true}),
        "live input track exposes correctness aligned to committed graphemes");
  auto m = s.Snapshot()["metrics"];
  Check(m["correct"] == 3 && m["omissions"] == 1 && m["substitutions"] == 0,
        "alignment recovers after missing character");
  Check(s.Complete(), "aligned final character completes");
  auto result = s.Finish(3, 100);
  Check(result && (*result)["metrics"]["effectiveCPM"] == 90 &&
            (*result)["metrics"]["finalAccuracy"] == 0.75,
        "elapsed and final accuracy");
  Check(result && (*result)["metrics"]["chords"] == 1 &&
            !result->contains("text") && !result->contains("states") &&
            !result->contains("typedCorrectness"),
        "aggregate-only history");
  Session correction(Article("ab"), Context(false));
  correction.Key(0, false, false, false);
  correction.Reconcile("x", 1);
  Check(correction.Snapshot()["typedCorrectness"] == Json::array({false}),
        "wrong committed graphemes are marked only in the transient snapshot");
  correction.Key(2, false, true, false);
  correction.Reconcile("", 2);
  correction.Reconcile("ab", 3);
  auto fixed = correction.Finish(4, 100);
  Check(fixed && (*fixed)["metrics"]["attempts"] == 3 &&
            (*fixed)["metrics"]["correctAttempts"] == 2,
        "correction retains wrong attempt");
  Check(fixed && (*fixed)["metrics"]["deleted"] == 1 &&
            (*fixed)["metrics"]["corrections"] == 1,
        "deletion distinct from backspace");
  Check(fixed && (*fixed)["metrics"]["chords"].is_null(),
        "other IME chord unavailable");
  Session identical(Article("abc"), Context());
  identical.Reconcile("abc", 0);
  Check(identical.Reconcile("abc", 1,
                            std::pair<std::size_t, std::size_t>{1, 2}) &&
            identical.Snapshot()["metrics"]["attempts"] == 4 &&
            identical.Snapshot()["metrics"]["deleted"] == 1,
        "same-text explicit replacement counts as another attempt");
  Check(
      !identical.Reconcile("xyz", 2, std::pair<std::size_t, std::size_t>{1, 2}),
      "stale insertion range rejected");
  Session pending(Article("abcd"), Context());
  pending.Key(1, false, false, false);
  pending.Reconcile("ab", 2);
  Check(pending.Snapshot()["metrics"]["omissions"] == 0,
        "untouched tail not counted live");
  auto early = pending.Finish(3, 100);
  Check(early && (*early)["metrics"]["omissions"] == 2 &&
            !(*early)["reasons"].empty() && !(*early)["complete"].get<bool>(),
        "manual finish counts tail and practice");
  Check(pending.Finish(300, 200) == early &&
            !pending.Snapshot()["complete"].get<bool>(),
        "finished results and completeness remain frozen");
  Session keys(Article("abc"), Context());
  keys.Key(1, false, true, false);
  keys.UpgradeBackspace();
  keys.Key(2, true, true, true);
  keys.Key(3, false, false, false);
  keys.Reconcile("a", 4);
  auto km = keys.Snapshot()["metrics"];
  Check(km["keys"] == 2 && km["repeats"] == 1 && km["backspaces"] == 2 &&
            km["compositionBackspaces"] == 2 && km["committedBackspaces"] == 0,
        "repeat/backspace/composition counters");
  Check(!keys.Reconcile(std::string(16385, 'a'), 5), "text byte bound");
  Check(!keys.Reconcile(std::string(2049, 'a'), 5), "character bound");
  Check(!keys.Reconcile("b", std::numeric_limits<double>::quiet_NaN()),
        "invalid clock rejected");
  for (int i = 5; i < 2000; ++i) keys.Tick(i);
  Check(keys.Snapshot()["metrics"]["samples"].size() <= 120,
        "bounded adaptive curve");
  Check(
      TypingKey('A', 0) && TypingKey(VK_BACK, 0) && !TypingKey(VK_LEFT, 0) &&
          !TypingKey(VK_SHIFT, 0) &&
          !TypingKey('V', static_cast<unsigned>(core::KeyModifiers::kControl)),
      "physical typing filter");
  Session clock(Article("abc"), Context());
  clock.Key(-0.5, false, false, false);
  Check(!clock.Reconcile("a", -0.5) && !clock.Started() &&
            clock.Snapshot()["metrics"]["keys"] == 0,
        "negative observations cannot start or mutate a live session");
  clock.Start(10);
  clock.Chord(9);
  clock.Chord(std::numeric_limits<double>::quiet_NaN());
  Check(clock.Snapshot()["metrics"]["chords"] == 0,
        "stale and invalid chord observations are ignored like macOS");
  clock.Key(11, false, true, false);
  clock.Chord(12);
  clock.Reconcile("a", 13);
  auto clockResult = clock.Finish(14, 100);
  Check(clockResult && (*clockResult)["metrics"]["elapsed"] == 4 &&
            (*clockResult)["metrics"]["chords"] == 1,
        "marked-text start and accepted chords share the monotonic clock");
  const auto frozen = clock.Snapshot();
  clock.UpgradeBackspace();
  clock.Key(15, false, false, false);
  clock.Chord(15);
  clock.Tick(15);
  Check(clock.Snapshot() == frozen,
        "finished telemetry cannot be reclassified by late input callbacks");
}
void PracticeViewPreferences(const std::filesystem::path& root) {
  using namespace metrics;
  std::filesystem::create_directories(root);
  Json old;
  {
    Service original(root);
    old = original.Data();
    Check(original.PracticePreferences() ==
              Json{{"materialsCollapsed", false}, {"gridHidden", false},
                   {"cellsHidden", false}, {"lastArticleID", ""}},
          "new and legacy profiles inherit macOS practice view defaults");
  }
  Check(!old.contains("practicePreferences"),
        "reading defaults does not migrate or dirty an old statistics file");
  std::ofstream(root / L"metrics.json", std::ios::trunc) << old.dump();
  {
    Service legacy(root);
    Check(legacy.Issue().empty() && legacy.Data() == old &&
              legacy.PracticePreferences()["materialsCollapsed"] == false,
          "unmodified six-field statistics files load without migration");
  }
  old["futureVersionMetadata"] = {{"offset", -2}, {"enabled", true}};
  old["practicePreferences"] = {{"futureView", {"width", -1}},
                                {"gridHidden", true}};
  std::ofstream(root / L"metrics.json", std::ios::trunc) << old.dump();
  {
    Service migrated(root);
    Check(migrated.Issue().empty(), "optional view settings and unknown fields are compatible");
    Check(migrated.PracticePreferences()["gridHidden"] == true &&
              migrated.PracticePreferences()["cellsHidden"] == false,
          "partial stored preferences inherit missing keys");
    Check(migrated.SetPracticePreferences({{"materialsCollapsed", true},
                                          {"lastArticleID", "practice-article"}}),
          "practice view changes persist immediately");
    const auto before = migrated.Data();
    Check(!migrated.SetPracticePreferences({{"gridHidden", "yes"}}) &&
              !migrated.SetPracticePreferences({{"lastArticleID", std::string(257, 'a')}}) &&
              !migrated.SetPracticePreferences({{"lastArticleID", std::string("a\0b", 3)}}) &&
              !migrated.SetPracticePreferences({{"lastArticleID", std::string(1, static_cast<char>(0xff))}}) &&
              !migrated.SetPracticePreferences({{"unknownUIWrite", true}}) &&
              migrated.Data() == before,
          "invalid preference patches are rejected atomically");
    migrated.Clear("results");
    Check(migrated.Data()["futureVersionMetadata"] == old["futureVersionMetadata"] &&
              migrated.PracticePreferences()["futureView"] == old["practicePreferences"]["futureView"],
          "preference updates and history clearing preserve opaque fields");
  }
  {
    Service restarted(root);
    Check(restarted.Issue().empty() &&
              restarted.PracticePreferences()["materialsCollapsed"] == true &&
              restarted.PracticePreferences()["gridHidden"] == true &&
              restarted.PracticePreferences()["lastArticleID"] == "practice-article",
          "practice view state survives a service restart");
  }
  auto malformed = old;
  malformed["practicePreferences"]["cellsHidden"] = 1;
  const auto bytes = malformed.dump();
  std::ofstream(root / L"metrics.json", std::ios::trunc) << bytes;
  {
    Service invalid(root);
    Check(!invalid.Issue().empty() &&
              !invalid.SetPracticePreferences({{"cellsHidden", false}}),
          "invalid known preference fields retain the read-only recovery boundary");
    invalid.Save();
    std::ifstream stream(root / L"metrics.json", std::ios::binary);
    const std::string retained{std::istreambuf_iterator<char>(stream), {}};
    Check(retained == bytes, "invalid preference storage remains byte-for-byte preserved");
  }
}
void Storage(const std::filesystem::path& root) {
  using namespace metrics;
  const auto day = Day(1700000000);
  {
    Service s(root);
    s.Daily('A', false, 0, 0, 0, 1700000000);
    s.Daily('B', false, 0, 2, 1, 1700000002);
    s.Daily('A', true, 0, 0, 0, 1700000003);
    s.Daily('C', false, 0, 1, 0, 1700000015);
    s.Save();
    auto data = s.Data();
    auto d = data["days"][day];
    Check(d["keyCount"] == 3 && d["characters"] == 3 && d["chords"] == 1 &&
              d["active"] == 2 && d["sessionCount"] == 2,
          "daily inactivity and actual counts");
    s.Preferences(false, true);
    s.Daily('Z', false, 0, 0, 0, 1700000016);
    Check(!s.Data()["days"][day]["keys"].contains("90"),
          "independent key-frequency consent");
    s.Enable(false, true);
    const auto before = s.Data();
    s.Daily('Z', false, 0, 2, 1, 1700000017);
    Check(s.Data() == before, "disabled statistics stops collection");
    s.Clear("activity");
    Check(s.Data()["days"][day]["keys"].size() == 3 &&
              s.Data()["days"][day]["characters"] == 0,
          "clear trends preserves key distribution");
  }
  {
    Service restarted(root);
    Check(!restarted.Data()["recordKeys"].get<bool>() &&
              restarted.Data()["recordActivity"].get<bool>(),
          "preferences persisted");
    restarted.Clear("keys", day);
    Check(restarted.Data()["days"][day]["keys"].empty(), "clear selected day");
  }
  std::ofstream(root / L"metrics.json", std::ios::trunc) << "{corrupt";
  {
    Service corrupt(root);
    Check(!corrupt.Issue().empty(), "corruption enters read-only mode");
    corrupt.Daily('A', false, 0, 1, 0, 1700000000);
    corrupt.Save();
    Check(std::filesystem::file_size(root / L"metrics.json") == 8,
          "corrupt original preserved");
    Check(corrupt.Repair() && corrupt.Issue().empty(),
          "explicit backup repair");
  }
  bool backup = false;
  for (const auto& e : std::filesystem::directory_iterator(root))
    if (e.path().filename().wstring().starts_with(L"metrics-backup-"))
      backup = true;
  Check(backup, "repair keeps original backup");
  Json malformed;
  {
    Service clean(root);
    malformed = clean.Data();
  }
  Session sample(Article("abc"), Context());
  sample.Key(1, false, false, false);
  sample.Reconcile("ab", 2);
  auto result = sample.Finish(3, 1700000000);
  Check(result.has_value(), "nonempty practice has aggregate result");
  (*result)["metrics"]["samples"] = Json::array({Json::array({"bad", 3})});
  malformed["results"].push_back(*result);
  const auto malformedBytes = malformed.dump();
  std::ofstream(root / L"metrics.json", std::ios::trunc) << malformedBytes;
  {
    Service corruptCurve(root);
    Check(!corruptCurve.Issue().empty(), "malformed curve rejected before rendering");
    corruptCurve.Save();
    Check(std::filesystem::file_size(root / L"metrics.json") == malformedBytes.size(),
          "semantically invalid original preserved");
  }
}
}  // namespace
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const auto root =
      std::filesystem::temp_directory_path() /
      (L"rimes-metrics-tests-" + std::to_wstring(GetCurrentProcessId()));
  try {
    Core();
    Storage(root);
    PracticeViewPreferences(root / L"practice-view");
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    ++failures;
  }
  std::filesystem::remove_all(root);
  CoUninitialize();
  std::cout << "Metrics tests: " << (failures ? "FAILED" : "PASS") << "\n";
  return failures ? 1 : 0;
}
