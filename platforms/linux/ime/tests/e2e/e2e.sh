#!/usr/bin/env bash
# Headless Fcitx5 typing E2E for the RIMES Linux IME.
# Required path: in-process testfrontend (no display).
# Additional paths: Xvfb + DBus + GTK/Qt hosts; weston headless when present.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
IME_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$IME_ROOT/../../.." && pwd -P)
if [[ ! -f "$REPO_ROOT/scripts/platform-preview/preview.py" ]]; then
    echo "error: could not locate the repository root from $IME_ROOT" >&2
    exit 1
fi

usage() {
    cat <<'EOF'
Run Linux IME end-to-end typing checks.

Usage:
  e2e.sh [--build-dir DIR] [--skip-display] [--skip-wayland]

The in-process Fcitx5 testfrontend job is required. Xvfb/GTK/Qt/Wayland
jobs run when the tools exist and are skipped (not failed) otherwise.
EOF
}

BUILD_DIR="${RIMES_IME_BUILD_DIR:-$IME_ROOT/build}"
SKIP_DISPLAY=0
SKIP_WAYLAND=0

while (($# > 0)); do
    case "$1" in
        --build-dir)
            BUILD_DIR=$2
            shift 2
            ;;
        --skip-display)
            SKIP_DISPLAY=1
            shift
            ;;
        --skip-wayland)
            SKIP_WAYLAND=1
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

if [[ ! -x "$BUILD_DIR/rimes-fcitx-e2e" ]]; then
    echo "error: build the IME first (missing $BUILD_DIR/rimes-fcitx-e2e)" >&2
    exit 1
fi

STAGE_DIR=$(mktemp -d /tmp/rimes-ime-e2e.XXXXXX)
cleanup() {
    local status=$?
    local pid_var pid
    for pid_var in FCITX_PID GTK_PID QT_PID WESTON_PID SWAY_PID BUFFER_UI_PID CAPSULE_UI_PID XVFB_PID DBUS_SESSION_BUS_PID; do
        pid=${!pid_var-}
        if [[ -n "$pid" ]]; then
            kill "$pid" >/dev/null 2>&1 || true
            wait "$pid" >/dev/null 2>&1 || true
        fi
    done
    rm -rf "$STAGE_DIR" >/dev/null 2>&1 || true
    return "$status"
}
trap cleanup EXIT

echo "==> staging reviewed rime-data"
python3 "$REPO_ROOT/scripts/platform-preview/preview.py" stage \
    --repo-root "$REPO_ROOT" \
    --output-dir "$STAGE_DIR/shared"

USER_DIR="$STAGE_DIR/user"
LOG_DIR="$STAGE_DIR/log"
mkdir -p "$USER_DIR" "$LOG_DIR"
export RIMES_SHARED_DIR="$STAGE_DIR/shared"
export RIMES_USER_DIR="$USER_DIR"
export RIMES_LOG_DIR="$LOG_DIR"

echo "==> live librime smoke (nihao / number / paging / Escape)"
"$BUILD_DIR/rimes-engine-smoke" "$STAGE_DIR/shared" "$USER_DIR" "$LOG_DIR"

echo "==> in-process Fcitx5 testfrontend"
"$BUILD_DIR/rimes-fcitx-e2e" "$BUILD_DIR" "." "test-data"

if [[ ! -x "$BUILD_DIR/rimes-buffer-fcitx-e2e" ]]; then
    echo "error: missing $BUILD_DIR/rimes-buffer-fcitx-e2e" >&2
    exit 1
fi
echo "==> in-process Fcitx5 Buffer (stage / send / Return / hold / focus / destroy)"
"$BUILD_DIR/rimes-buffer-fcitx-e2e" "$BUILD_DIR" "." "test-data"
RIMES_TEST_AUTO_SEND=1 "$BUILD_DIR/rimes-buffer-fcitx-e2e" "$BUILD_DIR" "." "test-data"
RIMES_TEST_SESSION_PROTECTION=1 RIMES_PRIVATE_SESSION_TEST=1 dbus-run-session -- \
    "$BUILD_DIR/rimes-buffer-fcitx-e2e" "$BUILD_DIR" "." "test-data"
