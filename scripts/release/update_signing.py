#!/usr/bin/env python3
"""P-256/SHA-256 authentication for the backwards-compatible update feed.

The exact UTF-8 payload bytes are signed. Native clients verify those bytes,
then require the signed JSON and legacy top-level fields to be identical.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
MAX_VALIDITY = 90 * 24 * 60 * 60
CLOCK_SKEW = 300
MAX_METADATA = 1024 * 1024
SPKI_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def load_keys():
    registry = json.loads((ROOT / "Shared/AppUpdate/trusted_keys.json").read_text())
    if registry.get("schemaVersion") != 1 or registry.get("algorithm") != "ecdsa-p256-sha256":
        raise ValueError("Unsupported public update trust registry")
    keys = {entry["id"]: entry["publicKey"] for entry in registry["keys"]}
    if not 1 <= len(keys) <= 8 or len(keys) != len(registry["keys"]):
        raise ValueError("Invalid trusted key set")
    return keys


def _b64(value):
    if not isinstance(value, str):
        raise ValueError("Expected base64 text")
    result = base64.b64decode(value, validate=True)
    if base64.b64encode(result).decode() != value:
        raise ValueError("Non-canonical base64")
    return result


def _raw_signature(der):
    # P-256 ECDSA DER signatures have short-form lengths and two positive INTEGERs.
    if len(der) < 8 or der[0] != 0x30 or der[1] != len(der) - 2:
        raise ValueError("Invalid ECDSA signature")
    position, values = 2, []
    for _ in range(2):
        if position + 2 > len(der) or der[position] != 2:
            raise ValueError("Invalid ECDSA integer")
        size = der[position + 1]
        value = der[position + 2:position + 2 + size]
        position += 2 + size
        if not 1 <= size <= 33 or len(value) != size or value[0] & 0x80:
            raise ValueError("Invalid ECDSA integer size")
        value = value.lstrip(b"\0")
        if len(value) > 32:
            raise ValueError("ECDSA integer outside P-256")
        values.append(value.rjust(32, b"\0"))
    if position != len(der):
        raise ValueError("Trailing ECDSA signature data")
    return b"".join(values)


def _der_signature(raw):
    if len(raw) != 64:
        raise ValueError("Expected a 64-byte P-256 signature")
    integers = []
    for value in (raw[:32], raw[32:]):
        value = value.lstrip(b"\0") or b"\0"
        if value[0] & 0x80:
            value = b"\0" + value
        integers.append(bytes([2, len(value)]) + value)
    body = b"".join(integers)
    return bytes([0x30, len(body)]) + body


def _openssl(*args, input=None):
    result = subprocess.run(["openssl", *map(str, args)], input=input, capture_output=True, timeout=30)
    if result.returncode:
        # Do not echo environment values, private paths, or OpenSSL input on failure.
        raise ValueError("Update signing cryptographic operation failed")
    return result.stdout


def public_key(private_path):
    data = _openssl("pkey", "-in", private_path, "-pubout", "-outform", "DER")
    if len(data) != len(SPKI_PREFIX) + 65 or not data.startswith(SPKI_PREFIX):
        raise ValueError("Update signing requires a P-256 key")
    return data[len(SPKI_PREFIX):].hex()


def _verify(payload, signature, public_hex):
    if not re.fullmatch(r"04[0-9a-f]{128}", public_hex):
        raise ValueError("Invalid trusted P-256 public key")
    with tempfile.TemporaryDirectory(prefix="xsime-update-verify-") as temp:
        root = Path(temp)
        (root / "public.der").write_bytes(SPKI_PREFIX + bytes.fromhex(public_hex))
        (root / "signature.der").write_bytes(_der_signature(signature))
        (root / "payload").write_bytes(payload)
        result = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(root / "public.der"),
                                 "-keyform", "DER", "-signature", str(root / "signature.der"), str(root / "payload")],
                                capture_output=True, timeout=30)
        return result.returncode == 0


def verify_manifest(document, *, now=None, trusted_keys=None):
    if not isinstance(document, dict) or len(canonical(document)) > MAX_METADATA:
        raise ValueError("Invalid update metadata")
    auth = document.get("authentication")
    if not isinstance(auth, dict) or type(auth.get("format")) is not int or auth.get("format") != 1 or auth.get("algorithm") != "ecdsa-p256-sha256":
        raise ValueError("A signed update feed is required")
    payload = _b64(auth.get("payload"))
    if len(payload) > MAX_METADATA:
        raise ValueError("Signed payload is too large")
    decoded = json.loads(payload)
    if not isinstance(decoded, dict) or decoded != {k: v for k, v in document.items() if k != "authentication"}:
        raise ValueError("Signed payload does not match update metadata")
    issued, expires = decoded.get("issuedAt"), decoded.get("expiresAt")
    current = int(time.time()) if now is None else now
    if type(issued) is not int or type(expires) is not int or not 0 <= issued < expires <= 253402300799:
        raise ValueError("Invalid update metadata validity interval")
    if issued > current + CLOCK_SKEW or expires <= current or expires - issued > MAX_VALIDITY:
        raise ValueError("Update metadata has expired or is not yet valid")
    signatures = auth.get("signatures")
    if not isinstance(signatures, list) or not 1 <= len(signatures) <= 8:
        raise ValueError("Missing or excessive update signatures")
    keys = load_keys() if trusted_keys is None else trusted_keys
    seen, valid = set(), False
    for item in signatures:
        if not isinstance(item, dict) or set(item) != {"keyId", "signature"}:
            raise ValueError("Invalid update signature record")
        key_id = item["keyId"]
        if not isinstance(key_id, str) or not re.fullmatch(r"[a-zA-Z0-9._-]{1,80}", key_id) or key_id in seen:
            raise ValueError("Invalid or duplicate update signing key")
        seen.add(key_id)
        signature = _b64(item["signature"])
        if len(signature) != 64:
            raise ValueError("Invalid update signature size")
        if key_id in keys and _verify(payload, signature, keys[key_id]):
            valid = True
    if not valid:
        raise ValueError("Update signature does not match a trusted publisher")
    return decoded


def sign_manifest(document, private_paths, *, now=None, validity=MAX_VALIDITY, trusted_keys=None):
    if "authentication" in document:
        raise ValueError("Refusing to resign an existing authenticated document")
    if type(validity) is not int or not 0 < validity <= MAX_VALIDITY:
        raise ValueError("Invalid signing lifetime")
    keys = load_keys() if trusted_keys is None else trusted_keys
    result = dict(document)
    result["issuedAt"] = int(time.time()) if now is None else now
    result["expiresAt"] = result["issuedAt"] + validity
    payload = canonical(result)
    signatures = []
    with tempfile.TemporaryDirectory(prefix="xsime-update-sign-") as temp:
        path = Path(temp) / "payload"
        path.write_bytes(payload)
        for private in private_paths:
            public = public_key(private)
            ids = [key_id for key_id, value in keys.items() if value == public]
            if len(ids) != 1:
                raise ValueError("Signing key does not match the reviewed public trust registry")
            signature = _raw_signature(_openssl("dgst", "-sha256", "-sign", private, "-binary", path))
            signatures.append({"keyId": ids[0], "signature": base64.b64encode(signature).decode()})
    result["authentication"] = {"format": 1, "algorithm": "ecdsa-p256-sha256",
                                "payload": base64.b64encode(payload).decode(), "signatures": signatures}
    verify_manifest(result, now=result["issuedAt"], trusted_keys=keys)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--sign", action="store_true")
    args = parser.parse_args()
    if args.manifest.is_symlink() or not args.manifest.is_file() or args.manifest.stat().st_size > MAX_METADATA:
        raise SystemExit("Expected a bounded regular manifest file")
    document = json.loads(args.manifest.read_bytes())
    if args.sign:
        with tempfile.TemporaryDirectory(prefix="xsime-release-keys-") as temp:
            keys = []
            for name in ["RIMES_UPDATE_SIGNING_KEY_PEM_BASE64", "RIMES_UPDATE_SIGNING_PREVIOUS_KEY_PEM_BASE64"]:
                value = os.environ.get(name)
                if not value:
                    if not keys:
                        raise SystemExit("Missing protected update signing key")
                    continue
                path = Path(temp) / str(len(keys))
                path.touch(mode=0o600)
                path.write_bytes(_b64(value))
                keys.append(path)
            document = sign_manifest(document, keys)
    else:
        verify_manifest(document)
    if args.output:
        args.output.write_bytes(canonical(document) + b"\n")
    print("Update metadata signature and validity verified")
