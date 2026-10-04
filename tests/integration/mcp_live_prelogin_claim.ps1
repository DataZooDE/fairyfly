param(
    [int]$Port = 8391,
    [string]$Fairyfly = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$endpoint = "http://127.0.0.1:$Port/mcp"
$cfgPath = Join-Path $env:TEMP ('fairyfly_prelogin_' + [Guid]::NewGuid().ToString('N') + '.yaml')
$suffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$tokens = @()
$trayProcessId = $null
$connectionId = $null
$cleanupIds = @()
$savedFingerprints = @{}
$ownerLoggedIn = $false
$ownerScreenGuard = ''
$testResult = $null
$cleanupFailures = @()
$unfinishedClosed = $false

function Saved-Connection([int]$savedId) {
    $listing = & $exe connection list --output json
    if ($LASTEXITCODE -ne 0) { return $null }
    return @(($listing | ConvertFrom-Json).data.connections | Where-Object { $_.id -eq $savedId })[0]
}

function Call-Tool([string]$name, [hashtable]$arguments, [string]$bearer) {
    $request = @{ jsonrpc = '2.0'; id = $name; method = 'tools/call'; params = @{
        name = $name; arguments = $arguments
    } } | ConvertTo-Json -Depth 12 -Compress
    $response = Invoke-RestMethod -Method Post -Uri $endpoint -Headers @{
        Authorization = 'Bearer ' + $bearer; Accept = 'application/json'
    } -ContentType 'application/json' -Body $request -TimeoutSec 120
    if ($response.error) {
        $routeCode = if ($response.error.data.code) { [string]$response.error.data.code }
            else { [string]$response.error.message }
        return [pscustomobject]@{ status = 'error'; error = [pscustomobject]@{ code = $routeCode } }
    }
    if ($response.result.structuredContent) { return $response.result.structuredContent }
    $plain = [string]$response.result.content[0].text
    if ($response.result.isError) {
        $detail = ($plain -split "`n")[-1] | ConvertFrom-Json
        return [pscustomobject]@{ status = 'error'; error = $detail }
    }
    try { return $plain | ConvertFrom-Json } catch { throw "Unexpected $name MCP response: $plain" }
}

try {
    if (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
        throw "$endpoint is already in use"
    }
    @('server:', '  transport: http', '  host: 127.0.0.1', "  port: $Port",
      'owner:', '  sap_identities: [A4H/001/DEVELOPER, A4H/001/OTHER]') |
        Set-Content -LiteralPath $cfgPath -Encoding ascii

    $issuedA = & $exe mcp token create "ffpreA$suffix" --scope session,session.lease,connection,screen `
        --sap-identity A4H/001/DEVELOPER --system A4H/001 --connections Bigfox --expires 1h --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the DEVELOPER token' }
    $tokens += "ffpreA$suffix"
    $bearerA = ($issuedA | ConvertFrom-Json).data.token
    $issuedB = & $exe mcp token create "ffpreB$suffix" --scope session,connection,screen `
        --sap-identity A4H/001/OTHER --system A4H/001 --connections Bigfox --expires 1h --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the OTHER token' }
    $tokens += "ffpreB$suffix"
    $bearerB = ($issuedB | ConvertFrom-Json).data.token
    if (-not $bearerA -or -not $bearerB) { throw 'Token secret missing' }

    $launch = & $exe mcp --http --tray --allow-write --config $cfgPath
    if ($LASTEXITCODE -ne 0 -or ($launch | Out-String) -notmatch 'pid\s+(\d+)') {
        throw 'Could not launch the tray'
    }
    $trayProcessId = [int]$Matches[1]
    $ready = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        try {
            $tcp = [Net.Sockets.TcpClient]::new()
            $tcp.Connect('127.0.0.1', $Port)
            $tcp.Dispose()
            $ready = $true
            break
        } catch { Start-Sleep -Milliseconds 200 }
    }
    if (-not $ready) { throw 'The tray did not start listening' }

    $opened = Call-Tool 'gui_session_launch' @{ name = 'Bigfox'; login = $false } $bearerA
    if ($opened.status -ne 'success' -or $null -eq $opened.data.connection_file_id) {
        throw ('Prelogin launch failed: ' + ($opened | ConvertTo-Json -Depth 8 -Compress))
    }
    $connectionId = [int]$opened.data.connection_file_id
    $cleanupIds += $connectionId
    $other = Call-Tool 'gui_session_login' @{ connection = $connectionId; credential = 'Bigfox' } $bearerB
    if ($other.status -ne 'error' -or $other.error.code -ne 'OWNER_SESSION_UNAVAILABLE') {
        throw 'The other token was not denied before credential use'
    }
    $owner = Call-Tool 'gui_session_login' @{ connection = $connectionId; credential = 'Bigfox' } $bearerA
    if ($owner.status -ne 'success') {
        throw ('Launching token could not finish login: ' + ($owner | ConvertTo-Json -Depth 8 -Compress))
    }
    $ownerLoggedIn = $true
    $savedFingerprints[$connectionId] = Saved-Connection $connectionId
    $screenGuardResult = Call-Tool 'gui_screen_read' @{ connection = $connectionId; no_tabs = $true } $bearerA
    $ownerScreenGuard = [string]$screenGuardResult.screen_guard
    if ($ownerScreenGuard -notmatch '^[0-9a-fA-F]{64}$') { throw 'Owner screen guard is unavailable' }
    $listed = Call-Tool 'gui_connection_list' @{} $bearerA
    if ($listed.status -ne 'success' -or
        -not @($listed.data.connections | Where-Object { $_.id -eq $connectionId }).Count) {
        throw 'Authenticated owner could not discover its SAP window'
    }
    $unfinished = Call-Tool 'gui_session_launch' @{ name = 'Bigfox'; login = $false } $bearerA
    if ($unfinished.status -ne 'success' -or $null -eq $unfinished.data.connection_file_id) {
        throw 'Could not launch the cleanup test window'
    }
    $unfinishedId = [int]$unfinished.data.connection_file_id
    $cleanupIds += $unfinishedId
    $savedFingerprints[$unfinishedId] = Saved-Connection $unfinishedId
    $otherClose = Call-Tool 'gui_session_disconnect' @{
        connection = $unfinishedId; close_session = $true
    } $bearerB
    if ($otherClose.status -ne 'error' -or $otherClose.error.code -ne 'OWNER_SESSION_UNAVAILABLE') {
        throw ('The other token got an unexpected close response: ' +
            ($otherClose | ConvertTo-Json -Depth 5 -Compress))
    }
    $ownerClose = Call-Tool 'gui_session_disconnect' @{
        connection = $unfinishedId; close_session = $true
    } $bearerA
    if ($ownerClose.status -ne 'success' -or -not $ownerClose.data.session_closed -or
        -not $ownerClose.data.file_deleted) {
        throw ('Launching token could not close its unfinished window: ' +
            ($ownerClose | ConvertTo-Json -Depth 6 -Compress))
    }
    $unfinishedClosed = $true
    $testResult = [pscustomobject]@{ prelogin_claim = 'passed'; other_token_denied = $true;
        owner_login = 'passed'; owner_discovery = 'passed'; owner_prelogin_close = 'passed';
        keyless_prelogin = -not [bool]$savedFingerprints[$unfinishedId].server_session_key }
} finally {
    if ($trayProcessId -and $bearerA) {
        foreach ($savedId in ($cleanupIds | Select-Object -Unique)) {
            if ($savedId -eq $unfinishedId) {
                if (-not $unfinishedClosed) {
                    Write-Warning "Window $savedId needs operator inspection: prelogin close was not confirmed"
                    $cleanupFailures += $savedId
                }
                continue
            }
            $current = Saved-Connection $savedId
            $original = $savedFingerprints[$savedId]
            if (-not $current -or -not $original -or
                $current.cache_generation -ne $original.cache_generation -or
                $current.session_id -ne $original.session_id) {
                Write-Warning "Window $savedId needs operator inspection: saved connection changed"
                $cleanupFailures += $savedId
                continue
            }
            if (-not $ownerLoggedIn) {
                Write-Warning "Window $savedId needs operator inspection: login outcome is uncertain"
                $cleanupFailures += $savedId
                continue
            }
            try {
                if ($savedId -eq $connectionId) {
                    if ($ownerScreenGuard -notmatch '^[0-9a-fA-F]{64}$') {
                        Write-Warning "Window $savedId needs operator cleanup: no saved screen guard"
                        $cleanupFailures += $savedId
                        continue
                    }
                    $lease = Call-Tool 'gui_session_lease' @{ action = 'acquire'; connection = $savedId } $bearerA
                    if (-not $lease.lease_id) {
                        throw ('Could not acquire cleanup lease: keys=' +
                            ($lease.PSObject.Properties.Name -join ',') + '; status=' + [string]$lease.status +
                            '; code=' + [string]$lease.error.code)
                    }
                    $closed = Call-Tool 'gui_session_disconnect' @{
                        connection = $savedId; close_session = $true; lease_id = $lease.lease_id;
                        expected_screen_guard = $ownerScreenGuard
                    } $bearerA
                } else { throw 'Unexpected cleanup connection' }
                if ($closed.status -ne 'success' -or -not $closed.data.file_deleted) {
                    Write-Warning "Window $savedId needs operator cleanup: MCP close was refused"
                    $cleanupFailures += $savedId
                }
            } catch {
                Write-Warning "Window $savedId needs operator cleanup: $($_.Exception.Message)"
                $cleanupFailures += $savedId
            }
        }
    }
    if ($trayProcessId) { Stop-Process -Id $trayProcessId -Force -ErrorAction SilentlyContinue }
    foreach ($token in $tokens) { & $exe mcp token delete $token --yes --output json | Out-Null }
    Remove-Item -LiteralPath $cfgPath -ErrorAction SilentlyContinue
}
if ($cleanupFailures.Count) { throw 'One or more test windows need operator cleanup' }
$testResult | ConvertTo-Json -Compress
