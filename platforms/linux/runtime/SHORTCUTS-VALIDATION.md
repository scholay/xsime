# Global shortcuts and independent workspace launch

Task [#160](https://github.com/scholay/xsime/issues/160), implementation evidence
2026-10-17. The issue stays open for Mailbox integration, login/compositor checks
and the macOS screenshot comparison.

## Shared contract and behavior

`generate_shortcut_contract.py` reads action identities, titles and default keys
from `Sources/RimeBuffer/ShortcutPreferences.swift`. Command maps to Ctrl;
combined Command/Control requires an explicit mapping review. Configuration
migrates missing shortcut fields without discarding older settings. The recorder
matches macOS click-to-record, Escape-to-cancel, Delete-to-reset, duplicate and
editing-key rejection. `generate_visual_tokens.py` imports the native row width,
spacing, insets, fonts, minimum recorder width and themed corner radius.
Fonts, glyph rendering and desktop-specific status controls still need visual
acceptance; importing dimensions is not a pixel-equivalence result.

Fcitx loads XSIME at startup even while another IM is selected. Four fixed action
identities share a D-Bus launcher; Buffer, Capsule and settings are implemented.
Mailbox is explicitly unavailable until task 18. A cross-IME launch switches
only the focused context and validates its UUID, focus and IM again after
activation. Blur, destruction, a later IM switch, configuration change or
protection revokes a pending capture. No focused context means detached launch.
Closing a workspace does not restore a previous IM.

The settings recorder temporarily releases global registrations and suppresses
the Fcitx shortcut watcher. Recording belongs to one D-Bus owner and one request
token: an old stop request cannot release a newer recorder, another process
cannot stop it, and owner loss/two-minute expiry restores registrations. A
Wayland desktop may ask for authorization again when registrations resume.

## Automated evidence

Ubuntu 24.04 arm64 container, Fcitx 5.1.7 and isolated test user directories:

- `configuration`: migration, canonical modifiers, reserved editing keys,
  duplicate bindings and save/reopen behavior.
- `settings-ui`: real GTK recorder preferences, conflict rejection, Escape,
  reset, persistence and existing theme/provider workflows under Xvfb.
- `desktop-shortcuts-x11`: real X server key events through xdotool, another
  client's grab conflict, repeated keydown, release/repress, Caps Lock and
  disable/re-registration.
- `desktop-shortcuts-portal`: a private D-Bus portal implements CreateSession,
  BindShortcuts and real typed signals. Checks Response-before-method-reply,
  granted subsets, desktop reassignment, held keys, denial, session retirement,
  provider restart, spoofed/old signals, the compiled `rimes-launch` CLI, and
  recorder cancellation, stale tokens and process exit. Retired callbacks are
  released on the next event-loop iteration, including denial and shutdown.
- `RIMES_TEST_LAUNCHER=1` in `rimes-buffer-fcitx-e2e`: the actual XSIME addon and
  Fcitx's TestIM `keyboard-us` entry verify switching, deferred capture, held
  keys, a user's intervening switch, blur, sensitive fields, changed bindings,
  global disable and Capsule launch. This is not a physical keyboard/compositor
  acceptance test.
- Source installer lifecycle tests include the launcher and both workspace
  desktop entries, verify removal and preserve personal files. deb payloads
  use the same install manifest.

Run the CTests with `ctest --test-dir BUILD --output-on-failure`; run the real
addon suites with `platforms/linux/ime/tests/e2e/e2e.sh --build-dir BUILD`.
The portal test requires its private `dbus-run-session`; it never takes the
portal name in an existing desktop session.

## Remaining desktop acceptance

Verify first login/restart, X11 keyboard layout changes and conflict recovery;
GNOME/KDE Wayland permission dialogs, custom desktop key reassignment and
compositor activation/focus policy; actual GTK/Qt/browser text fields and
settings single-window activation; and same-state macOS/Linux screenshots.
Task 11 owns window activation, placement and multiple displays. A desktop
without GlobalShortcuts support exposes actionable launcher commands, and is
not reported as having registered global shortcuts. Mailbox becomes available
when its real host lands in task 18.

Protocol sources: [GlobalShortcuts portal](https://github.com/flatpak/xdg-desktop-portal/blob/main/data/org.freedesktop.portal.GlobalShortcuts.xml),
[request lifecycle](https://github.com/flatpak/xdg-desktop-portal/blob/main/data/org.freedesktop.portal.Request.xml),
[XDG shortcut syntax](https://specifications.freedesktop.org/shortcuts/latest/),
[Fcitx TestIM](https://github.com/fcitx/fcitx5/blob/master/testing/testim/testim.cpp).
