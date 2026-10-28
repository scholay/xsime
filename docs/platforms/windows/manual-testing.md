# Windows IME manual test checklist

Use this on a real Windows 10 or 11 machine. CI covers in-process
TSF + Broker + pinned `rime.dll` only. It does
**not** prove host compatibility.

Run the sections in this order: ordinary typing with Buffer off, existing
Buffer interactions, then installation/recovery. Record the exact source,
package and installed version separately. New features remain deferred.

Prerequisites: build x64 and x86 Release, fetch the pinned librime, copy
`rime.dll` next to `RimesBroker.exe` (or into `%LOCALAPPDATA%\RIMES\runtime`),
copy shared schemas (at least `rime-data` or the e2e table), then:

```powershell
.\RimesBroker.exe --install-autostart
# elevated, once per architecture
..\scripts\Register-RimesWindows.ps1 -Architecture x64
..\scripts\Register-RimesWindows.ps1 -Architecture x86
```

Add RIMES from Windows Settings → Time & language → Language & region →
Chinese (Simplified) → Options → Keyboards. Do not make it the system default
until the pass below is done.

Mark each row pass / fail / n/a. Attach host, DPI, and schema.

## Hosts

- [ ] `RimesTsfTestHost.exe` (plain Win32 EDIT): `nihao` then Space → `你好`;
      inline dotted preedit; candidate window under the caret
- [ ] Notepad
- [ ] Windows taskbar search (`Win+S`) and Start menu search: open, type `nihao`,
      select/commit `你好`, clear, reopen, and repeat after switching input methods
- [ ] Windows Settings search and File Explorer address/search boxes
- [ ] WordPad
- [ ] Microsoft Edge address bar and a contenteditable page
- [ ] Chrome or Firefox text field
- [ ] An Electron app (VS Code / Cursor / Slack)
- [ ] QQ / WeChat chat editor (draft only; do not send)
- [ ] Microsoft Word
- [ ] Excel formula bar
- [ ] Outlook compose
- [ ] Windows Terminal / PowerShell (expect limited or no composition)
- [ ] UWP / WinUI text box if available
- [ ] Elevated Notepad vs unelevated Broker (UIPI): keys must fail open

For each tested host record app version, x86/x64, OS build, installed IME
version/signature and schema, then results for preedit, Space/number selection,
page navigation, Backspace, Escape, Shift switching and focus changes. Mark
unavailable/blocked hosts explicitly; automated fixture results are separate.
For a host that renders candidates itself, confirm no duplicate popup and no
stale candidate callback can commit into another field. Never send test drafts.

## Composition and candidates

- [ ] Buffer is closed and capture is off throughout the ordinary-input pass
- [ ] Settings Connector API URL/model fields accept idle `0123456789` and
      numeric-keypad digits, decimal and operators with the product schemas,
      including Shift held with the keypad, without losing characters
- [ ] An idle left-Shift tap switches Chinese/English in the same focus context;
      the next English letter reaches the host even if it refuses a write edit
- [ ] A configured initial ASCII mode, shifted punctuation, composing number
      selection and Buffer ASCII capture still follow their respective routes
