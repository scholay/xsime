#!/usr/bin/env bash
# Build and install the Fcitx5 RIMES addon plus reviewed rime-data.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
IME_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$IME_ROOT/../../.." && pwd -P)
if [[ ! -f "$REPO_ROOT/scripts/platform-preview/preview.py" ]]; then
    echo "error: could not locate the repository root from $IME_ROOT" >&2
    exit 1
fi

PREFIX="${RIMES_IME_PREFIX:-$HOME/.local}"
BUILD_DIR="${RIMES_IME_BUILD_DIR:-$IME_ROOT/build}"
STAGE_DIR="${RIMES_IME_STAGE_DIR:-}"

usage() {
    cat <<'EOF'
Install the RIMES Fcitx5 addon and its reviewed rime-data tree.

Usage:
  install.sh [--prefix DIR] [--build-dir DIR] [--skip-build]

Default prefix is $HOME/.local. System-wide install:
  sudo RIMES_IME_PREFIX=/usr install.sh
An already-built tree can use --skip-build with the same configured prefix.
Uninstall: uninstall.sh [--prefix DIR] [--dry-run]
Debian/Ubuntu users should normally install the release .deb with apt instead.
EOF
}

SKIP_BUILD=0
while (($# > 0)); do
    case "$1" in
        --prefix)
            (($# >= 2)) && [[ -n "$2" && "$2" != --* ]] || { echo "--prefix requires a directory" >&2; exit 2; }
            PREFIX=$2
            shift 2
            ;;
        --build-dir)
            (($# >= 2)) && [[ -n "$2" && "$2" != --* ]] || { echo "--build-dir requires a directory" >&2; exit 2; }
            BUILD_DIR=$2
            shift 2
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if (( SKIP_BUILD == 0 )); then
    RIMES_IME_PREFIX="$PREFIX" RIMES_IME_BUILD_DIR="$BUILD_DIR" \
        "$SCRIPT_DIR/build.sh"
fi

WORK_DIR=$(mktemp -d /tmp/rimes-ime-install.XXXXXX)
trap 'rm -rf "$WORK_DIR"' EXIT

if [[ -z "$STAGE_DIR" ]]; then
    STAGE_DIR="$WORK_DIR/data"
    python3 "$REPO_ROOT/scripts/platform-preview/preview.py" stage \
        --repo-root "$REPO_ROOT" \
        --output-dir "$STAGE_DIR"
fi

# Stage CMake and data together before changing the live prefix. The persistent
# ownership manifest includes both sets, and survives deletion of the build tree.
DESTDIR="$WORK_DIR/root" cmake --install "$BUILD_DIR" --prefix "$PREFIX"
DATA_DEST="$WORK_DIR/root${PREFIX}/share/rimes/data"
mkdir -p "$DATA_DEST"
cp -a "$STAGE_DIR"/. "$DATA_DEST/"
python3 "$SCRIPT_DIR/install_state.py" install --prefix "$PREFIX" \
    --stage "$WORK_DIR/root" --build-dir "$BUILD_DIR"

cat <<EOF
Installed the RIMES Fcitx5 addon, Buffer, Capsule and Mailbox workspaces.
  prefix:  $PREFIX
  addon:   $PREFIX/lib/*/fcitx5/rimes.so (or lib64)
  buffer:  $PREFIX/libexec/rimes/rimes-buffer and $PREFIX/bin/rimes-buffer
  mailbox: $PREFIX/bin/rimes-mailbox
  capsule: $PREFIX/libexec/rimes/rimes-capsule and $PREFIX/bin/rimes-capsule
  ctl:     $PREFIX/bin/rimes-buffer-ctl and $PREFIX/bin/rimes-capsule-ctl
  deploy:  $PREFIX/libexec/rimes/rimes-deploy
  data:    $PREFIX/share/rimes/data
  receipt: $PREFIX/share/rimes/ime-install-manifest.json
Uninstall with: $SCRIPT_DIR/uninstall.sh --prefix "$PREFIX"
Restart Fcitx5 (fcitx5 -r) and add "RIMES" in the input method list.
Toggle Buffer with Ctrl+Shift+B and Capsule with Ctrl+Shift+V while RIMES is current.
User schema/state lives in \$XDG_DATA_HOME/rimes, isolated from fcitx5-rime.
EOF
