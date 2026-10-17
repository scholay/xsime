#!/usr/bin/env python3
"""Require fresh production macOS password, passcode and TOTP fixtures."""
import hashlib
import json
from pathlib import Path
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
for name, expected in json.loads((HERE / "fixtures/native-password.json").read_text())["sources"].items():
    if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != expected:
        raise SystemExit(f"Native password fixtures stale for {name}; run RimeBuffer capsule-password-linux-fixtures on macOS")
print("Native password fixture hashes match")
