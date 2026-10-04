param(
    [Parameter(Mandatory = $true)][int]$SlowConnection,
    [Parameter(Mandatory = $true)][int]$ReaderConnection,
    [string]$SlowIdentity = '',
    [string]$ReaderIdentity = '',
    [string]$SlowMarker = '',
    [string]$ReaderMarker = '',
    [int]$TokenActivationDelayMs = 6000,
    [string]$Endpoint = 'http://127.0.0.1:8383/mcp',
    [string]$Fairyfly = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
if ($SlowConnection -eq $ReaderConnection -or $TokenActivationDelayMs -lt 0 -or
    $TokenActivationDelayMs -gt 10000) { throw 'Two distinct connections and a valid token delay are required' }
if (($SlowIdentity -or $ReaderIdentity) -and
    (-not $SlowIdentity -or -not $ReaderIdentity -or $SlowIdentity -eq $ReaderIdentity -or
     $SlowIdentity -cnotmatch '^[A-Z0-9]{3}/[0-9]{3}/[A-Z0-9_]+$' -or
     $ReaderIdentity -cnotmatch '^[A-Z0-9]{3}/[0-9]{3}/[A-Z0-9_]+$')) {
    throw 'Provide two distinct exact SAP identities as SID/CLIENT/USER'
}
if ($SlowIdentity -and (($SlowIdentity -split '/')[0..1] -join '/') -ne (($ReaderIdentity -split '/')[0..1] -join '/')) {
    throw 'The two SAP identities must share one SID/CLIENT to test SAP user isolation'
}
if ($SlowIdentity -and (-not $SlowMarker -or -not $ReaderMarker -or
    $SlowMarker.Length -lt 4 -or $ReaderMarker.Length -lt 4 -or
    $SlowMarker.Contains($ReaderMarker) -or $ReaderMarker.Contains($SlowMarker))) {
    throw 'Provide two distinct screen markers that appear in only their own SAP windows'
}
$endpointUri = $null
if (-not [Uri]::TryCreate($Endpoint, [UriKind]::Absolute, [ref]$endpointUri) -or
    -not $endpointUri.IsLoopback -or $endpointUri.Scheme -notin @('http', 'https')) {
    throw 'The live probe endpoint must be HTTP(S) loopback'
}
Add-Type -AssemblyName System.Net.Http
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(90)
$names = @()

function New-TestToken([string]$scopes, [string]$identity) {
    $name = 'fflive' + [Guid]::NewGuid().ToString('N')
    $script:names += $name
    $system = if ($identity) { ($identity -split '/')[0..1] -join '/' } else { 'A4H/001' }
    $options = @('mcp', 'token', 'create', $name, '--scope', $scopes, '--system', $system,
                 '--connections', 'Bigfox', '--read-only', '--expires', '1h', '--output', 'json')
    if ($identity) { $options += @('--sap-identity', $identity) }
    $raw = & $exe @options
    if ($LASTEXITCODE -ne 0) { throw 'Could not create test token' }
    return ($raw | ConvertFrom-Json).data.token
}

function Start-Call([string]$tool, $arguments, [string]$token, [int]$id) {
    $body = @{ jsonrpc='2.0'; id=$id; method='tools/call'; params=@{
        name=$tool; arguments=$arguments
    } } | ConvertTo-Json -Depth 12 -Compress
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, $Endpoint)
    $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $token)
    $request.Headers.Accept.ParseAdd('application/json')
    $request.Content = [System.Net.Http.StringContent]::new($body, [Text.Encoding]::UTF8, 'application/json')
    return @{ Request=$request; Task=$client.SendAsync($request) }
}

function Finish-Call($call) {
    $response = $call.Task.GetAwaiter().GetResult()
    try {
        $body = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
        if (-not $response.IsSuccessStatusCode -or $body.error -or $body.result.isError) {
            $code = if ($body.error) { $body.error.data.code } else { $body.result.structuredContent.error.code }
            throw "MCP call failed: $code"
        }
        return $body
    } finally {
        $response.Dispose()
        $call.Request.Dispose()
    }
}

