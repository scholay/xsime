import copy
import importlib.util
import json
import plistlib
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[2] / 'platforms/ios/scripts/verify-release-ipa.py'
SPEC = importlib.util.spec_from_file_location('ios_release_ipa', SCRIPT)
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


def metadata(identifier=VERIFY.APP_ID):
    info = {'CFBundleIdentifier': identifier, 'CFBundleShortVersionString': '1.1.8',
            'CFBundleVersion': '64', 'UIDeviceFamily': [1],
            'RIMESKeychainGroup': VERIFY.KEYCHAIN_GROUP, 'RIMESAppGroup': VERIFY.APP_GROUP}
    signed = {'application-identifier': VERIFY.TEAM_ID + '.' + identifier,
              'com.apple.developer.team-identifier': VERIFY.TEAM_ID,
              'keychain-access-groups': [VERIFY.KEYCHAIN_GROUP],
              'com.apple.security.application-groups': [VERIFY.APP_GROUP], 'get-task-allow': False}
    profile = {'TeamIdentifier': [VERIFY.TEAM_ID], 'ApplicationIdentifierPrefix': [VERIFY.TEAM_ID],
               'Entitlements': copy.deepcopy(signed)}
    profile['Entitlements']['keychain-access-groups'] = [VERIFY.TEAM_ID + '.*']
    return info, signed, profile


class ReleaseIPATests(unittest.TestCase):
    def validate(self, values, identifier=VERIFY.APP_ID):
        return VERIFY.validate_metadata(*values, identifier, '1.1.8', '64')

    def test_profile_wildcard_accepts_explicit_signed_group(self):
        for identifier in (VERIFY.APP_ID, VERIFY.KEYBOARD_ID):
            with self.subTest(identifier=identifier):
                self.assertEqual(self.validate(metadata(identifier), identifier)['keychain_group'], VERIFY.KEYCHAIN_GROUP)

    def test_explicit_profile_group_is_also_accepted(self):
        values = metadata()
        values[2]['Entitlements']['keychain-access-groups'] = [VERIFY.KEYCHAIN_GROUP]
        self.validate(values)

    def test_unsigned_build_lost_prefix_is_rejected_even_with_correct_signature(self):
        for group in ('org.scholay.rimes.ios.shared', '', '$(AppIdentifierPrefix)org.scholay.rimes.ios.shared',
                      '$(RIMES_KEYCHAIN_GROUP)', 'OTHER12345.org.scholay.rimes.ios.shared'):
            values = metadata()
            values[0]['RIMESKeychainGroup'] = group
            with self.subTest(group=group), self.assertRaisesRegex(ValueError, 'Info.plist Keychain group'):
                self.validate(values)

    def test_profile_allowance_does_not_replace_signed_permission(self):
        for groups in ([], [VERIFY.TEAM_ID + '.*'], [VERIFY.TEAM_ID + '.' + VERIFY.APP_ID]):
            values = metadata()
            values[1]['keychain-access-groups'] = groups
            with self.subTest(groups=groups), self.assertRaisesRegex(ValueError, 'signature lacks'):
                self.validate(values)

    def test_wrong_or_overbroad_profile_patterns_do_not_authorize_group(self):
        for patterns in ([], ['OTHER12345.*'], ['585J2TL9U.*'], ['*'], ['585J2TL9U6*'],
                         ['585J2TL9U6.org.*.shared'], [None], VERIFY.KEYCHAIN_GROUP):
            values = metadata()
            values[2]['Entitlements']['keychain-access-groups'] = patterns
            with self.subTest(patterns=patterns), self.assertRaisesRegex(ValueError, 'profile does not allow'):
                self.validate(values)

    def test_app_id_prefix_is_read_from_profile_not_inferred_from_team(self):
        values = metadata()
        values[2]['ApplicationIdentifierPrefix'] = ['LEGACY1234']
        for entitlements in (values[1], values[2]['Entitlements']):
            entitlements['application-identifier'] = 'LEGACY1234.' + VERIFY.APP_ID
        # The established shared group is separately and explicitly authorized.
        self.validate(values)

    def test_signed_and_profile_app_identity_and_groups_must_agree(self):
        for target in (1, 2):
            for key, value in [('application-identifier', VERIFY.TEAM_ID + '.' + VERIFY.KEYBOARD_ID),
                               ('com.apple.developer.team-identifier', 'OTHER12345'),
                               ('com.apple.security.application-groups', []), ('get-task-allow', True)]:
                values = metadata()
                entitlements = values[target] if target == 1 else values[target]['Entitlements']
                entitlements[key] = value
                with self.subTest(target=target, key=key), self.assertRaises(ValueError):
                    self.validate(values)

    def test_development_and_enterprise_profiles_are_rejected(self):
        for key, value in [('ProvisionedDevices', []), ('ProvisionsAllDevices', True)]:
            values = metadata()
            values[2][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'not an App Store'):
                self.validate(values)

    def test_unsigned_gate_checks_both_app_and_keyboard(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = Path(tmp) / 'RIMES.app'
            bundles = (app, app / 'PlugIns/RIMESKeyboard.appex')
            for bundle in bundles:
                bundle.mkdir(parents=True, exist_ok=True)
                (bundle / 'Info.plist').write_bytes(plistlib.dumps(metadata()[0]))
            VERIFY.validate_unsigned_app(app)
            for bundle in bundles:
                info = metadata()[0]
                info['RIMESKeychainGroup'] = 'org.scholay.rimes.ios.shared'
                (bundle / 'Info.plist').write_bytes(plistlib.dumps(info))
                with self.subTest(bundle=bundle.name), self.assertRaisesRegex(ValueError, 'Keychain group'):
                    VERIFY.validate_unsigned_app(app)
                (bundle / 'Info.plist').write_bytes(plistlib.dumps(metadata()[0]))

    def test_ipa_receipt_records_actual_groups_and_requests_xml_from_codesign(self):
        with tempfile.TemporaryDirectory() as tmp:
            ipa = Path(tmp) / 'release.ipa'
            with zipfile.ZipFile(ipa, 'w') as archive:
                for folder, identifier in [('Payload/RIMES.app', VERIFY.APP_ID),
                                           ('Payload/RIMES.app/PlugIns/RIMESKeyboard.appex', VERIFY.KEYBOARD_ID)]:
                    archive.writestr(folder + '/Info.plist', plistlib.dumps(metadata(identifier)[0]))
                    archive.writestr(folder + '/PrivacyInfo.xcprivacy', plistlib.dumps({}))
            def read_signing(command, **kwargs):
                identifier = VERIFY.KEYBOARD_ID if 'RIMESKeyboard.appex' in command[-1] else VERIFY.APP_ID
                values = metadata(identifier)
                if command[0] == 'codesign':
                    self.assertIn('--xml', command)
                    return plistlib.dumps(values[1])
                return plistlib.dumps(values[2])
            with mock.patch.object(VERIFY.subprocess, 'run'), mock.patch.object(VERIFY.subprocess, 'check_output', side_effect=read_signing):
                receipt = VERIFY.verify_ipa(ipa, '1.1.8', '64')
            self.assertEqual({b['bundle_id'] for b in receipt['bundles']}, {VERIFY.APP_ID, VERIFY.KEYBOARD_ID})
            self.assertEqual(json.loads(ipa.with_suffix('.verification.json').read_text()), receipt)


if __name__ == '__main__':
    unittest.main()
