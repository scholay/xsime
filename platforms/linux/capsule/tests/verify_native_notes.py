#!/usr/bin/env python3
"""Keep Linux grammar fixtures tied to native sources; regenerate only with AppKit."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
FIXTURE = HERE / "fixtures/native-notes.json"
SOURCES = [ROOT / "Sources/RimeBuffer" / name for name in (
    "CapsuleStructure.swift", "CapsuleBulletEditor.swift", "CapsuleRichTextProjection.swift")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regenerate", action="store_true")
    parser.add_argument("--readback", type=Path, help="JSON emitted by rimes-capsule-notes-tests")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="xsime-native-notes-") as temp:
        binary = Path(temp) / "native-notes"
        if args.regenerate:
            subprocess.run(["xcrun", "swiftc", *map(str, SOURCES),
                str(HERE / "native_notes_fixtures.swift"), "-o", str(binary)], check=True)
            output = Path(temp) / "native-notes.json"
            subprocess.run([str(binary), str(ROOT), str(output)], check=True)
            staged = FIXTURE.with_suffix(".tmp")
            staged.write_bytes(output.read_bytes())
            staged.replace(FIXTURE)
        fixture = json.loads(FIXTURE.read_bytes())
        for source in SOURCES:
            if hashlib.sha256(source.read_bytes()).hexdigest() != fixture["sources"][source.name]:
                raise SystemExit(f"Native notes fixtures are stale for {source.name}; regenerate on macOS")
        if args.readback:
            subprocess.run(["xcrun", "swiftc", str(SOURCES[-1]),
                str(HERE / "verify_linux_rich_text.swift"), "-o", str(binary)], check=True)
            subprocess.run([str(binary), str(args.readback.resolve())], check=True)
    print("Native Capsule note fixture source hashes match")


if __name__ == "__main__":
    main()