function Start-ProgressBatch($arguments, [string]$token) {
    $body = @{ jsonrpc='2.0'; id=1; method='tools/call'; params=@{
        name='gui_batch'; arguments=$arguments; _meta=@{ progressToken='batch-start-gate' }
    } } | ConvertTo-Json -Depth 12 -Compress
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, $Endpoint)
    $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $token)
    $request.Headers.Accept.ParseAdd('text/event-stream')
    $request.Content = [System.Net.Http.StringContent]::new($body, [Text.Encoding]::UTF8, 'application/json')
    $response = $client.SendAsync($request, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
    if (-not $response.IsSuccessStatusCode -or $response.Content.Headers.ContentType.MediaType -ne 'text/event-stream') {
        $response.Dispose(); $request.Dispose()
        throw 'Batch did not start as an SSE progress stream'
    }
    $reader = [IO.StreamReader]::new($response.Content.ReadAsStreamAsync().GetAwaiter().GetResult())
    return @{ Request=$request; Response=$response; Reader=$reader }
}

function Read-ProgressEvent($batch) {
    while ($true) {
        $lineTask = $batch.Reader.ReadLineAsync()
        if (-not $lineTask.Wait(30000)) { throw 'Timed out waiting for batch progress' }
        $line = $lineTask.GetAwaiter().GetResult()
        if ($null -eq $line) { throw 'Batch progress stream ended unexpectedly' }
        if (-not $line.StartsWith('data:')) { continue }
        $eventBody = $line.Substring(5).Trim() | ConvertFrom-Json
        if ($eventBody.method -eq 'notifications/progress') { return $eventBody }
        if ($eventBody.id -eq 1) { return $eventBody }
    }
}

