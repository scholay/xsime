# XSIME Buffer (Linux)

Default and derived Buffer workbench host for the Fcitx5 XSIME addon. Behavior and the
deliberate macOS gaps are in [the behavior specification](../../../docs/platforms/linux/buffer-spec.md). Real-desktop checks are in
[the desktop checklist](../../../docs/platforms/linux/acceptance.md#buffer).

## Layout

- `core/` — process-local block store, Return tap/hold, JSON snapshot wire
  format, immutable derived requests and revocable results. Shared by the addon, `rimes-buffer`, `rimes-buffer-ctl`, and tests.
  This is a new C++ implementation of the macOS contract, not a rewrite of
  the Swift sources.
- `ui/` — GTK 3 workbench. X11: keep-above + caret placement on the opposite
  side of the stock candidate popup (below the caret when the list flips
  up). wlroots Wayland: `gtk-layer-shell` overlay stretched left/right with
  a 760×73 standard request (105 high for live dual rails).
- `ctl/` — `rimes-buffer-ctl` talks to the IME socket without a display.
- Built from `platforms/linux/ime/CMakeLists.txt` and shipped in the same
  `fcitx5-rimes` `.deb`.

## How it is wired

`RimesIme::commitText` is the only librime → host commit path. When Buffer
owns the focused input context, that text is staged. `BufferService` then
delivers through `InputContext::commitString` after re-checking the same
context. Keys are intercepted in `HandleEarlyKey` before `RimesState`.

The GTK process is a renderer. It does not run librime.

## Build

```bash
platforms/linux/ime/scripts/deps.sh --install
platforms/linux/ime/scripts/build.sh
ctest --test-dir platforms/linux/ime/build --output-on-failure
platforms/linux/ime/tests/e2e/e2e.sh --build-dir platforms/linux/ime/build
```

## Runtime

| Item | Value |
|---|---|
| Toggle | `Alt+Shift+B` while XSIME is current |
| Close | same hotkey, Escape, or the toolbar close button |
| Send next | Return tap, or the paper plane |
| Send all | hold Return 1.2s |
| Socket | `$XDG_RUNTIME_DIR/rimes-buffer.sock` |
| UI binary | `/usr/libexec/rimes/rimes-buffer` and `/usr/bin/rimes-buffer` |

## Visual and worker fixtures

`rimes-buffer --render DIRECTORY` renders five themes and nine states using the
actual GTK workbench without connecting to a live socket. CTest checks the
standard → dual → exchange → restored dimensions. The native geometry and
colors are generated from macOS sources; see [visual acceptance](../../../docs/platforms/linux/visual-parity.md).

The selector lists only enabled compiled adapters; catalog entries alone do not
make a plugin available.
