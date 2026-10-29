#requires -Version 5.1
[CmdletBinding()]
param([string]$InstallRoot="$env:ProgramFiles\RIMES",[Parameter(Mandatory)][string]$ExpectedPackageDirectory,[switch]$NoAutostart)
. "$PSScriptRoot\Package.Common.ps1"
$UserSid=Get-InstallUserSid '' $false
$installRootLock=Enter-InstallRootLock $InstallRoot $UserSid
try {
Assert-InstallUser $InstallRoot $UserSid
$state=Get-RimesInstallation $InstallRoot
Read-VerifiedPackage $ExpectedPackageDirectory | Out-Null
if((Get-FileHash -LiteralPath "$ExpectedPackageDirectory\PACKAGE.json" -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath "$($state.active)\PACKAGE.json" -Algorithm SHA256).Hash){
    throw 'The installed package changed before user setup. Run the intended Setup.exe again.'
}
Assert-InstalledAppRegistration $InstallRoot $state.active $state.manifest
$oldAutostart=Get-BrokerAutostart
$oldShortcut=Read-SettingsShortcut
try {
    # Never deploy dictionaries or start a user-data-aware Broker as the UAC
    # administrator. These operations inherit the original user's token.
    # Deployment acquires the serving mutex before checking other sessions.
    # Setup.exe retains its unowned reservation across both install phases.
    Invoke-UserDictionaryDeployment $state.active
    if($NoAutostart){Restore-BrokerAutostart $null}
    else{
        & "$($state.active)\x64\RimesBroker.exe" --install-autostart | Out-Host
        if($LASTEXITCODE){throw 'Autostart registration failed'}
    }
    Write-SettingsShortcut $InstallRoot $state.active
    & "$PSScriptRoot\Verify.ps1" -InstallRoot $InstallRoot | Out-Host
} catch {
    $failure=$_
    $recoveryFailures=@()
    try{Restore-BrokerAutostart $oldAutostart}catch{$recoveryFailures+=$_.ToString()}
    try{Restore-SettingsShortcut $oldShortcut}catch{$recoveryFailures+=$_.ToString()}
    throw "RIMES system files were installed, but setup for this Windows user is incomplete. Run Setup.exe again from this account to retry. Dictionaries were retained. $failure Recovery errors: $($recoveryFailures -join '; ')"
}
} finally { $installRootLock.Dispose() }
