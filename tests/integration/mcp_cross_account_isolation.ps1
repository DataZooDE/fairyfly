# Run from a controller that can reach two owner-mode tray endpoints. Start each
# tray under a different interactive Windows account with a broad read-only
# token created in that account. Supply the tokens through the controller's
# FAIRYFLY_OWNER_A_TOKEN and FAIRYFLY_OWNER_B_TOKEN environment variables.
# Use exactly one SAP window per account and distinct visible screen text.
param(
    [Parameter(Mandatory = $true)][string]$EndpointA,
    [Parameter(Mandatory = $true)][string]$EndpointB,
    [Parameter(Mandatory = $true)][int]$ConnectionA,
    [Parameter(Mandatory = $true)][int]$ConnectionB,
    [Parameter(Mandatory = $true)][string]$SessionIdA,
    [Parameter(Mandatory = $true)][string]$SessionIdB,
    [Parameter(Mandatory = $true)][string]$ScreenMarkerA,
    [Parameter(Mandatory = $true)][string]$ScreenMarkerB,
    [int]$TrayPidA,
    [int]$TrayPidB,
    [string]$FairyflyExe,
    [string]$CacheDirA,
    [string]$CacheDirB,
    [switch]$ProtocolOnly
)

$ErrorActionPreference = 'Stop'
if ($EndpointA -eq $EndpointB) { throw 'The owner endpoints must be distinct' }
if ($SessionIdA -eq $SessionIdB) { throw 'Use different SAP GUI session paths for this isolation gate' }
if ($ScreenMarkerA -eq $ScreenMarkerB) { throw 'The SAP screen markers must be distinct' }

function Assert-LocalEndpoint([string]$endpoint) {
    $uri = [Uri]$endpoint
    if (-not $uri.IsAbsoluteUri -or $uri.Host -ne '127.0.0.1' -or
        $uri.Scheme -notin @('http', 'https') -or
        $uri.AbsolutePath.TrimEnd('/') -ne '/mcp' -or $uri.UserInfo -or $uri.Query -or $uri.Fragment) {
        throw 'Run this probe on the shared Windows host against loopback /mcp endpoints only'
    }
    return $uri
}

$uriA = Assert-LocalEndpoint $EndpointA
$uriB = Assert-LocalEndpoint $EndpointB
if ($uriA.AbsoluteUri.TrimEnd('/') -eq $uriB.AbsoluteUri.TrimEnd('/')) {
    throw 'The owner endpoints must have different registered URLs'
}

function Get-EndpointOwner([Uri]$uri, [int]$trayPid, [string]$exePath) {
    if ($trayPid -le 0) { throw 'Supply both live Fairyfly tray process IDs for account proof' }
    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$trayPid"
    if ($null -eq $process -or $process.Name -ine 'fairyfly.exe' -or $process.SessionId -le 0 -or
        $process.ExecutablePath -ine $exePath) {
        throw 'A supplied tray process is absent, is not Fairyfly, or is not interactive'
    }
    $owner = $process | Invoke-CimMethod -MethodName GetOwnerSid
    if ($owner.ReturnValue -ne 0 -or [string]::IsNullOrWhiteSpace($owner.Sid)) {
        throw 'Could not verify a tray process owner SID'
    }
    $state = (& netsh http show servicestate view=requestq verbose=yes | Out-String)
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect HTTP.sys request queues' }
    # netsh writes an IP-specific http.sys URL as HOST:PORT:IP/PATH; the
    # nested URL-group "Request queue name" line is indented and must not
    # start a new top-level queue block.
    $prefix = [regex]::Escape("$($uri.Scheme)://") + '(?:127\.0\.0\.1|\+|\*):' +
        $uri.Port + '(?::127\.0\.0\.1)?' + [regex]::Escape($uri.AbsolutePath.TrimEnd('/') + '/')
    $matched = $false
    foreach ($queue in [regex]::Split($state, '(?m)^Request queue name:\s*')) {
        if (-not [regex]::IsMatch($queue, '(?im)^\s*' + $prefix + '\s*$')) { continue }
        $ids = [regex]::Match($queue, 'Process IDs:\s*(.*?)\s*URL groups:', 'Singleline')
        $controller = [regex]::Match($queue, 'Controller process ID:\s*(\d+)')
        if (($ids.Success -and $ids.Groups[1].Value -match "\b$trayPid\b") -or
            ($controller.Success -and $controller.Groups[1].Value -eq [string]$trayPid)) {
            $matched = $true
            break
        }
    }
    if (-not $matched) { throw "The endpoint URL $uri is not registered to the supplied Fairyfly process in HTTP.sys" }
    return [pscustomobject]@{ Sid=$owner.Sid; Created=$process.CreationDate }
}

