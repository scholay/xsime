# XSIME iOS

Native iPhone app and keyboard extension for iOS 17+. The source version and
build number are defined in [`project.yml`](project.yml); the app and keyboard
extension use the same values. See the [1.2.0 release record](../../docs/releases/1.2.0.md)
for that delivery's channels and verification, and the [release guide](../../docs/platforms/ios/release.md)
for current build, signing and submission procedures.

Install through the [App Store](https://apps.apple.com/app/id6814620242) or an
available [TestFlight build](https://testflight.apple.com/join/Kdj9RB4q). The GitHub
community IPA requires user signing of both the app and keyboard extension.
Open the app for setup, then enable XSIME in Settings → General → Keyboard →
Keyboards and select it with the system globe. Basic input and Buffer work
without Full Access; optional AI, clipboard collection and image export have
separate access requirements described below.

## Implemented

- Offline simplified Pinyin, Ziranma, Wubi 86 and English; local Rime learning.
- During composition/candidate selection the left Settings button hides while
  the right Buffer button remains available.
- Optional hold-and-swipe symbols in ordinary layouts, off by default. Enable
  in the app's keyboard-layout page or the keyboard's Settings menu; hold for
  320 ms, slide up and release. Q–P insert 1–0; other letters and nine-key groups
  insert their labeled punctuation. Chord layouts retain their own gestures.
- With built-in Chinese input, the standard 123 and #+= pages show and type Chinese
  marks (`，。、？！：；（）“”` and `【】《》—…·‘’「」〈〉`), keeping a half-width
  `.` for decimals. A sentence mark (`，。？！、：；`) typed as the first key after
  opening 123 returns to letters; digits and other marks stay. English, imported
  schemes and the chord number page are unchanged. The nine-key punctuation key
  lists `，。？！、：；…“”（）` in the candidate row instead of a menu. Android uses
  the same table (`PunctuationLayout`), checked on both sides against one fixture.
- Imported Rime packages can be deleted from the package list with confirmation.
  Deleting the selected package restores the previous built-in scheme when the
  keyboard reopens and preserves learned dictionaries.
- Imported full-Pinyin packages can switch between 26 and 9 keys when they
  include a deployed `*_t9` sibling using the standard T9 alphabet conversion
  and the same dictionary. The sibling may be a deployment dependency, so only
  the main scheme needs selecting during import. Layout changes keep the
  package's own candidates, Lua and user dictionary; the keyboard does not
  compile resources or replace them with built-in Pinyin. Packages without a
  matching encoder retain their original layout and show an explanation in
  the layout menu.
- Extension-private last-scheme memory, independent of Full Access. An explicit
  app selection overrides it once; ordinary app configuration saves do not.
- Uppercase mechanical keycaps, depressed states and 30% opacity for unreachable
  chord keys. Haptics distinguish presses, changed combinations and commits;
  toggle them in the Settings menu. Letter, utility and tool keys share the mechanical
  cap style; candidates are borderless text with a transient touch highlight.
- Default chord now runs through the desktop-style Ziranma encoding layer:
  sequential G then H produces gang; EF maps sh to u, then H produces shang.
  Default one-finger left-to-right shortcuts: TY → ting, GH → gang, BN → bin;
  TYU → tu, GHJ → gan, BNM → bian; BH → bang, TH → tang, GY → guai. Second/third endpoints update preview;
  blank/unmapped positions preserve it, returning to the start resets the slide.
  Stored mapping JSON remains unchanged; imported profiles retain their encoding.
- The Default chord example and imported JSON mappings, with per-hand start/end sliding, live chord
  preview and a single commit when both hands release. Unmapped slide endpoints
  preserve the last valid combination; returning to the start or reaching another
  valid combination updates it. No traversed-key accumulation.
- Only two chord layouts remain: gapless orthogonal and 12 pt split orthogonal.
  Removed staggered/stacked preferences migrate to gapless orthogonal without
  resetting other settings. Chord letter caps are square, with 2 pt horizontal
  and 1 pt vertical frame gaps. Wide landscape layouts center a grid capped at
  480 pt so square caps do not inflate the keyboard height. The bottom row keeps
  its wider Space/function keys. Imported mappings and blank-slide protection remain.
- Haptics offer Off, Light, Strong and Stronger. The two stronger levels use
  medium/heavy UIKit impact generators and persist independently of the off switch;
  combinations are deduplicated with a 35 ms interval. Key-down pulses use their
  own 8 ms coalescing interval so a preceding preview cannot suppress a new key.
- Backspace deletes immediately, then after 400 ms repeats every 75 ms; lifting,
  leaving the key, cancellation, target changes, rotation and hiding stop it.
  Each deletion tick emits feedback at the selected haptic strength.
  In the Default Buffer each Delete removes the whole block before the caret (the
  block holding the caret if it is inside one); pinyin in composition, selections,
  plugin input and host fields keep their usual deletion.
- Outside chord resolution (QWERTY, numbers, Shift or EN), the cap under the finger
  at release is typed and the highlight follows the finger; taps in the 6 pt gutters
  between caps snap to the nearest cap. Chord hit testing is unchanged.
- Hold Space for 0.35 s, then drag: each 10 pt moves the caret one character (an
  emoji counts as one) in the host field or the Buffer, with a haptic tick. Releasing
  after a hold types no space. Holding during composition does not start it.
  In the chord grid Space is split: a tap on either half types a space; holding the
  right half moves the caret, holding the left half selects in the Buffer from the
  caret (Delete removes the selection; any other key clears it). iOS gives keyboards
  no API to select host text, so in app fields the left half also moves the caret,
  with a one-time note.
- Touches near the screen edges are no longer held back by the system's edge-swipe
  recognizers; their touch delay is released while the gestures stay enabled.
- Shift latches uppercase ASCII entry while preserving the chord layout. Return
  commits the engine's raw code without choosing a candidate or adding a newline;
  only automatically added chord separators are omitted. Space chooses a candidate.
- 中/EN toggles direct English and the previous Chinese scheme. Simplified/
  Traditional output is retained in More. Emoji remains in the right-side grid
  cell, or More outside the chord grid; the palette contains 29 local items.
  In the chord grid, Delete (with repeat) takes the right-hand cell below Emoji and
  中/EN takes Delete's place in the bottom row; numbers and emoji keep the usual order.
- While a chord is held the candidate row shows, mirrored about the centre: left keys,
  left mapping, the combined result in a teal pill, right mapping, right keys. `?`
  marks an unmapped hand and `—` an idle one. It is display only; release commits
  exactly the combined result.
- Explicit Buffer mode, character cursor editing, ordered next/all insertion,
  in-memory drafts and manual result confirmation. The fixed order above the keys
  is output → input → candidates. Send, plugin and the Buffer toggle form one
  right-hand column (output, input and candidate rows). Buffer shows both rows
  when enabled and hides both when disabled. Every mode uses the same two single
  lines that never wrap and scroll horizontally: a grey display-only output line
  and a teal-outlined input line with the caret. Candidates wrap by measured text
  width when expanded, including while Buffer is open; choosing a candidate or
  typing collapses them to one row until expanded again.
  Tap the paper plane for one block; hold it for one second for all remaining blocks.
  The More menu contains explicit source insertion, cursor movement and Clear.
- Local clipboard in the keyboard's Settings menu and Buffer settings. Collect
  current text uses an explicit native `UIPasteControl` tap and Full Access;
  opening the list never reads the system clipboard. Up to 40 text entries,
  16 KiB each and 128 KiB total remain in the keyboard's private, backup-excluded
  `TextClipboard/history-v1.json`. Exact duplicates move to the front; old entries
  are evicted. Delete/clear manage the history, and tapping an entry inserts it
  at the Buffer cursor without host insertion, automatic sending or plugin runs.
  Saved history is usable offline without Full Access. Changed fields, drafts,
  plugins, permissions and canceled providers cannot apply a late collection.
- After a Chinese commit, the empty candidate row offers associated words: first
  what you have typed next on this device, then continuations from the bundled Pinyin
  dictionary (`associations.tsv`, built by `scripts/build-associations.py`; e.g.
  谢谢 → 了/大家/合作). Tapping one inserts it and chains; any other key hides them
  (Space still types a space). Learning is keyboard-private, bounded to 600 words ×
  8 next words and excluded from backup. Clearing is available only in the app's
  Data management → Clear learned associations, with a destructive confirmation.
  The app sends a reset revision without reading any learned words; the keyboard
  applies it once on its next presentation. Updating the app does not request a reset.
- Composition appears inline as native marked text in the host, or at the Buffer
  cursor. Confirming a candidate replaces it once; unconfirmed text stays out of
  Buffer source revisions and translation requests. There is no separate preedit row.
- Settings is a gear at candidate-row left; input schemes and translation languages
  are nested in its menu. There is no top toolbar. A conditional system-required globe and wide Space
  key. The candidate row always reserves at least 32 pt, even when empty;
  the typing block stays anchored to the bottom while auxiliaries grow upward.
- Default Buffer shows committed characters/minute, touch starts/character,
  touch starts/second and backspace presses centered in the output line.
  Default does not duplicate source text there. As on the desktop, every commit is
  its own block (one typed word = one block); punctuation and spaces join the block
  before them and direct Latin letters join into one word. Text set whole (restored
  or cleared) falls back to clause segmentation. The input line shows these blocks as
  rounded backgrounds with a gap between them, the caret block outlined, and Send /
  automatic insertion deliver exactly these blocks; plugin output lines show their result
  blocks the same way, head block outlined. Gaps are kerning and the caret is a thin
  blinking overlay, so neither inserts characters or shifts the text. Repeats do not
  inflate touch counts; a six-second idle gap starts a new burst. No accuracy is
  inferred for free typing. Gear → Default automatic insertion offers Off/1/2/3/5 s
  (default Off). Blocks age separately, edited text restarts, composition pauses,
  and only the head is delivered through the existing proxy. Switching targets
  suspends automatic delivery until explicitly re-enabled; hiding clears drafts
  and timers. Translation/AI output remains explicitly inserted.
- Tap the Default Buffer stats readout to choose Emoji blocks, ordinary text, or
  an optional PNG in one preview. Text is inserted only after tapping Insert text;
  the gear menu has no stats-export entry. Emoji frames stay eight cells wide;
  long numbers continue on additional rows, with the complete GitHub link below.
  The preview
  has a complete drawn frame and a full-width hint at the bottom. Save to Photos
  requests add-only authorization on the explicit save; denied/restricted access
  and Photos write errors are shown as failures. Full Access is needed to export
  from the keyboard. The containing app's Typing stats card page also offers Save
  to Photos, system sharing and file export. Image copying is a secondary action.
  Preview pauses automatic insertion and does not consume Buffer or totals. The
  stats tap only previews; images are an opt-in format. Only the latest
  explicitly saved PNG is retained in the App Group, excluded from backup and
  deletable in the app. Photos copies are managed separately in Photos.
- Compiled-in Buffer plugins with serial, cancellable, revision-bound execution.
  Apple on-device translation on iOS 26+ previews after a 400 ms pause; defaults
  to Simplified Chinese → English. Download languages in the containing app first.
  iOS 17–25 retain offline input and existing AI. No automatic cloud fallback.
- Multiple HTTPS Chat Completions providers, manual model IDs, optional model lookup,
  endpoint consent, shared device-only Keychain credentials, cancellable streaming.
- Containing app with keyboard setup, typing playground, profile editor/import/export,
  privacy and license notices. English and simplified Chinese interface, light/dark,
  compact landscape keyboard.

Excluded: Xiaohe, Yoyo/Zhemei/Hanmei, runtime scripts, Mailbox, Capsule, clipboard
monitoring, dictation, music, cloud accounts/sync, subscriptions and auto-send.

## Build

Product builds and automated tests run through [iOS CI](../../.github/workflows/ios.yml).
For maintainer-requested local work, use macOS, full Xcode with the iOS SDK, CMake,
Python 3.12+, Git and the pinned build dependencies; match the Xcode version in
the workflow. The script sets
`DEVELOPER_DIR` per process and does not change `xcode-select` or install the macOS IME.

From the repository root:

```sh
platforms/ios/scripts/build.sh
```

The bootstrap verifies source revisions and archive SHA-256 values. librime 1.17.0
and pinned dependencies compile locally for arm64 iPhone and arm64 Simulator;
no macOS binaries are repackaged as iOS libraries. Logging and external plugin
loading are disabled. Dictionary deployment runs on the Mac build host, never on
first keyboard presentation. Simulator builds use ad-hoc signing so Keychain
entitlements can be exercised; device builds are unsigned until a team is configured.

Open `RIMES.xcodeproj` in Xcode after building. `project.yml` is the editable project
source; regenerate with `Vendor/ios-build/xcodegen/bin/xcodegen generate --spec
platforms/ios/project.yml` from the repository root. `Shared/` is a local Swift package.
The project uses the existing development team. Other maintainers should select
their own team in Xcode or override `DEVELOPMENT_TEAM` when building. Credentials
and profiles are not committed.

For core tests only:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer swift test --package-path Shared
```

For iOS hosted tests on a correctly configured Xcode installation:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild \
  -project platforms/ios/RIMES.xcodeproj -scheme RIMES \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' test
```

The debug app accepts `--engine-smoke` to run real on-device engine and Keychain
checks against synthetic inputs and write `Documents/development-smoke.json`.
It sends nothing over the network, and the hook is excluded from release builds.
This smoke is not a substitute for keyboard-extension lifecycle or physical testing.

## Architecture and boundaries

- `RimesCore` has no AppKit/InputMethodKit/CLI dependency. It owns portable keymaps,
  gesture reduction, Buffer state, provider requests and bounded SSE decoding.
- The original desktop encoding algorithm is preserved in a standalone mobile
  port. `scripts/verify.py` enforces source parity and the 427 mapping fixture.
- UIKit owns key hit testing, candidate UI and `UITextDocumentProxy` insertion.
  Each controller has one Rime session; initialization happens once per process.
- The containing app alone writes App Group configuration. A missing/inaccessible
  group falls back to offline defaults. Keyboard-local user dictionaries are never
  shared with the app, never synchronized, and excluded from backup.
- Keys use `kSecAttrAccessibleWhenUnlockedThisDeviceOnly` and an explicit shared
  Keychain group. They are not in JSON, exports, logs, source, screenshots or tests.
- API redirects are refused. Only explicit Buffer source text enters the request.
  Errors, partial streams, editing and lifecycle changes never auto-insert results.
- Switching fields while the keyboard stays visible cancels outstanding AI work
  and preserves pending blocks for a fresh Insert action. Hiding/resigning the
  keyboard ends the session and clears its in-memory draft.
- iOS's proxy gives no host acknowledgement of successful application-level send.
  XSIME makes one explicit proxy insertion and consumes that local block; it does
  not retry, synthesize Return or claim a message was sent.
- Basic typing, Buffer and card previews work without Full Access. Keyboard image
  export and clipboard access require Full Access; AI also requires separate
  recipient consent. Saving images requests add-only Photos permission, never
  library read access. Password fields and opting-out host apps use the
  system keyboard. No unsupported mechanisms are used to bypass those limits.

## Apple translation

Choose Buffer → Plugins → Apple translation to enable previews for the current
session, then choose a language pair. Download the on-device language models in
the containing app first. The paper plane inserts the active result; explicit
source insertion is available in More. Incomplete translation does not fall back
to source insertion. Hiding the keyboard clears drafts and disables the active
plugin for that session.

The DEBUG-only `--translation-smoke` app argument checks the Apple service using
synthetic text and writes `Documents/translation-smoke.json`; it does not download
missing models. Debug metrics contain aggregate presentation/input timings and
sampled memory, not input text or a claim of end-to-end frame latency.

## Distribution and privacy

The [release guide](../../docs/platforms/ios/release.md) documents CI archives,
Apple signing, App Store submission and community IPA packaging. Store copy and
versioned update notes live in [AppStore/](AppStore/README.md). Upload, TestFlight
review, App Review and public availability are separate states.

[Resource notices](../../docs/platforms/ios/resources.md) describe bundled licenses
and distribution evidence; [`distribution-audit.json`](distribution-audit.json)
is checked by the packaging tools. [Privacy disclosure](../../docs/platforms/ios/privacy.md)
covers local data, Full Access and optional provider requests. Preserve existing
bundle IDs, App Group, Keychain group and user data when preparing an update.
