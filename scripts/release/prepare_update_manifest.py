#!/usr/bin/env python3
"""Authenticate immutable package artifacts before the update signing step."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil

from release_guard import GitHub, REPO, require, source_commit, stable_version, select_artifact, verify_ci
from recover_update_assets import download_verified
from publish_update_assets import regular_file, json_file, verify_mac_stage, verify_checksum_file
from update_manifest import manifest


def prepare(directory, version, commit, run_id, macos_name, macos_id, macos_digest, linux_name,
            github=None, download=download_verified):
    github = github or GitHub()
    stable_version(version); source_commit(commit)
    require(run_id > 0 and macos_id > 0 and re.fullmatch(r"[0-9a-f]{64}", macos_digest),
            "Exact signed installer artifact identity required")
    run = github.json(f"repos/{REPO}/actions/runs/{run_id}")
    require(run.get("id") == run_id and run.get("repository", {}).get("full_name") == REPO
            and run.get("head_repository", {}).get("full_name") == REPO
            and run.get("path") == ".github/workflows/release.yml"
            and run.get("event") == "push" and run.get("head_sha") == commit
            and run.get("head_branch") == "v" + version, "Unexpected source tag workflow")
    require(github.json(f"repos/{REPO}/commits/v{version}")["sha"] == commit,
            "Stable tag no longer identifies the source")
    verify_ci(commit, github)
    artifacts = github.pages(f"repos/{REPO}/actions/runs/{run_id}/artifacts?per_page=100", "artifacts")
    names = {"rimes-signed-release-stage": macos_name, "linux-deb": linux_name,
             "windows-release": f"RIMES-Windows-release-{commit}",
             "android-release": f"RIMES-Android-release-{commit}"}
    require(re.fullmatch(f"RIMES-{re.escape(version)}-signed-stage-{run_id}-[1-9][0-9]*", macos_name)
            and re.fullmatch(f"rimes-linux-deb-{run_id}-[1-9][0-9]*", linux_name),
            "Unexpected producer artifact names")
    selected = {destination: select_artifact(artifacts, name, run_id, commit)
                for destination, name in names.items()}
    macos = selected["rimes-signed-release-stage"]
    require(macos["id"] == macos_id and macos["digest"] == "sha256:" + macos_digest,
            "Signed installer handoff identity changed")
    directory.mkdir(parents=True, exist_ok=False)
    for destination, artifact in selected.items():
        download(artifact, directory / destination)
    mac = directory / "rimes-signed-release-stage"
    package, _ = verify_mac_stage(mac, version, commit, run_id)
    windows, android, linux = (directory / name for name in ("windows-release", "android-release", "linux-deb"))
    windows_info = json_file(windows / "BUILD-INFO.json")
    android_info = json_file(android / f"RIMES-Android-{version}-BUILD-INFO.json")
    for info in (windows_info, android_info):
        require(info.get("sourceCommit") == commit and info.get("version") == version,
                "Package build-info source/version mismatch")
    verify_checksum_file(windows / "SHA256SUMS", [
        regular_file(windows / f"RIMES-Windows-{version}-Setup.exe"),
        regular_file(windows / f"RIMES-Windows-{version}.zip")])
    code = android_info.get("versionCode")
    require(isinstance(code, int) and not isinstance(code, bool) and code > 0, "Invalid Android build-info")
    deb = regular_file(linux / f"fcitx5-rimes_{version}_amd64.deb")
    verify_checksum_file(linux / (deb.name + ".sha256"), [deb])
    apk = regular_file(android / f"RIMES-Android-{version}.apk")
    verify_checksum_file(android / f"RIMES-Android-{version}-SHA256SUMS",
                         [apk, regular_file(android / f"RIMES-Android-{version}.aab")])
    packages = directory / "packages"; packages.mkdir()
    for file in (package, deb, apk, regular_file(windows / f"RIMES-Windows-{version}-Setup.exe")):
        shutil.copyfile(file, packages / file.name)
    value = manifest(packages, version, commit, code)
    (directory / "unsigned-update-manifest.json").write_text(json.dumps(value, indent=2) + "\n")
    (directory / "artifact-provenance.json").write_text(json.dumps({
        "sourceRun": run_id, "sourceCommit": commit, "artifacts": selected}, indent=2) + "\n")
    return value



def verify_call_context(version, commit, run_id, environ=None):
    environ = os.environ if environ is None else environ
    expected = {"GITHUB_REPOSITORY": REPO, "GITHUB_EVENT_NAME": "push",
                "GITHUB_REF_NAME": "v" + version, "GITHUB_SHA": commit, "GITHUB_RUN_ID": str(run_id)}
    require(all(environ.get(name) == value for name, value in expected.items()),
            "Signing inputs must belong to this canonical stable tag workflow run")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-run", type=int, required=True)
    parser.add_argument("--macos-artifact-name", required=True)
    parser.add_argument("--macos-artifact-id", type=int, required=True)
    parser.add_argument("--macos-artifact-digest", required=True)
    parser.add_argument("--linux-artifact-name", required=True)
    args = parser.parse_args()
    verify_call_context(args.version, args.source_commit, args.source_run)
    prepare(args.directory, args.version, args.source_commit, args.source_run,
            args.macos_artifact_name, args.macos_artifact_id, args.macos_artifact_digest, args.linux_artifact_name)
    print("Authenticated four native package artifacts; unsigned metadata is ready for protected signing")


if __name__ == "__main__":
    main()
