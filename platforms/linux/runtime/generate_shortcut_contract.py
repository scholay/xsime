#!/usr/bin/env python3
"""Import the four workbench launch bindings from the macOS source of truth."""
import argparse
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[3]


def generate():
    source = (ROOT / "Sources/RimeBuffer/ShortcutPreferences.swift").read_text()
    titles = source.split("var title: String", 1)[1].split("var detail:", 1)[0]
    defaults = source.split("var defaultShortcut:", 1)[1].split("case .previousPlugin:", 1)[0]
    actions = []
    for native, action in (("toggleWorkbench", "buffer"), ("toggleClipboardHistory", "capsule"),
                           ("openMailbox", "mailbox"), ("openSettings", "settings")):
        title = re.search(rf'case \.{native}: return "([^"]+)"', titles)
        block = re.search(rf"case \.{native}:(.*?)(?=case \.|$)", defaults, re.S)
        key = re.search(r"keyCode: UInt16\(kVK_ANSI_([A-Z])\)", block[1] if block else "")
        modifiers = re.search(r"modifiers: \[([^\]]+)\]", block[1] if block else "")
        if not title or not key or not modifiers:
            raise ValueError(f"Review macOS shortcut contract for {native}")
        names = {".command": "CTRL", ".control": "CTRL", ".shift": "SHIFT", ".option": "ALT"}
        mapped = [names[v.strip()] for v in modifiers[1].split(",")]
        if len(set(mapped)) != len(mapped):
            raise ValueError(f"Review Linux mapping of combined Command/Control for {native}")
        flags = set(mapped)
        chord = "+".join([m for m in ("CTRL", "ALT", "SHIFT", "LOGO") if m in flags] + [key[1].lower()])
        actions.append({"id": action, "native": native, "title": title[1], "default": chord})
    return actions


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    data = json.dumps(generate(), ensure_ascii=False, separators=(",", ":"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('#pragma once\nnamespace rimes::native {\ninline constexpr auto shortcutContract = R"SHORTCUTS(' + data + ')SHORTCUTS";\n}\n')
