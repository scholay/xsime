#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$ArtifactDirectory,
    [string]$WorkDirectory,
    [switch]$ProbeDesktop,
    [switch]$AppContainer,
    [switch]$Chord
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
if ($Chord -and $AppContainer) {
    throw 'Run -Chord separately from -AppContainer: the sandbox probe uses the sequential typing fixture.'
}

function Resolve-RimesExistingPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ValidateSet('Leaf', 'Container')][string]$PathType
    )
    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolved -PathType $PathType)) {
        throw "Expected a $PathType path: $Path"
    }
    return $resolved
}

function Write-RimesJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Object
    )
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, ($Object | ConvertTo-Json -Depth 6), $utf8)
}

function Get-RimesDesktopProbe {
    $process = Get-Process -Id $PID
    $sessionId = [int]$process.SessionId
    $desktopName = ''
    try {
        $desktopName = [System.Environment]::GetEnvironmentVariable('SESSIONNAME')
    } catch {
        $desktopName = ''
    }
    $explorer = @(Get-Process -Name explorer -ErrorAction SilentlyContinue)
    $notepadLaunch = [ordered]@{
        Attempted = $false
        Succeeded = $false
        Detail = 'not attempted'
    }
    if ($ProbeDesktop) {
        $notepadLaunch.Attempted = $true
        try {
            $notepad = Start-Process -FilePath "$env:SystemRoot\System32\notepad.exe" -PassThru -WindowStyle Minimized
            Start-Sleep -Milliseconds 800
            if ($null -ne $notepad -and -not $notepad.HasExited) {
                $notepadLaunch.Succeeded = $true
                $notepadLaunch.Detail = "notepad pid=$($notepad.Id)"
                Stop-Process -Id $notepad.Id -Force -ErrorAction SilentlyContinue
            } else {
                $notepadLaunch.Detail = 'notepad exited immediately'
            }
        } catch {
            $notepadLaunch.Detail = [string]$_.Exception.Message
        }
    }
    return [ordered]@{
        SessionId = $sessionId
        SessionName = $desktopName
        ExplorerProcessCount = $explorer.Count
        InteractiveSession = ($sessionId -gt 0)
        Notepad = $notepadLaunch
        Gap = @(
            'Hosted windows-2022 runners usually have a logon session and can start Notepad, but they do not provide a reliable interactive IME desktop.',
            'Registering RimesTsf and SendInput into Notepad/Edge cannot assert preedit, candidates, or 你好 on this image: there is no Chinese language pack, no user IME switch, and TSF often never attaches.',
            'The required typing assertions therefore run in-process: Fake ITfThreadMgr/ITfContext + real TextService + real Broker + pinned rime.dll.',
            'A later real-hardware pass must cover Win32, browsers, Electron, Office, DPI, password fields, and sleep/lock. See platforms/windows/native/MANUAL-TEST.md.'
        )
    }
}

$windowsRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$nativeRoot = Join-Path $windowsRoot 'native'
$fetchScript = Join-Path $windowsRoot 'scripts/Fetch-RimesLibrime.ps1'
$schemaRoot = Join-Path $nativeRoot 'testdata/e2e'
$openccRoot = [System.IO.Path]::GetFullPath((Join-Path $windowsRoot '..\..\rime-data\opencc'))
if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $nativeRoot "out/build/windows-$Architecture/$Configuration"
}
$artifactRoot = Resolve-RimesExistingPath -Path $ArtifactDirectory -PathType Container
$brokerPath = Resolve-RimesExistingPath -Path (Join-Path $artifactRoot 'RimesE2EBroker.exe') -PathType Leaf
$productionBrokerPath = Resolve-RimesExistingPath -Path (Join-Path $artifactRoot 'RimesBroker.exe') -PathType Leaf
$e2ePath = Resolve-RimesExistingPath -Path (Join-Path $artifactRoot 'RimesTsfE2E.exe') -PathType Leaf
$lexiconPath = Resolve-RimesExistingPath -Path (Join-Path $artifactRoot 'RimesUserLexiconTests.exe') -PathType Leaf
$schemaRoot = Resolve-RimesExistingPath -Path $schemaRoot -PathType Container

