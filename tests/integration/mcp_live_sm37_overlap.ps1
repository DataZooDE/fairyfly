param(
    [Parameter(Mandatory = $true)][int]$SelectionConnection,
    [Parameter(Mandatory = $true)][int]$ReaderConnection,
    [string]$Endpoint = 'http://127.0.0.1:8383/mcp',
    [string]$Fairyfly = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
if ($SelectionConnection -eq $ReaderConnection) { throw 'Two distinct connections are required' }
Add-Type -AssemblyName System.Net.Http
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$screen = (& $exe screen read --connection $SelectionConnection --only-fields --no-tabs --output json | ConvertFrom-Json).data
if ($LASTEXITCODE -ne 0 -or $screen.transaction -ne 'SM37' -or $screen.title -ne 'Simple Job Selection') {
    throw 'The slow connection must show SM37 Simple Job Selection'
}
$fields = @{}
foreach ($field in $screen.elements) { $fields[$field.name] = $field.text.Trim() }
$job = $fields['BTCH2170-JOBNAME']
$user = $fields['BTCH2170-USERNAME']
$from = $fields['BTCH2170-FROM_DATE']
$to = $fields['BTCH2170-TO_DATE']
if (-not $job -or $job.Contains('*') -or -not $user -or $user.Contains('*') -or
    -not $from -or $from -ne $to) {
    throw 'SM37 requires an exact job and user and one exact date before F8'
}

$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(45)
$names = @()
$lease = ''
$slowToken = ''
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
    } } | ConvertTo-Json -Depth 10 -Compress
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
try {
    $slowToken = New-TestToken 'session.lease,key'
    $readerToken = New-TestToken 'screen'
    $granted = Finish-Call (Start-Call 'gui_session_lease' @{
        action='acquire'; connection=$SelectionConnection
    } $slowToken 1)
    if ($granted.error -or $granted.result.isError) {
        $code = if ($granted.error) { $granted.error.data.code } else { $granted.result.structuredContent.error.code }
        $detail = if ($granted.result.content) { $granted.result.content[0].text } else { $granted.error.message }
        throw "Could not acquire the selection session lease: $code $detail"
    }
    $lease = $granted.result.structuredContent.lease_id
    if (-not $lease) { throw 'Lease response omitted its ID' }

    $timer = [Diagnostics.Stopwatch]::StartNew()
    $slow = Start-Call 'gui_key_send' @{
        connection=$SelectionConnection; key='f8'; lease_id=$lease
    } $slowToken 2
    Start-Sleep -Milliseconds 100
    $readerStarted = $timer.ElapsedMilliseconds
    $reader = Start-Call 'gui_screen_read' @{
        connection=$ReaderConnection; no_tabs=$true; max_rows=2
    } $readerToken 3
    $readResult = Finish-Call $reader
    $readerFinished = $timer.ElapsedMilliseconds
    $readerBeforeSlow = -not $slow.Task.IsCompleted
    $slowResult = Finish-Call $slow
    $slowFinished = $timer.ElapsedMilliseconds
    if ($readResult.error -or $readResult.result.isError -or
        -not $readResult.result.content[0].text.Contains("connection $ReaderConnection,")) {
        throw 'Other-window read failed or reached the wrong window'
    }
    if ($slowResult.error -or $slowResult.result.isError) { throw 'Exact SM37 query failed' }
    [pscustomobject]@{
        operation = 'F8 on exact one-day SM37 job selection through tray MCP'
        job = $job
        user = $user
        date = $from
        reader_started_ms = $readerStarted
        reader_finished_ms = $readerFinished
        f8_finished_ms = $slowFinished
        reader_finished_before_f8 = $readerBeforeSlow
    } | ConvertTo-Json -Compress
} finally {
    if ($lease -and $slowToken) {
        try { Finish-Call (Start-Call 'gui_session_lease' @{
            action='release'; connection=$SelectionConnection; lease_id=$lease
        } $slowToken 4) | Out-Null } catch {}
    }
    foreach ($name in $names) { & $exe mcp token delete $name --yes --output json | Out-Null }
    $client.Dispose()
}
