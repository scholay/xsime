import importlib.util
import plistlib
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location('ios_archive', Path(__file__).with_name('create-ios-cloud-archive.py'))
ARCHIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ARCHIVE)


class CloudArchiveTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.app = self.root / 'Products/Applications/RIMES.app'
        self.extension = self.app / 'PlugIns/RIMESKeyboard.appex'
        (self.root / 'Info.plist').write_bytes(plistlib.dumps({'ApplicationProperties': {'ApplicationPath': 'Applications/RIMES.app'}}))
        for path, identifier in ((self.app, 'org.scholay.rimes.ios'), (self.extension, 'org.scholay.rimes.ios.keyboard')):
            path.mkdir(parents=True)
            (path / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': identifier, 'CFBundleVersion': '62', 'CFBundleShortVersionString': '1.1.6', 'UIDeviceFamily': [1], 'CFBundleExecutable': 'RIMES', 'RIMESKeychainGroup': '585J2TL9U6.org.scholay.rimes.ios.shared', 'RIMESAppGroup': 'group.org.scholay.rimes.ios'}))
            (path / 'RIMES').write_bytes(b'executable fixture')
            (path / 'PrivacyInfo.xcprivacy').write_bytes(plistlib.dumps({}))

    def test_complete_unsigned_archive_is_accepted(self):
        ARCHIVE.validate_archive(self.root, '1.1.6', '62')

    def test_archive_with_lost_keychain_prefix_cannot_be_packaged(self):
        path = self.extension / 'Info.plist'
        info = plistlib.loads(path.read_bytes())
        info['RIMESKeychainGroup'] = 'org.scholay.rimes.ios.shared'
        path.write_bytes(plistlib.dumps(info))
        with self.assertRaisesRegex(ValueError, 'Keychain group'):
            ARCHIVE.validate_archive(self.root, '1.1.6', '62')

    def test_wrong_extension_build_is_rejected(self):
        path = self.extension / 'Info.plist'
        info = plistlib.loads(path.read_bytes())
        info['CFBundleVersion'] = '61'
        path.write_bytes(plistlib.dumps(info))
        with self.assertRaisesRegex(ValueError, 'CFBundleVersion'):
            ARCHIVE.validate_archive(self.root, '1.1.6', '62')

    def test_missing_extension_privacy_manifest_is_rejected(self):
        (self.extension / 'PrivacyInfo.xcprivacy').unlink()
        with self.assertRaisesRegex(ValueError, 'privacy manifest'):
            ARCHIVE.validate_archive(self.root, '1.1.6', '62')

    def test_unexpected_extension_is_rejected(self):
        (self.app / 'PlugIns/Unexpected.appex').mkdir()
        with self.assertRaisesRegex(ValueError, 'exactly the keyboard'):
            ARCHIVE.validate_archive(self.root, '1.1.6', '62')

    def test_invalid_apple_build_numbers_are_rejected(self):
        for build in ('', '0', '10000', '1.100', '1.1.1.1', '62-preview', '62\n'):
            with self.subTest(build=build), self.assertRaises(ValueError):
                ARCHIVE.check_build(build)
