#!/bin/bash
# Build a signed release locally. This does not install, upload or publish it.
set -euo pipefail
cd "$(dirname "$0")/.."

for name in RIMES_ANDROID_KEYSTORE RIMES_ANDROID_STORE_PASSWORD RIMES_ANDROID_KEY_ALIAS RIMES_ANDROID_KEY_PASSWORD; do
    if [[ -z "${!name:-}" ]]; then
        echo "Missing $name; refusing to label an unsigned APK as a release." >&2
        exit 2
    fi
done
[[ -f "$RIMES_ANDROID_KEYSTORE" ]] || { echo 'Release keystore does not exist.' >&2; exit 2; }
: "${ANDROID_HOME:?Set ANDROID_HOME to the Android SDK directory}"
tools="$ANDROID_HOME/build-tools/36.0.0"
[[ -x "$tools/apksigner" && -x "$tools/zipalign" ]] || { echo 'Android build-tools 36.0.0 are required.' >&2; exit 2; }

./gradlew --no-daemon :core:test :app:lintRelease :app:assembleRelease :app:bundleRelease
apk=app/build/outputs/apk/release/app-release.apk
aab=app/build/outputs/bundle/release/app-release.aab
python3 scripts/verify-release.py "$apk" "$aab" --sdk "$tools"
version="$(cat VERSION)"
out="dist/$version"
[[ ! -e "$out" ]] || { echo 'Output exists; retain it and choose a fresh build workspace.' >&2; exit 2; }
mkdir -p "$out"
cp "$apk" "$out/RIMES-Android-$version.apk"
cp "$aab" "$out/RIMES-Android-$version.aab"
python3 - "$out" <<'PY'
import hashlib, sys
from pathlib import Path
out = Path(sys.argv[1])
files = sorted(out.glob('RIMES-Android-*'))
(out / 'SHA256SUMS').write_text(''.join(
    f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in files))
print(f'Signed artifacts staged in {out}; device acceptance and publication remain separate.')
PY
