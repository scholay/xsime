#requires -Version 5.1
[CmdletBinding()]
param([string]$InstallRoot = "$env:ProgramFiles\RIMES",[switch]$NoAutostart,[switch]$AllowPendingRestart,[string]$PackageDirectory=$PSScriptRoot,[switch]$MachineOnly,[string]$UserSid,[string]$UserAutostartSnapshot)
. "$PSScriptRoot\Package.Common.ps1"
Assert-Administrator
$UserSid=Get-InstallUserSid $UserSid ([bool]$MachineOnly)
$installRootLock=Enter-InstallRootLock $InstallRoot $UserSid
try {
Assert-InstallUser $InstallRoot $UserSid
$PackageDirectory=[IO.Path]::GetFullPath($PackageDirectory)
$manifest=Read-VerifiedPackage $PackageDirectory
$InstallRoot=[IO.Path]::GetFullPath($InstallRoot)
$packageHash=(Get-FileHash -LiteralPath "$PackageDirectory\PACKAGE.json" -Algorithm SHA256).Hash.ToLowerInvariant()
$target=Join-Path $InstallRoot ('versions\'+$manifest.version+'-'+$manifest.commit.Substring(0,12)+'-'+$packageHash.Substring(0,12))
$maintenanceGates=@()
try {
$previous=$null
$legacy=@()
$requiresRestart=$false
$oldAutostart=if(-not $MachineOnly){Get-BrokerAutostart}else{$null}
$userStartupRecorded=-not $MachineOnly
if($MachineOnly -and $UserAutostartSnapshot){
    if($UserAutostartSnapshot.Length -gt 8192){throw 'Startup snapshot is too large'}
    $snapshot=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($UserAutostartSnapshot)) | ConvertFrom-Json
    if($snapshot.userSid -ne $UserSid -or ($null -ne $snapshot.autostart -and $snapshot.autostart -isnot [string])){throw 'Startup snapshot does not match the initiating user'}
    $oldAutostart=$snapshot.autostart
    $userStartupRecorded=$true
}
$oldInstalledApp=Read-InstalledAppRegistration
$oldShortcut=if(-not $MachineOnly){Read-SettingsShortcut}else{$null}
if (Test-Path -LiteralPath "$InstallRoot\state.json") {
    try {$previous=Get-Content -LiteralPath "$InstallRoot\state.json" -Raw | ConvertFrom-Json;Assert-OwnedVersion $InstallRoot $previous.active | Out-Null}
    catch {$previous=$null}
    if($previous){$requiresRestart=[bool]$previous.requiresSignOut}
    if($previous){$maintenanceGates+=@(Stop-OwnedBroker $previous.active $UserSid)}
    if ($previous -and $previous.active -eq $target) {
        try {
            $launcher=if(Test-Path -LiteralPath "$target\Uninstall-App.ps1"){$target}elseif($oldInstalledApp){[string]$oldInstalledApp.RIMESUninstallDirectory.value}else{$PSScriptRoot}
            Write-InstalledAppRegistration $InstallRoot $target $manifest $launcher $UserSid
            if(-not $MachineOnly){Write-SettingsShortcut $InstallRoot $target}
            & "$PSScriptRoot\Verify.ps1" -InstallRoot $InstallRoot -MachineOnly:$MachineOnly
        } catch {Restore-InstalledAppRegistration $oldInstalledApp;if(-not $MachineOnly){Restore-SettingsShortcut $oldShortcut};throw}
        return
    }
    if($previous){try{Assert-Unlocked $previous.active}catch{if(-not $AllowPendingRestart){throw};$requiresRestart=$true}}
}
if(-not $previous){
    $legacy=@(Get-LegacyViews $InstallRoot)
    foreach($entry in $legacy){
        $maintenanceGates+=@(Stop-OwnedBroker (Split-Path -Parent (Split-Path -Parent $entry.dll)) $UserSid)
        if($entry.missing){$requiresRestart=$true;continue}
        try{$stream=[IO.File]::Open($entry.dll,'Open','ReadWrite','None');$stream.Dispose()}
        catch{if(-not $AllowPendingRestart){throw 'An existing RIMES DLL could not be opened exclusively and may still be in use. Sign out first or explicitly use -AllowPendingRestart with immutable version directories.'};$requiresRestart=$true}
    }
}
if (Test-Path -LiteralPath $target) {
    Repair-VerifiedPackage $PackageDirectory $target
    foreach($entry in $legacy){
        if($entry.missing -and $entry.dll -eq (Join-Path $target "$($entry.architecture)\RimesTsf.dll")){
            $entry.missing=$false
            $entry.sha256=(Get-FileHash -LiteralPath $entry.dll -Algorithm SHA256).Hash
        }
    }
}
else {
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -Path "$PackageDirectory\*" -Destination $target -Recurse
    Read-VerifiedPackage $target | Out-Null
}
# Dependency and architecture probes precede every registration mutation.
foreach ($arch in @('x64','x86')) { & "$target\$arch\RimesRegistrar.exe" metadata; if($LASTEXITCODE){throw 'Registrar dependency preflight failed'} }
if(-not $MachineOnly){
    & "$target\x64\RimesBroker.exe" --print-paths
    if($LASTEXITCODE){throw 'Broker dependency preflight failed'}
    & "$target\x64\RimesBroker.exe" --deploy-only
    if($LASTEXITCODE){throw 'Dictionary deployment failed before registration; prior installation retained'}
}
$registered=@()
if($legacy.Count){[ordered]@{entries=$legacy;autostart=$oldAutostart;userStartupRecorded=$userStartupRecorded;userSid=$UserSid;recordedAt=(Get-Date).ToString('o')} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$InstallRoot\legacy-recovery.json" -Encoding UTF8}
try {
    if($previous){ foreach($arch in @('x86','x64')) {Invoke-Registrar $previous.active $arch 'unregister'} }
    else{foreach($entry in $legacy){Invoke-LegacyRegistrar $target $entry 'unregister'}}
    foreach($arch in @('x64','x86')) {Invoke-Registrar $target $arch 'register'; $registered+=$arch}
    foreach($arch in @('x64','x86')) {Invoke-Registrar $target $arch 'verify'}
    if(-not $MachineOnly){
        if(-not $NoAutostart){ & "$target\x64\RimesBroker.exe" --install-autostart; if($LASTEXITCODE){throw 'Autostart registration failed'} }
        else{Restore-BrokerAutostart $null}
    }
    $launcher=if(Test-Path -LiteralPath "$target\Uninstall-App.ps1"){$target}elseif($previous -and (Test-Path -LiteralPath "$($previous.active)\Uninstall-App.ps1")){$previous.active}else{$PSScriptRoot}
    Write-InstalledAppRegistration $InstallRoot $target $manifest $launcher $UserSid
    if(-not $MachineOnly){Write-SettingsShortcut $InstallRoot $target}
    $oldPath=if($previous){$previous.active}else{''}
    Write-InstallState $InstallRoot ([ordered]@{active=$target;userSid=$UserSid;previous=$oldPath;version=$manifest.version;commit=$manifest.commit;requiresSignOut=$requiresRestart;legacy=$legacy;previousAutostart=$oldAutostart;installedAt=(Get-Date).ToString('o')})
} catch {
    $failure=$_
    $rollbackFailures=@()
    foreach($arch in $registered){try{Invoke-Registrar $target $arch 'unregister'}catch{$rollbackFailures+=$_.ToString()}}
    if($previous){foreach($arch in @('x64','x86')){try{Invoke-Registrar $previous.active $arch 'register'}catch{$rollbackFailures+=$_.ToString()}}}
    else{foreach($entry in $legacy){try{Invoke-LegacyRegistrar $target $entry 'register'}catch{$rollbackFailures+=$_.ToString()}}}
    try{if(-not $MachineOnly){Restore-BrokerAutostart $oldAutostart}}catch{$rollbackFailures+=$_.ToString()}
    try{Restore-InstalledAppRegistration $oldInstalledApp}catch{$rollbackFailures+=$_.ToString()}
    try{if(-not $MachineOnly){Restore-SettingsShortcut $oldShortcut}}catch{$rollbackFailures+=$_.ToString()}
    if($rollbackFailures.Count){throw "Installation failed: $failure. Recovery is incomplete: $($rollbackFailures -join '; '). Recovery records and all prior DLLs are retained."}
    throw "Installation failed; prior registration and startup setting restored. $failure"
}
if($requiresRestart){Write-Output 'Registered the new immutable version. SIGN-OUT REQUIRED before daily use; running hosts may still use the previous DLL. No old DLL was overwritten.'}
else{Write-Output 'Installed and verified both TSF architectures. Select RIMES with Win+Space. User data was retained.'}
} finally {foreach($gate in $maintenanceGates){if($gate){$gate.Dispose()}}}

} finally { $installRootLock.Dispose() }
