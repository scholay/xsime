# Input scheme packages

This directory is the source of truth for the Chinese input schemes RIMES can offer for
import: where each one comes from, under what license, and whether the package built from it
passes the importer. The official website's scheme list is meant to be generated from
[`index.json`](index.json).

| File | Role | Edited by |
|---|---|---|
| [`sources.json`](sources.json) | Curated upstream sources, pinned to a commit | hand, or `scheme_build.py pin` |
| [`index.json`](index.json) | One record per package: status, checksum, schemes, license | `scheme_build.py index` |
| [`CATALOG.md`](CATALOG.md) | The same records as a table, by category | `scheme_build.py index` |
| `build/input-schemes/packages/*.zip` | The packages themselves (not committed) | `scheme_build.py build` |

The tooling lives in [`scripts/input-schemes/`](../../scripts/input-schemes).

## How RIMES imports a scheme

The five frontends now expose the same **original ZIP / coded UTF-8 TXT or TSV** import contract.
See [the portable import guide](../../docs/portable-scheme-import.md) for current limits,
runtime requirements, and validation boundaries.

| Platform | Import path | Activation |
|---|---|---|
| iOS | Settings › Rime 方案包与导入 | Review, deploy, enable; reopen the keyboard |
| macOS | Settings › 输入法 › 导入与管理方案 | Deploy, enable, restart the IME while retaining learning data |
| Android | App settings › Rime 方案包与导入 | Deploy in a separate process, enable, reopen the keyboard |
| Windows | Settings › 输入法 › 导入与管理方案 | Deploy, enable, save Buffer and restart RIMES |
| Linux (RIMES Fcitx5 addon) | Fcitx5 input-method configuration or the 方案管理 desktop entry (`rimes-schemes`) | Deploy, enable, restart Fcitx5 |

The ZIP may have an outer repository directory. Standard `.schema.yaml`, source
`.dict.yaml`, `.custom.yaml`, nested dictionaries, Lua modules and OpenCC resources stay
in their original relative locations. Do not import another device's `build/`, `sync/`
or user database. The source archive is not rewritten. Deployment happens in isolation;
only a successfully compiled and loaded package can be enabled. Removal retains learning
records. The old Linux **data preview** installer is separate from the Fcitx5 addon.

A source package must include its required dependencies. Current mobile engines do not
include octagram/predict; ZIPs containing `.gram` models are diagnosed instead of silently
dropping the model and calling the complete package supported. Keyboard layouts and
frontend-specific Lua/native APIs are not portable merely because the scheme uses Rime.

## Package format, version 1

An optional `rimes-package.yaml` with `rimes_package.format: 1` supplies recommendations
and provenance. Original upstream ZIPs without a manifest are accepted too. A recommended
schema may depend on auxiliary schemas, but those dependencies do not become user-visible
choices automatically. The catalog builder still records every upstream component, license
and modification in its manifest.

Current product import limits and unsupported formats are maintained in the
[portable scheme import guide](../../docs/portable-scheme-import.md). File paths must also be representable on Windows; links,
traversal, case/Unicode-normalization collisions and reserved device names are rejected.

## Earlier catalog builds

`index.json` and its `verifiedWithImporter` values are **historical evidence for the
recorded importer/runtime**, not a declaration that every record has passed on all five
frontends. Keep those hashes and results until the packages are actually reverified.

The older builder/linter targets shipped iOS importers with tighter limits and applies
compatibility transformations: placeholder optional references, flattened dictionaries,
large-table splitting and Lua shims. It remains available for those old clients. New
clients accept standard optional/local/`.yaml` references, reciprocal schema dependencies,
dictionary subdirectories, `*module*member`, `module/init.lua`, and ordinary Lua comments
and quoted examples directly. Prefer an original complete upstream ZIP when validating the
new importer; do not use a repaired catalog package as proof that the original is accepted.
Malformed YAML, missing required sources, unsupported native components and exceeded Lua
budgets still need a clear diagnostic. A successful compile alone is not input acceptance.

## Catalog

`sources.json` lists one entry per package: `id`, names, `category`/`family`, the upstream
`repo` with a pinned `revision`, an optional `root` directory, and optionally the `schemas`
to recommend (default: every scheme in the root). `providers` are the shared sources that
complete a package: prelude, essay, the stock Rime schemes that others depend on, OpenCC
data, and 雾凇 for the files derived configurations borrow from it.

An entry may use `sourceArchive` (`url`, `sha256`, optional `licenseFile`) when a
formal release contains generated resources absent from the Git source archive.
The builder verifies the release checksum; `licenseFile` is fetched from the pinned
source commit if the ZIP omits it. `pin` leaves these entries fixed: update the
revision, release URL and checksum together. The manifest retains the archive source.

Wanxiang uses the v18.1.1 Base release and the explicit `wanxiang-portable-v18.1.1`
compatibility profile. It repairs the Lua cache database names, includes the release's
OpenCC data, and disables unavailable octagram, precompiled charset filtering and
desktop layout-switch/redeploy commands. It is a fixed full-pinyin package. See
[`wanxiang-portable.txt`](wanxiang-portable.txt) for the user-facing import instructions and limits.

Categories: `phonetic` 音码 (全拼, 双拼, 注音, 拼式, 并击), `shape` 形码 (五笔, 仓颉, 郑码,
笔画, 行列, 宇浩, 虎码, …), `phonetic-shape` 音形码 (小鹤音形, 自然码, 键道, 声笔, 魔然, 墨奇,
…), `dialect` 方言与古汉语, `auxiliary`.

