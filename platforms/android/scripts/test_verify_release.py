import importlib.util
from pathlib import Path
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('verify_release', Path(__file__).with_name('verify-release.py'))
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


def integer(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128); value >>= 7
    result.append(value)
    return bytes(result)


def field(number, value):
    if isinstance(value, int):
        return integer(number << 3) + integer(value)
    if isinstance(value, str):
        value = value.encode()
    return integer(number << 3 | 2) + integer(len(value)) + value


def attribute(namespace, name, value, compiled=False):
    result = field(1, namespace) + field(2, name)
    if compiled:
        item = field(7, field(6, value)) if isinstance(value, int) else field(2, field(1, value))
        return result + field(6, item)
    return result + field(3, str(value))


def manifest(package='org.scholay.rimes.android', version='1.2.0', code=23, compiled=False, extra=b''):
    element = field(3, 'manifest')
    for value in [attribute('', 'package', package), attribute(VERIFY.ANDROID_NS, 'versionName', version, compiled),
                  attribute(VERIFY.ANDROID_NS, 'versionCode', code, compiled)]:
        element += field(4, value)
    return field(1, element + extra)


class BundleManifestIdentityTests(unittest.TestCase):
    def test_raw_and_compiled_aapt2_values(self):
        for compiled in [False, True]:
            self.assertEqual(VERIFY.bundle_identity(manifest(compiled=compiled)), ('org.scholay.rimes.android', '1.2.0', 23))

    def test_other_identity_is_not_mistaken_for_release(self):
        self.assertEqual(VERIFY.bundle_identity(manifest(package='org.scholay.rimes.android.debug', code=22)),
                         ('org.scholay.rimes.android.debug', '1.2.0', 22))

    def test_duplicate_and_truncated_identity_rejected(self):
        duplicate = field(4, attribute(VERIFY.ANDROID_NS, 'versionCode', 24))
        for value in [manifest(extra=duplicate), manifest()[:-1], b'\x0a\xff', field(1, field(3, 'application'))]:
            with self.assertRaises(ValueError):
                VERIFY.bundle_identity(value)

    def test_wrong_namespace_does_not_supply_android_identity(self):
        element = field(3, 'manifest') + field(4, attribute('', 'package', 'org.scholay.rimes.android'))
        element += field(4, attribute('attacker', 'versionCode', 23)) + field(4, attribute('attacker', 'versionName', '1.2.0'))
        with self.assertRaises(ValueError):
            VERIFY.bundle_identity(field(1, element))

    def test_conflicting_compiled_value_rejected(self):
        attr = attribute(VERIFY.ANDROID_NS, 'versionCode', 23) + field(6, field(7, field(6, 24)))
        with self.assertRaises(ValueError):
            VERIFY.bundle_identity(field(1, field(3, 'manifest') + field(4, attr)))

    def test_valid_but_wrong_signer_is_rejected_before_staging(self):
        with mock.patch.object(VERIFY, 'run', return_value='Signer #1 certificate SHA-256 digest: ' + 'a' * 64):
            with self.assertRaisesRegex(ValueError, 'pinned production signing certificate'):
                VERIFY.verify(Path('release.apk'), Path('release.aab'), Path('/sdk'), '1.2.0', 23)

    def test_wrong_apk_package_is_rejected(self):
        signature = 'Signer #1 certificate SHA-256 digest: ' + VERIFY.pinned_certificate()
        badging = "package: name='org.scholay.rimes.android.debug' versionCode='23' versionName='1.2.0'"
        with mock.patch.object(VERIFY, 'run', side_effect=[signature, '', badging]):
            with self.assertRaisesRegex(ValueError, 'APK package/version/code'):
                VERIFY.verify(Path('release.apk'), Path('release.aab'), Path('/sdk'), '1.2.0', 23)


if __name__ == '__main__':
    unittest.main()
