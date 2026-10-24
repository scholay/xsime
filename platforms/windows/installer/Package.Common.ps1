#requires -Version 5.1
Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
function Get-ProductDisplayName {
    $language = [Globalization.CultureInfo]::CurrentUICulture.Name
    if ($language -match '^zh-(Hant|TW|HK|MO)(-|$)') { return '犀術IME' }
    if ($language -match '^zh($|-)') { return '犀术IME' }
    return 'XSIME'
}

function Read-VerifiedPackage([string]$Directory,[switch]$AllowMissingFiles,[string]$ManifestSourceDirectory) {
    $root = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
    $manifestDirectory=if($ManifestSourceDirectory){[IO.Path]::GetFullPath($ManifestSourceDirectory)}else{$root}
    $manifestItem=Get-Item -LiteralPath "$manifestDirectory\PACKAGE.json" -Force -ErrorAction SilentlyContinue
    if($manifestItem -and ($manifestItem.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Reparse points are not accepted'}
    $manifest = Get-Content -LiteralPath "$manifestDirectory\PACKAGE.json" -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.formatVersion -ne 1 -or $manifest.product -ne 'RIMES' -or $manifest.protocol -ne 2 -or $manifest.commit -notmatch '^[a-f0-9]{40}$' -or $manifest.version -notmatch '^[a-zA-Z0-9._-]{1,64}$') { throw 'Unsupported package manifest' }
    $seen = @{}
    foreach ($file in $manifest.files) {
        if ($file.path -match '(^[/\\]|:|(^|[/\\])\.\.([/\\]|$))') { throw 'Invalid package path' }
        $path = [IO.Path]::GetFullPath((Join-Path $root $file.path))
        if (-not $path.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase) -or $seen.ContainsKey($path)) { throw 'Invalid or duplicate package path' }
        $seen[$path] = $true
        $cursor = $path
        while ($cursor.Length -ge $root.Length) {
            # Test-Path can report false for dangling links. Read the link's
            # own metadata so repair never follows it as a missing file.
            $cursorItem=Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
            if ($cursorItem -and ($cursorItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Reparse points are not accepted' }
            $cursor = Split-Path -Parent $cursor
        }
        $item=Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        if ($AllowMissingFiles -and -not $item) { continue }
        if (-not $item) { throw "Package file is missing: $($file.path)" }
        if ($item.PSIsContainer -or $item.Length -ne $file.bytes -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) { throw "Package checksum failed: $($file.path)" }
    }
    foreach ($item in Get-ChildItem -LiteralPath $root -Recurse -File) {
        if ($item.FullName -ne "$root\PACKAGE.json" -and -not $seen.ContainsKey($item.FullName)) { throw "Unlisted package file: $($item.Name)" }
    }
    return $manifest
}
function Repair-VerifiedPackage([string]$Source,[string]$Target){
    Read-VerifiedPackage $Source | Out-Null
    if(-not (Test-Path -LiteralPath "$Target\PACKAGE.json")){
        # The target name was derived from this exact manifest. Validate every
        # surviving byte and reject reparse/unlisted files before restoring it.
        Read-VerifiedPackage $Target -AllowMissingFiles -ManifestSourceDirectory $Source | Out-Null
        Copy-Item -LiteralPath "$Source\PACKAGE.json" -Destination "$Target\PACKAGE.json"
    }
    Read-VerifiedPackage $Target -AllowMissingFiles | Out-Null
    if((Get-FileHash -LiteralPath "$Source\PACKAGE.json" -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath "$Target\PACKAGE.json" -Algorithm SHA256).Hash){throw 'Repair package does not match the retained version manifest'}
    $manifest=Get-Content -LiteralPath "$Source\PACKAGE.json" -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach($file in $manifest.files){
        $destination=Join-Path $Target $file.path
        if(-not (Test-Path -LiteralPath $destination)){
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $Source $file.path) -Destination $destination
        }
    }
    Read-VerifiedPackage $Target | Out-Null
}
function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this script from an elevated 64-bit PowerShell window.' }
    if (-not [Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitProcess) { throw 'Windows x64 and 64-bit PowerShell are required.' }
}
# A credential-prompt elevation may change the process identity. The elevated
# phase records the initiating SID, but never opens that user's profile or HKCU.
function Get-InstallUserSid([string]$UserSid,[bool]$MachineOnly) {
    if($MachineOnly -and -not $UserSid){throw 'Machine-only operations require the initiating user SID'}
    $current=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    if(-not $UserSid){return $current}
    $sid=[Security.Principal.SecurityIdentifier]::new($UserSid)
    if($sid.Value -ne $UserSid){throw 'Invalid initiating user SID'}
    if(-not $MachineOnly -and $UserSid -ne $current){throw 'User configuration must run as the initiating Windows account'}
    return $sid.Value
}
function Assert-InstallUser([string]$InstallRoot,[string]$UserSid) {
    $entry=Read-InstalledAppRegistration
    if($entry -and $entry.ContainsKey('RIMESUserSid') -and $entry.RIMESUserSid.value -ne $UserSid){
        throw 'Sign in to the Windows account that installed RIMES before changing this installation.'
    }
    # Keep the owner check when Installed Apps registration needs repair.
    if(Test-Path -LiteralPath "$InstallRoot\state.json"){
        $state=$null
        try {$state=Get-Content -LiteralPath "$InstallRoot\state.json" -Raw | ConvertFrom-Json}catch{}
        if($state -and $state.PSObject.Properties['userSid'] -and $state.userSid -ne $UserSid){
            throw 'The installation state belongs to another Windows account.'
        }
    }
}
function Invoke-Registrar([string]$Directory,[string]$Architecture,[string]$Operation) {
    $registrar = Join-Path $Directory "$Architecture\RimesRegistrar.exe"
    # Native diagnostics are not PowerShell result objects. Keep them visible
    # without contaminating the one structured result consumed by launchers.
    & $registrar $Operation --dll (Join-Path $Directory "$Architecture\RimesTsf.dll") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Registrar $Operation $Architecture failed: $LASTEXITCODE" }
}
function Assert-Unlocked([string]$Directory) {
    foreach ($arch in @('x64','x86')) {
        $path = Join-Path $Directory "$arch\RimesTsf.dll"
        if (-not (Test-Path -LiteralPath $path)) { continue }
        try { $stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None); $stream.Dispose() }
        catch { throw "A RIMES input method DLL could not be opened exclusively and may still be in use. Switch input methods, close applications using RIMES or sign out, then retry. No files were overwritten: $path" }
    }
}
function Get-BrokerMutexName([string]$UserSid,[int]$Session) {
    if(-not ('RimesMaintenanceNames' -as [type])){
        Add-Type -TypeDefinition @'
public static class RimesMaintenanceNames {
    public static string Mutex(string sid, int session) {
        ulong hash=14695981039346656037UL;
        unchecked { foreach(char unit in sid) { hash^=unit; hash*=1099511628211UL; } }
        return "Local\\RIMES.Broker.v2.session-"+session+".user-"+hash.ToString("x16");
    }
}
'@
    }
    return [RimesMaintenanceNames]::Mutex(([Security.Principal.SecurityIdentifier]::new($UserSid)).Value,$Session)
}
function New-BrokerMaintenanceReservation([string]$UserSid=([Security.Principal.WindowsIdentity]::GetCurrent().User.Value)) {
    # A different UAC administrator does not open original-user kernel objects.
    # Setup.exe / Invoke-UserUninstall retains that user's reservation instead.
    if($UserSid -ne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value){return $null}
    $name=Get-BrokerMutexName $UserSid ([Diagnostics.Process]::GetCurrentProcess().SessionId)
    $security=[Security.AccessControl.MutexSecurity]::new()
    $security.SetAccessRuleProtection($true,$false)
    $security.AddAccessRule([Security.AccessControl.MutexAccessRule]::new([Security.Principal.SecurityIdentifier]::new($UserSid),'FullControl','Allow'))
    $created=$false
    # Retain even an existing object's handle before stopping its Broker. Its
    # existence blocks TSF auto-start; ownership remains available to deploy.
    return [Threading.Mutex]::new($false,$name,[ref]$created,$security)
}
function Stop-OwnedBroker([string]$Directory,[string]$UserSid=([Security.Principal.WindowsIdentity]::GetCurrent().User.Value)) {
    # Callers have already validated the installed path/manifest. Recovery also
    # supports absent files, so do not require a complete package again here.
    $expected=[IO.Path]::GetFullPath((Join-Path $Directory 'x64\RimesBroker.exe'))
    $reservation=New-BrokerMaintenanceReservation $UserSid
    try {
        foreach($process in Get-Process -Name RimesBroker -ErrorAction SilentlyContinue){
            $native=Get-CimInstance Win32_Process -Filter ('ProcessId='+$process.Id)
            if(-not $native){continue}
            if(-not $native.ExecutablePath){if($process.WaitForExit(500)){continue};throw 'Cannot verify a Broker executable. Run maintenance with administrator approval.'}
            if([IO.Path]::GetFullPath($native.ExecutablePath) -ne $expected){continue}
            $owner=Invoke-CimMethod -InputObject $native -MethodName GetOwnerSid
            if($owner.ReturnValue -ne 0 -or -not $owner.Sid){throw 'Cannot verify the Broker owner. No process was stopped.'}
            if($owner.Sid -ne $UserSid){continue}
            if($native.CommandLine -match '(?i)(^|\s)--(deploy-only|once|print-|install-autostart|remove-autostart|user-data-dir|shared-data-dir|rime-dll|log-dir)'){
                throw 'Another Rime deployment or diagnostic operation is using this executable. Wait for it to finish, then retry.'
            }
            # Bind to the process object and creation time, not a reusable PID.
            $process.Refresh()
            if($process.HasExited){continue}
            if([Math]::Abs(($process.StartTime.ToUniversalTime()-$native.CreationDate.ToUniversalTime()).TotalMilliseconds) -gt 1){throw 'Broker identity changed during maintenance. Retry.'}
            $stopEvent=$null
            try {
                $name=(Get-BrokerMutexName $UserSid $process.SessionId)+'.shutdown-'+$process.Id
                try {$stopEvent=[Threading.EventWaitHandle]::OpenExisting($name,[Security.AccessControl.EventWaitHandleRights]::Modify)}catch [Threading.WaitHandleCannotBeOpenedException]{}catch [UnauthorizedAccessException]{throw 'Cannot request the owned Broker graceful shutdown. No process was terminated; retry maintenance with the required access.'}
                if($stopEvent){
                    $stopEvent.Set() | Out-Null
                    if($process.WaitForExit(5000)){continue}
                    # A current Broker may be finishing an uncancellable Rime
                    # dictionary write. Abort maintenance without forcing it
                    # down; the lifetime gate remains owned until it exits.
                    throw 'The Broker is still completing a safe shutdown. Maintenance was cancelled without terminating it. Wait for it to exit, then retry.'
                }
                # Legacy Brokers have no graceful maintenance event. Terminate
                # only the verified owned executable, never its TSF host app.
                Stop-Process -InputObject $process -Force -ErrorAction Stop
                if(-not $process.WaitForExit(5000)){throw 'The owned Broker could not be stopped'}
            } finally {if($stopEvent){$stopEvent.Dispose()}}
        }
        return $reservation
    } catch {if($reservation){$reservation.Dispose()};throw}
}
function Write-InstallState([string]$Root,$State) {
    New-Item -ItemType Directory -Path $Root -Force | Out-Null
    $temporary = Join-Path $Root ('state.'+[guid]::NewGuid().ToString('N')+'.tmp')
    $State | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination (Join-Path $Root 'state.json') -Force
}
function Assert-OwnedVersion([string]$Root,[string]$Directory) {
    $versions=[IO.Path]::GetFullPath((Join-Path $Root 'versions')).TrimEnd('\')+'\'
    if (-not [IO.Path]::GetFullPath($Directory).StartsWith($versions,[StringComparison]::OrdinalIgnoreCase)) {throw 'Installed state points outside the managed versions directory'}
    return Read-VerifiedPackage $Directory
}
# Registry is the authority for recovery. Never guess the active version by
# directory ordering: an older retained directory can be the one still loaded.
function Get-RegisteredRimesViews {
    $entries=@()
    foreach($arch in @('x64','x86')) {
        $view=if($arch -eq 'x64'){[Microsoft.Win32.RegistryView]::Registry64}else{[Microsoft.Win32.RegistryView]::Registry32}
        $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
        $key=$null
        try {
            $key=$base.OpenSubKey('SOFTWARE\Classes\CLSID\{0B2C570B-9811-45DF-989B-EA306281F6B4}\InprocServer32')
            if($key){
                $path=[string]$key.GetValue('')
                if(-not [IO.Path]::IsPathRooted($path) -or [IO.Path]::GetFileName($path) -ne 'RimesTsf.dll' -or $key.GetValue('ThreadingModel') -ne 'Apartment'){throw 'RIMES registration ownership is incomplete. Preserve it and repair from a current RIMES package.'}
                $entries += [pscustomobject]@{architecture=$arch;dll=[IO.Path]::GetFullPath($path)}
            }
        } finally {if($key){$key.Dispose()};$base.Dispose()}
    }
    return $entries
}
function Assert-OwnedRegisteredPath([string]$Root,$Entry,[switch]$AllowMissingManifest) {
    $directory=Split-Path -Parent (Split-Path -Parent $Entry.dll)
    $versions=[IO.Path]::GetFullPath((Join-Path $Root 'versions')).TrimEnd('\')
    if((Split-Path -Parent $directory) -ne $versions -or $Entry.dll -ne (Join-Path $directory "$($Entry.architecture)\RimesTsf.dll")){throw 'Registered RIMES DLL points outside this managed installation. No registration was changed.'}
    $cursor=$directory
    while($cursor){
        $cursorItem=Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if($cursorItem -and ($cursorItem.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Reparse points are not accepted for installation recovery'}
        $cursor=Split-Path -Parent $cursor
    }
    if(-not (Test-Path -LiteralPath "$directory\PACKAGE.json") -and $AllowMissingManifest){
        # The actual complete RIMES CLSID registration still identifies this
        # exact immutable directory after an external uninstaller removed it.
        if([IO.Path]::GetFileName($directory) -notmatch '^[a-zA-Z0-9._-]{1,64}-[a-f0-9]{12}-[a-f0-9]{12}$'){throw 'The missing installation has no identifiable managed version identity'}
        return $directory
    }
    $manifest=Read-VerifiedPackage $directory -AllowMissingFiles
    foreach($arch in @('x64','x86')){
        if(-not ($manifest.files | Where-Object {$_.path -eq "$arch/RimesTsf.dll"}) -or -not ($manifest.files | Where-Object {$_.path -eq "$arch/RimesRegistrar.exe"})){throw 'Package manifest does not identify both RIMES architectures'}
    }
    return $directory
}
function Get-RimesInstallation([string]$Root,[switch]$AllowIncomplete) {
    $Root=[IO.Path]::GetFullPath($Root)
    $state=$null
    if(Test-Path -LiteralPath "$Root\state.json"){
        try {$state=Get-Content -LiteralPath "$Root\state.json" -Raw | ConvertFrom-Json}catch{}
    }
    $entries=@(Get-RegisteredRimesViews)
    $directories=@($entries | ForEach-Object {Assert-OwnedRegisteredPath $Root $_ -AllowMissingManifest:$AllowIncomplete} | Select-Object -Unique)
    if($directories.Count -gt 1){throw 'RIMES registry views point to different versions. Repair with a current Setup package before uninstalling.'}
    $active=if($directories.Count){$directories[0]}elseif($state -and $state.PSObject.Properties['active']){[string]$state.active}else{''}
    if(-not $active){
        $app=Read-InstalledAppRegistration
        if($app -and $app.ContainsKey('RIMESInstallRoot') -and $app.RIMESInstallRoot.value -eq $Root -and $app.ContainsKey('InstallLocation')){$active=[string]$app.InstallLocation.value}
    }
    if(-not $active){throw 'Cannot identify an owned RIMES installation. Run the current Setup.exe to repair it, then use Windows Installed Apps to uninstall. No registration was changed.'}
    try {
        # This validates containment and every present hash; missing bytes are
        # accepted only for unregistering the exact paths read above.
        $probe=[pscustomobject]@{architecture='x64';dll=(Join-Path $active 'x64\RimesTsf.dll')}
        $missingManifest=$AllowIncomplete -and $entries.Count -gt 0 -and -not (Test-Path -LiteralPath "$active\PACKAGE.json")
        Assert-OwnedRegisteredPath $Root $probe -AllowMissingManifest:$missingManifest | Out-Null
        $manifest=if($missingManifest){$null}elseif($AllowIncomplete){Read-VerifiedPackage $active -AllowMissingFiles}else{Assert-OwnedVersion $Root $active}
    } catch {throw "RIMES installation is incomplete or unverifiable. Run the current Setup.exe to repair it, then retry uninstall. No registration was changed. $($_.Exception.Message)"}
    $knownState=$state -and $state.PSObject.Properties['active'] -and $state.active -eq $active
    $requiresSignOut=if($knownState -and $state.PSObject.Properties['requiresSignOut']){[bool]$state.requiresSignOut}else{$null}
    return [pscustomobject]@{active=$active;manifest=$manifest;requiresSignOut=$requiresSignOut;recovered=(-not $knownState);entries=$entries}
}
function Get-LegacyViews([string]$InstallRoot='') {
    $entries=@(Get-RegisteredRimesViews)
    foreach($entry in $entries){
        $missing=-not (Test-Path -LiteralPath $entry.dll -PathType Leaf)
        if($missing){
            if(-not $InstallRoot){throw 'Existing RIMES registration points to a missing DLL. Run a current Setup package with the original install directory to repair it.'}
            Assert-OwnedRegisteredPath $InstallRoot $entry -AllowMissingManifest | Out-Null
        }
        $entry | Add-Member -NotePropertyName missing -NotePropertyValue $missing
        $entry | Add-Member -NotePropertyName sha256 -NotePropertyValue $(if($missing){$null}else{(Get-FileHash -LiteralPath $entry.dll -Algorithm SHA256).Hash})
    }
    return $entries
}
function Invoke-LegacyRegistrar([string]$Package,$Entry,[string]$Operation){
    if($Entry.missing){
        if($Operation -eq 'register'){throw 'The original DLL was missing; a dangling registration cannot be restored. Recovery record retained.'}
        if(Test-Path -LiteralPath $Entry.dll){throw 'The missing DLL path changed during recovery'}
    } elseif((Get-FileHash -LiteralPath $Entry.dll -Algorithm SHA256).Hash -ne $Entry.sha256){throw 'Previous DLL changed since its recovery record was written'}
    & (Join-Path $Package "$($Entry.architecture)\RimesRegistrar.exe") $Operation --dll $Entry.dll | Out-Host
    if($LASTEXITCODE){throw "Legacy registration $Operation failed"}
}
function Invoke-RecoveryRegistrar([string]$Package,[string]$Directory,[string]$Architecture,[string]$Operation){
    & (Join-Path $Package "$Architecture\RimesRegistrar.exe") $Operation --dll (Join-Path $Directory "$Architecture\RimesTsf.dll") | Out-Host
    if($LASTEXITCODE){throw "Registrar $Operation $Architecture failed: $LASTEXITCODE"}
}
function Assert-OwnedBrokerAutostart([string]$Directory){
    $current=Get-BrokerAutostart
    if($null -eq $current){return}
    $expected='"'+(Join-Path $Directory 'x64\RimesBroker.exe')+'"'
    if($current -ne $expected){throw 'The RimesBroker startup command belongs to another version. Preserve it and repair the installation.'}
    return
}
function Remove-OwnedBrokerAutostart([string]$Directory){
    Assert-OwnedBrokerAutostart $Directory
    Restore-BrokerAutostart $null
}
function Get-BrokerAutostart {
    $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
    try { if($key){return $key.GetValue('RimesBroker',$null)} } finally {if($key){$key.Dispose()}}
}
function Restore-BrokerAutostart($Value) {
    $key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
    try {if($null -eq $Value){$key.DeleteValue('RimesBroker',$false)}else{$key.SetValue('RimesBroker',[string]$Value)}} finally {$key.Dispose()}
}
function Get-InstalledAppKey {return 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\RIMES'}
function Read-InstalledAppRegistration {
    $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    $key=$null
    try {
        $key=$base.OpenSubKey((Get-InstalledAppKey))
        if(-not $key){return $null}
        $values=@{}
        foreach($name in $key.GetValueNames()){$values[$name]=[pscustomobject]@{value=$key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames);kind=$key.GetValueKind($name)}}
        return $values
    } finally {if($key){$key.Dispose()};$base.Dispose()}
}
function Restore-InstalledAppRegistration($Values) {
    $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    $key=$null
    try {
        $path=Get-InstalledAppKey
        $base.DeleteSubKeyTree($path,$false)
        if($null -ne $Values){
            $key=$base.CreateSubKey($path)
            foreach($name in $Values.Keys){$key.SetValue($name,$Values[$name].value,$Values[$name].kind)}
        }
    } finally {if($key){$key.Dispose()};$base.Dispose()}
}
function Get-UninstallCommand([string]$InstallRoot,[string]$LauncherDirectory) {
    $powershell=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    return '"'+$powershell+'" -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "'+(Join-Path $LauncherDirectory 'Uninstall-App.ps1')+'" -InstallRoot "'+$InstallRoot+'"'
}
function Write-InstalledAppRegistration([string]$InstallRoot,[string]$Directory,$Manifest,[string]$LauncherDirectory=$Directory,[string]$UserSid=([Security.Principal.WindowsIdentity]::GetCurrent().User.Value)) {
    Assert-OwnedVersion $InstallRoot $LauncherDirectory | Out-Null
    if(-not (Test-Path -LiteralPath (Join-Path $LauncherDirectory 'Uninstall-App.ps1') -PathType Leaf)){throw 'The managed uninstall launcher is missing'}
    $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    $key=$null
    try {
        $key=$base.CreateSubKey((Get-InstalledAppKey))
        $key.SetValue('DisplayName',(Get-ProductDisplayName))
        $key.SetValue('DisplayVersion',[string]$Manifest.version)
        $key.SetValue('Publisher','Scholay')
        $key.SetValue('InstallLocation',$Directory)
        $key.SetValue('DisplayIcon',(Join-Path $Directory 'x64\RimesBroker.exe')+',0')
        $key.SetValue('UninstallString',(Get-UninstallCommand $InstallRoot $LauncherDirectory))
        $key.SetValue('RIMESInstallRoot',$InstallRoot)
        $key.SetValue('RIMESUninstallDirectory',$LauncherDirectory)
        $key.SetValue('RIMESUserSid',$UserSid)
        $key.SetValue('URLInfoAbout','https://xsime.com')
        $key.SetValue('InstallDate',(Get-Date -Format 'yyyyMMdd'))
        $key.SetValue('NoModify',1,[Microsoft.Win32.RegistryValueKind]::DWord)
        $key.SetValue('NoRepair',1,[Microsoft.Win32.RegistryValueKind]::DWord)
        $bytes=(Get-ChildItem -LiteralPath $Directory -Recurse -File | Measure-Object -Property Length -Sum).Sum
        $key.SetValue('EstimatedSize',[int][Math]::Min([int]::MaxValue,[Math]::Ceiling($bytes/1024)),[Microsoft.Win32.RegistryValueKind]::DWord)
    } finally {if($key){$key.Dispose()};$base.Dispose()}
    Assert-InstalledAppRegistration $InstallRoot $Directory $Manifest
}
function Assert-InstalledAppRegistration([string]$InstallRoot,[string]$Directory,$Manifest) {
    $values=Read-InstalledAppRegistration
    if($null -eq $values){throw 'RIMES is missing from Windows Installed Apps'}
    foreach($name in @('DisplayName','DisplayVersion','InstallLocation','RIMESInstallRoot','RIMESUninstallDirectory','UninstallString')){if(-not $values.ContainsKey($name)){throw "Installed Apps entry is incomplete: $name"}}
    if($values.DisplayName.value -notin @('RIMES','XSIME','犀术IME','犀術IME') -or $values.DisplayVersion.value -ne $Manifest.version -or $values.InstallLocation.value -ne $Directory -or $values.RIMESInstallRoot.value -ne $InstallRoot){throw 'Installed Apps entry does not match the active version'}
    $launcher=[string]$values.RIMESUninstallDirectory.value
    Assert-OwnedVersion $InstallRoot $launcher | Out-Null
    if(-not (Test-Path -LiteralPath (Join-Path $launcher 'Uninstall-App.ps1') -PathType Leaf) -or $values.UninstallString.value -ne (Get-UninstallCommand $InstallRoot $launcher)){throw 'Installed Apps uninstall command is invalid'}
}
function Get-SettingsShortcutPath {return Join-Path ([Environment]::GetFolderPath('Programs')) 'RIMES\RIMES Settings.lnk'}
function Read-SettingsShortcut {
    $path=Get-SettingsShortcutPath
    if(Test-Path -LiteralPath $path -PathType Leaf){return [pscustomobject]@{path=$path;bytes=[IO.File]::ReadAllBytes($path)}}
    return $null
}
function Restore-SettingsShortcut($State) {
    $path=Get-SettingsShortcutPath
    if($null -eq $State){if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force};return}
    if($State.path -ne $path){throw 'Shortcut recovery path mismatch'}
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [IO.File]::WriteAllBytes($path,[byte[]]$State.bytes)
}
function Test-OwnedSettingsShortcut([string]$InstallRoot,$Shortcut) {
    $versions=[IO.Path]::GetFullPath((Join-Path $InstallRoot 'versions')).TrimEnd('\')+'\'
    return $Shortcut.Arguments -eq '--settings' -and [IO.Path]::GetFileName($Shortcut.TargetPath) -eq 'RimesBroker.exe' -and $Shortcut.TargetPath.StartsWith($versions,[StringComparison]::OrdinalIgnoreCase)
}
function Test-SettingsCommandSupported([string]$Directory) {
    try {
        $help=& (Join-Path $Directory 'x64\RimesBroker.exe') --help 2>&1 | Out-String
        return $LASTEXITCODE -eq 0 -and $help -match '--settings\b'
    } catch {return $false}
}
function Write-SettingsShortcut([string]$InstallRoot,[string]$Directory) {
    if(-not (Test-SettingsCommandSupported $Directory)){
        Remove-OwnedSettingsShortcut $InstallRoot
        Write-Host 'This version does not support the settings launch command. Open settings from the RIMES tray menu; its managed Start Menu entry was removed.'
        return
    }
    $path=Get-SettingsShortcutPath
    $shell=New-Object -ComObject WScript.Shell
    $shortcut=$null
    try {
        $shortcut=$shell.CreateShortcut($path)
        if((Test-Path -LiteralPath $path) -and -not (Test-OwnedSettingsShortcut $InstallRoot $shortcut)){return}
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        $shortcut.TargetPath=Join-Path $Directory 'x64\RimesBroker.exe'
        $shortcut.Arguments='--settings'
        $shortcut.WorkingDirectory=Join-Path $Directory 'x64'
        $shortcut.IconLocation=$shortcut.TargetPath+',0'
        $shortcut.Description='Open ' + (Get-ProductDisplayName) + ' settings'
        $shortcut.Save()
    } finally {if($shortcut){[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) | Out-Null};[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) | Out-Null}
    Assert-SettingsShortcut $InstallRoot $Directory
}
function Assert-SettingsShortcut([string]$InstallRoot,[string]$Directory) {
    $path=Get-SettingsShortcutPath
    $supported=Test-SettingsCommandSupported $Directory
    if(-not $supported -and -not (Test-Path -LiteralPath $path -PathType Leaf)){return}
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)){throw 'The RIMES Start Menu settings shortcut is missing'}
    $shell=New-Object -ComObject WScript.Shell
    $shortcut=$null
    try {
        $shortcut=$shell.CreateShortcut($path)
        if(Test-OwnedSettingsShortcut $InstallRoot $shortcut){
            if(-not $supported){throw 'The RIMES settings shortcut targets a version that lacks the settings command'}
            if($shortcut.TargetPath -ne (Join-Path $Directory 'x64\RimesBroker.exe')){throw 'The RIMES Start Menu shortcut points to a different version'}
        }
        # User-edited shortcuts retain their target and arguments.
    } finally {if($shortcut){[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) | Out-Null};[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) | Out-Null}
}
function Write-RetainedUninstallState([string]$Root,[string]$Directory,$Result) {
    $statePath=Join-Path $Root 'state.json'
    $destination=Join-Path $Root 'uninstalled-state.json'
    $destinationItem=Get-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
    if($destinationItem -and ($destinationItem.PSIsContainer -or ($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint))){throw 'The uninstall recovery record path is not a regular file. Preserve it and repair the installation.'}
    $record=[pscustomobject]@{active=$Directory;recovered=$true}
    if(Test-Path -LiteralPath $statePath){
        try {
            $previous=Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
            if($previous.PSObject.Properties['active'] -and $previous.active -eq $Directory){$record=$previous}
        } catch {}
    }
    $record | Add-Member -NotePropertyName uninstalled -NotePropertyValue $true -Force
    $record | Add-Member -NotePropertyName uninstallResult -NotePropertyValue $Result -Force
    $temporary=Join-Path $Root ('uninstalled.'+[guid]::NewGuid().ToString('N')+'.tmp')
    try {
        $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporary -Encoding UTF8
        Move-Item -LiteralPath $temporary -Destination $destination -Force
        # Keep the old active state intact until the recovery record is safely
        # published. Failure here lets the caller restore registration.
        if(Test-Path -LiteralPath $statePath){Remove-Item -LiteralPath $statePath -Force}
    } finally {if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}}
}
function Get-RecordedUninstallResult([string]$Root,[string]$Directory,[int]$ExitCode) {
    if($ExitCode -notin @(0,3010)){throw 'Uninstall did not confirm completion'}
    $pending=$ExitCode -eq 3010
    $fallback=[pscustomobject]@{Uninstalled=$true;RequiresSignOut=$pending;SignOutReason=$(if($pending){'unknown-installation-state'}else{'none'});UserDataRetained=$true}
    try {
        $record=Get-Content -LiteralPath (Join-Path $Root 'uninstalled-state.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        $result=$record.uninstallResult
        $reasons=if($pending){@('locked-dll','missing-registered-dll','previous-signout-required','unknown-installation-state')}else{@('none')}
        if($record.active -eq $Directory -and $record.uninstalled -is [bool] -and $record.uninstalled -and
           $result.Uninstalled -is [bool] -and $result.Uninstalled -and $result.UserDataRetained -is [bool] -and $result.UserDataRetained -and
           $result.RequiresSignOut -is [bool] -and $result.RequiresSignOut -eq $pending -and $result.SignOutReason -in $reasons){return $result}
    } catch {}
    # Old, damaged or mismatched records never suppress exit 3010's sign-out
    # warning or contribute arbitrary text to the original user's dialog.
    return $fallback
}
function Get-UninstallCompletionMessage($Result) {
    $message='RIMES was uninstalled. Your dictionaries, settings and credentials were kept.'
    if($Result.RequiresSignOut){
        $message+="`n`nSave your work and sign out before continuing. "
        if($Result.SignOutReason -eq 'locked-dll'){
            $message+='A previous input method DLL could not be opened exclusively and may still be in use.'
        } elseif($Result.SignOutReason -eq 'missing-registered-dll') {
            $message+='A registered input method DLL was missing, so whether a deleted copy remains loaded could not be confirmed. Signing out completes recovery.'
        } elseif($Result.SignOutReason -eq 'previous-signout-required') {
            $message+='The previous installation recorded a pending sign-out. Checking the current version cannot confirm whether that older copy remains loaded.'
        } else {
            $message+='Installation history was missing or damaged, so whether an old copy remains loaded could not be confirmed. Signing out completes recovery.'
        }
    }
    return $message
}
function Remove-OwnedSettingsShortcut([string]$InstallRoot) {
    $path=Get-SettingsShortcutPath
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)){return}
    $shell=New-Object -ComObject WScript.Shell
    $shortcut=$null
    try {$shortcut=$shell.CreateShortcut($path);if(Test-OwnedSettingsShortcut $InstallRoot $shortcut){Remove-Item -LiteralPath $path -Force}}
    finally {if($shortcut){[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) | Out-Null};[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) | Out-Null}
}
function Assert-UserUninstallCleanup([string]$InstallRoot) {
    if($null -ne (Get-BrokerAutostart)){throw 'A RimesBroker startup entry remains. User cleanup is incomplete.'}
    $path=Get-SettingsShortcutPath
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)){return}
    $shell=New-Object -ComObject WScript.Shell
    $shortcut=$null
    try {
        $shortcut=$shell.CreateShortcut($path)
        if(Test-OwnedSettingsShortcut $InstallRoot $shortcut){throw 'The managed RIMES settings shortcut remains. User cleanup is incomplete.'}
    } finally {if($shortcut){[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) | Out-Null};[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) | Out-Null}
}

