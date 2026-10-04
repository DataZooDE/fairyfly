# Called by test_su01_create_user.ps1 after the disposable user's first login.
# The credential exists only in this Windows user's Credential Manager for this run.
param(
    [Parameter(Mandatory = $true)][string]$Username,
    [Parameter(Mandatory = $true)][string]$CredentialName,
    [Parameter(Mandatory = $true)][int]$AdminConnectionId,
    [string]$ConnectionName = 'Bigfox',
    [string]$FairyflyPath = '',
    [int]$Port = 8394,
    [string]$AdminMarker = 'SU01',
    [string]$SecondMarker = 'SAP Easy Access'
)

$ErrorActionPreference = 'Stop'
if ($Username -cnotmatch '^[A-Z0-9_]{1,12}$' -or $Username -eq 'DEVELOPER' -or
    $CredentialName -ne "FFTEST-$Username" -or $AdminConnectionId -lt 0 -or
    $ConnectionName -ne 'Bigfox' -or $Port -lt 1 -or $Port -gt 65535) {
    throw 'Invalid disposable SAP user or gate arguments'
}
if (-not $FairyflyPath -or -not (Test-Path -LiteralPath $FairyflyPath -PathType Leaf)) {
    throw 'The Fairyfly executable is unavailable'
}
if (-not $AdminMarker -or -not $SecondMarker -or $AdminMarker.Length -lt 4 -or
    $SecondMarker.Length -lt 4 -or $AdminMarker.Contains($SecondMarker) -or
    $SecondMarker.Contains($AdminMarker)) {
    throw 'The two SAP screens need distinct markers'
}
$exe = (Resolve-Path -LiteralPath $FairyflyPath).Path
$endpoint = "http://127.0.0.1:$Port/mcp"
$config = Join-Path $env:TEMP ('fairyfly_two_sap_users_' + [Guid]::NewGuid().ToString('N') + '.yaml')
$suffix = [Guid]::NewGuid().ToString('N')
$tokens = @()
$trayProcessId = $null
$secondId = $null
$secondSaved = $null
$secondLoggedIn = $false
$secondClosed = $false
$secondGuard = ''
$bearerB = ''
$cleanupFailed = $false
$windowsBeforeLaunch = @()

function Saved-Connection([int]$id) {
    $raw = & $exe connection list --output json
    if ($LASTEXITCODE -ne 0) { return $null }
    return @(($raw | ConvertFrom-Json).data.connections | Where-Object { $_.id -eq $id })[0]
}

function New-Token([string]$label, [string]$identity, [string]$scope) {
    $name = "fftwo${label}$suffix"
    $script:tokens += $name
    $raw = & $exe mcp token create $name --scope $scope --sap-identity $identity `
        --system A4H/001 --connections $ConnectionName --expires 1h --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not create a temporary MCP token' }
    $bearer = ($raw | ConvertFrom-Json).data.token
    if (-not $bearer) { throw 'Temporary token secret was missing' }
    return [string]$bearer
}

function Live-Identity($saved) {
    $match = [regex]::Match([string]$saved.session_id, '^/app/con\[(\d+)\]/ses\[(\d+)\]$')
    if (-not $match.Success) { throw 'Saved SAP session ID has an unexpected form' }
    $probe = Join-Path $PSScriptRoot '..\..\build\Release\sap_sta_parallel_probe.exe'
    if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
        throw 'Build sap_sta_parallel_probe before the live two-user gate'
    }
    $raw = & $probe --facts $match.Groups[1].Value $match.Groups[2].Value
    if ($LASTEXITCODE -ne 0) { throw 'Independent SAP identity probe failed' }
    $facts = $raw | ConvertFrom-Json
    if ($facts.session -ne $saved.session_id) { throw 'Independent SAP identity probe read another session' }
    return "$(($facts.system).ToUpperInvariant())/$($facts.client)/$(($facts.user).ToUpperInvariant())"
}

function Live-WindowIds {
    $raw = & $exe session list --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate SAP GUI windows' }
    $listed = $raw | ConvertFrom-Json
    return @($listed.data.connections | ForEach-Object { $_.sessions } | ForEach-Object { $_.id })
}

function Call-Tool([string]$name, [hashtable]$arguments, [string]$bearer) {
    $request = @{ jsonrpc='2.0'; id=$name; method='tools/call'; params=@{
        name=$name; arguments=$arguments
    } } | ConvertTo-Json -Depth 12 -Compress
    $response = Invoke-RestMethod -Method Post -Uri $endpoint -Headers @{
        Authorization = 'Bearer ' + $bearer; Accept = 'application/json'
    } -ContentType 'application/json' -Body $request -TimeoutSec 120
    if ($response.error) { throw "$name returned a JSON-RPC error" }
    if ($response.result.structuredContent) { return $response.result.structuredContent }
    $plain = [string]$response.result.content[0].text
    if ($response.result.isError) {
        try {
            $detail = ($plain -split "`n")[-1] | ConvertFrom-Json
            return [pscustomobject]@{ status='error'; error=$detail }
        } catch { throw "$name returned an MCP tool error" }
    }
    return $plain | ConvertFrom-Json
}

