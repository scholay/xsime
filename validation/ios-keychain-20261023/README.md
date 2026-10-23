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
- Full iOS build and hosted tests: pending remote CI.
- Original uploaded App Store IPA: on another Mac and not obtained. Local
  signing reproduction is not evidence of the exact shipped binary.
- Physical-device saving/reading of a new provider in the containing app and
  keyboard extension: not performed. No real credentials or user data were used.
- New release, Apple upload, review and availability: not yet performed.

The maintainer requested no version bump while validating this repair. Project
version/build anchors remain **1.1.8 (64)**. No new build has been uploaded or
submitted for review. A previously prepared App Store Connect 1.1.9 draft remains
unsubmitted and is not a published version. App Store publication is on hold:
[Apple requires an incremental version for an update to an already released app](https://developer.apple.com/help/app-store-connect/update-your-app/create-a-new-version).

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
The maintainer subsequently authorized installing official Xcode 26.6. Its
Apple-signed archive was verified and expanded, but macOS rejected launching
the app as incompatible with the current OS; first-run setup is not complete.
