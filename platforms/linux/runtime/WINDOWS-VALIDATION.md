# Linux desktop windows and native visual contract

Implementation evidence for [task #161](https://github.com/scholay/xsime/issues/161),
2026-10-17. This is functional desktop evidence; the task remains open for the
remaining host and pixel comparisons in [VISUAL-PARITY.md](../VISUAL-PARITY.md).

## Implemented behavior

Buffer and Capsule use one passive-window implementation. X11 converts valid
global cursor rectangles into GTK application pixels; Wayland/RelativeRect
coordinates are never treated as a global anchor. Fresh X11 anchors take
precedence when opening. Otherwise the panel restores its monitor-relative
position, or uses the lower part of the work area with space for candidates.
Resizing between 73 and 105 logical pixels preserves the bottom edge.

Wayland layer-shell uses one anchor per axis, explicit monitor selection,
overlay level, zero exclusive zone and keyboard mode NONE. It no longer
stretches Buffer/Capsule between opposite screen edges. Monitor changes,
scale changes and work-area changes clamp the panel to available space.
Intercepting layer-shell's output-close event keeps GTK from destroying the
content when a display disappears. On desktops without layer-shell, an
available XWayland display provides the passive utility-window fallback. When
neither is available, placement/focus is compositor-managed, window-position
actions are disabled, and the function menu explains the limitation.

Buffer's empty toolbar toggles the 35-pixel folded state. Movement uses a
passive pointer gesture on both backends, with release detection and a bounded
cleanup timer. Its function menu restores the window position, moves to a
display and opens settings. The compact “发送完关闭” switch imports its 26×15
track, 28×17 control and knob geometry from `RimeFixedAccentSwitch.swift`;
its colors and accent contrast come from `RimeUI.swift`. User clicks persist
through the same configuration store as settings. Snapshot updates do not
emit another toggle. Existing protected-state command restrictions remain.

Positions are stored atomically in the private `windows/` directory under
`RIMES_USER_DIR` (or the normal user-data root), independently of the plugin
configuration revision. Moving a window cannot cancel translation or AI work.
Malformed files and symlinks are rejected without deleting user data. Settings
uses GtkApplication activation so repeated launches reuse one window.
Capsule receives the configured theme through its host snapshot; CSS attaches
to the actual panel screen, including an explicitly opened XWayland display.

The coordinate basis follows [Fcitx 5.1.7's XCB candidate renderer](https://github.com/fcitx/fcitx5/blob/5.1.7/src/ui/classic/xcbinputwindow.cpp)
and [GDK monitor application-pixel geometry](https://docs.gtk.org/gdk3/method.Monitor.get_geometry.html).
Output removal handling follows [gtk-layer-shell's close-event contract](https://wmww.github.io/gtk-layer-shell/#gtk-layer-set-respect-close),
including the 0.8.2 version shipped by the validation environment.

## Reproducible checks

Ubuntu 24.04 arm64 container; Fcitx 5.1.7, GTK 3.24, gtk-layer-shell 0.8.2,
Xvfb/xfwm4 and a private headless Sway session; isolated profiles and synthetic
content throughout. No live macOS input-method install or release was performed.

```sh
cmake -S platforms/linux/ime -B /tmp/xsime-ime-build
cmake --build /tmp/xsime-ime-build -j4
ctest --test-dir /tmp/xsime-ime-build --output-on-failure
xvfb-run -a dbus-run-session -- python3 platforms/linux/runtime/tests/desktop_windows_test.py /tmp/xsime-ime-build
GDK_SCALE=2 xvfb-run -a -s '-screen 0 2560x1600x24' dbus-run-session -- \
  python3 platforms/linux/runtime/tests/desktop_windows_test.py /tmp/xsime-ime-build
# Run the compositor as an unprivileged user.
dbus-run-session -- python3 platforms/linux/runtime/tests/panel_wayland_test.py /tmp/xsime-ime-build
RIMES_SHARED_DIR=/path/to/staged/shared RIMES_TEST_AUTO_SEND=1 \
  /tmp/xsime-ime-build/rimes-buffer-fcitx-e2e /tmp/xsime-ime-build . test-data
```

- 39 CTests passed, including geometry, both integer scales, Capsule theme
  propagation, settings, render and existing input/plugin/security suites.
- Real X11 mouse input passed at 1× and 2×: a second settings process exits
  after activating the first window; opening/clicking/dragging Buffer keeps
  the host active; first switch click sends exactly one command; fold/unfold,
  dual-rail resizing and saved drag position work. A socket fixture observes
  UI commands only; it does not stand in for Fcitx delivery.
- Real Sway layer-shell allocation preserves Buffer 760×73 and Capsule 940×208.
  The shared panel test covers mixed 1×/2× scales, a 1.5× scale, negative output
  origin, monitor selection and recovery after disabling/re-enabling an output.
- The production Fcitx auto-send suite verifies both toolbar settings persist
  and update the running addon, composition blocks automatic delivery, and
  hidden intervals do not count toward the timer. The existing Default Buffer
  suite remains part of the regression checks.
- CI runs both X11 scales and the Sway acceptance script in addition to CTest.

Local run logs are in `.build/linux-parity-20261017/task11-{acceptance,wayland,final-regression}.log`.
They are build artifacts, not distribution assets.

## Still required before closing the task

GNOME/KDE desktop sessions, compositor fullscreen policies, real mixed-DPI
hardware, physical pointer motion between outputs and candidate overlap with
GTK/Qt/Electron/terminal hosts remain in task #179. The native macOS standard
preview on this host currently renders the protected state because Secure
Input is active; it must not be mislabeled as a normal-state reference. The
current matching dimensions and imported colors do not establish pixel equality.
Record actual state-matched screenshots, diffs, font/material differences and
remaining target-indicator/toolbar details before visual acceptance. Capsule's
full native management surface belongs to tasks #162–#167; its existing rail
size is not a claim that those pages have already been implemented.

## No-layer-shell display teardown follow-up (2026-10-18)

GTK owns the display returned by `gdk_display_open`. Passive panels now reuse
that display through GDK's manager and release only their own explicit
reference; destroying a panel cannot close a connection used by other GTK
widgets. This removes the extra unref reported in PR #193 and also avoids
GTK display-scoped teardown during panel recreation. See the
[GTK ownership contract](https://docs.gtk.org/gdk3/type_func.Display.open.html).

A real Weston X11-backend compositor without layer-shell, nested in private
Xvfb, exercises Wayland-to-X11 fallback, placement and repeated panel teardown
with `G_DEBUG=fatal-criticals`. The test verifies a reused connection and passes
on Ubuntu 24.04 arm64. Linux CI runs the same fixture; this is separate from
GNOME/KDE desktop acceptance.
