# XSIME Android

Native Java Android input method for daily Chinese input and local dictionary
learning, requiring Android 8.0 / API 26 or later. The source version is defined
in [`VERSION`](VERSION), with `versionCode` in [`app/build.gradle.kts`](app/build.gradle.kts).
See the [1.2.0 release record](../../docs/releases/1.2.0.md) for that delivery's
packages and verification, and the [release guide](../../docs/platforms/android/release.md)
for build, signing and upgrade requirements.

Install the signed APK, enable XSIME in the system input-method settings, then
select it as the active keyboard. The release identity is `org.scholay.rimes.android`;
the independent development package is `org.scholay.rimes.android.debug`.
Upgrade with the same package and signing identity, preserving application data.

## Main app settings

Open **XSIME** from the launcher (**XSIME Dev** for a debug build), or the system input-method settings entry.
The home follows the iOS grouped navigation: welcome, getting started, typing,
and data/privacy. Try typing has its own page; trial text is never restored.
Settings include the default Chinese scheme, standard and chord layouts, an
actual geometry preview, 18 themes, all 427 built-in chord mappings, local
translation direction, OpenAI-compatible provider configuration, the AI Mock switch, word learning and bundled licenses.
The keyboard and main app share validated preferences. Nine-key selects Pinyin;
another Chinese scheme returns it to QWERTY. Selecting a regular scheme exits
chord mode. Turning Mock off cancels existing AI work and revokes its output while
preserving Buffer source; local translation remains available.

## Input

- Offline librime 1.17.0: simplified Pinyin, natural-code Shuangpin and Wubi 86.
  Default Pinyin; the last Chinese schema is remembered. English and numeric
  input remain available while Chinese resources initialize; failures show Retry.
- The footer 中/英 control changes language. The candidate row's left settings icon opens
  layout, Chinese schema and palette choices; its right layers icon toggles Buffer. There
  are nine candidates per page, with tap selection and previous/next buttons.
  Composing text is displayed through the host's `InputConnection` when Buffer
  is off, and in the Buffer input rail when it is on. Hosts that publish `TYPE_NULL`
  for their own editor (the Douyin live-room chat box does) still get Chinese input;
  only numeric and password fields stay on the literal route, and unknown classes
  keep private, non-learning resources without Buffer.
- Space selects the first candidate on the current page. Without composition it
  inserts a space. Return during composition confirms the raw input code only;
  the same press never also sends Buffer. Delete edits composition first.
- Switching language/schema settles raw code into the current route. Buffer's
  existing blocks remain. Buffer cannot toggle during composition or queued work;
  turning it off never sends stored blocks.
- Buffer keeps composition/candidates inside the keyboard. Each confirmed Rime
  string is a whole block; English still forms word blocks. Insert sends the next
  block, Insert all sends the remainder, and Delete removes a whole last block.
  Failed framework acceptance retains text for retry, including already queued
  results. Storage is bounded to 16,384 UTF-16 units in the block store.
- Changing fields, hiding/switching the keyboard or destroying the service
  revokes queued engine work and delivery authority, and clears transient state.
  Ordinary keys also retire held touch streams and queued native clicks; a late
  pointer cannot acquire a different key after the target or mode has changed.
  Android hosts may finish and retain the already displayed preedit in the *old*
  editor before disconnecting it. XSIME never replays it into the next editor.

## Keyboard layouts and appearance

The candidate-row settings icon, Buffer appearance button and Setup open the
layout and appearance chooser. The layout and theme are stored with the existing
schema/learning preferences; changing colors does not settle composition or reset
Buffer. The chooser also exposes schema selection, Clear Buffer and the system
keyboard picker. There is no persistent toolbar or separate 20 dp preedit row.
A 28 dp status row appears only during preparation, failure or pending delivery.

The normal stack follows iOS: optional Buffer, candidates, optional nine-key
spelling choices, then the key surface. Each chrome row is separated by 4 dp.
Root padding is 5 dp plus Android navigation/cutout insets.

