import unittest
from release_guard import REPO, validate_ci_runs, verify_ci, verify_android_build, select_artifact, validate_environment

SHA = "a" * 40


class FakeGitHub:
    def __init__(self, releases=None, build=23):
        self.releases = releases or []
        self.build = build

    def pages(self, path, key=None):
        return self.releases

    def json(self, path, **unused):
        return {"sha": SHA}

    def asset_bytes(self, asset, **unused):
        import json
        version = asset.get("version", "1.2.0")
        value = {"version": version, "sourceCommit": SHA}
        if asset["name"] == "update-manifest.json":
            value["platforms"] = {"android": {"build": self.build}}
        else:
            value["versionCode"] = asset.get("build", self.build)
        return json.dumps(value).encode()


def release(version="1.2.0"):
    return {"id": 4, "tag_name": "v" + version, "draft": False, "prerelease": False,
            "assets": [{"id": 5, "name": "update-manifest.json", "state": "uploaded", "version": version}]}


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.workflow = {"id": 12, "path": ".github/workflows/ci.yml", "state": "active"}
        self.run = {"id": 33, "workflow_id": 12, "path": self.workflow["path"],
                    "head_sha": SHA, "event": "push", "head_branch": "main",
                    "repository": {"full_name": REPO}, "head_repository": {"full_name": REPO},
                    "status": "completed", "conclusion": "success", "run_attempt": 1}

    def test_exact_canonical_push_passes(self):
        self.assertEqual(validate_ci_runs(self.workflow, [self.run], SHA)["run_id"], 33)

    def test_fork_pr_wrong_branch_sha_path_or_workflow_cannot_satisfy_gate(self):
        changes = [{"workflow_id": 13}, {"path": ".github/workflows/imposter.yml"},
                   {"event": "pull_request"}, {"head_branch": "other"}, {"head_sha": "b" * 40},
                   {"head_repository": {"full_name": "someone/fork"}},
                   {"repository": {"full_name": "someone/fork"}}]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                validate_ci_runs(self.workflow, [{**self.run, **change}], SHA)

    def test_older_success_does_not_mask_new_failure_or_running_attempt(self):
        for change in ({"id": 34, "conclusion": "failure"},
                       {"id": 34, "status": "in_progress", "conclusion": None},
                       {"run_attempt": 2, "conclusion": "failure"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                validate_ci_runs(self.workflow, [self.run, {**self.run, **change}], SHA)

    def test_current_main_gate_precedes_workflow_lookup(self):
        github = FakeGitHub()
        with self.assertRaisesRegex(ValueError, "current protected main"):
            verify_ci("b" * 40, github)

    def test_android_code_must_increase(self):
        for code in (22, 23, 0, True):
            with self.subTest(code=code), self.assertRaises(ValueError):
                verify_android_build("1.2.1", code, FakeGitHub([release()]))
        self.assertEqual(verify_android_build("1.2.1", 24, FakeGitHub([release()]))["previousTag"], "v1.2.0")

    def test_older_version_cannot_retake_latest(self):
        with self.assertRaisesRegex(ValueError, "newer stable"):
            verify_android_build("1.1.9", 99, FakeGitHub([release()]), allow_existing=True)

    def test_public_same_version_only_accepts_original_build_in_recovery(self):
        with self.assertRaises(ValueError):
            verify_android_build("1.2.0", 23, FakeGitHub([release()]))
        verify_android_build("1.2.0", 23, FakeGitHub([release()]), allow_existing=True)
        with self.assertRaisesRegex(ValueError, "must not change"):
            verify_android_build("1.2.0", 24, FakeGitHub([release()]), allow_existing=True)

    def test_drafts_and_previews_do_not_change_baseline(self):
        newer = {**release("9.0.0"), "draft": True}
        verify_android_build("1.2.1", 24, FakeGitHub([newer, release()]))

    def test_missing_or_conflicting_prior_android_metadata_fails_closed(self):
        previous = release(); previous["assets"] = []
        with self.assertRaises(ValueError):
            verify_android_build("1.2.1", 24, FakeGitHub([previous]))
        previous = release()
        previous["assets"].append({"id": 6, "name": "RIMES-Android-1.2.0-BUILD-INFO.json",
                                   "state": "uploaded", "build": 22})
        with self.assertRaises(ValueError):
            verify_android_build("1.2.1", 24, FakeGitHub([previous]))

    def test_first_stable_release_has_no_prior_build(self):
        self.assertIsNone(verify_android_build("1.0.0", 1, FakeGitHub())["previousTag"])

    def test_release_environment_requires_reviewers_no_bypass_and_tags(self):
        environment = {"name": "macos-release", "can_admins_bypass": False,
                       "protection_rules": [{"type": "required_reviewers", "reviewers": [{"id": 1}]}],
                       "deployment_branch_policy": {"custom_branch_policies": True, "protected_branches": False}}
        policies = [{"name": "v*", "type": "tag"}]
        validate_environment(environment, policies, "macos-release")
        for mutation in ({"can_admins_bypass": True}, {"protection_rules": []},
                         {"deployment_branch_policy": {"custom_branch_policies": False, "protected_branches": True}}):
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_environment({**environment, **mutation}, policies, "macos-release")
        for policies in ([], [{"name": "main", "type": "branch"}], [{"name": "v*", "type": "branch"}]):
            with self.subTest(policies=policies), self.assertRaises(ValueError):
                validate_environment(environment, policies, "macos-release")

    def test_artifact_must_match_run_sha_and_digest_and_unique_name(self):
        artifact = {"id": 1, "name": "fixture", "expired": False, "size_in_bytes": 1,
                    "digest": "sha256:" + "b" * 64, "workflow_run": {"id": 2, "head_sha": SHA}}
        self.assertEqual(select_artifact([artifact], "fixture", 2, SHA), artifact)
        for change in ({"expired": True}, {"digest": ""}, {"workflow_run": {"id": 3, "head_sha": SHA}}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                select_artifact([{**artifact, **change}], "fixture", 2, SHA)
        with self.assertRaises(ValueError):
            select_artifact([artifact, artifact], "fixture", 2, SHA)


if __name__ == "__main__":
    unittest.main()
