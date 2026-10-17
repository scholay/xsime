#!/usr/bin/env python3
"""File ownership for source installs; never manage the Rime user directory."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[4]
RECEIPT = "share/rimes/ime-install-manifest.json"
PROGRAMS = {"rimes-buffer", "rimes-buffer-ctl", "rimes-capsule",
            "rimes-capsule-ctl", "rimes-schemes", "rimes-plugins", "rimes-settings", "rimes-translation", "rimes-launch"}


def managed_path(raw):
    """Constrain even an edited manifest to installed application resources."""
    if not isinstance(raw, str) or not raw or any(c in raw for c in "\x00\r\n"):
        raise ValueError(f"Invalid managed path: {raw!r}")
    path = PurePosixPath(raw)
    if path.is_absolute() or any(p in {"", ".", ".."} for p in raw.split("/")):
        raise ValueError(f"Unsafe managed path: {raw}")
    allowed = (
        (path.parent == PurePosixPath("bin") and path.name in PROGRAMS)
        or (path.parent == PurePosixPath("libexec/rimes")
            and path.name in {"rimes-buffer", "rimes-capsule", "rimes-deploy", "rimes-capsule-preview", "rimes-capsule-capture"})
        or (path.parent == PurePosixPath("libexec/rimes/translation")
            and path.name in {"runtime.py", "translation_core.py", "resources.lock.json", "NOTICE.md"})
        or re.fullmatch(r"lib(?:64)?/(?:[^/]+/)?fcitx5/rimes\.so", raw)
        or raw in {"share/fcitx5/addon/rimes.conf", "share/fcitx5/inputmethod/rimes.conf",
                   "share/metainfo/org.fcitx.Fcitx5.Addon.Rimes.metainfo.xml",
                   "share/applications/org.scholay.rimes.schemes.desktop",
                   "share/applications/org.scholay.rimes.settings.desktop",
                   "share/applications/org.scholay.rimes.buffer.desktop",
                   "share/applications/org.scholay.rimes.capsule.desktop"}
        or re.fullmatch(r"share/icons/hicolor/[^/]+/apps/rimes\.(?:png|svg)", raw)
        or raw.startswith("share/licenses/rimes/")
        or raw.startswith("share/rimes/data/")
    )
    if not allowed or any(p in {"build", "userdb", "schemes", "learning"}
                          or ".userdb" in p or p.endswith(".bin") for p in path.parts):
        raise ValueError(f"Not an installed RIMES application file: {raw}")
    return raw


def checked_file(root, relative):
    """Do not follow a replacement file or directory symlink inside the prefix."""
    path = root
    for part in PurePosixPath(relative).parts:
        path = path / part
        if path.is_symlink():
            raise ValueError(f"Refusing a symlink in installation: {path}")
    if path.exists() and not path.is_file():
        raise ValueError(f"Expected a regular installed file: {path}")
    return path


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def read_receipt(root, prefix):
    path = checked_file(root, RECEIPT)
    if not path.exists():
        return None
    value = json.loads(path.read_text())
    if (not isinstance(value, dict) or value.get("formatVersion") != 1
            or value.get("prefix") != prefix or not isinstance(value.get("files"), dict)):
        raise ValueError(f"Invalid installation manifest or wrong prefix: {path}")
    for relative, checksum in value["files"].items():
        managed_path(relative)
        if not isinstance(checksum, str) or not re.fullmatch(r"[0-9a-f]{64}", checksum):
            raise ValueError(f"Invalid installed checksum: {relative}")
    return value["files"]


def write_receipt(root, prefix, files):
    path = checked_file(root, RECEIPT)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=".ime-manifest-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as output:
            json.dump({"formatVersion": 1, "prefix": prefix, "files": files},
                      output, indent=2, sort_keys=True)
            output.write("\n")
        os.chmod(name, 0o644)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def install(root, prefix, stage):
    staged_prefix = stage / prefix.lstrip("/")
    sources = {}
    # Inspect the whole DESTDIR so a CMake absolute destination outside the
    # selected prefix cannot silently escape the ownership manifest.
    for source in sorted(stage.rglob("*")):
        if source.is_symlink():
            raise ValueError(f"Unexpected staged symlink: {source}")
        if source.is_dir():
            continue
        relative = managed_path(source.relative_to(staged_prefix).as_posix())
        if not source.is_file():
            raise ValueError(f"Unexpected staged file type: {source}")
        checked_file(root, relative)
        sources[relative] = source
    if not sources:
        raise ValueError("Empty installation payload")
    files = read_receipt(root, prefix) or {}
    files.update({relative: digest(source) for relative, source in sources.items()})
    # Record before copying, so an interrupted first install remains discoverable.
    # Keep older entries too: an upgrade must not orphan retired application files.
    write_receipt(root, prefix, files)
    for relative, source in sources.items():
        destination = checked_file(root, relative)
        destination.parent.mkdir(parents=True, exist_ok=True)
        fd, name = tempfile.mkstemp(prefix=".rimes-install-", dir=destination.parent)
        os.close(fd)
        try:
            shutil.copyfile(source, name)
            os.chmod(name, stat.S_IMODE(source.stat().st_mode))
            os.replace(name, destination)
        finally:
            if os.path.exists(name):
                os.unlink(name)
    print(f"Recorded {len(files)} installed files in {root / RECEIPT}")


def legacy_files(prefix, build_dir):
    """Old CMake manifests cover programs; policy covers the extra data copy."""
    manifest = build_dir / "install_manifest.txt"
    files = {}
    for line in manifest.read_text().splitlines():
        if not line:
            continue
        path = PurePosixPath(line)
        if not path.is_absolute():
            raise ValueError(f"Non-absolute path in {manifest}: {line}")
        relative = managed_path(path.relative_to(prefix).as_posix())
        files[relative] = None  # Old installers did not record binary checksums.
    if not files:
        raise ValueError(f"Empty legacy install manifest: {manifest}")
    policy = json.loads((REPO_ROOT / "scripts/platform-preview/policy.json").read_text())
    for relative in policy["include"]:
        target = managed_path("share/rimes/data/" + relative)
        files[target] = digest(REPO_ROOT / "rime-data" / relative)
    print("Using legacy CMake file list; shared data must match this source checkout.")
    return files


def uninstall(root, prefix, build_dir, dry_run):
    files = read_receipt(root, prefix)
    recorded = files is not None
    if files is None:
        if build_dir is None:
            print("No source-install manifest found. For an older install, use --build-dir DIR; "
                  "for a deb, use sudo apt remove fcitx5-rimes.")
            return
        files = legacy_files(prefix, build_dir)
    pending = []
    modified = []
    # Validate the entire plan before removing anything, including the receipt.
    for relative, checksum in sorted(files.items()):
        path = checked_file(root, relative)
        if not path.exists():
            continue
        if checksum is not None and digest(path) != checksum:
            modified.append(str(path))
        pending.append((relative, path.stat()))
    if modified:
        raise ValueError("Modified installed files were preserved; nothing removed. "
                         "Move or restore these files and retry:\n  " + "\n  ".join(modified))
    for relative, _ in pending:
        print(f"{'Would remove' if dry_run else 'Remove'}: {root / relative}")
    if dry_run:
        if recorded:
            print(f"Would remove: {root / RECEIPT}")
        print("Dry run: no files changed.")
        return
    parents = set()
    for relative, before in pending:
        path = checked_file(root, relative)
        after = path.stat()
        if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError(f"Installed file changed during uninstall; stopped: {path}")
        path.unlink()
        parents.update(parent for parent in path.parents if parent != root and root in parent.parents)
    if recorded:
        checked_file(root, RECEIPT).unlink()
    for parent in sorted(parents, key=lambda p: len(p.parts), reverse=True):
        if parent == root / "share/rimes":
            continue  # This is also the default per-user Rime directory.
        try:
            parent.rmdir()  # Never recursively delete shared or user directories.
        except OSError:
            pass
    print("RIMES application files removed. User dictionaries, learning records, "
          "imported schemes and notes were preserved.")
    print('Remove RIMES from the Fcitx5 input-method list, then restart Fcitx5 (fcitx5 -r).')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "uninstall"))
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--destdir", default=os.environ.get("DESTDIR", ""))
    parser.add_argument("--stage", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    try:
        if (not args.prefix.startswith("/") or ".." in Path(args.prefix).parts
                or any(c in args.prefix for c in "\x00\r\n")):
            raise ValueError("--prefix must be an absolute path without '..' or newlines")
        prefix = str(Path(args.prefix))
        if prefix == "/":
            raise ValueError("Refusing installation prefix /")
        if args.destdir and not Path(args.destdir).is_absolute():
            raise ValueError("DESTDIR must be an absolute path")
        root = Path(args.destdir + prefix).resolve()
        if args.action == "install":
            if args.stage is None or args.build_dir is None:
                raise ValueError("install requires --stage and --build-dir")
            cache = (args.build_dir / "CMakeCache.txt").read_text()
            configured = re.search(r"^CMAKE_INSTALL_PREFIX:[^=]+=(.+)$", cache, re.M)
            if configured is None or str(Path(configured[1])) != prefix:
                raise ValueError("Installation prefix differs from the build; rebuild with this prefix "
                                 "(omit --skip-build)")
            install(root, prefix, args.stage)
        else:
            uninstall(root, prefix, args.build_dir, args.dry_run)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
