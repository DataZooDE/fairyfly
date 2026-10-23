<#
.SYNOPSIS
  Live check: `session list` and `screen read` name the SAP system, client and user of a session, before and
  after logon (Bigfox = A4H / 001).

.DESCRIPTION
  Requires SAP Logon running on the interactive desktop with the SAP Logon entry "Bigfox" (A4H) and a Windows
  Credential Manager entry for the logon (`fairyfly credentials set Bigfox`, target fairyfly:Bigfox; the script
  never reads, prints or passes a password).

  Steps:
    1. `session launch Bigfox` (no logon) opens a new window on the SAP logon screen.
    2. Before logon: the session's `session list` entry has system == A4H, client == 001, user == "",
       transaction, language, app_server and description == Bigfox; `screen read --output json` has system,
       client, user == "" and transaction in its header; `screen read --output markdown` shows
       "**System:** `A4H` / client `001` / not logged on"; the password field's value is not in either output.
    3. `session login --connection <id> --credential Bigfox`.
    4. After logon: the same checks with a non-empty user (equal to -ExpectUser when given).
    5. `session disconnect --connection <id> --close-session` closes the window again (unless -KeepSession).

  Run from the repository root after building (Release):
    powershell -NoProfile -File tests\integration\session_info_live.ps1
    powershell -NoProfile -File tests\integration\session_info_live.ps1 -ExpectUser DEVELOPER -KeepSession

  Exit code: 0 all checks passed, 1 a check failed, 2 the environment is not ready (launch/login failed).

.PARAMETER Exe            Path to fairyfly.exe (default: build\Release\fairyfly.exe, then build\bin\Release\fairyfly.exe).
.PARAMETER Connection     SAP Logon entry to launch (default Bigfox).
.PARAMETER Credential     Credential Manager entry for the logon (default: same as -Connection).
.PARAMETER ExpectSystem   Expected GuiSessionInfo.SystemName (default A4H).
.PARAMETER ExpectClient   Expected GuiSessionInfo.Client (default 001).
.PARAMETER ExpectUser     Expected user after logon (default: any non-empty user).
.PARAMETER MultipleLogon  Passed to `session login --multiple-logon` (default keep: continue next to other logons).
.PARAMETER KeepSession    Leave the launched window open at the end.
#>
param(
    [string]$Exe = '',
    [string]$Connection = 'Bigfox',
    [string]$Credential = '',
    [string]$ExpectSystem = 'A4H',
    [string]$ExpectClient = '001',
    [string]$ExpectUser = '',
    [ValidateSet('fail', 'keep', 'terminate')][string]$MultipleLogon = 'keep',
    [switch]$KeepSession
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Exe) {
    $Exe = Join-Path $repoRoot 'build\Release\fairyfly.exe'
    if (-not (Test-Path -LiteralPath $Exe)) {
        $alt = Join-Path $repoRoot 'build\bin\Release\fairyfly.exe'
        if (Test-Path -LiteralPath $alt) { $Exe = $alt }
    }
}
if (-not (Test-Path -LiteralPath $Exe)) { Write-Host "fairyfly.exe not found: $Exe"; exit 2 }
if (-not $Credential) { $Credential = $Connection }

$script:Failures = 0
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    if ($Ok) { Write-Host "PASS  $Name" }
    else { Write-Host "FAIL  $Name $Detail"; $script:Failures++ }
}

# Runs fairyfly; returns stdout as one string (logs go to stderr and are dropped).
function Invoke-FF([string[]]$FfArgs) {
    $out = & $Exe @FfArgs 2>$null | Out-String
    return $out
}
function Invoke-FFJson([string[]]$FfArgs) {
    $text = Invoke-FF ($FfArgs + @('--output', 'json'))
    try { return $text | ConvertFrom-Json } catch { throw "fairyfly $($FfArgs -join ' ') did not print JSON: $text" }
}

