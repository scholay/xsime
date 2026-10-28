#!/usr/bin/env python3
from pathlib import Path
root=Path(__file__).resolve().parents[3];base=root/'platforms/ios'
text='RIMES iOS — License and third-party notices\n\n'
text+='RIMES application code and original minimal schemas: Apache-2.0, Copyright 2026 scholay. Historical MIT grants and separately identified component licenses are retained below.\n'
text+='Core maintainer: 学术海 — https://pm.scholay.com . Project source: https://github.com/scholay/xsime .\n'
text+='librime 1.17.0, commit 33e78140250125871856cdc5b42ddc6a5fcd3cd4: BSD-3-Clause.\n'
text+='Build dependencies: LevelDB (BSD), Marisa (BSD/LGPL dual, BSD option), OpenCC (Apache-2.0), yaml-cpp (MIT), Boost (BSL-1.0). Revisions: dependencies.lock.json. OpenCC bundles Marisa and RapidJSON headers with the notices below.\n'
text+='pinyin_simp dictionary: rime/rime-pinyin-simp, commit 0c6861ef7420ee780270ca6d993d18d4101049d0, Apache-2.0. Derived upstream from Android PinyinIME.\n'
text+='Wubi 86: rime/rime-wubi commit 152a0d3f3efe40cae216d1e3b338242446848d07, LGPL-3.0. Original YAML dictionary, contributor attribution and full license texts are included unchanged. Corresponding source: https://github.com/rime/rime-wubi/tree/152a0d3f3efe40cae216d1e3b338242446848d07 . The component remains available under its own license.\n'
text+='Wubi 86 and Lingxi Wubi schema adaptations: LGPL-3.0-only; z-prefixed pinyin lookup, Wubi code annotations and mixed input. Mobile schemas derive from the shared RIMES Wubi schemas and use pinyin_simp. Corresponding source: https://github.com/scholay/xsime/tree/main/rime-data .\n'
text+='Default chord example: functional key-to-Pinyin mappings from RIMES chord-keymaps/Isaac2025.json at 20651da, with dvuo → niang added by the publisher on 2026-09-28. The example can be copied, edited or replaced by an imported JSON profile. No proprietary input-method executable, documentation or artwork is included.\n'
text+='Status-light pets: Animated Noto Emoji by Google (https://googlefonts.github.io/noto-emoji-animation/), licensed under CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). Changed for RIMES: the 512 px animated WebP files were downscaled to 96 px and re-encoded as animated PNG, keeping at most 60 frames by merging neighbouring frames. Codepoints: U+1F407, U+1F415, U+1F416, U+1F419, U+1F422, U+1F423, U+1F427, U+1F429, U+1F438, U+1F43C, U+1F980, U+1F98A.\n'
text+='Storefront country-code mappings: Unicode CLDR 49, Unicode License v3. Country-code facts are projected from codeMappings.json and the regular-region validity list. Source: https://github.com/unicode-org/cldr-json/tree/49.0.0 .\n'
text+='The bundled minimal iOS schemas omit desktop Lua and external includes. User-imported Rime schemes may use a statically linked, restricted Lua runtime; imported scheme content retains its own licenses and is not redistributed with the app.\n'
for file in sorted((base/'Licenses').glob('*')):text+='\n--- '+file.name+' ---\n'+'\n'.join(line.rstrip() for line in file.read_text().splitlines()).rstrip()+'\n'
for heading, file in [('RIMES Apache-2.0', 'LICENSE'), ('RIMES NOTICE', 'NOTICE'), ('RIMES license scope', 'LICENSING.md'), ('RIMES historical MIT notice', 'LICENSES/MIT-legacy.txt'), ('RIMES official plugin NOTICE', 'OfficialPlugins/NOTICE')]:
    text+='\n--- '+heading+' ---\n'+(root/file).read_text()
(base/'Resources/THIRD_PARTY.txt').write_text(text)
