#!/usr/bin/env python3
"""Unit tests for the release version and notes rules."""

from __future__ import annotations

import copy
import io
import json
import os
import plistlib
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import release_tool as tool  # noqa: E402

TODAY = [
    "v0.1.0", "v0.3.8", "v0.4.1", "v0.4.2", "v0.4.3",
    "v0.5.0-preview.1", "platform-preview-v0.1.0",
]


def planned(kind: str, names: list[str], arg: str | None = None) -> tool.Plan:
    return tool.plan(kind, names, arg)


class VersionOrderTests(unittest.TestCase):
    def test_preview_sorts_before_its_stable_release(self):
        preview = tool.parse_mac_tag("v0.5.0-preview.9")
        stable = tool.parse_mac_tag("v0.5.0")
        older = tool.parse_mac_tag("v0.4.9")
        self.assertLess(older.key, preview.key)
        self.assertLess(preview.key, stable.key)

    def test_rejects_leading_zeros_and_unknown_suffixes(self):
        for name in ("v01.0.0", "v0.5.0-preview.0", "v0.5.0-rc.1", "0.5.0", "v0.5"):
            self.assertIsNone(tool.parse_mac_tag(name), name)

    def test_reads_ls_remote_lines(self):
        names = tool.tag_names(["abc123\trefs/tags/v0.4.3", "", "v0.5.0-preview.1"])
        self.assertEqual(names, ["v0.4.3", "v0.5.0-preview.1"])


class PreviewPlanTests(unittest.TestCase):
    def test_continues_the_open_line(self):
        result = planned("preview", TODAY)
        self.assertEqual(result.tag, "v0.5.0-preview.2")
        self.assertEqual(result.previous, "v0.5.0-preview.1")
        self.assertEqual(result.warnings, ())

    def test_bump_that_lands_on_the_open_line_continues_it(self):
        self.assertEqual(planned("preview", TODAY, "minor").tag, "v0.5.0-preview.2")

    def test_refuses_a_line_below_the_open_line(self):
        with self.assertRaisesRegex(tool.ReleaseError, "低于进行中的预览线"):
            planned("preview", TODAY, "patch")

    def test_a_higher_line_replaces_the_open_line_with_a_warning(self):
        result = planned("preview", TODAY, "0.6.0")
        self.assertEqual(result.tag, "v0.6.0-preview.1")
        self.assertEqual(result.previous, "v0.5.0-preview.1")
        self.assertEqual(len(result.warnings), 1)

    def test_requires_an_argument_when_no_line_is_open(self):
        names = ["v0.4.3", "v0.5.0-preview.1", "v0.5.0"]
        with self.assertRaisesRegex(tool.ReleaseError, "没有进行中的预览线"):
            planned("preview", names)
        self.assertEqual(planned("preview", names, "patch").tag, "v0.5.1-preview.1")

    def test_refuses_previews_of_a_released_version(self):
        with self.assertRaisesRegex(tool.ReleaseError, "已经正式发布"):
            planned("preview", ["v0.5.0"], "0.5.0")

    def test_first_preview_ever(self):
        result = planned("preview", [], "minor")
        self.assertEqual((result.tag, result.previous), ("v0.1.0-preview.1", None))


class StablePlanTests(unittest.TestCase):
    def test_promotes_the_open_line_with_notes_since_the_last_stable(self):
        result = planned("stable", TODAY + ["v0.5.0-preview.2"])
        self.assertEqual(result.tag, "v0.5.0")
        self.assertEqual(result.previous, "v0.4.3")

    def test_bump_from_the_highest_stable(self):
        self.assertEqual(planned("stable", TODAY, "minor").tag, "v0.5.0")
        result = planned("stable", TODAY, "patch")
        self.assertEqual(result.tag, "v0.4.4")
        self.assertEqual(len(result.warnings), 1)

    def test_refuses_existing_or_lower_versions(self):
        with self.assertRaisesRegex(tool.ReleaseError, "已存在"):
            planned("stable", TODAY, "0.4.3")
        with self.assertRaisesRegex(tool.ReleaseError, "不高于"):
            planned("stable", TODAY, "0.4.0")

    def test_stable_without_an_open_line_needs_a_version(self):
        with self.assertRaisesRegex(tool.ReleaseError, "没有可转正"):
            planned("stable", ["v0.4.3"])


