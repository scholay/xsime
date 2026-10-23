# TestFlight and App Store handoff — 0.1.0 (10)

App Review submitted 2026-09-22 10:34 CST; waiting for review. No testers added,
no TestFlight installation or public release claimed. Build 10 physical update
is blocked by unavailable device; last verified installation is build 9.
See `AppStore/README.md` for current state and `VALIDATION.md` for actual checks.

## Signing and operator inputs

| Item | Prepared value / remaining input |
|---|---|
| Application | `org.scholay.rimes.ios` |
| Keyboard extension | `org.scholay.rimes.ios.keyboard` |
| App Group | `group.org.scholay.rimes.ios` |
| Shared Keychain group | `585J2TL9U6.org.scholay.rimes.ios.shared`, supplied through `RIMES_KEYCHAIN_GROUP` even for unsigned archives |
| Supported device/OS | iPhone, iOS 17+ |
| Developer team | `585J2TL9U6`; App Store profiles and active free-app agreement verified |
| App Store Connect app | `6814620242`, `RIMES: Chinese Keyboard`, publisher Haiyang Cui |
| Public privacy/support | Live HTTPS pages at `https://scholay.github.io/rimes/ios/`; bilingual policy and project contact |
| Device access | Connected/trusted iPhone, Developer Mode and device-side confirmations required |
| Review AI service | If reviewers need AI access, provision a separate limited demo credential through review information; never commit it |

The same team and access-group prefix must sign both targets. Read back the key
saved in the main app from the actual keyboard extension; a main-app Keychain
smoke alone does not prove sharing. Test App Group configuration separately and
verify offline default input with Full Access disabled.

## Proposed beta description

中文：RIMES 是面向 iPhone 的离线中文键盘，支持全拼、自然码双拼、五笔 86、
英文及双拇指滑动并击。可手动开启 Buffer，编辑文本后整段或分块插入。
可选 AI 使用你自己的服务与 API Key，仅在你明确同意并提交文本后工作。

English: RIMES is an offline Chinese keyboard for iPhone, with Pinyin, Ziranma,
Wubi 86, English and two-thumb slide chords. Turn on Buffer to edit text before
inserting all of it or one block at a time. Optional AI uses your own provider
and API key, and sends text only after consent and an explicit action.

## Proposed “What to Test”

1. In Settings → General → Keyboard → Keyboards, add RIMES. Leave Full Access off
   initially and switch to it using the globe key.
2. Test all five input options in Notes, Safari forms, Messages and WeChat. Check
   candidates, deletion, spaces, Return, punctuation and secure-input fallback.
3. For slide chords, move each thumb from its starting letter to one endpoint.
   Passed letters should not accumulate. Release both thumbs to commit once.
   Test both release orders, return-to-start, cross-region movement, extra fingers
   and rotation. Record comfort and reach problems before finalizing the layout.
4. Enable Buffer. Edit text, insert one block, switch fields while the keyboard
   stays visible and explicitly insert the remainder. No target switch may trigger
   automatic insertion. Hiding the keyboard ends the session and clears the draft.
5. Configure your HTTPS AI service in RIMES. Enable Full Access only to exercise
   keyboard AI, confirm the recipient, then test translation, polish and rewrite.
   Check invalid keys, rate limits, network loss, cancellation and source edits.
   Partial or failed responses must not be inserted; source text must survive.
6. Repeat in light/dark appearance, portrait/landscape, after lock/unlock and under
   extended typing. Measure cold presentation, candidate latency and peak memory.

Feedback should include device model, iOS version, app version/build, input scheme,
host app and reproduction steps. Do not attach API keys or private input text.

## Prepared review notes

- The containing app explains setup and provides a native typing playground,
  keyboard settings, custom mapping editor, AI settings and privacy notices.
- Basic keyboard input and Buffer work without Full Access, network or an account.
  A globe button invokes the system's next-keyboard action.
- AI is optional and uses HTTPS Chat Completions. The app discloses the destination
  and Buffer-only text scope. It does not read host documents or the clipboard.
- JSON imports are validated data mappings, not scripts or executable plug-ins.
- No payments, subscriptions, tracking, ads, cloud sync or automatic message send.
- Resource distribution basis is recorded in `AppStore/RESOURCE_CLEARANCE.md`; physical acceptance remains incomplete.
  The complete current notes are in `AppStore/review-notes.txt`; Apple requires
  all contact fields with them; these have now been saved and verified after reload.

These boundaries follow the [keyboard and privacy review guidelines](https://developer.apple.com/app-store/review/guidelines/).
External testers require the separate review step described in [Apple's TestFlight guidance](https://developer.apple.com/testflight/).

## Release sequence and evidence

1. Keep the audit evidence current and finish the outstanding iPhone cases in
   `VALIDATION.md`. Owner-authorized App Review submission occurred with these
   manual checks still unverified; do not retroactively mark them passed.
2. Set the real `RIMES_DEVELOPMENT_TEAM`, then run `scripts/archive.sh`. Inspect
   signing, entitlements, compiled resources, notices and privacy declarations.
3. Upload through Xcode Organizer to the correct App Store Connect app. Record the
   archive commit, version/build, upload time and processing result.
4. Add an internal tester after processing succeeds. On the real iPhone, install
   from TestFlight and rerun the core scenarios. Record results separately from
   local Xcode installation. Only this step can satisfy TestFlight delivery.
5. If external testers are later requested, supply beta review/contact information
   and complete Beta App Review before invitations.

Acceptance record to fill: source commit; archive/build ID; processing state;
tester installation source; device model; iOS version; test date; tested host apps;
Full Access off/on results; chord/candidate/Buffer/AI outcomes; cold startup;
candidate P95; peak memory; failures and follow-up evidence.