$proofA = $null
$proofB = $null
if (-not $ProtocolOnly) {
    if ([string]::IsNullOrWhiteSpace($FairyflyExe) -or $CacheDirA -or $CacheDirB) {
        throw 'Live mode requires -FairyflyExe and derives both cache directories from the owner profiles'
    }
    $exePath = (Resolve-Path -LiteralPath $FairyflyExe).Path
    $proofA = Get-EndpointOwner $uriA $TrayPidA $exePath
    $proofB = Get-EndpointOwner $uriB $TrayPidB $exePath
    if ($proofA.Sid -eq $proofB.Sid) { throw 'Both endpoints run under the same Windows account' }
    $profileA = Get-CimInstance Win32_UserProfile -Filter "SID='$($proofA.Sid)'"
    $profileB = Get-CimInstance Win32_UserProfile -Filter "SID='$($proofB.Sid)'"
    if ($null -eq $profileA -or $null -eq $profileB) { throw 'Could not resolve both Windows user profiles' }
    $CacheDirA = Join-Path $profileA.LocalPath 'AppData\Local\fairyfly\sessions'
    $CacheDirB = Join-Path $profileB.LocalPath 'AppData\Local\fairyfly\sessions'
}

function Get-CacheSnapshot([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Container)) {
        throw 'The account-local Fairyfly session cache directory is unavailable'
    }
    $files = @(Get-ChildItem -LiteralPath $path -Filter 'fairyfly.*.con' -File | Sort-Object Name)
    if ($files.Count -lt 1) { throw 'The account-local Fairyfly session cache has no saved connection' }
    return (($files | ForEach-Object {
        $_.Name + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }) -join '|')
}

$checkCache = -not [string]::IsNullOrWhiteSpace($CacheDirA) -and
    -not [string]::IsNullOrWhiteSpace($CacheDirB)
if ($ProtocolOnly -and ([bool]$CacheDirA -ne [bool]$CacheDirB)) {
    throw 'Supply both cache directories or neither in protocol-only mode'
}
$tokenA = [Environment]::GetEnvironmentVariable('FAIRYFLY_OWNER_A_TOKEN')
$tokenB = [Environment]::GetEnvironmentVariable('FAIRYFLY_OWNER_B_TOKEN')
$attachTokenA = [Environment]::GetEnvironmentVariable('FAIRYFLY_OWNER_A_ATTACH_TOKEN')
$attachTokenB = [Environment]::GetEnvironmentVariable('FAIRYFLY_OWNER_B_ATTACH_TOKEN')
if ([string]::IsNullOrWhiteSpace($tokenA) -or [string]::IsNullOrWhiteSpace($tokenB) -or $tokenA -eq $tokenB) {
    throw 'Set two distinct FAIRYFLY_OWNER_A_TOKEN and FAIRYFLY_OWNER_B_TOKEN values in the controller environment'
}
if ([string]::IsNullOrWhiteSpace($attachTokenA) -or [string]::IsNullOrWhiteSpace($attachTokenB) -or
    $attachTokenA -eq $attachTokenB -or $attachTokenA -eq $tokenA -or $attachTokenB -eq $tokenB) {
    throw 'Set separate scoped FAIRYFLY_OWNER_A_ATTACH_TOKEN and FAIRYFLY_OWNER_B_ATTACH_TOKEN values'
}
Add-Type -AssemblyName System.Net.Http
$handler = [System.Net.Http.HttpClientHandler]::new()
$handler.AllowAutoRedirect = $false
$client = [System.Net.Http.HttpClient]::new($handler)
$client.Timeout = [TimeSpan]::FromSeconds(30)

function Invoke-Tool([string]$endpoint, [string]$token, [string]$tool, $arguments) {
    $body = @{ jsonrpc='2.0'; id=1; method='tools/call'; params=@{
        name=$tool; arguments=$arguments
    } } | ConvertTo-Json -Depth 12 -Compress
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, $endpoint)
    $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $token)
    $request.Headers.Accept.ParseAdd('application/json')
    $request.Content = [System.Net.Http.StringContent]::new($body, [Text.Encoding]::UTF8, 'application/json')
    try {
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        try {
            return [pscustomobject]@{
                Status = [int]$response.StatusCode
                Body = ($response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json)
            }
        } finally { $response.Dispose() }
    } finally { $request.Dispose() }
}

function Require-Success($response, [string]$what) {
    if ($response.Status -ne 200 -or $response.Body.error -or $response.Body.result.isError) {
        throw "$what failed with HTTP $($response.Status)"
    }
    return [string]$response.Body.result.content[0].text
}