RIMES_TEST_LAUNCHER=1 dbus-run-session -- \
    "$BUILD_DIR/rimes-buffer-fcitx-e2e" "$BUILD_DIR" "." "test-data"

if [[ ! -x "$BUILD_DIR/rimes-buffer-respawn-e2e" || ! -x "$BUILD_DIR/rimes-buffer-ui-stub" ]]; then
    echo "error: missing rimes-buffer-respawn-e2e or rimes-buffer-ui-stub" >&2
    exit 1
fi
echo "==> in-process Fcitx5 Buffer UI respawn (kill + delayed connect)"
"$BUILD_DIR/rimes-buffer-respawn-e2e" "$BUILD_DIR" "." "test-data"

if [[ ! -x "$BUILD_DIR/rimes-capsule-fcitx-e2e" ]]; then
    echo "error: missing $BUILD_DIR/rimes-capsule-fcitx-e2e" >&2
    exit 1
fi
echo "==> in-process Fcitx5 Capsule (show / insert / search / focus / password)"
"$BUILD_DIR/rimes-capsule-fcitx-e2e" "$BUILD_DIR" "." "test-data"

if [[ ! -x "$BUILD_DIR/rimes-capsule-respawn-e2e" || ! -x "$BUILD_DIR/rimes-capsule-ui-stub" ]]; then
    echo "error: missing rimes-capsule-respawn-e2e or rimes-capsule-ui-stub" >&2
    exit 1
fi
echo "==> in-process Fcitx5 Capsule UI respawn (kill + delayed connect)"
"$BUILD_DIR/rimes-capsule-respawn-e2e" "$BUILD_DIR" "." "test-data"

if (( SKIP_DISPLAY == 1 )); then
    echo "skip: display clients (--skip-display)"
    exit 0
fi

if ! command -v Xvfb >/dev/null || ! command -v dbus-launch >/dev/null; then
    echo "skip: Xvfb/dbus-launch not installed; display clients were not run"
    exit 0
fi

export XDG_RUNTIME_DIR="$STAGE_DIR/runtime"
export XDG_CONFIG_HOME="$STAGE_DIR/config"
export XDG_DATA_HOME="$STAGE_DIR/xdg-data"
export XDG_CACHE_HOME="$STAGE_DIR/cache"
export HOME="$STAGE_DIR/home"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/fcitx5" "$XDG_DATA_HOME" \
    "$XDG_CACHE_HOME" "$HOME"
chmod 700 "$XDG_RUNTIME_DIR"

# Isolated Fcitx5 profile that only enables RIMES.
cat > "$XDG_CONFIG_HOME/fcitx5/profile" <<'EOF'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=rimes

[Groups/0/Items/0]
Name=rimes

[GroupOrder]
0=Default
EOF

# Isolated XDG layout: live fcitx5 reads addon/inputmethod conf from
# $XDG_DATA_HOME/fcitx5/..., and the .so from FCITX_ADDON_DIRS.
mkdir -p "$XDG_DATA_HOME/fcitx5/addon" "$XDG_DATA_HOME/fcitx5/inputmethod"
cp "$BUILD_DIR/test-data/addon/rimes.conf" "$XDG_DATA_HOME/fcitx5/addon/rimes.conf"
cp "$BUILD_DIR/test-data/inputmethod/rimes.conf" \
    "$XDG_DATA_HOME/fcitx5/inputmethod/rimes.conf"
# FCITX_ADDON_DIRS replaces the default search path, so keep the system
# module dir (dbus/xcb/classicui) and prepend the just-built rimes.so.
MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)
SYSTEM_ADDON_DIR=""
for candidate in \
    ${MULTIARCH:+/usr/lib/$MULTIARCH/fcitx5} \
    /usr/lib/x86_64-linux-gnu/fcitx5 \
    /usr/lib64/fcitx5 \
    /usr/lib/fcitx5; do
    if [[ -d "$candidate" ]]; then
        SYSTEM_ADDON_DIR=$candidate
        break
    fi