class PlatformPlanTests(unittest.TestCase):
    def test_bumps_and_refuses_regressions(self):
        self.assertEqual(planned("platform", TODAY, "minor").tag, "platform-preview-v0.2.0")
        with self.assertRaisesRegex(tool.ReleaseError, "不高于"):
            planned("platform", TODAY, "0.1.0")
        with self.assertRaises(tool.ReleaseError):
            planned("platform", TODAY)


class RehearsalAndLatestTests(unittest.TestCase):
    def test_rehearsal_uses_the_next_preview(self):
        self.assertEqual(tool.rehearsal_version(TODAY), "0.5.0-preview.2")
        self.assertEqual(tool.rehearsal_version(["v0.5.0-preview.3", "v0.5.0"]), "0.5.1-preview.1")

    def test_latest_release_is_highest_published_macos_version(self):
        releases = [
            {"tagName": "platform-preview-v0.1.0", "isDraft": False},
            {"tagName": "v0.4.3", "isDraft": False},
            {"tagName": "v0.5.0-preview.1", "isDraft": False},
            {"tagName": "v0.5.0-preview.2", "isDraft": True},
        ]
        self.assertEqual(tool.latest_release(releases), "0.5.0-preview.1")
        self.assertEqual(tool.latest_release(releases, exclude="0.5.0-preview.1"), "0.4.3")
        with self.assertRaises(tool.ReleaseError):
            tool.latest_release(releases[:1])


