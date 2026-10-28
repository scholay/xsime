# RIMES Cross-Platform Preview

This preview is a validation and packaging baseline for the platform-neutral
Rime data used by RIMES. It is deliberately **not** a Windows, Linux, or macOS
native input-method application.

The generated ZIP contains a reviewed `SharedSupport` data overlay, a content
manifest, and a notice. It contains no librime binary, native IME shell, UI,
installer, updater, or platform text-delivery implementation.

## What is verified

`scripts/platform-preview/preview.py` uses only the Python standard library and
performs the same checks on Windows, Linux, and macOS:

- `default.yaml` and `default.custom.yaml` expose exactly five core product
  schemas, in the frozen order `rime_ice`, `double_pinyin`,
  `double_pinyin_flypy`, `wubi86`, `english`.
- `my_combo` remains packaged as the optional schema supplied by the disabled-
  by-default Chording extension, but is absent from both fresh-install lists.
- Every core and optional-extension schema file declares the matching
  `schema_id`.
- The package is the reviewed dependency closure of those schemas: hidden
  schemas, dictionaries and import tables, static text tables, Lua components,
  local OpenCC resources, symbol tables, and notices must all resolve.
- Runtime-owned dependencies are explicit. The current data overlay expects a
  compatible librime build with librime-lua and its stock OpenCC
  `opencc/s2t.json` configuration plus dictionaries.
- `my_combo` keeps the single-key identity algebra for `v`, keeps `v` in its
  speller alphabet/initials, disables the inherited `punct` recognizer route,
  and does not activate `v_filter` or the `symbols_v` tables. These are static
  configuration invariants; they do not replace an eventual native runtime
  typing test.
- Package paths are portable and case-collision-free, files are regular files
  rather than symlinks, and every archive member is covered by the manifest.

The reviewed include/exclude boundary is in
`scripts/platform-preview/policy.json`. A new file under `rime-data` fails
verification until it is explicitly classified.

## Publication boundary

The preview package rejects or omits:

- build/output/cache/temp files, logs, backups, lock files, compiled Rime
  tables, and Python bytecode;
- installation state, sync directories, user dictionaries/userdb files, and
  other mutable profile data;
- credential-like filenames and high-confidence private-key/token/API-secret
  content;
- `rime_ai.example.json` and the retired `ai_*`/`ai_box_*` Lua experiment;
- optional dictionaries, spelling variants, and Lua modules that are not in the
  five-core-schema plus optional-Chording preview closure.

`custom_phrase.txt` remains included because it is a checked-in, reviewed
static table referenced by the product configuration. A user's replacement
custom phrases, `custom_phrase_double.txt`, learned English userdb, AI settings,
and other profile state are never collected by this tool.

## License and source boundary

Every included file is assigned exactly once in `policy.json` under
`provenanceGroups`; validation fails if a future payload file lacks a license
and source classification. RIMES-authored packaging/configuration remains MIT,
the Rime Ice-derived data is GPL-3.0-only, the easy-en dictionary and Rime
Wubi 86 data are LGPL-3.0-only, and `lua/search.lua` retains its CC BY-SA 4.0
attribution. The GPL/LGPL texts and source notices are part of the validated
payload, while both platform packages also include `THIRD_PARTY_NOTICES.md`.

## Local commands

From the repository root:

```sh
python3 scripts/platform-preview/preview.py verify --repo-root .
python3 scripts/platform-preview/preview.py stage \
  --repo-root . \
  --output-dir /tmp/rimes-shared-support
python3 scripts/platform-preview/preview.py package \
  --repo-root . \
  --output-dir /tmp/rimes-platform-preview \
  --label local
python3 scripts/platform-preview/preview.py inspect \
  --archive /tmp/rimes-platform-preview/RIMES-platform-preview-data-local.zip
```

On Windows, the same CLI can be invoked with the Python launcher:

```powershell
py -3 scripts/platform-preview/preview.py verify --repo-root .
py -3 scripts/platform-preview/preview.py stage `
  --repo-root . `
  --output-dir platform-preview-stage
py -3 scripts/platform-preview/preview.py package `
  --repo-root . `
  --output-dir platform-preview-out `
  --label windows-local
```

The interface is intentionally platform-script agnostic:

- `verify --repo-root <root>` returns nonzero on schema, dependency, privacy,
  or packaging-policy drift.
- `stage --repo-root <root> --output-dir <empty-dir>` performs the complete
  validation first, then copies exactly `policy.include` into that directory.
  The output directory itself is the `SharedSupport` root: it directly contains
  `default.yaml`, schema files, `lua/`, `cn_dicts/`, and so on. It contains no
  generated manifest or notice that could be installed into Rime by mistake.
- `package --repo-root <root> --output-dir <dir> --label <safe-label>` validates
  first, writes a deterministic-layout ZIP, a sidecar manifest, and a SHA-256
  checksum, then inspects the resulting ZIP.
- `inspect --archive <zip>` validates a built asset without requiring the
  source tree.

Future Windows/Linux preview scripts may consume this interface without being
named or laid out in any particular way. After extraction, the archive's
`SharedSupport` directory is an overlay for a separately supplied compatible
librime runtime; it is not runnable by itself.

## Platform packages

- `platforms/windows/` builds `RIMES-Windows-Data-Preview-*.zip`. Its PowerShell
  installer targets an existing Weasel user directory, verifies every payload
  hash, stops on conflicts by default, and only replaces a conflict after an
  explicit verified backup request.
- `platforms/linux/` builds `RIMES-Linux-Data-Preview-*.tar.gz`. Its shell
  installer targets Fcitx5 Rime or IBus Rime, stops on every existing payload
  collision, and removes only unchanged files recorded in its ownership
  manifest.

Both builders call `preview.py stage`; neither may copy the whole `rime-data`
tree as a fallback. Both leave generated `build/`, userdb, learning state, and
unrelated frontend files untouched. Their package-specific READMEs contain the
install, verify, deploy, and uninstall commands.

## CI behavior

`.github/workflows/platform-preview.yml` runs weekly and by manual dispatch.
It runs the CLI self-tests and the same
stage/package/inspect sequence on
`ubuntu-latest`, `windows-latest`, and `macos-15`, then uploads short-lived,
data-only workflow artifacts with manifests and checksums. Separate native
Windows and Linux jobs also exercise each package's isolated
install/verify/uninstall transaction smoke. These are maintenance evidence,
not required checks for a PR, macOS preview, or formal macOS Release.

The workflow has read-only repository permissions. It never creates a GitHub
Release and never labels these artifacts as complete native applications.

Tags matching `platform-preview-vX.Y.Z` instead invoke
`.github/workflows/platform-preview-release.yml`. That workflow runs the full
Windows and Linux package transaction smokes, rebuilds both platform assets,
and also rebuilds the unchanged macOS target before creating a GitHub
**Pre-release**. The tag deliberately does not match the macOS stable `v*`
release workflow, and a prerelease is excluded from the macOS updater's
`/releases/latest` channel.

Create a platform tag only through an explicit maintenance decision with
`./scripts/release.sh platform X.Y.Z` (or `platform patch|minor|major`); the
script refuses dirty or diverged worktrees and verifies that both `origin` URLs
resolve to the canonical `scholay/xsime` repository before pushing. This
separate preview channel never unblocks or blocks the macOS release path.