function Screen-Text([int]$id, [string]$bearer) {
    $request = @{ jsonrpc='2.0'; id='screen'; method='tools/call'; params=@{
        name='gui_screen_read'; arguments=@{ connection=$id; no_tabs=$true; max_rows=2 }
    } } | ConvertTo-Json -Depth 12 -Compress
    $response = Invoke-RestMethod -Method Post -Uri $endpoint -Headers @{
        Authorization = 'Bearer ' + $bearer; Accept = 'application/json'
    } -ContentType 'application/json' -Body $request -TimeoutSec 120
    if ($response.error -or $response.result.isError) { throw 'Could not read a test SAP screen' }
    return [string]$response.result.content[0].text
}

try {
    $adminSaved = Saved-Connection $AdminConnectionId
    if (-not $adminSaved -or -not $adminSaved.valid) { throw 'Admin saved connection is not live' }
    if (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
        throw "$endpoint is already in use"
    }
    @('server:', '  transport: http', '  host: 127.0.0.1', "  port: $Port",
      'owner:', "  sap_identities: [A4H/001/DEVELOPER, A4H/001/$Username]") |
        Set-Content -LiteralPath $config -Encoding ascii

    $bearerA = New-Token 'A' 'A4H/001/DEVELOPER' 'session,connection,screen,batch'
    $bearerB = New-Token 'B' "A4H/001/$Username" 'session,session.lease,connection,screen,batch'
    $launch = & $exe mcp --http --tray --allow-write --config $config
    if ($LASTEXITCODE -ne 0 -or ($launch | Out-String) -notmatch 'pid\s+(\d+)') {
        throw 'Could not start the temporary owner-mode tray'
    }
    $trayProcessId = [int]$Matches[1]
    $ready = $false
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        try {
            $tcp = [Net.Sockets.TcpClient]::new()
            $tcp.Connect('127.0.0.1', $Port)
            $tcp.Dispose()
            $ready = $true
            break
        } catch { Start-Sleep -Milliseconds 200 }
    }
    if (-not $ready) { throw 'The temporary tray did not start listening' }

    $adminList = Call-Tool 'gui_connection_list' @{} $bearerA
    if ($adminList.status -ne 'success' -or
        -not @($adminList.data.connections | Where-Object { $_.id -eq $AdminConnectionId }).Count) {
        throw 'The admin token cannot see its own live SAP window'
    }
    $windowsBeforeLaunch = @(Live-WindowIds)
    $opened = Call-Tool 'gui_session_launch' @{
        name=$ConnectionName; login=$true; credential=$CredentialName
    } $bearerB
    if ($opened.status -ne 'success' -or $null -eq $opened.data.connection_file_id) {
        if ($opened.error.connection_file_id -ne $null) {
            $secondId = [int]$opened.error.connection_file_id
            $secondSaved = Saved-Connection $secondId
        }
        throw "MCP did not complete the disposable SAP user login ($($opened.error.code))"
    }
    $secondId = [int]$opened.data.connection_file_id
    $secondSaved = Saved-Connection $secondId
    if (-not $secondSaved -or -not $secondSaved.valid -or $secondId -eq $AdminConnectionId) {
        throw 'The disposable SAP user window could not be verified'
    }
    $secondLoggedIn = $true

    if ((Live-Identity $adminSaved) -cne 'A4H/001/DEVELOPER' -or
        (Live-Identity $secondSaved) -cne "A4H/001/$Username") {
        throw 'Independent SAP GUI identity check did not confirm both users'
    }

    $adminScreen = Screen-Text $AdminConnectionId $bearerA
    $secondScreen = Screen-Text $secondId $bearerB
    if (-not $adminScreen.Contains($AdminMarker) -or $adminScreen.Contains($SecondMarker) -or
        -not $secondScreen.Contains($SecondMarker) -or $secondScreen.Contains($AdminMarker)) {
        throw 'The two live SAP screens do not have distinct verified markers'
    }
    $screen = Call-Tool 'gui_screen_read' @{ connection=$secondId; no_tabs=$true } $bearerB
    $secondGuard = [string]$screen.screen_guard
    if ($secondGuard -notmatch '^[0-9a-fA-F]{64}$') { throw 'Original disposable screen guard unavailable' }

    $parallel = & (Join-Path $PSScriptRoot 'mcp_live_parallel_gate.ps1') `
        -ConnectionA $AdminConnectionId -ConnectionB $secondId `
        -IdentityA A4H/001/DEVELOPER -IdentityB "A4H/001/$Username" `
        -ScreenMarkerA $AdminMarker -ScreenMarkerB $SecondMarker -Endpoint $endpoint -Fairyfly $exe
    $parallelResult = $parallel | ConvertFrom-Json
    if (-not $parallelResult.all_responses_targeted_correctly -or
        -not $parallelResult.two_sap_user_boundary_checked) {
        throw 'The two-SAP-user isolation and parallelism probe did not pass'
    }
    $batch = & (Join-Path $PSScriptRoot 'mcp_live_batch_overlap.ps1') `
        -SlowConnection $AdminConnectionId -ReaderConnection $secondId `
        -SlowIdentity A4H/001/DEVELOPER -ReaderIdentity "A4H/001/$Username" `
        -SlowMarker $AdminMarker -ReaderMarker $SecondMarker -Endpoint $endpoint -Fairyfly $exe
    $batchResult = $batch | ConvertFrom-Json
    if (-not $batchResult.reader_finished_before_batch -or -not $batchResult.same_session_waited_for_batch) {
        throw 'The cross-window progress and same-window ordering probe did not pass'
    }
    $closedProbe = & (Join-Path $PSScriptRoot 'mcp_live_queued_close.ps1') `
        -ConnectionId $secondId -Bearer $bearerB -Endpoint $endpoint -Fairyfly $exe `
        -OwnMarker $SecondMarker -OtherMarker $AdminMarker
    $closedResult = $closedProbe | ConvertFrom-Json
    if (-not $closedResult.queued_close_denied -or -not $closedResult.window_closed) {
        throw 'Queued read was not denied after its test-owned SAP window closed'
    }
    $secondClosed = $true
    [pscustomobject]@{ two_sap_users='passed'; admin='A4H/001/DEVELOPER';
        second="A4H/001/$Username"; parallel_ms=$parallelResult.different_session_ms;
        serial_ms=$parallelResult.serial_ms; same_window_ordering='passed';
        queued_close='passed' } | ConvertTo-Json -Compress
} finally {
    try {
      if (-not $secondClosed -and $secondId -ne $null -and $trayProcessId -and $bearerB) {
        $current = Saved-Connection $secondId
        if (-not $current -and $secondSaved -and
            $secondSaved.session_id -notin @(Live-WindowIds)) {
            $secondClosed = $true
        }
      }
      if (-not $secondClosed -and $secondId -ne $null -and $trayProcessId -and $bearerB) {
        $current = Saved-Connection $secondId
        if (-not $secondLoggedIn -or -not $secondGuard) {
            Write-Warning "Disposable window $secondId has uncertain login state; operator cleanup is required"
            $cleanupFailed = $true
        } elseif (-not $secondSaved -or -not $current -or
            $current.cache_generation -ne $secondSaved.cache_generation -or
            $current.session_id -ne $secondSaved.session_id) {
            Write-Warning "Disposable window $secondId changed; operator cleanup is required"
            $cleanupFailed = $true
        } else {
            try {
                $lease = Call-Tool 'gui_session_lease' @{ action='acquire'; connection=$secondId } $bearerB
                if (-not $lease.lease_id) { throw 'Cleanup lease unavailable' }
                $closed = Call-Tool 'gui_session_disconnect' @{
                    connection=$secondId; close_session=$true; lease_id=$lease.lease_id;
                    expected_screen_guard=$secondGuard
                } $bearerB
                if ($closed.status -ne 'success' -or -not $closed.data.session_closed -or
                    -not $closed.data.file_deleted) { throw 'MCP did not confirm close' }
            } catch {
                Write-Warning "Disposable window $secondId needs operator cleanup: $($_.Exception.Message)"
                $cleanupFailed = $true
            }
        }
      }
    } catch {
        Write-Warning "Could not inspect or close disposable window $secondId`: $($_.Exception.Message)"
        $cleanupFailed = $true
    }
    try {
      if ($windowsBeforeLaunch.Count -and (-not $secondLoggedIn -or -not $secondGuard)) {
        $untracked = @(Live-WindowIds | Where-Object { $_ -notin $windowsBeforeLaunch })
        if ($untracked.Count) {
            Write-Warning "New SAP window(s) require operator inspection: $($untracked -join ', ')"
            $cleanupFailed = $true
        }
      }
    } catch {
        Write-Warning "Could not inspect new SAP windows: $($_.Exception.Message)"
        $cleanupFailed = $true
    }
    if ($trayProcessId) { Stop-Process -Id $trayProcessId -Force -ErrorAction SilentlyContinue }
    foreach ($token in $tokens) {
        try {
            $raw = & $exe mcp token delete $token --yes --output json
            $exitCode = $LASTEXITCODE
            $deleted = $raw | ConvertFrom-Json
            if (($exitCode -ne 0 -or $deleted.status -ne 'success') -and
                $deleted.error.code -ne 'TOKEN_NOT_FOUND') { throw 'Token deletion failed' }
        } catch {
            Write-Warning "Could not remove temporary MCP token $token"
            $cleanupFailed = $true
        }
    }
    Remove-Item -LiteralPath $config -ErrorAction SilentlyContinue
    if ($cleanupFailed) { throw 'Disposable SAP user window was not safely closed' }
}
