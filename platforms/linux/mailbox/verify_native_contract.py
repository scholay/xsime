#!/usr/bin/env python3
"""Check (or import) the real AppKit Mailbox export. Never synthesize native pixels."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
SWIFT = ['MailboxModels', 'MailboxStore', 'MailboxModules', 'MailboxWorkspaceUI',
         'MailboxWorkspaceViewController', 'MailboxMessagesWorkspace', 'MailboxAdaptiveSplitView', 'MailboxLinuxFixtures']
SOURCES = [f'Sources/RimeBuffer/Mailbox/{name}.swift' for name in SWIFT]
SOURCES += ['Sources/RimeBuffer/RimeUI.swift']
SOURCES += [f'OfficialPlugins/plugins/mailbox.{module}/plugin.json' for module in ['terminal', 'chat', 'inbox']]
KEYS = ['palettes', 'metrics', 'modules', 'sources']

def validate(fixture):
    if fixture['purpose'] != 'public-parity-fixture':
        raise ValueError('Only the public native Mailbox fixture may be imported')
    expected = {path: hashlib.sha256((ROOT/path).read_bytes()).hexdigest() for path in SOURCES}
    if fixture['sources'] != expected:
        stale = [path for path in SOURCES if fixture['sources'].get(path) != expected[path]]
        raise ValueError('Native Mailbox sources changed; rebuild RimeBuffer, run mailbox-linux-fixtures, then --import-fixture. Stale: ' + ', '.join(stale))
    if set(fixture['palettes']) != {'night','day','quiet','rasta'}:
        raise ValueError('Four native theme palettes are required')
    if [m['id'] for m in fixture['modules']] != ['terminal','chat','inbox']:
        raise ValueError('Review native Mailbox module order')

def publish(path, data):
    temporary = path.with_name(path.name + '.edit')
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n')
    temporary.replace(path)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--import-fixture', type=Path)
    args = parser.parse_args()
    fixture_path = HERE/'tests/fixtures/native-mailbox.json'
    resource_path = HERE/'resources/native-mailbox.json'
    fixture = json.loads((args.import_fixture or fixture_path).read_text())
    validate(fixture)
    source = (args.import_fixture or fixture_path).parent
    for name, digest in fixture.get('images', {}).items():
        if Path(name).name != name or not name.endswith('.png'):
            raise ValueError('Invalid fixture attachment name')
        data = (source / 'images' / name).read_bytes()
        if len(data) > 24 * 1048576 or hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('Native attachment fixture differs')
        if args.import_fixture:
            target = fixture_path.parent / 'images' / name
            target.parent.mkdir(exist_ok=True)
            temporary = target.with_name(name + '.edit')
            temporary.write_bytes(data)
            temporary.replace(target)
    tokens = {key: fixture[key] for key in KEYS}
    if args.import_fixture:
        publish(fixture_path, fixture)
        publish(resource_path, tokens)
    elif json.loads(resource_path.read_text()) != tokens:
        raise ValueError('Mailbox UI tokens and native behavior fixture differ; import one native export')
    print('Mailbox native source, module, geometry, theme and fixture contract is current')

if __name__ == '__main__':
    try: main()
    except (ValueError, KeyError, OSError) as error:
        sys.exit(str(error))
