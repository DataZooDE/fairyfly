param(
    [int]$Connection = 2,
    [ValidateSet('revoke', 'rotate', 'delete')][string]$Mutation = 'revoke',
    [string]$Endpoint = 'http://127.0.0.1:8383/mcp',
    [string]$Fairyfly = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
Add-Type -AssemblyName System.Net.Http
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(30)
$names = @()

function New-TestToken {
    $name = 'fflease' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $raw = & $exe mcp token create $name --scope session.lease --system A4H/001 --connections Bigfox --read-only --expires 1h --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not create lease test token' }
    $script:names += $name
    return @{ Name=$name; Secret=($raw | ConvertFrom-Json).data.token }
}

function Acquire-Lease([string]$token, [int]$connection) {
    $body = @{ jsonrpc='2.0'; id=1; method='tools/call'; params=@{
        name='gui_session_lease'; arguments=@{ action='acquire'; connection=$connection }
    } } | ConvertTo-Json -Depth 10 -Compress
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, $Endpoint)
    $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $token)
    $request.Headers.Accept.ParseAdd('application/json')
    $request.Content = [System.Net.Http.StringContent]::new($body, [Text.Encoding]::UTF8, 'application/json')
    try {
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        try {
            $reply = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
            $reply | Add-Member -NotePropertyName http_status -NotePropertyValue ([int]$response.StatusCode)
            return $reply
        }
        finally { $response.Dispose() }
    } finally { $request.Dispose() }
}

function Acquire-VisibleLease([string]$token, [int]$connection) {
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    do {
        $reply = Acquire-Lease $token $connection
        if ($reply.http_status -ne 401) { return $reply }
        Start-Sleep -Milliseconds 250
    } while ($deadline.ElapsedMilliseconds -lt 7000)
    return $reply
}

try {
    $a = New-TestToken
    $b = New-TestToken
    $first = Acquire-VisibleLease $a.Secret $Connection
    if ($first.error -or $first.result.isError -or $first.result.structuredContent.status -ne 'granted') {
        throw "First token could not acquire its session lease: $($first | ConvertTo-Json -Depth 5 -Compress)"
    }
    $held = Acquire-VisibleLease $b.Secret $Connection
    if (-not $held.result.isError -or ($held.result.content[0].text -notmatch 'LEASE_HELD')) {
        throw 'Second token acquired a lease while the first token was still active'
    }
    if ($Mutation -eq 'delete') {
        & $exe mcp token delete $a.Name --yes --output json | Out-Null
    } else {
        & $exe mcp token $Mutation $a.Name --output json | Out-Null
    }
    if ($LASTEXITCODE -ne 0) { throw "Could not $Mutation first token" }
    $second = Acquire-VisibleLease $b.Secret $Connection
    if ($second.error -or $second.result.isError -or $second.result.structuredContent.status -ne 'granted') {
        throw "Token $Mutation still blocked the second client from acquiring the session lease"
    }
    [pscustomobject]@{
        operation = "owner-mode lease after cross-process token $Mutation"
        connection = $Connection
        active_holder_blocked_other_client = $true
        invalid_holder_released_on_next_acquire = $true
    } | ConvertTo-Json -Compress
} finally {
    foreach ($name in $names) { & $exe mcp token delete $name --yes --output json | Out-Null }
    $client.Dispose()
}
