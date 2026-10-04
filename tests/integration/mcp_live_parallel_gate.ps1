param(
    [Parameter(Mandatory = $true)][int]$ConnectionA,
    [Parameter(Mandatory = $true)][int]$ConnectionB,
    [int]$CallsPerSession = 6,
    [string]$IdentityA = '',
    [string]$IdentityB = '',
    [string]$ScreenMarkerA = '',
    [string]$ScreenMarkerB = '',
    [int]$TokenActivationDelayMs = 6000,
    [string]$Endpoint = 'http://127.0.0.1:8383/mcp',
    [string]$Fairyfly = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
if ($ConnectionA -eq $ConnectionB -or $CallsPerSession -lt 1 -or $CallsPerSession -gt 12 -or
    $TokenActivationDelayMs -lt 0 -or $TokenActivationDelayMs -gt 10000) {
    throw 'Provide two distinct connections and 1 to 12 calls per session'
}
if (($IdentityA -or $IdentityB) -and
    (-not $IdentityA -or -not $IdentityB -or $IdentityA -eq $IdentityB -or
     $IdentityA -cnotmatch '^[A-Z0-9]{3}/[0-9]{3}/[A-Z0-9_]+$' -or
     $IdentityB -cnotmatch '^[A-Z0-9]{3}/[0-9]{3}/[A-Z0-9_]+$')) {
    throw 'Provide two distinct exact SAP identities as SID/CLIENT/USER'
}
if ($IdentityA -and (($IdentityA -split '/')[0..1] -join '/') -ne (($IdentityB -split '/')[0..1] -join '/')) {
    throw 'The two SAP identities must share one SID/CLIENT to test SAP user isolation'
}
if ($IdentityA -and (-not $ScreenMarkerA -or -not $ScreenMarkerB -or
    $ScreenMarkerA.Length -lt 4 -or $ScreenMarkerB.Length -lt 4 -or
    $ScreenMarkerA.Contains($ScreenMarkerB) -or $ScreenMarkerB.Contains($ScreenMarkerA))) {
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
$client.Timeout = [TimeSpan]::FromSeconds(60)
$tokenNames = @()

function New-TestToken([string]$identity) {
    $name = 'fflive' + [Guid]::NewGuid().ToString('N')
    $script:tokenNames += $name
    $system = if ($identity) { ($identity -split '/')[0..1] -join '/' } else { 'A4H/001' }
    $options = @('mcp', 'token', 'create', $name, '--scope', 'session,connection,screen',
                 '--system', $system, '--connections', 'Bigfox', '--read-only', '--expires', '1h', '--output', 'json')
    if ($identity) { $options += @('--sap-identity', $identity) }
    $raw = & $exe @options
    if ($LASTEXITCODE -ne 0) { throw 'Could not create test token' }
    return ($raw | ConvertFrom-Json).data.token
}

function Call-Tool([string]$name, [hashtable]$arguments, [string]$token) {
    $body = @{ jsonrpc='2.0'; id=$name; method='tools/call'; params=@{
        name=$name; arguments=$arguments
    } } | ConvertTo-Json -Depth 10 -Compress
    return Invoke-RestMethod -Method Post -Uri $Endpoint -Headers @{
        Authorization = 'Bearer ' + $token; Accept = 'application/json'
    } -ContentType 'application/json' -Body $body -TimeoutSec 60
}

function Assert-OwnerBoundary([int]$own, [int]$other, [string]$token) {
    $listing = Call-Tool 'gui_connection_list' @{} $token
    if ($listing.error -or $listing.result.isError) { throw 'Owner connection discovery failed' }
    $data = $listing.result.structuredContent
    if (-not $data) { $data = $listing.result.content[0].text | ConvertFrom-Json }
    $rows = @($data.data.connections)
    if (-not @($rows | Where-Object { $_.id -eq $own }).Count -or
        @($rows | Where-Object { $_.id -eq $other }).Count) {
        throw 'Owner discovery did not isolate the two SAP identities'
    }
    $foreign = Call-Tool 'gui_screen_read' @{ connection=$other; no_tabs=$true; max_rows=2 } $token
    if ($foreign.error) {
        $denied = ($foreign.error | ConvertTo-Json -Depth 8 -Compress)
    } else {
        $denied = ($foreign.result | ConvertTo-Json -Depth 8 -Compress)
    }
    if (-not $foreign.result.isError -and -not $foreign.error) {
        throw 'A token read the other SAP user through a guessed connection ID'
    }
    if ($denied -notmatch 'OWNER_SESSION_UNAVAILABLE') {
        throw 'Foreign SAP user was refused with an unexpected result'
    }
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

function Complete-Reads($calls) {
    foreach ($call in $calls) {
        $response = $call.Task.GetAwaiter().GetResult()
        try {
            $body = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
            $screen = [string]$body.result.content[0].text
            $expectedMarker = if ($call.Connection -eq $ConnectionA) { $ScreenMarkerA } else { $ScreenMarkerB }
            $otherMarker = if ($call.Connection -eq $ConnectionA) { $ScreenMarkerB } else { $ScreenMarkerA }
            if (-not $response.IsSuccessStatusCode -or $body.error -or $body.result.isError -or
                -not $screen.Contains("connection $($call.Connection),") -or
                ($IdentityA -and (-not $screen.Contains($expectedMarker) -or $screen.Contains($otherMarker)))) {
                throw "MCP read came from the wrong SAP window or failed: connection $($call.Connection), $($response.StatusCode)"
            }
        } finally {
            $response.Dispose()
            $call.Request.Dispose()
        }
    }
}

try {
    $tokenA = New-TestToken $IdentityA
    $tokenB = New-TestToken $IdentityB
    if ($TokenActivationDelayMs) { Start-Sleep -Milliseconds $TokenActivationDelayMs }
    if ($IdentityA) {
        Assert-OwnerBoundary $ConnectionA $ConnectionB $tokenA
        Assert-OwnerBoundary $ConnectionB $ConnectionA $tokenB
    }
    $serial = [Diagnostics.Stopwatch]::StartNew()
    $baseline = @{}
    foreach ($target in @(@($ConnectionA, $tokenA), @($ConnectionB, $tokenB))) {
        $part = [Diagnostics.Stopwatch]::StartNew()
        for ($i = 0; $i -lt $CallsPerSession; $i++) {
            Complete-Reads @(Start-Read $target[0] $target[1])
        }
        $part.Stop()
        $baseline[[int]$target[0]] = $part.ElapsedMilliseconds
    }
    $serial.Stop()

    $different = [Diagnostics.Stopwatch]::StartNew()
    $calls = @()
    for ($i = 0; $i -lt $CallsPerSession; $i++) {
        $calls += Start-Read $ConnectionA $tokenA
        $calls += Start-Read $ConnectionB $tokenB
    }
    Complete-Reads $calls
    $different.Stop()
    $saving = $serial.ElapsedMilliseconds - $different.ElapsedMilliseconds
    $shorterBaseline = [Math]::Min($baseline[$ConnectionA], $baseline[$ConnectionB])
    if ($IdentityA -and ($saving -lt 50 -or $saving * 5 -lt $shorterBaseline)) {
        throw "Different SAP windows did not overlap enough: A=$($baseline[$ConnectionA])ms, B=$($baseline[$ConnectionB])ms, concurrent=$($different.ElapsedMilliseconds)ms"
    }

    $same = [Diagnostics.Stopwatch]::StartNew()
    $calls = @()
    for ($i = 0; $i -lt $CallsPerSession; $i++) {
        $calls += Start-Read $ConnectionA $tokenA
        $calls += Start-Read $ConnectionA $(if ($IdentityA) { $tokenA } else { $tokenB })
    }
    Complete-Reads $calls
    $same.Stop()

    [pscustomobject]@{
        operation = 'gui_screen_read through owner-mode HTTP MCP'
        connections = @($ConnectionA, $ConnectionB)
        calls_per_client = $CallsPerSession
        serial_ms = $serial.ElapsedMilliseconds
        baseline_a_ms = $baseline[$ConnectionA]
        baseline_b_ms = $baseline[$ConnectionB]
        different_session_ms = $different.ElapsedMilliseconds
        overlap_saving_ms = $saving
        same_session_ms = $same.ElapsedMilliseconds
        all_responses_targeted_correctly = [bool]$IdentityA
        two_sap_user_boundary_checked = [bool]$IdentityA
    } | ConvertTo-Json -Compress
} finally {
    $tokenCleanupFailed = $false
    foreach ($name in $tokenNames) {
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
    if ($tokenCleanupFailed) { throw 'Parallel probe could not remove all temporary MCP tokens' }
}
