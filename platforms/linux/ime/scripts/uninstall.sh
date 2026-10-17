#!/usr/bin/env bash
# Remove a source installation of the complete Fcitx5 addon, preserving user data.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PREFIX="${RIMES_IME_PREFIX:-$HOME/.local}"
ARGS=(uninstall)

usage() {
    cat <<'EOF'
Uninstall a source-installed RIMES Fcitx5 addon, Buffer, Capsule, Mailbox and shared data.

Usage: uninstall.sh [--prefix DIR] [--dry-run] [--build-dir DIR]

Default prefix: $HOME/.local (or RIMES_IME_PREFIX).
  --dry-run        List the files without deleting anything.
  --build-dir DIR  Legacy installs only: use the original install_manifest.txt.
                   Shared data is checked against this source checkout.

System-wide: sudo uninstall.sh --prefix /usr
DESTDIR is supported for staged installations and tests.
User dictionaries, learning records, imported schemes, notes, Mailbox messages and attachments are preserved.
Modified installed files stop the uninstall; move or restore them and retry.
Deb packages must be removed with: sudo apt remove fcitx5-rimes
EOF
}

while (($# > 0)); do
    case "$1" in
        --prefix|--build-dir)
            if (($# < 2)) || [[ -z "$2" || "$2" == --* ]]; then
                echo "error: $1 requires a directory" >&2
                exit 2
            fi
            if [[ "$1" == --prefix ]]; then
                PREFIX=$2
            else
                ARGS+=(--build-dir "$2")
            fi
            shift 2
            ;;
        --dry-run) ARGS+=(--dry-run); shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

exec python3 "$SCRIPT_DIR/install_state.py" "${ARGS[@]}" --prefix "$PREFIX"