`redistribution` decides whether the website may host the ZIP:

| Value | Meaning | Website |
|---|---|---|
| `open` | Upstream declares a license that permits redistribution with changes | May host the package |
| `restricted` | License forbids commercial use or derivatives (CC BY-NC-SA, CC BY-NC-ND) | Link to upstream; decide case by case |
| `undeclared` | Upstream declares no license | Link to upstream; ask the author before hosting |
| `proprietary` | Commercial code table | Link only |

It defaults from the license GitHub detects; set it explicitly in `sources.json` when the
LICENSE file says something GitHub cannot classify. A package is `publishable` in
`index.json` only when it is `open`, `verified`, and not graded `high` for device risk.

Package `status`:

| Status | Checked by | Meaning |
|---|---|---|
| `verified` | importer + engine | The real importer accepts and stages it; the iOS engine build deploys it, loads every recommended scheme without a Lua error, and any typing probes match |
| `importer-ok` | importer | As above, without the engine step (engine host slice not built) |
| `lint-ok` | `scheme_lint.py` | Passes the restated contract only |
| `import-fails` | importer or engine | Built, but rejected at inspect, stage, deploy, load or a probe; see `statusReason` |
| `partial`, `blocked` | lint | Some or all recommended schemes cannot be selected |
| `rejected` | lint | The importer would refuse the archive |
| `failed` | build | Could not be assembled from the source |

## Workflow

```bash
python3 -m venv .venv && .venv/bin/pip install -r scripts/input-schemes/requirements.txt
```

```bash
.venv/bin/python scripts/input-schemes/scheme_build.py build
```

```bash
.venv/bin/python scripts/input-schemes/scheme_build.py verify
```

```bash
.venv/bin/python scripts/input-schemes/scheme_build.py index
```

- `build` fetches each pinned revision into `build/input-schemes/cache`, assembles the
  package and writes `packages/*.zip` and `reports/<id>.json`. `--only id …` limits the run.
- `verify` compiles the unmodified `RimeSchemeImportService.swift` into a host command and
  runs `inspect` and `stage` on every package; then deploys and loads the staged result with
  [`host_probe.cc`](../../scripts/input-schemes/host_probe.cc), which repeats what
  `RimeMobile.deployResources` and `validateResources` do. It needs Swift, and for the engine
  step the host slice from `platforms/ios/scripts/build-engine.py host`. `--importer-ref HEAD`
  takes the importer from a commit instead of the working tree. A rebuild keeps the
  verification of a package whose content did not change; `verify --missing` checks only the
  rest. One engine step may take `--timeout` seconds (default 600).
- Packages that convert between simplified and traditional characters take their OpenCC
  data from `Vendor/rime/SharedSupport/opencc`, which `scripts/fetch-rime.sh` provides.
- `index` writes `index.json` and `CATALOG.md`.
- `scheme_lint.py <zip> …` checks any ZIP, including one a user sends in, against the contract.
- `pin --only id …` moves entries to the current upstream head. Rebuild and verify after.

To add a scheme, append an entry to `sources.json` (run `pin` to fill in the revision),
build and verify it, and commit `sources.json`, `index.json` and `CATALOG.md`. Add `probes`
(`schema`, `keys`, `expect`) when you know a code that must produce a given candidate.
Probes may set an `options` list (for example `["s2tw"]`) before typing. Both matching
output and a clean Lua runtime are required; a candidate produced alongside a Lua
error is a failed probe.

To publish, run `index --collect <dir>`: it copies the `publishable` ZIPs and a `SHA256SUMS`
file into `<dir>`. Upload those as release assets and let the website read `index.json`:
`archive`, `bytes` and `sha256` identify the download, `schemes` lists what the user will see
in the import review, `engine` carries the measured deployment cost. `build/input-schemes/packages`
also holds packages that are not publishable; do not upload that directory as a whole.

## What verification does and does not show

`verified` means the importer's own code accepted the package and the same engine build the
iOS app links (librime with the restricted Lua runtime, host slice) compiled and loaded it
on a Mac. It does not show behaviour on a phone: deployment there is slower and memory is
tighter. `engine.deploySeconds` and `engine.peakMemoryMB` are the Mac figures for compiling
the dictionaries, and `engine.deviceRisk` grades them against the one package known to deploy
on an iPhone 15 Pro (13 s and 1.2 GB on an M4 in the recorded benchmark):
`medium` from 10 s or 1 GB, `high` from 60 s or 2 GB. The grades are a guide to what to try
on a device first, not a measurement. `high` packages are not `publishable`. Chord schemes load but need
key-release events the iOS keyboard does not send to imported schemes. Typing probes exist
for a small set of well-known schemes; for the rest, “loads” is the evidence.

## Licenses

Every package carries the license text of each component and names the component of every
file in `rimes-package.yaml`. Modified upstream files are listed there with the change, as
GPL and LGPL sources require. The packages are data under their upstream licenses; the
Apache-2.0 license of RIMES covers only the generated manifest, placeholders and shims.
Whether a code table itself may be licensed by the repository that publishes it (五笔, 郑码,
小鹤音形, 虎码, 嘸蝦米 and other schemes have rights holders) is not something this catalog can
settle: `open` records what upstream declares.
