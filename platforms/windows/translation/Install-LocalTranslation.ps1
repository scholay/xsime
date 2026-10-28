param([Parameter(Mandatory=$true)][string]$Root, [string]$Cache = '')
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Root = [IO.Path]::GetFullPath($Root)
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'resources.lock.json') -Raw | ConvertFrom-Json
function File-Sha256([string]$path) {
    $hash = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { return ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose(); $hash.Dispose() }
}
function Throw-TranslationDownloadFailure([string]$Reason) {
    $failure = [IO.IOException]::new($Reason)
    $failure.Data['TranslationSafeReason'] = $Reason
    throw $failure
}
function Translation-DownloadFailureReason([Exception]$ErrorValue) {
    $lastFailure = $ErrorValue
    for ($failureCursor = $ErrorValue; $failureCursor; $failureCursor = $failureCursor.InnerException) {
        $lastFailure = $failureCursor
        if ($failureCursor.Data.Contains('TranslationSafeReason')) { return [string]$failureCursor.Data['TranslationSafeReason'] }
        if ($failureCursor -is [Net.WebException]) {
            $status = if ($failureCursor.Response -is [Net.HttpWebResponse]) { ' HTTP=' + [int]$failureCursor.Response.StatusCode } else { '' }
            return 'network=' + $failureCursor.Status + $status
        }
    }
    # Exception messages may contain an authenticated redirect URL. Keep them out
    # of diagnostics; the logical resource and bounded phase are logged instead.
    return 'local-error=' + $lastFailure.GetType().Name + ' HRESULT=0x' + $lastFailure.HResult.ToString('X8')
}
function Test-TranslationArchive($Item, [string]$Path) {
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and
        (Get-Item -LiteralPath $Path).Length -eq $Item.bytes -and
        (File-Sha256 $Path) -eq $Item.sha256
}
function Translation-RequestTimeout([DateTime]$Deadline) {
    $remaining = [Math]::Floor(($Deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if ($remaining -le 0) { Throw-TranslationDownloadFailure 'download deadline exceeded' }
    return [int][Math]::Min(120000, $remaining)
}
function New-TranslationHttpRequest([Uri]$Uri, [int]$TimeoutMilliseconds) {
    $request = [Net.HttpWebRequest]::Create($Uri)
    $request.AllowAutoRedirect = $false
    $request.Timeout = $TimeoutMilliseconds
    $request.ReadWriteTimeout = [Math]::Min(15000, $TimeoutMilliseconds)
    $request.UserAgent = 'RIMES/1.1 offline translator (Mozilla/5.0)'
    $request.Headers['Cache-Control'] = 'no-cache'
    $request.Credentials = $null
    return $request
}
function Invoke-TranslationTransfer($Item, [string]$Output, [DateTime]$Deadline) {
    $uri = [Uri]$Item.url
    for ($redirect = 0; $redirect -le 5; $redirect++) {
        if (!$uri.IsAbsoluteUri -or $uri.Scheme -ne 'https' -or !$uri.Host -or
            $uri.UserInfo -or $uri.Fragment -or (!$uri.IsDefaultPort -and $uri.Port -ne 443)) {
            Throw-TranslationDownloadFailure 'source and redirects must remain HTTPS'
        }
        $request = $null; $response = $null; $responseStream = $null; $outputStream = $null
        try {
            $request = New-TranslationHttpRequest $uri (Translation-RequestTimeout $Deadline)
            $response = $request.GetResponse()
            $status = [int]$response.StatusCode
            if ($status -in @(301, 302, 303, 307, 308)) {
                $location = [string]$response.Headers['Location']
                if (!$location -or $redirect -eq 5) { Throw-TranslationDownloadFailure 'invalid or excessive redirects' }
                $uri = [Uri]::new($uri, $location)
                continue
            }
            if ($status -ne 200) { Throw-TranslationDownloadFailure ('HTTP=' + $status) }
            if ($response.ContentLength -gt [long]$Item.bytes) { Throw-TranslationDownloadFailure 'response exceeds pinned resource size' }
            $responseStream = $response.GetResponseStream()
            $outputStream = [IO.File]::Open($Output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            $buffer = New-Object byte[] 65536
            [long]$total = 0
            while ($true) {
                $remaining = Translation-RequestTimeout $Deadline
                if ($responseStream.CanTimeout) { $responseStream.ReadTimeout = [Math]::Min(15000, $remaining) }
                $count = $responseStream.Read($buffer, 0, $buffer.Length)
                if ($count -eq 0) { break }
                $total += $count
                if ($total -gt [long]$Item.bytes) { Throw-TranslationDownloadFailure ('received bytes=' + $total + ' exceed pinned bytes=' + $Item.bytes) }
                $outputStream.Write($buffer, 0, $count)
            }
            $outputStream.Dispose(); $outputStream = $null
            $actual = File-Sha256 $Output
            if ($total -ne [long]$Item.bytes -or $actual -ne $Item.sha256) {
                Throw-TranslationDownloadFailure ('received bytes=' + $total + ' expected bytes=' + $Item.bytes +
                    ' SHA256=' + $actual + ' expected SHA256=' + $Item.sha256)
            }
            return
        } finally {
            if ($outputStream) { $outputStream.Dispose() }
            if ($responseStream) { $responseStream.Dispose() }
            if ($response) { $response.Close() }
            if ($request) { $request.Abort() }
        }
    }
}
function Wait-TranslationDownloadRetry([int]$Attempt, [DateTime]$Deadline) {
    $remaining = Translation-RequestTimeout $Deadline
    Start-Sleep -Milliseconds ([Math]::Min($remaining, 1000 * $Attempt))
}
function Get-VerifiedTranslationArchive($Item, [string]$Archive, [string]$Cache, [DateTime]$Deadline) {
    if ($Item.name -notmatch '^[A-Za-z0-9_.-]{1,80}$' -or $Item.file -notmatch '^[A-Za-z0-9_.-]+$' -or
        $Item.sha256 -notmatch '^[A-Fa-f0-9]{64}$' -or [long]$Item.bytes -le 0 -or [long]$Item.bytes -gt 536870912) {
        throw 'Invalid pinned translation resource metadata'
    }
    if (Test-TranslationArchive $Item $Archive) {
        Write-Host ('translation verified-cache resource=' + $Item.name + ' bytes=' + $Item.bytes)
        return
    }
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $partial = $Archive + '.partial-' + [Guid]::NewGuid().ToString('N')
        $phase = 'preflight'
        try {
            $remaining = Translation-RequestTimeout $Deadline
            Write-Host ('translation download resource=' + $Item.name + ' attempt=' + $attempt + '/3 expected-bytes=' + $Item.bytes)
            if ($Cache) {
                $phase = 'offline-cache'
                $cached = Join-Path $Cache $Item.file
                if (!(Test-TranslationArchive $Item $cached)) { Throw-TranslationDownloadFailure 'offline cache is missing or differs from the pinned resource' }
                Copy-Item -LiteralPath $cached -Destination $partial
            } else {
                $phase = 'download'
                $attemptDeadline = [DateTime]::UtcNow.AddMilliseconds([Math]::Min(120000, $remaining))
                Invoke-TranslationTransfer $Item $partial $attemptDeadline
            }
            $phase = 'cache-verification'
            if (!(Test-TranslationArchive $Item $partial)) { Throw-TranslationDownloadFailure 'resource changed before verified cache publication' }
            # A second caller may have populated the same verified cache while
            # this request was in flight. Preserve it instead of replacing it.
            if (!(Test-TranslationArchive $Item $Archive)) {
                $phase = 'cache-publication'
                # PowerShell coerces $null to an empty string for a string
                # argument. File.Replace requires a real null backup path.
                if ([IO.File]::Exists($Archive)) { [IO.File]::Replace($partial, $Archive, [System.Management.Automation.Language.NullString]::Value) }
                else { [IO.File]::Move($partial, $Archive) }
            }
            Write-Host ('translation verified resource=' + $Item.name + ' bytes=' + $Item.bytes + ' SHA256=' + $Item.sha256)
            return
        } catch {
            $reason = Translation-DownloadFailureReason $_.Exception
            Write-Host ('translation failed resource=' + $Item.name + ' attempt=' + $attempt + '/3 phase=' + $phase + ' reason=' + $reason)
            if ($Cache -or $attempt -eq 3 -or [DateTime]::UtcNow -ge $Deadline) {
                throw [IO.IOException]::new('Translation resource ' + $Item.name + ' was not verified after ' + $attempt + ' attempt(s): ' + $reason)
            }
            Wait-TranslationDownloadRetry $attempt $Deadline
        } finally {
            if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
        }
    }
}
$identity = File-Sha256 (Join-Path $PSScriptRoot 'resources.lock.json')
$ready = Join-Path $Root 'ready.txt'
if ((Test-Path -LiteralPath $ready) -and (Get-Content -LiteralPath $ready -Raw).Trim() -eq $identity) { return }
$parent = Split-Path -Parent $Root
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$downloadCache = Join-Path $parent 'downloads'
New-Item -ItemType Directory -Path $downloadCache -Force | Out-Null
$stage = Join-Path $parent ('install-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
function Expand-VerifiedZip([string]$archive, [string]$destination) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $prefix = [IO.Path]::GetFullPath($destination).TrimEnd('\') + '\'
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($entry in $zip.Entries) {
            # Wheel test fixtures are not runtime dependencies; some exceed
            # Windows PowerShell 5.1's legacy MAX_PATH extraction limit.
            if ($entry.FullName -match '(^|/)tests/') { continue }
            $path = [IO.Path]::GetFullPath((Join-Path $destination $entry.FullName))
            if (!$path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe archive path' }
            if (!$entry.Name) { continue }
            New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $path, $false)
        }
    } finally { $zip.Dispose() }
}
try {
    # Keep the existing 600-second smoke limit meaningful: leave time for
    # extraction and the isolated interpreter check instead of one endless read.
    $downloadDeadline = [DateTime]::UtcNow.AddSeconds(540)
    foreach ($item in $manifest.resources) {
        # Keep verified archives across requests, failed installs and restarts.
        # Buffer edits must never force a fresh 182 MiB download.
        $archive = Join-Path $downloadCache $item.file
        Get-VerifiedTranslationArchive $item $archive $Cache $downloadDeadline
        $destination = switch ($item.kind) {
            'python' { Join-Path $stage 'python' }
            'wheel' { Join-Path $stage 'python/site-packages' }
            'model' { Join-Path $stage 'models' }
            default { throw 'Unknown resource type' }
        }
        Write-Host ('translation extract resource=' + $item.name)
        Expand-VerifiedZip $archive $destination
    }
    # Isolated embedded Python: never import packages or customizations from the host.
    Set-Content -LiteralPath (Join-Path $stage 'python/python312._pth') -Encoding ASCII -Value "python312.zip`n.`nsite-packages"
    Write-Host 'translation verify isolated runtime'
    & (Join-Path $stage 'python/python.exe') -I -W ignore::UserWarning -c 'import ctranslate2, sentencepiece, numpy'
    if ($LASTEXITCODE -ne 0) { throw 'Local translation runtime self-check failed' }
    Set-Content -LiteralPath (Join-Path $stage 'ready.txt') -Encoding ASCII -Value $identity
    $parentPrefix = [IO.Path]::GetFullPath($parent).TrimEnd('\') + '\'
    if (!$Root.StartsWith($parentPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        ![IO.Path]::GetFullPath($stage).StartsWith($parentPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Translation install path escaped its parent'
    }
    if (Test-Path -LiteralPath $Root) {
        # Preserve an older/broken install for diagnosis; never erase user data.
        Move-Item -LiteralPath $Root -Destination ($Root + '.previous-' + [Guid]::NewGuid().ToString('N'))
    }
    Move-Item -LiteralPath $stage -Destination $Root
    Write-Host 'translation verified runtime ready'
} finally {
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    $allowed = [IO.Path]::GetFullPath($parent).TrimEnd('\') + '\install-'
    if ($resolvedStage.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolvedStage)) {
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
}
