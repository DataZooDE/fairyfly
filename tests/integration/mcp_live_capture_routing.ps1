# Prepare visibly distinct screens first (for example SM37 and SAP Easy Access), then start an owner-mode tray.
param(
    [int]$ConnectionA = 2,
    [int]$ConnectionB = 3,
    [string]$Endpoint = 'http://127.0.0.1:8383/mcp',
    [string]$Fairyfly = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
if ($ConnectionA -eq $ConnectionB) { throw 'Capture targets must be distinct saved connections' }
Add-Type -AssemblyName System.Net.Http
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(60)
$tokenNames = @()

function New-TestToken {
    $name = 'ffcapture' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $raw = & $exe mcp token create $name --scope session,screen --system A4H/001 --connections Bigfox --read-only --expires 1h --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not create capture test token' }
    $script:tokenNames += $name
    return ($raw | ConvertFrom-Json).data.token
}

function Start-Capture([int]$connection, [string]$token) {
    $body = @{ jsonrpc='2.0'; id=$connection; method='tools/call'; params=@{
        name='gui_screen_capture'; arguments=@{ connection=$connection; scale=0.5 }
    } } | ConvertTo-Json -Depth 10 -Compress
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, $Endpoint)
    $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $token)
    $request.Headers.Accept.ParseAdd('application/json')
    $request.Content = [System.Net.Http.StringContent]::new($body, [Text.Encoding]::UTF8, 'application/json')
    return @{ Request=$request; Task=$client.SendAsync($request); Connection=$connection }
}

function Complete-Capture($call) {
    $response = $call.Task.GetAwaiter().GetResult()
    try {
        $body = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
        if (-not $response.IsSuccessStatusCode -or $body.error -or $body.result.isError -or
            $body.result.content[0].type -ne 'image' -or $body.result.content[0].mimeType -ne 'image/png') {
            throw "MCP capture for connection $($call.Connection) failed: $($response.StatusCode) $($body.result.content[-1].text)"
        }
        $bytes = [Convert]::FromBase64String($body.result.content[0].data)
        if ($bytes.Length -lt 1000) { throw "Capture for connection $($call.Connection) is unexpectedly small" }
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = [BitConverter]::ToString($sha.ComputeHash($bytes)) }
        finally { $sha.Dispose() }
        return @{ Connection=$call.Connection; Hash=$hash; Bytes=$bytes.Length; Caption=$body.result.content[1].text }
    } finally {
        $response.Dispose()
        $call.Request.Dispose()
    }
}

try {
    $tokenA = New-TestToken
    $tokenB = New-TestToken
    $a = Start-Capture $ConnectionA $tokenA
    $b = Start-Capture $ConnectionB $tokenB
    $captureA = Complete-Capture $a
    $captureB = Complete-Capture $b
    if ($captureA.Hash -eq $captureB.Hash) {
        throw 'The two windows returned identical screenshot bytes; use visibly distinct screens to prove targeting'
    }
    [pscustomobject]@{
        operation = 'concurrent owner-mode gui_screen_capture'
        connections = @($ConnectionA, $ConnectionB)
        image_bytes = @($captureA.Bytes, $captureB.Bytes)
        images_distinct = $true
        captions = @($captureA.Caption, $captureB.Caption)
    } | ConvertTo-Json -Compress
} finally {
    foreach ($name in $tokenNames) { & $exe mcp token delete $name --yes --output json | Out-Null }
    $client.Dispose()
}
