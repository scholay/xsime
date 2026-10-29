import copy
from pathlib import Path
import tempfile
import unittest

from publish_update_assets import publish

SHA = "a" * 40


class FakeReleaseGitHub:
    def __init__(self):
        self.current = None
        self.bytes = {}
        self.events = []
        self.fail_download = False
        self.fail_after_promote = False

    def json(self, path):
        if path.endswith("/releases/latest"):
            return {"id": 7}
        return {"sha": SHA}

    def release(self, tag):
        return copy.deepcopy(self.current)

    def create_draft(self, tag, commit, body):
        self.events.append("create_draft")
        self.current = {"id": 7, "tag_name": tag, "body": body, "draft": True,
                        "prerelease": False, "assets": []}
        return self.release(tag)

    def upload(self, tag, path):
        self.events.append("upload:" + path.name)
        data = path.read_bytes(); self.bytes[path.name] = data
        self.current["assets"].append({"id": len(self.bytes), "name": path.name,
                                       "size": len(data), "state": "uploaded"})

    def download(self, asset, path):
        self.events.append("readback:" + asset["name"])
        if self.fail_download:
            self.fail_download = False
            raise OSError("transient readback failure")
        path.write_bytes(self.bytes[asset["name"]])

    def promote(self, release_id):
        self.events.append("promote")
        self.current["draft"] = False
        if self.fail_after_promote:
            self.fail_after_promote = False
            raise OSError("publication response lost")
        return self.release("v1.2.1")


class PublishTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        self.assets = {}
        for name in ("fixture.pkg", "fixture.deb", "fixture.exe", "fixture.apk", "update-manifest.json"):
            path = self.root / name; path.write_bytes(name.encode()); self.assets[name] = path
        self.github = FakeReleaseGitHub(); self.guard_calls = 0

    def guard(self):
        self.guard_calls += 1

    def run_publish(self, suffix="a", guard=None):
        return publish(self.github, "1.2.1", SHA, 24, self.assets, "approved notes + identity", self.root / suffix,
                       monotonic_guard=guard or self.guard)

    def test_all_assets_read_back_before_one_publication(self):
        self.run_publish()
        self.assertEqual(self.github.events[-1], "promote")
        self.assertEqual(sum(x.startswith("readback:") for x in self.github.events), len(self.assets))
        self.assertEqual(self.guard_calls, 2)

    def test_create_then_failure_resumes_same_draft_without_overwrite(self):
        self.github.fail_download = True
        with self.assertRaises(OSError): self.run_publish()
        self.assertTrue(self.github.current["draft"])
        self.run_publish("b")
        self.assertEqual(self.github.events.count("create_draft"), 1)
        self.assertEqual(self.github.events.count("promote"), 1)
        self.assertEqual(sum(x.startswith("upload:") for x in self.github.events), len(self.assets))

    def test_publication_response_loss_is_idempotent(self):
        self.github.fail_after_promote = True
        with self.assertRaises(OSError): self.run_publish()
        self.run_publish("b")
        self.assertEqual(self.github.events.count("promote"), 1)
        self.assertEqual(sum(x.startswith("upload:") for x in self.github.events), len(self.assets))

    def test_different_existing_bytes_are_never_replaced(self):
        self.github.create_draft("v1.2.1", SHA, "approved notes + identity")
        path = next(iter(self.assets.values())); self.github.upload("v1.2.1", path)
        self.github.bytes[path.name] = b"x" * path.stat().st_size
        with self.assertRaisesRegex(ValueError, "bytes differ"): self.run_publish()
        self.assertNotIn("promote", self.github.events)
        self.assertEqual(sum(x.startswith("upload:") for x in self.github.events), 1)

    def test_unrelated_existing_draft_is_not_adopted(self):
        self.github.create_draft("v1.2.1", SHA, "different identity")
        with self.assertRaisesRegex(ValueError, "not this authenticated"): self.run_publish()
        self.assertEqual(self.github.events, ["create_draft"])

    def test_version_race_rechecked_before_latest_changes(self):
        def guard():
            self.guard_calls += 1
            if self.guard_calls == 2: raise ValueError("newer stable release exists")
        with self.assertRaisesRegex(ValueError, "newer stable"): self.run_publish(guard=guard)
        self.assertTrue(self.github.current["draft"])
        self.assertNotIn("promote", self.github.events)

    def test_public_incomplete_release_is_not_modified(self):
        self.github.create_draft("v1.2.1", SHA, "approved notes + identity"); self.github.current["draft"] = False
        with self.assertRaisesRegex(ValueError, "public release is incomplete"): self.run_publish()
        self.assertFalse(any(x.startswith("upload:") for x in self.github.events))


if __name__ == "__main__":
    unittest.main()