# Check both identities before starting either process. No test may attach to
# the installed Broker, even when it is already serving this user's session.
$testEndpoint = (& $brokerPath --print-endpoint | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $testEndpoint -notmatch '^\\\\\.\\pipe\\RIMES\.E2E\.Broker\.v2\.session-') {
    throw 'The E2E Broker does not have the isolated test endpoint identity.'
}
$productionEndpoint = (& $productionBrokerPath --print-endpoint | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $testEndpoint -eq $productionEndpoint) {
    throw 'The E2E Broker endpoint overlaps the daily input method.'
}

if ([string]::IsNullOrWhiteSpace($WorkDirectory)) {
    $WorkDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('rimes-ime-e2e-' + [guid]::NewGuid().ToString('N'))
}
$workRoot = [System.IO.Path]::GetFullPath($WorkDirectory)
New-Item -ItemType Directory -Path $workRoot -Force | Out-Null

$runtime = & $fetchScript -Architecture $Architecture -OutputDirectory (Join-Path $workRoot 'librime')
$rimeDll = Resolve-RimesExistingPath -Path $runtime.DllPath -PathType Leaf

# The learning fixture creates its own schema, dictionary and user directory;
# it neither starts a Broker nor opens the installed input method's data.
# Use the same checksum-verified architecture-specific engine as typing below.
$lexiconRoot = Join-Path $workRoot "user-lexicon-$Architecture"
New-Item -ItemType Directory -Path $lexiconRoot -Force | Out-Null
$lexiconStdout = Join-Path $lexiconRoot 'stdout.log'
$lexiconStderr = Join-Path $lexiconRoot 'stderr.log'
$lexiconExit = -1
$lexiconTimedOut = $false
$lexiconProcess = $null
try {
    $lexiconArgs = @('--engine', ('"{0}"' -f $rimeDll), ('"{0}"' -f $lexiconRoot))
    $lexiconProcess = Start-Process -FilePath $lexiconPath -ArgumentList $lexiconArgs -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $lexiconStdout -RedirectStandardError $lexiconStderr
    if ($null -eq $lexiconProcess) { throw 'Failed to start the isolated learning-dictionary fixture.' }
    $lexiconProcess.Handle | Out-Null
    if (-not $lexiconProcess.WaitForExit(180000)) {
        $lexiconTimedOut = $true
        # Only this newly launched fixture is terminated; its writes are all
        # confined to the private test tree retained with the diagnostics.
        $lexiconProcess.Kill()
        $lexiconProcess.WaitForExit(5000) | Out-Null
        throw 'Isolated real-engine learning-dictionary fixture timed out.'
    }
    $lexiconExit = [int]$lexiconProcess.ExitCode
    if ($lexiconExit -ne 0) { throw "Real-engine learning-dictionary fixture failed: exit=$lexiconExit" }
    Write-Host 'PASS: real Rime learning import/export, snapshot restore, live-session preservation and restart'
} finally {
    Write-RimesJson -Path (Join-Path $lexiconRoot 'result.json') -Object ([ordered]@{
        Passed = ($lexiconExit -eq 0)
        Architecture = $Architecture
        ExitCode = $lexiconExit
        TimedOut = $lexiconTimedOut
        Executable = $lexiconPath
        ExecutableSha256 = (Get-FileHash -LiteralPath $lexiconPath -Algorithm SHA256).Hash.ToLowerInvariant()
        RimeDll = $rimeDll
        RimeDllSha256 = (Get-FileHash -LiteralPath $rimeDll -Algorithm SHA256).Hash.ToLowerInvariant()
        LibrimeTag = [string]$runtime.Tag
        LibrimeArchiveSha256 = [string]$runtime.Sha256
        PrivateOutputParent = $lexiconRoot
    })
    foreach ($log in @($lexiconStdout, $lexiconStderr)) {
        if (Test-Path -LiteralPath $log -PathType Leaf) { Get-Content -LiteralPath $log -Tail 40 | ForEach-Object { Write-Host $_ } }
    }
    if ($null -ne $lexiconProcess) { $lexiconProcess.Dispose() }
}

# Exercise the actual archive/store/compiler path with real pinned Rime. This
# maintenance mode has no IPC, registration or installed-profile side effects.
Copy-Item -LiteralPath $rimeDll -Destination (Join-Path $artifactRoot 'rime.dll') -Force
& $brokerPath --scheme-import-smoke (Join-Path $workRoot 'scheme-import')
if ($LASTEXITCODE -ne 0) { throw 'Native scheme import/typing/rollback smoke failed.' }

