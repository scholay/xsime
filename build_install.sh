#!/bin/bash
# Builds RIMES as a SELF-CONTAINED IMK input method: librime + the Rime shared
# data are packaged inside, so no separate Squirrel install is needed. Installs into the per-user Input Methods folder and
# registers + enables + selects it so it shows in System Settings / the input menu.
#
# IMPORTANT invariants (learned the hard way — see docs/release/README.md):
#   * Bundle, executable, identifier and display name are all RIMES. Older
#     installs under ETInput.app / RimeBuffer.app advertise the same input
#     source, so they are deregistered and removed here rather than left to
#     compete for the same TIS identity.
#   * There must be EXACTLY ONE bundle with this id on disk. A stray copy (e.g.
#     left in the repo working tree) registers the same input-source id at a
#     second path and poisons TIS/LaunchServices → blank/greyed picker row. So
#     we assemble in a throwaway staging dir and delete it after installing.
#
# (The SPM target / source dir stay named "RimeBuffer" — internal code symbols;
# the shipped product is RIMES.)
set -euo pipefail
cd "$(dirname "$0")"
source scripts/lib/rime-user-state.sh
python3 scripts/prepare-official-plugins.py

CONFIG="${1:-release}"
APP="RIMES.app"                     # One name everywhere: bundle, executable, display.
EXE="RIMES"
LEGACY_APPS=("ETInput.app" "RimeBuffer.app")
STAGE=".build/stage"                # assemble here, not in the repo root
APP_PATH="$STAGE/$APP"
DEST="$HOME/Library/Input Methods/$APP"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
COMPANION_AGENT_HELPER="scripts/pkg/scripts/rimes-companion-launch-agent.sh"
SYSTEM_COMPANION_AGENT="/Library/LaunchAgents/com.scholay.rimes.companion-start.plist"
USER_COMPANION_AGENT="$HOME/Library/LaunchAgents/com.scholay.rimes.dev-companion-start.plist"
COMPANION_AGENT_BACKUP=""
COMPANION_AGENT_HAD_PREVIOUS=0
COMPANION_AGENT_CHANGED=0

if [ ! -r "$COMPANION_AGENT_HELPER" ] \
    || ! /bin/bash -n "$COMPANION_AGENT_HELPER"; then
    echo "!! companion LaunchAgent helper is missing or invalid"
    exit 1
fi

# A system-wide release copy and this per-user dev copy would advertise the
# same bundle/input-source IDs. Stop before creating that poisoned duplicate;
# remove the pkg-installed copy explicitly before returning to dev installs.
for system_copy in "/Library/Input Methods/RIMES.app" \
                   "/Library/Input Methods/ETInput.app" \
                   "/Library/Input Methods/RimeBuffer.app" \
                   "/Library/Input Methods/Enter输入法.app" \
                   "/Library/Input Methods/恩特输入法.app"; do
    [ -e "$system_copy" ] || continue
    system_id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$system_copy/Contents/Info.plist" 2>/dev/null || true)"
    case "$system_id" in
        com.scholay.inputmethod.isaac|com.scholay.isaac|com.isaac.inputmethod.RimeBuffer|com.isaac.inputmethod.ETInput)
            echo "!! found system-wide duplicate: $system_copy"
            echo "   remove it first: sudo rm -rf '$system_copy'"
            exit 1
            ;;
    esac
done
if [ -e "$SYSTEM_COMPANION_AGENT" ] || [ -L "$SYSTEM_COMPANION_AGENT" ]; then
    echo "!! found the release-package companion LaunchAgent: $SYSTEM_COMPANION_AGENT"
    echo "   remove the system package and its agent before returning to a per-user dev install"
    echo "   sudo /bin/bash '$COMPANION_AGENT_HELPER' remove-system"
    exit 1
fi

# IMK discovery requires an inputmethod segment even when code signing succeeds.
BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' Info.plist)"
case "$BUNDLE_ID" in
    *.inputmethod.*) ;;
    *) echo "!! input-method bundle identifier must contain .inputmethod.: $BUNDLE_ID"; exit 1 ;;
esac

# Fetch the bundled librime runtime (cached in Vendor/, not committed to git).
./scripts/fetch-rime.sh

