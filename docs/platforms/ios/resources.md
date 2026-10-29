# iOS 资源清单与发行依据

本页集中说明随包资源、来源和发行依据；依赖版本与校验和以锁文件为准。授权声明保留其来源和时间，构建检查不代替资源许可。

- [iOS resource and dependency inventory](#inventory)
- [Distribution evidence — 2026-09-22](#distribution-basis)

---

<a id="inventory"></a>

## iOS resource and dependency inventory

Exact commits and download SHA-256 values are in the [dependency lock](../../../platforms/ios/dependencies.lock.json).
Full license texts are in [`platforms/ios/Licenses/`](../../../platforms/ios/Licenses/)
and are displayed by the app through [`THIRD_PARTY.txt`](../../../platforms/ios/Resources/THIRD_PARTY.txt).

| Shipped component | Source/version | Changes for iOS | License and distribution record |
|---|---|---|---|
| librime | [rime/librime](https://github.com/rime/librime/tree/33e78140250125871856cdc5b42ddc6a5fcd3cd4), 1.17.0 | Static build, logging/external plugins/timestamps disabled; source unmodified | BSD-3-Clause; retain copyright, conditions and disclaimer in source and binary notices |
| LevelDB | [google/leveldb](https://github.com/google/leveldb/tree/99b3c03b3284f5886f9ef9a4ef703d57373e61be) | Static build; optional compression/allocator integrations disabled | BSD-3-Clause; included notice |
| marisa-trie | [s-yata/marisa-trie](https://github.com/s-yata/marisa-trie/tree/3e87d53b78e15f2f43783d5e376561a8c9722051) | Static build, source unmodified | Upstream BSD-2-Clause option selected; included copyright, conditions and disclaimer |
| yaml-cpp | [jbeder/yaml-cpp](https://github.com/jbeder/yaml-cpp/tree/2f86d13775d119edbb69af52e5f566fd65c6953b), 0.8.0 | Static build without tests/tools/contrib | MIT; retain copyright and permission notice |
| OpenCC | [BYVoid/OpenCC](https://github.com/BYVoid/OpenCC/tree/556ed22496d650bd0b13b6c163be9814637970ae), 1.1.9 | Library only on iOS; Darts disabled; no conversion dictionaries or CLI shipped | Apache-2.0; preserve license and applicable notices; no source patches |
| OpenCC bundled marisa | `deps/marisa-0.2.6` at the pinned OpenCC revision | Source unmodified | Upstream BSD-2-Clause option selected; separate included notice |
| OpenCC RapidJSON headers | `deps/rapidjson-1.1.0` at the pinned OpenCC revision | Source unmodified | Upstream MIT/Tencent license and third-party statements retained in the included license file |
| Boost headers | [Boost 1.86.0](https://archives.boost.io/release/1.86.0/source/) | Headers used by librime, no Boost runtime downloaded at launch | Boost Software License 1.0; included license |
| Simplified Pinyin dictionary | [rime-pinyin-simp](https://github.com/rime/rime-pinyin-simp/tree/0c6861ef7420ee780270ca6d993d18d4101049d0) | Dictionary source unchanged; compiled into table/prism assets; `associations.tsv` (word continuations, top 8 per word) derived from the same unchanged source by `platforms/ios/scripts/build-associations.py` | Apache-2.0; original source metadata and license retained |
| Wubi 86 dictionary | [rime-wubi](https://github.com/rime/rime-wubi/tree/152a0d3f3efe40cae216d1e3b338242446848d07) | Dictionary bytes match pinned source; compiled into table/prism assets | LGPL-3.0; full license texts, unchanged corresponding source, attribution and source URL bundled. See the [distribution basis](#distribution-basis). |
| FlyYao map | Repository `chord-keymaps/Isaac2025.json` at `20651da` | 427 mappings: the 426 from `20651da` plus `dvuo → niang`, added to the source by the publisher on 2026-09-28; iOS ID/name and default full-Pinyin metadata applied | Publisher-declared functional mapping example; public name Default chord. Source provenance retained. This is not an upstream license grant or an independently verified patent clearance; see the [distribution basis](#distribution-basis). |
| Status-light pets | [Animated Noto Emoji](https://googlefonts.github.io/noto-emoji-animation/) (Google), 12 animals: 1f407, 1f415, 1f416, 1f419, 1f422, 1f423, 1f427, 1f429, 1f438, 1f43c, 1f980, 1f98a | 512 px animated WebP downscaled to 96 px animated PNG, at most 60 frames (neighbours merged); keyboard target only, `platforms/ios/Resources/Pets/` | CC BY 4.0 per the project FAQ; attribution, license link and changes stated in `platforms/ios/Resources/THIRD_PARTY.txt` and in the keyboard's settings panel |
| RIMES rhino mascot | Owner's Scholay Rhino pixel animations (PixelLab), local library `01-design/Rhino` (V4 character 943577fb: idle-breathe-blink-s, walk south-east, run-se-v3, celebrate-s-v2; sitting 9b12afdc: sit-to-stand-s; object bc06f594: waiting-for-input, playful jump, sighing) | Each clip cropped to the character's union box, scaled to one standing height and bottom-aligned in a 96 px cell, re-encoded as animated PNG (`platforms/ios/Resources/Pets/rhino-*.png`); keyboard target only | Owner-provided brand character, added at the owner's request (2026-09-29) |
| iOS schemas and Ziranma algebra | Project-owned mobile schemas and shared encoding | Prepared by `platforms/ios/scripts/prepare-data.py`; dictionary compilation and restricted Lua support are verified before packaging | Repository Apache-2.0; preserve applicable historical MIT grants as described in `LICENSING.md` |
| App icon | Generated from the owner's existing Scholay Rhino 2D reference assets, at their request | Flat black monoline rhino and backwards cap on white; 1024-square opaque PNG | Owner-provided character reference; generated with built-in image_gen. Model version was not exposed by the tool |

XcodeGen 2.46.0 is a checksum-pinned build tool and is not shipped in the app.
CMake, Git, Python and the Apple toolchain are host tools. OpenCC's test frameworks,
benchmarks, bindings and command-line parsers are not included in the iOS library.

`platforms/ios/scripts/prepare-data.py` records checksums for the generated engine assets.
`platforms/ios/scripts/verify.py` checks dictionary sources, mapping parity, generated hashes,
schema allowlist and the release audit. Permission changes require evidence in
[`distribution-audit.json`](../../../platforms/ios/distribution-audit.json); neither this inventory nor a successful build is a
TestFlight redistribution clearance.

---

<a id="distribution-basis"></a>

## Distribution evidence — 2026-09-22

The input method itself and its particular packaged implementation/data are distinct.
The app is free; noncommercial distribution is not used as an exemption from licenses.

| Component | Distribution basis and evidence |
|---|---|
| Wubi 86 dictionary | rime/rime-wubi revision 152a0d3f3efe40cae216d1e3b338242446848d07, LGPL-3.0. Full license texts, original attribution and unchanged corresponding YAML source are bundled. Notices provide the pinned source URL. Apple Standard EULA preserves permissions provided by open-source component licenses. A separate permission request is not treated as automatically necessary. |
| Default chord example (legacy ID builtin.flyyao) | 427 functional key-to-Pinyin mappings from Isaac2025.json: the 426 at 20651da plus dvuo → niang, added by the publisher on 2026-09-28. Publisher explicitly represented on 2026-09-22 that these functional mappings/input methods are not copyrighted content and requested distribution. No proprietary executable, documentation or artwork is included in this resource. This is publisher-provided provenance/rights context, not a newly obtained upstream license or an independent worldwide patent/copyright clearance. |
| Status-light pets (Animated Noto Emoji) | 12 animated animal emoji by Google, CC BY 4.0 (stated in the project's FAQ, checked 2026-09-29). Commercial use in apps is permitted with attribution. Attribution, license URL and the changes made (downscaled to 96 px, re-encoded as animated PNG, frames merged) are in `platforms/ios/Resources/THIRD_PARTY.txt` and the keyboard settings panel. |
| RIMES rhino mascot | The owner's own Scholay Rhino character animations, generated with PixelLab and supplied from the owner's local asset library at their request; cropped and resized for the status light. No third-party artwork. |
| librime | BSD-3-Clause; pinned source and notices retained. |
| pinyin_simp | Apache-2.0; pinned source, unchanged dictionary and notices retained. |

The repository Apache-2.0 license does not replace component licenses or historical MIT grants (see `LICENSING.md`). Internal
provenance remains intact while the public example name is Default chord.
Import/export and custom profiles remain supported.

App Store Connect Content Rights was saved as containing third-party content with
necessary usage rights, on the above license and publisher-representation basis.
It was not declared to contain no third-party content.

Sources:
- https://github.com/rime/rime-wubi/tree/152a0d3f3efe40cae216d1e3b338242446848d07
- https://www.apple.com/legal/internet-services/itunes/dev/stdeula/
- https://www.copyright.gov/help/faq/faq-protect.html (US distinction between methods/systems and protected expression; not worldwide legal clearance)

The publisher declaration remains separately identified in
[`distribution-audit.json`](../../../platforms/ios/distribution-audit.json).
Accepting that documented distribution basis does not claim independent clearance;
account classification and distribution requirements are separate.
