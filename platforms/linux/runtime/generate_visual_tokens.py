#!/usr/bin/env python3
"""Import reviewed native macOS colors/navigation, never a stale prototype."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
SOURCES = ["Sources/RimeBuffer/RimeUI.swift", "Sources/RimeBuffer/SettingsWindow.swift",
           "Sources/RimeBuffer/SettingsRouting.swift", "Sources/RimeBuffer/BufferWindowController.swift",
           "Sources/RimeBuffer/BufferInlineView.swift", "Sources/RimeBuffer/ShortcutPreferences.swift",
           "Sources/RimeBuffer/RimeFixedAccentSwitch.swift"]
CATALOG = "Catalog/buffer-plugins.json"


def extract():
    ui, settings, routing, buffer, inline, shortcuts, switches = [(ROOT / name).read_text() for name in SOURCES]
    native_fields = set(re.findall(r"let (\w+): UInt32", ui.split("struct RimeThemePalette")[1].split("enum RimeThemePalettes")[0]))
    palettes = {}
    for name in ("night", "day", "quiet", "rasta"):
        block = re.search(r"static let " + name + r" = RimeThemePalette\((.*?)\n    \)", ui, re.S).group(1)
        pairs = re.findall(r"(\w+): (0x[0-9A-Fa-f]+|productGreen)", block)
        palettes[name] = {key: "#" + (value[2:] if value != "productGreen" else "22C55E") for key, value in pairs}
        if set(palettes[name]) != native_fields:
            raise ValueError(f"Native palette contract changed: {name}; review before import")
        # RimeThemePalette.accentText uses WCAG luminance, not a fixed green.
        def luminance(hex_color):
            rgb = [int(hex_color[i:i + 2], 16) / 255 for i in (1, 3, 5)]
            linear = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in rgb]
            return sum(v * weight for v, weight in zip(linear, (0.2126, 0.7152, 0.0722)))
        if ") >= 4.5 ? accentGreen : selectedCandidateBackground" not in ui:
            raise ValueError("Review native accent text contrast policy")
        palette = palettes[name]
        lo, hi = sorted((luminance(palette["accentGreen"]), luminance(palette["surfaceSecondary"])))
        palette["accentText"] = palette["accentGreen" if (hi + 0.05) / (lo + 0.05) >= 4.5 else "selectedCandidateBackground"]
    core = routing.split("enum SettingsCoreRoute:")[1].split("enum PluginManagementSubpage:")[0]
    ids = re.findall(r'case (\w+) = "([^"]+)"', core)
    titles = dict(re.findall(r'case \.(\w+): return "([^"]+)"', core.split("var symbolName")[0]))
    symbols = dict(re.findall(r'case \.(\w+): return "([^"]+)"', core.split("var symbolName")[1]))
    subpages = routing.split("enum CoreSettingsSubpages")[1].split("struct SettingsSubpageDescriptor")[0]
    plugin_section = routing.split("enum PluginManagementSubpage:")[1].split("enum CoreSettingsSubpages")[0]
    plugin_ids = re.findall(r'case (\w+) = "([^"]+)"', plugin_section)
    plugin_titles = dict(re.findall(r'case \.(\w+): return "([^"]+)"', plugin_section))
    routes = []
    for name, identity in ids:
        block = re.search(r"case \." + name + r":(.*?)(?=\n        case |\n        \})", subpages, re.S).group(1)
        pages = re.findall(r'\("([^"]+)", "([^"]+)"\)', block)
        if name == "rimes": pages.insert(0, ("about", "XSIME"))
        if name == "plugins": pages = [(key, plugin_titles[field]) for field, key in plugin_ids]
        if not pages: raise ValueError(f"Missing native subpages: {name}")
        routes.append({"id": identity, "title": titles.get(name, "XSIME"), "icon": symbols[name], "subpages": pages})
    def number(pattern, source=settings):
        found = re.search(pattern, source)
        if not found: raise ValueError(f"Native layout contract changed: {pattern}")
        return int(found.group(1))
    metrics = {
        "settingsWidth": number(r"previewContentSize = NSSize\(width: (\d+)"),
        "settingsHeight": number(r"previewContentSize = NSSize\(width: \d+, height: (\d+)"),
        "sidebarWidth": number(r"sidebarScrollView.widthAnchor.constraint\(equalToConstant: (\d+)"),
        "tabsHeight": number(r"tabsBar.heightAnchor.constraint\(equalToConstant: (\d+)"),
        "headingHeight": number(r"headingBar.heightAnchor.constraint\(equalToConstant: (\d+)"),
        "contentWidth": number(r"grid.widthAnchor.constraint\(equalToConstant: (\d+)"),
        "cardHeight": number(r"card.heightAnchor.constraint\(equalToConstant: (\d+)"),
        "controlSize": number(r"enum BufferWorkbenchMetrics.*?controlSize: CGFloat = (\d+)", buffer.replace("\n", " ")),
    }
    for name, field in (("bufferMinimumWidth", "standardMinimumWidth"), ("bufferMaximumWidth", "standardMaximumWidth"),
                        ("bufferHeight", "expandedHeight"), ("bufferDerivedHeight", "translationExpandedHeight"),
                        ("bufferToolbarOnlyHeight", "toolbarOnlyHeight")):
        metrics[name] = number(r"static let " + field + r": CGFloat = (\d+)", buffer)
    metrics["compactSwitchTrackWidth"] = number(r"isCompact \? NSSize\(width: (\d+)", switches)
    metrics["compactSwitchTrackHeight"] = number(r"isCompact \? NSSize\(width: \d+, height: (\d+)", switches)
    metrics["switchControlInset"] = number(r"NSSize\(width: trackSize.width \+ (\d+)", switches)
    metrics["switchKnobInset"] = number(r"let inset: CGFloat = (\d+)", switches)
    for field in ("controlSize", "chromeInset", "mainHorizontalInset", "shelfHorizontalInset"):
        metrics[field] = number(r"static let " + field + r": CGFloat = (\d+)", buffer)
    for field in ("blockSpacing", "railHorizontalInset", "itemHeight", "blockUnderlineWidth", "selectedBlockUnderlineWidth"):
        metrics[field] = number(r"static let " + field + r": CGFloat = (\d+)", inline)
    chrome = settings.split("private enum SettingsVisualStyle")[1].split("private enum SettingsPluginSwitchMode")[0]
    shortcut_rows = settings.split("private func shortcutSettingsView()", 1)[1].split("private func candidateMetricRow", 1)[0]
    for name, pattern in {
        "shortcutTitleFont": r"titleLabel.font = .systemFont\(ofSize: (\d+)",
        "shortcutTitleMaxWidth": r"titleLabel.widthAnchor.constraint\(lessThanOrEqualToConstant: (\d+)",
        "shortcutRowWidth": r"row.widthAnchor.constraint\(equalToConstant: (\d+)",
        "shortcutControlSpacing": r"row.spacing = (\d+)",
        "shortcutInsetVertical": r"NSEdgeInsets\(top: (\d+)",
        "shortcutInsetHorizontal": r"NSEdgeInsets\(top: \d+, left: (\d+)",
        "shortcutRowSpacing": r"stack.spacing = (\d+)",
    }.items(): metrics[name] = number(pattern, shortcut_rows)
    metrics["shortcutRecorderFont"] = number(r"font = .monospacedSystemFont\(ofSize: (\d+)", shortcuts)
    metrics["shortcutRecorderMinWidth"] = number(r"widthAnchor.constraint\(greaterThanOrEqualToConstant: (\d+)", shortcuts)
    metrics["settingsCardRadius"] = number(r"cardRadius: CGFloat \{ RimeUI.isLiquidGlass \? \d+ : (\d+)", chrome)
    metrics["settingsGlassCardRadius"] = number(r"cardRadius: CGFloat \{ RimeUI.isLiquidGlass \? (\d+)", chrome)
    for name, palette in palettes.items():
        for key, field in (("settingsBackground", "background"), ("settingsSeparator", "separator")):
            match = re.search(r"static var " + field + r".*?RimeUI.isDark \? (0x[0-9A-Fa-f]+) : (0x[0-9A-Fa-f]+)", chrome, re.S)
            if not match: raise ValueError(f"Missing native Settings chrome: {field}")
            palette[key] = "#" + match.group(2 if name == "day" else 1)[2:]
    # Labels and producer identities are shared with the native macOS catalog.
    # Import presentation only; availability is always decided by Linux adapters.
    plugins = {p["id"]: {"name": p["nameZH"], "producer": {
        "official": "RIMES 官方", "openai": "OpenAI", "anthropic": "Anthropic", "scholay": "Scholay"
    }[p["producerID"]]} for p in json.loads((ROOT / CATALOG).read_text())["plugins"]}
    # These are semantic names, not copies of the Apple SF Symbols assets.
    icons = {"apple-translation": "character.book.closed", "codex-cli": "rimes.brand.chatgpt",
             "claude-code-cli": "rimes.brand.claude", "openai-compatible": "network",
             "stream-input": "waveform", "music": "waveform", "morse": "dot.radiowaves.left.and.right",
             "polisher": "rimes.brand.scholay", "latex": "rimes.brand.scholay", "scholay": "rimes.brand.scholay",
             "statistics": "chart.bar.xaxis", "typing-speed": "speedometer", "fly-chord-learning": "hands.sparkles",
             "capsule.temporary": "clock.arrow.circlepath", "capsule.capture": "camera.viewfinder",
             "capsule.notes": "note.text", "capsule.resources": "books.vertical", "capsule.passwords": "lock",
             "mailbox.terminal": "terminal", "mailbox.chat": "bubble.left", "mailbox.inbox": "tray"}
    for identity, plugin in plugins.items():
        if identity.removeprefix("builtin.") not in icons:
            raise ValueError(f"Review native visual identity before importing: {identity}")
        plugin["icon"] = icons[identity.removeprefix("builtin.")]
    brands = {name: base64.b64encode((ROOT / f"Sources/RimeBuffer/Resources/PluginIcons/{name}.png").read_bytes()).decode()
              for name in ("chatgpt", "claude")}
    return {"schemaVersion": 1, "palettes": palettes, "routes": routes, "metrics": metrics,
            "plugins": plugins, "brands": brands,
            "sources": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in SOURCES + [CATALOG]}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    imported = extract()
    value = json.dumps(imported, ensure_ascii=False, sort_keys=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('// Generated from native macOS sources; do not edit.\n#pragma once\n'
                           'namespace rimes::desktop { inline constexpr char kNativeVisuals[] = R"xsime(' + value + ')xsime"; }\n')

    # Core geometry constraints use the same generated constants as the renderer.
    args.output.with_name("native_buffer_metrics.hpp").write_text(
        "// Generated from macOS native metrics.\n#pragma once\nnamespace rimes::native {\n" +
        "\n".join(f"inline constexpr int {key} = {value};" for key, value in imported["metrics"].items()) + "\n}\n")