- [ ] `nihao` shows preedit and a candidate page; Space commits `你好`
- [ ] Number keys 1–5 select the matching candidate
- [ ] PageDown / PageUp (and `,` / `.` if the schema binds them) change pages
- [ ] Escape cancels and inserts nothing
- [ ] Backspace edits the preedit; the candidate list updates
- [ ] Enter commits the highlight (or the schema's Enter binding)
- [ ] Independent Shift taps follow the scheme's `ascii_composer/switch_key`:
      the standard schemes use left Shift to commit raw code and switch mode,
      right Shift is a no-op, and the official chord scheme switches on both
      sides. A configured raw-code commit occurs once, without delayed text
- [ ] Shift with letters, F1, Ctrl/Alt/Win, both Shift keys, long holds and
      focus changes does not accidentally switch Chinese/English
- [ ] English letters, idle Space/Return/Backspace, Ctrl+A/C/V and host
      shortcuts remain usable without an IME insertion or deletion
- [ ] Switching away mid-composition ends or hides the panel without a stray commit
- [ ] Password / PIN fields (`TF_TMAE_SECUREMODE`): no Broker traffic, no
      candidate window, keys pass through

## Placement, DPI, monitors

- [ ] 100% DPI, 150%, 200% — candidate text stays readable, not clipped
- [ ] Caret near the bottom of the screen: window flips above
- [ ] Caret near the right edge: window stays on the same monitor
- [ ] Drag the host to a second monitor and type again
- [ ] Mixed-DPI laptop + external display
- [ ] Per-monitor DPI change while composing (move window, then type)

## Broker robustness

- [ ] First activation in a desktop host with no Broker running starts `RimesBroker.exe`
- [ ] After login with autostart enabled, AppContainer hosts connect to the
      already-running Broker; with no Broker, keys fail open without a launch
- [ ] Logoff / logon starts the Broker via the HKCU Run value `RimesBroker`
- [ ] Kill `RimesBroker.exe` while focused in a host; the next key fail-opens,
      then composition works again after reconnect
- [ ] Sleep and resume; lock and unlock; Fast User Switching if available
- [ ] `--print-paths` shows sensible `%LOCALAPPDATA%\RIMES` / `%APPDATA%\RIMES`
      defaults when no flags are passed
- [ ] `--remove-autostart` then reboot: Broker does not start until first IME use

## Schemes and data

- [ ] Isolated e2e table schema (CI schema) still types `nihao`
- [ ] Product `rime_ice` with full `rime-data` + lua: first-run deploy, Space
      commit, number select (expected gap if lua scripts were not copied)
- [ ] OpenCC 简↔繁 if the user enables it

## Existing Buffer interactions

- [ ] Leave temporary Buffer text present and install/update/reinstall/uninstall:
      no save/copy or manual-exit prerequisite; only the owned IME runtime stops.
      Dictionaries, settings and credentials remain separate and retained.
- [ ] During maintenance, cached TSF activation cannot open a second engine.
      After uninstall/upgrade, a new-build cached retired DLL cannot relaunch its
      sibling Broker. Never kill the host; record any required sign-out.

- [ ] Short field: Buffer opens aligned with the field's left edge and width
      (520–1100 DIPs), 10 DIPs below its outer bottom edge
- [ ] Tall document: width still follows the field, but vertical placement
      follows the current caret line rather than the document's bottom
- [ ] Field near screen bottom: Buffer opens above; adding source/result rows
      keeps the input-facing edge fixed and expands away from the field
- [ ] No trustworthy caret / inaccessible provider: center near the bottom of
      the mouse's monitor, preserving prior width or using 680 DIPs initially
- [ ] Drag Buffer, then type: the manually chosen position is not reset;
      close/reopen or explicitly rebind to recalculate initial geometry
- [ ] Negative-coordinate monitor, mixed 100%/150%/200% DPI, narrow work area:
      geometry remains inside the work area without unreadable clipping
- [ ] New settings default to Ctrl+Shift+B (macOS Cmd+Shift+B equivalent);
      saved Ctrl+Alt shortcuts remain unchanged and can be switched in settings
- [ ] Alt+Shift+B can be explicitly selected, saved and reopened; its tray
      label matches the real chord, and a conflict/failed save preserves the
      previous working shortcut rather than silently claiming success
- [ ] Shortcut conflicts are visible; the tray still opens Buffer, and failed
      shortcut changes or failed settings saves keep the previous chord working
- [ ] Toggle/bind to a real editable target; type, cancel preedit and deliver
      exactly one block on plain Return with no duplicate release delivery
- [ ] Ctrl/Alt/Win/AltGr with Return, Backspace or Escape remains a host
      shortcut and does not send, delete or close Buffer
- [ ] Returning from translation to Input stops automatic requests and rejects
      late responses; source and reviewed results remain, with explicit Send
      required. An already issued delivery keeps its original acknowledgement
- [ ] Check select/copy/paste, target changes and close/reopen against macOS;
      record remaining differences rather than assuming complete parity

## Uninstall / coexistence

- [ ] EXE fresh install and data-preserving upgrade; cancellation and rollback
      leave the previously working installation usable
- [ ] Standard-user login with another administrator's UAC credentials:
      installation and uninstall keep dictionaries/startup in the original
      user's account (split-token implementation present; real two-account
      acceptance still required for #93)
- [ ] Missing/corrupt state, missing DLL and reinstall after uninstall;
      user dictionaries survive and other IMEs keep their registrations
- [ ] Unregister x86, x64 remains usable
- [ ] Unregister x64, no RIMES profile remains
- [ ] Weasel or other TSF IMEs still work after RIMES is removed

## Outside this stabilization pass

New plugins, Capsule/Mailbox implementation, signed MSI and making RIMES
the default keyboard. Existing unsigned-EXE security prompts remain part of
installation acceptance.
