#pragma once
#include "plugin_store.hpp"
#include <optional>
#include <string>
#include <vector>

namespace rimes::runtime {
struct Shortcut {
    enum Modifier : unsigned { Control = 1, Alt = 2, Shift = 4, Logo = 8 };
    unsigned modifiers = 0;
    std::string key;
    std::string Canonical() const;
    std::string Fcitx() const;
};
// Empty means unbound; malformed or unsafe combinations throw. The canonical
// format is shared with XDG portals (e.g. CTRL+SHIFT+b).
std::optional<Shortcut> ParseShortcut(const std::string& value);
plugins::Json ShortcutActions();
plugins::Json DefaultShortcuts();
void ValidateShortcuts(const plugins::Json& value);
} // namespace rimes::runtime
