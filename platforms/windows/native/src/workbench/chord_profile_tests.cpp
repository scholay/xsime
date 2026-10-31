#include "chord_profile.hpp"
#include <iostream>
#include <stdexcept>

using namespace rimes::windows::workbench;
using rimes::windows::core::Json;
namespace {
void Check(bool ok, const char *reason) {
  if (!ok)
    throw std::runtime_error(reason);
}
void Reject(const Json &value) {
  bool rejected = false;
  try {
    (void)ChordProfile::Parse(value);
  } catch (...) {
    rejected = true;
  }
  Check(rejected, "malformed or unsupported keymap must be rejected");
}
} // namespace
int main() {
  try {
    const auto source = BuiltinChordProfile();
    const auto p = ChordProfile::Parse(source);
    Check(p.name == "isaac2026" && p.Find("ivd") &&
              p.Find("ivd")->output == "ni",
          "portable built-in mapping and name");
    Check(!p.Find("dvi!"),
          "unknown keys cannot silently select another mapping");
    for (const auto &m : p.mappings)
      Check(p.Find(m.keys) != nullptr, "all built-in combinations resolve");
    auto invalid = source;
    invalid["mappings"].push_back(invalid["mappings"].front());
    Reject(invalid);
    invalid = source;
    invalid["leftKeys"] = "aa";
    Reject(invalid);
    invalid = source;
    invalid["mappings"][0]["output"] = "n;exit";
    Reject(invalid);
    invalid = source;
    invalid["mappings"][0]["keys"] = "dd";
    Reject(invalid);
    invalid = source;
    invalid["nativeSchemeID"] = "custom";
    Reject(invalid);
    invalid = source;
    invalid["outputEncoding"] = "unknown";
    Reject(invalid);
    invalid = source;
    invalid["formatVersion"] = 2;
    Reject(invalid);
    invalid = source;
    invalid["name"] = std::string("name\0tail", 9);
    Reject(invalid);
    ChordComposer composer;
    Check(composer.Key(p, 'd', true, 0, 50).handled, "first down is owned");
    Check(composer.Key(p, 'v', true, 20, 50).text.empty(),
          "chord waits for physical release");
    Check(composer.Key(p, 'd', true, 30, 50).text.empty(), "repeat is ignored");
    Check(composer.Key(p, 'd', false, 35, 50).text.empty(),
          "partial release does not settle");
    auto out = composer.Key(p, 'v', false, 40, 50);
    Check(out.text == "n'" && !out.replace,
          "left fragment uses legacy batch boundary");
    composer.Key(p, 'i', true, 60, 50);
    out = composer.Key(p, 'i', false, 65, 50);
    Check(out.replace && out.raw == "ni'" && out.text == "i",
          "adjacent right batch replaces only owned preedit; fallback stays "
          "available");
    Check(!composer.Key(p, 'i', false, 70, 50).handled,
          "late release cannot duplicate a chord");
    composer.Reset();
    composer.Key(p, 'd', true, 0, 50);
    composer.Key(p, 'v', true, 10, 50);
    composer.Key(p, 'i', true, 20, 50);
    composer.Key(p, 'v', false, 30, 50);
    composer.Key(p, 'd', false, 35, 50);
    out = composer.Key(p, 'i', false, 40, 50);
    Check(out.text == "ni'" && !out.replace,
          "simultaneous full syllable needs no rewrite");
    composer.Reset();
    composer.Key(p, 'd', true, 0, 50);
    composer.Key(p, 'v', true, 10, 50);
    Check(composer.Pending(), "batch is pending before context switch");
    composer.Reset();
    Check(!composer.Key(p, 'v', false, 30, 50).handled && !composer.Pending(),
          "context reset drops delayed old releases");
    composer.Key(p, 'i', true, 40, 50);
    out = composer.Key(p, 'i', false, 45, 50);
    Check(out.text == "i" && !out.replace,
          "new context cannot pair with old left keys");
    composer.Reset();
    composer.Key(p, 'd', true, 0, 50);
    out = composer.Key(p, 'v', true, 100, 50);
    Check(out.text == "d" && composer.Pending(),
          "interval splits non-simultaneous batches");
    auto explicit_source = source;
    explicit_source["boundaryPolicy"] = "explicitSyllables";
    const auto explicit_profile = ChordProfile::Parse(explicit_source);
    composer.Reset();
    composer.Key(explicit_profile, 'd', true, 0, 50);
    composer.Key(explicit_profile, 'v', true, 10, 50);
    out = composer.Flush(explicit_profile);
    Check(out.text == "n",
          "explicit boundaries preserve fragments without a separator");
    composer.RevokePair();
    composer.Key(explicit_profile, 'i', true, 20, 50);
    composer.Key(explicit_profile, 'd', false, 30, 50);
    composer.Key(explicit_profile, 'v', false, 31, 50);
    out = composer.Key(explicit_profile, 'i', false, 32, 50);
    Check(!out.replace, "selection or external edit revokes adjacent rewrite");
    Check(p.encoding == "ziranma" && p.EngineSchema() == "rimes_windows_chord_ziranma",
          "maintained Isaac2026 default uses the matching natural double-pinyin engine");
    Check(p.id == "21616362-5ee0-4833-a32b-f1dd05c589e7",
          "default export retains the portable Isaac2026 UUID");
    for (const auto& [pinyin, code] : std::initializer_list<std::pair<const char*, const char*>>{
             {"qing", "qy"}, {"zhang", "vh"}, {"shui", "uv"}, {"xiong", "xs"},
             {"lve", "lt"}, {"a", "aa"}, {"e", "ee"}, {"o", "oo"}, {"ang", "ah"}})
      Check(p.EngineOutput({"", pinyin, "syllable"}) == code,
            "complete syllables use canonical two-key codes");
    for (const auto& [pinyin, code] : std::initializer_list<std::pair<const char*, const char*>>{
             {"zh", "v"}, {"ch", "i"}, {"sh", "u"}, {"ong", "s"}, {"iu", "q"}, {"uan", "r"}, {"a", "a"}})
      Check(p.EngineOutput({"", pinyin, "fragment"}) == code,
            "fragments retain one-key natural codes instead of becoming full syllables");
    for (const auto& m : p.mappings) {
      const auto code = p.EngineOutput(m);
      Check(code.size() == (m.kind == "syllable" ? 2U : 1U),
            "every shipped mapping can be encoded with its declared type");
    }
    Check(p.Find("qy")->output == "qing", "editable and learning mappings stay full pinyin");
    auto settle = [&](const ChordProfile& profile, const std::string& keys, std::uint64_t at) {
      ChordComposer::Output result;
      for (char key : keys) composer.Key(profile, key, true, at++, 50);
      for (char key : keys) result = composer.Key(profile, key, false, at++, 50);
      return result;
    };
    composer.Reset();
    out = settle(p, "qy", 0);
    Check(out.text == "qy'", "physical qy chord feeds two encoded letters, not qing");
    composer.Reset();
    out = settle(p, "dv", 0);
    Check(out.text == "n'", "settled left batch preserves the legacy boundary rule");
    out = settle(p, "km", 20);
    Check(out.replace && out.raw == "ns'" && out.text == "s'",
          "left/right recombination re-encodes nong as one syllable and retains fallback");
    composer.Reset();
    out = settle(p, "dvkm", 0);
    Check(!out.replace && out.text == "ns'", "simultaneous and split nong chords encode identically");
    const auto legacy = LegacyBuiltinChordProfile();
    Check(EffectiveChordProfile(legacy) == source && EffectiveChordProfile(Json()) == source,
          "only unchanged maintained defaults advance to Isaac2026 natural encoding");
    auto custom = legacy;
    custom["name"] = "My preserved full-pinyin profile";
    Check(EffectiveChordProfile(custom) == custom && ChordProfile::Parse(custom).encoding == "fullPinyin",
          "legacy edited keymaps are not silently migrated");
    custom.erase("outputEncoding");
    Check(EffectiveChordProfile(custom) == custom && ChordProfile::Parse(custom).encoding == "fullPinyin",
          "a missing encoding in an old custom keymap still means full pinyin");
    custom = source; custom["outputEncoding"] = "fullPinyin";
    Check(EffectiveChordProfile(custom) == custom,
          "an explicit full-pinyin choice remains stable after restart");
    composer.Reset();
    out = settle(ChordProfile::Parse(custom), "qy", 0);
    Check(out.text == "qing'", "legacy full-pinyin encoding is still executable");
    invalid = source; invalid["mappings"][0]["output"] = "zhx";
    Reject(invalid);
    const auto yaml = NaturalChordSchema();
    Check(yaml.starts_with("# XSIME Windows generated chord engine schema; do not edit.\n") &&
              yaml.find("chord_composer") == std::string::npos &&
              yaml.find("prism: rimes_windows_chord_ziranma") != std::string::npos &&
              yaml.find("preedit_format:") != std::string::npos,
          "encoded Windows batches use a matching prism and full-pinyin preedit without a second chord processor");
    std::cout << "isaac2026 keymap validation, physical batches, interval, "
                 "pairing and revocation passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
