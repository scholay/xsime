#requires -Version 5.1
param([string]$InstallRoot="$env:ProgramFiles\RIMES")
. "$PSScriptRoot\Package.Common.ps1"
Assert-Administrator
$installRootLock=Enter-InstallRootLock $InstallRoot
try {
Assert-InstallUser $InstallRoot (Get-InstallUserSid '' $false)
$state=Get-Content -LiteralPath "$InstallRoot\state.json" -Raw | ConvertFrom-Json
Assert-OwnedVersion $InstallRoot $state.active | Out-Null
if(-not (Test-Path -LiteralPath "$InstallRoot\legacy-recovery.json")){throw 'No legacy registration is recorded'}
$recovery=Get-Content -LiteralPath "$InstallRoot\legacy-recovery.json" -Raw | ConvertFrom-Json
if($recovery.PSObject.Properties['userStartupRecorded'] -and -not $recovery.userStartupRecorded){throw 'The original user startup snapshot was not recorded. Use a current Setup.exe to repair instead; no registration was changed.'}
$legacy=@($recovery.entries)
if(-not $legacy.Count){throw 'No legacy registration is recorded'}
$maintenance=Stop-OwnedBroker $state.active
try {
Assert-Unlocked $state.active
foreach($entry in $legacy){if((Get-FileHash -LiteralPath $entry.dll -Algorithm SHA256).Hash -ne $entry.sha256){throw 'Legacy DLL checksum mismatch'}}
$oldAutostart=Get-BrokerAutostart
$oldInstalledApp=Read-InstalledAppRegistration
$oldShortcut=Read-SettingsShortcut
try{
    foreach($arch in @('x86','x64')){Invoke-Registrar $state.active $arch 'unregister'}
    foreach($entry in $legacy){Invoke-LegacyRegistrar $state.active $entry 'register'}
    Restore-BrokerAutostart $recovery.autostart
    Restore-InstalledAppRegistration $null
    Remove-OwnedSettingsShortcut $InstallRoot
    Move-Item -LiteralPath "$InstallRoot\state.json" -Destination "$InstallRoot\legacy-restored-state.json" -Force
}catch{
    foreach($entry in $legacy){try{Invoke-LegacyRegistrar $state.active $entry 'unregister'}catch{Write-Warning $_}}
    $failure=$_
    $recoveryFailures=@()
    foreach($arch in @('x64','x86')){try{Invoke-Registrar $state.active $arch 'register'}catch{$recoveryFailures+=$_.ToString()}}
    try{Restore-BrokerAutostart $oldAutostart}catch{$recoveryFailures+=$_.ToString()}
    try{Restore-InstalledAppRegistration $oldInstalledApp}catch{$recoveryFailures+=$_.ToString()}
    try{Restore-SettingsShortcut $oldShortcut}catch{$recoveryFailures+=$_.ToString()}
    if($recoveryFailures.Count){throw "Legacy rollback failed: $failure. Recovery is incomplete: $($recoveryFailures -join '; ')"}
    throw "Legacy rollback failed; the managed registration, startup and Installed Apps entry were restored. $failure"
}
Write-Output 'Restored the exact previous DLL paths and startup setting. User data retained. Sign out before daily use if any host used the preview.'
} finally {if($maintenance){$maintenance.Dispose()}}

} finally { $installRootLock.Dispose() }