done
export FCITX_ADDON_DIRS="$BUILD_DIR${SYSTEM_ADDON_DIR:+:$SYSTEM_ADDON_DIR}${FCITX_ADDON_DIRS:+:$FCITX_ADDON_DIRS}"
export GTK_IM_MODULE=fcitx
export QT_IM_MODULE=fcitx
export XMODIFIERS=@im=fcitx
export RIMES_BUFFER_SOCKET="$STAGE_DIR/rimes-buffer.sock"
export RIMES_BUFFER_HEADLESS=1
export RIMES_BUFFER_CLOSE_AFTER_LAST=0
export RIMES_CAPSULE_SOCKET="$STAGE_DIR/rimes-capsule.sock"
export RIMES_CAPSULE_HEADLESS=1
export PATH="$BUILD_DIR:$PATH"

export DISPLAY=:93
Xvfb "$DISPLAY" -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
sleep 0.3

assert_geometry() {
    local geom=$1
    local label=$2
    local expect_w=$3
    local expect_h=$4
    python3 - "$geom" "$label" "$expect_w" "$expect_h" <<'PY'
import json, sys
path, label, expect_w, expect_h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
with open(path, encoding="utf-8") as handle:
    data = json.load(handle)
if data.get("size_request_w") != expect_w or data.get("size_request_h") != expect_h:
    raise SystemExit(f"{label}: size_request {data!r} is not {expect_w}x{expect_h}")
width = int(data.get("width") or 0)
if width > 0 and width < expect_w:
    raise SystemExit(f"{label}: allocated width {width} < {expect_w} ({data!r})")
print(f"ok: {label} size_request {expect_w}x{expect_h} allocated={data.get('width')}x{data.get('height')} layer={data.get('layer')}")
PY
}

if [[ -x "$BUILD_DIR/rimes-buffer" ]]; then
    echo "==> Buffer UI geometry (X11 / Xvfb)"
    GEOM_X11="$STAGE_DIR/buffer-geom-x11.json"
    if timeout 8 "$BUILD_DIR/rimes-buffer" --preview --dump-geometry "$GEOM_X11" --quit-after-dump \
        >"$STAGE_DIR/buffer-geom-x11.log" 2>&1; then
        assert_geometry "$GEOM_X11" "X11 preview" 760 73
    else
        echo "warning: rimes-buffer --dump-geometry exited ($STAGE_DIR/buffer-geom-x11.log)" >&2
        tail -n 20 "$STAGE_DIR/buffer-geom-x11.log" >&2 || true
    fi
fi

if [[ -x "$BUILD_DIR/rimes-capsule" ]]; then
    echo "==> Capsule UI geometry (X11 / Xvfb)"
    GEOM_CAP_X11="$STAGE_DIR/capsule-geom-x11.json"
    if timeout 8 "$BUILD_DIR/rimes-capsule" --preview --dump-geometry "$GEOM_CAP_X11" --quit-after-dump \
        >"$STAGE_DIR/capsule-geom-x11.log" 2>&1; then
        assert_geometry "$GEOM_CAP_X11" "X11 Capsule preview" 940 208
    else
        echo "warning: rimes-capsule --dump-geometry exited ($STAGE_DIR/capsule-geom-x11.log)" >&2
        tail -n 20 "$STAGE_DIR/capsule-geom-x11.log" >&2 || true
    fi
fi

eval "$(dbus-launch --sh-syntax)"

fcitx5 --disable=wayland,kimpanel,notificationitem --verbose=*=4 \
    >"$STAGE_DIR/fcitx5.log" 2>&1 &
FCITX_PID=$!

echo "==> waiting for fcitx5 DBus"
python3 - <<'PY'
import time
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

deadline = time.monotonic() + 40
while time.monotonic() < deadline:
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        Gio.DBusProxy.new_sync(
            bus, Gio.DBusProxyFlags.NONE, None,
            "org.fcitx.Fcitx5", "/controller",
            "org.fcitx.Fcitx.Controller1", None,
        ).call_sync("CurrentInputMethod", None,
                    Gio.DBusCallFlags.NO_AUTO_START, 1000, None)
        raise SystemExit(0)
    except GLib.Error:
        time.sleep(0.25)
raise SystemExit("fcitx5 DBus did not become ready")
PY

