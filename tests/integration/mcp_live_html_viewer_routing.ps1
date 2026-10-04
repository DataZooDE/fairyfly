param(
    [int]$WorkbenchConnection = 2,
    [int]$WorkProcessConnection = 3,
    [string]$Endpoint = 'http://127.0.0.1:8383/mcp',
    [string]$Fairyfly = ''
)

# Prepare distinct read-only HTML viewer screens: SE80 on WorkbenchConnection and SM50 on WorkProcessConnection.
$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
if ($WorkbenchConnection -eq $WorkProcessConnection) { throw 'HTML viewer targets must be distinct saved connections' }
Add-Type -AssemblyName System.Net.Http
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(60)
$tokenNames = @()

function New-TestToken {
    $name = 'ffhtml' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $raw = & $exe mcp token create $name --scope session,screen --system A4H/001 --connections Bigfox --read-only --expires 1h --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not create HTML viewer test token' }
    $script:tokenNames += $name
    return ($raw | ConvertFrom-Json).data.token
}

function Start-Read([int]$connection, [string]$token) {
    $body = @{ jsonrpc='2.0'; id=$connection; method='tools/call'; params=@{
        name='gui_screen_read'; arguments=@{ connection=$connection; no_tabs=$true; max_rows=2 }
    } } | ConvertTo-Json -Depth 10 -Compress
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, $Endpoint)
    $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $token)
    $request.Headers.Accept.ParseAdd('application/json')
    $request.Content = [System.Net.Http.StringContent]::new($body, [Text.Encoding]::UTF8, 'application/json')
    return @{ Request=$request; Task=$client.SendAsync($request); Connection=$connection }
}

function Complete-Read($call, [string]$expected, [string]$foreign) {
    $response = $call.Task.GetAwaiter().GetResult()
    try {
        $body = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
        if (-not $response.IsSuccessStatusCode -or $body.error -or $body.result.isError) {
            throw "MCP HTML viewer read for connection $($call.Connection) failed: $($response.StatusCode)"
        }
        $text = ($body.result.content | Where-Object type -eq 'text' | ForEach-Object text) -join "`n"
        if (-not $text.Contains("connection $($call.Connection),") -or
            -not $text.Contains($expected) -or $text.Contains($foreign)) {
            throw "HTML viewer content was missing or crossed between windows for connection $($call.Connection)"
        }
    } finally {
        $response.Dispose()
        $call.Request.Dispose()
    }
}

try {
    $tokenA = New-TestToken
    $tokenB = New-TestToken
    $timer = [Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt 3; $i++) {
        $a = Start-Read $WorkbenchConnection $tokenA
        $b = Start-Read $WorkProcessConnection $tokenB
        Complete-Read $a 'Welcome to the ABAP Workbench' 'Total Number of Work Processes'
        Complete-Read $b 'Total Number of Work Processes' 'Welcome to the ABAP Workbench'
    }
    $timer.Stop()
    [pscustomobject]@{
        operation = 'concurrent owner-mode HTML viewer screen reads'
        connections = @($WorkbenchConnection, $WorkProcessConnection)
        calls_per_window = 3
        correct_window_content = $true
        elapsed_ms = $timer.ElapsedMilliseconds
    } | ConvertTo-Json -Compress
} finally {
    foreach ($name in $tokenNames) { & $exe mcp token delete $name --yes --output json | Out-Null }
    $client.Dispose()
}
