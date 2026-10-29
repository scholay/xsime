#requires -Version 5.1
[CmdletBinding()]
param([ValidateSet('Verify','Install','CompleteUser')][string]$Action='Verify',[switch]$NoAutostart,[switch]$MachineOnly,[string]$UserSid,[string]$UserAutostartSnapshot)
. "$PSScriptRoot\Package.Common.ps1"
$manifest=Read-VerifiedPackage $PSScriptRoot
if($Action -eq 'Verify'){
    [ordered]@{verified=$true;installed=$false;version=$manifest.version;commit=$manifest.commit;signing=$manifest.signing.mode;files=$manifest.files.Count} | ConvertTo-Json -Compress
    return
}
if($Action -eq 'CompleteUser'){
    $completionLock=Enter-InstallRootLock "$env:ProgramFiles\RIMES" (Get-InstallUserSid '' $false)
    try {
    & "$PSScriptRoot\Initialize-User.ps1" -ExpectedPackageDirectory $PSScriptRoot -NoAutostart:$NoAutostart | Out-Host
    $verification=& "$PSScriptRoot\Verify.ps1" -MachineOnly:$MachineOnly
    [ordered]@{verified=$true;installed=$true;version=$verification.Version;requiresSignOut=[bool]$verification.RequiresSignOut;requiresRestart=$false;directory=$verification.Directory} | ConvertTo-Json -Compress
    return
    } finally { $completionLock.Dispose() }
}
Assert-Administrator
$UserSid=Get-InstallUserSid $UserSid ([bool]$MachineOnly)
$installRootLock=Enter-InstallRootLock "$env:ProgramFiles\RIMES" $UserSid
try {
Assert-InstallUser "$env:ProgramFiles\RIMES" $UserSid
# The EXE carries the official installers so a clean Windows machine needs no
# compiler, Python, manually opened shell, or separate runtime download.
$restart=$false
foreach($arch in @('x64','x86')){
    $runtime=Join-Path $PSScriptRoot "runtimes\vc_redist.$arch.exe"
    $signature=Get-AuthenticodeSignature -LiteralPath $runtime
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation(?:,|$)'){
        throw "Visual C++ runtime signature validation failed: $arch"
    }
    $process=Start-Process -FilePath $runtime -ArgumentList @('/install','/quiet','/norestart') -Wait -PassThru
    # 1638 means a newer redistributable is already installed.
    if($process.ExitCode -notin @(0,3010,1638)){throw "Visual C++ runtime installation failed: $arch, exit $($process.ExitCode)"}
    if($process.ExitCode -eq 3010){$restart=$true}
}
& "$PSScriptRoot\Install.ps1" -NoAutostart:$NoAutostart -AllowPendingRestart -MachineOnly:$MachineOnly -UserSid $UserSid -UserAutostartSnapshot $UserAutostartSnapshot | Out-Host
$verification=& "$PSScriptRoot\Verify.ps1" -MachineOnly:$MachineOnly
if(-not $verification.Verified -or $verification.Version -ne $manifest.version){throw 'Installed version verification failed'}
[ordered]@{verified=$true;installed=$true;version=$manifest.version;requiresSignOut=[bool]$verification.RequiresSignOut;requiresRestart=$restart;directory=$verification.Directory} | ConvertTo-Json -Compress

} finally { $installRootLock.Dispose() }