try {
    $slowToken = New-TestToken 'batch,screen' $SlowIdentity
    $readerToken = New-TestToken 'screen' $ReaderIdentity
    if ($TokenActivationDelayMs) { Start-Sleep -Milliseconds $TokenActivationDelayMs }
    $items = @(for ($i = 0; $i -lt 20; $i++) {
        @{ tool='gui_screen_read'; arguments=@{ no_tabs=$true; max_rows=2 } }
    })
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $slow = Start-ProgressBatch @{ connection=$SlowConnection; items=$items } $slowToken
    $progress = Read-ProgressEvent $slow
    while ($progress.method -eq 'notifications/progress' -and $progress.params.progress -lt 1) {
        $progress = Read-ProgressEvent $slow
    }
    if ($progress.method -ne 'notifications/progress' -or $progress.params.progress -ne 1) {
        throw 'The batch did not confirm completion of its first worker item before the reader started'
    }
    $batchStarted = $timer.ElapsedMilliseconds
    $readerStarted = $timer.ElapsedMilliseconds
    $reader = Start-Call 'gui_screen_read' @{ connection=$ReaderConnection; no_tabs=$true; max_rows=2 } $readerToken 2
    $sameStarted = $timer.ElapsedMilliseconds
    $same = Start-Call 'gui_screen_read' @{ connection=$SlowConnection; no_tabs=$true; max_rows=2 } $slowToken 3
    $batchProgress = 1
    $batchFinalBeforeReader = $false
    $pendingLine = $slow.Reader.ReadLineAsync()
    while (-not $reader.Task.IsCompleted) {
        if ($same.Task.IsCompleted -and -not $batchFinalBeforeReader) {
            throw 'Same-session read completed before the batch final response'
        }
        if (-not $pendingLine.Wait(20)) { continue }
        $line = $pendingLine.GetAwaiter().GetResult()
        if ($null -eq $line) { throw 'Batch progress stream ended before its final response' }
        if ($line.StartsWith('data:')) {
            $eventBody = $line.Substring(5).Trim() | ConvertFrom-Json
            if ($eventBody.method -eq 'notifications/progress') { $batchProgress = [int]$eventBody.params.progress }
            if ($eventBody.id -eq 1) { $batchFinalBeforeReader = $true; break }
        }
        $pendingLine = $slow.Reader.ReadLineAsync()
    }
    $readResult = Finish-Call $reader
    $readerFinished = $timer.ElapsedMilliseconds
    if ($same.Task.IsCompleted -and -not $batchFinalBeforeReader) {
        throw 'Same-session read completed before the batch final response'
    }
    # A final SSE line may already be buffered when the reader task completes.
    # Drain immediately available lines before deciding the ordering.
    while (-not $batchFinalBeforeReader -and $pendingLine.IsCompleted) {
        if ($same.Task.IsCompleted -and -not $batchFinalBeforeReader) {
            throw 'Same-session read completed before the batch final response'
        }
        $line = $pendingLine.GetAwaiter().GetResult()
        if ($null -eq $line) { throw 'Batch progress stream ended before its final response' }
        if ($line.StartsWith('data:')) {
            $eventBody = $line.Substring(5).Trim() | ConvertFrom-Json
            if ($eventBody.method -eq 'notifications/progress') { $batchProgress = [int]$eventBody.params.progress }
            if ($eventBody.id -eq 1) { $batchFinalBeforeReader = $true; break }
        }
        $pendingLine = $slow.Reader.ReadLineAsync()
    }
    $readerBeforeBatch = -not $batchFinalBeforeReader
    if (-not $batchFinalBeforeReader) {
        $finalWait = [Diagnostics.Stopwatch]::StartNew()
        while ($true) {
            if ($same.Task.IsCompleted -and -not $batchFinalBeforeReader) {
                throw 'Same-session read completed before the batch final response'
            }
            if (-not $pendingLine.Wait(20)) {
                if ($finalWait.ElapsedMilliseconds -ge 30000) { throw 'Timed out waiting for the batch result' }
                continue
            }
            $line = $pendingLine.GetAwaiter().GetResult()
            if ($null -eq $line) { throw 'Batch progress stream ended before its final response' }
            if ($line.StartsWith('data:')) {
                $eventBody = $line.Substring(5).Trim() | ConvertFrom-Json
                if ($eventBody.method -eq 'notifications/progress') { $batchProgress = [int]$eventBody.params.progress }
                if ($eventBody.id -eq 1) { break }
            }
            $pendingLine = $slow.Reader.ReadLineAsync()
        }
    }
    $progress = $eventBody
    if ($progress.error -or $progress.result.isError) { throw 'Batch returned an error after the progress gate' }
    $batchFinished = $timer.ElapsedMilliseconds
    $readerScreen = [string]$readResult.result.content[0].text
    if (-not $readerScreen.Contains("connection $ReaderConnection,") -or
        ($ReaderIdentity -and (-not $readerScreen.Contains($ReaderMarker) -or $readerScreen.Contains($SlowMarker)))) {
        throw 'Reader response came from the wrong SAP window'
    }
    if (-not $readerBeforeBatch) { throw 'Reader did not complete before the slow batch' }
    $sameResult = Finish-Call $same
    $sameFinished = $timer.ElapsedMilliseconds
    $slowScreen = [string]$sameResult.result.content[0].text
    if (-not $slowScreen.Contains("connection $SlowConnection,") -or
        ($SlowIdentity -and (-not $slowScreen.Contains($SlowMarker) -or $slowScreen.Contains($ReaderMarker)))) {
        throw 'Queued same-session response came from the wrong SAP window'
    }
    [pscustomobject]@{
        slow_connection = $SlowConnection
        reader_connection = $ReaderConnection
        batch_items = 20
        first_batch_item_finished_ms = $batchStarted
        reader_started_ms = $readerStarted
        reader_finished_ms = $readerFinished
        batch_finished_ms = $batchFinished
        reader_finished_before_batch = $readerBeforeBatch
        same_session_read_started_ms = $sameStarted
        same_session_read_finished_ms = $sameFinished
        same_session_waited_for_batch = $true
    } | ConvertTo-Json -Compress
} finally {
    if ($slow) { $slow.Reader.Dispose(); $slow.Response.Dispose(); $slow.Request.Dispose() }
    $tokenCleanupFailed = $false
    foreach ($name in $names) {
        $raw = & $exe mcp token delete $name --yes --output json
        $exitCode = $LASTEXITCODE
        try { $deleted = $raw | ConvertFrom-Json } catch { $deleted = $null }
        if (($exitCode -ne 0 -or $deleted.status -ne 'success') -and
            $deleted.error.code -ne 'TOKEN_NOT_FOUND') {
            Write-Warning "Could not remove temporary MCP token $name"
            $tokenCleanupFailed = $true
        }
    }
    $client.Dispose()
    if ($tokenCleanupFailed) { throw 'Batch probe could not remove all temporary MCP tokens' }
}
