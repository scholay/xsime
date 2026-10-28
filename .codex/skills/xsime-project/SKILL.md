---
name: xsime-project
description: XSIME (犀术IME) five-platform input method development guide for macOS, iOS, Android, Windows and Linux. Use when changing the Rime/librime bridges, key routing, composition, candidates, scheme import and per-scheme preferences, settings, Buffer, Capsule, plugins, permissions, installers or release records; preserve compatibility identifiers, user data and each platform's text-delivery contract. Formerly `rimes-project`.
---

# XSIME Project

## Start Here

Use this skill in the `scholay/xsime` checkout. Resolve paths from the current repository root, not a previous developer's workspace.

1. Read `docs/README.md`, `docs/architecture/system.md`, and `docs/development/maintenance.md` for the current structure and working rules.
2. Identify the platform and layer: key routing, composition, candidates, engine bridge, scheme import/preferences, settings, Buffer/Capsule/plugins, permissions, installation, release, or docs. Read its implementation before changing behavior.
3. For additional macOS context, consult `references/fable-architecture.md` or `references/buffer-ui.md` as needed. They contain dated implementation notes: current source, the system architecture, and maintenance rules take precedence. Their old local-build checklists do not authorize running product builds or tests locally.

## Naming And Compatibility

Follow `docs/development/branding.md`. Display names are 犀术IME / 犀術IME / XSIME; old brand names belong to history and migration notes. Preserve compatibility identifiers: `RIMES.app` and the `RIMES` executable, the `RimeBuffer` SwiftPM target, bundle/input-source IDs, Windows CLSIDs and registry keys, Android applicationId, Linux package/addon IDs, user-data paths, preference keys, scheme IDs, plugin IDs, and published asset names. Rime / librime keep their upstream names.

## Platform Map

| Platform | Entry points | Contract to preserve |
|---|---|---|
| Shared | `Shared/Sources/RimesCore/`, `Shared/InputHabits/`, `Shared/SchemeImport/`, `Shared/CandidateMatrix/` | Shared Swift serves macOS/iOS; C++ helpers and portable contracts serve other frontends. Do not assume one shared runtime or UI implementation. |
| macOS | `Sources/RimeBuffer/RIMESController.swift`, `RimeEngine.swift`, `CompositionSession.swift`, `InputFocusCoordinator.swift`, `Delivery.swift`; `Sources/CRimeBridge/` | One Rime session per IMK controller; marked text while composing; every commit reaches the client through `Delivery.insert`; preserve bridge ABI layout. |
| iOS | `platforms/ios/App/`, `Keyboard/KeyboardViewController.swift`, `Support/Settings.swift`, `Support/ProxyTextDelivery.swift`, `Bridge/RimeMobile.mm` | App/extension preferences use revisions; delivery checks the live document identity; no private APIs to identify the host app. |
| Android | `platforms/android/app/src/main/java/org/scholay/rimes/android/` (`RimesInputMethodService.java`, `EngineWorker.java`); `platforms/android/native/rime_jni.cpp` | Engine work is serial; apply coherent preference snapshots; revalidate the exact InputConnection/editor lease; one touch produces one output. |
| Windows | `platforms/windows/native/src/tsf/TextService.cpp`, `broker/main.cpp`, `engine/`, `workbench/` | TSF owns host edits; Broker owns engine/workbench work. Keep ordinary sandbox input separate from privileged controls and preserve x64/x86 behavior. |
| Linux | `platforms/linux/ime/src/fcitx/rimes_ime.cpp`, `engine/`, `platforms/linux/buffer/`, `platforms/linux/runtime/` | Preserve Fcitx5 context ownership, cancellable deployment and coexistence; never write other input methods' data. |

## Configuration And Delivery

- Keep scheme resources/user patches, product preferences, and device settings separate. Read actual schema capabilities rather than guessing from names.
- Scope overrides by `builtin:<schema>` or `<package-id>:<schema>`. Missing fields inherit; an explicit false remains a choice. Restore only overrides this product owns, preserve unknown keys, and roll back failed deployment to the previous working state.
- Apply schema changes after composition safely ends. App/keyboard writes must detect revision drift; layout changes must not reinterpret an active touch.
- Buffer requests and results carry source revision, workspace/plugin identity, and generation. Validate the live target and current block before each insertion; stale callbacks cannot consume or overwrite newer content.
- Consume only successfully accepted deliveries. Platform API acceptance is not proof that the destination saved or sent anything. Failed and unsent blocks remain available.
- Use `Sources/RimeBuffer/BufferModel.swift`, `BufferDeliveryCoordinator.swift`, and `BufferWindowController.swift` for macOS Buffer changes; other platforms retain their own native delivery adapters.

## Permissions And User Data

- Never restore external UI polling, `state.json` synchronization, paste injection or Accessibility text injection for ordinary commits.
- Keep `~/Library/RIMES` isolated from Squirrel's `~/Library/Rime`. Installation, updates and recovery preserve persistent user data; Buffer's transient content follows its separate lifecycle.
- Request permissions only after an explicit action. macOS capture uses `Sources/RimeBuffer/Capture/CapturePermissionGuide.swift`; ordinary typing or opening a settings page must not trigger permission prompts.
- Never put private input, clipboard contents, credentials, raw validation logs or acceptance screenshots into Git. Follow `validation/README.md` for local acceptance attachments.

## Checks, Installs And Releases

- Product builds and automated tests run on remote CI under `docs/development/maintenance.md`. Do not run product builds/tests on the maintainer's machine by default.
- Relevant workflows are `ci.yml`, `release.yml`, `ios.yml`, `android.yml`, `windows-ime.yml`, `windows-native.yml`, `linux-ime.yml`, and `linux-deb.yml`. When remote execution is authorized, inspect results for the exact commit; do not claim a pass from a submitted workflow or an older run.
- Install or run a live input-method replacement only when requested. Preserve signing identities, compatibility IDs and user data; verify the actually running build rather than relying solely on installer receipts.
- Before handing work back, inspect the diff, run `git diff --check`, and confirm build outputs, local logs and acceptance attachments remain ignored.
- Track issue → fixing commit → automated check → real-host confirmation → released version. Merge/build/CI success alone does not close a typing issue. Read release versions from actual published assets when verifying a release; do not infer them from merge dates.
- Current workflow and signing details live in `docs/release/README.md` and `docs/release/reference.md`. Repository documentation does not establish the current online release or store-review state.

## Debugging

macOS behavior logs live at `~/rimebuffer.log`; librime logs live under `~/Library/RIMES/`. For typing failures record the host identity, schema, key route, engine handled flag, commit path, composition state, candidate-rect source and Buffer state, without collecting text or secrets.
