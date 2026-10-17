#!/usr/bin/env python3
"""Require actual macOS Capture output after production rules change."""
import hashlib
import json
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
for name, expected in json.loads((HERE / "fixtures/native-capture.json").read_text())["sources"].items():
    if hashlib.sha256((ROOT / name).read_bytes()).hexdigest()!=expected:
        raise SystemExit(f"Native capture fixtures stale for {name}; run RimeBuffer capture-linux-fixtures on macOS")
print("Native capture fixture hashes match")
