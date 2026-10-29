#requires -Version 5.1
param([string]$OutputDirectory = (Join-Path ([IO.Path]::GetTempPath()) ('rimes-translation-download-' + [Guid]::NewGuid().ToString('N'))))
Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot '..\translation\Install-LocalTranslation.ps1'
$tokens = $null; $parseErrors = $null
$tree = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Translation installer must parse in Windows PowerShell 5.1' }
$functions = @('File-Sha256', 'Throw-TranslationDownloadFailure', 'Translation-DownloadFailureReason',
    'Test-TranslationArchive', 'Translation-RequestTimeout', 'New-TranslationHttpRequest',
    'Invoke-TranslationTransfer', 'Wait-TranslationDownloadRetry', 'Get-VerifiedTranslationArchive')
foreach ($name in $functions) {
    $found = @($tree.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $false))
    if ($found.Count -ne 1) { throw "Missing unique downloader function: $name" }
    . ([ScriptBlock]::Create($found[0].Extent.Text))
}
# The real request factory is tested without connecting. Transfer tests replace
# only that factory, so the actual stream bounds, retry and publication run.
$request = New-TranslationHttpRequest ([Uri]'https://fixture.invalid/file') 4500
if ($request.Timeout -ne 4500 -or $request.ReadWriteTimeout -gt 4500 -or $request.AllowAutoRedirect) {
    throw 'Per-request connection/read/redirect bounds are not configured'
}
$request.Abort()
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Net;
public sealed class TranslationTimeoutStream : MemoryStream {
    public override int Read(byte[] buffer, int offset, int count) {
        throw new WebException("credential-token-must-not-be-logged", WebExceptionStatus.Timeout);
    }
}
'@
$script:Outcomes = New-Object Collections.Queue
$script:Requests = 0; $script:Retries = 0; $script:Timeouts = @()
function New-TranslationHttpRequest([Uri]$Uri, [int]$TimeoutMilliseconds) {
    $script:Requests++
    $script:Timeouts += $TimeoutMilliseconds
    if (!$script:Outcomes.Count) { throw 'Unexpected network request' }
    $result = [pscustomobject]@{ Outcome = $script:Outcomes.Dequeue() }
    $result | Add-Member ScriptMethod GetResponse {
        $outcome = $this.Outcome
        if ($outcome.Kind -eq 'timeout') {
            throw [Net.WebException]::new('credential-token-must-not-be-logged', [Net.WebExceptionStatus]::Timeout)
        }
        $response = [pscustomobject]@{
            StatusCode = $outcome.Status
            ContentLength = $outcome.Length
            Headers = @{ Location = $outcome.Location }
            Bytes = $outcome.Bytes
            Kind = $outcome.Kind
        }
        $response | Add-Member ScriptMethod GetResponseStream {
            if ($this.Kind -eq 'read-timeout') { return [TranslationTimeoutStream]::new() }
            return [IO.MemoryStream]::new([byte[]]$this.Bytes, $false)
        }
        $response | Add-Member ScriptMethod Close { }
        return $response
    }
    $result | Add-Member ScriptMethod Abort { }
    return $result
}
function Wait-TranslationDownloadRetry([int]$Attempt, [DateTime]$Deadline) {
    $script:Retries++
    if ((Translation-RequestTimeout $Deadline) -le 0) { throw 'Retry exceeded its deadline' }
}
function Outcome([string]$Kind = 'body', [byte[]]$Bytes = ([Text.Encoding]::UTF8.GetBytes('fixture')), [int]$Status = 200, [long]$Length = -1, [string]$Location = '') {
    return [pscustomobject]@{ Kind=$Kind; Bytes=$Bytes; Status=$Status; Length=$Length; Location=$Location }
}
function Reset-Outcomes($Values) {
    $script:Outcomes.Clear(); $script:Requests=0; $script:Retries=0; $script:Timeouts=@()
    foreach ($value in $Values) { $script:Outcomes.Enqueue($value) }
}
function Require([bool]$Value, [string]$Message) {
    if (!$Value) { throw $Message }
    Write-Host ('PASS: ' + $Message)
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$fixture = Join-Path $OutputDirectory 'source'
[IO.File]::WriteAllBytes($fixture, [Text.Encoding]::UTF8.GetBytes('fixture'))
$item = [pscustomobject]@{ name='public-fixture'; file='fixture.zip'; url='https://fixture.invalid/file?credential-token-must-not-be-logged'; bytes=7; sha256=(File-Sha256 $fixture) }
function Attempt([string]$Name, $Outcomes, [bool]$ShouldSucceed, [DateTime]$Deadline = ([DateTime]::UtcNow.AddSeconds(20)), [string]$Cache = '') {
    $archive = Join-Path $OutputDirectory ($Name + '.zip')
    Reset-Outcomes $Outcomes
    $succeeded = $false
    $captured = New-Object 'Collections.Generic.List[string]'
    try {
        & { Get-VerifiedTranslationArchive $item $archive $Cache $Deadline } 6>&1 |
            ForEach-Object { $captured.Add($_.ToString()) }
        $succeeded = $true
    } catch {
        $captured.Add($_.Exception.Message)
    }
    $messages = [string]::Join("`n", $captured)
    Require ($succeeded -eq $ShouldSucceed) ($Name + ' has the expected terminal result')
    Require (!$messages.Contains('credential-token-must-not-be-logged')) ($Name + ' does not expose source or redirect credentials')
    Require (@(Get-ChildItem -LiteralPath $OutputDirectory -Filter ($Name + '.zip.partial-*')).Count -eq 0) ($Name + ' cleans temporary downloads')
    Require (@($script:Timeouts | Where-Object { $_ -le 0 -or $_ -gt 120000 }).Count -eq 0) ($Name + ' keeps request deadlines bounded')
    if ($ShouldSucceed) { Require (Test-TranslationArchive $item $archive) ($Name + ' publishes only the pinned bytes') }
    else { Require (!(Test-Path -LiteralPath $archive)) ($Name + ' does not publish unverified bytes') }
    return $archive
}
try {
    $null = Attempt 'connection-retry' @((Outcome 'timeout'), (Outcome)) $true
    Require ($script:Requests -eq 2 -and $script:Retries -eq 1) 'connection timeout has a finite retry'
    $null = Attempt 'read-retry' @((Outcome 'read-timeout'), (Outcome)) $true
    Require ($script:Requests -eq 2) 'read timeout retries without publishing a partial file'
    $null = Attempt 'http-retry' @((Outcome -Status 503), (Outcome)) $true
    Require ($script:Requests -eq 2) 'transient HTTP failure retries'
    $bad = [Text.Encoding]::UTF8.GetBytes('changed')
    $null = Attempt 'hash-retry' @((Outcome -Bytes $bad), (Outcome)) $true
    Require ($script:Requests -eq 2) 'checksum mismatch is retried without changing the pin'
    $null = Attempt 'persistent-mismatch' @((Outcome -Bytes $bad), (Outcome -Bytes $bad), (Outcome -Bytes $bad)) $false
    Require ($script:Requests -eq 3 -and $script:Retries -eq 2) 'persistent corruption fails after exactly three requests'
    $null = Attempt 'oversized' @((Outcome -Bytes ([Text.Encoding]::UTF8.GetBytes('oversize'))), (Outcome), (Outcome)) $true
    Require ($script:Requests -eq 2) 'body exceeding the pinned byte limit is rejected before retry'
    $null = Attempt 'downgrade' @((Outcome -Status 302 -Location 'http://fixture.invalid/credential-token-must-not-be-logged'), (Outcome -Status 302 -Location 'http://fixture.invalid/x'), (Outcome -Status 302 -Location 'http://fixture.invalid/x')) $false
    Require ($script:Requests -eq 3) 'HTTPS downgrade never follows the redirect'
    $null = Attempt 'expired' @() $false ([DateTime]::UtcNow.AddSeconds(-1))
    Require ($script:Requests -eq 0) 'expired total deadline never starts a request'
    $offline = Join-Path $OutputDirectory 'offline'
    New-Item -ItemType Directory -Path $offline | Out-Null
    Copy-Item -LiteralPath $fixture -Destination (Join-Path $offline $item.file)
    $null = Attempt 'offline' @() $true ([DateTime]::UtcNow.AddSeconds(20)) $offline
    Require ($script:Requests -eq 0) 'verified offline cache avoids network requests'
    [IO.File]::WriteAllBytes((Join-Path $offline $item.file), $bad)
    $null = Attempt 'corrupt-offline' @() $false ([DateTime]::UtcNow.AddSeconds(20)) $offline
    Require ($script:Requests -eq 0) 'corrupt offline cache cannot bypass verification'
    $cached = Join-Path $OutputDirectory 'cached.zip'
    Copy-Item -LiteralPath $fixture -Destination $cached
    Reset-Outcomes @()
    Get-VerifiedTranslationArchive $item $cached '' ([DateTime]::UtcNow.AddSeconds(20))
    Require ($script:Requests -eq 0 -and (Test-TranslationArchive $item $cached)) 'verified cache is preserved without another request'
    $replace = Join-Path $OutputDirectory 'replace.zip'
    [IO.File]::WriteAllBytes($replace, $bad)
    Reset-Outcomes @((Outcome))
    Get-VerifiedTranslationArchive $item $replace '' ([DateTime]::UtcNow.AddSeconds(20))
    Require (Test-TranslationArchive $item $replace) 'invalid cache is replaced only after complete verification'
    Write-Host 'Translation download deadlines, retries, hashes, cache and diagnostics passed'
} finally {
    Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
}
