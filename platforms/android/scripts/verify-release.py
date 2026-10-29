#!/usr/bin/env python3
"""Verify local APK/AAB identity against the same production certificate pinned by CI."""
import argparse
import ast
import base64
import hashlib
from pathlib import Path
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[3]
ANDROID_NS = 'http://schemas.android.com/apk/res/android'


def pinned_certificate():
    tree = ast.parse((ROOT / 'scripts/release/sign-android-ci.py').read_text())
    values = [ast.literal_eval(node.value) for node in tree.body if isinstance(node, ast.Assign)
              and any(isinstance(target, ast.Name) and target.id == 'CERTIFICATE' for target in node.targets)]
    if len(values) != 1 or not re.fullmatch(r'[a-f0-9]{64}', values[0]):
        raise ValueError('Missing unambiguous CI production certificate pin')
    return values[0]


def fields(data):
    """Bounded protobuf reader for AAPT2's public Resources.proto XML messages."""
    def varint(offset):
        value = 0
        for shift in range(0, 70, 7):
            if offset >= len(data):
                raise ValueError('Truncated bundle manifest')
            byte = data[offset]; offset += 1
            value |= (byte & 127) << shift
            if not byte & 128:
                return value, offset
        raise ValueError('Invalid protobuf integer')
    offset = 0
    result = {}
    while offset < len(data):
        tag, offset = varint(offset)
        number, wire = tag >> 3, tag & 7
        if not number:
            raise ValueError('Invalid protobuf field')
        if wire == 0:
            value, offset = varint(offset)
        elif wire in (1, 2, 5):
            if wire == 2:
                size, offset = varint(offset)
            else:
                size = 8 if wire == 1 else 4
            if size > len(data) - offset:
                raise ValueError('Truncated bundle manifest field')
            value = data[offset:offset + size]; offset += size
        else:
            raise ValueError('Unsupported protobuf wire format')
        result.setdefault(number, []).append(value)
    return result


def one(message, number, default=None):
    values = message.get(number, [])
    if len(values) > 1:
        raise ValueError('Duplicate bundle manifest field')
    return values[0] if values else default


def text(value):
    return value.decode('utf-8') if isinstance(value, bytes) else None


def bundle_identity(manifest):
    # Schema: https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/tools/aapt2/Resources.proto
    node = fields(manifest)
    element = fields(one(node, 1, b''))
    if text(one(element, 3)) != 'manifest':
        raise ValueError('Bundle root is not an Android manifest')
    attributes = {}
    for encoded in element.get(4, []):
        attribute = fields(encoded)
        key = (text(one(attribute, 1, b'')), text(one(attribute, 2)))
        if key in attributes:
            raise ValueError('Duplicate Android manifest attribute')
        value = text(one(attribute, 3))
        item = one(attribute, 6)
        compiled = None
        if item is not None:
            item = fields(item)
            for number in (2, 3):
                if number in item:
                    compiled = text(one(fields(one(item, number)), 1))
            if 7 in item:
                primitive = fields(one(item, 7))
                for number in (6, 7):
                    if number in primitive:
                        compiled = str(one(primitive, number))
        if value and compiled is not None and value != compiled:
            raise ValueError('Conflicting Android manifest values')
        attributes[key] = value if value else compiled
    package = attributes.get(('', 'package'))
    version = attributes.get((ANDROID_NS, 'versionName'))
    code = attributes.get((ANDROID_NS, 'versionCode'))
    if not package or not version or not code or not re.fullmatch(r'[1-9][0-9]*', code):
        raise ValueError('Bundle identity is incomplete')
    return package, version, int(code)


def run(*command):
    return subprocess.check_output([str(part) for part in command], text=True, stderr=subprocess.STDOUT)


def verify(apk, aab, sdk, version, code):
    expected = ('org.scholay.rimes.android', version, code)
    certificate = pinned_certificate()
    signature = run(sdk / 'apksigner', 'verify', '--verbose', '--print-certs', apk)
    actual = re.findall(r'^Signer #\d+ certificate SHA-256 digest: ([a-fA-F0-9]+)$', signature, re.M)
    if [value.lower() for value in actual] != [certificate]:
        raise ValueError('APK does not use the pinned production signing certificate')
    run(sdk / 'zipalign', '-c', '-P', '16', '4', apk)
    badging = run(sdk / 'aapt2', 'dump', 'badging', apk).splitlines()[0]
    match = re.fullmatch(r"package: name='([^']+)' versionCode='([0-9]+)' versionName='([^']+)'(?: .*)?", badging)
    if match is None or (match[1], match[3], int(match[2])) != expected:
        raise ValueError('APK package/version/code does not match the release target')
    verification = run('jarsigner', '-J-Duser.language=en', '-verify', aab)
    if 'jar verified.' not in verification or 'unsigned entries' in verification.lower():
        raise ValueError('AAB signature verification failed or contains unsigned entries')
    certificates = run('keytool', '-J-Duser.language=en', '-printcert', '-rfc', '-jarfile', aab)
    pem = re.findall(r'-----BEGIN CERTIFICATE-----\s*(.*?)\s*-----END CERTIFICATE-----', certificates, re.S)
    digests = [hashlib.sha256(base64.b64decode(re.sub(r'\s+', '', value), validate=True)).hexdigest() for value in pem]
    if digests != [certificate]:
        raise ValueError('AAB does not use the pinned production signing certificate')
    with zipfile.ZipFile(aab) as archive:
        entries = archive.infolist()
        if len({item.filename for item in entries}) != len(entries):
            raise ValueError('Duplicate AAB entries')
        item = archive.getinfo('base/manifest/AndroidManifest.xml')
        if item.file_size > 8 * 1024 * 1024:
            raise ValueError('Oversized AAB manifest')
        if bundle_identity(archive.read(item)) != expected:
            raise ValueError('AAB package/version/code does not match the release target')
    print(f'Verified production APK and AAB identity: {version} ({code})')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('aab', type=Path)
    parser.add_argument('--sdk', required=True, type=Path)
    args = parser.parse_args()
    version = (ROOT / 'platforms/android/VERSION').read_text().strip()
    codes = re.findall(r'^\s*versionCode = ([0-9]+)\s*$', (ROOT / 'platforms/android/app/build.gradle.kts').read_text(), re.M)
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version) or len(codes) != 1:
        raise ValueError('Ambiguous Android release version')
    verify(args.apk, args.aab, args.sdk, version, int(codes[0]))


if __name__ == '__main__':
    main()
