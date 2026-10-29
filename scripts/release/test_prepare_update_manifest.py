import copy
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from prepare_update_manifest import prepare, verify_call_context
from publish_update_assets import digest, prepare_assets
from release_guard import REPO

VERSION = "1.2.1"
SHA = "a" * 40
RUN = 123


def package_fixture(root):
    folders = {}
    for name in ("rimes-signed-release-stage", "linux-deb", "windows-release", "android-release"):
        folders[name] = root / name; folders[name].mkdir(parents=True)
    mac = folders["rimes-signed-release-stage"]
    pkg = mac / f"RIMES-{VERSION}.pkg"; pkg.write_bytes(b"signed mac installer fixture")
    notes = mac / "RELEASE-NOTES.md"; notes.write_text("Approved package notes\n")
    (mac / "SHA256SUMS").write_text(f"{digest(pkg)}  {pkg.name}\n")
    values = {"schema": "rimes-signed-pkg-release-v1", "repository": REPO, "run_id": str(RUN),
              "commit": SHA, "tag": "v" + VERSION, "version": VERSION, "pkg": pkg.name,
              "pkg_sha256": digest(pkg), "pkg_bytes": str(pkg.stat().st_size),
              "notes": notes.name, "notes_sha256": digest(notes), "notes_bytes": str(notes.stat().st_size)}
    (mac / "release-manifest.txt").write_text("".join(f"{key}={value}\n" for key, value in values.items()))
    linux = folders["linux-deb"]; deb = linux / f"fcitx5-rimes_{VERSION}_amd64.deb"; deb.write_bytes(b"linux fixture")
    (linux / (deb.name + ".sha256")).write_text(f"{digest(deb)}  {deb.name}\n")
    windows = folders["windows-release"]
    for suffix in ("-Setup.exe", ".zip"):
        (windows / f"RIMES-Windows-{VERSION}{suffix}").write_bytes(("windows" + suffix).encode())
    for name in ("BUILD-INFO.json", "SOURCE-FILES.json", "SETUP-VERIFICATION.json"):
        (windows / name).write_text(json.dumps({"sourceCommit": SHA, "version": VERSION}))
    (windows / "SHA256SUMS").write_text("".join(
        f"{digest(windows / name)}  {name}\n" for name in
        (f"RIMES-Windows-{VERSION}-Setup.exe", f"RIMES-Windows-{VERSION}.zip")))
    android = folders["android-release"]
    for suffix in (".apk", ".aab"):
        (android / f"RIMES-Android-{VERSION}{suffix}").write_bytes(("android" + suffix).encode())
    (android / f"RIMES-Android-{VERSION}-BUILD-INFO.json").write_text(json.dumps(
        {"sourceCommit": SHA, "version": VERSION, "versionCode": 24}))
    (android / f"RIMES-Android-{VERSION}-SHA256SUMS").write_text("".join(
        f"{digest(android / name)}  {name}\n" for name in
        (f"RIMES-Android-{VERSION}.apk", f"RIMES-Android-{VERSION}.aab")))
    return folders


class FixtureGitHub:
    def __init__(self):
        names = [f"RIMES-{VERSION}-signed-stage-{RUN}-1", f"rimes-linux-deb-{RUN}-1",
                 f"RIMES-Windows-release-{SHA}", f"RIMES-Android-release-{SHA}"]
        self.artifacts = [{"id": i, "name": name, "expired": False, "size_in_bytes": 100,
                           "digest": "sha256:" + "b" * 64,
                           "workflow_run": {"id": RUN, "head_sha": SHA}}
                          for i, name in enumerate(names, 1)]
        self.run = {"id": RUN, "repository": {"full_name": REPO}, "head_repository": {"full_name": REPO},
                    "path": ".github/workflows/release.yml", "event": "push", "head_sha": SHA,
                    "head_branch": "v" + VERSION}

    def json(self, path):
        return self.run if "/actions/runs/" in path else {"sha": SHA}

    def pages(self, path, key=None):
        return self.artifacts


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); self.fixtures = package_fixture(self.root / "original")
        self.github = FixtureGitHub()

    def prepare(self, name="prepared"):
        output = self.root / name
        def download(artifact, destination):
            shutil.copytree(self.fixtures[destination.name], destination)
        with patch("prepare_update_manifest.verify_ci") as gate:
            value = prepare(output, VERSION, SHA, RUN, self.github.artifacts[0]["name"], 1, "b" * 64,
                            self.github.artifacts[1]["name"], self.github, download)
            gate.assert_called_once_with(SHA, self.github)
        return output, value

    def test_signing_cli_is_bound_to_its_current_tag_and_run(self):
        environment = {"GITHUB_REPOSITORY": REPO, "GITHUB_EVENT_NAME": "push",
                       "GITHUB_REF_NAME": "v" + VERSION, "GITHUB_SHA": SHA, "GITHUB_RUN_ID": str(RUN)}
        verify_call_context(VERSION, SHA, RUN, environment)
        for key in environment:
            with self.subTest(key=key), self.assertRaises(ValueError):
                verify_call_context(VERSION, SHA, RUN, {**environment, key: "different"})

    def test_four_authenticated_packages_create_only_unsigned_metadata(self):
        output, value = self.prepare()
        self.assertEqual(set(value["platforms"]), {"macos", "windows", "linux", "android"})
        self.assertNotIn("authentication", value)
        self.assertTrue((output / "unsigned-update-manifest.json").is_file())
        self.assertEqual(value["sourceCommit"], SHA)

    def test_different_source_run_or_artifact_digest_is_rejected(self):
        self.github.run["head_sha"] = "c" * 40
        with self.assertRaises(ValueError): self.prepare()
        self.github.run["head_sha"] = SHA
        self.github.artifacts[0]["digest"] = "sha256:" + "c" * 64
        with self.assertRaises(ValueError): self.prepare()

    def test_build_info_source_and_package_hash_mismatch_are_rejected(self):
        path = self.fixtures["android-release"] / f"RIMES-Android-{VERSION}-BUILD-INFO.json"
        data = json.loads(path.read_text()); data["sourceCommit"] = "c" * 40; path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "source/version"): self.prepare()
        data["sourceCommit"] = SHA; path.write_text(json.dumps(data))
        deb = self.fixtures["linux-deb"] / f"fcitx5-rimes_{VERSION}_amd64.deb"; deb.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"): self.prepare("changed")

    def test_publisher_requires_verified_feed_and_matches_every_package(self):
        output, value = self.prepare()
        feed_root = output / "signed-update-feed"; feed_root.mkdir()
        (feed_root / "update-manifest.json").write_text(json.dumps(value))
        def reject(_): raise ValueError("invalid or expired signature")
        with self.assertRaisesRegex(ValueError, "invalid or expired"):
            prepare_assets(output, VERSION, SHA, RUN, reject)
        self.assertFalse((output / "application-release").exists())
        assets, body, code = prepare_assets(output, VERSION, SHA, RUN, lambda doc: doc)
        self.assertIn("update-manifest.json", assets); self.assertEqual(code, 24)
        self.assertIn("xsime-release-source:", body)
        changed = copy.deepcopy(value); changed["platforms"]["windows"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "windows package"):
            prepare_assets(output, VERSION, SHA, RUN, lambda doc: changed)


if __name__ == "__main__":
    unittest.main()
