# XSIME Fcitx5 input method (Linux)

This is a real XSIME frontend for [Fcitx5](https://fcitx-im.org/): a C++ addon
that links `librime` (with `librime-lua` and OpenCC) and deploys the reviewed
XSIME `rime-data` set. It lives next to the existing
[Linux data preview](../README.md), which still installs into stock
`fcitx5-rime` / `ibus-rime` and is unchanged.

This is step 3 of the Linux port (IME + Default Buffer + Capsule rail).
The [30-task Linux parity plan](../PARITY.md) now tracks the full desktop port.
The host has a compiled plugin registry with capability checks and revocable
task grants. Fcitx5 configuration can disable the existing Capsule notes module:
disabling closes/disarms its rail without deleting notes or affecting typing.
Catalog declarations alone never enable an unimplemented Linux adapter.
The `rimes-plugins` companion lists the pinned official catalog and manages
compatible packages; unsupported platform entries are visibly unavailable.
Package installation, update and enablement are separate steps. Updates and
uninstalls revoke running jobs and preserve user content. See the
[plugin runtime guide](../runtime/README.md).
`rimes-settings` adds a native settings entry for plugin/package management,
private provider credentials, themes and live Buffer preferences. The settings
surface is undergoing macOS pixel comparison; unported module pages are marked
as pending. See [the visual acceptance contract](../VISUAL-PARITY.md).
The translation card also provides pinned offline Chinese/English resource
installation, status, cancellation and repair; see the
[resource instructions](../runtime/README.md#local-translation-resources).
Its Python 3.12 runtime is included in deb dependencies. Downloaded resources
remain in the user directory when either source or deb installation is removed.
The [Mailbox host](../mailbox/README.md) now provides independent navigation, persistent threads, local notes, reviewed Buffer/Capsule transfers and a loopback HTTP/MCP gateway. AI conversation and PTY adapters are tracked separately. Buffer and Capsule behavior are specified in
[`../buffer/SPEC.md`](../buffer/SPEC.md) and
[`../capsule/SPEC.md`](../capsule/SPEC.md). Hooks live in
`src/engine/rime_hooks.hpp`.

## What works

- Preedit and Fcitx5 candidate UI (inline XOR popup row, matching stock rime)
- Space commit (`nihao` + Space → `你好` on `rime_ice`)
- Number keys 1–9 select the current page
- Page Up / Page Down
- Escape cancels composition without committing
- First-run (and later rebuild) dictionary deploy runs in the background;
  Fcitx5 stays responsive and keys pass through until librime is ready
  (`subMode` shows `Deploying`). A worker watcher notifies the Fcitx5
  event loop when deploy finishes — status leaves `Deploying` without a restart.
- A process-lifetime user-directory lock prevents two XSIME instances from
  deploying or opening user dictionaries in the same directory concurrently.
  A replacement reports a locked-directory error until the owner finishes
  exiting. The lock file remains on disk; the OS releases ownership on exit.
- Isolated user directory: `$XDG_DATA_HOME/rimes` (not `…/fcitx5/rime`)
- Shared data: `$prefix/share/rimes/data` (policy-staged 57-file closure)
- Default Buffer workbench (configurable, default `Ctrl+Shift+B`): Rime commits
  stage as blocks, Return tap/hold and the paper plane send through the same
  `commitText` → `commitString` path
- Capsule rail (configurable, default `Ctrl+Shift+V`): structured/Bullet/rich
  notes, grouped detail rows and opt-in clipboard history. Text uses native
  `commitString` after an armed-IC recheck; rich clipboard items restore original
  MIME formats for explicit paste. The GTK manager exposes read-only history
  previews, copy, favorite and delete. Resources add reference metadata, file/PDF
  previews and explicit attachment copies. Capture adds PNG screenshots and silent
  VP8 WebM recording through X11 or desktop portals, with a private archive.
  See [Capsule usage](../capsule/README.md).

macOS-only behaviour that is **not** reproduced here: custom candidate chrome,
Mailbox AI/PTY adapters, Capsule iCloud, advanced capture editing and audio,
Buffer stream/music plugins, cross-batch chord pairing,
IMK `Delivery.insert`. See [`../buffer/SPEC.md`](../buffer/SPEC.md) and
[`../capsule/SPEC.md`](../capsule/SPEC.md) for the explicit gap lists.
Local Chinese/English translation, incremental units and OpenAI-compatible
streaming requests are implemented; their desktop and visual acceptance is
tracked separately in the parity plan.

## Desktop launch and global shortcuts

Use **XSIME Buffer**, **XSIME Capsule**, **XSIME Mailbox** or **XSIME Settings** from the application
menu, even when another input method is selected. `rimes-launch buffer`,
`rimes-launch capsule`, `rimes-launch mailbox` and `rimes-launch settings` provide the same entry points
for system custom shortcuts. They start Fcitx5 if necessary; add XSIME to its
input-method group first. A Buffer or Capsule launch selects XSIME for the focused
input context, waits for activation and then captures it. Changing focus or
switching input methods in that interval cancels capture. Closing the workspace
keeps the selected input method. With no focused input context, the workspace
opens without a delivery target.

In **Settings → Buffer → 快捷键**, click a recorder and press a combination.
Escape cancels; unmodified Delete restores that action's default. Duplicate
bindings and Ctrl/Super+A/V are rejected. The defaults and action titles are
imported from macOS `ShortcutPreferences.swift`, mapping Command to Ctrl:
Buffer `Ctrl+Shift+B`, Capsule `Ctrl+Shift+V`, settings `Ctrl+Shift+S`.
Mailbox uses `Ctrl+Shift+M`; it opens its independent workspace without changing
the current input method. Super can be selected explicitly in the recorder.
Global grabs pause during recording and resume on completion, cancellation,
settings-process exit or a two-minute timeout.

On X11, checked key grabs report conflicts; choose another combination or
release the conflicting application's shortcut and click **重新绑定全局快捷键**.
Caps Lock and Num Lock do not change a binding. On Wayland, the desktop's
**GlobalShortcuts portal** asks for permission and controls the actual binding.
The recorder displays that binding after authorization. Unsupported or declined
portals remain visibly unavailable; use the desktop entries or bind the
`rimes-launch` commands in the desktop's own custom-shortcut settings.
There is no XWayland grab fallback. Turning off **在其他输入法中使用全局快捷键**
retains shortcuts while XSIME itself is selected. Preferences survive restart
and removal of the application.

`rimes-launch --status` reports bindings and the most recent launch result;
`rimes-launch --retry` retries registration. The launcher accepts fixed action
names only. Source uninstall and deb removal include the new launcher and menu
entries while preserving user preferences, dictionaries and learning data.
See [shortcut validation and desktop limits](../runtime/SHORTCUTS-VALIDATION.md).

## Importing source schemes

Open the XSIME configuration entry in Fcitx5, or launch `rimes-schemes` from the
application menu. Review a ZIP or coded UTF-8 TXT/TSV, compile, then explicitly enable
a scheme and restart Fcitx5. Restore built-ins before removing an active package.
Learning data stays in `$XDG_DATA_HOME/rimes/schemes/learning` when a package is removed.
See [the shared format and verification guide](../../../docs/portable-scheme-import.md).

## Maintenance limits

Background deployment runs in `libexec/rimes/rimes-deploy`, before the Fcitx
process opens its runtime. On shutdown XSIME terminates that worker, gives it
250 ms to exit, then forces termination and reaps it before releasing the
user-directory lock. No compiler thread is detached. An interrupted deployment
leaves a marker that requests a full check on the next start, including partial
build directories. The inherited lock also prevents a new
XSIME process from writing while a worker survives an unexpected parent exit.
This lock does not coordinate stock fcitx5-rime, IBus, or external deploy tools.

Buffer preserves capture for one same-IC activation while a toolbar drag
is pending. Once the IME receives `drag_end`, the next activation retires
capture without a grace period. The GTK workbench normally detects physical
release by polling every 50 ms; a click before the IME receives that message
can still consume the pending drag. The 0–10 ms physical-release case in
[#44](https://github.com/scholay/xsime/issues/44) was accepted by the maintainer
on 2026-10-16. The Fcitx regression covers command receipt, not physical pointer
release. See the timing matrix in [`../buffer/MANUAL-TEST.md`](../buffer/MANUAL-TEST.md).

## Dependencies

See `scripts/deps.sh --print` for Debian/Ubuntu, Arch, and Fedora package
lists. On Ubuntu 24.04:

```bash
platforms/linux/ime/scripts/deps.sh --install
```

Source builds also require the pinned official plugin dependency:

```bash
git submodule update --init OfficialPlugins
```

The build checks `plugins.lock.json` and generates its catalog from that exact
revision. It never follows the plugin repository's latest branch automatically.

The distro runtime still supports the built-in data. Original modern schemes may
require newer Lua bindings than Ubuntu 24.04 supplies. Build a private pinned runtime
(the script does not replace system libraries):

```bash
python3 platforms/linux/ime/scripts/build-engine.py
export RIMES_IME_RIME_PREFIX="$PWD/platforms/linux/ime/.engine/install"
platforms/linux/ime/scripts/build.sh
```

Keep that prefix when installing/running this build; installed targets retain its library
search path. Building against a distro runtime remains possible by leaving the variable
unset. An import with unavailable components/Lua APIs fails without replacing the active
scheme. OpenCC tables required by an imported scheme must be in its own ZIP.
Release `.deb` packages include this private Rime/Lua build with a distinct library identity, so the system Rime package remains untouched.

## Install and upgrade (Debian / Ubuntu: recommended)

Use the `fcitx5-rimes_<version>_amd64.deb` and matching `.sha256` attached to
the project's [GitHub Release](https://github.com/scholay/xsime/releases).
Older releases may not have a deb; until a release with this asset is
published, build one locally using the packaging instructions below.
The release workflow builds on Ubuntu 24.04 amd64. Other Debian/Ubuntu
versions must satisfy the package's declared dependencies; rebuild on the
target distribution or architecture when they do not. This is a Fcitx5
package, not an IBus engine.

In the directory containing the downloaded files (replace the filename with
the chosen version):

```bash
sha256sum -c fcitx5-rimes_<version>_amd64.deb.sha256
sudo apt install ./fcitx5-rimes_<version>_amd64.deb
fcitx5 -r
```

Then add **XSIME** in Fcitx5 configuration. Upgrade by downloading the next
release and running the same `apt install` command. A downloaded deb does
not configure an apt repository or automatic updates.

To uninstall the deb:

```bash
sudo apt remove fcitx5-rimes
fcitx5 -r
```

Remove XSIME from the Fcitx5 input-method list as well. User dictionaries,
learning records, imported schemes, Capsule notes and original-format clipboard
history/favorites remain in
`${XDG_DATA_HOME:-$HOME/.local/share}/rimes`. Do not delete this directory
when removing the application. If a source installation already exists,
uninstall it with the instructions below before switching to the deb.

## Build and install from source (development)

All commands below run from the repository root. Source installation needs
Python 3 in addition to the build dependencies.

```bash
platforms/linux/ime/scripts/build.sh
platforms/linux/ime/scripts/install.sh --skip-build  # default prefix: ~/.local
fcitx5 -r
```

`install.sh` without `--skip-build` builds first. Keep the same prefix used
for the build; to change it, run a build for that prefix. For a system-wide
source install, use `sudo platforms/linux/ime/scripts/install.sh --prefix /usr`.
`--build-dir DIR` / `RIMES_IME_BUILD_DIR` selects a non-default build tree;
`--prefix DIR` / `RIMES_IME_PREFIX` selects the installation prefix.

Then add **XSIME** in Fcitx5 configuration. Restart the compositor/session if
the new input method does not appear.

The installer stages and records the addon, companion programs, Fcitx5
descriptors, icons, licenses **and** shared dictionary data in
`<prefix>/share/rimes/ime-install-manifest.json`. Keep this manifest; it lets
uninstall work even after the build directory has been removed. A private
engine selected with `RIMES_IME_RIME_PREFIX` still needs its original library
directory while the input method is installed.

### Uninstall a source installation

Switch to another input method, remove XSIME from Fcitx5 configuration, then:

```bash
platforms/linux/ime/scripts/uninstall.sh --dry-run
platforms/linux/ime/scripts/uninstall.sh
fcitx5 -r

# For a source installation with --prefix /usr:
sudo platforms/linux/ime/scripts/uninstall.sh --prefix /usr --dry-run
sudo platforms/linux/ime/scripts/uninstall.sh --prefix /usr
```

Use the original prefix. The script removes only recorded application files
whose SHA-256 still matches, and then empty directories. If a managed file
was edited or replaced by a symlink, it stops before deleting anything;
move or restore that file and retry. Unrecorded files are preserved, including
user dictionaries (`*.userdb`), `schemes/learning`, imported schemes, notes,
`capsule/clipboard/history-v1.sqlite3` (history and favorites),
runtime configuration and logs. It never recursively removes `share/rimes`
or `~/.local`, and it does not remove the private engine or restart Fcitx5.
`DESTDIR` is supported for staged installs and uninstalls.

For installations made before the persistent manifest was added, keep the
original build directory and corresponding source checkout, and run:

```bash
platforms/linux/ime/scripts/uninstall.sh --build-dir platforms/linux/ime/build --dry-run
platforms/linux/ime/scripts/uninstall.sh --build-dir platforms/linux/ime/build
```

This fallback restricts the original `install_manifest.txt` to known XSIME
program/resource paths and checks shared data against that source version.
Old installers did not record program hashes, so the fallback cannot detect
edits to those program files. Missing files are harmless; changed shared data
stops removal. Without either manifest, the script does not guess which files
belong to RIMES. The data preview's `platforms/linux/scripts/uninstall.sh`
is a separate installer and does not uninstall this addon. Use `apt remove`
for deb installations.

Environment overrides used by tests and unusual layouts:

| Variable | Meaning |
|---|---|
| `RIMES_SHARED_DIR` | Absolute SharedSupport root (`default.yaml`) |
| `RIMES_USER_DIR` | Isolated user dir (must not be the stock fcitx5-rime path) |
| `RIMES_LOG_DIR` | librime log directory |

## Desktop windows

Buffer's empty toolbar folds/unfolds its content; drag it to move the panel.
The function menu restores its position or moves it to a selected display.
The number button cycles automatic delivery and the adjacent switch controls
closing after the last delivered block. Both share the settings configuration.
Repeated settings launches activate the existing settings window.

X11 and Wayland layer-shell panels keep a fixed logical width and preserve
their bottom edge when the second rail opens. Positions live in the private
user-data `windows/` directory and survive upgrades and uninstall. A compositor
without layer-shell uses XWayland when available. Without either placement
backend, the compositor controls the window; return to the input field and
reopen Buffer to capture it. See [window validation](../runtime/WINDOWS-VALIDATION.md)
for tested environments and outstanding desktop/pixel comparisons.

## Packaging

```bash
python3 platforms/linux/ime/scripts/build-engine.py
platforms/linux/ime/scripts/package-deb.sh /tmp/rimes-deb
```

writes `fcitx5-rimes_<version>_<arch>.deb` and its `.sha256` (addon + `rimes-buffer` +
`rimes-buffer-ctl` + `rimes-capsule` + `rimes-capsule-ctl` + `rimes-schemes` + private Rime/Lua + reviewed data).
The release package targets Ubuntu 24.04 amd64; install with `sudo apt install ./fcitx5-rimes_1.1.5_amd64.deb`. The package manager removes application files while user dictionaries remain in the user data directory.
The package recommends `libgtk-layer-shell0`, `wl-clipboard` and the Inter/Noto
fonts (including emoji). GStreamer base/good/PipeWire plugins and
`xdg-desktop-portal` are runtime dependencies for capture. Install the portal
backend appropriate to the desktop; GTK alone does not provide screen recording.
A wlroots session can use `xdg-desktop-portal-wlr`. See
[capture validation](../capsule/CAPTURE-VALIDATION.md) for tested paths and limits.
Flatpak notes are in
`packaging/flatpak/README.md`. The data preview tarball is a separate artifact
and stays data-only.

### Versioned releases

Linux now uses the repository-root `VERSION` for source builds and ordinary
local packages. The shared `vX.Y.Z` release workflow produces the deb for
the **same tag and commit** and attaches it to the same Release as macOS.
`vX.Y.Z-preview.N` is packaged as Debian version `X.Y.Z~preview.N`, so apt
orders it before `X.Y.Z`. `RIMES_RELEASE_VERSION` supplies this tag version
in CI; a formal package version must match root `VERSION`.

The reusable `linux-deb.yml` workflow builds on Ubuntu 24.04 amd64, runs source
install/uninstall tests, then installs, reinstalls and removes the exact deb
on a disposable runner while checking user-data preservation. Publication
waits for this job; only an existing, successfully published `v*` Release
receives the deb and checksum. PR/manual/nightly rehearsals retain CI
artifacts without publishing. Source changes, installed-host typing, CI
results and publicly downloadable assets remain separate evidence.

For every version, review these install/upgrade/uninstall instructions,
dependency compatibility and Linux release notes along with the code. Verify
the actual release deb on the intended X11/Wayland desktop before claiming
desktop acceptance; headless package tests do not establish it.

## Tests

```bash
python3 -m unittest discover -s platforms/linux/ime/tests -p 'test_*.py' -v
platforms/linux/ime/scripts/run-tests.sh
```

The Python lifecycle tests use real CMake file installation in temporary
prefixes and do not install an input method on the host. The deb lifecycle
test is only for disposable Linux runners (`RIMES_DEB_TEST_DISPOSABLE=1`).

1. CTest unit tests (keysyms, UTF-8 snapshot, path isolation)
2. Live `librime` smoke: `nihao` + Space, number selection, paging, Escape
3. In-process Fcitx5 `testfrontend` (no display)
4. Optional Xvfb + live Fcitx5 DBus virtual input context
5. GTK/Qt hosts via `xdotool` (best-effort: headless Xvfb has no window
   manager, so toolkit IM modules may not commit even when DBus does)
6. Weston headless is probed when installed; injecting keys into a
   seat-less compositor is a documented limitation

GitHub Actions workflow `.github/workflows/linux-ime.yml` runs the same
sequence on `ubuntu-latest`. It is **not** a macOS merge/release gate.

## IBus follow-up

An IBus engine can reuse `RimeEngine`, `EngineSnapshot`, and the path resolver.
It would be a new `ibus-rimes` component XML plus a C++ `IBusEngine` that maps
IBus keysyms (already X11) into `ProcessKey` and commits with
`ibus_engine_commit_text`. Estimate: similar size to this addon, no Fcitx5 UI.
Not started in this tree.

## Buffer / Capsule / Mailbox

Buffer and Capsule are implemented: `BufferService` / `CapsuleService` live
in the addon. GTK companions are `rimes-buffer` and `rimes-capsule`, on
`$XDG_RUNTIME_DIR/rimes-buffer.sock` and
`$XDG_RUNTIME_DIR/rimes-capsule.sock`. `rimes-mailbox` uses one GTK application
per canonical user directory, a process lock and native D-Bus forwarding.
See [Mailbox usage, data preservation and parity](../mailbox/README.md).

See `src/engine/rime_hooks.hpp`. Ordinary Rime commits still go through
`RimesIme::commitText` → `InputContext::commitString` (or into Buffer when
capture owns that IC). Capsule insert calls `commitString` after the armed
token recheck so it cannot become a Buffer chip. One Rime session per
Fcitx5 input context. Do not start a second librime runtime.
