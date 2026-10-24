# XSIME Native Windows Foundation

This source prepares **Windows 1.1.5**. Published installers are listed in
[GitHub Releases](https://github.com/scholay/xsime/releases?q=windows&expanded=true);
a source change or CI artifact does not establish a public release or desktop
acceptance. Preview packages include their source commit, checksums and
`ACCEPTANCE-WINDOWS.md`. Use that checklist and the
[maintenance checklist](MANUAL-TEST.md) when testing a data-preserving upgrade.

This directory contains the native Windows implementation of RIMES. It is
separate from the Weasel data preview in the parent directory.

The in-process TSF DLL stays small:

- `RimesTsf.dll` is the Windows Text Services Framework boundary. It now
  starts an inline composition (preedit + dotted display attribute), shows a
  DPI-aware candidate window near `ITfContextView::GetTextExt`, and commits
  through a write edit session.
- `RimesBroker.exe` is a per-user process that owns the single `librime`
  instance. Missing path flags resolve to files next to the executable or to
  `%LOCALAPPDATA%\RIMES` / `%APPDATA%\RIMES`. `--install-autostart` writes a
  current-user Run key. The TSF client launches a sibling Broker on first use
  and reconnects after a crash.
- `RimesRegistrar.exe` registers or removes the TSF text service.
- `src/core` is the bounded named-pipe protocol shared by TSF and Broker.
- `librime/` pins official `rime.dll` (x64 and x86) for CI and local smokes.

TSF still must not load `librime` or do network I/O. Broker failure fail-opens.

## Sandboxed and UI-less hosts

The development source for [#133](https://github.com/scholay/xsime/issues/133)
adds `IMMERSIVESUPPORT` and `UIELEMENTENABLED` registration. Candidate UI uses
TSF Begin/Update/End negotiation: a host can render the list itself and suppress
the popup, including when no Win32 caret is available. Host selection/finalize
uses the same context and revision guards as mouse selection. Native popups
are owned by the input window and emit IME show/change/hide events.

AppContainer clients use a separate `.input` pipe. Its normal and restricted
package ACEs support both AppContainer and LPAC clients, granting
`FILE_GENERIC_READ | FILE_WRITE_DATA`, without pipe-instance creation or ACL
modification; only this endpoint has a low mandatory label. The Broker still
authenticates the actual client SID, session and PID. The sandbox client checks
the pipe's explicit owner and kernel-reported server session/PID, since it
cannot query the desktop process token. The endpoint exposes ordinary Rime
sessions and candidate guards; it has no Buffer target, settings command, AI
or delivery controls. Engine settings are read from the Broker normally.

The Broker must already be running (normally through installed autostart) for
sandbox input. Desktop hosts retain first-use launch; sandbox clients do not
launch an external process. Secure input remains fail-open with no Broker use.
Integrated search suggestions and tray/input-mode compartments are separate
features and are not advertised by this change.

`Invoke-RimesImeE2E.ps1 -AppContainer` adds a capability-free AppContainer
and an LPAC with the Win32/COM `registryRead` and `lpacCom` capabilities to the
isolated TSF/Broker/Rime test. The launcher verifies the actual token access
boundary before resuming either process, then the probes repeat Chinese input
and endpoint isolation checks.
Its TSF document host is a fixture;
passing it does not establish compatibility with SearchHost or every app.
See the [local validation record](../../../validation/windows-search-compatibility-20261014/README.md)
and the [host checklist](MANUAL-TEST.md) for the remaining desktop acceptance.

Installation and host acceptance are recorded in the
[1.1.0 delivery report](../../../validation/release-1.1.0/delivery.md).
The [1.0.0 preparation checklist](RELEASE-1.0.0.md) remains historical context.

## Interaction (macOS-aligned)

The current development branch also repairs the Buffer workbench: the default
Alt+Shift+Enter chord sends one ready block on release or drains the frozen queue
after a 0.6-second hold by default, including in hosts that intercept modified
Enter. Settings → Buffer → Shortcuts accepts a 500–3000 ms hold threshold.
Alt+Shift+S opens Settings. Candidate geometry follows the Buffer insertion caret;
source editing supports block navigation, select-all and paste. AI generation
uses one exchange rail, with return-to-edit and complete-result copy-and-close.
Buffer behavior settings expose optional 1/2/3/5-second delivery and default-on
close after the final acknowledged block. Focus changes pause delivery.

Translation defaults to a private CPU runtime with Chinese ↔ English models,
downloaded and SHA-256 verified on first use (about 182 MiB). Subsequent translation
is offline and requires no API key; this does not call a Windows system translation
API. Other target languages can use the configured AI connector. See the
[resource licenses](../translation/NOTICE.md) and
[implementation/acceptance record](../../../validation/windows-buffer-parity-20261012/implementation.md).

Where the TSF host cooperates, typing matches the macOS XSIME controller:

- Latin keys while composing stay inside the engine.
- Space commits the highlighted candidate.
- Left / Right select a candidate without moving the inline preedit caret.
- Down expands consecutive candidate pages into a matrix showing up to three
  rows. Up / Down then move between rows; more pages load as needed. Up on the
  first row or Escape collapses the matrix.
- `1`–`9` select by label from the active row.
- PageDown / PageUp page the menu.
- Escape outside the matrix cancels and commits nothing.
- A short independent Shift tap uses the scheme's Chinese/English switch.
  Its raw-code commit is collected immediately while Shift remains a host
  modifier. Other key combinations, long holds and focus changes cancel the
  tap; unavailable or older Brokers do not receive deferred input.

The candidate panel is a `WS_EX_NOACTIVATE` topmost tool window so it cannot
steal focus from the host. Horizontal pages stay on one line within the configured
maximum width and monitor work area; oversized entries are ellipsized while keeping all
selection targets. Preedit is displayed only in the host editor or Buffer.

Candidate placement uses a cloned range collapsed to the start of composition,
not the moving selection caret. Typing, backspace, internal caret movement and
candidate paging keep this origin; host scrolling/layout changes still relocate
it with the document. An unavailable rectangle retains only the current
composition's verified anchor. Commit/cancel, capture changes and focus changes
discard it. Buffer publishes its preedit start separately from the drawn caret.

Standalone processes that load librime declare UTF-8 as their active Windows
code page. This keeps the UTF-8 paths passed to librime compatible with its
dependencies' narrow filesystem calls, including Chinese user/data/log paths.
The TSF DLL does not change the host application's code page.

For the six product schemas, idle unshifted number-row keys and numeric-keypad
digits, decimal and operators (also with Shift) pass directly to the host;
composing candidate selection and Buffer capture still go through the engine.
Idle spaces in both modes and all idle
ASCII-mode keys also pass through, including at the TSF test callback.
Key prediction does not require a document read lock: a host may be holding
its text-store lock during the first test callback. The real key callback
still validates the input scope under a granted read lock before forwarding
input to the Broker. The first-key regressions exercise this boundary on
initial focus and refocus; real Codex/Electron acceptance remains separate.
Native `my_combo` batches also retain a host-only zero-width marked-text guard
from the first owned key down until Rime preedit is available. The pending
batch flag is distinct from Rime's composing flag; held keys do not repeatedly
rewrite the guard, and cancellation/focus loss clears it without committing it.
Run `Invoke-RimesImeE2E.ps1 -Architecture x64 -Chord` (and x86) separately from
the AppContainer probe to exercise actual simultaneous chord keys and releases.
The initial mode and subsequent mode flags are exchanged only when both TSF
and Broker negotiate the key-routing capability, preserving the legacy wire
format. A modifier-only state update with no composition or committed text
does not request a document write edit. This avoids losing the context when
a host denies an unnecessary write during an idle Shift tap. Custom schemas
with idle digit bindings and real desktop hosts need separate acceptance.

Settings → Appearance → Size controls the candidate font, page size (1–9),
vertical arrangement, maximum width, minimum row height and label size.
Page size changes the real librime menu pagination.
Vertical candidates use additional columns when the current monitor is too
short to fit the page. If the host temporarily cannot resolve its caret, the
panel keeps the last position within the current focus context; with no valid
position it stays hidden until a caret is available.

The tray and the TSF language-bar/input-method indicator use the shared macOS
horn from `Logo/rhino-horn.svg`. The tray's black and white variants follow the
Windows taskbar theme. The language profile selects the fourth icon group in
`RimesTsf.dll` (a two-tone horn badge) through `AddLanguageProfile`; keep the resource
order in `src/product_version.rc.in` stable when adding icons. Regenerate the
ICO sizes with `python platforms/windows/scripts/Export-RimesTrayIcons.py`
(Pillow required). Registration refreshes existing profiles after COM/TSF
verification, so an upgrade also updates the language-bar icon without removing
the profile or changing the user's enabled/default input-method choice.

Windows product shortcuts default to **Alt+Shift+…**: **Alt+Shift+B** toggles
Buffer globally; **Alt+Shift+Enter** sends from the captured Buffer;
**Alt+Shift+Up/Down** cycles through enabled Buffer plugins. Settings →
Buffer → Shortcuts supports independent modifier/key editing, click-to-record,
Escape to cancel recording, Delete to reset that binding, and reset-all.
Changes take effect on Save; closing without Save discards the draft. Letters,
digits, function and navigation keys are supported. A global shortcut requires
Ctrl, Alt or Win; only local send may use bare Enter or F6–F12. Editing shortcuts,
unsafe system chords and duplicate action bindings are rejected. Buffer summon
registration conflicts or failed saves leave the previous binding active; the
tray remains available. Settings v3 preserves explicit old/custom shortcuts
(including the indistinguishable legacy defaults) and v1/v2 key-only meanings.
Missing shortcut fields and explicit Reset use the new defaults. Save/load never
resets dictionaries, credentials or unrelated preferences.

Plugin cycling also reserves its configured native hotkeys while Buffer owns
the current foreground input target and has no unsettled composition or pending
delivery. This lets the Broker consume the chord before host accelerators even
when a running application still has an older TSF DLL loaded. Switching preserves
the exact input lease and draft, without activating or rebinding a window. Closing,
pausing, moving focus, or borrowing input for Capsule/Mailbox releases the scoped
chords; a changed binding releases the previous chord even if the new one is busy.
Conflicts show a tray notice. During composition, the updated TSF settle-and-cycle
path handles the chord. Applications retaining an older DLL must be fully restarted,
or Windows signed out and back in, to use that composition path.

Buffer remembers the last explicitly selected plugin, including Input, across
ordinary close/reopen and Broker restarts. Menu selection and plugin-cycle
shortcuts share the preference. It is stored atomically as a plugin ID in
`official-plugins-v1/buffer-selection.json`, separately from draft settings,
Buffer text/results and execution grants. The first explicit opening revalidates
the installed/enabled Buffer catalog; disabled, removed or invalid choices fall
back to Input. Re-enabling a plugin does not select it. A missing preference
starts in Input; startup never opens Buffer or restores old draft text.

Buffer's initial geometry follows macOS `BufferWindowGeometry`: align the
left edge and width to the current input field, clamp width to 520–1100 DIPs
(or the smaller monitor work area), and prefer opening below the field with
a 10-DIP gap. Short fields use their outer edge; tall documents use the caret
line for vertical placement. Insufficient space flips the panel above. If a
fresh, trustworthy caret is unavailable, use the current width (680 DIPs on
first launch) and center near the bottom of the mouse's monitor. Work-area
edges retain an 8-DIP margin. Expansion goes away from the input.

Explicit opening/rebinding and a verified new input focus recalculate this
geometry; typing does not reset a manually dragged panel. Windows probes only geometry, never
field text, through native EDIT caret bounds, a fresh TSF edit-session caret,
and bounded UI Automation queries for the focused field width. TextPattern2
is optional when TSF supplies the caret. The opening reply is tied to the
exact session, context and summon, expires after one second, and is rejected
after typing, focus changes or manual dragging. Password and stale-focus
providers are rejected. Actual coverage of browser/Electron/Office providers still needs
the manual host pass; fallback positioning is intentional.

An open Buffer folds to its toolbar 600 ms after losing input capture, matching
macOS's grace period for transient focus changes. Draft blocks, completed
results, selected plugin and in-flight generation stay in the workbench.
A fresh external TSF focus receives a short-lived, exact-session challenge;
only a foreground editable selection and fresh caret under a granted read lock
can answer it. The answer creates a new capture/opening revision, expands the
rails immediately and positions them at that caret. Password/read-only fields,
settings, obsolete replies and previously queued insertions cannot inherit
capture. Explicit close/protection disarms automatic reopening; peripheral
review still requires an explicit bind. This requires the updated TSF DLL in
each host: running applications using an older DLL must restart (or sign out).

Each accepted Rime commit now remains one source block. Only uninterrupted
ASCII/direct-input keystrokes merge into an editable tail; Backspace deletes
a finalized commit block, but edits the direct-input tail by character.
Pasted text is separately segmented without changing its concatenated text.

The configured send chord (default **Alt+Shift+Enter**) sends one block on
release, or drains the frozen queue after a 0.6-second hold. Plain/Shift Return
only settles composition and never submits the host field or sends staged text.
A composing send press only settles Rime, even for a custom non-Enter binding;
its repeats/release cannot send. The physical key owns its release even if
modifiers change. Focus, capture, configuration or visibility transitions cancel
a held send, including a return to the same target. Unassigned modified commands
remain with the host, and closing/pausing Buffer returns its send chord to the
host. Capture notifications carry an atomic key/modifier configuration to TSF
so local OnTest prediction and Broker execution agree.

Selecting Translation now arms automatic sentence translation, including
when selected before any text is typed. After 800 ms without a commit and
with no preedit, complete sentences are translated through the enabled
plugin and configured connector. Incomplete tails wait for punctuation (or
an explicit Translate action). Paused/unbound workspaces do not start new
automatic requests. Ordinary AI generation remains explicit. Switching the workbench
back to Input stops automatic translation and queued processing; source and
completed results remain available for explicit sending. These changes describe
the current source; verify the package's source commit before testing them.

Host availability is a separate acceptance gate. The current TSF registers
keyboard, display-attribute, immersive and UI-less candidate categories; the
corresponding implementation is described above. A basic registrar
`verify` success must not be reported as Chrome/Windows Search acceptance.
Use `RimesRegistrar diagnose` for read-only profile enablement and capability
categories, and `Debug-RimesWindows.ps1 -Registered` for these checks plus the
active architecture's DLL signature. Missing capabilities and unsigned DLLs
are warnings, not something the diagnostic tool silently changes. Do not
register unimplemented capabilities or weaken Windows security to remove them.

## Settings, connectors and isaac2026

The native settings window follows the macOS sidebar, segmented navigation,
cards and four themes. Appearance can follow Windows light/dark mode. Buffer
has a configurable default width and can fold its editor without discarding
text; new input expands it again. A reliable host-field width still takes
precedence when explicitly opening or rebinding Buffer.

Settings → Connectors stores multiple OpenAI-compatible API and Codex profiles.
Only the saved active profile is used for generation and AI translation. Each
API profile has an independent Windows Credential Manager entry; credentials
are also tied to the endpoint. `/models` discovery is optional and the model
can be entered manually. Editing settings, switching profiles or cancelling a
request invalidates obsolete results. API output streams into Buffer; Codex
publishes only a complete successful result. Codex uses a native executable,
its own login and an isolated `exec` session. The connection check verifies
installation/login without making a generation request. No Claude connector
is included in this preview.

Settings → Chording edits the applied **isaac2026** keymap: enable/disable,
physical key-batch interval, left/right keys, mappings and combination lookup.
Import/export supports the shared `formatVersion=1`, `fullPinyin` JSON format.
Native-scheme references and natural-code encoded profiles are rejected.
This is separate from importing an ordinary third-party Rime scheme package.
Changes take effect only on Save; Cancel preserves the applied profile.

The Broker keeps each native chord composer with its TSF session and clears
pending batches on focus/configuration changes. Pending-key negotiation allows
Space/Escape and Buffer commands to settle a held batch before it has Rime
preedit. Older loaded DLLs continue to use the original librime `my_combo`
implementation until restarted; the custom native keymap requires the new DLL.
The editable mapper covers physical batches and adjacent left/right pairing;
it does not claim every macOS release-timer behavior.

## Build

Use a Visual Studio 2022 developer environment with the x86/x64 C++ workload
and a current Windows SDK:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release

cmake --preset windows-x86
cmake --build --preset windows-x86-release
ctest --preset windows-x86-release
```

Both architectures are required. A 64-bit TSF DLL cannot be loaded by a
32-bit application, and vice versa.

CTest covers protocol, engine unit, key translation, broker options, candidate
layout, registrar metadata, the pipe endpoint, and inferred default paths. It
does not load `rime.dll`.

## Development diagnostics

The read-only diagnostic helper checks the horn resource order, TSF language-bar
icon wiring, available build artifacts, and compiled registrar metadata:

```powershell
..\scripts\Debug-RimesWindows.ps1 -Architecture x64
..\scripts\Debug-RimesWindows.ps1 -Architecture x86 -Configuration Release
```

On Windows, add `-Registered` to inspect the current XSIME language-profile
registration, including its `IconFile` and zero-based `IconIndex`. Add `-Json`
when another script or CI job needs machine-readable results. The helper is
read-only and does not register, unregister, restart, or modify user data.

## Pinned librime and typing tests

```powershell
..\scripts\Fetch-RimesLibrime.ps1 -Architecture x64
..\tests\Invoke-RimesImeE2E.ps1 -Architecture x64 -ProbeDesktop
```

`Invoke-RimesImeE2E.ps1` starts a real Broker against the isolated
`testdata/e2e` table schema and drives the real `TextService` through a Fake
TSF stack (`ITfThreadMgr` / `ITfContext` / `ITfComposition`). It asserts
preedit, candidate contents, `nihao`+Space → `你好`, number selection, paging,
Escape, Shift switching, ordinary editing and host-command pass-through.

The test-only `RimesE2EBroker.exe` and `RimesTsfE2E.exe` use a compile-time
`RIMES.E2E.Broker` pipe/mutex identity, while daily builds keep `RIMES.Broker`.
The same user/session and pipe access checks apply to both. E2E tests can run
alongside the installed input method without stopping its Broker or losing
Buffer contents; their TSF client never auto-launches a daily Broker.

Hosted GitHub `windows-2022` runners usually have a logon session and can
launch Notepad, but they are **not** a reliable interactive IME desktop: no
Chinese language pack, no user IME switch, and TSF often never attaches to a
SendInput host. The required CI assertions are therefore in-process. The
script records a desktop probe for honesty. Real-host coverage is
[MANUAL-TEST.md](MANUAL-TEST.md).

PR-time evidence is the non-required **Windows IME** workflow
(`.github/workflows/windows-ime.yml`). The older **Windows Native Foundation**
workflow stays schedule/manual only so it cannot join the macOS release gate.

See [librime/README.md](librime/README.md) for URLs and SHA-256.

## Daily-use layout

With no path flags, Broker uses:

| Role | Default |
| --- | --- |
| `rime.dll` | `<exe>\rime.dll`, else `%LOCALAPPDATA%\RIMES\runtime\rime.dll` |
| Shared data | `<exe>\shared`, else `%LOCALAPPDATA%\RIMES\shared` |
| User data | `%APPDATA%\RIMES` |
| Logs | `%LOCALAPPDATA%\RIMES\logs` |

```powershell
.\RimesBroker.exe --print-paths
.\RimesBroker.exe --install-autostart
.\RimesBroker.exe --remove-autostart
```

First TSF activation also launches `RimesBroker.exe` from the same directory
as `RimesTsf.dll` when the pipe is missing.

## Product shared data

The native Broker needs the product Lua files as well as OpenCC's standard
`s2t.json` and its referenced dictionaries. The pinned DLL does not supply them.
Stage a complete data directory using Python 3.10+ and an explicit OpenCC build:

```powershell
python ..\scripts\prepare-native-data.py stage `
  --opencc-data C:\OpenCC\share\opencc `
  --opencc-license C:\OpenCC\source\LICENSE `
  --opencc-revision 'REPLACE_WITH_FULL_OPENCC_SOURCE_COMMIT' `
  --output C:\RimesTest\shared
python ..\scripts\prepare-native-data.py verify C:\RimesTest\shared
python -m unittest discover -s ..\tests -p test_native_data.py
```

Use a new output directory. The tool copies only the reviewed 57-file XSIME
closure, the dictionaries referenced by `s2t.json`, and OpenCC's license. The
standard configuration adds four files (59 total, plus the manifest). It refuses
missing dependencies, unsafe paths, symlinks and existing output; verification
detects extra files and changed bytes after transfer. The source revision records
the supplied OpenCC build's provenance; the manifest is not a signature or an
independent attestation of that external build.

Place the resulting `shared` beside `RimesBroker.exe` or pass `--shared-data-dir`.
Run `RimesEngineSmoke` with the matching-architecture DLL and a **fresh test user
directory**, then complete [real-host checks](MANUAL-TEST.md). Successful staging
alone does not prove Windows librime, TSF or the final installer works.

## Safety boundaries

- The TSF DLL must not perform network requests or load `librime`.
- Pipe frames and every variable-length field are bounded before allocation.
- Broker failure or protocol incompatibility must fail open for typing.
- Secure-mode TSF hosts never connect to the Broker.
- Registration identifiers in `src/tsf/Guids.h` are release identity.
- Registration changes input profiles and must only run on an authorized
  test machine.

## Later-step blockers

- Product data can now be staged with the tool above. The complete product
  schemas still need to be exercised on Windows with the pinned MSVC DLL.
- Display-attribute underline depends on the host querying
  `ITfDisplayAttributeProvider`. The registrar does not add a new TSF
  category, so some hosts may skip the dotted underline.
- This development tree adds Capsule and Mailbox preview windows through the tray,
  Settings and Alt+Shift+V / Alt+Shift+M. See [PERIPHERALS.md](PERIPHERALS.md) for
  implemented workflows and remaining macOS parity gaps. This is not an installed-version claim.
- The 1.1.0 release uses an unsigned EXE installer. Windows security prompts may appear; signing is optional.
- The EXE installer handles elevated dual-architecture registration. The actual upgrade was verified; fresh-install coverage and broader host acceptance remain separate follow-up checks.
