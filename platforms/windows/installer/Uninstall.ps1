#requires -Version 5.1
param([string]$InstallRoot="$env:ProgramFiles\RIMES",[switch]$AllowPendingRestart,[switch]$MachineOnly,[string]$UserSid)
. "$PSScriptRoot\Package.Common.ps1"
Assert-Administrator
$UserSid=Get-InstallUserSid $UserSid ([bool]$MachineOnly)
$installRootLock=Enter-InstallRootLock $InstallRoot $UserSid
try {
Assert-InstallUser $InstallRoot $UserSid
$state=Get-RimesInstallation $InstallRoot -AllowIncomplete
# Use this verified current package for both registrar architectures even if
# the installed x86 directory or DLL has been removed.
try {Read-VerifiedPackage $PSScriptRoot | Out-Null}
catch {throw "The current uninstall tools are incomplete or unverifiable. Run the current Setup.exe to repair the installation, then retry uninstall. No registration was changed. $($_.Exception.Message)"}
$maintenance=Stop-OwnedBroker $state.active $UserSid
try {
$requiresSignOut=($null -eq $state.requiresSignOut -or [bool]$state.requiresSignOut)
$signOutReason=if($null -eq $state.requiresSignOut){'unknown-installation-state'}elseif($state.requiresSignOut){'previous-signout-required'}else{'none'}
# A deleted registered DLL can remain mapped in an application. Only use the
# exact owned registrations resolved above, not an absent unused architecture.
if(@($state.entries | Where-Object {-not (Test-Path -LiteralPath $_.dll -PathType Leaf)}).Count){$requiresSignOut=$true;$signOutReason='missing-registered-dll'}
try{Assert-Unlocked $state.active}catch{if(-not $AllowPendingRestart){throw};$requiresSignOut=$true;$signOutReason='locked-dll'}
$oldAutostart=if(-not $MachineOnly){Get-BrokerAutostart}else{$null}
$oldInstalledApp=Read-InstalledAppRegistration
if(-not $MachineOnly){Assert-OwnedBrokerAutostart $state.active}
if($oldInstalledApp -and (-not $oldInstalledApp.ContainsKey('RIMESInstallRoot') -or $oldInstalledApp.RIMESInstallRoot.value -ne $InstallRoot)){throw 'Installed Apps entry belongs to another managed installation. No registration was changed.'}
$oldShortcut=if(-not $MachineOnly){Read-SettingsShortcut}else{$null}
try {
    foreach($arch in @('x86','x64')){Invoke-RecoveryRegistrar $PSScriptRoot $state.active $arch 'unregister'}
    if(-not $MachineOnly){Remove-OwnedBrokerAutostart $state.active}
    foreach($arch in @('x86','x64')){Invoke-RecoveryRegistrar $PSScriptRoot $state.active $arch 'verify-absent'}
    Restore-InstalledAppRegistration $null
    if(-not $MachineOnly){Remove-OwnedSettingsShortcut $InstallRoot}
    Write-RetainedUninstallState $InstallRoot $state.active ([pscustomobject]@{Uninstalled=$true;RequiresSignOut=$requiresSignOut;SignOutReason=$signOutReason;UserDataRetained=$true})
} catch {
    $failure=$_
    $recoveryFailures=@()
    foreach($arch in @('x64','x86')){try{Invoke-RecoveryRegistrar $PSScriptRoot $state.active $arch 'register'}catch{$recoveryFailures+=$_.ToString()}}
    try{if(-not $MachineOnly){Restore-BrokerAutostart $oldAutostart}}catch{$recoveryFailures+=$_.ToString()}
    try{Restore-InstalledAppRegistration $oldInstalledApp}catch{$recoveryFailures+=$_.ToString()}
    try{if(-not $MachineOnly){Restore-SettingsShortcut $oldShortcut}}catch{$recoveryFailures+=$_.ToString()}
    if($recoveryFailures.Count){throw "Uninstall failed: $failure. Recovery is incomplete: $($recoveryFailures -join '; '). Installed files and user data retained."}
    throw "Uninstall failed; registration, startup and Installed Apps entry restored. Installed files and user data retained. $failure"
}
Write-Host 'Unregistered RIMES. Version files, user dictionaries, settings and credentials retained for recovery. No other input method was changed.'
[pscustomobject]@{Uninstalled=$true;RequiresSignOut=$requiresSignOut;SignOutReason=$signOutReason;UserDataRetained=$true}
} finally {if($maintenance){$maintenance.Dispose()}}

} finally { $installRootLock.Dispose() }