$fixture = Join-Path $workRoot 'wanxiang-original.zip'
python (Join-Path $windowsRoot '../../scripts/input-schemes/fetch_fixture.py') $fixture
if ($LASTEXITCODE -ne 0) { throw 'Pinned original scheme fixture could not be verified.' }
$expected = ([string][char]0x4f60) + [char]0x597d
& $brokerPath --scheme-import-smoke (Join-Path $workRoot 'wanxiang-import') $fixture wanxiang nihao $expected
if ($LASTEXITCODE -ne 0) {
    # Only this isolated fixture's compiler logs; no installed profile or text.
    Get-ChildItem -LiteralPath (Join-Path $workRoot 'wanxiang-import/library/logs') -Filter '*.log' |
        ForEach-Object { Get-Content -LiteralPath $_.FullName -Tail 12 }
    throw 'Original Wanxiang import and native typing failed.'
}
& $brokerPath --scheme-import-smoke (Join-Path $workRoot 'wanxiang-t9-import') $fixture wanxiang_t9 64426 $expected
if ($LASTEXITCODE -ne 0) {
    Get-ChildItem -LiteralPath (Join-Path $workRoot 'wanxiang-t9-import/library/logs') -Filter '*.log' |
        ForEach-Object { Get-Content -LiteralPath $_.FullName -Tail 12 }
    throw 'Original Wanxiang nine-key typing failed.'
}

$sharedDir = Join-Path $workRoot 'shared'
$userDir = Join-Path $workRoot 'user'
$logDir = Join-Path $workRoot 'logs'
New-Item -ItemType Directory -Path $sharedDir, $userDir, $logDir -Force | Out-Null
Copy-Item -Path (Join-Path $schemaRoot '*') -Destination $sharedDir -Force
if (Test-Path -LiteralPath $openccRoot -PathType Container) {
    $openccDestination = Join-Path $sharedDir 'opencc'
    New-Item -ItemType Directory -Path $openccDestination -Force | Out-Null
    Copy-Item -Path (Join-Path $openccRoot '*') -Destination $openccDestination -Recurse -Force
}

$env:RIMES_ALLOW_SESSION_0 = '1'
$probe = Get-RimesDesktopProbe
Write-RimesJson -Path (Join-Path $workRoot 'desktop-probe.json') -Object $probe
Write-Host ("Desktop probe: session={0} explorer={1} notepad={2}" -f $probe.SessionId, $probe.ExplorerProcessCount, $probe.Notepad.Detail)

$brokerStdout = Join-Path $logDir 'broker-stdout.log'
$brokerStderr = Join-Path $logDir 'broker-stderr.log'
$brokerArgs = @(
    '--full-maintenance-check',
    '--rime-dll', $rimeDll,
    '--shared-data-dir', $sharedDir,
    '--user-data-dir', $userDir,
    '--log-dir', $logDir
)
# The workbench reads LOCALAPPDATA, independently of librime's --user-data-dir.
# Keep developer settings (including ASCII mode) out of the test, and never
# allow a test workbench to save into the real user's configuration directory.
$previousLocalAppData = $env:LOCALAPPDATA
$testLocalAppData = Join-Path $workRoot 'appdata'
New-Item -ItemType Directory -Path (Join-Path $testLocalAppData 'RIMES') -Force | Out-Null
Write-RimesJson -Path (Join-Path $testLocalAppData 'RIMES/settings.json') -Object ([ordered]@{
    schema = $(if ($Chord) { 'my_combo' } else { 'rime_ice' })
    ascii = $false
    chord_enabled = [bool]$Chord
    chord_interval_ms = 300
})

