"""Recovery must not turn unapproved, cancelled or different-source runs into releases."""
import copy
import hashlib
from pathlib import Path
import stat
import tempfile
import unittest
import zipfile

from recover_update_assets import authorize, extract_verified, verify_recovery_context, REQUIRED_JOBS, PUBLISHER


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        commit = "a" * 40
        self.args = [
            {"id": 123, "repository": {"full_name": "scholay/xsime"},
             "head_repository": {"full_name": "scholay/xsime"},
             "path": ".github/workflows/release.yml", "event": "push",
             "status": "completed", "conclusion": "success", "head_branch": "v1.1.6",
             "head_sha": commit},
            [{"name": name, "status": "completed", "conclusion": "success"}
             for name in REQUIRED_JOBS] +
            [{"name": "Passively verify and publish unsigned preview", "status": "completed",
              "conclusion": "skipped"}],
            [{"id": index, "name": name, "expired": False,
              "size_in_bytes": 1024, "digest": "sha256:" + "b" * 64,
              "workflow_run": {"id": 123, "head_sha": commit}}
             for index, name in enumerate((f"RIMES-Windows-release-{commit}",
                 f"RIMES-Android-release-{commit}", "RIMES-1.1.6-signed-stage-123-1",
                 "RIMES-1.1.6-signed-update-feed-123-1", "rimes-linux-deb-123-1"), start=1)],
            {"id": 456, "tag_name": "v1.1.6", "draft": True, "prerelease": False, "assets": []},
            {"id": 455, "tag_name": "v1.1.5"}, commit,
        ]

    def test_completed_approved_source_accepts_intentionally_skipped_preview(self):
        plan = authorize(*self.args)
        self.assertEqual(plan["commit"], "a" * 40)
        self.assertEqual(set(plan["artifacts"]), {"windows-release", "android-release", "linux-deb",
                                                  "rimes-signed-release-stage", "signed-update-feed"})

    def test_every_required_job_must_have_run_and_passed(self):
        for name in REQUIRED_JOBS:
            for state in ("skipped", "failure", "cancelled"):
                with self.subTest(name=name, state=state):
                    args = copy.deepcopy(self.args)
                    next(job for job in args[1] if job["name"] == name)["conclusion"] = state
                    with self.assertRaises(ValueError):
                        authorize(*args)

    def test_source_and_release_identity_fail_closed(self):
        mutations = [
            lambda a: a[0].update(event="pull_request"),
            lambda a: a[0].update(path=".github/workflows/ci.yml"),
            lambda a: a[0].update(status="in_progress"),
            lambda a: a[0].update(head_branch="v1.1.6-preview.1"),
            lambda a: a[0]["head_repository"].update(full_name="someone/fork"),
            lambda a: a.__setitem__(5, "c" * 40),
            lambda a: a[3].update(prerelease=True),
            lambda a: a[3].update(tag_name="v1.1.5"),
            lambda a: a[4].update(tag_name="v1.2.0"),
            lambda a: a[2][0].update(expired=True),
            lambda a: a[2][0].update(digest=""),
            lambda a: a[2][0]["workflow_run"].update(id=124),
            lambda a: a[2][0]["workflow_run"].update(head_sha="c" * 40),
            lambda a: a[2].append(copy.deepcopy(a[2][0])),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                args = copy.deepcopy(self.args)
                mutate(args)
                with self.assertRaises(ValueError):
                    authorize(*args)

    def test_failure_before_create_or_after_publication_can_recover(self):
        for release in (None, self.args[3], {**self.args[3], "draft": False}):
            for conclusion in ("failure", "cancelled"):
                with self.subTest(release=release, conclusion=conclusion):
                    args = copy.deepcopy(self.args)
                    args[0]["conclusion"] = conclusion
                    args[1].append({"name": PUBLISHER, "status": "completed", "conclusion": conclusion})
                    args[3] = release
                    authorize(*args)

    def test_producer_failure_is_not_treated_as_publication_failure(self):
        args = copy.deepcopy(self.args)
        args[1].append({"name": "unknown producer", "status": "completed", "conclusion": "failure"})
        with self.assertRaises(ValueError): authorize(*args)

    def test_multiple_rebuilt_signed_artifacts_are_not_guessed(self):
        args = copy.deepcopy(self.args)
        extra = copy.deepcopy(args[2][2]); extra["id"] = 9
        extra["name"] = "RIMES-1.1.6-signed-stage-123-2"; args[2].append(extra)
        with self.assertRaises(ValueError): authorize(*args)

    def test_recovery_dispatch_requires_original_tag_commit_and_repository(self):
        run = self.args[0]
        environment = {"GITHUB_REPOSITORY": "scholay/xsime", "GITHUB_EVENT_NAME": "workflow_dispatch",
                       "GITHUB_REF": "refs/tags/v1.1.6", "GITHUB_REF_NAME": "v1.1.6", "GITHUB_SHA": "a" * 40}
        verify_recovery_context(run, environment)
        for field, value in (("GITHUB_REPOSITORY", "someone/fork"),
                             ("GITHUB_EVENT_NAME", "push"),
                             ("GITHUB_REF", "refs/heads/main"),
                             ("GITHUB_REF", "refs/heads/v1.1.6"),
                             ("GITHUB_REF_NAME", "v1.1.7"),
                             ("GITHUB_SHA", "b" * 40)):
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                verify_recovery_context(run, {**environment, field: value})
        with self.assertRaises(ValueError):
            verify_recovery_context({**run, "head_branch": "v1.1.6-preview.1"}, environment)

    def test_archive_hash_paths_and_symlinks(self):
        for name, mode in (("BUILD-INFO.json", stat.S_IFREG),
                           ("../escape", stat.S_IFREG), ("/escape", stat.S_IFREG),
                           ("link", stat.S_IFLNK)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                archive = root / "source.zip"
                with zipfile.ZipFile(archive, "w") as bundle:
                    info = zipfile.ZipInfo(name)
                    info.external_attr = (mode | 0o644) << 16
                    bundle.writestr(info, b"fixture")
                digest = hashlib.sha256(archive.read_bytes()).hexdigest()
                with self.assertRaises(ValueError):
                    extract_verified(archive, root / "bad-digest", "0" * 64)
                if name == "BUILD-INFO.json":
                    extract_verified(archive, root / "valid", digest)
                    self.assertEqual((root / "valid" / name).read_bytes(), b"fixture")
                else:
                    with self.assertRaises(ValueError):
                        extract_verified(archive, root / "unsafe", digest)
                    self.assertFalse((root / "unsafe").exists())


if __name__ == "__main__":
    unittest.main()
