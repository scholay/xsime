#!/usr/bin/env python3
"""Import the reviewed macOS chord encoding and schema for the Windows adapter."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CONTRACT = ROOT / 'platforms/linux/chord/resources/native-chord.json'
SOURCES = ('ChordOutputEncoding', 'ChordKeymap', 'ChordController')


def generate():
    value = json.loads(CONTRACT.read_text(encoding='utf-8'))
    if value.get('schemaVersion') != 1 or value.get('purpose') != 'public-chord-contract':
        raise SystemExit('Expected the actual macOS chord compiler export')
    # The full Linux fixture also checks Linux-only integration fixtures.
    # Validate the actual compiler sources consumed here independently.
    for name in SOURCES:
        relative = f'OfficialPlugins/native/macos/{name}.swift'
        if hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() != value['sources'].get(relative):
            raise SystemExit('Chord compiler changed; regenerate and review the native contract: ' + relative)
    schema = value['templates']['ziranma']
    processor = '    - chord_composer\n'
    start, end = schema.index('chord_composer:\n'), schema.index('speller:\n')
    if schema.count(processor) != 1 or start >= end:
        raise SystemExit('Unexpected macOS chord schema structure')
    schema = schema[:start] + schema[end:]
    schema = schema.replace(processor, '')
    schema = schema.replace('@SCHEMA_ID@', 'rimes_windows_chord_ziranma').replace('@NAME@', "'Isaac2026'")
    schema = schema.replace('prism: rimes_chord_ziranma', 'prism: rimes_windows_chord_ziranma')
    if '@ALPHABET@' in schema or '@ALGEBRA@' in schema or 'chord_composer' in schema:
        raise SystemExit('Windows must receive already encoded physical batches')
    schema = '# XSIME Windows generated chord engine schema; do not edit.\n' + schema
    tables = json.dumps({k: value[k] for k in ('syllables', 'fragments')}, ensure_ascii=False, sort_keys=True)
    def literal(name, text):
        if ')chord"' in text:
            raise SystemExit('Unexpected generated literal delimiter')
        chunks = [text[i:i + 3500] for i in range(0, len(text), 3500)]
        return f'inline constexpr char {name}[] =\n' + '\n'.join('R"chord(' + chunk + ')chord"' for chunk in chunks) + ';\n'
    return ('// Generated from the reviewed macOS native chord compiler; do not edit.\n#pragma once\n'
            'namespace rimes::windows::workbench::generated {\n'
            + literal('kChordEncoding', tables) + literal('kNaturalChordSchema', schema) + '}\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = generate()
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if not args.output.exists() or args.output.read_text(encoding='utf-8') != result:
            args.output.write_text(result, encoding='utf-8')
    print('macOS chord encoding and Windows engine schema verified')
