#!/usr/bin/env python3
"""Resume an authenticated draft, verify all assets, then publish exactly once."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from release_guard import GitHub, REPO, require, source_commit, stable_version, verify_android_build

MAX_ASSET = 512 * 1024 * 1024
IDENTITY_PREFIX = "<!-- xsime-release-source:"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def regular_file(path, maximum=MAX_ASSET):
    require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= maximum,
            f"Missing, unsafe or oversized release file: {path.name}")
    return path


def json_file(path):
    return json.loads(regular_file(path, 2 * 1024 * 1024).read_text(encoding="utf-8-sig"))


def verify_checksum_file(path, files):
    expected = {file.name: digest(file) for file in files}
    rows = regular_file(path, 1024 * 1024).read_text().splitlines()
    actual = {}
    for row in rows:
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/\\]+)", row)
        require(match and match[2] not in actual, f"Invalid checksum file: {path.name}")
        actual[match[2]] = match[1]
    require(actual == expected, f"Package checksum mismatch: {path.name}")


def verify_mac_stage(mac, version, commit, run_id):
    require(mac.is_dir() and not mac.is_symlink(), "Unsafe signed installer artifact root")
    package = regular_file(mac / f"RIMES-{version}.pkg")
    notes = regular_file(mac / "RELEASE-NOTES.md", 2 * 1024 * 1024)
    verify_checksum_file(mac / "SHA256SUMS", [package])
    expected_stage = {
        "schema": "rimes-signed-pkg-release-v1", "repository": REPO, "run_id": str(run_id),
        "commit": commit, "tag": "v" + version, "version": version, "pkg": package.name,
        "pkg_sha256": digest(package), "pkg_bytes": str(package.stat().st_size),
        "notes": notes.name, "notes_sha256": digest(notes), "notes_bytes": str(notes.stat().st_size),
    }
    stage_rows = regular_file(mac / "release-manifest.txt", 8192).read_text().splitlines()
    require(len(stage_rows) == len(expected_stage) and all("=" in row for row in stage_rows),
            "Invalid signed stage manifest")
    require(dict(row.split("=", 1) for row in stage_rows) == expected_stage,
            "Signed stage source or byte identity mismatch")

    return package, notes


def prepare_assets(root, version, commit, run_id, verify_feed=None):
    """Only read/copy package bytes; never execute a downloaded package."""
    stable_version(version); source_commit(commit)
    require(isinstance(run_id, int) and run_id > 0, "Source workflow run required")
    mac, linux = root / "rimes-signed-release-stage", root / "linux-deb"
    windows, android = root / "windows-release", root / "android-release"
    feed = root / "signed-update-feed" / "update-manifest.json"
    for folder in (mac, linux, windows, android, feed.parent):
        require(folder.is_dir() and not folder.is_symlink(), "Unsafe artifact root")
    package, notes = verify_mac_stage(mac, version, commit, run_id)

    win_info = json_file(windows / "BUILD-INFO.json")
    android_info = json_file(android / f"RIMES-Android-{version}-BUILD-INFO.json")
    for info in (win_info, android_info):
        require(info.get("version") == version and info.get("sourceCommit") == commit,
                "Platform package source/version mismatch")
    code = android_info.get("versionCode")
    require(isinstance(code, int) and not isinstance(code, bool) and code > 0, "Invalid Android build identity")
    sources = {package.name: package, "SHA256SUMS": mac / "SHA256SUMS"}
    deb = regular_file(linux / f"fcitx5-rimes_{version}_amd64.deb")
    verify_checksum_file(linux / (deb.name + ".sha256"), [deb])
    sources.update({deb.name: deb, deb.name + ".sha256": linux / (deb.name + ".sha256")})
    for name in (f"RIMES-Windows-{version}-Setup.exe", f"RIMES-Windows-{version}.zip"):
        sources[name] = regular_file(windows / name)
    verify_checksum_file(windows / "SHA256SUMS",
                         [sources[f"RIMES-Windows-{version}-Setup.exe"], sources[f"RIMES-Windows-{version}.zip"]])
    for name in ("BUILD-INFO.json", "SOURCE-FILES.json", "SETUP-VERIFICATION.json", "SHA256SUMS"):
        sources[f"RIMES-Windows-{version}-{name}"] = regular_file(windows / name)
    for suffix in (".apk", ".aab", "-SHA256SUMS", "-BUILD-INFO.json"):
        name = f"RIMES-Android-{version}{suffix}"
        sources[name] = regular_file(android / name)
    verify_checksum_file(android / f"RIMES-Android-{version}-SHA256SUMS",
                         [sources[f"RIMES-Android-{version}.apk"], sources[f"RIMES-Android-{version}.aab"]])
    document = json_file(feed)
    if verify_feed is None:
        from update_signing import verify_manifest
        verify_feed = verify_manifest
    payload = verify_feed(document)
    require(payload.get("version") == version and payload.get("sourceCommit") == commit,
            "Signed update feed source/version mismatch")
    require(payload.get("platforms", {}).get("android", {}).get("build") == code,
            "Signed update feed Android build mismatch")
    targets = {"macos": package.name, "linux": deb.name,
               "windows": f"RIMES-Windows-{version}-Setup.exe", "android": f"RIMES-Android-{version}.apk"}
    for platform, name in targets.items():
        target = payload.get("platforms", {}).get(platform, {})
        file = sources[name]
        require(target.get("url") == f"https://github.com/{REPO}/releases/download/v{version}/{name}"
                and target.get("size") == file.stat().st_size and target.get("sha256") == digest(file),
                f"Signed update feed does not identify the staged {platform} package")
    sources[feed.name] = feed
    output = root / "application-release"
    output.mkdir(exist_ok=True)
    require(not output.is_symlink(), "Unsafe public asset directory")
    for name, file in sources.items():
        destination = output / name
        if destination.exists() or destination.is_symlink():
            require(regular_file(destination).stat().st_size == file.stat().st_size
                    and digest(destination) == digest(file), "Existing staged public asset differs")
        else:
            shutil.copyfile(file, destination)
    require({path.name for path in output.iterdir()} == set(sources), "Unexpected staged public asset")
    assets = {name: output / name for name in sources}
    identity = {"repository": REPO, "runID": run_id, "sourceCommit": commit, "version": version,
                "assets": {name: {"sha256": digest(path), "size": path.stat().st_size}
                           for name, path in sorted(assets.items())}}
    seal = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    require(IDENTITY_PREFIX not in notes.read_text(), "Release notes contain reserved source marker")
    body = notes.read_text().rstrip() + f"\n\n{IDENTITY_PREFIX} run={run_id} commit={commit} version={version} assets={seal} -->\n"
    return assets, body, code


class ReleaseGitHub(GitHub):
    def release(self, tag):
        return self.json(f"repos/{REPO}/releases/tags/{tag}", missing_ok=True)

    def create_draft(self, tag, commit, body):
        return self.json(f"repos/{REPO}/releases", method="POST", data={
            "tag_name": tag, "target_commitish": commit, "name": "犀术IME / 犀術IME / XSIME " + tag[1:],
            "body": body, "draft": True, "prerelease": False, "make_latest": "false"})

    def upload(self, tag, path):
        subprocess.run(["gh", "release", "upload", tag, str(path), "--repo", REPO], check=True)

    def download(self, asset, path):
        require(asset.get("state") == "uploaded" and isinstance(asset.get("id"), int)
                and 0 < asset.get("size", 0) <= MAX_ASSET, "Invalid public asset metadata")
        with path.open("xb") as stream:
            subprocess.run(["gh", "api", "-H", "Accept: application/octet-stream",
                            f"repos/{REPO}/releases/assets/{asset['id']}"], stdout=stream, check=True)

    def promote(self, release_id):
        return self.json(f"repos/{REPO}/releases/{release_id}", method="PATCH",
                         data={"draft": False, "prerelease": False, "make_latest": "true"})


def verify_release_identity(release, tag, body):
    require(release.get("tag_name") == tag and release.get("prerelease") is False
            and release.get("body", "").rstrip() == body.rstrip(),
            "Existing release is not this authenticated source/asset set")
    require(isinstance(release.get("id"), int), "Release ID missing")


def verify_remote_asset(github, asset, path, readback):
    require(asset.get("size") == path.stat().st_size and asset.get("state") == "uploaded",
            f"Published asset size/state mismatch: {path.name}")
    destination = readback / path.name
    github.download(asset, destination)
    require(destination.stat().st_size == path.stat().st_size and digest(destination) == digest(path),
            f"Published asset bytes differ: {path.name}")
    destination.unlink()


def publish(github, version, commit, code, assets, body, readback, monotonic_guard=None):
    tag = "v" + version
    monotonic_guard = monotonic_guard or (lambda: verify_android_build(version, code, github, allow_existing=True))
    require(github.json(f"repos/{REPO}/commits/{tag}")["sha"] == commit,
            "Release tag moved after the verified build")
    monotonic_guard()
    release = github.release(tag)
    if release is None:
        release = github.create_draft(tag, commit, body)
    verify_release_identity(release, tag, body)
    readback.mkdir(exist_ok=True)
    require(not readback.is_symlink() and not any(readback.iterdir()), "Readback directory must be empty")
    existing_names = [asset.get("name") for asset in release.get("assets", [])]
    require(len(existing_names) == len(set(existing_names)) and set(existing_names) <= set(assets),
            "Release contains unexpected or duplicate assets")
    # Refuse any differing existing byte before adding another missing asset.
    for asset in release.get("assets", []):
        verify_remote_asset(github, asset, assets[asset["name"]], readback)
    for name, path in sorted(assets.items()):
        release = github.release(tag)
        verify_release_identity(release, tag, body)
        matches = [asset for asset in release.get("assets", []) if asset.get("name") == name]
        require(len(matches) <= 1, f"Ambiguous existing asset: {name}")
        if not matches:
            require(release.get("draft") is True, "A public release is incomplete; do not modify published assets")
            github.upload(tag, path)
            release = github.release(tag)
            matches = [asset for asset in release.get("assets", []) if asset.get("name") == name]
            require(len(matches) == 1, f"Upload did not produce exactly one asset: {name}")
        verify_remote_asset(github, matches[0], path, readback)
    release = github.release(tag)
    verify_release_identity(release, tag, body)
    require({asset["name"] for asset in release.get("assets", [])} == set(assets),
            "Release contains an unexpected asset; refusing publication")
    # Every package AND the signed feed have been read back before Latest moves.
    monotonic_guard()
    require(github.json(f"repos/{REPO}/commits/{tag}")["sha"] == commit, "Release tag moved before publication")
    if release.get("draft"):
        release = github.promote(release["id"])
    verify_release_identity(release, tag, body)
    require(release.get("draft") is False, "Release was not published")
    latest = github.json(f"repos/{REPO}/releases/latest")
    require(latest["id"] == release["id"], "Latest changed concurrently; refusing to retarget an older release")
    return {"releaseID": release["id"], "tag": tag, "sourceCommit": commit,
            "assets": {name: digest(path) for name, path in sorted(assets.items())}}


def main():
    require(os.environ.get("GH_REPO") == REPO, "Canonical release repository required")
    root = Path(os.environ["RUNNER_TEMP"])
    version, commit = os.environ["VERSION"], os.environ["RELEASE_COMMIT"]
    require(os.environ["RELEASE_TAG"] == "v" + version, "Stable tag/version mismatch")
    run_id = int(os.environ.get("SOURCE_RUN_ID", os.environ["GITHUB_RUN_ID"]))
    assets, body, code = prepare_assets(root, version, commit, run_id)
    with tempfile.TemporaryDirectory(prefix="release-readback-", dir=root) as directory:
        evidence = publish(ReleaseGitHub(), version, commit, code, assets, body, Path(directory))
    (root / "release-publication-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"Verified complete publication of v{version} from {commit}")


if __name__ == "__main__":
    main()
