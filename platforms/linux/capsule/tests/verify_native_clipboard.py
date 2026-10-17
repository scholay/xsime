#!/usr/bin/env python3
"""Keep clipboard semantic fixtures tied to actual macOS model execution."""
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
fixture = json.loads((HERE / "fixtures/native-clipboard.json").read_text())
for name, expected in fixture["sources"].items():
    if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != expected:
        raise SystemExit(f"Native clipboard fixtures are stale for {name}; run RimeBuffer clipboard-linux-fixtures on macOS")
print("Native clipboard fixture source hashes match")
