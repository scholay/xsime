#requires -Version 5.1
param([string]$InstallRoot="$env:ProgramFiles\RIMES",[switch]$MachineOnly,[string]$UserSid)
. "$PSScriptRoot\Package.Common.ps1"
Assert-Administrator
$UserSid=Get-InstallUserSid $UserSid ([bool]$MachineOnly)
$installRootLock=Enter-InstallRootLock $InstallRoot $UserSid
try {
Assert-InstallUser $InstallRoot $UserSid
$state=Get-Content -LiteralPath "$InstallRoot\state.json" -Raw | ConvertFrom-Json
if(-not $state.previous -and $MachineOnly){throw 'Cross-account legacy rollback requires the original user startup snapshot; no registration was changed. Use a current Setup.exe to repair instead.'}
if(-not $state.previous){ & "$PSScriptRoot\Restore-Legacy.ps1" -InstallRoot $InstallRoot; return }
Assert-OwnedVersion $InstallRoot $state.previous | Out-Null
& "$PSScriptRoot\Install.ps1" -InstallRoot $InstallRoot -PackageDirectory $state.previous -MachineOnly:$MachineOnly -UserSid $UserSid

} finally { $installRootLock.Dispose() }
