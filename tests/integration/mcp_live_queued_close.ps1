# Called only for the disposable SAP user's test-owned window, after other probes.
param(
    [Parameter(Mandatory=$true)][int]$ConnectionId,
    [Parameter(Mandatory=$true)][string]$Bearer,
    [Parameter(Mandatory=$true)][string]$Endpoint,
    [Parameter(Mandatory=$true)][string]$Fairyfly,
    [Parameter(Mandatory=$true)][string]$OwnMarker,
    [Parameter(Mandatory=$true)][string]$OtherMarker
)
$ErrorActionPreference = 'Stop'
$uri = $null
if ($ConnectionId -lt 0 -or -not $OwnMarker -or -not $OtherMarker -or
    -not [Uri]::TryCreate($Endpoint, [UriKind]::Absolute, [ref]$uri) -or
    -not $uri.IsLoopback -or $uri.Scheme -notin @('http','https')) {
    throw 'Queued close probe requires a saved ID and loopback endpoint'
}
Add-Type -AssemblyName System.Net.Http
$client = [Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(90)
$batchRequest = $null
$batchResponse = $null
$batchReader = $null
$queuedRequest = $null
$queuedResponse = $null
$queuedReader = $null

function New-SseRequest([string]$name, $arguments, [int]$id) {
    $body = @{ jsonrpc='2.0'; id=$id; method='tools/call'; params=@{
        name=$name; arguments=$arguments; _meta=@{ progressToken="close-$id" }
    } } | ConvertTo-Json -Depth 12 -Compress
    $request = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::Post, $Endpoint)
    $request.Headers.Authorization = [Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $Bearer)
    $request.Headers.Accept.ParseAdd('text/event-stream')
    $request.Content = [Net.Http.StringContent]::new($body, [Text.Encoding]::UTF8, 'application/json')
    return $request
}

function Read-SseData($reader) {
    while ($true) {
        $lineTask = $reader.ReadLineAsync()
        if (-not $lineTask.Wait(30000)) { throw 'Timed out waiting for SSE data' }
        $line = $lineTask.GetAwaiter().GetResult()
        if ($null -eq $line) { throw 'SSE stream ended without a final response' }
        if ($line.StartsWith('data:')) { return $line.Substring(5).Trim() | ConvertFrom-Json }
    }
}

