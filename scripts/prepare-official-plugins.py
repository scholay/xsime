#!/usr/bin/env python3
"""Import pinned official plugin sources without overwriting local edits.

Run `git submodule update --init OfficialPlugins` first. Maintainers update the
gitlink and use --update-lock after reviewing a new plugin commit. Generated
files stay at the native build paths so private host types never become a
downloadable ABI. Runtime packages contain data, not these native sources.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def canonical(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def cpp_string(text):
    # MSVC limits each source literal to 16,380 bytes. Adjacent small literals
    # preserve the exact UTF-8 package bytes, including the full license text.
    return "\n".join(json.dumps(text[i:i + 2048], ensure_ascii=False)
                     for i in range(0, len(text), 2048)) or '""'


def safe_path(root, name):
    if not isinstance(name, str) or not name or "\\" in name or PurePosixPath(name).is_absolute():
        raise ValueError("Invalid plugin source path")
    current = root
    for component in name.split("/"):
        if component in ("", ".", ".."):
            raise ValueError("Plugin source path escapes its root")
        current /= component
        if current.is_symlink():
            raise ValueError("Plugin source paths must not contain symlinks")
    return current


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".plugin-", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        temporary.chmod(0o644)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def prepare(root, source, update_lock=False):
    lock_path = root / "plugins.lock.json"
    receipt_path = root / ".build/official-plugin-imports.json"
    if not (source / "source-map.json").is_file():
        raise ValueError("Initialize the pinned dependency: git submodule update --init OfficialPlugins")
    source_map = json.loads((source / "source-map.json").read_bytes())
    if source_map.get("schemaVersion") != 1:
        raise ValueError("Unsupported plugin source map")
    imports = {}
    input_names = {"VERSION", "source-map.json", "LICENSE", "NOTICE", "LICENSES/MIT-legacy.txt", "LICENSES/Swift-Apache-2.0.txt", "THIRD_PARTY_NOTICES.md"}
    for entry in source_map["files"]:
        origin, destination = entry["source"], entry["destination"]
        if not origin.startswith("native/") or not destination.startswith(("Sources/", "Shared/Sources/", "Shared/TypingPractice/", "platforms/")):
            raise ValueError("Plugin import is outside an allowed native source directory")
        if destination in imports:
            raise ValueError("Duplicate plugin import destination")
        safe_path(root, destination)
        data = safe_path(source, origin).read_bytes()
        imports[destination] = data
        input_names.add(origin)
    packages = []
    release_version = (source / "VERSION").read_text(encoding="utf-8").strip()
    for path in sorted((source / "plugins").glob("*/plugin.json")):
        input_names.add(path.relative_to(source).as_posix())
        package = json.loads(path.read_bytes())
        package = {**package, "licenseText": (source / "LICENSE").read_text(encoding="utf-8"),
                   "notice": (source / "NOTICE").read_text(encoding="utf-8")}
        data = canonical(package)
        asset = f"preset-plugin-{package['id']}-{package['version']}.json"
        packages.append({**package, "sha256": digest(data), "downloadAssetName": asset,
                         "downloadURL": f"https://github.com/scholay/rimes-plugins/releases/download/v{release_version}/{asset}"})
        imports[f"Shared/Sources/RimesCore/Resources/OfficialPlugins/{asset}"] = data
        if "android" in package["platforms"]:
            imports[f"platforms/android/app/src/main/assets/official-plugins/{asset}"] = data
        # Compatibility contents let an existing user retain installed plugins
        # offline after migration. Fresh profiles still require an install receipt.
        if "macos" in package["platforms"]:
            destination = f"Sources/RimeBuffer/Resources/OfficialPlugins/{package['id']}.json"
            safe_path(root, destination)
            imports[destination] = canonical(package)
    if packages:
        catalog = {"schemaVersion": 1, "releaseVersion": release_version, "plugins": packages}
        imports["Shared/Sources/RimesCore/Resources/OfficialPlugins/catalog.json"] = canonical(catalog)
        for platform, destination in [
            ("android", "platforms/android/app/src/main/assets/official-plugins/catalog.json"),
            ("windows", "platforms/windows/native/resources/official-plugin-catalog.json")
        ]:
            imports[destination] = canonical({**catalog, "plugins": [p for p in packages if platform in p["platforms"]]})
    windows = [entry for entry in packages if "windows" in entry["platforms"]]
    if windows:
        header = "// Generated from the pinned official plugin catalog. Do not edit.\n#pragma once\n#include <string_view>\nnamespace rimes::windows::official {\n"
        header += 'inline constexpr std::string_view kCatalogJSON =\n' + cpp_string(canonical({"schemaVersion": 1, "plugins": windows}).decode()) + ';\n'
        header += "struct BundledPackage { std::string_view id, bytes; };\ninline constexpr BundledPackage kPackages[] = {\n"
        for entry in windows:
            package = {key: value for key, value in entry.items() if key not in ("sha256", "downloadAssetName", "downloadURL")}
            data = canonical(package).decode()
            header += '{' + cpp_string(entry["id"]) + ',\n' + cpp_string(data) + '},\n'
        header += "};\n}\n"
        imports["platforms/windows/native/src/workbench/official_plugin_catalog.generated.hpp"] = header.encode()
    # Keep unsupported desktop entries visible as unavailable. The Linux host
    # additionally checks its compiled adapters before accepting any package.
    linux = [entry for entry in packages if "macos" in entry["platforms"] or "linux" in entry["platforms"]]
    if linux:
        header = "// Generated from the pinned official plugin catalog. Do not edit.\n#pragma once\n#include <string_view>\nnamespace rimes::linuxruntime::official {\n"
        # The complete license-bearing catalog exceeds GCC's constexpr strlen
        # limit. An array preserves the exact bytes without evaluating strlen
        # at compile time or increasing compiler resource limits.
        header += 'inline constexpr char kCatalogJSON[] =\n' + cpp_string(canonical({"schemaVersion": 1, "plugins": linux}).decode()) + ';\n'
        header += "struct BundledPackage { std::string_view id, bytes; };\ninline constexpr BundledPackage kPackages[] = {\n"
        for entry in linux:
            package = {key: value for key, value in entry.items() if key not in ("sha256", "downloadAssetName", "downloadURL")}
            header += '{' + cpp_string(entry["id"]) + ',\n' + cpp_string(canonical(package).decode()) + '},\n'
        header += "};\n}\n"
        imports["platforms/linux/runtime/generated/official_plugin_catalog.hpp"] = header.encode()
    inputs = {name: digest(safe_path(source, name).read_bytes()) for name in sorted(input_names)}
    # Source tarballs (distro packaging) carry no git metadata; the file digests
    # below still pin every input, so the revision may be supplied explicitly.
    if not update_lock and not (source / ".git").exists() and os.environ.get("RIMES_OFFICIAL_PLUGINS_REVISION"):
        revision = os.environ["RIMES_OFFICIAL_PLUGINS_REVISION"]
    else:
        revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True, encoding="utf-8").strip()
    if update_lock:
        dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"], text=True, encoding="utf-8")
        if dirty:
            raise ValueError("Commit reviewed changes in the plugin repository before updating its source lock")
        subprocess.run(["git", "-C", str(source), "ls-files", "--error-unmatch", *sorted(input_names)],
                       check=True, stdout=subprocess.DEVNULL)
    expected_lock = {"schemaVersion": 1, "repository": "https://github.com/scholay/rimes-plugins",
                     "revision": revision, "files": inputs}
    if not update_lock:
        if not lock_path.is_file() or json.loads(lock_path.read_bytes()) != expected_lock:
            raise ValueError("Official plugin dependency differs from plugins.lock.json; review it before --update-lock")

    # Validate every destination before changing any file. An earlier import is
    # replaceable only while its bytes still match the recorded receipt.
    previous = json.loads(receipt_path.read_bytes()) if receipt_path.is_file() else {}
    for name, data in imports.items():
        path = safe_path(root, name)
        if path.exists():
            current = digest(path.read_bytes())
            if current not in (digest(data), previous.get(name)):
                raise ValueError(f"Preserved local edit in {name}; move the change to OfficialPlugins first")
    removed = set(previous) - set(imports)
    for name in removed:
        if not name.startswith(("Sources/", "Shared/Sources/", "Shared/TypingPractice/", "platforms/")):
            raise ValueError("Invalid retired import path in receipt")
        path = safe_path(root, name)
        if path.exists() and digest(path.read_bytes()) != previous[name]:
            raise ValueError(f"Preserved local edit in retired plugin source {name}")
    for name, data in imports.items():
        path = safe_path(root, name)
        if not path.exists() or path.read_bytes() != data:
            atomic_write(path, data)
    for name in removed:
        safe_path(root, name).unlink(missing_ok=True)
    atomic_write(receipt_path, canonical({name: digest(data) for name, data in imports.items()}))
    if update_lock:
        atomic_write(lock_path, canonical(expected_lock))
    return len(imports)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update-lock", action="store_true")
    args = parser.parse_args()
    try:
        count = prepare(ROOT, ROOT / "OfficialPlugins", args.update_lock)
        print(f"Prepared {count} pinned official plugin source files.")
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Official plugin preparation failed: {error}\n")
