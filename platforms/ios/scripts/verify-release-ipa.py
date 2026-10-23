#!/usr/bin/env python3
"""Verify the exact package before transmitting it to Apple."""
import argparse
import hashlib
import json
import plistlib
import re
import subprocess
import tempfile
import zipfile
from pathlib import Path

TEAM_ID = '585J2TL9U6'
APP_ID = 'org.scholay.rimes.ios'
KEYBOARD_ID = APP_ID + '.keyboard'
APP_GROUP = 'group.' + APP_ID
# An existing compatibility identifier, not a value inferred from the signing team.
KEYCHAIN_GROUP = '585J2TL9U6.' + APP_ID + '.shared'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def profile_allows(patterns, value):
    """Profiles may allow prefix.*; signed app entitlements must be explicit."""
    return isinstance(patterns, list) and any(
        isinstance(pattern, str) and (pattern == value or (
            pattern.endswith('.*') and pattern.count('*') == 1
            and value.startswith(pattern[:-1]))) for pattern in patterns)


def validate_metadata(info, signed, profile, identifier, version, build):
    for key, expected in {'CFBundleIdentifier': identifier,
                          'CFBundleShortVersionString': version,
                          'CFBundleVersion': build, 'UIDeviceFamily': [1],
                          'RIMESAppGroup': APP_GROUP}.items():
        require(info.get(key) == expected, f'{identifier}: unexpected {key}')
    require(profile.get('TeamIdentifier') == [TEAM_ID], f'{identifier}: wrong profile team')
    require('ProvisionedDevices' not in profile and not profile.get('ProvisionsAllDevices'),
            f'{identifier}: not an App Store profile')
    prefixes = profile.get('ApplicationIdentifierPrefix', [])
    require(isinstance(prefixes, list) and len(prefixes) == 1
            and isinstance(prefixes[0], str) and re.fullmatch(r'[A-Z0-9]{10}', prefixes[0]),
            f'{identifier}: invalid App ID prefix')
    # App ID prefixes can differ from Team IDs (for example legacy seed IDs).
    application_id = prefixes[0] + '.' + identifier
    requested_group = info.get('RIMESKeychainGroup')
    require(requested_group == KEYCHAIN_GROUP,
            f'{identifier}: wrong Info.plist Keychain group: {requested_group!r}; expected {KEYCHAIN_GROUP}')
    allowed = profile.get('Entitlements', {})
    for label, entitlements in [('signature', signed), ('profile', allowed)]:
        require(entitlements.get('application-identifier') == application_id,
                f'{identifier}: {label} has wrong application-identifier')
        require(entitlements.get('com.apple.developer.team-identifier') == TEAM_ID,
                f'{identifier}: {label} has wrong team entitlement')
        require(not entitlements.get('get-task-allow'), f'{identifier}: {label} allows debugging')
        require(APP_GROUP in entitlements.get('com.apple.security.application-groups', []),
                f'{identifier}: {label} lacks App Group')
    require(requested_group in signed.get('keychain-access-groups', []),
            f'{identifier}: signature lacks the requested shared Keychain group')
    require(profile_allows(allowed.get('keychain-access-groups', []), requested_group),
            f'{identifier}: profile does not allow the requested shared Keychain group')
    return {'bundle_id': identifier, 'application_identifier': application_id,
            'keychain_group': requested_group, 'app_group': APP_GROUP,
            'signed_keychain_groups': signed['keychain-access-groups'],
            'profile_keychain_groups': allowed['keychain-access-groups']}


def validate_unsigned_app(app):
    """Catch lost build-setting expansion before cloud signing an archive."""
    for bundle in (Path(app), Path(app) / 'PlugIns/RIMESKeyboard.appex'):
        info = plistlib.loads((bundle / 'Info.plist').read_bytes())
        require(info.get('RIMESKeychainGroup') == KEYCHAIN_GROUP,
                f'{bundle.name}: wrong Info.plist Keychain group: {info.get("RIMESKeychainGroup")!r}')
        require(info.get('RIMESAppGroup') == APP_GROUP, f'{bundle.name}: wrong Info.plist App Group')


def verify_ipa(ipa, version, build):
    ipa = Path(ipa)
    evidence = []
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(ipa) as archive:
            archive.extractall(tmp)
        apps = list((Path(tmp) / 'Payload').glob('*.app'))
        require(len(apps) == 1, 'Expected exactly one app')
        app = apps[0]
        extensions = list(app.glob('PlugIns/*.appex'))
        require(len(extensions) == 1, 'Expected exactly one keyboard extension')
        for bundle, identifier in [(app, APP_ID), (extensions[0], KEYBOARD_ID)]:
            info = plistlib.loads((bundle / 'Info.plist').read_bytes())
            subprocess.run(['codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)
            # Current codesign otherwise emits a human-readable [Dict], not a plist.
            signed = plistlib.loads(subprocess.check_output(
                ['codesign', '--display', '--entitlements', '-', '--xml', str(bundle)],
                stderr=subprocess.DEVNULL))
            profile = plistlib.loads(subprocess.check_output(
                ['security', 'cms', '-D', '-i', str(bundle / 'embedded.mobileprovision')]))
            evidence.append(validate_metadata(info, signed, profile, identifier, version, build))
            require((bundle / 'PrivacyInfo.xcprivacy').is_file(), f'{identifier}: missing privacy manifest')
    receipt = {'version': version, 'build': build, 'sha256': hashlib.sha256(ipa.read_bytes()).hexdigest(),
               'bytes': ipa.stat().st_size, 'signatures': 'valid', 'bundles': evidence}
    ipa.with_suffix('.verification.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-unsigned-app', type=Path,
                        help='Check expanded shared groups before signing; no signature claim')
    parser.add_argument('ipa', nargs='?')
    parser.add_argument('version', nargs='?')
    parser.add_argument('build', nargs='?')
    args = parser.parse_args()
    if args.check_unsigned_app:
        if any((args.ipa, args.version, args.build)):
            parser.error('Do not combine unsigned app checks with IPA arguments')
        validate_unsigned_app(args.check_unsigned_app)
        print('PASS: unsigned app and keyboard contain the complete shared group identifiers')
    else:
        if not all((args.ipa, args.version, args.build)):
            parser.error('Expected IPA, version and build')
        verify_ipa(args.ipa, args.version, args.build)
        print('PASS: exact IPA versions, App Store profiles, signatures, shared groups and privacy manifests')


if __name__ == '__main__':
    main()
