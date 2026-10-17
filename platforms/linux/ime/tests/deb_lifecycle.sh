#!/usr/bin/env bash
# This test installs/removes a system package. Run only on a disposable Linux runner.
set -euo pipefail
[[ "${RIMES_DEB_TEST_DISPOSABLE:-}" == 1 ]] || {
    echo "error: requires a disposable runner and RIMES_DEB_TEST_DISPOSABLE=1" >&2
    exit 1
}
[[ $# == 1 && -f "$1" ]] || { echo "Usage: deb_lifecycle.sh /path/to/package.deb" >&2; exit 2; }
DEB=$(realpath "$1")
if dpkg-query -W -f='${db:Status-Status}' fcitx5-rimes 2>/dev/null | grep -qx installed; then
    echo "error: refusing to replace an existing fcitx5-rimes installation" >&2
    exit 1
fi
[[ "$(dpkg-deb -f "$DEB" Package)" == fcitx5-rimes ]]
VERSION=$(dpkg-deb -f "$DEB" Version)
(cd "$(dirname "$DEB")" && sha256sum -c "$(basename "$DEB").sha256")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
DATA="$WORK/user-data/rimes"
mkdir -p "$DATA/rime_ice.userdb" "$DATA/schemes/learning/custom.userdb" "$DATA/capsule/clipboard" "$DATA/capsule/passwords" "$DATA/capsule/assets" "$DATA/capsule/captures/fixture" "$DATA/translation/cache" "$DATA/mailbox/images"
printf '%s\n' 'keep learned dictionary' > "$DATA/rime_ice.userdb/CURRENT"
printf '%s\n' 'keep imported learning' > "$DATA/schemes/learning/custom.userdb/CURRENT"
printf '%s\n' 'keep notes' > "$DATA/capsule/note.md"
printf '%s\n' 'keep original MIME history and favorites' > "$DATA/capsule/clipboard/history-v1.sqlite3"
printf '%s\n' 'keep downloaded model' > "$DATA/translation/cache/model.argosmodel"
printf '%s\n' 'keep imported reference attachment' > "$DATA/capsule/assets/reference.asset"
printf '%s\n' 'apa7' > "$DATA/capsule/reference-style-v1"
printf '%s\n' 'keep capture metadata' > "$DATA/capsule/captures/fixture/record.json"
printf '%s\n' 'keep original screen recording' > "$DATA/capsule/captures/fixture/original.webm"
printf '%s\n' 'keep existing encryption master key' > "$DATA/capsule/master-key"
printf '%s\n' 'keep salted reveal credential' > "$DATA/capsule/reveal-credential-v1"
printf '%s\n' 'keep encrypted password record' > "$DATA/capsule/passwords/fixture.md"
printf '%s\n' 'keep Mailbox threads and messages' > "$DATA/mailbox/mailbox.json"
printf '%s\n' 'keep Mailbox original attachment' > "$DATA/mailbox/images/fixture.png"
printf '%s\n' 'keep Mailbox window preferences' > "$DATA/mailbox/window.json"
(cd "$DATA" && find . -type f -exec sha256sum {} +) > "$WORK/user-data.sha256"

sudo env XDG_DATA_HOME="$WORK/user-data" apt-get install -y "$DEB"
[[ "$(dpkg-query -W -f='${Version}' fcitx5-rimes)" == "$VERSION" ]]
mapfile -t FILES < <(dpkg-query -L fcitx5-rimes)
[[ -x /usr/bin/rimes-schemes && -x /usr/bin/rimes-buffer && -x /usr/bin/rimes-capsule ]]
[[ -x /usr/bin/rimes-settings && -x /usr/bin/rimes-plugins && -x /usr/bin/rimes-mailbox ]]
[[ -f /usr/share/applications/org.scholay.rimes.mailbox.desktop ]]
/usr/bin/rimes-mailbox --version
[[ -x /usr/bin/rimes-translation && -f /usr/libexec/rimes/translation/resources.lock.json ]]
RIMES_USER_DIR="$WORK/resource-status" /usr/bin/rimes-translation --status | python3 -c 'import json,sys; assert json.load(sys.stdin)["state"] == "missing"'
[[ -f /usr/share/applications/org.scholay.rimes.settings.desktop ]]
/usr/bin/rimes-settings --help
[[ -f /usr/share/rimes/data/default.yaml ]]
for program in /usr/bin/rimes-mailbox /usr/bin/rimes-schemes /usr/bin/rimes-buffer /usr/bin/rimes-capsule /usr/bin/rimes-settings /usr/bin/rimes-plugins /usr/bin/rimes-translation /usr/libexec/rimes/rimes-capsule-preview /usr/libexec/rimes/rimes-capsule-capture; do
    ldd "$program" > "$WORK/ldd.txt"
    if grep -q 'not found' "$WORK/ldd.txt"; then
        cat "$WORK/ldd.txt" >&2
        exit 1
    fi
done
# Exercise the installed ELF and its resolved shared libraries, not only a build-tree binary.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
dbus-run-session -- xvfb-run -a python3 "$SCRIPT_DIR/../../capsule/tests/password_ui_test.py" \
    python3 "$SCRIPT_DIR/../../mailbox/tests/mailbox_process_test.py" /usr/bin/rimes-mailbox \
    "$SCRIPT_DIR/../../mailbox/tests/fixtures/native-mailbox.json"
sudo env XDG_DATA_HOME="$WORK/user-data" apt-get install --reinstall -y "$DEB"
sudo env XDG_DATA_HOME="$WORK/user-data" apt-get remove -y fcitx5-rimes
for file in "${FILES[@]}"; do
    [[ ! -f "$file" && ! -L "$file" ]] || { echo "Package file survived removal: $file" >&2; exit 1; }
done
(cd "$DATA" && sha256sum -c "$WORK/user-data.sha256")
echo "deb install/reinstall/remove passed; user dictionaries, learning and notes preserved."