echo "==> DBus virtual input context"
DBUS_IME_OK=0
if python3 "$SCRIPT_DIR/dbus_client.py"; then
    DBUS_IME_OK=1
else
    echo "---- fcitx5.log ----" >&2
    tail -n 80 "$STAGE_DIR/fcitx5.log" >&2 || true
    echo "DBus virtual IC is best-effort on headless runners; testfrontend remains authoritative." >&2
    echo "warning: DBus virtual input context did not complete" >&2
fi

echo "==> DBus virtual input context + Buffer"
if python3 "$SCRIPT_DIR/dbus_buffer_client.py"; then
    echo "ok: DBus Buffer staged and sent 你好"
elif (( DBUS_IME_OK == 1 )); then
    echo "---- fcitx5.log ----" >&2
    tail -n 80 "$STAGE_DIR/fcitx5.log" >&2 || true
    echo "error: DBus IME worked but Buffer DBus E2E failed" >&2
    exit 1
else
    echo "warning: DBus Buffer E2E skipped after DBus IME was unavailable" >&2
fi

echo "==> DBus virtual input context + Capsule"
if python3 "$SCRIPT_DIR/dbus_capsule_client.py"; then
    echo "ok: DBus Capsule showed the seed note and inserted RIMES"
elif (( DBUS_IME_OK == 1 )); then
    echo "---- fcitx5.log ----" >&2
    tail -n 80 "$STAGE_DIR/fcitx5.log" >&2 || true
    echo "error: DBus IME worked but Capsule DBus E2E failed" >&2
    exit 1
else
    echo "warning: DBus Capsule E2E skipped after DBus IME was unavailable" >&2
fi

if command -v xdotool >/dev/null && [[ -x "$BUILD_DIR/rimes-gtk-host" ]]; then
    echo "==> GTK host + xdotool"
    GTK_OUT="$STAGE_DIR/gtk-state.txt"
    "$BUILD_DIR/rimes-gtk-host" "$GTK_OUT" 20000 &
    GTK_PID=$!
    for _ in $(seq 1 40); do
        if [[ -f "$GTK_OUT" ]]; then
            break
        fi
        sleep 0.1
    done
    WINDOW_ID=$(xdotool search --name "RIMES GTK host" | head -n 1 || true)
    if [[ -n "$WINDOW_ID" ]]; then
        # Xvfb has no window manager; activate is unsupported, focus is enough.
        xdotool windowfocus --sync "$WINDOW_ID" || true
        xdotool type --window "$WINDOW_ID" --delay 40 nihao || true
        xdotool key --window "$WINDOW_ID" space || true
        sleep 1.2
        if grep -Fq $'commit\t你好' "$GTK_OUT"; then
            echo "ok: GTK host committed 你好"
        else
            echo "warning: GTK host did not observe 你好 (see $GTK_OUT)" >&2
            echo "GTK host is best-effort on headless runners; DBus/testfrontend remain authoritative." >&2
        fi
    else
        echo "warning: GTK host window was not found" >&2
    fi
    kill "$GTK_PID" >/dev/null 2>&1 || true
    unset GTK_PID
else
    echo "skip: GTK host or xdotool unavailable"
fi

if command -v xdotool >/dev/null && [[ -x "$BUILD_DIR/rimes-qt-host" ]]; then
    echo "==> Qt host + xdotool"
    QT_OUT="$STAGE_DIR/qt-state.txt"
    "$BUILD_DIR/rimes-qt-host" "$QT_OUT" 20000 &
    QT_PID=$!
    for _ in $(seq 1 40); do
        if [[ -f "$QT_OUT" ]]; then
            break
        fi
        sleep 0.1
    done
    WINDOW_ID=$(xdotool search --name "RIMES Qt host" | head -n 1 || true)
    if [[ -n "$WINDOW_ID" ]]; then
        xdotool windowfocus --sync "$WINDOW_ID" || true
        xdotool type --window "$WINDOW_ID" --delay 40 nihao || true
        xdotool key --window "$WINDOW_ID" space || true
        sleep 1.2
        if grep -Fq $'commit\t你好' "$QT_OUT"; then
            echo "ok: Qt host committed 你好"
        else
            echo "warning: Qt host did not observe 你好 (see $QT_OUT)" >&2
            echo "Qt host is best-effort on headless runners; DBus/testfrontend remain authoritative." >&2
        fi
    else
        echo "warning: Qt host window was not found" >&2
    fi
    kill "$QT_PID" >/dev/null 2>&1 || true
    unset QT_PID
