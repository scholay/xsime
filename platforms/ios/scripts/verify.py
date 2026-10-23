#!/usr/bin/env python3
from pathlib import Path
import hashlib,json,sys,plistlib
root=Path(__file__).resolve().parents[3]; ios=root/'platforms/ios'; data=ios/'Resources/EngineData'
lock=json.loads((ios/'dependencies.lock.json').read_text())
for item in lock['data'] + lock['localInputs']:
    assert hashlib.sha256((root/item['path']).read_bytes()).hexdigest()==item['sha256'],item['path']
assert (data/'wubi86.dict.yaml').read_bytes()==(root/'rime-data/wubi86.dict.yaml').read_bytes()
checks=json.loads((data/'checksums.json').read_text())
for path,sha in checks.items():
    assert hashlib.sha256((data/path).read_bytes()).hexdigest()==sha,path
for name in ['rimes_pinyin','rimes_pinyin9','rimes_ziranma','rimes_wubi','lingxi_wubi']:
    assert (data/'build'/f'{name}.schema.yaml').exists()
    assert (data/'build'/f'{name}.prism.bin').exists()
assert (data/'associations.tsv').is_file() and 'associations.tsv' in checks
assert len(json.loads((data/'nine-key-syllables.json').read_text())) > 300
assert {str(p.relative_to(data)) for p in data.rglob('*.lua')} == {'lua/rimes_wubi_autocommit.lua'}
assert (data/'lua/rimes_wubi_autocommit.lua').read_bytes() == (root/'rime-data/lua/rimes_wubi_autocommit.lua').read_bytes()
assert not any(p.suffix == '.dylib' or 'yoyo' in p.name or 'flypy' in p.name for p in data.rglob('*'))
for name in ['App/Info.plist','Keyboard/Info.plist']:
    p=plistlib.loads((ios/name).read_bytes())
    assert p['CFBundleLocalizations']==['en','zh-Hans','zh-Hant'], f'{name}: unexpected supported localizations'
    assert p['CFBundleShortVersionString']=='$(MARKETING_VERSION)'
    assert p['CFBundleVersion']=='$(CURRENT_PROJECT_VERSION)'
    assert p['RIMESKeychainGroup']=='$(RIMES_KEYCHAIN_GROUP)'
for name in ['App/RIMES.entitlements','Keyboard/RIMESKeyboard.entitlements']:
    p=plistlib.loads((ios/name).read_bytes())
    assert p['com.apple.security.application-groups']==['group.org.scholay.rimes.ios']
    assert p['keychain-access-groups']==['$(RIMES_KEYCHAIN_GROUP)']
p=plistlib.loads((ios/'Keyboard/Info.plist').read_bytes())
assert p['NSExtension']['NSExtensionAttributes']['RequestsOpenAccess'] is True
manifest=plistlib.loads((ios/'Resources/PrivacyInfo.xcprivacy').read_bytes())
assert manifest['NSPrivacyTracking'] is False
reasons={entry['NSPrivacyAccessedAPIType']:entry['NSPrivacyAccessedAPITypeReasons'] for entry in manifest['NSPrivacyAccessedAPITypes']}
assert 'E174.1' in reasons.get('NSPrivacyAccessedAPICategoryDiskSpace', []), 'Scheme import capacity checks require the on-device disk-space reason'
# The optional connection is provider-neutral. Keep the settings explanation
# consistent with the store copy without changing endpoint or consent behavior.
provider_ui=(ios/'App/RimesApp.swift').read_text()
assert '通用接口格式' in provider_ui and 'general-purpose interface format' in provider_ui
assert not any(brand in provider_ui.lower() for brand in ['openai', 'chatgpt']), 'Unexpected provider-brand wording in iOS app UI'
# Keep mobile encoding behavior aligned with the desktop source without changing desktop packaging.
a=(root/'Sources/RimeBuffer/ChordOutputEncoding.swift').read_text().split('\nextension ChordKeymapProfile')[0]
b=(root/'Shared/Sources/RimesCore/ChordEncoding.swift').read_text().split('\n',1)[1].replace('public ','')
assert a==b,'Encoding source drift: update the shared port and run mapping/engine tests'
p=json.loads((root/'Shared/Sources/RimesCore/Resources/flyyao.json').read_text())
original=json.loads((root/'chord-keymaps/Isaac2025.json').read_text())
assert p['mappings']==original['mappings'] and len(p['mappings'])==427
print('PASS: resources, source parity, permissions and privacy manifest')
def distribution_issues(audit):
    # An explicitly accepted publisher declaration remains a declaration, not
    # independently verified clearance. Unlisted or undocumented assets fail.
    authorization = audit.get('releaseAuthorization', {})
    accepted = authorization.get('acceptedPublisherDeclarations', [])
    def accepted_asset(asset):
        if asset['status'] == 'verified':
            return True
        return (asset['status'] == 'publisher-declared'
                and asset['name'] in accepted
                and bool(asset.get('evidence', '').strip())
                and bool(authorization.get('date'))
                and bool(authorization.get('scope', '').strip()))
    return ([asset['name'] for asset in audit['assets'] if not accepted_asset(asset)]
            + [item['name'] for item in audit['releasePrerequisites'] if not item['verified']])

if '--distribution' in sys.argv:
    audit=json.loads((ios/'distribution-audit.json').read_text())
    issues=distribution_issues(audit)
    if audit['status']!='ready' or issues:
        print('DISTRIBUTION BLOCKED: '+ '; '.join(issues));sys.exit(2)