| Region | Portrait | Landscape |
|---|---:|---:|
| Buffer above candidates | 76 dp: 36 + 4 + 36 | 60 dp: 28 + 4 + 28 |
| Candidate row | 32 dp | 32 dp |
| Nine-key spelling row, while open | 34 dp | 34 dp |
| Standard 26/9/numeric/symbol/emoji key surface | 206 dp | 143 dp |
| Standard visible key cap / vertical gap | 44 / 10 dp | 32 / 5 dp |
| Standard horizontal cap gap | 6 dp | 5 dp |
| Chord footer | 40 dp | 34 dp |

Letter touch cells remain continuous and equal-width, extending into the visible
cap gaps. The four standard touch rows are 51.5 / 35.75 dp; cap geometry is
independent of their hit areas. Nine-key Return spans two visible rows (98 / 69 dp).

- **26-key QWERTY** centers its staggered letter rows, with Shift/Delete flanking
  row three. The footer has numbers, emoji, language, a wide Space, and Return.
  Return displays the real editor action, raw-code confirmation or Buffer insert.
- **9-key Pinyin** uses the iOS telephone arrangement, `ABC` through `WXYZ`, a
  double-height Return, separator, punctuation and spelling selection. Choosing
  this layout selects Pinyin. Natural-code, Wubi, English and protected fields
  use QWERTY; returning to Pinyin restores the chosen nine-key preference.
  `64426` offers `你好` through librime. “选拼音” opens a separate spelling row
  without replacing the candidate row; deleting a pinned syllable's separator
  returns it to its digit spelling. The punctuation key lists
  `，。？！、：；…“”（）` in the candidate row; a mark confirms the first
  candidate of any composition before it, and any other key closes the list.
- **Orthogonal / split orthogonal chord** uses the iOS built-in 427 mappings and
  Natural Code encoding through the existing `rimes_ziranma` engine, including
  private/off-learning variants. The split surface has a 12 dp hand gap. Its
  letter-grid height is `3 × (min(width, 480) / 10 − 1)` dp, or
  `3 × ((min(width, 480) − 12) / 10 − 1)` dp for split mode. The separate footer
  follows below. English or
  Shift uses ordinary literal letters on the same surface. Choosing another
  Chinese schema returns to QWERTY. Mapping provenance and all 427 independently
  generated iOS encoding fixtures are in [resources/chord-provenance.md](resources/chord-provenance.md).
- **Numbers / symbols / emoji** have dedicated pages. In Chinese input the 123
  and #+= pages show and type Chinese marks (`，。、？！：；（）“”` and
  `【】《》—…·‘’「」〈〉`), keeping a half-width `.` for decimals; English and
  password/numeric fields keep the half-width pages. A sentence mark
  (`，。？！、：；`) typed as the first key after opening 123 returns to letters;
  digits and other marks stay on the page. iOS uses the same table, checked on
  both sides against `Shared/Tests/RimesCoreTests/Fixtures/punctuation-layout.tsv`.
  Numeric/symbol rows keep the letter-key width; the emoji page offers 30 fixed choices without recording
  recents. Emoji are committed through the existing native input route.
- **18 palettes** mirror iOS: native, Rhino, hermit crab, kitten, puppy, piglet,
  dog, poodle, pig, rabbit, crab, penguin, fox, panda, turtle, octopus, frog and
  chick. Every palette has light/dark colors, functional caps and visible press
  feedback. Palette names and colors are retained; controls use one pinned Lucide vector family. Tap the top-left pet to cycle through the 18 skins; hold it to open the pet chooser, then select Key layout & schema for input options. Animated pets are not included.
- **Buffer's two rows** match the iOS source/output positions: ordinary Buffer
  has source above statistics; a selected plugin has output above source. Send
  stays at the upper right; settings, the optional Run button and plugin selector
  occupy the lower row. The duplicate edit and close buttons have been removed; explicit paste and clear remain in the pet panel. Only one pet control is visible while Buffer is open. Long-press Send inserts all ordinary Buffer blocks.
  A confirmed block has its own rounded chip, and preedit stays in the active chip. Visual block gaps never become spaces in delivered text. Near
  the 16,384-unit limit only visible chips are drawn; unchanged confirmed blocks
  are not remeasured when composition changes. Target loss clears both drawing
  and accessibility projections.
