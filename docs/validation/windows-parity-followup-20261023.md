# Windows parity follow-up · 2026-10-23

Scope: continue the Windows/macOS comparison except Mailbox. Claude, OCR,
recording and other independent capabilities remain explicitly unavailable.
Existing user preferences and durable data must survive the update.

## Implemented behavior

- Ordinary horizontal candidates use the configured width budget and stable
  visible subpages. Numeric selection resolves the currently displayed item.
  Candidate end-of-page metadata is capability-negotiated with older TSF hosts.
  Compact and expanded candidates honor the effective schema's explicit `-`/`=`
  page bindings, including reversed directions, and leave custom bindings alone.
- Settings, Capsule and screenshot shortcuts can be edited immediately. New
  registrations and persistence form one transaction; failure retains old keys.
  An unchanged shortcut that was unavailable at startup does not block unrelated
  settings or credential updates. Mailbox's reserved chord is rejected by every
  configurable action.
  Mailbox's binding and workspace behavior are unchanged.
- Buffer adds target-alignment and optional application-switch clearing settings,
  plus a move-to-current-screen action. Clearing is off by default and preserves
  external source content. AI availability is checked outside the input/UI threads;
  an unavailable connector disables generation and explains the next step. Active
  requests show elapsed monotonic seconds.
- Clipboard entries retain a bounded set of original text, HTML, RTF and image
  representations, and an ordered group of copied files. Copy restores the group;
  rich activation restores its representations and uses the guarded clipboard
  paste path; ordinary plain text delivery still uses TSF. Publications return
  the sequence acquired while owning the clipboard, so a concurrent copy cannot
  be mistaken for the restored card. Format/file assets participate in reference
  tracking and automatic cleanup. Old entries remain readable.
  Editing a file-group card's title or summary retains every attachment;
  its attachment type and body preview are read-only. Store-level updates also
  preserve the file group when invalidating stale text representations.
- Capsule's duplicate settings subpage is removed. Maintenance exposes data/log
  directories and a graceful Broker restart. Reinstallation remains disabled in
  this interface; the normal verified installer is the installation path.
  A verified restart successor waits for the predecessor's safe exit, including
  an in-progress dictionary transfer, before acquiring the engine lifetime gate.
  Installer maintenance refuses to proceed if a Broker with graceful-shutdown
  support has not exited; the legacy force-stop fallback is not used for it.
- Capsule keeps its panel open through an activation transition with a null
  destination. Native tracing captured its own tab receiving both physical
  hook events before a null `WA_INACTIVE` incorrectly queued dismissal. Actual
  outside mouse hits and known external activation targets still cancel it.
- Fresh profiles begin with chord input disabled and ChatGPT opt-in. Existing
  explicit settings and the old plugin store's implicit defaults are preserved.
- Learned dictionary management uses librime's `levers` interface rather than
  scheme import or direct replacement of live LevelDB files. The source contract
  is the macOS `UserLexiconService` and the pinned librime public API. Empty live
  sessions temporarily release their translators under the engine mutex, then
  regain their exact schema configuration and options without changing session
  IDs. Active composition blocks the transfer. Imports use validated private
  snapshots; exports replace the selected destination only after validation.

## Validation record

Local Release builds pass for x64 and x86. Each architecture has 39 passing CTest
checks, combining the complete run with the corrected restart-specific rerun.
The initial x64 run found an image-path comparison failure when launched through
the development junction; restart now compares file identity and also tests
hard-link aliases and rejection of a separate copied executable.

Both architectures pass the isolated real-librime 1.17.0 learning fixture:
three dictionaries, TSV import/export, packed snapshot restore, wrong-dictionary
rejection, active-composition protection, existing session IDs/options/page size,
post-failure typing and persistence across engine restart. The fixture's user
directory is separate from the installed input method. Compiled schema bytes
remain unchanged and no temporary schema identity is persisted. Windows IME CI
now runs this fixture before typing and packaging.

The isolated provider/model transport suite, plugin catalog/import checks (7),
and Windows foundation static checks pass locally. Native settings tests cover
immediate preferences, failed shortcut transactions, first opening, themes/DPI,
and the asynchronous learning dictionary window. Four-theme settings/Buffer
renders were exported; the new shortcut, input and maintenance pages were
visually inspected.

Both architectures pass the updated peripheral headless suite, full UI suite
and real mouse navigation at 144 DPI, including detached and borrowed input.
The null-activation cancellation was reproduced with an in-memory event trace
and fixed, with a deterministic regression before internal navigation. One
later x86 message-driven UI attempt also closed during borrowed navigation;
it had no event trace and remains an unexplained single failure. The traced
rerun passed. Both failures and successful runs are retained in the local logs;
the latter does not establish a cause for the untraced failure. Two accidentally
overlapping diagnostic processes were excluded from independent run counts.

The final shortcut/runtime/restart targeted reruns pass on both architectures.
Mock installer maintenance checks (14), uninstall boundary checks and installer
static checks pass; no real registration is changed by those fixtures.

The first remote run passed both Release/CTest jobs and the macOS/tooling checks,
but typing failed an old assertion that compared entire engine pages after a
visible subpage move. The probe now compares the actual native viewport and
checks backward restoration. The x86 run also recorded 234 ms in first-commit
statistics before disconnecting at the existing 200 ms response ceiling. Unicode
statistics initialization now happens during Broker startup; additional isolated
phase timings distinguish segmentation from aggregation without logging text.
The product chord fixture explicitly opts into chord input, and the native UI
cold-start fixture uses the production settings-command startup allowance.
These corrections require fresh validation and a fresh source-identified package.
The expanded native settings collection exceeded its previous 45-second aggregate
budget on the local desktop. A full instrumented run completed without assertion
failures; stage timings and a focused lifecycle mode now make failures diagnosable.
The aggregate allowance is 120 seconds, while individual interaction deadlines
remain bounded. Cross-thread native activation messages are serviced during waits.

Final source identity, remote CI, peripheral UI and installed-version results
are recorded separately in `validation/windows-parity-followup-20261023/`.
Local synthetic targets do not establish acceptance in every desktop app or
mixed-DPI monitor arrangement. This is a development update, not a published
release. Mailbox behavior and independent unavailable capabilities remain outside
this implementation scope.
