# iOS GitHub Actions release

Current App Store release: **1.1.8 (64)**. App Store Connect reports
`READY_FOR_SALE`; public Apple storefront lookups for China mainland, the United
States and Japan all return version **1.1.8**, released October 23, 2026 at
06:05 JST. The accepted App Review submission is
`f7bd1c0a-f2d1-4d21-9bd7-307a39f37db9` (October 22 at 22:35 JST).
Build `64` is `VALID`.

External TestFlight remains a separate review: it was submitted October 22 at
22:27 JST and assigned to the existing **RIMES Community** group. Its latest
readback on October 23 is still `WAITING_FOR_BETA_REVIEW`; App Store availability
does not establish external TestFlight availability.

The maintainer chose the **148 currently available territories**, including
China mainland, for this release. The 27 EU territories remain unavailable
pending the account's trader declaration; no legal classification was changed.
Automatic App Store release after approval and automatic TestFlight notification
remain enabled. English, Simplified Chinese and Traditional Chinese store notes
were saved and read back; TestFlight uses the existing English and Simplified
Chinese locales.

October 23 follow-up: the original build-64 unsigned CI archive contains
`org.scholay.rimes.ios.shared` in both bundles' `RIMESKeychainGroup`, without
the established `585J2TL9U6.` prefix. Signing a copy with the correct entitlement
leaves that runtime value unchanged. The repair pins the existing full group in
`RIMES_KEYCHAIN_GROUP` for both Info.plist and entitlements and gates unsigned
device builds/archives as well as the final IPA. See the
[evidence and acceptance limits](../../validation/ios-keychain-20261023/README.md).
The originally uploaded signed IPA has not been inspected in this follow-up.

