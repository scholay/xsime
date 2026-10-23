# iOS shared Keychain group investigation

Reported symptom: saving an AI provider in App Store 1.1.8 (64) returns
OSStatus `-34018`. The containing app saves its API key before persisting the
provider configuration (`Support/Settings.swift`, `App/RimesApp.swift`).

## Evidence

- Original CI source: `cbe03560f7582326c1cd43f5c4f6a7a5304121e9`,
  [run 39776428911](https://github.com/scholay/xsime/actions/runs/39776428911),
  artifact `12426038858`. The complete download SHA-256 matches the release
  record: `08706edeb5704b3ae7cd1a6bb2dbda629b4ca2f3a3b24453b289569bdb10e6d2`.
- Both original Info.plists contain `org.scholay.rimes.ios.shared`. The build
  uses `CODE_SIGNING_ALLOWED=NO`; its `AppIdentifierPrefix` substitution is empty.
- A locally ad-hoc-signed copy contains the correct full shared Keychain
  entitlement, but retains the unprefixed Info.plist value. After replacing the
  runtime value with the repaired build-setting value and signing the copy
  again, both bundles' group comparisons and strict signature checks pass.
  See [machine-readable evidence](signing-reproduction.json).
- On this Mac, `codesign --display --entitlements -` prints a human-readable
  `[Dict]`; `--xml` is required for `plistlib` parsing.

Apple explains that `-34018` is an unauthorized Keychain access-group request
in [Troubleshooting -34018 Keychain Errors](https://developer.apple.com/forums/thread/114456).
[TN2415](https://developer.apple.com/library/archive/technotes/tn2415/_index.html)
explicitly permits profile wildcard allowances such as `PREFIX.*` while the
app's signature carries concrete groups.

## Repair

- Preserve the existing compatibility group
  `585J2TL9U6.org.scholay.rimes.ios.shared`. Define it through
  `RIMES_KEYCHAIN_GROUP` for both targets' Info.plists and entitlements, without
  depending on signing-time prefix expansion. No Keychain migration or fallback
  into a private/default access group is introduced.
- Reject malformed runtime groups in the actual unsigned device build and
  before packaging the cloud-signing archive.
- The final IPA verifier requests XML, validates each bundle's identity and
  shared groups, accepts profile wildcard authorization, and requires concrete
  signed permissions. App ID prefixes are read from the profile independently
  of Team ID. Explicit exceptions keep checks active under Python `-O`.
- Retain actual group values in the verification receipt for future uploads.

## Validation and remaining acceptance

- Local release-tool suite: 118 tests passed, including 16 iOS archive/IPA tests.
- Local signing reproduction: original runtime group rejected; corrected copy
  passes both group comparison and strict signature validation.
- Full iOS build and hosted tests passed on repair commit `bc1858a` in
  [run 39942595962](https://github.com/scholay/xsime/actions/runs/39942595962):
  281 tests, 5 conditional skips and 0 failures, including the Keychain
  save/read/delete test and the unsigned device artifact group check.
- The general CI and macOS runtime/package installation/upgrade checks passed.
  [PR #276](https://github.com/scholay/xsime/pull/276) was squash-merged as
  `9ed518bab8a43104469036f71ea901e99bc894d1`.
- Xcode 27.1 cloud-signing comparison: both IPA signatures are valid, but the
  original unprefixed runtime group remains unchanged after export and fails
  both bundles' metadata checks. The corrected group passes the complete IPA
  verifier, including with Python optimization enabled. These are old-archive
  reproduction copies, not a new source build or either originally uploaded IPA.
  See [cloud-signing comparison](cloud-signing-comparison.json) and
  [optimized verifier results](verifier-cli-results.json).
- Original uploaded App Store IPA: on another Mac and not obtained. Local
  signing reproduction is not evidence of the exact shipped binary.
- Physical-device saving/reading of a new provider in the containing app and
  keyboard extension: not performed. No real credentials or user data were used.
- New release, Apple upload, review and availability: not yet performed.

The maintainer approved an iOS-only **1.1.9 (65)** repair release after
confirming that [Apple requires an incremental version for an update to an
already released app](https://developer.apple.com/help/app-store-connect/update-your-app/create-a-new-version).
Other platforms remain at **1.1.8**. The App Store Connect 1.1.9 draft is prepared;
the new source archive, upload and review states must be recorded independently.
See [release preparation](../../platforms/ios/AppStore/release-preparation/1.1.9-build65.md).

Apple Build Metadata for the exact uploaded 1.1.8 (64) build was subsequently
read after signing in: both targets contain the full expected Keychain group.
See [Apple build metadata](apple-build-metadata.json). This rules out a missing
shared Keychain entitlement in that upload. The page does not expose the custom
Info.plist runtime group; original IPA bytes remain unavailable.

At the initial inspection, this Mac had Command Line Tools and a development
signing identity but no full Xcode installation. GitHub repository and `ios-app-store`
environment secret lists were empty on October 23. The existing automated
App Store workflow therefore cannot sign/upload until its credentials are
configured, or the established interactive cloud-signing path is available.
Official Xcode 26.6 could not launch on this Mac's OS. Compatible Xcode 27.1
was subsequently installed, the maintainer accepted its license and signed into
the original Apple account, and cloud export reused the existing distribution
certificate and App Store profiles. Both exported comparison packages remain
local and have not been uploaded.