$broker = $null
$e2eExit = 1
$gracefulShutdown = $false
try {
    $env:LOCALAPPDATA = $testLocalAppData
    $broker = Start-Process -FilePath $brokerPath -ArgumentList $brokerArgs -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $brokerStdout -RedirectStandardError $brokerStderr
    if ($null -eq $broker) {
        throw 'Failed to start RimesBroker.exe'
    }
    # Keep the native handle before exit. Windows PowerShell's Start-Process
    # can otherwise report a null ExitCode for an already-exited child.
    $broker.Handle | Out-Null
    Start-Sleep -Seconds 2
    if ($broker.HasExited) {
        throw "RimesBroker exited before the typing tests with code $($broker.ExitCode). stderr=$(Get-Content -LiteralPath $brokerStderr -Raw -ErrorAction SilentlyContinue)"
    }

    $typingArgs = @{FilePath=$e2ePath; Wait=$true; PassThru=$true; NoNewWindow=$true}
    if ($Chord) { $typingArgs.ArgumentList = @('--chord') }
    $e2e = Start-Process @typingArgs
    $e2eExit = [int]$e2e.ExitCode
    if ($e2eExit -eq 0 -and (Test-Path -LiteralPath $brokerStderr)) {
        Select-String -LiteralPath $brokerStderr -Pattern 'RIMES key latency' |
            ForEach-Object { Write-Host $_.Line }
    }
    if ($e2eExit -ne 0) {
        Write-Host "--- broker stderr ---"
        if (Test-Path -LiteralPath $brokerStderr) {
            Get-Content -LiteralPath $brokerStderr -Raw
        }
        Write-Host "--- broker stdout ---"
        if (Test-Path -LiteralPath $brokerStdout) {
            Get-Content -LiteralPath $brokerStdout -Raw
        }
        throw "RimesTsfE2E.exe exited with code $e2eExit"
    }
    if ($AppContainer) {
        & (Join-Path $artifactRoot 'RimesAppContainerLauncher.exe') (Join-Path $artifactRoot 'RimesSandboxProbe.exe')
        if ($LASTEXITCODE -ne 0) { throw "AppContainer input probe failed: exit=$LASTEXITCODE" }
        Write-Host 'PASS: capability-free AppContainer TSF/Broker Chinese input and endpoint isolation'
        & (Join-Path $artifactRoot 'RimesAppContainerLauncher.exe') (Join-Path $artifactRoot 'RimesSandboxProbe.exe') --lpac
        if ($LASTEXITCODE -ne 0) { throw "LPAC input probe failed: exit=$LASTEXITCODE" }
        Write-Host 'PASS: LPAC TSF/Broker Chinese input and endpoint isolation'
    }
    # Exercise the real maintenance shutdown on the test-only endpoint. A
    # retained lifetime handle prevents a cached TSF from opening another
    # engine; the UI exits normally and librime flushes/closes its userdb.
    $mutexName='Local\'+$testEndpoint.Substring('\\.\pipe\'.Length)
    $reservation=[Threading.Mutex]::OpenExisting($mutexName)
    try {
        $stopEvent=[Threading.EventWaitHandle]::OpenExisting($mutexName+'.shutdown-'+$broker.Id)
        try {$stopEvent.Set() | Out-Null}finally{$stopEvent.Dispose()}
        $exited=$broker.WaitForExit(10000)
        if(-not $exited -or $broker.ExitCode -ne 0){throw "Isolated Broker did not shut down gracefully for maintenance (exited=$exited, exitCode=$($broker.ExitCode))"}
        $created=$false
        $attempt=[Threading.Mutex]::new($false,$mutexName,[ref]$created)
        try{if($created){throw 'Maintenance lost its Broker lifetime reservation'}}finally{$attempt.Dispose()}
        $gracefulShutdown=$true
        Write-Host 'PASS: real Broker graceful maintenance shutdown retains its lifetime gate'
    }finally{$reservation.Dispose()}
} finally {
    $env:LOCALAPPDATA = $previousLocalAppData
    if ($null -ne $broker -and -not $broker.HasExited) {
        Stop-Process -Id $broker.Id -Force -ErrorAction SilentlyContinue
        try { $broker.WaitForExit(5000) | Out-Null } catch { }
    }
}

Write-RimesJson -Path (Join-Path $workRoot 'e2e-result.json') -Object ([ordered]@{
    Passed = ($e2eExit -eq 0)
    Architecture = $Architecture
    ChordInput = [bool]$Chord
    RimeDll = $rimeDll
    LibrimeSource = [string]$runtime.Source
    LibrimeTag = [string]$runtime.Tag
    LibrimeSha256 = [string]$runtime.Sha256
    UserLexiconPassed = ($lexiconExit -eq 0)
    UserLexiconResult = (Join-Path $lexiconRoot 'result.json')
    Endpoint = $testEndpoint
    IsolatedFromDailyBroker = $true
    GracefulMaintenanceShutdown = $gracefulShutdown
    AppContainerInputPassed = [bool]$AppContainer
    LessPrivilegedAppContainerInputPassed = [bool]$AppContainer
    DesktopProbe = $probe
})

[pscustomobject]@{
    Passed = $true
    Architecture = $Architecture
    WorkDirectory = $workRoot
    RimeDll = $rimeDll
    DesktopProbe = $probe
}