The CI archive came from `cbe03560f7582326c1cd43f5c4f6a7a5304121e9`,
[run 39776428911](https://github.com/scholay/xsime/actions/runs/39776428911).
The later `851f02fc` main integration changes only three Windows files, so the
iOS inputs are unchanged. The existing cloud-managed distribution certificate,
both App Store profiles and upload API key were reused. There was no local
product build, device test or installation. See the
[1.1.8 release record](../../docs/releases/1.1.8.md).

The previous **1.1.7 (63)** was confirmed `READY_FOR_SALE` on October 22.
Its October 19 submission replaced **1.1.6 (62)** before review; see the
[1.1.7 record](../../docs/releases/1.1.7.md) and
[historical 1.1.6 record](../../docs/releases/1.1.6.md).
Historical build-60 evidence remains in [its rebuild record](AppStore/release-preparation/1.1.5-build60.md).
Build, upload, TestFlight, App Review and public availability remain separate states.
[TestFlight invitation](https://testflight.apple.com/join/Kdj9RB4q).

`ios.yml` runs unsigned builds, shared tests and hosted iOS tests on matching
pull requests targeting main and matching pushes to main. `ios-release.yml` runs
only for stable tags `ios-vMAJOR.MINOR.PATCH`, using macos-26 / Xcode 26.6 and
Fastlane 2.240.1. It does not use the maintainer's interactive Apple login.

## Reuse the existing Apple cloud signing account

The existing releases were signed with a **Cloud Managed Apple Distribution**
certificate, not a local Apple Distribution private key. On October 19 the
certificate and both matching App Store profiles were still valid through
September 2027. The existing Developer-role API key can read builds, but does
not grant cloud signing or App Review submission. Do not export unrelated
macOS identities or copy an interactive Apple session into CI.

For a maintainer-authorized release using that existing account:

1. Dispatch `ios.yml` on `main` with `archive=true` and an unused `build_number`.
   GitHub runs the normal resource checks, shared tests and hosted iOS tests,
   then archives that exact commit for a generic iOS device without credentials.
2. Download the `ios-cloud-archive-<commit>-<build>` artifact. Check its run,
   commit, version, build and ZIP SHA-256 against `archive-provenance.json`.
   The ZIP preserves archive permissions and symlinks. It is unsigned and is
   not an installable or uploaded release.
3. Keep the unsigned archive intact. If Xcode needs a development-signed archive,
   sign a copy with the existing matching development identity/profiles, including
   both targets' App Group and shared Keychain entitlements. Sign nested code
   before the keyboard extension and containing app; verify the resulting
   entitlements. Before any signing, run
   `python3 platforms/ios/scripts/verify-release-ipa.py --check-unsigned-app <archive>/Products/Applications/RIMES.app`.
   Both bundles must contain `585J2TL9U6.org.scholay.rimes.ios.shared` in
   `RIMESKeychainGroup`; signing does not repair a missing Info.plist prefix.
   This preparation is signing only, not a local build or test.
4. Open the copy in Xcode Organizer. Use Custom > App Store Connect > Export,
   automatic cloud signing and the existing authorized Apple Account. Turn off
   Manage version and build number. Verify the exported IPA with
   `scripts/verify-release-ipa.py`, then inspect both targets' signed App Group
   and shared Keychain entitlements before uploading. The verifier compares the
   runtime group to the actual signed group and to the profile's explicit or
   wildcard allowance, and records both targets' groups in the verification JSON.
   The existing upload API
   key can upload this distribution-signed IPA even though it cannot cloud-sign
   an archive or submit App Review. Never upload a development-signed copy.
5. Read back processing in App Store Connect, select that exact build in the
   matching version draft, update all three locales and review notes, then
   submit through the authorized webpage. Preserve screenshots, territories,
   pricing and automatic release after approval unless explicitly changed.
6. Record upload, processing and review-submission evidence separately. If the
   existing login or cloud signing permission fails, stop before introducing
   new credentials or claiming a submission.

Normal pull-request/push checks never publish an archive. The manual archive
path is restricted to `main`, has no signing secrets or App Store write access,
and retains artifacts for 14 days. Fully unattended release remains the
separate `ios-release.yml` path below, requiring its own credentials.

## GitHub community IPA

The mobile downloads on GitHub Release are the signed Android APK and an
**unsigned iOS IPA for user signing**, with App Store and TestFlight links.
These are separate distribution channels: publishing the IPA does not require
Apple review and does not imply that the store version is available.

Package `Payload/RIMES.app` from the original unsigned CI archive, preserving
executable permissions and symlinks. Include the keyboard extension, verify the
version/build and privacy manifests for both targets, and compare executable
digests against the CI archive. Do not publish the development-signing copy,
device provisioning profiles or credentials. Attach a SHA-256 file, CI/source
provenance and signing instructions. Read back the public download and verify
its complete digest; do not replace assets already published under the tag.

The community IPA cannot be installed directly like an APK. User signing must
cover both targets with valid profiles and authorized shared App Group/Keychain
entitlements. Changing bundle IDs alone does not preserve shared-data access;
arbitrary third-party re-signing tools are not verified. Ordinary users can use
the App Store or TestFlight links. iOS Settings updates continue to use the
actual App Store version, not the GitHub community package.

## One-time configuration

The `ios-app-store` GitHub environment has been created and restricted to
`ios-v*` tags. Add these environment Secrets through GitHub Settings; do not put them
in chat, git, workflow inputs or logs:

| Secret | Value |
|---|---|
| ASC_KEY_ID | App Store Connect API key ID |
| ASC_ISSUER_ID | Team API key issuer ID |
| ASC_PRIVATE_KEY_BASE64 | Base64-encoded downloaded .p8 key, App Manager role sufficient for release operations |
| IOS_DISTRIBUTION_P12_BASE64 | Base64-encoded Apple Distribution certificate with its private key |
| IOS_DISTRIBUTION_P12_PASSWORD | Nonempty password protecting that P12 |
| IOS_APP_PROFILE_BASE64 | App Store profile for org.scholay.rimes.ios |
| IOS_KEYBOARD_PROFILE_BASE64 | App Store profile for org.scholay.rimes.ios.keyboard |

Both profiles must use team 585J2TL9U6, the supplied certificate, the existing App
Group and shared Keychain entitlement. No new Apple permissions are granted by
the workflow. Profiles and certificates expire: replace the Secrets when renewed.
Do not export every private key from the developer's login keychain.

As inspected during setup, this repository has no iOS signing/API Secrets.
The implementation is not evidence that credentialed CI publication has run.

## Publish a new version

1. Integrate the intended iOS source changes into `origin/main` and add real
   three-locale update notes in `AppStore/release-notes/<version>.json`. Run checks
   on that commit. Local main-only maintenance is not permission to push or tag.
2. Create and push its exact version tag, e.g. `ios-v1.0.0`. Never tag an older
   commit while the intended app changes remain uncommitted in a worktree.
3. Actions requires the tagged commit to be in `origin/main` history, validates
   the tag, notes and Secrets, builds/tests, imports signing
   material into an ephemeral keychain and signs both targets. Marketing version
   comes from the tag. Build number is `(run_number + 100).run_attempt.0`.
4. It uploads the IPA, waits up to 40 minutes for Apple processing, selects that
   exact build, sends the update notes and submits for review with automatic
   release after approval. No beta testers are added.
5. Read the job summary's version/build/commit and Apple review state. Upload,
   review submission, approval and live release are distinct outcomes.

The pipeline does not withdraw a pending review, replace a different draft, change
storefront selection, accept new agreements, or resolve EU DSA classification.
A duplicate already-submitted version fails rather than submitting twice. If an
upload succeeds but processing times out, inspect Apple state before retrying;
rerunning creates a different build number. Signing material is removed on failure
as well as success; only GitHub-hosted disposable runners are supported.

The existing 0.1.0 (10) review record is historical evidence, not live Apple status.
Read App Store Connect before describing a version as approved or available.
The strict manual distribution checklist remains an evidence record; CI does not
mark outstanding real-device checks passed.
The owner-authorized tag is the release action. Review licensing/privacy and actual
device results when the relevant functionality or bundled content changes.

References: [GitHub signing](https://docs.github.com/en/actions/how-tos/deploy/deploy-to-third-party-platforms/sign-xcode-applications),
[Fastlane deliver](https://docs.fastlane.tools/actions/deliver/),
[Apple API](https://developer.apple.com/app-store-connect/api/).