class PublicReleaseSourceTests(unittest.TestCase):
    @staticmethod
    def response(payload):
        result = mock.MagicMock()
        result.read.return_value = json.dumps(payload).encode("utf-8")
        result.__enter__.return_value = result
        return result

    def test_normalizes_rest_and_gh_records_and_excludes_drafts(self):
        releases = [
            {"tag_name": "v0.5.0-preview.3", "draft": False, "prerelease": True},
            {"tagName": "v0.5.1", "isDraft": False, "isPrerelease": False},
            {"tag_name": "v0.5.2", "draft": True},
            {"tag_name": "platform-preview-v0.2.0", "draft": False},
        ]
        self.assertEqual(
            tool.public_release_tag_names(releases),
            ["v0.5.0-preview.3", "v0.5.1"],
        )
        self.assertEqual(tool.latest_release(releases), "0.5.1")

    def test_reads_paginated_releases_without_a_gh_dependency(self):
        first = [{"tag_name": f"v1.0.{index}", "draft": False}
                 for index in range(tool.GITHUB_RELEASE_PAGE_SIZE)]
        second = [{"tag_name": "v2.0.0", "draft": False}]
        with mock.patch.object(
            tool.urlrequest,
            "urlopen",
            side_effect=[self.response(first), self.response(second)],
        ) as opener:
            records = tool.github_release_records()
        self.assertEqual(records, first + second)
        self.assertEqual(opener.call_count, 2)
        self.assertIn("page=1", opener.call_args_list[0].args[0].full_url)
        self.assertIn("page=2", opener.call_args_list[1].args[0].full_url)

    def test_network_or_json_failure_never_falls_back_to_git_tags(self):
        with mock.patch.object(tool.urlrequest, "urlopen", side_effect=tool.urlerror.URLError("offline")):
            with self.assertRaisesRegex(tool.ReleaseError, "无法读取 GitHub Releases"):
                tool.github_release_records()
        invalid = mock.MagicMock()
        invalid.read.return_value = b"not json"
        invalid.__enter__.return_value = invalid
        with mock.patch.object(tool.urlrequest, "urlopen", return_value=invalid):
            with self.assertRaisesRegex(tool.ReleaseError, "无效 JSON"):
                tool.github_release_records()

    def test_offline_release_json_is_supported(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "releases.json"
            fixture.write_text('[{"tag_name":"v0.5.0-preview.3","draft":false}]', encoding="utf-8")
            self.assertEqual(
                tool.public_release_tag_names(tool.release_records_from_json(fixture)),
                ["v0.5.0-preview.3"],
            )


class CommitRulesTests(unittest.TestCase):
    def commit(self, subject: str, body: str = "") -> tool.Commit:
        return tool.Commit("abc1234", subject, body)

    def test_sections(self):
        self.assertEqual(self.commit("feat(chord): add yoyo").section, "feat")
        self.assertEqual(self.commit("fix!: drop legacy path").section, "breaking")
        self.assertEqual(self.commit("refactor: x", "BREAKING CHANGE: y").section, "breaking")
        self.assertEqual(self.commit("ci: pin runner").section, "maintenance")
        self.assertEqual(self.commit("release: enable previews").section, "other")
        self.assertEqual(self.commit("Shrink the thing").section, "other")

    def test_entry_formats_scope(self):
        self.assertEqual(self.commit("feat(chord): add yoyo").entry(), "- **chord:** add yoyo (abc1234)")
        self.assertEqual(self.commit("docs: tidy").entry(), "- tidy (abc1234)")


class GitHistoryTests(unittest.TestCase):
    """Exercise notes, changelog and commit lint against a throwaway repository."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        env = {
            "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com",
            "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.com",
            "GIT_AUTHOR_DATE": "2026-09-01T00:00:00Z", "GIT_COMMITTER_DATE": "2026-09-01T00:00:00Z",
        }
        self.env = {**os.environ, **env}
        self.run_git("init", "-q", "-b", "main")
        self.commit("chore: initial import")
        self.run_git("tag", "v0.4.3")
        self.commit("feat(chord): add yoyo schemes")
        self.commit("fix: keep the buffer link")
        self.run_git("switch", "-q", "-c", "topic")
        self.commit("docs: describe previews")
        self.run_git("switch", "-q", "main")
        self.run_git("merge", "-q", "--no-ff", "topic", "-m", "Merge #17: 呦呦音形")
        self.run_git("tag", "v0.5.0-preview.1")
        self.commit("Shrink the capsule")
        # v0.5.0 is an immutable but cancelled/unpublished tag. Public notes
        # must not silently use it as a comparison baseline.
        self.run_git("tag", "v0.5.0")
        self.releases = [
            {"tag_name": "v0.4.3", "draft": False, "prerelease": True},
            {"tag_name": "v0.5.0-preview.1", "draft": False, "prerelease": True},
        ]
        self.patch = mock.patch.object(tool, "REPO_ROOT", self.root)
        self.patch.start()

    def tearDown(self):
        self.patch.stop()
        self.tmp.cleanup()

    def run_git(self, *args: str) -> str:
        return subprocess.run(
            ["git", *args], cwd=self.root, env=self.env, check=True, capture_output=True, text=True
        ).stdout

    def commit(self, subject: str) -> None:
        self.run_git("commit", "-q", "--allow-empty", "-m", subject)

    def test_notes_group_commits_and_list_merged_prs(self):
        notes = tool.release_notes("v0.5.0-preview.1", releases=self.releases)
        self.assertIn("自 v0.4.3 以来", notes)
        self.assertIn("- #17 呦呦音形", notes)
        self.assertIn("### 新功能\n\n- **chord:** add yoyo schemes", notes)
        self.assertIn("### 修复\n\n- keep the buffer link", notes)
        self.assertIn("### 文档\n\n- describe previews", notes)
        self.assertNotIn("initial import", notes)
        self.assertIn("compare/v0.4.3...v0.5.0-preview.1", notes)

    def test_notes_for_an_untagged_candidate(self):
        notes = tool.release_notes("v0.5.0-preview.2", ref="HEAD", releases=self.releases)
        self.assertIn("自 v0.5.0-preview.1 以来", notes)
        self.assertIn("### 其他\n\n- Shrink the capsule", notes)

    def test_notes_ignore_cancelled_tags_when_selecting_the_public_baseline(self):
        notes = tool.release_notes("v0.5.1", ref="HEAD", releases=self.releases)
        self.assertIn("自 v0.5.0-preview.1 以来", notes)
        self.assertIn("compare/v0.5.0-preview.1...v0.5.1", notes)
        self.assertNotIn("v0.5.0...v0.5.1", notes)

    def test_changelog_lists_tags_newest_first(self):
        text = tool.render_changelog(self.releases)
        self.assertLess(text.index("## [v0.5.0-preview.1]"), text.index("## [v0.4.3]"))
        self.assertIn("— 2026-09-01", text)
        self.assertNotIn("## [v0.5.0]", text)

    def test_changelog_ignores_tags_that_are_not_public_releases(self):
        text = tool.render_changelog([{"tag_name": "v0.5.0-preview.1", "draft": False}])
        self.assertNotIn("## [v0.4.3]", text)
        self.assertIn("- **chord:** add yoyo schemes", text)
        self.assertIn("- initial import", text)
        with self.assertRaisesRegex(tool.ReleaseError, "本地缺少公开 GitHub Release 的 tag"):
            tool.render_changelog([
                {"tag_name": "v0.5.0-preview.1", "draft": False},
                {"tag_name": "v0.6.0-preview.1", "draft": False},
            ])

    def test_lint_commits_flags_non_conventional_subjects(self):
        problems = tool.lint_commits("v0.4.3", "HEAD")
        self.assertEqual(len(problems), 1)
        self.assertIn("Shrink the capsule", problems[0])

    def test_lint_reconciles_an_already_applied_patch_without_allowing_new_bad_titles(self):
        start = self.run_git("rev-parse", "HEAD").strip()
        self.run_git("switch", "-c", "historical", start)
        (self.root / "installer.txt").write_text("fixed installer\n")
        self.run_git("add", "installer.txt")
        self.commit("release(windows): historical installer fix")
        old = self.run_git("rev-parse", "HEAD").strip()

        self.run_git("switch", "-c", "integration", start)
        self.run_git("cherry-pick", "--no-commit", old)
        self.commit("fix(windows): deliver installer fix")
        base = self.run_git("rev-parse", "HEAD").strip()
        self.run_git("merge", "--no-ff", "historical", "-m", "chore: reconcile history")
        self.assertEqual(tool.lint_commits(base, "HEAD"), [])

        (self.root / "installer.txt").write_text("a genuinely different fix\n")
        self.run_git("add", "installer.txt")
        self.commit("release(windows): new unsupported title")
        problems = tool.lint_commits(base, "HEAD")
        self.assertEqual(len(problems), 1)
        self.assertIn("new unsupported title", problems[0])


class PlistTests(unittest.TestCase):
    def test_product_version_anchor_is_required(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Info.plist"
            path.write_bytes(plistlib.dumps({"CFBundleShortVersionString": tool.PRODUCT_VERSION}))
            self.assertIsNone(tool.check_plist(path))
            path.write_bytes(plistlib.dumps({"CFBundleShortVersionString": "0.4.2"}))
            self.assertIn("0.4.2", tool.check_plist(path))


class EnvironmentApprovalTests(unittest.TestCase):
    """Exercise the actual passive checks in both protected workflow jobs."""

    @classmethod
    def setUpClass(cls):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/release.yml").read_text()
        marker = 'python3 - "$environment_json" "$branch_policies_json" "$ENVIRONMENT_NAME" <<\'PY\'\n'
        blocks = workflow.split(marker)[1:]
        if len(blocks) != 2:
            raise AssertionError("Both signing and publishing must validate their environment")
        cls.checks = [
            compile(textwrap.dedent(block.split("\n          PY\n", 1)[0]), "environment-policy", "exec")
            for block in blocks
        ]

    def fixture(self):
        return {
            "name": "macos-release",
            "can_admins_bypass": False,
            "protection_rules": [{
                "type": "required_reviewers",
                "reviewers": [{"type": "User", "reviewer": {"login": "scholay"}}],
                "prevent_self_review": False,
            }],
            "deployment_branch_policy": {
                "custom_branch_policies": True, "protected_branches": False,
            },
        }

    def validate(self, check, environment, policies=None):
        documents = {
            "environment.json": json.dumps(environment),
            "policies.json": json.dumps(policies if policies is not None else {
                "branch_policies": [{"name": "v*", "type": "tag"}],
            }),
        }
        with mock.patch.object(sys, "argv", ["check", "environment.json", "policies.json", "macos-release"]), \
                mock.patch("builtins.open", side_effect=lambda path, **kwargs: io.StringIO(documents[path])):
            exec(check, {})

    def test_allows_explicit_review_by_the_initiator_or_an_independent_reviewer(self):
        for check in self.checks:
            for prevent_self_review in (False, True):
                with self.subTest(check=check, prevent_self_review=prevent_self_review):
                    environment = self.fixture()
                    environment["protection_rules"][0]["prevent_self_review"] = prevent_self_review
                    self.validate(check, environment)

    def test_still_requires_reviewers_no_admin_bypass_and_selected_deployments(self):
        for check in self.checks:
            for field, value in (
                ("protection_rules", []),
                ("protection_rules", [{"type": "required_reviewers", "reviewers": []}]),
                ("can_admins_bypass", True),
                ("can_admins_bypass", None),
                ("deployment_branch_policy", None),
                ("deployment_branch_policy", {"custom_branch_policies": False, "protected_branches": True}),
                ("name", "wrong-environment"),
            ):
                with self.subTest(field=field, value=value):
                    environment = copy.deepcopy(self.fixture())
                    environment[field] = value
                    with self.assertRaises(SystemExit):
                        self.validate(check, environment)

    def test_still_requires_the_release_tag_pattern(self):
        for check in self.checks:
            with self.assertRaises(SystemExit):
                self.validate(check, self.fixture(), {"branch_policies": [{"name": "main"}]})


class FormalPackageOnlyWorkflowTests(unittest.TestCase):
    """Keep the supported Installer path as the only formal notarization lane."""

    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        workflow = (root / ".github/workflows/release.yml").read_text()
        cls.preview_job = workflow.split("  publish_unsigned_preview:", 1)[1].split(
            "  sign_and_stage:", 1
        )[0]
        cls.signing_job = workflow.split("  sign_and_stage:", 1)[1].split(
            "  publish_staged_release:", 1
        )[0]
        cls.publish_job = workflow.split("  publish_staged_release:", 1)[1]
        cls.publisher = (root / "scripts/release/publish_update_assets.py").read_text()
        cls.package_script = (root / "scripts/make-pkg.sh").read_text()
        cls.rehearsal_script = (root / "scripts/rehearse-release-pkg.sh").read_text()
        cls.reference_doc = (root / "docs/release/reference.md").read_text()

    def test_formal_job_notarizes_only_the_final_package(self):
        self.assertIn('notarize-macos.sh pkg "RIMES-${VERSION}.pkg"', self.signing_job)
        self.assertNotIn("notarize-macos.sh app", self.signing_job)
        self.assertNotIn("RIMES-${VERSION}.zip", self.signing_job)
        self.assertNotIn("app_zip", self.signing_job)
        self.assertNotIn('stapler validate -v "$payload_app"', self.signing_job)

    def test_mac_stage_and_public_release_do_not_publish_an_app_zip(self):
        self.assertIn('"$PKG" SHA256SUMS RELEASE-NOTES.md release-manifest.txt', self.signing_job)
        self.assertIn('package.name: package, "SHA256SUMS": mac / "SHA256SUMS"', self.publisher)
        self.assertIn("scripts/release/publish-update-assets.sh", self.publish_job)
        self.assertNotIn("APP_ZIP", self.publish_job)
        self.assertNotIn("RIMES-${VERSION}.zip", self.publish_job)

    def test_package_and_rehearsal_validate_the_notarized_outer_container(self):
        self.assertNotIn("RIMES_REQUIRE_NOTARIZATION", self.package_script)
        self.assertIn('stapler validate -v "$package_path"', self.rehearsal_script)
        self.assertNotIn('stapler validate -v "$payload_app"', self.rehearsal_script)
        self.assertNotIn('stapler validate -v "$installed_app"', self.rehearsal_script)

    def test_reference_document_matches_the_package_only_policy(self):
        self.assertIn("只提交该 PKG 公证", self.reference_doc)
        self.assertIn("四个成员", self.reference_doc)
        self.assertIn("正式 Release 不包含 `RIMES-X.Y.Z.zip`", self.reference_doc)
        self.assertNotIn("逐字节复核五个成员", self.reference_doc)

    def test_release_note_generation_receives_the_github_api_token(self):
        for job in (self.preview_job, self.signing_job):
            with self.subTest(job=job[:48]):
                self.assertIn("GITHUB_TOKEN: ${{ github.token }}", job)
                self.assertIn("scripts/release/release_tool.py notes", job)


class MacOSReleaseGatePolicyTests(unittest.TestCase):
    """Require native platform evidence for stable releases, not legacy previews."""

    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.release_script = (root / "scripts/release.sh").read_text()
        cls.release_workflow = (root / ".github/workflows/release.yml").read_text()
        cls.platform_workflow = (root / ".github/workflows/platform-preview.yml").read_text()
        cls.windows_workflow = (root / ".github/workflows/windows-native.yml").read_text()

    def test_stable_release_requires_exact_source_ci_before_signing(self):
        self.assertIn('scripts/release/release_guard.py "${guard_arguments[@]}"', self.release_script)
        self.assertIn('guard_arguments+=(--workflow ci.yml)', self.release_script)
        self.assertIn('release_guard.py android-build --version "$VERSION" --build "$android_build"', self.release_script)
        signing = self.release_workflow.split("  sign_and_stage:", 1)[1].split("  sign_update_manifest:", 1)[0]
        self.assertLess(signing.index('release_guard.py ci --commit "$GITHUB_SHA"'),
                        signing.index('name: Import protected Developer ID credentials'))

    def test_complete_signed_metadata_precedes_the_only_stable_publisher(self):
        job = self.release_workflow.split("  publish_staged_release:", 1)[1].split("  attach_linux_deb:", 1)[0]
        self.assertIn("needs.sign_update_manifest.result == 'success'", job)
        self.assertIn("group: stable-release-publication", job)
        self.assertNotIn("gh release create", job)
        self.assertNotIn("  publish_update_manifest:", self.release_workflow)
        recovery = (Path(__file__).resolve().parents[2] / ".github/workflows/complete-release-assets.yml").read_text()
        self.assertIn("environment: macos-publish", recovery)
        self.assertIn("group: stable-release-publication", recovery)

    def test_cross_platform_workflows_are_scheduled_or_manual_maintenance_only(self):
        for workflow in (self.platform_workflow, self.windows_workflow):
            with self.subTest(workflow=workflow.splitlines()[0]):
                trigger = workflow.split("permissions:", 1)[0]
                self.assertIn("schedule:", trigger)
                self.assertIn("workflow_dispatch:", trigger)
                self.assertNotIn("push:", trigger)
                self.assertNotIn("pull_request:", trigger)

    def test_formal_release_jobs_remain_on_macos(self):
        names = ("build_and_smoke", "sign_and_stage", "publish_staged_release")
        for index, name in enumerate(names):
            job = self.release_workflow.split(f"  {name}:", 1)[1]
            if index + 1 < len(names):
                job = job.split(f"  {names[index + 1]}:", 1)[0]
            self.assertIn("runs-on: macos-15", job)


if __name__ == "__main__":
    unittest.main()