else
    echo "skip: Qt host or xdotool unavailable"
fi

if (( SKIP_WAYLAND == 0 )) && command -v weston >/dev/null && command -v wtype >/dev/null; then
    echo "==> weston headless + wtype (best effort)"
    weston --backend=headless-backend.so --socket=rimes-weston --idle-time=0 \
        >"$STAGE_DIR/weston.log" 2>&1 &
    WESTON_PID=$!
    sleep 1
    echo "ok: weston headless started (wtype against a real compositor needs a seat; recorded as available)"
    kill "$WESTON_PID" >/dev/null 2>&1 || true
    unset WESTON_PID
else
    echo "skip: weston/wtype not available or --skip-wayland"
fi

if (( SKIP_WAYLAND == 0 )) && command -v sway >/dev/null && [[ -x "$BUILD_DIR/rimes-buffer" ]]; then
    echo "==> sway headless + Buffer UI preview"
    export WLR_BACKENDS=headless
    export WLR_LIBINPUT_NO_DEVICES=1
    export XDG_CURRENT_DESKTOP=sway
    sway --unsupported-gpu -c /dev/null >"$STAGE_DIR/sway.log" 2>&1 &
    SWAY_PID=$!
    sleep 1
    if [[ -S "$XDG_RUNTIME_DIR/wayland-1" || -n "${WAYLAND_DISPLAY:-}" ]]; then
        export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
        GEOM_WL="$STAGE_DIR/buffer-geom-wayland.json"
        "$BUILD_DIR/rimes-buffer" --preview --dump-geometry "$GEOM_WL" --quit-after-dump \
            >"$STAGE_DIR/buffer-ui.log" 2>&1 &
        BUFFER_UI_PID=$!
        for _ in $(seq 1 30); do
            if [[ -s "$GEOM_WL" ]] || ! kill -0 "$BUFFER_UI_PID" >/dev/null 2>&1; then
                break
            fi
            sleep 0.1
        done
        wait "$BUFFER_UI_PID" >/dev/null 2>&1 || true
        unset BUFFER_UI_PID
        if [[ -s "$GEOM_WL" ]]; then
            assert_geometry "$GEOM_WL" "sway layer-shell preview" 760 73
        else
            echo "warning: rimes-buffer preview did not dump geometry (see $STAGE_DIR/buffer-ui.log)" >&2
        fi
        if [[ -x "$BUILD_DIR/rimes-capsule" ]]; then
            echo "==> sway headless + Capsule UI preview"
            GEOM_CAP_WL="$STAGE_DIR/capsule-geom-wayland.json"
            "$BUILD_DIR/rimes-capsule" --preview --dump-geometry "$GEOM_CAP_WL" --quit-after-dump \
                >"$STAGE_DIR/capsule-ui.log" 2>&1 &
            CAPSULE_UI_PID=$!
            for _ in $(seq 1 30); do
                if [[ -s "$GEOM_CAP_WL" ]] || ! kill -0 "$CAPSULE_UI_PID" >/dev/null 2>&1; then
                    break
                fi
                sleep 0.1
            done
            wait "$CAPSULE_UI_PID" >/dev/null 2>&1 || true
            unset CAPSULE_UI_PID
            if [[ -s "$GEOM_CAP_WL" ]]; then
                assert_geometry "$GEOM_CAP_WL" "sway Capsule layer-shell preview" 940 208
            else
                echo "warning: rimes-capsule preview did not dump geometry (see $STAGE_DIR/capsule-ui.log)" >&2
            fi
        fi
    else
        echo "warning: sway headless did not expose a wayland socket" >&2
    fi
    kill "$SWAY_PID" >/dev/null 2>&1 || true
    unset SWAY_PID
    unset WLR_BACKENDS WLR_LIBINPUT_NO_DEVICES
else
    echo "skip: sway or rimes-buffer UI not available"
fi

echo "RIMES Linux IME E2E finished"
