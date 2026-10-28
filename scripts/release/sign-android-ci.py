#!/usr/bin/env python3
"""Sign CI-tested Android outputs without exposing release credentials."""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from release_guard import verify_android_build

CERTIFICATE = "e6228d5b065f1f8c2f1e1162b80ea1a9245ce59a091bb6e403e47936c3820c54"


def main():
    root = Path(__file__).resolve().parents[2]
    version = (root / "VERSION").read_text().strip()
    assert re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", version)
    assert os.environ["GITHUB_REF_NAME"] == "v" + version
    code = int(re.search(r"versionCode = (\d+)", (root / "platforms/android/app/build.gradle.kts").read_text())[1])
    # Reject an un-upgradeable identity before importing the production key.
    verify_android_build(version, code)
    source = Path(os.environ["RUNNER_TEMP"]) / "android-unsigned"
    output = Path(os.environ["RUNNER_TEMP"]) / "android-signed"
    output.mkdir(exist_ok=False)
    apks, aabs = list(source.rglob("*unsigned.apk")), list(source.rglob("*.aab"))
    assert len(apks) == len(aabs) == 1
    sdk = Path(os.environ["ANDROID_HOME"]) / "build-tools/36.0.0"
    apk, aab = output / f"RIMES-Android-{version}.apk", output / f"RIMES-Android-{version}.aab"
    def run(*arguments):
        return subprocess.check_output([str(value) for value in arguments], text=True, stderr=subprocess.STDOUT)
    with tempfile.TemporaryDirectory(prefix="rimes-signing-", dir=os.environ["RUNNER_TEMP"]) as temp:
        keystore = Path(temp) / "release.p12"
        keystore.write_bytes(base64.b64decode(os.environ["RIMES_ANDROID_KEYSTORE_BASE64"], validate=True))
        keystore.chmod(0o600)
        alias = os.environ["RIMES_ANDROID_KEY_ALIAS"]
        run(sdk / "zipalign", "-c", "-P", "16", "4", apks[0])
        run(sdk / "apksigner", "sign", "--ks", keystore, "--ks-key-alias", alias,
            "--ks-pass", "env:RIMES_ANDROID_STORE_PASSWORD", "--key-pass", "env:RIMES_ANDROID_KEY_PASSWORD",
            "--v4-signing-enabled", "false", "--out", apk, apks[0])
        run("jarsigner", "-keystore", keystore, "-storepass:env", "RIMES_ANDROID_STORE_PASSWORD",
            "-keypass:env", "RIMES_ANDROID_KEY_PASSWORD", "-sigalg", "SHA256withRSA", "-digestalg", "SHA-256",
            "-signedjar", aab, aabs[0], alias)
    signature = run(sdk / "apksigner", "verify", "--verbose", "--print-certs", apk)
    assert "Signer #1 certificate SHA-256 digest: " + CERTIFICATE in signature
    run(sdk / "zipalign", "-c", "-P", "16", "4", apk)
    assert "jar verified." in run("jarsigner", "-verify", aab)
    identity = run(sdk / "aapt2", "dump", "badging", apk).splitlines()[0]
    assert "name='org.scholay.rimes.android'" in identity
    assert f"versionCode='{code}'" in identity and f"versionName='{version}'" in identity
    run(sys.executable, root / "platforms/android/scripts/verify-release.py",
        apk, aab, "--sdk", sdk)
    hashes = {file.name: hashlib.sha256(file.read_bytes()).hexdigest() for file in [apk, aab]}
    (output / f"RIMES-Android-{version}-SHA256SUMS").write_text("".join(f"{digest}  {name}\n" for name, digest in hashes.items()))
    (output / f"RIMES-Android-{version}-BUILD-INFO.json").write_text(json.dumps({
        "version": version, "versionCode": code, "sourceCommit": os.environ["GITHUB_SHA"],
        "applicationId": "org.scholay.rimes.android", "signingCertificateSHA256": CERTIFICATE,
        "checksums": hashes, "validation": "Remote CI build, engine, emulator, alignment and production signature verification"
    }, indent=2) + "\n")
    print(f"Verified production-signed Android {version} ({code})")


if __name__ == "__main__":
    main()
