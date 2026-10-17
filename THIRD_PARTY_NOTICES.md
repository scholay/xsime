# Third-party notices

## RIMES application license

RIMES-maintained application and packaging code is licensed under Apache-2.0,
except for separately identified components and resources. See [LICENSE](LICENSE),
[NOTICE](NOTICE) and [LICENSING.md](LICENSING.md). Earlier MIT grants and the
original copyright notice are retained in [LICENSES/MIT-legacy.txt](LICENSES/MIT-legacy.txt).
These project terms do not replace the component licenses below.

## Rime Input Method Engine (librime)

RIMES Chinese input uses [rime/librime](https://github.com/rime/librime),
licensed under BSD-3-Clause. Copyright (c) 2014, RIME Developers.
The complete original license is retained in
[LICENSES/librime-BSD-3-Clause.txt](LICENSES/librime-BSD-3-Clause.txt).
Use of the engine and attribution do not imply endorsement by the Rime project.

## AudioKit

The Buffer Music audio graph uses
[`AudioKit/AudioKit`](https://github.com/AudioKit/AudioKit), version 5.7.2
(revision `97a35fe04cd8e3d13b8744e1216b8673f825bef6`). Its AudioEngine,
AppleSampler, Mixer, and Reverb supply audio playback and processing. The
electronic instrument is synthesized by RIMES; no AudioKit demo sound banks
are included. The separately licensed AVL acoustic drum recordings are
documented below.

Copyright (c) 2016 Aurelius Prochazka

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## Rime Ice configuration and dictionaries

The Rime schemas, dictionaries, Lua modules, OpenCC data, symbol tables, and
supporting files under `rime-data/` include material derived from
[`iDvel/rime-ice`](https://github.com/iDvel/rime-ice), distributed under the
GNU General Public License version 3 only. The original import is identified by
RIMES commit `d6d9f9cea97da478540b8a5e9a5c102216e2cf1d`; subsequent modifications
are available in this repository's history. The corresponding source is the
plain-text data shipped in this repository and in the preview packages.

The GPL text and a detailed source notice are included as
`rime-data/licenses/GPL-3.0.txt` and
`rime-data/licenses/rime-ice-SOURCE.md`.

The Xiaohe double-pinyin mapping and its mixed Chinese/English static table
are additionally pinned to upstream Rime Ice revision
`c398c0d4526b012cb3b306f792089abed13e0413`. The schema header and Rime Ice
source notice record the RIMES adaptation and exact checksums.

`rime-data/lua/search.lua` retains its embedded attribution to Mirtle and is
licensed separately under
[CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/); its source URL
is recorded both in the file header and the Rime Ice source notice.

## rime-easy-en dictionary

`rime-data/easy_en.dict.yaml` is imported from
[`BlindingDark/rime-easy-en`](https://github.com/BlindingDark/rime-easy-en) at
revision `54a4a07289412efc54134092c0d945f895a71ed3` and is distributed under
the GNU Lesser General Public License version 3. The license and source notice
are included under `rime-data/licenses/`.

## Rime Octagram language model

The Simplified-Chinese language model used by the hidden consciousness-stream
schema and its upstream `grammar.yaml` come from
[`lotem/rime-octagram-data`](https://github.com/lotem/rime-octagram-data),
pinned respectively to master revision
`8ceef1b42eb77e86501382a52e85c309c0f2f04c` and hans revision
`f8ce3b534733e489a8470a7c2adf5a154e8ea069`. They are distributed under
the GNU Lesser General Public License version 3. Exact asset size, checksum,
source boundary, and license location are recorded in
`rime-data/licenses/rime-octagram-data-SOURCE.md`.

## Rime Wubi 86

`rime-data/wubi86.schema.yaml` and `rime-data/wubi86.dict.yaml` are imported
from [`rime/rime-wubi`](https://github.com/rime/rime-wubi) at revision
`152a0d3f3efe40cae216d1e3b338242446848d07` and are distributed under the GNU
Lesser General Public License version 3. The dictionary is verbatim; RIMES
modifies the schema to use the already bundled Rime Ice dictionary for pinyin
reverse lookup and the bundled default punctuation preset. The derived
`lingxi_wubi.schema.yaml` adds simultaneous Wubi/Pinyin lookup. Mobile Wubi
schemas are generated from these sources with the bundled `pinyin_simp`
dictionary; all Wubi schema adaptations retain LGPL-3.0-only. The exact source,
modification boundary, checksum, and license text are included as
`rime-data/licenses/rime-wubi-SOURCE.md` and
`rime-data/licenses/rime-wubi-LICENSE`.

## 呦呦音形 (麓鸣输入法 · 音形分支)

The 呦呦音形 chord schemes (`yoyo-yx` with 折梅 fingering, `yoyo-yx-hm` with
寒梅 fingering), their shared `yoyo.yaml`, dictionaries and `lua/yoyo/`
processors are by Rayalizing, from
[`Rayalizing/yoyo`](https://github.com/Rayalizing/yoyo) at revision
`2f9dcc86f04558282c185b7c712630a311b8513b`. They are macOS chord-extension data
and are excluded from the Linux and Windows preview packages. The upstream
repository declares no license; the file list, checksums, redistribution
status and upstream data provenance are recorded in
`rime-data/licenses/yoyo-SOURCE.md`.

The RIMES application license does not replace the licenses above for
third-party data.

## GRDB.swift

The My Prompt local search index uses
[`groue/GRDB.swift`](https://github.com/groue/GRDB.swift), version 7.11.1.

Copyright (C) 2015-2025 Gwendal Roué

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## rmscene

The narrow reMarkable v6 typed-text reader in RIMES is informed by
[`ricklupton/rmscene`](https://github.com/ricklupton/rmscene), version 0.8.0.

Copyright (c) 2022 Rick Lupton

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## AVL Drumkits — Black Pearl 4pc

The acoustic drum sample library `Black_Pearl_4_LV2.sf2` is by Glen MacArthur
and Robin Gareus, from x42/avldrums.lv2 commit
`9389f16f139410d9a55f9006976bd54cbd8b7806`. It is bundled unmodified, separately
from RIMES code, under CC BY-SA 3.0 with the author's exception for music and
other non-sample-library works. The full upstream attribution/exception is in
`Music/AVL-README.txt`, shipped with the sample asset.

Source: https://github.com/x42/avldrums.lv2/tree/9389f16f139410d9a55f9006976bd54cbd8b7806/sf2

License: https://creativecommons.org/licenses/by-sa/3.0/

The x42 LV2 player/FluidSynth code is not included. AudioKit's AppleSampler
loads the licensed sample library. RIMES drum patterns are original; no
BeatBuddy recordings, MIDI content, or firmware are redistributed.

## Portable Rime scheme import

- macOS and iOS: [Yams 5.1.3](https://github.com/jpsim/Yams/tree/5.1.3) and [ZIPFoundation 0.9.19](https://github.com/weichsel/ZIPFoundation/tree/0.9.19), MIT. Full notices are in `LICENSES/Yams-MIT.txt` and `LICENSES/ZIPFoundation-MIT.txt`; iOS also embeds them in its third-party notices.
- Windows and Linux: [yaml-cpp 0.8.0](https://github.com/jbeder/yaml-cpp/tree/0.8.0), [miniz 3.1.2](https://github.com/richgel999/miniz/tree/3.1.2), and [PicoSHA2](https://github.com/okdshin/PicoSHA2/tree/161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29), MIT. Exact source hashes are pinned in `Shared/SchemeImport/CMakeLists.txt`; full notices are in `LICENSES/`.
- Android: [SnakeYAML 2.4](https://bitbucket.org/snakeyaml/snakeyaml), Apache-2.0. Its upstream license is bundled under `assets/licenses/`.

## Linux clipboard data-control protocols

The Capsule clipboard backend uses unmodified ext-data-control-v1 (wayland-protocols 1.45) and wlr-data-control-unstable-v1 (swaywm/wlr-protocols b010a03648b88d143236de193bddbfea0c08bc84) XML descriptions. Their permissive licenses are retained in the XML and `LICENSES/Wayland-data-control.txt`; exact hashes and source locations are in `platforms/linux/capsule/protocols/README.md`. Generated bindings link against the system Wayland client library. Clipboard storage links the system SQLite library (public domain).

The Linux Capsule resource preview helper links the distribution's Poppler GLib PDF renderer, GdkPixbuf and Cairo. Poppler's source and API documentation are at <https://poppler.freedesktop.org/>; the Ubuntu 24.04 package's copyright notice identifies the core license as GPL-2 or GPL-3. The distribution packages provide their license notices and corresponding source. These libraries are dynamically resolved by the deb dependencies; their binaries are not vendored in this repository.

## Linux screen capture and media preview

The Linux capture and preview helpers dynamically use the distribution's GStreamer libraries and plugins (including PNG, VP8/WebM and PipeWire) and libportal for interactive screenshots. GStreamer source and component licenses are published at <https://gstreamer.freedesktop.org/>; libportal source and its LGPL-3.0 / LGPL-2.1 notices are at <https://github.com/flatpak/libportal>. The deb declares system package dependencies, including the runtime plugins that are loaded dynamically. No GStreamer, libportal or PipeWire binary is vendored in this repository. The test-only virtual-pointer XML retains its upstream MIT license and pinned provenance under `platforms/linux/capsule/protocols/README.md`.
