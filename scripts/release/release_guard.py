#!/usr/bin/env python3
"""Read-only release authority, exact-source CI and monotonic version gates."""
import argparse
import hashlib
import json
import re
import subprocess

REPO = "scholay/xsime"
WORKFLOWS = ("ci.yml", "ios.yml", "android.yml", "windows-ime.yml", "linux-ime.yml")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def stable_version(value):
    require(isinstance(value, str) and re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", value),
            "Stable MAJOR.MINOR.PATCH version required")
    return tuple(map(int, value.split(".")))


def source_commit(value):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{40}", value), "Exact source SHA required")
    return value


class GitHub:
    def json(self, endpoint, *, missing_ok=False, method="GET", data=None):
        command = ["gh", "api", "--method", method, endpoint]
        payload = None
        if data is not None:
            command += ["--input", "-"]
            payload = json.dumps(data).encode()
        result = subprocess.run(command, input=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            if missing_ok and b"HTTP 404" in result.stderr:
                return None
            raise ValueError(f"GitHub request failed: {method} {endpoint}")
        return json.loads(result.stdout)

    def pages(self, endpoint, key=None):
        result = subprocess.run(["gh", "api", "--paginate", "--slurp", endpoint],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        require(result.returncode == 0, f"GitHub pagination failed: {endpoint}")
        pages = json.loads(result.stdout)
        return [item for page in pages for item in (page[key] if key else page)]

    def asset_bytes(self, asset, *, limit=1024 * 1024):
        require(isinstance(asset.get("id"), int) and 0 < asset.get("size", 0) <= limit,
                "Missing or oversized release metadata")
        result = subprocess.run(["gh", "api", "-H", "Accept: application/octet-stream",
                                 f"repos/{REPO}/releases/assets/{asset['id']}"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        require(result.returncode == 0 and len(result.stdout) == asset["size"] and len(result.stdout) <= limit,
                "Release metadata download failed or changed size")
        digest = asset.get("digest")
        if digest is not None:
            require(digest == "sha256:" + hashlib.sha256(result.stdout).hexdigest(),
                    "Release metadata digest mismatch")
        return result.stdout



def validate_environment(environment, policies, name):
    require(name in {"macos-release", "macos-publish"} and environment.get("name") == name,
            "Unexpected protected release environment")
    require(any(rule.get("type") == "required_reviewers" and isinstance(rule.get("reviewers"), list)
                and rule["reviewers"] for rule in environment.get("protection_rules", [])),
            f"{name} must have populated required reviewers")
    require(environment.get("can_admins_bypass") is False, f"{name} must forbid admin bypass")
    policy = environment.get("deployment_branch_policy", {})
    require(policy.get("custom_branch_policies") is True and policy.get("protected_branches") is False,
            f"{name} must select deployment branch/tag policies")
    require(any(policy.get("name") == "v*" and policy.get("type", "tag") == "tag" for policy in policies),
            f"{name} must allow the v* tag policy")
    return {"environment": name, "reviewersRequired": True, "adminBypass": False}


def verify_environment(name, github=None):
    github = github or GitHub()
    require(name in {"macos-release", "macos-publish"}, "Unexpected release environment")
    environment = github.json(f"repos/{REPO}/environments/{name}")
    policies = github.pages(f"repos/{REPO}/environments/{name}/deployment-branch-policies?per_page=100", "branch_policies")
    return validate_environment(environment, policies, name)


def validate_ci_runs(workflow, runs, commit):
    """Require the newest canonical push run, never a same-named PR/fork run."""
    source_commit(commit)
    require(isinstance(workflow.get("id"), int) and workflow.get("state") == "active",
            "Required workflow is missing or inactive")
    candidates = [run for run in runs
                  if run.get("workflow_id") == workflow["id"]
                  and run.get("path") == workflow["path"]
                  and run.get("head_sha") == commit and run.get("event") == "push"
                  and run.get("head_branch") == "main"
                  and run.get("repository", {}).get("full_name") == REPO
                  and run.get("head_repository", {}).get("full_name") == REPO]
    require(candidates, f"No canonical main push CI for {workflow['path']} at {commit}")
    newest = max(candidates, key=lambda run: (run["id"], run.get("run_attempt", 1)))
    require(newest.get("status") == "completed" and newest.get("conclusion") == "success",
            f"Latest exact-source CI did not pass: {workflow['path']}")
    return {"workflow_id": workflow["id"], "path": workflow["path"],
            "run_id": newest["id"], "run_attempt": newest.get("run_attempt", 1)}


def verify_ci(commit, github=None, *, require_tip=True, workflows=WORKFLOWS):
    github = github or GitHub()
    source_commit(commit)
    if require_tip:
        require(github.json(f"repos/{REPO}/commits/main")["sha"] == commit,
                "Formal release must use the current protected main commit")
    evidence = []
    for filename in workflows:
        require(filename in WORKFLOWS, "Unknown native workflow")
        workflow = github.json(f"repos/{REPO}/actions/workflows/{filename}")
        require(workflow.get("path") == ".github/workflows/" + filename,
                "Required workflow path mismatch")
        runs = github.pages(f"repos/{REPO}/actions/workflows/{workflow['id']}/runs"
                            f"?head_sha={commit}&event=push&branch=main&per_page=100", "workflow_runs")
        evidence.append(validate_ci_runs(workflow, runs, commit))
    return evidence


def formal_releases(releases):
    result = []
    for release in releases:
        if release.get("draft") or release.get("prerelease"):
            continue
        tag = release.get("tag_name", "")
        if re.fullmatch(r"v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", tag):
            result.append(release)
    return sorted(result, key=lambda item: stable_version(item["tag_name"][1:]), reverse=True)


def previous_android_build(release, github):
    version = release["tag_name"][1:]
    source = github.json(f"repos/{REPO}/commits/{release['tag_name']}")["sha"]
    assets = release.get("assets", [])
    names = {"update-manifest.json", f"RIMES-Android-{version}-BUILD-INFO.json"}
    evidence = []
    for name in sorted(names):
        matches = [asset for asset in assets if asset.get("name") == name and asset.get("state") == "uploaded"]
        require(len(matches) <= 1, f"Ambiguous previous Android metadata: {name}")
        if not matches:
            continue
        data = json.loads(github.asset_bytes(matches[0]))
        require(data.get("version") == version and data.get("sourceCommit") == source,
                "Previous Android metadata source/version mismatch")
        build = (data.get("platforms", {}).get("android", {}).get("build")
                 if name == "update-manifest.json" else data.get("versionCode"))
        require(isinstance(build, int) and not isinstance(build, bool) and build > 0,
                "Previous release does not establish Android versionCode")
        evidence.append(build)
    require(evidence and len(set(evidence)) == 1,
            "Previous stable release needs consistent Android feed/build metadata")
    return evidence[0]


def verify_android_build(version, build, github=None, *, allow_existing=False):
    github = github or GitHub()
    target = stable_version(version)
    require(isinstance(build, int) and not isinstance(build, bool) and 0 < build <= 2100000000,
            "Valid positive Android versionCode required")
    releases = formal_releases(github.pages(f"repos/{REPO}/releases?per_page=100"))
    require(not releases or stable_version(releases[0]["tag_name"][1:]) <= target,
            "A newer stable release already exists; refusing to move Latest backwards")
    same = [release for release in releases if release["tag_name"] == "v" + version]
    require(allow_existing or not same, "Stable version is already public; use verified recovery")
    if same:
        require(previous_android_build(same[0], github) == build,
                "Already published Android versionCode must not change")
    previous = next((release for release in releases if stable_version(release["tag_name"][1:]) < target), None)
    if previous:
        require(build > previous_android_build(previous, github),
                "Android versionCode must exceed the preceding stable release")
    return {"version": version, "androidBuild": build,
            "previousTag": previous["tag_name"] if previous else None}


def validate_artifact(artifact, name, run_id, commit):
    require(artifact.get("name") == name and not artifact.get("expired"), "Wrong or expired artifact")
    require(isinstance(artifact.get("id"), int) and 0 < artifact.get("size_in_bytes", 0) <= 1024 * 1024 * 1024,
            "Invalid artifact identity/size")
    require(artifact.get("workflow_run", {}).get("id") == run_id
            and artifact.get("workflow_run", {}).get("head_sha") == commit,
            "Artifact source run/SHA mismatch")
    require(re.fullmatch(r"sha256:[0-9a-f]{64}", artifact.get("digest", "")),
            "Authenticated immutable artifact digest required")
    return artifact


def select_artifact(artifacts, name, run_id, commit):
    matches = [artifact for artifact in artifacts if artifact.get("name") == name]
    require(len(matches) == 1, f"Missing or ambiguous immutable artifact: {name}")
    return validate_artifact(matches[0], name, run_id, commit)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    ci = commands.add_parser("ci")
    ci.add_argument("--commit", required=True)
    ci.add_argument("--allow-ancestor", action="store_true")
    ci.add_argument("--workflow", action="append", choices=WORKFLOWS)
    environment = commands.add_parser("environment")
    environment.add_argument("--name", required=True, choices=("macos-release", "macos-publish"))
    android = commands.add_parser("android-build")
    android.add_argument("--version", required=True)
    android.add_argument("--build", required=True, type=int)
    android.add_argument("--allow-existing", action="store_true")
    args = parser.parse_args()
    if args.command == "ci":
        result = verify_ci(args.commit, require_tip=not args.allow_ancestor, workflows=args.workflow or WORKFLOWS)
    elif args.command == "environment":
        result = verify_environment(args.name)
    else:
        result = verify_android_build(args.version, args.build, allow_existing=args.allow_existing)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
