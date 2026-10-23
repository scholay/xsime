#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
cd "$ROOT"
python3 scripts/prepare-official-plugins.py
python3 platforms/ios/scripts/bootstrap.py
python3 platforms/ios/scripts/build-engine.py
python3 platforms/ios/scripts/prepare-data.py
python3 platforms/ios/scripts/write-notices.py
Vendor/ios-build/xcodegen/bin/xcodegen generate --spec platforms/ios/project.yml
python3 platforms/ios/scripts/verify.py
swift test --package-path Shared
xcodebuild -project platforms/ios/RIMES.xcodeproj -scheme RIMES -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath platforms/ios/build/DerivedData CODE_SIGN_IDENTITY=- CODE_SIGNING_ALLOWED=YES build
xcodebuild -project platforms/ios/RIMES.xcodeproj -scheme RIMES -sdk iphoneos -destination 'generic/platform=iOS' -derivedDataPath platforms/ios/build/DeviceDerivedData CODE_SIGNING_ALLOWED=NO build
python3 platforms/ios/scripts/verify-release-ipa.py --check-unsigned-app platforms/ios/build/DeviceDerivedData/Build/Products/Debug-iphoneos/RIMES.app
