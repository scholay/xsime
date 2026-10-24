#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$env:PSModulePath=[IO.Path]::Combine($env:WINDIR,'System32\WindowsPowerShell\v1.0\Modules')
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a fresh isolated output directory'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
. "$PSScriptRoot\..\installer\Package.Common.ps1"
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$session=[Diagnostics.Process]::GetCurrentProcess().SessionId
$name='Local\RIMES.MaintenanceTests-'+[guid]::NewGuid().ToString('N')
$directory=[IO.Path]::GetFullPath((Join-Path $OutputDirectory 'owned'))
$checks=@()
function Check([string]$Name,[bool]$Passed){if(-not $Passed){throw "Failed: $Name"};$script:checks+=$Name;Write-Host "PASS: $Name"}
$canonical=Get-BrokerMutexName $sid $session
Check 'reservation follows the native versioned endpoint identity' ($canonical -match ('^Local\\RIMES\.Broker\.v2\.session-'+$session+'\.user-[a-f0-9]{16}$'))
# Every process boundary below is synthetic; no daily process is stopped.
function Get-BrokerMutexName([string]$UserSid,[int]$Session){return $script:name+'.session-'+$Session}
function Get-Process {[CmdletBinding()]param([string]$Name);return $script:processes}
function Get-CimInstance {[CmdletBinding()]param([string]$ClassName,[string]$Filter);return $script:natives[[int]($Filter.Split('=')[1])]}
function Invoke-CimMethod {[CmdletBinding()]param($InputObject,[string]$MethodName);return [pscustomobject]@{ReturnValue=$script:ownerStatus;Sid=$InputObject.owner}}
function Stop-Process {[CmdletBinding()]param($InputObject,[switch]$Force);$script:stopped+=$InputObject.Id;$InputObject.HasExited=$true}
function Invoke-Registrar([string]$Directory,[string]$Architecture,[string]$Operation){$script:registrationMutations+=$Operation}
function Fixture([int]$Id,[string]$Path,[string]$Owner){
    $now=[DateTime]::Now
    $process=[pscustomobject]@{Id=$Id;SessionId=$script:session;StartTime=$now;HasExited=$false}
    $process | Add-Member ScriptMethod Refresh {}
    $process | Add-Member ScriptMethod WaitForExit {
        param($milliseconds)
        if($script:graceful -and $script:stopEvent -and $script:stopEvent.WaitOne(0)){$this.HasExited=$true}
        return $this.HasExited
    }
    $script:natives[$Id]=[pscustomobject]@{ExecutablePath=$Path;CreationDate=$now;owner=$Owner;CommandLine=('"'+$Path+'"')}
    return $process
}
$natives=@{};$stopped=@();$ownerStatus=0;$graceful=$false;$stopEvent=$null
$processes=@(Fixture 12345 (Join-Path $directory 'x64\RimesBroker.exe') $sid)
$original=New-BrokerMaintenanceReservation $sid
try {
    $lease=Stop-OwnedBroker $directory $sid
    $original.Dispose();$original=$null
    try {
        Check 'owned legacy Broker stops without a Buffer confirmation' ($stopped -contains 12345)
        $created=$false;$attempt=[Threading.Mutex]::new($false,(Get-BrokerMutexName $sid $session),[ref]$created)
        try{Check 'the reservation survives stopping the original Broker' (-not $created)}finally{$attempt.Dispose()}
    }finally{$lease.Dispose()}
}finally{if($original){$original.Dispose()}}
$created=$false;$after=[Threading.Mutex]::new($false,(Get-BrokerMutexName $sid $session),[ref]$created)
try{Check 'maintenance releases its reservation at completion' $created}finally{$after.Dispose()}
$stopped=@();$natives=@{}
$processes=@(Fixture 12346 (Join-Path $directory 'x64\RimesBroker.exe') 'S-1-5-21-1-2-3-1001';Fixture 12347 'C:\foreign\RimesBroker.exe' $sid)
$lease=Stop-OwnedBroker $directory $sid
try{Check 'another account and another executable remain untouched' ($stopped.Count -eq 0)}finally{$lease.Dispose()}
$natives=@{};$processes=@(Fixture 12348 (Join-Path $directory 'x64\RimesBroker.exe') $sid)
$natives[12348].ExecutablePath=$null
$failed=$false;try{$lease=Stop-OwnedBroker $directory $sid;if($lease){$lease.Dispose()}}catch{$failed=$true}
Check 'unreadable executable is never killed on its name alone' ($failed -and $stopped.Count -eq 0)
$natives[12348].ExecutablePath=Join-Path $directory 'x64\RimesBroker.exe'
$ownerStatus=5;$failed=$false
try{$lease=Stop-OwnedBroker $directory $sid;if($lease){$lease.Dispose()}}catch{$failed=$true}
Check 'unknown owner is preserved' ($failed -and $stopped.Count -eq 0)
$ownerStatus=0;$natives[12348].CreationDate=$natives[12348].CreationDate.AddSeconds(-1);$failed=$false
try{$lease=Stop-OwnedBroker $directory $sid;if($lease){$lease.Dispose()}}catch{$failed=$true}
Check 'reused process identity is rejected' ($failed -and $stopped.Count -eq 0)
$natives[12348].CreationDate=$processes[0].StartTime
$natives[12348].CommandLine+=' --deploy-only';$failed=$false
try{$lease=Stop-OwnedBroker $directory $sid;if($lease){$lease.Dispose()}}catch{$failed=$true}
Check 'an active dictionary deployment is not terminated as a disposable UI' ($failed -and $stopped.Count -eq 0)
$natives=@{};$processes=@(Fixture 12349 (Join-Path $directory 'x64\RimesBroker.exe') $sid)
$graceful=$true
$stopEvent=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,((Get-BrokerMutexName $sid $session)+'.shutdown-12349'))
try {
    $lease=Stop-OwnedBroker $directory $sid
    try{Check 'new Broker uses graceful engine shutdown without a forced stop' ($stopEvent.WaitOne(0) -and $stopped.Count -eq 0 -and $processes[0].HasExited)}finally{$lease.Dispose()}
}finally{$stopEvent.Dispose();$stopEvent=$null}
$natives=@{};$processes=@(Fixture 12350 (Join-Path $directory 'x64\RimesBroker.exe') $sid)
$graceful=$false;$registrationMutations=@();$failed=$false;$failure='';$lease=$null
$stopEvent=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,((Get-BrokerMutexName $sid $session)+'.shutdown-12350'))
try {
    try {
        $lease=Stop-OwnedBroker $directory $sid
        # Installer/rollback/uninstall registration work follows this gate.
        # These are synthetic mutations and must never be reached on failure.
        Invoke-Registrar $directory 'x64' 'unregister'
        Invoke-Registrar $directory 'x86' 'unregister'
    } catch {$failed=$true;$failure=$_.Exception.Message}
    finally {if($lease){$lease.Dispose()}}
    Check 'slow graceful shutdown aborts maintenance and preserves the live Broker' ($failed -and $failure -match 'safe shutdown' -and $stopEvent.WaitOne(0) -and -not $processes[0].HasExited -and $stopped.Count -eq 0)
    Check 'slow graceful shutdown prevents subsequent registration mutations' ($registrationMutations.Count -eq 0)
    $created=$false;$afterFailure=[Threading.Mutex]::new($false,(Get-BrokerMutexName $sid $session),[ref]$created)
    try{Check 'failed graceful maintenance releases only its own reservation' $created}finally{$afterFailure.Dispose()}
}finally{$stopEvent.Dispose();$stopEvent=$null}
Check 'alternate administrator never opens original-user kernel objects' ($null -eq (New-BrokerMaintenanceReservation 'S-1-5-21-1-2-3-1001'))
[pscustomobject]@{status='passed';checks=$checks;realProcessesStopped=$false} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding UTF8
$global:LASTEXITCODE=0
