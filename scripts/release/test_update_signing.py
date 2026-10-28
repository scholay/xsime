import base64
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from update_signing import (ROOT, CLOCK_SKEW, MAX_VALIDITY, canonical, load_keys,
                            public_key, sign_manifest, verify_manifest)


class UpdateSigningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads((ROOT / "Shared/AppUpdate/fixtures/signed_manifest.json").read_text())

    def verify(self, document=None, **kwargs):
        return verify_manifest(document or self.fixture["document"],
                               now=kwargs.get("now", self.fixture["now"]),
                               trusted_keys=kwargs.get("keys", self.fixture["trustedKeys"]))

    def test_public_cross_platform_vector(self):
        payload = self.verify()
        self.assertEqual(payload["version"], "9.8.7")
        self.assertEqual(payload["platforms"]["android"]["build"], 99)
        self.assertNotIn("authentication", payload)
        self.assertFalse(set(self.fixture["trustedKeys"]) & set(load_keys()))
        self.assertFalse(set(self.fixture["trustedKeys"].values()) & set(load_keys().values()))

    def test_unsigned_legacy_feed_is_not_a_trust_fallback(self):
        changed = copy.deepcopy(self.fixture["document"])
        del changed["authentication"]
        with self.assertRaises(ValueError): self.verify(changed)

    def test_outer_mutation_cannot_hide_behind_an_old_signature(self):
        for field, value in [("version", "99.0.0"), ("sourceCommit", "b" * 40)]:
            changed = copy.deepcopy(self.fixture["document"])
            changed[field] = value
            with self.assertRaises(ValueError): self.verify(changed)

    def test_mutating_both_payloads_still_requires_a_signature(self):
        changed = copy.deepcopy(self.fixture["document"])
        changed["platforms"]["windows"]["sha256"] = "0" * 64
        payload = {k: v for k, v in changed.items() if k != "authentication"}
        changed["authentication"]["payload"] = base64.b64encode(canonical(payload)).decode()
        with self.assertRaises(ValueError): self.verify(changed)

    def test_invalid_signature_and_unknown_key_are_rejected(self):
        for field, value in [("keyId", "unknown-key"), ("signature", base64.b64encode(b"x" * 64).decode())]:
            changed = copy.deepcopy(self.fixture["document"])
            changed["authentication"]["signatures"][0][field] = value
            with self.assertRaises(ValueError): self.verify(changed)

    def test_expired_future_and_boundary_times(self):
        feed = self.fixture["document"]
        self.verify(now=feed["expiresAt"] - 1)
        self.verify(now=feed["issuedAt"] - CLOCK_SKEW)
        for now in [feed["expiresAt"], feed["issuedAt"] - CLOCK_SKEW - 1]:
            with self.assertRaises(ValueError): self.verify(now=now)

    def test_duplicate_excessive_and_malformed_signatures(self):
        valid = self.fixture["document"]["authentication"]["signatures"][0]
        for signatures in [[], [valid, valid], [valid] * 9, [{}], [{**valid, "signature": "not base64"}],
                           [{**valid, "signature": base64.b64encode(b"x" * 63).decode()}]]:
            changed = copy.deepcopy(self.fixture["document"])
            changed["authentication"]["signatures"] = signatures
            with self.assertRaises(ValueError): self.verify(changed)

    def test_payload_is_verified_as_bytes_not_reserialized_json(self):
        changed = copy.deepcopy(self.fixture["document"])
        auth = changed["authentication"]
        auth["payload"] = base64.b64encode(base64.b64decode(auth["payload"]) + b"\n").decode()
        with self.assertRaises(ValueError): self.verify(changed)

    def test_rotation_signatures_allow_each_generation_of_trusted_keys(self):
        with tempfile.TemporaryDirectory() as temp:
            paths, keys = [], {}
            for name in ["old-key", "new-key"]:
                path = Path(temp) / (name + ".pem")
                subprocess.run(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt",
                                "ec_paramgen_curve:prime256v1", "-out", str(path)],
                               check=True, capture_output=True)
                keys[name] = public_key(path); paths.append(path)
            source = {k: v for k, v in self.fixture["document"].items()
                      if k not in {"authentication", "issuedAt", "expiresAt"}}
            document = sign_manifest(source, paths, now=self.fixture["now"], trusted_keys=keys)
            for name, public in keys.items(): self.verify(document, keys={name: public})
            with self.assertRaises(ValueError):
                sign_manifest(source, paths, now=self.fixture["now"], validity=MAX_VALIDITY + 1, trusted_keys=keys)
            with self.assertRaises(ValueError): sign_manifest(source, paths, trusted_keys={})


if __name__ == "__main__": unittest.main()
