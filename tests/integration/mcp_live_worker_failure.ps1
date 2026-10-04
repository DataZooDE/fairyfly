# Run against a freshly started test tray: the first warm read must be the
# only private worker so the script can identify it without guessing.
param(
    [Parameter(Mandatory = $true)][int]$InterruptedConnection,
    [Parameter(Mandatory = $true)][int]$OtherConnection,
    [Parameter(Mandatory = $true)][int]$TrayPid,
    [switch]$AllowBetweenItems,
    [ValidateRange(0, 500)][int]$DelayAfterFirstMs = 120,
    [string]$Endpoint = 'http://127.0.0.1:8383/mcp',
    [string]$Fairyfly = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
if ($InterruptedConnection -eq $OtherConnection) { throw 'Two distinct connections are required' }
Add-Type -AssemblyName System.Net.Http
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$tray = Get-CimInstance Win32_Process -Filter "ProcessId = $TrayPid"
if (-not $tray -or $tray.ExecutablePath -ne $exe -or $tray.CommandLine -notlike '*--tray*') {
    throw 'The specified process is not this Fairyfly tray executable'
}
$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(90)
$names = @()

function New-TestToken([string]$scopes) {
    $name = 'fflive' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $raw = & $exe mcp token create $name --scope $scopes --system A4H/001 --connections Bigfox --read-only --expires 1h --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not create test token' }
    $script:names += $name
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
    try { return ($response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json) }
    finally { $response.Dispose(); $call.Request.Dispose() }
}

function Start-ProgressBatch($arguments, [string]$token) {
    $body = @{ jsonrpc='2.0'; id=2; method='tools/call'; params=@{
        name='gui_batch'; arguments=$arguments; _meta=@{ progressToken='worker-interruption-gate' }
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
        if ($eventBody.method -eq 'notifications/progress' -or $eventBody.id -eq 2) { return $eventBody }
    }
}

function Require-Read($reply, [int]$connection) {
    if ($reply.error -or $reply.result.isError -or
        -not $reply.result.content[0].text.Contains("connection $connection,")) {
        throw "The read of connection $connection did not succeed"
    }
}

try {
    $slowToken = New-TestToken 'batch,screen'
    $otherToken = New-TestToken 'screen'
    Require-Read (Finish-Call (Start-Call 'gui_screen_read' @{
        connection=$InterruptedConnection; no_tabs=$true; max_rows=2
    } $slowToken 1)) $InterruptedConnection
    $workers = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $TrayPid" |
        Where-Object { $_.ExecutablePath -eq $exe -and $_.CommandLine -like '*--mcp-session-worker*' })
    if ($workers.Count -ne 1) { throw "Expected one private worker after the warm read; found $($workers.Count)" }
    $workerPid = [int]$workers[0].ProcessId

    $items = @(for ($i = 0; $i -lt 20; $i++) {
        @{ tool='gui_screen_read'; arguments=@{ no_tabs=$true; max_rows=2 } }
    })
    $slow = Start-ProgressBatch @{ connection=$InterruptedConnection; items=$items } $slowToken
    $eventBody = Read-ProgressEvent $slow
    while ($eventBody.method -eq 'notifications/progress' -and $eventBody.params.progress -lt 1) {
        $eventBody = Read-ProgressEvent $slow
    }
    if ($eventBody.method -ne 'notifications/progress' -or $eventBody.params.progress -ne 1) {
        throw 'The first batch item did not complete before worker interruption'
    }
    if ($DelayAfterFirstMs -gt 0) { Start-Sleep -Milliseconds $DelayAfterFirstMs }
    $worker = Get-CimInstance Win32_Process -Filter "ProcessId = $workerPid"
    if (-not $worker -or $worker.ParentProcessId -ne $TrayPid -or
        $worker.ExecutablePath -ne $exe -or $worker.CommandLine -notlike '*--mcp-session-worker*') {
        throw 'Private worker identity changed before interruption'
    }
    Stop-Process -Id $workerPid -ErrorAction Stop
    $interrupted = Read-ProgressEvent $slow
    while ($interrupted.method -eq 'notifications/progress') { $interrupted = Read-ProgressEvent $slow }
    $code = if ($interrupted.error) { $interrupted.error.data.code }
        elseif ($interrupted.result.structuredContent.error) { $interrupted.result.structuredContent.error.code }
        elseif ($interrupted.result.isError -and
                $interrupted.result.content[0].text.Contains('"error_code":"OUTCOME_UNKNOWN"')) { 'OUTCOME_UNKNOWN' }
        elseif ($interrupted.result.isError -and
                $interrupted.result.content[0].text.Contains('"error_code":"OWNER_SESSION_UNAVAILABLE"')) { 'OWNER_SESSION_UNAVAILABLE' }
        else { '' }
    if ($code -notin @('OUTCOME_UNKNOWN', 'OWNER_SESSION_UNAVAILABLE') -or
        (-not $AllowBetweenItems -and $code -ne 'OUTCOME_UNKNOWN')) {
        $summary = if ($interrupted.result.content) {
            ($interrupted.result.content[0].text -split "`n---", 2)[0]
        } else { '' }
        throw "Interrupted call returned $code instead of the required worker-failure result; isError=$($interrupted.result.isError); summary=$summary"
    }

    Require-Read (Finish-Call (Start-Call 'gui_screen_read' @{
        connection=$OtherConnection; no_tabs=$true; max_rows=2
    } $otherToken 3)) $OtherConnection
    Require-Read (Finish-Call (Start-Call 'gui_screen_read' @{
        connection=$InterruptedConnection; no_tabs=$true; max_rows=2
    } $slowToken 4)) $InterruptedConnection
    [pscustomobject]@{
        interrupted_connection = $InterruptedConnection
        other_connection = $OtherConnection
        interrupted_code = $code
        in_flight_action_result_unknown = ($code -eq 'OUTCOME_UNKNOWN')
        other_session_read_succeeded = $true
        interrupted_session_recovered = $true
    } | ConvertTo-Json -Compress
} finally {
    if ($slow) { $slow.Reader.Dispose(); $slow.Response.Dispose(); $slow.Request.Dispose() }
    foreach ($name in $names) { & $exe mcp token delete $name --yes --output json | Out-Null }
    $client.Dispose()
}