# Its OWN Rime user dir (never fights Squirrel over the userdb LevelDB lock).
# IMPORT: if you have a live ~/Library/Rime (Squirrel), carry your real config
# in — your schemes, learned userdb, custom_phrase, lua, dicts — so RIMES uses
# your actual setup. We then force RIMES's five ordinary product schemas
# (雾凇全拼、自然码双拼、小鹤双拼、五笔86、英文) + 9 candidates; the optional
# 并击 schema is added only when its extension is enabled. Everything else you
# have is preserved. With no ~/Library/Rime, the app deploys from the bundled
# schemas instead. RB_KEEP_USERDB=1 skips reseeding.
RB_USER="$HOME/Library/RIMES"
RB_LEGACY_USER="$HOME/Library/RimeBuffer"
if [ -L "$RB_USER" ]; then
    echo "!! refusing to update symlinked RIMES user directory: $RB_USER"
    exit 1
fi
# Data from before the RIMES rename moves the way the app's first launch moves
# it (RimesDataMigration): copied once into an absent or empty directory,
# marked, original left in place. It has to happen here, because seeding below
# creates $RB_USER and the app then refuses to merge into an occupied one.
RB_MIGRATION_MARKER="$RB_USER/.rimes-migrated-from-rimebuffer"
if [ -d "$RB_LEGACY_USER" ] && [ ! -L "$RB_LEGACY_USER" ] && [ ! -e "$RB_MIGRATION_MARKER" ]; then
    if [ ! -e "$RB_USER" ] || [ -z "$(ls -A "$RB_USER")" ]; then
        echo "==> copying pre-rename data $RB_LEGACY_USER -> $RB_USER (original left in place)"
        /usr/bin/ditto "$RB_LEGACY_USER" "$RB_USER"
        : > "$RB_MIGRATION_MARKER"
    else
        echo "!! $RB_USER already holds data; pre-rename data stays in $RB_LEGACY_USER"
    fi
fi
if [ "${RB_KEEP_USERDB:-0}" != "1" ]; then
    if [ -d "$HOME/Library/Rime" ]; then
        echo "==> importing your ~/Library/Rime into $RB_USER (schemes, userdb, custom_phrase, lua…)"
        import_rime_user_dir_preserving_product_state "$HOME/Library/Rime" "$RB_USER"
    else
        echo "==> no ~/Library/Rime; deploying from the bundled schemas"
        reset_rime_user_dir_preserving_product_state "$RB_USER"
    fi
    # Enforce RIMES's five ordinary product schemas + 9 candidates. The optional
    # chord schema is reconciled by the extension state at startup. Your learned
    # userdb and unrelated tweaks are kept.
    cp rime-data/default.custom.yaml "$RB_USER/default.custom.yaml"
fi

# Product-owned schemas must advance even when the learned userdb is kept.
# Rime gives a root user-data schema precedence over the app's SharedSupport
# copy, and older installs imported exactly such a my_combo.schema.yaml from
# Squirrel.  Keep user customisations in my_combo.custom.yaml (Rime's standard
# overlay); refresh only the versioned base schema here.
mkdir -p "$RB_USER"
install -m 0644 rime-data/my_combo.schema.yaml "$RB_USER/my_combo.schema.yaml"

# GRDB 7.11 requires a newer toolchain than the Command Line Tools Swift on
# some supported Macs. Allow CI/developers to pin one, otherwise prefer the
# keg-only Homebrew Swift when present and fall back to the active Xcode Swift.
if [[ -n "${RB_SWIFT_BIN:-}" ]]; then
    RIMES_SWIFT_BIN="$RB_SWIFT_BIN"
elif [[ -x /opt/homebrew/opt/swift/bin/swift ]]; then
    RIMES_SWIFT_BIN=/opt/homebrew/opt/swift/bin/swift
elif [[ -x /usr/local/opt/swift/bin/swift ]]; then
    RIMES_SWIFT_BIN=/usr/local/opt/swift/bin/swift
else
    RIMES_SWIFT_BIN="$(command -v swift || true)"
fi
if [[ -z "$RIMES_SWIFT_BIN" || ! -x "$RIMES_SWIFT_BIN" ]]; then
    echo "!! no usable Swift toolchain found"
    exit 1
fi