try {
    $savedRaw = & $Fairyfly connection list --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the disposable saved connection before batch' }
    $saved = @(($savedRaw | ConvertFrom-Json).data.connections | Where-Object { $_.id -eq $ConnectionId })[0]
    if (-not $saved -or -not $saved.valid) { throw 'Disposable saved connection changed before batch' }
    $items = @(for ($i=0; $i -lt 20; $i++) {
        @{ tool='gui_screen_read'; arguments=@{ no_tabs=$true; max_rows=2 } }
    })
    $batchRequest = New-SseRequest 'gui_batch' @{ connection=$ConnectionId; items=$items } 1
    $batchResponse = $client.SendAsync($batchRequest,
        [Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
    if (-not $batchResponse.IsSuccessStatusCode -or
        $batchResponse.Content.Headers.ContentType.MediaType -ne 'text/event-stream') {
        throw 'The disposable-window batch was not accepted as an SSE call'
    }
    $batchReader = [IO.StreamReader]::new($batchResponse.Content.ReadAsStreamAsync().GetAwaiter().GetResult())
    do { $progress = Read-SseData $batchReader }
    while ($progress.method -eq 'notifications/progress' -and [int]$progress.params.progress -lt 1)
    if ($progress.method -ne 'notifications/progress' -or [int]$progress.params.progress -ne 1 -or
        [int]$progress.params.total -ne 20 -or
        [string]$progress.params.message -ne 'batch item completed') {
        throw 'The batch ended before its first item confirmed execution'
    }

    $queuedRequest = New-SseRequest 'gui_screen_read' @{
        connection=$ConnectionId; no_tabs=$true; max_rows=2
    } 2
    # Response headers are sent by http.sys only after HttpEndpoint::handle has
    # successfully submitted this request to the session lane.
    $queuedResponse = $client.SendAsync($queuedRequest,
        [Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
    if (-not $queuedResponse.IsSuccessStatusCode -or
        $queuedResponse.Content.Headers.ContentType.MediaType -ne 'text/event-stream') {
        throw 'The second read was not admitted to the session lane'
    }
    $queuedReader = [IO.StreamReader]::new($queuedResponse.Content.ReadAsStreamAsync().GetAwaiter().GetResult())
    $pending = $queuedReader.ReadLineAsync()
    $earlyCheck = [Diagnostics.Stopwatch]::StartNew()
    while ($earlyCheck.ElapsedMilliseconds -lt 250) {
        $remaining = [Math]::Max(1, 250 - [int]$earlyCheck.ElapsedMilliseconds)
        if (-not $pending.Wait($remaining)) { break }
        $line = $pending.GetAwaiter().GetResult()
        if ($null -eq $line) { throw 'Second read ended before the test window was closed' }
        if ($line.StartsWith('event:') -or $line.StartsWith('data:')) {
            throw 'The second read started or finished before the test window was closed'
        }
        $pending = $queuedReader.ReadLineAsync()
    }

    do { $laterProgress = Read-SseData $batchReader }
    while ($laterProgress.method -eq 'notifications/progress' -and [int]$laterProgress.params.progress -lt 2)
    if ($laterProgress.method -ne 'notifications/progress' -or
        [int]$laterProgress.params.progress -ne 2 -or
        [int]$laterProgress.params.total -ne 20 -or
        [string]$laterProgress.params.message -ne 'batch item completed') {
        throw 'The batch did not remain active after the second read was admitted'
    }
    while ($pending.IsCompleted) {
        $line = $pending.GetAwaiter().GetResult()
        if ($null -eq $line -or $line.StartsWith('event:') -or $line.StartsWith('data:')) {
            throw 'The second read started or finished before the test window was closed'
        }
        $pending = $queuedReader.ReadLineAsync()
    }
    $closedRaw = & $Fairyfly session disconnect --close-session --connection $ConnectionId --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not close the disposable SAP window' }
    $closed = $closedRaw | ConvertFrom-Json
    if ($closed.status -ne 'success' -or -not $closed.data.session_closed -or
        -not $closed.data.file_deleted -or $closed.data.session_id -ne $saved.session_id) {
        throw 'Disposable SAP window close could not be verified'
    }

    $final = $null
    while (-not $final) {
        if ($pending) {
            if (-not $pending.Wait(30000)) { throw 'Timed out waiting for queued read result' }
            $line = $pending.GetAwaiter().GetResult()
            $pending = $null
            if ($null -eq $line) { throw 'Queued read SSE stream ended unexpectedly' }
            if (-not $line.StartsWith('data:')) { continue }
            $event = $line.Substring(5).Trim() | ConvertFrom-Json
        } else {
            $event = Read-SseData $queuedReader
        }
        if ($event.id -eq 2) { $final = $event }
    }
    $screen = if ($final.result -and $final.result.content) {
        [string]$final.result.content[0].text
    } else { '' }
    $wholeMessage = $final | ConvertTo-Json -Depth 16 -Compress
    if ($wholeMessage.Contains($OwnMarker) -or $wholeMessage.Contains($OtherMarker) -or
        (-not $final.error -and -not $final.result.isError)) {
        throw 'Queued read returned SAP screen content after its window closed'
    }
    $code = if ($final.error) { [string]$final.error.data.code } else {
        [string]$final.result.structuredContent.error.code
    }
    $detail = $null
    if (-not $code -and $screen) {
        try {
            $detail = ($screen -split "`n")[-1] | ConvertFrom-Json
            $code = [string]$detail.code
        } catch {}
    }
    $withheldAfterClose = $code -eq 'OUTCOME_UNKNOWN' -and
        [string]$detail.message -eq 'the SAP session changed while the call was running; its result is withheld'
    if ($code -notin @('SESSION_ROUTE_CHANGED','OWNER_SESSION_UNAVAILABLE','SESSION_UNAVAILABLE','CONNECTION_NOT_FOUND') -and
        -not $withheldAfterClose) {
        throw "Queued read returned an unexpected code after window close: $code"
    }
    do { $batchFinal = Read-SseData $batchReader }
    while ($batchFinal.method -eq 'notifications/progress')
    if ($batchFinal.id -ne 1 -or (-not $batchFinal.error -and -not $batchFinal.result.isError)) {
        throw 'The first batch completed before the SAP window close took effect'
    }
    [pscustomobject]@{ queued_close_denied=$true; window_closed=$true; error_code=$code } |
        ConvertTo-Json -Compress
} finally {
    if ($queuedReader) { $queuedReader.Dispose() }
    if ($queuedResponse) { $queuedResponse.Dispose() }
    if ($queuedRequest) { $queuedRequest.Dispose() }
    if ($batchReader) { $batchReader.Dispose() }
    if ($batchResponse) { $batchResponse.Dispose() }
    if ($batchRequest) { $batchRequest.Dispose() }
    $client.Dispose()
}
