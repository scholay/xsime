#!/usr/bin/env python3
"""Check the exported native gateway rules, then generate a Linux read-only header."""
import argparse
import hashlib
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
DESTINATION = Path(__file__).resolve().parent / "resources/native-gateway.json"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--import-fixture", type=Path)
parser.add_argument("--output", type=Path)
parser.add_argument("--import-settings", type=Path)
args = parser.parse_args()
source = args.import_fixture or DESTINATION
data = json.loads(source.read_text())
if data.get("purpose") != "public-gateway-contract" or data.get("schemaVersion") != 1:
    raise SystemExit("Expected a public native gateway export")
expected = {"Sources/RimeBuffer/Inbound/LocalGateway.swift", "Sources/RimeBuffer/Inbound/InboundBus.swift", "Sources/RimeBuffer/Inbound/GatewayToken.swift"}
if set(data["sources"]) != expected:
    raise SystemExit("Native gateway source map changed; review required")
for path, digest in data["sources"].items():
    if hashlib.sha256((ROOT / path).read_bytes()).hexdigest() != digest:
        raise SystemExit("Native gateway source changed; regenerate and review: " + path)
if [tool["name"] for tool in data["mcp"]["tools"]] != ["buffer_push", "buffer_stream_begin", "buffer_stream_append", "buffer_stream_end"]:
    raise SystemExit("Review native gateway capabilities before compiling")
if args.import_fixture:
    temp = DESTINATION.with_suffix(".json.edit")
    temp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    temp.replace(DESTINATION)
settings_path = DESTINATION.with_name("native-gateway-settings.json")
settings = json.loads((args.import_settings or settings_path).read_text())
if settings.get("purpose") != "public-gateway-settings" or settings.get("schemaVersion") != 1 or set(settings.get("sources", {})) != {"Sources/RimeBuffer/SettingsWindow.swift"}:
    raise SystemExit("Expected actual native gateway settings export")
for path, digest in settings["sources"].items():
    if hashlib.sha256((ROOT / path).read_bytes()).hexdigest() != digest:
        raise SystemExit("Native gateway settings changed; regenerate actual settings render: " + path)
if args.import_settings:
    temp = settings_path.with_suffix(".json.edit")
    temp.write_text(json.dumps(settings, ensure_ascii=False, indent=2) + "\n")
    temp.replace(settings_path)
data["settings"] = settings
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('// Generated from the actual native gateway; do not edit.\n#pragma once\n'
        'namespace rimes::mailbox { inline constexpr char kNativeGateway[] = R"gateway('
        + json.dumps(data, ensure_ascii=False, sort_keys=True) + ')gateway"; }\n')
print("Native gateway protocol, tools, limits and origin contract is current")