# Homebrew/upstream Swift may compile against the selected CLT SDK while
# recording the deployment target as both minOS and SDK in LC_BUILD_VERSION.
# AppKit uses that SDK field for linked-on-or-after rendering behavior, so make
# both platform versions explicit from their authoritative sources.
RIMES_PACKAGE_DESCRIPTION="$("$RIMES_SWIFT_BIN" package dump-package)"
RIMES_PACKAGE_PLATFORM="$(
    printf '%s' "$RIMES_PACKAGE_DESCRIPTION" |
        /usr/bin/plutil -extract platforms.0.platformName raw -o - -
)"
RIMES_DEPLOYMENT_TARGET="$(
    printf '%s' "$RIMES_PACKAGE_DESCRIPTION" |
        /usr/bin/plutil -extract platforms.0.version raw -o - -
)"
RIMES_MACOS_SDK_PATH="$(/usr/bin/xcrun --sdk macosx --show-sdk-path)"
RIMES_MACOS_SDK_VERSION="$(/usr/bin/xcrun --sdk macosx --show-sdk-version)"

if [[ "$RIMES_PACKAGE_PLATFORM" != "macos" ||
      ! "$RIMES_DEPLOYMENT_TARGET" =~ ^[0-9]+([.][0-9]+){1,2}$ ||
      ! "$RIMES_MACOS_SDK_VERSION" =~ ^[0-9]+([.][0-9]+){1,2}$ ]]; then
    echo "!! could not resolve macOS deployment/SDK versions safely"
    exit 1
fi

echo "==> swift build ($CONFIG, macOS $RIMES_DEPLOYMENT_TARGET / SDK $RIMES_MACOS_SDK_VERSION)"
"$RIMES_SWIFT_BIN" build -c "$CONFIG" \
    --sdk "$RIMES_MACOS_SDK_PATH" \
    -Xlinker -platform_version \
    -Xlinker macos \
    -Xlinker "$RIMES_DEPLOYMENT_TARGET" \
    -Xlinker "$RIMES_MACOS_SDK_VERSION"

BIN=".build/$CONFIG/RimeBuffer"
RIMES_BUILD_VERSION_INFO="$(/usr/bin/vtool -show-build "$BIN")"
RIMES_BUILT_MIN_OS="$(
    printf '%s\n' "$RIMES_BUILD_VERSION_INFO" |
        awk '$1 == "minos" { print $2; exit }'
)"
RIMES_BUILT_SDK="$(
    printf '%s\n' "$RIMES_BUILD_VERSION_INFO" |
        awk '$1 == "sdk" { print $2; exit }'
)"
if [[ "$RIMES_BUILT_MIN_OS" != "$RIMES_DEPLOYMENT_TARGET" ||
      "$RIMES_BUILT_SDK" != "$RIMES_MACOS_SDK_VERSION" ]]; then
    echo "!! invalid LC_BUILD_VERSION: minOS=$RIMES_BUILT_MIN_OS SDK=$RIMES_BUILT_SDK"
    exit 1
fi

echo "==> assembling $APP (in $STAGE)"
rm -rf "$APP_PATH"
mkdir -p "$APP_PATH/Contents/MacOS" "$APP_PATH/Contents/Resources" \
         "$APP_PATH/Contents/Frameworks" "$APP_PATH/Contents/SharedSupport"
cp "$BIN" "$APP_PATH/Contents/MacOS/$EXE"
# Ship the music sample bank and dependency privacy manifests inside the signed
# Resources directory; the installed instrument resolves its asset here.
for resource_bundle in ".build/$CONFIG/"*.bundle; do
    [ -d "$resource_bundle" ] || continue
    cp -R "$resource_bundle" "$APP_PATH/Contents/Resources/"
done
cp Info.plist "$APP_PATH/Contents/Info.plist"

# Bump CFBundleVersion on the installed copy each build so LaunchServices/TIS
# re-read the bundle's metadata instead of serving a stale cache. (Source
# Info.plist is untouched, so git stays clean.)
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $(date +%s)" "$APP_PATH/Contents/Info.plist" 2>/dev/null || true
# Development builds keep the 1.0 product anchor and a separate source suffix.
# The updater excludes nonnumeric versions; only the signed release pipeline
# stamps a plain VERSION value after checking its exact release tag.
product_version="$(cat VERSION)"
dev_commit="$(git rev-parse --short=12 HEAD)"
dev_version="${product_version}-dev.${dev_commit}"
if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
    dev_version="${dev_version}.dirty"
fi
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $dev_version" \
    "$APP_PATH/Contents/Info.plist"
# A complete .app has a PkgInfo (both Squirrel and Sogou ship one).
printf 'APPL????' > "$APP_PATH/Contents/PkgInfo"