- Candidate refreshes reuse the keyboard surface. Candidates are plain text with
  press feedback, natural widths and horizontal scrolling. With no composition,
  candidates or held chord, this row shows the five iOS Buffer plugin shortcuts:
  翻译, 快问, 润色, 作诗 and 画画. Each is 68 × 30 dp with a 6 dp gap.
  Tap enables Buffer and selects the plugin; tapping it again returns to ordinary
  Buffer. Long-press opens its settings; switching keeps source blocks intact.
  Translation defaults to the bundled local Chinese–English dictionary. Configure
  an OpenAI-compatible provider in the main app for real AI and optional contextual AI translation;
  otherwise AI entries use a clearly labelled OpenAI-format local Mock. Translation runs automatically 400 ms after confirmed Buffer input; further edits cancel the older result. Other AI plugins use Run; Cancel stops work,
  and Send/Return insert only a completed result. Private/password fields have no entries.
  Punctuation remains on the numeric/symbol page and nine-key punctuation control.
  Key labels size within fixed touch cells for system fonts.

During a Chinese chord, each hand has one finger with a fixed start and movable
endpoint. Held keys and eligible next keys are highlighted; the candidate row
shows both hand fragments and the combined preview. Holding or releasing only
one hand sends nothing to the engine or host; the released hand remains frozen.
The final finger lift resolves once into Natural Code input, then normal Rime
candidate selection confirms Chinese through the direct or Buffer route. The
nine built-in endpoint slide shortcuts also follow iOS. This is not an arbitrary
custom gesture system.

Chord Space is a single “空格” tap target for candidate selection or a space.
iOS's divided Space with two glyphs supports left-half hold/drag for Buffer
selection and right-half hold/drag for caret movement. Those split-Space visuals
and editing gestures are deferred with full Buffer editing on Android.

A second finger on the same hand, an extra pointer on a gap/utility, or Android
`ACTION_CANCEL` cancels the batch. Cancellation remains in force until every
finger lifts. Field/selection changes, hiding, keyboard switches, surface
resizing/detachment and layout/language changes retire the old touch stream;
delayed releases cannot restore candidates or type into a new target. Candidate
selection, Buffer-send and chord-footer mode controls are blocked while fingers
are held; opening settings cancels the held gesture.

These are iOS-derived logical dimensions and keycap styles, not a pixel-perfect
claim across platforms. Android dp/sp density and font scale, system fonts and
emoji, navigation insets and rasterization differ from iOS pt and Apple fonts.
Plugin execution, full Buffer editing and system keyboard actions also differ.
Compare cropped key/Buffer regions at the same logical width, and keep system
bottom rows and host chrome out of the geometry comparison.

Layout/language/page changes settle pending raw code through the current input
route and preserve confirmed Buffer blocks. Switching a theme only changes colors.
Nine-key schemas have the same private/off-learning variants and share the
existing Pinyin user dictionary; they are compiled on the build machine.

## Translation and AI

Translation performs CC-CEDICT word/phrase lookup in both directions, using a
pinned **125,166-entry** source. Automatic direction follows the source script;
Buffer settings also expose Chinese → English and English → Chinese. The build
machine precompiles a compact radix index; phones lazily load it on the dedicated
plugin worker. No dictionary downloads or compilation occur on the phone. Unknown
fragments retain their original Unicode text; partial coverage is labelled and
zero matches fail without consuming source. This is lexical lookup, not contextual
sentence translation. [Source, license and conversion](resources/dictionary/README.md)
include CC BY-SA 4.0 attribution for both original and derived dictionary data.

