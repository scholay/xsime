# Mailbox inbox validation — 2026-10-18

Task [#169](https://github.com/scholay/xsime/issues/169), after the workspace host. Source connectors, CLI channels, AI conversations and PTY are subsequent tasks. This implementation processes existing schema-v1 receipts and keeps native input delivery in Fcitx.

## Behavior and storage

The native public fixture now includes pending, accepted, rejected and PNG receipts, plus chat and archive records. Linux consumes the actual native filter results, messages, dates, review transitions and image hashes. The Linux-mutated document is decoded by the production macOS store: all 11 public threads, complete sources/messages/dates/reviews and every PNG hash match. Native debug build, store smoke and workspace smoke pass in isolated profiles.

Core regression tests exercise review CAS and capacity, repeated acceptance/rejection, cancellation before commit, private PNG export, explicit overwrite, symlink rejection and cleanup after failure. Capsule receives an independently stored SHA-256 PNG asset or deduplicated note. Removing either source preserves the other; a file-write failure after an already visible atomic rename cannot delete a published attachment. Writers reject contention promptly instead of blocking the input event loop.

The real GTK workspace tests clipboard ownership, copy, PNG preview and save-dialog cancel/save, exact exported bytes, Capsule deduplication, ignore state, delete cancel/confirm and independent attachment survival. The draft-completion regressions cover hiding after a durable save and typing a newer draft while the previous note is saving. The separately tested Task 18 fix is `13217938`.

## Authoritative Buffer acceptance

`receive_mailbox` contains identity, source revision and fresh host scope/generation; renderer-supplied text is rejected. The Fcitx worker validates the compiled module and current package grant, rereads the completed inbound message under its writer lock, and prepares a complete model copy. The event loop validates focus/session/password state and the exact live Unix peer before applying that copy. Staging appends without replacing selection, truncating text, capturing input or auto-sending; explicit later delivery still uses native `commitString`.

A private Mailbox journal couples review acceptance with Buffer staging. Failure rolls back only that transaction's review/status, retaining concurrent notes and metadata. Startup obtains a single transfer-host lock and recovers unfinished receipts without a user action. Cleanup after a delivered receipt retries separately. The worker/event-loop rendezvous has a bounded timeout: an expired callback cannot stage after rollback.

`mailbox-buffer-host` runs the production BufferService, actual Fcitx Instance/event loop and Unix socket against a recording native InputContext. It covers source CAS, capacity, repeated acceptance, selected source preservation, explicit native sends, module revocation, password protection, focus change after the durable journal, peer disconnect, startup recovery and a deliberately delayed callback beyond its timeout. It does not substitute renderer text or simulate paste. The existing actual-addon Capsule/Fcitx end-to-end regression also passes.

## Visual evidence

[48 actual AppKit/GTK pairs](../../../validation/linux-mailbox-inbox-20261018/README.md) cover inbox/chat/terminal/image/accepted/rejected × four themes × wide/compact at 2× and Asia/Tokyo. Production native constants now also supply action height, icon size/gap, message/action spacing, body/paragraph metrics and image limits. GTK renders real PNGs, selectable plain text and matching local-note backgrounds. Native-source hashes are checked at build time; Mailbox native changes trigger the Linux workflow.

All comparisons remain `requires_review`. The current differences include SF/PingFang versus Inter/Noto font metrics, symbol shapes, search/segment/composer controls, the Linux delete toolbar action and the native return-to-terminal action. Markdown/AI and PTY content belong to #173/#174; hover/focus/error/protected/disabled states and physical GNOME/KDE/X11/Wayland acceptance remain in #179. No font substitution or passing functional test is treated as pixel acceptance.

## Reproduction and evidence boundaries

Run the standard full Linux CTest suite. The focused tests are `mailbox-store`, `mailbox-ui`, `mailbox-buffer-host`, `RimesBufferModel` and `RimesBufferProtocol`; the native contract and session/process tests remain enabled. The final fresh pinned-engine Release/deb build passes all 68 CTests (76.54 s). All 17 source-install/version checks pass. Native and Linux fixture commands are in [README](README.md).

Local logs are `.build/linux-parity-20261017/task19-*`; public fixtures, source hashes, PNGs and differences are committed in the validation directory. An unpublished local package does not establish a public release or physical desktop acceptance. Installation and removal preserve dictionaries, learning, Mailbox messages/images/window state and Capsule assets, including independently saved receipt PNGs. The eventual public deb remains attached to the same root project `v*` Release.

The actual arm64 deb is installed, its installed Mailbox ELF/process/deep-link/window behavior exercised, reinstalled and removed on the disposable Ubuntu 24.04 runner. Every user-data sentinel hash is unchanged, including the independent Capsule receipt PNG. Package SHA-256: `d65ea110b8d58eab7d640bc2c4cab07c5d56371a8bb86a0e3734e84748f88dd2`. This local 1.1.5 artifact is unpublished. `task19-deb.log`, `task19-deb-lifecycle.log`, `task19-source-install.log`, and `task19-fcitx-e2e.log` record the checks.