function Test-SessionInfo([string]$Phase, [string]$SessionId, [int]$ConnId, [bool]$LoggedOn) {
    # --- session list ---------------------------------------------------------------------------------------
    $listed = Invoke-FFJson @('session', 'list')
    Check "$Phase session list succeeds" ($listed.status -eq 'success') ($listed | ConvertTo-Json -Depth 4 -Compress)
    $entry = $null
    foreach ($c in @($listed.data.connections)) {
        foreach ($s in @($c.sessions)) { if ($s.id -eq $SessionId) { $entry = $s } }
    }
    Check "$Phase session list contains $SessionId" ($null -ne $entry)
    if ($entry) {
        Write-Host ('      entry: ' + ($entry | Select-Object id, system, client, user, transaction, language, app_server, description |
            ConvertTo-Json -Compress))
        foreach ($key in 'system', 'client', 'user', 'transaction', 'language', 'app_server', 'description') {
            Check "$Phase session list has '$key'" ($entry.PSObject.Properties.Name -contains $key)
        }
        Check "$Phase session list system == $ExpectSystem" ($entry.system -eq $ExpectSystem) "got '$($entry.system)'"
        Check "$Phase session list client == $ExpectClient" ($entry.client -eq $ExpectClient) "got '$($entry.client)'"
        Check "$Phase session list description == $Connection" ($entry.description -eq $Connection) "got '$($entry.description)'"
        if ($LoggedOn) {
            Check "$Phase session list user is set" (-not [string]::IsNullOrEmpty($entry.user))
            if ($ExpectUser) { Check "$Phase session list user == $ExpectUser" ($entry.user -eq $ExpectUser) "got '$($entry.user)'" }
        } else {
            Check "$Phase session list user is empty" ($entry.user -eq '') "got '$($entry.user)'"
        }
    }

    # --- screen read (JSON header) ----------------------------------------------------------------------------
    $readArgs = @('screen', 'read', '--connection', "$ConnId", '--no-tabs')
    $screen = Invoke-FFJson $readArgs
    Check "$Phase screen read succeeds" ($screen.status -eq 'success') ($screen | ConvertTo-Json -Depth 3 -Compress)
    $d = $screen.data
    Write-Host ('      header: ' + ($d | Select-Object system, client, user, transaction, title | ConvertTo-Json -Compress))
    Check "$Phase screen read system == $ExpectSystem" ($d.system -eq $ExpectSystem) "got '$($d.system)'"
    Check "$Phase screen read client == $ExpectClient" ($d.client -eq $ExpectClient) "got '$($d.client)'"
    Check "$Phase screen read has transaction" (-not [string]::IsNullOrEmpty($d.transaction))
    if ($LoggedOn) {
        Check "$Phase screen read user is set" (-not [string]::IsNullOrEmpty($d.user))
        if ($entry) { Check "$Phase screen read user matches session list" ($d.user -eq $entry.user) }
    } else {
        Check "$Phase screen read user is empty" ($d.user -eq '') "got '$($d.user)'"
        # The logon screen's password field must never carry a value in the output.
        $pwdFields = @($d.elements | Where-Object { $_.id -like '*pwdRSYST-BCODE' })
        foreach ($p in $pwdFields) {
            $v = [string]$p.value + [string]$p.text
            Check "$Phase password field value withheld" ($v -eq '' -or $v.StartsWith('[REDACTED')) "value='$v'"
        }
    }

    # --- screen read (Markdown header, what gui_screen_read returns by default) ------------------------------
    $md = Invoke-FF ($readArgs + @('--output', 'markdown'))
    $expectLine = if ($LoggedOn) {
        "**System:** ``$ExpectSystem`` / client ``$ExpectClient`` / user ``" + $d.user + '`'
    } else {
        "**System:** ``$ExpectSystem`` / client ``$ExpectClient`` / not logged on"
    }
    Check "$Phase markdown header '$expectLine'" ($md.Contains($expectLine))
}

# ---------------------------------------------------------------------------------------------------------------
$launched = Invoke-FFJson @('session', 'launch', $Connection)
if ($launched.status -ne 'success' -or -not $launched.data.session_id) {
    Write-Host ('Launch failed: ' + ($launched | ConvertTo-Json -Depth 5 -Compress))
    exit 2
}
$sessionId = [string]$launched.data.session_id
$connId = [int]$launched.data.connection_file_id
Write-Host "Launched $Connection -> $sessionId (fairyfly connection $connId)"

$exitCode = 0
try {
    Test-SessionInfo 'before logon:' $sessionId $connId $false

    $login = Invoke-FFJson @('session', 'login', '--connection', "$connId", '--credential', $Credential,
                             '--multiple-logon', $MultipleLogon)
    if ($login.status -ne 'success') {
        Write-Host ('Login failed: ' + ($login | ConvertTo-Json -Depth 5 -Compress))
        $exitCode = 2
    } else {
        Test-SessionInfo 'after logon: ' $sessionId $connId $true
    }
} finally {
    if (-not $KeepSession) {
        $closed = Invoke-FFJson @('session', 'disconnect', '--connection', "$connId", '--close-session')
        Write-Host "Closed the test window: $($closed.status)"
    }
}

if ($exitCode -ne 0) { exit $exitCode }
if ($script:Failures -gt 0) { Write-Host "$($script:Failures) check(s) FAILED"; exit 1 }
Write-Host 'All session info checks passed'
exit 0
