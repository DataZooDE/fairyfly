param(
    [Parameter(Mandatory = $true)][string]$OwnerIdentity,
    [Parameter(Mandatory = $true)][int]$ExpectedCount,
    [int]$ExpectedSessions = -1,
    [string]$ConnectionPattern = '*',
    [int]$UnavailableConnection = -1,
    [int]$Port = 8383,
    [string]$Fairyfly = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
if ($OwnerIdentity -notmatch '^[A-Z0-9]{3}/[0-9]{3}/[A-Z0-9_]+$' -or $ExpectedCount -lt 0 -or
    $ExpectedSessions -lt -1 -or
    $Port -lt 1 -or $Port -gt 65535) { throw 'Invalid owner identity, expected count, or port' }

$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$system = ($OwnerIdentity -split '/')[0..1] -join '/'
$endpoint = "http://127.0.0.1:$Port/mcp"
$cfgPath = Join-Path $env:TEMP ('fairyfly_owner_discovery_' + [Guid]::NewGuid().ToString('N') + '.yaml')
$tokenName = 'ffdiscovery' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$trayProcessId = $null
$tokenCreated = $false

function Read-Tool([string]$name, [string]$bearer) {
    $request = @{ jsonrpc = '2.0'; id = $name; method = 'tools/call'; params = @{
        name = $name; arguments = @{}
    } } | ConvertTo-Json -Depth 10 -Compress
    $response = Invoke-RestMethod -Method Post -Uri $endpoint -Headers @{
        Authorization = 'Bearer ' + $bearer
        Accept = 'application/json'
    } -ContentType 'application/json' -Body $request
    if ($response.error -or $response.result.isError) { throw "$name returned an MCP error" }
    if ($response.result.structuredContent) { return $response.result.structuredContent }
    return $response.result.content[0].text | ConvertFrom-Json
}

try {
    if (Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
        throw "$endpoint is already in use"
    }
    @('server:', '  transport: http', '  host: 127.0.0.1', "  port: $Port",
      'owner:', "  sap_identities: [$OwnerIdentity]") |
        Set-Content -LiteralPath $cfgPath -Encoding ascii

    $issued = & $exe mcp token create $tokenName --scope connection,system,screen,session --system $system `
        --connections $ConnectionPattern --read-only --expires 1h --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not create a temporary token' }
    $tokenCreated = $true
    $bearer = ($issued | ConvertFrom-Json).data.token
    if (-not $bearer) { throw 'Token response omitted the secret' }

    $launch = & $exe mcp --http --tray --config $cfgPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not launch the owner-mode tray' }
    if (($launch | Out-String) -notmatch 'pid\s+(\d+)') { throw 'Tray launch did not report its child PID' }
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
    if (-not $ready) { throw 'The tray endpoint did not start listening' }

    $listed = Read-Tool 'gui_connection_list' $bearer
    $diagnosed = Read-Tool 'gui_doctor' $bearer
    if ((@($listed.data.PSObject.Properties.Name | Sort-Object) -join ',') -ne 'connections,count' -or
        (@($diagnosed.data.PSObject.Properties.Name | Sort-Object) -join ',') -ne 'checks,overall_health') {
        throw 'Owner discovery returned unexpected data fields'
    }
    if ($listed.data.count -ne $ExpectedCount -or $listed.data.connections.Count -ne $ExpectedCount) {
        throw 'Owner-filtered connection count differs from the expected count'
    }
    if ($diagnosed.data.checks.Count -ne 1 -or $diagnosed.data.checks[0].name -ne 'owner_connections' -or
        $diagnosed.data.checks[0].count -ne $ExpectedCount) {
        throw 'Owner diagnostic count differs from the verified connection list'
    }
    if ((@($diagnosed.data.checks[0].PSObject.Properties.Name | Sort-Object) -join ',') -ne 'count,message,name,status') {
        throw 'Owner diagnostic exposed unexpected details'
    }
    $expectedHealth = if ($ExpectedCount -gt 0) { 'ok' } else { 'warning' }
    if ($diagnosed.data.overall_health -ne $expectedHealth) { throw 'Owner diagnostic health is incorrect' }
    foreach ($row in $listed.data.connections) {
        if ((@($row.PSObject.Properties.Name | Sort-Object) -join ',') -ne 'description,id,session_id,valid') {
            throw 'Connection row exposed unexpected metadata'
        }
        if ($row.description -notlike $ConnectionPattern -or -not $row.valid -or -not $row.session_id) {
            throw 'A connection row lacks a verified live target'
        }
    }
    if ($ExpectedSessions -ge 0) {
        $sessions = Read-Tool 'gui_session_list' $bearer
        if ($sessions.data.total_sessions -ne $ExpectedSessions) {
            throw 'Owner-filtered SAP session count differs from the expected count'
        }
        $sessionJson = $sessions.data | ConvertTo-Json -Depth 12 -Compress
        if ($sessionJson -match 'connection_string|server_session_key|cache_generation|window_title') {
            throw 'Owner session discovery exposed unfiltered SAP metadata'
        }
    }
    if ($UnavailableConnection -ge 0) {
        $request = @{ jsonrpc = '2.0'; id = 'unavailable'; method = 'tools/call'; params = @{
            name = 'gui_screen_read'; arguments = @{ connection = $UnavailableConnection; no_tabs = $true }
        } } | ConvertTo-Json -Depth 10 -Compress
        $headers = @{ Authorization = 'Bearer ' + $bearer; Accept = 'application/json' }
        try {
            $rejected = Invoke-RestMethod -Method Post -Uri $endpoint -Headers $headers `
                -ContentType 'application/json' -Body $request
        } catch {
            if (-not $_.Exception.Response) { throw }
            $reader = [IO.StreamReader]::new($_.Exception.Response.GetResponseStream())
            try { $rejected = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        }
        $denial = $rejected | ConvertTo-Json -Depth 12 -Compress
        if ($denial -notmatch 'OWNER_SESSION_UNAVAILABLE' -or
            $denial -match 'server_session_key|cache_generation|connection_string|window_title') {
            throw 'An unavailable saved ID did not receive a non-enumerating owner denial'
        }
    }
    [pscustomobject]@{
        endpoint = $endpoint
        owner = $OwnerIdentity
        visible_connections = $listed.data.count
        visible_sessions = if ($ExpectedSessions -ge 0) { $sessions.data.total_sessions } else { $null }
        doctor_health = $diagnosed.data.overall_health
        unavailable_id_denied = $UnavailableConnection -ge 0
        filtered_response = $true
    } | ConvertTo-Json -Compress
} finally {
    if ($trayProcessId) { Stop-Process -Id $trayProcessId -Force -ErrorAction SilentlyContinue }
    else {
        Get-CimInstance Win32_Process -Filter "Name='fairyfly.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -and $_.CommandLine.Contains($cfgPath) } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    }
    if ($tokenCreated) { & $exe mcp token delete $tokenName --yes --output json | Out-Null }
    Remove-Item -LiteralPath $cfgPath -ErrorAction SilentlyContinue
}