# Bundle the self-contained runtime: librime + plugins + Rime shared data.
cp -R Vendor/rime/Frameworks/* "$APP_PATH/Contents/Frameworks/"
cp -R Vendor/rime/SharedSupport/* "$APP_PATH/Contents/SharedSupport/"

# Ship no unrelated stock input schemes. The two non-product schemas copied
# below (melt_eng/radical_pinyin) are required hidden dependencies and are not
# present in schema_list/F4.
find "$APP_PATH/Contents/SharedSupport" -maxdepth 1 -type f -name '*.schema.yaml' -delete

# Overlay OUR Rime schemas (并击、自然码双拼、雾凇拼音、英文，以及它们的隐藏依赖)
# onto the stock SharedSupport so a fresh install deploys the real schemas — not
# just default luna_pinyin. This works WITHOUT a separate Squirrel/~/Library/Rime. The secret
# rime_ai.local.json is intentionally NOT bundled (only rime_ai.example.json).
cp -R rime-data/* "$APP_PATH/Contents/SharedSupport/"

# App icon, if it's been generated.
if [ -f "Logo/AppIcon.icns" ]; then
    cp "Logo/AppIcon.icns" "$APP_PATH/Contents/Resources/AppIcon.icns"
fi

# Localized input-source display name (RIMES) + the input-mode menu icon.
# Without the .lproj the source shows its raw id; without the icon it renders as
# a blank row and won't enable.
cp -R Resources/*.lproj "$APP_PATH/Contents/Resources/" 2>/dev/null || true
cp Resources/etinput.pdf "$APP_PATH/Contents/Resources/" 2>/dev/null || true
cp Resources/etinput-menu.pdf "$APP_PATH/Contents/Resources/" 2>/dev/null || true
cp Resources/menubar-template.png "$APP_PATH/Contents/Resources/" 2>/dev/null || true
cp LICENSE NOTICE LICENSING.md ATTRIBUTION.md THIRD_PARTY_NOTICES.md README.md README.en.md README.ja.md README.ko.md README.es.md "$APP_PATH/Contents/Resources/"
cp -R LICENSES "$APP_PATH/Contents/Resources/"

# Ad-hoc sign. --deep now that we have nested dylibs (librime + plugins).
# Signing identity decides whether permissions survive a rebuild, which is
# not obvious from the outside. An ad-hoc signature's designated requirement
# is a cdhash — a new value every build — so macOS stops matching the recorded
# Accessibility grant while System Settings still shows the checkbox ticked.
# An Apple Development requirement names the certificate instead, with no
# cdhash in it, and the grant persists across rebuilds. This developer lane
# intentionally never picks a Developer ID identity: that certificate belongs
# only to the formal package/release lane.
SIGN_IDENTITY="${RIMES_SIGN_IDENTITY:-}"
if [ -n "$SIGN_IDENTITY" ]; then
    case "$SIGN_IDENTITY" in
        "Apple Development:"*) ;;
        *)
            echo "!! RIMES_SIGN_IDENTITY must name an Apple Development identity."
            echo "   Developer ID identities are reserved for formal release packages."
            exit 1
            ;;
    esac
else
    SIGN_IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
        | /usr/bin/awk '$2 ~ /^[0-9A-F]{40}$/ && /"Apple Development: / { print $2; exit }')"
fi
if [ -n "$SIGN_IDENTITY" ]; then
    echo "==> signing development install (deep) with $SIGN_IDENTITY"
    codesign --force --deep --sign "$SIGN_IDENTITY" "$APP_PATH"
else
    echo "==> ad-hoc signing (deep) — no Apple Development identity found"
    echo "   Accessibility grants will not survive a rebuild. Set"
    echo "   RIMES_SIGN_IDENTITY to an Apple Development identity, or add one."
    codesign --force --deep --sign - "$APP_PATH"
fi

echo "==> handing active clients to a safe fallback input source"
if ! /bin/launchctl asuser "$(id -u)" "$APP_PATH/Contents/MacOS/$EXE" --prepare-update; then
    echo "!! could not leave the active RIMES source safely; refusing a hot replacement"
    exit 1
fi
sleep 0.5

echo "==> purging stray/duplicate registrations (same id at other paths poisons the picker)"
pkill -x "$EXE" 2>/dev/null || true
pkill -x RimeBuffer 2>/dev/null || true
# The previous install runs under its old executable name and is respawned by
# the text-input system while its input source is still selected, so it has to
# be stopped here too rather than only detected.
for legacy_exe in "${LEGACY_APPS[@]}"; do
    pkill -x "${legacy_exe%.app}" 2>/dev/null || true
done
sleep 1
if /usr/bin/pgrep -U "$(id -u)" -x "$EXE" >/dev/null 2>&1 \
    || /usr/bin/pgrep -U "$(id -u)" -x RimeBuffer >/dev/null 2>&1 \
    || /usr/bin/pgrep -U "$(id -u)" -x ETInput >/dev/null 2>&1; then
    echo "!! existing RIMES process did not stop; refusing a live bundle replacement"
    exit 1
fi
# Any leftover copies in the repo tree or a previous CJK-named install.
for stray in "Enter输入法.app" "恩特输入法.app" "ETInput.app" "RimeBuffer.app" \
             "$HOME/Library/Input Methods/Enter输入法.app" \
             "$HOME/Library/Input Methods/恩特输入法.app" \
             "$HOME/Library/Input Methods/RimeBuffer.app" \
             "$HOME/Library/Input Methods/ETInput.app" \
             "$HOME/Documents/05-dev/apps/rime-buffer/RimeBuffer.app"; do
    if [ -e "$stray" ]; then
        "$LSREGISTER" -u "$stray" 2>/dev/null || true
        rm -rf "$stray"
        echo "    removed stray: $stray"
    fi
done

echo "==> staging the new install beside $DEST"
mkdir -p "$HOME/Library/Input Methods"
DEST_NEW="$DEST.new"
DEST_BACKUP="$DEST.bak"
rm -rf "$DEST_NEW" "$DEST_BACKUP"
if ! cp -R "$APP_PATH" "$DEST_NEW"; then
    echo "!! failed to stage the new bundle; keeping the current install"
    exit 1
fi
rm -rf "$STAGE/$APP"                 # don't leave a staging copy lying around

restore_previous_install() {
    echo "!! restoring the previous RIMES installation"
    # The new bundle may already have registered or selected its TIS mode if a
    # later shell/runtime failure interrupted the install. While its executable
    # still exists, hand the session to a safe fallback before removing it.
    if [ -x "$DEST/Contents/MacOS/$EXE" ]; then
        if ! /bin/launchctl asuser "$(id -u)" \
                "$DEST/Contents/MacOS/$EXE" --prepare-update \
                >>"$HOME/rimebuffer-install.log" 2>&1; then
            echo "!! could not move TIS to a safe fallback; keeping the valid new bundle in place"
            return 1
        fi
    fi
    if [ "$COMPANION_AGENT_CHANGED" -eq 1 ]; then
        if [ "$COMPANION_AGENT_HAD_PREVIOUS" -eq 1 ] \
            && [ -n "$COMPANION_AGENT_BACKUP" ]; then
            if ! /bin/mv -f "$COMPANION_AGENT_BACKUP" \
                    "$USER_COMPANION_AGENT" 2>/dev/null; then
                echo "!! failed to restore the previous companion LaunchAgent"
            fi
            COMPANION_AGENT_BACKUP=""
        else
            /bin/bash "$COMPANION_AGENT_HELPER" remove-user \
                2>/dev/null || true
        fi
        COMPANION_AGENT_CHANGED=0
    fi
    pkill -x "$EXE" 2>/dev/null || true
    "$LSREGISTER" -u "$DEST" 2>/dev/null || true
    rm -rf "$DEST"
    if [ -e "$DEST_BACKUP" ]; then
        mv "$DEST_BACKUP" "$DEST"
        "$LSREGISTER" -f "$DEST" 2>/dev/null || true
        /bin/launchctl asuser "$(id -u)" "$DEST/Contents/MacOS/$EXE" --install >> "$HOME/rimebuffer-install.log" 2>&1 || true
        open -g "$DEST" 2>/dev/null || true
    fi
    rm -rf "$DEST_NEW"
}

snapshot_companion_agent() {
    local agent_dir snapshot

    agent_dir="$(dirname "$USER_COMPANION_AGENT")"
    if [ -L "$USER_COMPANION_AGENT" ]; then
        echo "!! refusing a symlinked development companion LaunchAgent"
        return 1
    fi
    if [ -e "$USER_COMPANION_AGENT" ]; then
        [ -f "$USER_COMPANION_AGENT" ] || return 1
        snapshot="$(
            /usr/bin/mktemp "$agent_dir/.rimes-companion-build-backup.XXXXXX"
        )" || return 1
        if ! /bin/cp -p "$USER_COMPANION_AGENT" "$snapshot"; then
            /bin/rm -f "$snapshot"
            return 1
        fi
        if ! /usr/bin/cmp -s "$USER_COMPANION_AGENT" "$snapshot"; then
            /bin/rm -f "$snapshot"
            return 1
        fi
        COMPANION_AGENT_BACKUP="$snapshot"
        COMPANION_AGENT_HAD_PREVIOUS=1
    fi
}

wait_for_installed_app_process() {
    local expected="$DEST/Contents/MacOS/$EXE"
    local attempts=0
    local stable_matches=0
    local pids pid running_command found

    while [ "$attempts" -lt 20 ]; do
        pids="$(
            /usr/bin/pgrep -U "$(id -u)" -x "$EXE" 2>/dev/null || true
        )"
        found=0
        for pid in $pids; do
            case "$pid" in
                ""|*[!0-9]*) continue ;;
            esac
            running_command="$(
                /bin/ps -p "$pid" -o command= 2>/dev/null || true
            )"
            case "$running_command" in
                "$expected"|"$expected -psn_"*) found=1; break ;;
            esac
        done
        if [ "$found" -eq 1 ]; then
            stable_matches=$((stable_matches + 1))
            [ "$stable_matches" -ge 2 ] && return 0
        else
            stable_matches=0
        fi
        attempts=$((attempts + 1))
        /bin/sleep 0.25
    done
    return 1
}

echo "==> atomically swapping the installed bundle"
"$LSREGISTER" -u "$DEST" 2>/dev/null || true
if [ -e "$DEST" ] && ! mv "$DEST" "$DEST_BACKUP"; then
    echo "!! could not move the current bundle aside"
    rm -rf "$DEST_NEW"
    exit 1
fi
if ! mv "$DEST_NEW" "$DEST"; then
    echo "!! could not activate the staged bundle"
    restore_previous_install
    exit 1
fi

echo "==> registering the single installed copy with Launch Services"
"$LSREGISTER" -f "$DEST" || true

echo "==> installing one-shot login bootstrap for companion shortcuts"
if ! snapshot_companion_agent; then
    echo "!! could not snapshot the existing companion LaunchAgent"
    restore_previous_install
    exit 1
fi
COMPANION_AGENT_CHANGED=1
if ! /bin/bash "$COMPANION_AGENT_HELPER" install-user; then
    echo "!! could not install the RIMES companion LaunchAgent"
    restore_previous_install
    exit 1
fi
open -g "$DEST" 2>/dev/null || true  # LaunchServices can report late success.
if ! wait_for_installed_app_process; then
    echo "!! installed RIMES did not remain running from the canonical bundle"
    restore_previous_install
    exit 1
fi

# Do not select the new TIS source until every later fail-closed lifecycle check
# has passed. A failed fresh install can then restore/remove the bundle without
# ever leaving the user's current input source pointing at deleted bytes.
echo "==> self-install: register + enable + select inside the login session"
INSTALL_LOG="$HOME/rimebuffer-install.log"
ACTIVATION_READY=1
if ! /bin/launchctl asuser "$(id -u)" \
        "$DEST/Contents/MacOS/$EXE" --install 2>&1 | tee "$INSTALL_LOG"; then
    # The bundle is already valid, resident, and has its login bootstrap.
    # Recent macOS releases can require a session refresh before TIS exposes a
    # newly registered source, so activation remains an explicit nonfatal tail.
    ACTIVATION_READY=0
    echo "!! input-source activation is pending a logout/login session refresh"
fi
[ -z "$COMPANION_AGENT_BACKUP" ] || /bin/rm -f "$COMPANION_AGENT_BACKUP"
COMPANION_AGENT_BACKUP=""
COMPANION_AGENT_CHANGED=0
rm -rf "$DEST_BACKUP"

if [ "$ACTIVATION_READY" -eq 1 ]; then
    activation_summary="Installed, registered, and enabled RIMES."
else
    activation_summary="Installed RIMES; registration/enablement is pending session refresh."
fi

cat <<EOF

==> done. $activation_summary

If RIMES doesn't appear in the input menu (⌃Space) immediately, run:
  log out and back in once, then add RIMES in System Settings if needed.
After switching to it, press F4 to choose an input scheme.

macOS 27: switching to RIMES in sandboxed WeChat/QQ has been reported to crash
the host app. If affected, use another input method and see:
  https://github.com/scholay/xsime/blob/main/docs/macos/input-source-cache-recovery.md
App caches were not cleared. Restarting/logging out is not a guaranteed fix (#90).

Watch behaviour:  tail -f ~/rimebuffer.log
Self-contained: librime + Rime data are bundled, no Squirrel needed.
EOF
