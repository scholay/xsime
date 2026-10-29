#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a fresh lock-test directory'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$common=[IO.Path]::GetFullPath("$PSScriptRoot\..\installer\Package.Common.ps1")
. $common
$root=Join-Path $OutputDirectory 'install-root'
$checks=@()
function Check([string]$Name,[bool]$Passed){if(-not $Passed){throw "Failed: $Name"};$script:checks+=$Name;Write-Host "PASS: $Name"}
function Start-LockChild([string]$Body){
    $code='$ErrorActionPreference=''Stop''; . '''+$common.Replace("'","''")+'''; '+$Body
    $info=[Diagnostics.ProcessStartInfo]::new()
    $info.FileName=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $info.Arguments='-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $info.EnvironmentVariables['PSModulePath']=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\Modules'
    return [Diagnostics.Process]::Start($info)
}
function Probe-Lock([string]$Directory){
    $body='try { $gate=Enter-InstallRootLock '''+$Directory.Replace("'","''")+'''; $gate.Dispose(); exit 0 } catch { if($_.Exception.Message -match ''Another installation''){exit 17}; [Console]::Error.WriteLine($_); exit 18 }'
    $child=Start-LockChild $body
    try{
        if(-not $child.WaitForExit(15000)){$child.Kill();throw 'Lock probe timed out'}
        $code=$child.ExitCode
        if($code -eq 18){throw $child.StandardError.ReadToEnd()}
        return $code
    }finally{$child.Dispose()}
}
$originalUser='S-1-5-21-111111111-222222222-333333333-1001'
$held=Enter-InstallRootLock $root $originalUser
try{
    $name=[RimesInstallRootLock]::Name($root)
    Check 'system gate uses a machine-wide namespace' ($name -match '^Global\\RIMES\.InstallRoot\.v1\.[A-F0-9]{64}$')
    $securityHandle=[Threading.Mutex]::OpenExisting($name,[Security.AccessControl.MutexRights]::ReadPermissions)
    try{
        $rules=$securityHandle.GetAccessControl().GetAccessRules($true,$false,[Security.Principal.SecurityIdentifier])
        $originalRules=@($rules | Where-Object {$_.IdentityReference.Value -eq $originalUser -and $_.AccessControlType -eq 'Allow'})
        Check 'an alternate administrator-created gate explicitly admits the initiating user SID' ($originalRules.Count -eq 1 -and ($originalRules[0].MutexRights -band [Security.AccessControl.MutexRights]::FullControl) -eq [Security.AccessControl.MutexRights]::FullControl)
        Check 'user completion access is not granted to every account' (@($rules | Where-Object {$_.IdentityReference.Value -in @('S-1-1-0','S-1-5-11')}).Count -eq 0)
    }finally{$securityHandle.Dispose()}
    $alias=([IO.Path]::GetFullPath($root)+'\.\').ToUpperInvariant()
    Check 'case and dot aliases resolve to the same root gate' ($name -eq [RimesInstallRootLock]::Name($alias))
    Check 'another process cannot enter an active system transaction' ((Probe-Lock $alias) -eq 17)
    $nested=Enter-InstallRootLock $alias
    try{Check 'nested rollback and install calls retain the outer lock' ((Probe-Lock $root) -eq 17)}finally{$nested.Dispose()}
    Check 'nested disposal does not release the outer transaction' ((Probe-Lock $root) -eq 17)
    Check 'independent installation roots do not block each other' ((Probe-Lock ($root+'-other')) -eq 0)
    New-Item -ItemType Directory -Path $root | Out-Null
    Check 'root creation does not change transaction identity' ($name -eq [RimesInstallRootLock]::Name($root))
}finally{$held.Dispose()}
Check 'next process can retry after transaction release' ((Probe-Lock $root) -eq 0)

# Preserve an open handle while a disposable owner exits without releasing it.
# This tests abandoned-lock recovery without touching a product process.
$abandonedRoot=Join-Path $OutputDirectory 'abandoned-root'
$ready=Join-Path $OutputDirectory 'ready'
$body='$gate=Enter-InstallRootLock '''+$abandonedRoot.Replace("'","''")+'''; [IO.File]::WriteAllText('''+$ready.Replace("'","''")+''',''ready''); Start-Sleep -Seconds 60'
$child=Start-LockChild $body
$keeper=$null
try{
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    while(-not (Test-Path -LiteralPath $ready)){
        if($child.HasExited -or [DateTime]::UtcNow -gt $deadline){throw 'Abandoned-lock fixture did not become ready'}
        Start-Sleep -Milliseconds 50
    }
    $keeper=[Threading.Mutex]::OpenExisting([RimesInstallRootLock]::Name($abandonedRoot))
    $child.Kill();$child.WaitForExit()
    $recovered=Enter-InstallRootLock $abandonedRoot
    try{Check 'an abandoned machine transaction can be recovered' ((Probe-Lock $abandonedRoot) -eq 17)}finally{$recovered.Dispose()}
}finally{
    if(-not $child.HasExited){$child.Kill();$child.WaitForExit()}
    $child.Dispose();if($keeper){$keeper.Dispose()}
}

# Execute the real original-user completion script. Only OS mutation boundaries
# are synthetic; another process probes the actual root gate during every stage.
$completionTools=Join-Path $OutputDirectory 'completion-tools'
New-Item -ItemType Directory -Path $completionTools | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot\..\installer\Initialize-User.ps1" -Destination "$completionTools\Initialize-User.ps1"
'{}' | Set-Content -LiteralPath "$completionTools\PACKAGE.json" -Encoding UTF8
$fixtureCommon=@'
. '__COMMON__'
function Assert-Administrator {throw 'User completion must not request elevation'}
function Assert-InstallUser([string]$Root,[string]$Sid) {Test-CompletionLock 'owner-check'}
function Get-RimesInstallation([string]$Root) {
    Test-CompletionLock 'state-read'
    return [pscustomobject]@{active=$global:RimesCompletionLockTools;manifest=[pscustomobject]@{version='fixture'}}
}
function Read-VerifiedPackage([string]$Directory) {return [pscustomobject]@{version='fixture'}}
function Assert-InstalledAppRegistration([string]$Root,[string]$Directory,$Manifest) {Test-CompletionLock 'registration-read'}
function Get-BrokerAutostart {Test-CompletionLock 'startup-read';return 'retained-startup'}
function Read-SettingsShortcut {return $null}
function Invoke-UserDictionaryDeployment([string]$Directory) {Test-CompletionLock 'deployment'}
function Restore-BrokerAutostart($Value) {Test-CompletionLock 'startup-write'}
function Restore-SettingsShortcut($Value) {Test-CompletionLock 'shortcut-recovery'}
function Write-SettingsShortcut([string]$Root,[string]$Directory) {Test-CompletionLock 'shortcut-write'}
'@
$fixtureCommon.Replace('__COMMON__',$common.Replace("'","''")) | Set-Content -LiteralPath "$completionTools\Package.Common.ps1" -Encoding UTF8
@'
param([string]$InstallRoot)
Test-CompletionLock 'verification'
[pscustomobject]@{Verified=$true;Version='fixture';Directory=$global:RimesCompletionLockTools}
'@ | Set-Content -LiteralPath "$completionTools\Verify.ps1" -Encoding UTF8
$global:RimesCompletionLockRoot=Join-Path $OutputDirectory 'completion-root'
$global:RimesCompletionLockTools=$completionTools
$global:RimesCompletionLockPhases=@()
$global:RimesCompletionLockFail=''
$global:RimesCompletionLockSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
function Test-CompletionLock([string]$Phase){
    if([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -ne $global:RimesCompletionLockSid){throw 'User completion changed token'}
    if((Probe-Lock $global:RimesCompletionLockRoot) -ne 17){throw "Root transaction escaped during $Phase"}
    $global:RimesCompletionLockPhases+=$Phase
    if($Phase -eq $global:RimesCompletionLockFail){throw 'Synthetic user deployment failure'}
}
try{
    & "$completionTools\Initialize-User.ps1" -InstallRoot $global:RimesCompletionLockRoot -ExpectedPackageDirectory $completionTools -NoAutostart | Out-Host
    foreach($phase in @('owner-check','state-read','deployment','startup-write','shortcut-write','verification')){
        Check ("original-user completion keeps the root transaction through $phase") ($global:RimesCompletionLockPhases -contains $phase)
    }
    Check 'user completion releases its transaction after verification' ((Probe-Lock $global:RimesCompletionLockRoot) -eq 0)
    $global:RimesCompletionLockFail='deployment'
    $failed=$false
    try{& "$completionTools\Initialize-User.ps1" -InstallRoot $global:RimesCompletionLockRoot -ExpectedPackageDirectory $completionTools -NoAutostart | Out-Host}catch{$failed=$_.Exception.Message -match 'setup for this Windows user is incomplete'}
    Check 'failed user completion retains its recovery behavior' $failed
    Check 'failed user completion releases the root gate for repair' ((Probe-Lock $global:RimesCompletionLockRoot) -eq 0)
}finally{
    Remove-Variable -Scope Global RimesCompletionLockRoot,RimesCompletionLockTools,RimesCompletionLockPhases,RimesCompletionLockFail,RimesCompletionLockSid -ErrorAction SilentlyContinue
}
[pscustomobject]@{status='passed';checks=$checks;productProcessesTouched=$false} | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding UTF8
$global:LASTEXITCODE=0