**Online AI is opt-in.** In **AI services → Connection settings**, enter an HTTPS API
URL, model and key, enable online AI, optionally enable AI translation, and save.
A base URL or full `/chat/completions` URL is accepted. DeepSeek defaults to
`https://api.deepseek.com` with `deepseek-flash`
([official documentation](https://api-docs.deepseek.com/api/create-chat-completion/));
other OpenAI-compatible providers can use their own URL and model. Legacy profiles
without a URL retain their original CometAPI recipient and encrypted key.
Changing to another host or port requires entering that provider's key.
Ask, polish, poem and art send the current Buffer source only after explicit Run.
When the user enables AI translation, confirmed input in the selected Translation plugin is sent automatically after the same debounce. Local dictionary lookup, ordinary non-plugin typing, password and private fields do not call the service. Ask, polish and poem
produce real text; art still produces text prompts, not images. Online AI
translation translates sentences rather than performing dictionary lookup.

The key is encrypted with AES-GCM using a non-exportable Android Keystore key.
Settings show only a blank masked input; blank on Save preserves the existing key for the same service.
Remove key disables the online route. Configuration changes revoke in-flight and
completed results. There are no embedded credentials, key/prompt/response logs,
request history, automatic retries, or redirects. Network errors preserve source
and do not fall back to mock replies. HTTP 401/403, 402, 429, redirects, server
errors, wrong content types, truncation and invalid UTF-8 fail closed.

Requests have a 10-second connect timeout, 20-second read timeout and 60-second
total deadline. Cancellation disconnects the network asynchronously. Output is
limited to 2,048 tokens and 16,384 UTF-16 units; wire size is limited to 1 MiB.
DeepSeek requests disable thinking. The JSON/SSE codec requires a successful
`finish_reason` plus `[DONE]` and complete stream validation before Send is enabled.

When online AI is off, the optional **Mock** remains an explicitly labelled local
demo. Its fragmented UTF-8 SSE frames and 40 ms delays test the same streaming
interface; they are not real model inference or provider-performance evidence.

Type source into the lower Buffer rail. Translation runs automatically after confirmed input; other AI plugins start with Run or a clean Return.
During execution Run becomes Cancel; preview remains unsendable. Complete output
appears in the upper scrollable rail. Send, long-press Send or a subsequent Return
inserts the complete result as **one block**. Only framework acceptance consumes
its exact captured source. A failed host insertion retains source and completed
output for retry; execution failure retains source and discards partial output. Repeated
Return while running cannot duplicate the request. Composition Return still
settles raw code only, without executing on that same press.

Every callback carries a frozen source capture, plugin request identity and input
lease. Source edits, cancellation, direction/plugin changes, host selection/field
changes, hiding or service destruction invalidate old output. Settings reselecting
the same plugin preserve the current result. Source/output stay in memory only;
there is no request/response history or prompt logging. Both are bounded to 16,384
UTF-16 units. Ordinary Buffer retains its existing next/all block delivery behavior.

## Local learning and privacy

“Learn words on this device” in Setup is on by default. Rime candidate selections,
including Buffer selections, update local user dictionaries. There is no complete
input-history log or cloud synchronization. Open Buffer and tap its paste button
to explicitly read the current clipboard text; it is appended as a preserved block,
without inserting into the host or running a plugin. Empty/non-text and over-capacity
clips leave the draft intact. Composition, private fields, hidden keyboards and
changed targets cannot read or apply a paste. There is no clipboard monitoring.
The keyboard settings also offer **Local clipboard**, or hold the Buffer paste
button to open it. **Collect current text** explicitly saves plain text in
`no_backup/text-clipboard/history-v1.json`: at most 40 entries, 16 KiB each and
128 KiB total, exact duplicates move to the front and oldest entries are evicted.
Entries can be deleted or cleared and tapped into Buffer; no AI or host insertion
runs automatically. Private fields cannot view, collect or insert the history.
This is local explicit collection, not a background clipboard history or sync service.
The INTERNET
permission serves explicitly configured and invoked online AI and user-requested
official plugin downloads.
Turning learning off preserves existing data and uses precompiled schema variants
with `enable_user_dict: false`. A live setting change settles unfinished raw code
through the current route, preserves confirmed Buffer blocks, and switches the
engine before subsequent keys. Private editors use the same variants and disable
Buffer. Password/numeric input bypasses Chinese composition; passwords have no
candidate UI. An OEM may independently substitute its own secure keyboard.

System assets live in versioned `no_backup/rime-system/<manifest hash>` directories.
User dictionaries live separately in `no_backup/rime-user`; upgrades do not replace
this directory. Preferences stay in `shared_prefs/keyboard.xml`. User dictionaries
are excluded from backup; cloud/device-transfer rules also exclude app data.
Only validated, precompiled resources are extracted on-device. There is no runtime
maintenance/deployment or dictionary compiler invocation. Official plugin packages
are separately verified against the bundled catalog before installation.

A provider may reject a request with `403 / region_restricted`. XSIME shows the
provider error and preserves the source; service availability depends on the
configured provider and network.

## Build

Product builds and automated tests run through [Android CI](../../.github/workflows/android.yml).
For maintainer-requested local work, use JDK 21 and set `JAVA_HOME` and `ANDROID_HOME`. Gradle 9.7.1 and AGP 9.4.0 are
pinned. Install compile SDK 37.2, build tools 36.0.0, NDK 29.0.14206865 and CMake 3.22.1:

```sh
sdkmanager 'platforms;android-37.2' 'build-tools;36.0.0' 'ndk;29.0.14206865' 'cmake;3.22.1'
python3 scripts/prepare-official-plugins.py
cd platforms/android
python3 scripts/build-engine.py
./gradlew --no-daemon :core:test :app:assembleDebug :app:lintDebug
"$ANDROID_HOME/build-tools/36.0.0/zipalign" -c -P 16 -v 4 app/build/outputs/apk/debug/app-debug.apk
```

The Android-only builder reads the iOS dependency lock, three minimal mobile schemas and
licenses, plus the Android-owned nine-key resources in `resources/`. It never invokes iOS generation or writes iOS resources. Its `.native/`
cache, generated assets and ABI libraries are ignored. Optional
`--source-cache /path/to/Vendor/ios-build` exports the exact pinned Git commits and
checks the Boost archive checksum; it does not copy source modifications or builds.
Both `arm64-v8a` and `x86_64` are built by default. Gradle fails if assets, native
inputs, receipts or library alignment do not match.

Output: `app/build/outputs/apk/debug/app-debug.apk`. The existing development key
is used; install with `adb -s "$RIMES_ANDROID_SERIAL" install -r ...` to preserve data.
Never uninstall or clear application data during an ordinary upgrade.

The APK includes third-party licenses in `assets/licenses/`. Dictionary sources
are pinned by URL and SHA-256 in `platforms/ios/dependencies.lock.json`; Wubi data
is LGPL-3.0 and Pinyin data Apache-2.0. librime and every linked dependency retain
their original license notices. The official Lucide icon subset is pinned and hash-verified; its complete ISC/MIT
notice is packaged too. No iOS executable code or Apple framework is linked.

## Checks and diagnostics

`core:test` covers Buffer boundaries, editor revocation, immutable snapshots,
keyboard geometry and gesture cancellation. App instrumentation exercises the
real JNI bridge, resources, rendering and delivery policies. The separate
`testhost` app verifies actual host text in native, private, password and WebView
fields; it is not included in the keyboard APK. The [workflow](../../.github/workflows/android.yml)
is the source for the complete automated command sequence.

From `platforms/android`, the host-side native contract runs with:

```sh
python3 scripts/test-linux-native.py
```

It requires x86_64 Linux and e2fsprogs, downloads a checksum-pinned AOSP runtime,
and exercises Android native libraries through Bionic. This does not replace
Android UI or physical-device testing.

For device diagnostics, build the instrumentation and test-host packages:

```sh
./gradlew :app:assembleDebugAndroidTest :testhost:assembleDebug :testhost:assembleDebugAndroidTest
```

Install the test APKs with `adb install -r`. Keep another enabled IME selected
while running [EngineInstrumentation](app/src/androidTest/java/org/scholay/rimes/android/EngineInstrumentation.java),
then force-stop the instrumented keyboard process before selecting XSIME for
[InputContractInstrumentation](testhost/src/androidTest/java/org/scholay/rimes/testhost/InputContractInstrumentation.java).
This isolates the test engine from the live keyboard service. Both runners accept
`-e mode <mode>`; the host runner includes layout, chord, touch, plugin, soak and
benchmark modes. Record the exact package, build and mode with each result.

Benchmarks report worker/JNI or host TextWatcher timings, not hardware touch-to-frame
latency. Use a consistent device, theme, orientation and sample count; engine
benchmarks use separate synthetic dictionaries. A short benchmark is not a soak
test. Preserve and restore the original default IME and rotation settings, and
record OEM secure-keyboard substitution or skipped password cases separately.
Keep screenshots and raw diagnostic output local or in CI artifacts.