# Deployment shares the serving Broker's single-instance lifetime. Holding its
# existing per-user/session mutex prevents TSF from starting a second engine
# after machine registration while original-user setup builds dictionaries.
function Invoke-UserDictionaryDeployment([string]$Directory) {
    $reservation=Stop-OwnedBroker $Directory
    try {
    $broker=Join-Path $Directory 'x64\RimesBroker.exe'
    $endpoint=(& $broker --print-endpoint | Out-String).Trim()
    if($LASTEXITCODE -or $endpoint -notmatch '^\\\\\.\\pipe\\RIMES\.Broker\.v2\.session-[0-9]+\.user-[a-f0-9]{16}$'){
        throw 'Cannot determine the current-user Broker deployment lock'
    }
    $name='Local\'+$endpoint.Substring('\\.\pipe\'.Length)
    $security=[Security.AccessControl.MutexSecurity]::new()
    $security.SetAccessRuleProtection($true,$false)
    $rule=[Security.AccessControl.MutexAccessRule]::new([Security.Principal.WindowsIdentity]::GetCurrent().User,[Security.AccessControl.MutexRights]::FullControl,[Security.AccessControl.AccessControlType]::Allow)
    $security.AddAccessRule($rule)
    $created=$false
    $mutex=[Threading.Mutex]::new($true,$name,[ref]$created,$security)
    $owned=$created
    try {
        if(-not $created){
            try {$owned=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$owned=$true}
            if(-not $owned){throw 'Another Rime operation still owns the dictionary lock. Wait for it to finish, then retry.'}
        }
        & $broker --deploy-only | Out-Host
        if($LASTEXITCODE){throw 'Dictionary deployment failed'}
    } finally {
        if($owned){$mutex.ReleaseMutex()}
        $mutex.Dispose()
    }
    } finally {if($reservation){$reservation.Dispose()}}
}
function Invoke-UserUninstall([string]$InstallRoot,[string]$Directory,[scriptblock]$UnregisterMachine) {
    Assert-OwnedBrokerAutostart $Directory
    $oldAutostart=Get-BrokerAutostart
    $oldShortcut=Read-SettingsShortcut
    $reservation=New-BrokerMaintenanceReservation
    try {
    try {
        # Remove the initiating user's launch entries first. A cancelled or
        # failed elevated phase restores them under this same original token.
        Remove-OwnedBrokerAutostart $Directory
        Remove-OwnedSettingsShortcut $InstallRoot
        $code=& $UnregisterMachine
        if($code -notin @(0,3010)){throw 'System unregistration failed'}
    } catch {
        $failure=$_
        $recoveryFailures=@()
        try{Restore-BrokerAutostart $oldAutostart}catch{$recoveryFailures+=$_.ToString()}
        try{Restore-SettingsShortcut $oldShortcut}catch{$recoveryFailures+=$_.ToString()}
        if($recoveryFailures.Count){throw "Uninstall failed: $failure. User entry recovery is incomplete: $($recoveryFailures -join '; ')"}
        throw $failure
    }
    # Recheck the initiating user's final state after the elevated worker exits.
    # Do not restore retired launch entries if this post-unregistration cleanup
    # fails: machine registration has already been removed successfully.
    try {
        Remove-OwnedBrokerAutostart $Directory
        Remove-OwnedSettingsShortcut $InstallRoot
        Assert-UserUninstallCleanup $InstallRoot
    } catch {throw "RIMES system registration was removed, but original-user cleanup is incomplete. Repair with the current Setup.exe and retry uninstall. $($_.Exception.Message)"}
    return $code
    } finally {if($reservation){$reservation.Dispose()}}
}
