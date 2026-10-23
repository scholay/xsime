#!/usr/bin/env python3
"""Package an unsigned CI archive for the existing Xcode cloud-signing account."""
import hashlib
import json
import plistlib
import re
import runpy
import subprocess
import sys
from pathlib import Path


def check_build(build):
    if not re.fullmatch(r'[1-9][0-9]{0,3}(?:\.[0-9]{1,2}){0,2}', build):
        raise ValueError('Expected an Apple build number, e.g. 62 or 101.1.0')


def validate_archive(archive, version, build):
    check_build(build)
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version):
        raise ValueError('Expected a stable product version')
    archive = Path(archive)
    metadata = plistlib.loads((archive / 'Info.plist').read_bytes())
    if metadata.get('ApplicationProperties', {}).get('ApplicationPath') != 'Applications/RIMES.app':
        raise ValueError('Archive is not the RIMES iOS application')
    app = archive / 'Products/Applications/RIMES.app'
    extension = app / 'PlugIns/RIMESKeyboard.appex'
    if list((app / 'PlugIns').glob('*.appex')) != [extension]:
        raise ValueError('Expected exactly the keyboard extension')
    for bundle, identifier in ((app, 'org.scholay.rimes.ios'),
                               (extension, 'org.scholay.rimes.ios.keyboard')):
        info = plistlib.loads((bundle / 'Info.plist').read_bytes())
        expected = {'CFBundleIdentifier': identifier, 'CFBundleShortVersionString': version,
                    'CFBundleVersion': build, 'UIDeviceFamily': [1]}
        for key, value in expected.items():
            if info.get(key) != value:
                raise ValueError(f'{bundle.name}: unexpected {key}')
        executable = info.get('CFBundleExecutable', '')
        if not executable or Path(executable).name != executable or not (bundle / executable).is_file():
            raise ValueError(f'{bundle.name}: missing executable')
        if not (bundle / 'PrivacyInfo.xcprivacy').is_file():
            raise ValueError(f'{bundle.name}: missing privacy manifest')
    verifier = runpy.run_path(str(Path(__file__).resolve().parents[2] / 'platforms/ios/scripts/verify-release-ipa.py'))
    verifier['validate_unsigned_app'](app)


def main(args):
    if len(args) == 2 and args[0] == '--check-build':
        check_build(args[1])
        return
    archive, version, build, commit, destination = args
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Expected the full source commit')
    validate_archive(archive, version, build)
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    package = destination / f'XSIME-{version}-{build}.xcarchive.zip'
    if package.exists():
        raise ValueError('Refusing to overwrite an existing archive package')
    subprocess.run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', archive, str(package)], check=True)
    digest = hashlib.sha256()
    with package.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    receipt = {'version': version, 'build': build, 'commit': commit, 'file': package.name,
               'bytes': package.stat().st_size, 'sha256': digest.hexdigest(),
               'signing': 'unsigned; requires Apple cloud signing before upload'}
    (destination / 'archive-provenance.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main(sys.argv[1:])