try {
    foreach ($side in @(
        @{ Name='A'; Endpoint=$EndpointA; Token=$tokenA; AttachToken=$attachTokenA; Cache=$CacheDirA;
           Own=$ConnectionA; Other=$ConnectionB;
           Session=$SessionIdA; ForeignSession=$SessionIdB;
           Marker=$ScreenMarkerA; Foreign=$ScreenMarkerB },
        @{ Name='B'; Endpoint=$EndpointB; Token=$tokenB; AttachToken=$attachTokenB; Cache=$CacheDirB;
           Own=$ConnectionB; Other=$ConnectionA;
           Session=$SessionIdB; ForeignSession=$SessionIdA;
           Marker=$ScreenMarkerB; Foreign=$ScreenMarkerA }
    )) {
        $sessions = Invoke-Tool $side.Endpoint $side.Token 'gui_session_list' @{}
        [void](Require-Success $sessions "$($side.Name) session list")
        $listed = $sessions.Body.result.structuredContent.data
        if ($null -eq $listed -or $listed.total_sessions -ne 1 -or
            $listed.connections.Count -ne 1 -or $listed.connections[0].sessions.Count -ne 1 -or
            $listed.connections[0].sessions[0].id -ne $side.Session) {
            throw "$($side.Name) session list did not contain exactly its own window"
        }
        $connections = Invoke-Tool $side.Endpoint $side.Token 'gui_connection_list' @{}
        if ($connections.Status -eq 200 -and -not $connections.Body.result.isError) {
            throw "$($side.Name) connection list exposed saved connection metadata"
        }
        if (-not ([string]$connections.Body.result.content[0].text).Contains('OWNER_IDENTITY_UNKNOWN')) {
            throw "$($side.Name) connection list failed for an unexpected reason"
        }
        $own = Require-Success (Invoke-Tool $side.Endpoint $side.Token 'gui_screen_read' @{
            connection=$side.Own; no_tabs=$true; max_rows=2
        }) "$($side.Name) own-window read"
        if (-not $own.Contains($side.Marker) -or $own.Contains($side.Foreign)) {
            throw "$($side.Name) screen read did not return its own SAP window"
        }
        $guessed = Invoke-Tool $side.Endpoint $side.Token 'gui_screen_read' @{
            connection=$side.Other; no_tabs=$true; max_rows=2
        }
        if ($guessed.Status -eq 200 -and -not $guessed.Body.error -and -not $guessed.Body.result.isError) {
            $guessedText = [string]$guessed.Body.result.content[0].text
            if (-not $guessedText.Contains($side.Marker) -or $guessedText.Contains($side.Foreign)) {
                throw "$($side.Name) guessed connection ID returned an unexpected window"
            }
        }
        $beforeCache = if ($checkCache) { Get-CacheSnapshot $side.Cache } else { $null }
        $attach = Invoke-Tool $side.Endpoint $side.AttachToken 'gui_session_attach' @{
            session_id=$side.ForeignSession
        }
        if ($checkCache -and (Get-CacheSnapshot $side.Cache) -ne $beforeCache) {
            throw "$($side.Name) saved connection changed during foreign session attach"
        }
        if ($attach.Status -ne 200 -or -not $attach.Body.result.isError -or
            -not ([string]$attach.Body.result.content[0].text -match
                'OWNER_IDENTITY_UNKNOWN|OWNER_IDENTITY_DENIED|OWNER_SESSION_UNAVAILABLE')) {
            throw "$($side.Name) foreign session attach was not denied by owner identity"
        }
    }
    $crossA = Invoke-Tool $EndpointB $tokenA 'gui_session_list' @{}
    $crossB = Invoke-Tool $EndpointA $tokenB 'gui_session_list' @{}
    if ($crossA.Status -ne 401 -or $crossB.Status -ne 401) {
        throw 'A token from one owner was accepted by the other owner endpoint'
    }
    if (-not $ProtocolOnly) {
        $afterA = Get-EndpointOwner $uriA $TrayPidA $exePath
        $afterB = Get-EndpointOwner $uriB $TrayPidB $exePath
        if ($afterA.Sid -ne $proofA.Sid -or $afterB.Sid -ne $proofB.Sid -or
            $afterA.Created -ne $proofA.Created -or $afterB.Created -ne $proofB.Created) {
            throw 'An endpoint process changed while the probe was running'
        }
    }
    [pscustomobject]@{
        account_endpoints_isolated = (-not $ProtocolOnly)
        protocol_checks_passed = $true
        own_windows_readable = $true
        owner_session_lists_isolated = $true
        connection_lists_withheld = $true
        foreign_tokens_rejected = $true
        guessed_numeric_ids_checked = $true
        foreign_session_attach_denied = $true
        attach_cache_unchanged = $checkCache
    } | ConvertTo-Json -Compress
} finally {
    $client.Dispose()
}
