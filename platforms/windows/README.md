# XSIME Windows Data / Input-Schemes Preview

原生犀术IME开发与日用安装请从 [TSF/Broker 开发说明](native/README.md) 和 [EXE 安装器说明](installer/README.md) 进入。下文仅介绍较早的 Weasel 词库预览通道。

This is an **experimental data preview for the official Weasel input method**. It packages XSIME's current Rime schemas, dictionaries, OpenCC data, and Lua modules so Windows users can try the input schemes with Weasel.

It is deliberately **not advertised as a native Windows build of XSIME**. The package does not contain a TSF input-method DLL, XSIME Buffer, the workbench, AI/translation/OCR features, settings UI, or any XSIME executable. All key handling, composition, candidate UI, and text delivery come from Weasel.

## Requirements

- Windows 10 or Windows 11.
- A current [official Weasel](https://github.com/rime/weasel) installation.
- Windows PowerShell 5.1 or PowerShell 7.

Weasel documents its default user directory as `%APPDATA%\Rime` and requires a redeploy after configuration changes. The scripts also honor Weasel's custom `HKCU\Software\Rime\Weasel\RimeUserDir` value. The upstream deploy command used here is `WeaselDeployer.exe /deploy`.

## Install an extracted release ZIP

Open PowerShell in any directory. The scripts resolve their package files relative to their own location, so the current working directory does not matter.

```powershell
& "C:\path\to\RIMES-Windows-Data-Preview\Verify-RimesDataPreview.ps1"
& "C:\path\to\RIMES-Windows-Data-Preview\Install-RimesDataPreview.ps1" -PlanOnly
& "C:\path\to\RIMES-Windows-Data-Preview\Install-RimesDataPreview.ps1"
```

The default install is fail-closed: if a payload filename already exists with different content, no destination file is changed. Review the reported list first. To explicitly back up and replace only those conflicts:

```powershell
& "C:\path\to\RIMES-Windows-Data-Preview\Install-RimesDataPreview.ps1" -BackupConflicts
```

Backups and the ownership manifest are stored outside the Rime user directory under `%LOCALAPPDATA%\RIMES\DataPreview`. Files that already existed with identical content remain user-owned and are never removed by uninstall.

If automatic discovery of Weasel fails, pass the official executable explicitly:

```powershell
& .\Install-RimesDataPreview.ps1 -WeaselDeployerPath "C:\Program Files\Rime\weasel-0.17.4\WeaselDeployer.exe"
```

## Verify or uninstall

```powershell
& .\Verify-RimesDataPreview.ps1 -Installed
& .\Uninstall-RimesDataPreview.ps1
```

Uninstall removes only files recorded as created by this package and restores verified backups for files explicitly replaced with `-BackupConflicts`. If an owned file changed after installation, uninstall stops before making changes. After review, `-ForceRestore` explicitly discards those changes and restores the pre-install state.

## User data safety

- The release payload permits only static `.yaml`, `.txt`, `.lua`, `.json`, `.md`, and license files.
- `*.userdb*`, `build/`, `sync/`, `installation.yaml`, `user.yaml`, compiled `.bin` files, logs, locks, symlinks, and reparse points are rejected during packaging and again during installation.
- The scripts never copy, open, merge, or delete an active Rime user database. Existing Weasel learning data remains where it is and is opened only by Weasel.
- Package hashes are checked against `payload-manifest.json` before any destination write. Copies and backups are hashed again during file transactions.
- A failed install rolls back files from verified backups. A failed uninstall restores its pre-uninstall transaction snapshot.
- Install, installed-state verification, and uninstall hold exclusive crash-cleaned locks for both the state root and Rime destination. Do not edit managed schema files, run Weasel Deploy, or start another configuration transaction while install/uninstall is running: these locks serialize XSIME scripts, not arbitrary external writers.

As with any input-method configuration preview, back up important custom schemas independently before opting into `-BackupConflicts`.

## Custom destination and CI transaction tests

`-Destination` bypasses Weasel-directory discovery. `-StateRoot` keeps test state isolated. These options allow Windows CI to exercise install/verify/uninstall transactions without a Weasel installation:

```powershell
$sandbox = Join-Path $env:TEMP ([guid]::NewGuid())
& .\Install-RimesDataPreview.ps1 `
  -Destination (Join-Path $sandbox 'Rime') `
  -StateRoot (Join-Path $sandbox 'State') `
  -SkipDeploy
& .\Verify-RimesDataPreview.ps1 -Installed `
  -Destination (Join-Path $sandbox 'Rime') `
  -StateRoot (Join-Path $sandbox 'State')
& .\Uninstall-RimesDataPreview.ps1 `
  -Destination (Join-Path $sandbox 'Rime') `
  -StateRoot (Join-Path $sandbox 'State') `
  -SkipDeploy
```

`-SkipPlatformCheck` exists only so the same isolated file logic can run under PowerShell 7 on Linux/macOS CI. It does not make Weasel available on those systems.

## Build the offline ZIP

From a repository checkout, Python 3 plus PowerShell 5.1 or 7 can run the repository's reviewed platform-preview policy, stage its exact 57-file dependency closure, and create a self-contained ZIP without network access:

```powershell
pwsh -File platforms/windows/scripts/New-RimesDataPreviewPackage.ps1 `
  -Version 0.1.0-preview.1
```

The command writes the ZIP and a sibling `.sha256` file under `platforms/windows/dist/` by default. Use `-OutputDirectory` for CI artifacts and `-KeepStaging` to retain the verified staging tree.

## Known limitations

- No Windows machine has yet validated typing behavior, Lua plugin availability, deployment time, application compatibility, or the isaac2026 chord timing in this Weasel data preview. This must therefore be published as an untested preview, not a stable cross-platform XSIME release.
- XSIME's macOS buffer/workbench behavior is not reproduced by Weasel.
- The package supplies the `my_combo` Rime chord schema, including literal `v`
  behavior. XSIME's cross-batch mutual-pairing logic lives in the macOS Swift
  frontend and is not included in this data preview.
- Weasel owns candidate rendering and application compatibility; visual behavior will differ from macOS.
- This package does not install or update Weasel and does not change its user-directory registry setting.

Upstream references: [Weasel README and user directory](https://github.com/rime/weasel#%E5%AE%9A%E8%A3%BD%E8%BC%B8%E5%85%A5%E6%B3%95), [Weasel customization guide](https://github.com/rime/weasel/wiki/Weasel-%E5%AE%9A%E5%88%B6%E5%8C%96), and [`WeaselDeployer.exe /deploy` source](https://github.com/rime/weasel/blob/master/WeaselDeployer/WeaselDeployer.cpp).

The packaging scripts are covered by the repository's Apache-2.0 license, subject to [the licensing scope](../../LICENSING.md) and retained historical MIT grants. Future packages must include `LICENSE`, `NOTICE` and applicable retained licenses. Bundled data retains its original notices, including files under `rime-data/licenses/`; see `THIRD_PARTY_NOTICES.md` as well.
