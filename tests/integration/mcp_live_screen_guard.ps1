param(
    [int]$Connection = 2,
    [string]$Endpoint = 'http://127.0.0.1:8383/mcp',
    [string]$Fairyfly = ''
)

# Run against an owner-mode tray while the saved session is at SAP Easy Access.
$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
Add-Type -AssemblyName System.Net.Http
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(40)
$name = 'ffguard' + [Guid]::NewGuid().ToString('N').Substring(0, 8)

function Invoke-Tool([string]$tool, $arguments, [string]$token, [int]$id) {
    $body = @{ jsonrpc='2.0'; id=$id; method='tools/call'; params=@{
        name=$tool; arguments=$arguments
    } } | ConvertTo-Json -Depth 10 -Compress
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, $Endpoint)
    $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $token)
    $request.Headers.Accept.ParseAdd('application/json')
    $request.Content = [System.Net.Http.StringContent]::new($body, [Text.Encoding]::UTF8, 'application/json')
    try {
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        try { return ($response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json) }
        finally { $response.Dispose() }
    } finally { $request.Dispose() }
}

try {
    $raw = & $exe mcp token create $name --scope screen,session.lease --system A4H/001 --connections Bigfox --read-only --expires 1h --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not create screen guard test token' }
    $token = ($raw | ConvertFrom-Json).data.token
    $observed = Invoke-Tool 'gui_screen_read' @{ connection=$Connection; no_tabs=$true; max_rows=2 } $token 1
    if ($observed.error -or $observed.result.isError) { throw 'Initial owner-mode screen read failed' }
    $guard = $observed.result.structuredContent.screen_guard
    if ($guard -notmatch '^[0-9a-f]{64}$') { throw 'Initial screen read did not return a valid screen guard' }
    $lease = Invoke-Tool 'gui_session_lease' @{ action='acquire'; connection=$Connection } $token 2
    if ($lease.error -or $lease.result.isError -or $lease.result.structuredContent.status -ne 'granted') {
        throw 'Could not acquire the navigation lease'
    }
    $id = $lease.result.structuredContent.lease_id
    $same = Invoke-Tool 'gui_screen_read' @{
        connection=$Connection; max_rows=2; lease_id=$id; expected_screen_guard=$guard
    } $token 3
    if ($same.error -or $same.result.isError) { throw 'Matching screen guard rejected a same-screen call' }

    & $exe transaction start SM37 --connection $Connection --output json | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not navigate to safe SM37 test screen' }
    $stale = Invoke-Tool 'gui_screen_read' @{
        connection=$Connection; max_rows=2; lease_id=$id; expected_screen_guard=$guard
    } $token 4
    if (-not $stale.result.isError -or $stale.result.content[0].text -notmatch 'SCREEN_CHANGED') {
        throw 'An obsolete screen guard did not reject the changed SAP dynpro'
    }
    [pscustomobject]@{
        operation = 'owner-mode screen precondition'
        connection = $Connection
        matching_guard_accepted = $true
        stale_guard_rejected = $true
    } | ConvertTo-Json -Compress
} finally {
    & $exe transaction start S000 --connection $Connection --output json | Out-Null
    & $exe mcp token delete $name --yes --output json | Out-Null
    $client.Dispose()
}
