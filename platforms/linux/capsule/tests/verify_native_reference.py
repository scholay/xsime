#!/usr/bin/env python3
"""Require actual macOS production formatter output after native changes."""
import hashlib
import json
from pathlib import Path
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
for name, expected in json.loads((HERE / "fixtures/native-reference.json").read_text())["sources"].items():
    if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != expected:
        raise SystemExit(f"Native reference fixtures stale for {name}; run RimeBuffer capsule-reference-linux-fixtures on macOS")
print("Native reference fixture hashes match")
