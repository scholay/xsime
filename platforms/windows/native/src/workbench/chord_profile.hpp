#pragma once
#include "../core/control.hpp"
#include <algorithm>
#include <optional>
#include <set>

namespace rimes::windows::workbench {
struct ChordMapping {
  std::string keys, output, kind;
};
struct ChordProfile {
  std::string id, name, left, right, boundary;
  std::vector<ChordMapping> mappings;
  std::string encoding = "fullPinyin";
  std::string EngineOutput(const ChordMapping& mapping) const;
  std::string EngineSchema() const {
    return encoding == "ziranma" ? "rimes_windows_chord_ziranma" : "rime_ice";
  }
  std::string Canonical(const std::string &keys) const {
    std::string result;
    for (char c : left + right)
      if (keys.find(c) != std::string::npos)
        result += c;
    return result;
  }
  const ChordMapping *Find(const std::string &keys) const {
    if (keys.find_first_not_of(left + right) != std::string::npos)
      return nullptr;
    const auto canonical = Canonical(keys);
    for (const auto &mapping : mappings)
      if (mapping.keys == canonical)
        return &mapping;
    return nullptr;
  }
  static ChordProfile Parse(const core::Json &value) {
    if (!value.is_object() || value.value("formatVersion", 0) != 1 ||
        value.dump().size() > 2 * 1024 * 1024)
      throw std::runtime_error(
          "并击配置必须是 formatVersion=1 的 JSON，且不超过 2 MiB。");
    ChordProfile p{value.at("id"),
                   value.at("name"),
                   value.at("leftKeys"),
                   value.at("rightKeys"),
                   value.value("boundaryPolicy", "explicitSyllables"),
                   {}, value.value("outputEncoding", "fullPinyin")};
    if (p.id.empty() || p.id.size() > 160 ||
        p.id.find('\0') != std::string::npos || p.name.empty() ||
        p.name.size() > 240 || p.name.find('\0') != std::string::npos ||
        p.left.empty() || p.right.empty() ||
        (p.encoding != "fullPinyin" && p.encoding != "ziranma") ||
        (value.contains("nativeSchemeID") &&
         !value.at("nativeSchemeID").is_null()) ||
        (p.boundary != "explicitSyllables" && p.boundary != "legacyBatches"))
      throw std::runtime_error(
          "请选择全拼或自然码双拼输出；名称和左右键区不能为空。");
    const auto alphabet = p.left + p.right;
    std::set<char> keys(alphabet.begin(), alphabet.end());
    if (keys.size() != alphabet.size() ||
        alphabet.find_first_not_of("abcdefghijklmnopqrstuvwxyz,.") !=
            std::string::npos)
      throw std::runtime_error(
          "左右键区不能重复，只能使用小写字母、逗号和句号。");
    if (!value.at("mappings").is_array() || value.at("mappings").empty() ||
        value.at("mappings").size() > 4096)
      throw std::runtime_error("并击配置需要 1–4096 条映射。");
    std::set<std::string> seen;
    for (const auto &row : value.at("mappings")) {
      ChordMapping m{row.at("keys"), row.at("output"), row.at("kind")};
      const std::set<char> unique(m.keys.begin(), m.keys.end());
      if (unique.size() < 2 || unique.size() != m.keys.size() ||
          m.keys.find_first_not_of(alphabet) != std::string::npos ||
          m.output.empty() || m.output.size() > 32 ||
          m.output.find_first_not_of("abcdefghijklmnopqrstuvwxyz") !=
              std::string::npos ||
          (m.kind != "fragment" && m.kind != "syllable"))
        throw std::runtime_error(
            "映射需要至少两个不同的键、全拼字母输出和有效的映射类型。");
      m.keys = p.Canonical(m.keys);
      if (!seen.insert(m.keys).second)
        throw std::runtime_error("并击组合重复（按键顺序不影响组合）。");
      if (p.encoding == "ziranma") {
        try { (void)p.EngineOutput(m); }
        catch (...) {
          throw std::runtime_error("第 " + std::to_string(p.mappings.size() + 1) +
              " 条映射（" + m.output + "）不能转为自然码，请检查完整音节或片段类型。");
        }
      }
      p.mappings.push_back(std::move(m));
    }
    return p;
  }
};
core::Json BuiltinChordProfile();
core::Json LegacyBuiltinChordProfile();
core::Json EffectiveChordProfile(const core::Json& value);
std::string NaturalChordSchema();

// A physical batch never outlives its TSF session. The interval separates
// non-simultaneous key downs; the final release settles the current batch.
// Only the immediately adjacent left/right batches may rewrite owned preedit.
class ChordComposer {
public:
  struct Output {
    bool handled = false, replace = false;
    std::string text, raw;
    bool settled = false;
  };
  void Reset() {
    held_.clear();
    batch_.clear();
    previous_.clear();
    raw_.clear();
    last_at_ = 0;
  }
  void RevokePair() {
    previous_.clear();
    raw_.clear();
  }
  bool Pending() const { return !batch_.empty(); }
  Output Key(const ChordProfile &p, char key, bool down, std::uint64_t now,
             unsigned interval) {
    Output output;
    if ((p.left + p.right).find(key) == std::string::npos)
      return output;
    output.handled = true;
    if (down) {
      if (held_.contains(key))
        return output;
      if (!batch_.empty() && now - last_at_ > interval)
        output = Settle(p);
      held_.insert(key);
      batch_ += key;
      last_at_ = now;
    } else {
      if (!held_.erase(key)) {
        output.handled = false;
        return output;
      }
      if (held_.empty() && !batch_.empty())
        output = Settle(p);
    }
    return output;
  }
  Output Flush(const ChordProfile &p) {
    return batch_.empty() ? Output{} : Settle(p);
  }

private:
  std::set<char> held_;
  std::string batch_, previous_, previous_prefix_, raw_;
  std::uint64_t last_at_ = 0;
  Output Settle(const ChordProfile &p) {
    const auto keys = p.Canonical(batch_);
    batch_.clear();
    std::string text = keys;
    if (keys.size() > 1) {
      if (const auto *m = p.Find(keys))
        text = p.EngineOutput(*m) +
               ((m->kind == "syllable" || p.boundary == "legacyBatches") ? "'"
                                                                         : "");
      else {
        text.erase(std::remove_if(text.begin(), text.end(),
                                  [](char c) { return c == ',' || c == '.'; }),
                   text.end());
        std::sort(text.begin(), text.end());
      }
    }
    const auto fallback = text;
    bool replace = false;
    if (!previous_.empty() && (keys.size() > 1 || previous_.size() > 1) &&
        previous_.find_first_not_of(p.left) == std::string::npos &&
        keys.find_first_not_of(p.right) == std::string::npos) {
      if (const auto *combined = p.Find(previous_ + keys);
          combined && combined->kind == "syllable") {
        text = p.EngineOutput(*combined) + "'";
        raw_ = previous_prefix_;
        replace = true;
      }
    }
    previous_prefix_ = raw_;
    raw_ += text;
    previous_ = replace || keys.find_first_not_of(p.left) != std::string::npos
                    ? ""
                    : keys;
    if (raw_.size() > 512)
      RevokePair();
    return {true, replace, replace ? fallback : text, raw_, true};
  }
};
} // namespace rimes::windows::workbench
