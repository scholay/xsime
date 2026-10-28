#!/usr/bin/env python3
"""Recover immutable producer artifacts for a newly approved publication attempt.

The recovery workflow independently requires macos-publish approval. Producers,
including signing and exact-package installation, must already have succeeded.
This module never signs, executes packages, modifies a Release or moves Latest.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import zipfile

from release_guard import (GitHub, REPO, require, source_commit, stable_version,
                           select_artifact, verify_ci)

PUBLISHER = "Approve and publish complete stable release"
REQUIRED_JOBS = {
    "Build and runtime-smoke macOS without secrets",
    "Sign, verify, and stage macOS",
    "Install the exact signed package on a disposable Mac",
    "sign_update_manifest / Sign complete update manifest",
    "build_linux_deb / Ubuntu 24.04 amd64 deb installation and removal",
    "build_windows_release / Release x64",
    "build_windows_release / Release x86",
    "build_windows_release / Complete Windows installer",
    "build_windows_release / IME typing x64",
    "build_windows_release / IME typing x86",
    "build_android_release / build",
    "build_android_release / sign-release",
}


def authorize(run, jobs, artifacts, release, latest, tag_commit):
    require(run.get("repository", {}).get("full_name") == REPO and
            run.get("head_repository", {}).get("full_name") == REPO, "Wrong source repository")
    require(run.get("path") == ".github/workflows/release.yml" and run.get("event") == "push",
            "Source must be the canonical tag workflow")
    require(run.get("status") == "completed" and run.get("conclusion") in {"success", "failure", "cancelled"},
            "Source workflow is unfinished")
    tag, commit = run["head_branch"], source_commit(run["head_sha"])
    require(tag.startswith("v"), "Stable tag required")
    version = tag[1:]; target = stable_version(version)
    require(tag_commit == commit, "Tag no longer identifies the verified source")
    by_name = {job["name"]: job for job in jobs}
    require(len(by_name) == len(jobs), "Ambiguous source job names")
    for name in REQUIRED_JOBS:
        job = by_name.get(name, {})
        require(job.get("status") == "completed" and job.get("conclusion") == "success",
                f"Required producer did not pass: {name}")
    require(all(job.get("conclusion") in {"success", "skipped"} or job["name"] == PUBLISHER for job in jobs),
            "A source job failed outside the publication transaction")
    if release is not None:
        require(release.get("tag_name") == tag and release.get("prerelease") is False,
                "Existing release is not the expected stable tag")
        # Source/asset marker and every byte are checked by the common publisher.
    if latest is not None:
        require(stable_version(latest["tag_name"][1:]) <= target,
                "A newer stable release already exists")
    names = {
        "windows-release": f"RIMES-Windows-release-{commit}",
        "android-release": f"RIMES-Android-release-{commit}",
    }
    prefixes = {
        "rimes-signed-release-stage": f"RIMES-{version}-signed-stage-{run['id']}-",
        "signed-update-feed": f"RIMES-{version}-signed-update-feed-{run['id']}-",
        "linux-deb": f"rimes-linux-deb-{run['id']}-",
    }
    for destination, prefix in prefixes.items():
        matches = [artifact["name"] for artifact in artifacts
                   if re.fullmatch(re.escape(prefix) + r"[1-9]\d*", artifact.get("name", ""))]
        require(len(matches) == 1, f"Missing or ambiguous original artifact: {prefix}*")
        names[destination] = matches[0]
    selected = {destination: select_artifact(artifacts, name, run["id"], commit)
                for destination, name in names.items()}
    return {"version": version, "commit": commit, "tag": tag, "source_run": run["id"], "artifacts": selected}


def extract_verified(archive, destination, digest):
    with archive.open("rb") as stream:
        require(hashlib.file_digest(stream, "sha256").hexdigest() == digest,
                "Artifact archive digest mismatch")
    with zipfile.ZipFile(archive) as bundle:
        members = bundle.infolist()
        require(0 < len(members) <= 32 and sum(member.file_size for member in members) <= 1024 * 1024 * 1024,
                "Unexpected artifact size or member count")
        names = set()
        for member in members:
            name, mode = member.filename, member.external_attr >> 16
            require(name not in names and name not in {"", ".", ".."} and
                    "/" not in name and "\\" not in name and
                    stat.S_IFMT(mode) in {0, stat.S_IFREG} and not member.is_dir(),
                    "Artifact must contain unique regular files at its root")
            names.add(name)
        destination.mkdir()
        for member in members:
            with bundle.open(member) as source, (destination / member.filename).open("xb") as target:
                shutil.copyfileobj(source, target)


def download_verified(artifact, destination):
    archive = destination.parent / (destination.name + "-artifact.zip")
    with archive.open("xb") as stream:
        subprocess.run(["gh", "api", f"repos/{REPO}/actions/artifacts/{artifact['id']}/zip"],
                       stdout=stream, check=True)
    require(archive.stat().st_size == artifact["size_in_bytes"], "Artifact ZIP byte count changed")
    extract_verified(archive, destination, artifact["digest"][7:])


def verify_recovery_context(run, environ=None):
    environ = os.environ if environ is None else environ
    tag = run.get("head_branch", "")
    require(tag.startswith("v"), "Stable source tag required")
    stable_version(tag[1:])
    commit = source_commit(run.get("head_sha"))
    expected = {"GITHUB_REPOSITORY": REPO, "GITHUB_EVENT_NAME": "workflow_dispatch",
                "GITHUB_REF": "refs/tags/" + tag, "GITHUB_REF_NAME": tag, "GITHUB_SHA": commit}
    require(all(environ.get(name) == value for name, value in expected.items()),
            "Recovery must be dispatched at the original stable tag and source commit")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-id", type=int, required=True)
    args = parser.parse_args()
    require(args.run_id > 0, "Positive source run ID required")
    github = GitHub(); prefix = f"repos/{REPO}"
    run = github.json(f"{prefix}/actions/runs/{args.run_id}")
    verify_recovery_context(run)
    tag = run["head_branch"]
    require(re.fullmatch(r"v\d+\.\d+\.\d+", tag), "Stable source tag required")
    plan = authorize(
        run, github.pages(f"{prefix}/actions/runs/{args.run_id}/jobs?per_page=100", "jobs"),
        github.pages(f"{prefix}/actions/runs/{args.run_id}/artifacts?per_page=100", "artifacts"),
        github.json(f"{prefix}/releases/tags/{tag}", missing_ok=True),
        github.json(f"{prefix}/releases/latest", missing_ok=True),
        github.json(f"{prefix}/commits/{tag}")["sha"],
    )
    verify_ci(plan["commit"], github, require_tip=False)
    root = Path(os.environ["RUNNER_TEMP"])
    (root / "release-completion-evidence.json").write_text(json.dumps(plan, indent=2) + "\n")
    for destination, artifact in plan["artifacts"].items():
        download_verified(artifact, root / destination)
    with Path(os.environ["GITHUB_OUTPUT"]).open("a") as stream:
        for key in ("version", "commit", "tag", "source_run"):
            stream.write(f"{key}={plan[key]}\n")
    print(f"Authenticated producer run {args.run_id}, tag {tag}, commit {plan['commit']}")


if __name__ == "__main__":
    main()
