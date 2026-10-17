#include "shortcuts.hpp"
#include "native_shortcuts.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace rimes::runtime {
namespace {
const std::vector<std::pair<std::string, unsigned>> kModifiers = {
    {"CTRL", Shortcut::Control}, {"ALT", Shortcut::Alt}, {"SHIFT", Shortcut::Shift}, {"LOGO", Shortcut::Logo}};
void Require(bool valid) { if (!valid) throw std::runtime_error("快捷键无效、重复或占用了编辑快捷键。"); }
}
std::string Shortcut::Canonical() const {
    std::string result;
    for (const auto& [name, mask] : kModifiers) if (modifiers & mask) result += name + "+";
    return result + key;
}
std::string Shortcut::Fcitx() const {
    std::string result;
    const char* names[] = {"Control", "Alt", "Shift", "Super"};
    for (unsigned i = 0; i < 4; ++i) if (modifiers & (1U << i)) result += std::string(names[i]) + "+";
    return result + key;
}
std::optional<Shortcut> ParseShortcut(const std::string& value) {
    if (value.empty()) return {};
    Require(value.size() <= 80);
    Shortcut result;
    std::size_t start = 0;
    while (true) {
        const auto end = value.find('+', start);
        if (end == value.npos) { result.key = value.substr(start); break; }
        const auto token = value.substr(start, end - start);
        const auto found = std::find_if(kModifiers.begin(), kModifiers.end(), [&](const auto& m) { return m.first == token; });
        Require(found != kModifiers.end() && !(result.modifiers & found->second));
        result.modifiers |= found->second; start = end + 1;
    }
    const std::set<std::string> special = {"Return", "Tab", "space", "comma", "period", "Left", "Right", "Up", "Down",
        "Home", "End", "Page_Up", "Page_Down", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    Require((result.key.size() == 1 && ((result.key[0] >= 'a' && result.key[0] <= 'z') ||
        (result.key[0] >= '0' && result.key[0] <= '9'))) || special.count(result.key));
    Require(result.modifiers & (Shortcut::Control | Shortcut::Alt | Shortcut::Logo));
    Require(!((result.modifiers == Shortcut::Control || result.modifiers == Shortcut::Logo) &&
              (result.key == "a" || result.key == "v")));
    Require(result.Canonical() == value);
    return result;
}
plugins::Json ShortcutActions() { return plugins::Json::parse(rimes::native::shortcutContract); }
plugins::Json DefaultShortcuts() {
    auto result = plugins::Json::object();
    for (const auto& action : ShortcutActions()) result[action.at("id").get<std::string>()] = action.at("default");
    return result;
}
void ValidateShortcuts(const plugins::Json& value) {
    Require(value.is_object() && value.size() == ShortcutActions().size());
    std::set<std::string> seen;
    for (const auto& action : ShortcutActions()) {
        const auto id = action.at("id").get<std::string>();
        Require(value.contains(id) && value.at(id).is_string());
        const auto parsed = ParseShortcut(value.at(id));
        if (parsed) Require(seen.insert(parsed->Canonical()).second);
    }
}
} // namespace rimes::runtime
