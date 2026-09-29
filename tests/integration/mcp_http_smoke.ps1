<#
.SYNOPSIS
  Live smoke test of the fairyfly HTTP MCP server (`fairyfly mcp --http`) against a logged-in SAP GUI session.

.DESCRIPTION
  Starts `fairyfly mcp --http` on a free loopback port (a read-only server, then a write-mode server), creates
  short-lived bearer tokens with `mcp token create`, and checks over plain HTTP: authentication failures
  (401), method and content-type errors (405, 415, 404), both protocol eras (legacy initialize and stateless
  server/discover), tools/list ordering, a real read call, scope / token read-only / T-code / rate-limit
  refusals, revocation, SSE framing, the audit trail (principal, transport, remote_addr, no token string) and
  a clean shutdown of the server process.

  Prerequisites: Windows PowerShell 5.1, SAP GUI with scripting enabled, a logged-in session (SAP Easy Access, no
  popup), a built fairyfly.exe, and access to the Windows Credential Manager of the current user (tokens are
  stored there). No SAP credentials are read or sent; nothing is logged on.

  What it touches in SAP: reads the current screen, and starts transaction SM50 (process overview, display only)
  with a T-code-restricted token, then returns to /n. It never presses Save, Delete, Release, Stop and never
  fills a field: the write-mode server is only used to prove that a read-only token is refused (READ_ONLY)
  before SAP is reached.

  Tokens: named <prefix>-main, -screen, -tcode, -rate, -revoke, -ro with a random prefix. They are printed
  nowhere (only held in variables). The finally block revokes every token that was created and deletes its
  Credential Manager entry (cmdkey /delete:fairyfly-mcp:<name>), also after a failure or Ctrl+C.

  A temporary yaml config (server.transport/port only) and a temporary audit file (FAIRYFLY_AUDIT_FILE) are used
  so the user's real mcp.yaml and audit trail are not involved. The token-expiry check that would need an already
  expired token is reported as SKIP (creation rejects a past --expires; that rejection is checked instead).

  Exit code: 0 all checks passed, 1 at least one check failed, 2 no SAP session (or no session reachable).

.PARAMETER Exe
  Path to fairyfly.exe (default: build\Release\fairyfly.exe, then build\bin\Release\fairyfly.exe under the repo root).
.PARAMETER Port
  TCP port on 127.0.0.1 for the servers (default 0: pick a free port).
.PARAMETER AuditFile
  Audit file of the read-only server (default: a temp file). The write server uses <name>-write.jsonl.
.PARAMETER AllowedTcode
  Transaction a T-code-restricted token may start (default SM50).
.PARAMETER DeniedTcode
  Transaction that token must be refused for (default SE16). It is never started.
.PARAMETER DryRun
  Print the plan and exit without starting anything or creating tokens.
.PARAMETER SkipWriteMode
  Skip the --allow-write server.
#>
[CmdletBinding()]
param(
    [string]$Exe = '',
    [int]$Port = 0,
    [string]$AuditFile = '',
    [string]$AllowedTcode = 'SM50',
    [string]$DeniedTcode = 'SE16',
    [switch]$DryRun,
    [switch]$SkipWriteMode
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# ---- Exe and file resolution --------------------------------------------------------------------------
if (-not $Exe) {
    $candidates = @((Join-Path $RepoRoot 'build\Release\fairyfly.exe'), (Join-Path $RepoRoot 'build\bin\Release\fairyfly.exe'))
    $Exe = $candidates[0]
    foreach ($c in $candidates) { if (Test-Path -LiteralPath $c) { $Exe = $c; break } }
}
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (-not $AuditFile) { $AuditFile = Join-Path ([IO.Path]::GetTempPath()) ('fairyfly-mcp-http-smoke-{0}.jsonl' -f $Stamp) }
$AuditWrite = ($AuditFile -replace '\.jsonl$', '') + '-write.jsonl'

$Plan = @(
    "tokens: <prefix>-main (session,connection,screen,transaction), -screen (screen), -tcode (--tcode $AllowedTcode),",
    '        -rate (--rate 3), -revoke, -ro (session,screen,element); all --read-only; revoked and deleted in finally',
    'server 1: mcp --http --mcp-port <free> (read-only), FAIRYFLY_AUDIT_FILE=<AuditFile>, temp yaml',
    '  server.banner                  posture banner names the endpoint and read-only mode',
    '  http.no_token_401              401 AUTH_REQUIRED + WWW-Authenticate',
    '  http.bad_token_401             401 TOKEN_INVALID',
    '  http.get_405                   405, Allow: POST',
    '  http.content_type_415          415 for text/plain',
    '  http.other_path_404            404',
    '  proto.initialize_legacy        echoes 2025-06-18, serverInfo fairyfly, no Mcp-Session-Id',
    '  proto.notification_202         notifications/initialized -> 202',
    '  proto.server_discover          stateless: supportedVersions incl. 2026-07-28, resultType complete',
    '  proto.unsupported_version_400  JSON-RPC -32022 with data.supported',
    '  proto.header_mismatch_400      JSON-RPC -32020',
    '  proto.tools_list_sorted        20 tools sorted by name, stable across calls, no gui_element_fill; stateless ttlMs',
    '  token.expiry_past_rejected     token create --expires <past> fails (INVALID_ARGUMENT); server-side expiry: SKIP',
    '  sap.sessions / sap.attach      needs a SAP session (exit 2 when none)',
    '  sap.read_call                  gui_screen_read with the main token succeeds, untrusted-data header',
    '  authz.scope_denied             screen-only token: gui_transaction_start -> SCOPE_DENIED; gui_screen_read works',
    "  authz.tcode_denied             -tcode token: $DeniedTcode -> TCODE_DENIED (not started)",
    "  authz.tcode_allowed            -tcode token: $AllowedTcode starts; back to /n",
    '  sse.tools_call                 Accept: text/event-stream -> text/event-stream with "event: message"',
    '  authz.rate_limited             -rate 3: 3 calls ok, 4th RATE_LIMITED',
    '  token.revoke_effective         revoke, then 401 TOKEN_REVOKED within 6 s',
    '  server.clean_stop              Ctrl+C: exit code 0 within 15 s',
    '  audit.*                        transport http, principal, remote_addr, era, denials recorded, no token string',
    'server 2: mcp --http --allow-write (skipped with -SkipWriteMode), audit <name>-write.jsonl',
    '  write.tools_list               21 tools incl. gui_element_fill',
    '  write.ro_token_refused         read-only token: gui_element_fill -> READ_ONLY (nothing reaches SAP)',
    '  write.clean_stop / write.audit_no_token',
    'never pressed/filled: Save, Delete, Release, Stop, any field; no SAP credentials are used'
)

if ($DryRun) {
    Write-Host 'mcp_http_smoke.ps1 DRY RUN (no server is started, no token is created, SAP is not touched)'
    Write-Host ('Exe:       {0}  (exists: {1})' -f $Exe, (Test-Path -LiteralPath $Exe))
    Write-Host ('Port:      {0}' -f $(if ($Port -gt 0) { $Port } else { 'a free port on 127.0.0.1' }))
    Write-Host ('AuditFile: {0}' -f $AuditFile)
    if (-not $SkipWriteMode) { Write-Host ('           {0}' -f $AuditWrite) }
    Write-Host ('SkipWriteMode: {0}' -f [bool]$SkipWriteMode)
    Write-Host 'Plan:'
    foreach ($line in $Plan) {
        if ($SkipWriteMode -and ($line -like 'server 2*' -or $line -like '  write.*')) { continue }
        Write-Host $line
    }
    exit 0
}
if (-not (Test-Path -LiteralPath $Exe)) { Write-Host "fairyfly.exe not found: $Exe"; exit 1 }

# Never let a system proxy intercept the loopback requests.
[System.Net.WebRequest]::DefaultWebProxy = $null

# ---- Check framework ----------------------------------------------------------------------------------
$script:Pass = 0; $script:Fail = 0; $script:Skip = 0
$script:Flags = @{ SapReady = $false }

function Assert-That($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

function Check([string]$Name, [scriptblock]$Body, [string]$Needs = '') {
    if ($Needs -and -not $script:Flags[$Needs]) {
        $script:Skip++
        Write-Host ('SKIP {0} - precondition "{1}" not met' -f $Name, $Needs)
        return
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        [void](& $Body)
        $script:Pass++
        Write-Host ('PASS {0} [{1}ms]' -f $Name, $sw.ElapsedMilliseconds)
    } catch {
        $script:Fail++
        Write-Host ('FAIL {0} [{1}ms] - {2}' -f $Name, $sw.ElapsedMilliseconds, $_.Exception.Message)
    }
}
function Skip-Check([string]$Name, [string]$Reason) {
    $script:Skip++
    Write-Host ('SKIP {0} - {1}' -f $Name, $Reason)
}

function ConvertFrom-JsonSafe([string]$Text) {
    try { return ($Text | ConvertFrom-Json) } catch { return $null }
}
function Has-Prop($Obj, [string]$Name) { return ($null -ne $Obj -and $null -ne $Obj.PSObject.Properties[$Name]) }

# ---- Tokens (held in memory only; never printed) -------------------------------------------------------
$Prefix = 'ffsmoke-' + ([Guid]::NewGuid().ToString('N').Substring(0, 6))
$script:Tokens = @{}          # short name -> @{ Name; Secret }
$script:CreatedNames = New-Object System.Collections.ArrayList

function New-SmokeToken([string]$Short, [string[]]$ExtraArgs) {
    $name = "$Prefix-$Short"
    $argv = @('mcp', 'token', 'create', $name) + $ExtraArgs + @('--output', 'json', '--no-audit')
    [void]$script:CreatedNames.Add($name)   # register first: a half-created token is still cleaned up
    $text = (& $Exe @argv 2>$null | Out-String)
    $j = ConvertFrom-JsonSafe $text
    if ($null -eq $j -or $j.status -ne 'success' -or -not (Has-Prop $j.data 'token') -or -not $j.data.token) {
        $msg = if ($null -ne $j -and (Has-Prop $j 'error')) { [string]$j.error.message } else { 'unparseable output' }
        throw "token create $Short failed: $msg"
    }
    $script:Tokens[$Short] = @{ Name = $name; Secret = [string]$j.data.token }
}

function Remove-SmokeTokens {
    foreach ($name in @($script:CreatedNames)) {
        try { [void](& $Exe mcp token revoke $name --output json --no-audit 2>$null | Out-String) } catch { }
        try { [void](& cmdkey.exe "/delete:fairyfly-mcp:$name" 2>$null) } catch { }
    }
}
function Get-TokenHeader([string]$Short) { return @{ Authorization = ('Bearer ' + $script:Tokens[$Short].Secret) } }

# ---- HTTP helpers --------------------------------------------------------------------------------------
function Get-FreePort {
    $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    $p = ([System.Net.IPEndPoint]$l.LocalEndpoint).Port
    $l.Stop()
    return $p
}

function ConvertTo-BodyText($Content) {
    if ($null -eq $Content) { return '' }
    if ($Content -is [byte[]]) { return [Text.Encoding]::UTF8.GetString($Content) }
    return [string]$Content
}

function Send-Http([string]$Method, [string]$Path = '/mcp', $Body = $null, [hashtable]$Headers = @{},
                   [string]$ContentType = 'application/json', [int]$TimeoutSec = 30) {
    $p = @{ Uri = ('http://127.0.0.1:{0}{1}' -f $script:Port, $Path); Method = $Method; UseBasicParsing = $true
            TimeoutSec = $TimeoutSec; ErrorAction = 'Stop' }
    if ($Headers.Count -gt 0) { $p['Headers'] = $Headers }
    if ($null -ne $Body) { $p['Body'] = [Text.Encoding]::UTF8.GetBytes([string]$Body); $p['ContentType'] = $ContentType }
    $status = 0; $text = ''; $hdr = @{}
    try {
        $r = Invoke-WebRequest @p
        $status = [int]$r.StatusCode
        $text = ConvertTo-BodyText $r.Content
        foreach ($k in $r.Headers.Keys) { $hdr[([string]$k).ToLowerInvariant()] = [string]$r.Headers[$k] }
    } catch [System.Net.WebException] {
        $resp = $_.Exception.Response
        if ($null -eq $resp) { throw }
        $status = [int]$resp.StatusCode
        # Windows PowerShell 5.1 has already consumed the error stream into ErrorDetails; fall back to the stream.
        if ($_.ErrorDetails -and $_.ErrorDetails.Message) { $text = [string]$_.ErrorDetails.Message }
        else {
            $reader = New-Object System.IO.StreamReader($resp.GetResponseStream(), [Text.Encoding]::UTF8)
            try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
        }
        foreach ($k in $resp.Headers.AllKeys) { $hdr[$k.ToLowerInvariant()] = [string]$resp.Headers[$k] }
    }
    return [pscustomobject]@{ Status = $status; Text = $text; Headers = $hdr; Json = (ConvertFrom-JsonSafe $text) }
}

$script:RpcId = 0
function New-RpcBody([string]$Method, $Params = $null) {
    $script:RpcId++
    $msg = [ordered]@{ jsonrpc = '2.0'; id = $script:RpcId; method = $Method }
    if ($null -ne $Params) { $msg['params'] = $Params }
    return ($msg | ConvertTo-Json -Compress -Depth 20)
}

function Invoke-Mcp([string]$Short, [string]$Method, $Params = $null, [hashtable]$ExtraHeaders = @{}, [int]$TimeoutSec = 30) {
    $h = @{}
    if ($Short) { $h = Get-TokenHeader $Short }
    foreach ($k in $ExtraHeaders.Keys) { $h[$k] = $ExtraHeaders[$k] }
    return Send-Http 'POST' '/mcp' (New-RpcBody $Method $Params) $h 'application/json' $TimeoutSec
}
function Invoke-ToolHttp([string]$Short, [string]$Name, $Arguments = @{}, [hashtable]$ExtraHeaders = @{}, [int]$TimeoutSec = 130) {
    return Invoke-Mcp $Short 'tools/call' @{ name = $Name; arguments = $Arguments } $ExtraHeaders $TimeoutSec
}

function Get-ToolText($R) {
    $j = if ($R -is [pscustomobject] -and (Has-Prop $R 'Json')) { $R.Json } else { $R }
    if ($null -eq $j -or -not (Has-Prop $j 'result') -or $null -eq $j.result.content) { return '' }
    foreach ($b in @($j.result.content)) { if ($b.type -eq 'text') { return [string]$b.text } }
    return ''
}
function Test-ToolError($R) {
    $j = $R.Json
    return ($null -ne $j -and (Has-Prop $j 'result') -and $j.result.isError -eq $true)
}
function Get-ErrCode($R) {
    $t = Get-ToolText $R
    if ($t -match '^ERROR ([A-Z0-9_]+):') { return $Matches[1] }
    if ($t -match '^(CALL_TIMEOUT|SERVER_BUSY)') { return $Matches[1] }
    return ''
}
function Get-RpcErrorCode($R) {
    if ($null -eq $R.Json -or -not (Has-Prop $R.Json 'error')) { return $null }
    return [int]$R.Json.error.code
}

# ---- Server process helpers ----------------------------------------------------------------------------
$TempDir = Join-Path ([IO.Path]::GetTempPath()) ('fairyfly-mcp-http-smoke-' + $Stamp)
[void](New-Item -ItemType Directory -Force -Path $TempDir)
$CtrlCScript = Join-Path $TempDir 'send-ctrlc.ps1'
@'
param([uint32]$TargetPid)
$sig = '[DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();' +
       '[DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);' +
       '[DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetConsoleCtrlHandler(IntPtr h, bool add);' +
       '[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint ev, uint grp);'
Add-Type -Namespace Ffly -Name K -MemberDefinition $sig
[void][Ffly.K]::FreeConsole()
if (-not [Ffly.K]::AttachConsole($TargetPid)) { exit 3 }
[void][Ffly.K]::SetConsoleCtrlHandler([IntPtr]::Zero, $true)
[void][Ffly.K]::GenerateConsoleCtrlEvent(1, 0)
Start-Sleep -Milliseconds 500
[void][Ffly.K]::FreeConsole()
exit 0
'@ | Set-Content -LiteralPath $CtrlCScript -Encoding ASCII

function Read-SharedText([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try { $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8); return $sr.ReadToEnd() } finally { $fs.Dispose() }
    } catch { return '' }
}

function Start-HttpServer([string]$Extra, [string]$AuditPath, [string]$Tag) {
    if (Test-Path -LiteralPath $AuditPath) { Remove-Item -LiteralPath $AuditPath -Force }
    $yaml = Join-Path $TempDir "mcp-$Tag.yaml"
    "server:`r`n  transport: http`r`n  port: $($script:Port)`r`n" | Set-Content -LiteralPath $yaml -Encoding ASCII
    $out = Join-Path $TempDir "server-$Tag.out.log"
    $err = Join-Path $TempDir "server-$Tag.err.log"
    $argLine = ('mcp --http --mcp-host 127.0.0.1 --mcp-port {0} -c "{1}" {2}' -f $script:Port, $yaml, $Extra).Trim()
    $saved = @{}
    foreach ($n in @('FAIRYFLY_AUDIT', 'FAIRYFLY_READ_ONLY', 'FAIRYFLY_AUDIT_FILE')) { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
    try {
        [Environment]::SetEnvironmentVariable('FAIRYFLY_AUDIT', $null)
        [Environment]::SetEnvironmentVariable('FAIRYFLY_READ_ONLY', $null)
        [Environment]::SetEnvironmentVariable('FAIRYFLY_AUDIT_FILE', $AuditPath)
        $proc = Start-Process -FilePath $Exe -ArgumentList $argLine -PassThru -WindowStyle Hidden `
                              -RedirectStandardOutput $out -RedirectStandardError $err
    } finally {
        foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
    }
    $null = $proc.Handle   # keep the handle so ExitCode is available
    $srv = @{ Proc = $proc; Out = $out; Err = $err; Audit = $AuditPath; Stopped = $false; ExitCode = $null; Tag = $Tag }
    # Wait until the port accepts connections (or the process died).
    $deadline = (Get-Date).AddSeconds(30)
    $ready = $false
    while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
        try {
            $c = New-Object System.Net.Sockets.TcpClient
            $c.Connect('127.0.0.1', $script:Port)
            $c.Close()
            $ready = $true
            break
        } catch { Start-Sleep -Milliseconds 200 }
    }
    if (-not $ready) {
        $why = (Read-SharedText $err)
        if ($why.Length -gt 300) { $why = $why.Substring(0, 300) }
        throw "server '$Tag' did not start listening on 127.0.0.1:$($script:Port): $why"
    }
    return $srv
}

function Stop-HttpServer($Srv) {
    if ($null -eq $Srv) { return $null }
    if ($Srv.Stopped) { return $Srv.ExitCode }
    $proc = $Srv.Proc
    if (-not $proc.HasExited) {
        try {
            Start-Process -FilePath 'powershell.exe' -Wait -WindowStyle Hidden -ArgumentList @(
                '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $CtrlCScript), '-TargetPid', [string]$proc.Id)
        } catch { }
    }
    if (-not $proc.WaitForExit(15000)) {
        try { $proc.Kill() } catch { }
        [void]$proc.WaitForExit(5000)
        $Srv.ExitCode = -1
    } else {
        $Srv.ExitCode = $proc.ExitCode
    }
    $Srv.Stopped = $true
    return $Srv.ExitCode
}

function Get-AuditRecords([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $records = @()
    foreach ($line in (Get-Content -LiteralPath $Path -Encoding UTF8)) {
        if ($line.Trim()) { $obj = ConvertFrom-JsonSafe $line; if ($null -ne $obj) { $records += $obj } }
    }
    return $records
}

function Assert-NoTokenText([string]$Text, [string]$Where) {
    foreach ($k in $script:Tokens.Keys) {
        $t = $script:Tokens[$k]
        $secret = $t.Secret
        $tail = if ($secret.Length -gt 13) { $secret.Substring(13) } else { $secret }   # ffy_<8 hex>_<secret>
        Assert-That (-not $Text.Contains($secret)) "a token string appears in $Where"
        Assert-That (-not $Text.Contains($tail)) "a token secret appears in $Where"
    }
}

# ---- Run ------------------------------------------------------------------------------------------------
$s1 = $null; $s2 = $null
$exit = 0
$script:Port = if ($Port -gt 0) { $Port } else { Get-FreePort }
try {
    Write-Host ('== tokens: prefix {0} (created now, revoked and deleted at the end)' -f $Prefix)
    New-SmokeToken 'main'   @('--scope', 'session,connection,screen,transaction', '--read-only', '--expires', '1d')
    New-SmokeToken 'screen' @('--scope', 'screen', '--read-only', '--expires', '1d')
    New-SmokeToken 'tcode'  @('--scope', 'session,screen,transaction', '--tcode', $AllowedTcode, '--read-only', '--expires', '1d')
    New-SmokeToken 'rate'   @('--scope', 'session', '--rate', '3', '--read-only', '--expires', '1d')
    New-SmokeToken 'revoke' @('--scope', 'screen', '--read-only', '--expires', '1d')
    New-SmokeToken 'ro'     @('--scope', 'session,screen,element', '--read-only', '--expires', '1d')

    # ======================================================================================================
    # Server 1: read-only
    # ======================================================================================================
    Write-Host ('== server 1: mcp --http (read-only) on 127.0.0.1:{0}, audit file {1}' -f $script:Port, $AuditFile)
    $s1 = Start-HttpServer '' $AuditFile 'ro'

    Check 'server.banner' {
        $deadline = (Get-Date).AddSeconds(5)
        $banner = ''
        while ((Get-Date) -lt $deadline) { $banner = Read-SharedText $s1.Err; if ($banner -match 'fairyfly MCP server \(HTTP\)') { break }; Start-Sleep -Milliseconds 200 }
        Assert-That ($banner -match 'fairyfly MCP server \(HTTP\)') 'posture banner not found on stderr'
        Assert-That ($banner -match ('127\.0\.0\.1:{0}/mcp' -f $script:Port)) 'banner does not name the endpoint'
        Assert-That ($banner -match 'read-only guard') 'banner does not say read-only'
    }

    # ---- HTTP-level rejections (no SAP needed) -------------------------------------------------------
    Check 'http.no_token_401' {
        $r = Send-Http 'POST' '/mcp' (New-RpcBody 'ping') @{}
        Assert-That ($r.Status -eq 401) "status is $($r.Status)"
        Assert-That ($r.Json.error_code -eq 'AUTH_REQUIRED') "error_code is '$($r.Json.error_code)'"
        Assert-That ($r.Headers['www-authenticate'] -match 'Bearer') 'no WWW-Authenticate: Bearer challenge'
    }
    Check 'http.bad_token_401' {
        $fake = 'ffy_00000000_' + ('A' * 43)
        $r = Send-Http 'POST' '/mcp' (New-RpcBody 'ping') @{ Authorization = "Bearer $fake" }
        Assert-That ($r.Status -eq 401) "status is $($r.Status)"
        Assert-That ($r.Json.error_code -eq 'TOKEN_INVALID') "error_code is '$($r.Json.error_code)'"
    }
    Check 'http.get_405' {
        $r = Send-Http 'GET' '/mcp' $null (Get-TokenHeader 'main')
        Assert-That ($r.Status -eq 405) "status is $($r.Status)"
        Assert-That ($r.Headers['allow'] -eq 'POST') "Allow header is '$($r.Headers['allow'])'"
    }
    Check 'http.content_type_415' {
        $r = Send-Http 'POST' '/mcp' (New-RpcBody 'ping') (Get-TokenHeader 'main') 'text/plain'
        Assert-That ($r.Status -eq 415) "status is $($r.Status)"
    }
    Check 'http.other_path_404' {
        $r = Send-Http 'POST' '/other' (New-RpcBody 'ping') (Get-TokenHeader 'main')
        Assert-That ($r.Status -eq 404) "status is $($r.Status)"
    }

    # ---- protocol ---------------------------------------------------------------------------------------
    Check 'proto.initialize_legacy' {
        $r = Invoke-Mcp 'main' 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}
                                               clientInfo = @{ name = 'mcp-http-smoke'; version = '1.0' } }
        Assert-That ($r.Status -eq 200) "status is $($r.Status)"
        Assert-That ($r.Json.result.protocolVersion -eq '2025-06-18') "protocolVersion is '$($r.Json.result.protocolVersion)'"
        Assert-That ($r.Json.result.serverInfo.name -eq 'fairyfly') "serverInfo.name is '$($r.Json.result.serverInfo.name)'"
        Assert-That (-not $r.Headers.ContainsKey('mcp-session-id')) 'a Mcp-Session-Id header was minted'
    }
    Check 'proto.notification_202' {
        $h = Get-TokenHeader 'main'
        $r = Send-Http 'POST' '/mcp' '{"jsonrpc":"2.0","method":"notifications/initialized"}' $h
        Assert-That ($r.Status -eq 202) "status is $($r.Status)"
    }
    Check 'proto.server_discover' {
        $r = Invoke-Mcp 'main' 'server/discover' @{}
        Assert-That ($r.Status -eq 200) "status is $($r.Status)"
        Assert-That (@($r.Json.result.supportedVersions) -contains '2026-07-28') 'supportedVersions lacks 2026-07-28'
        Assert-That ($r.Json.result.resultType -eq 'complete') "resultType is '$($r.Json.result.resultType)'"
        Assert-That ($r.Json.result.serverInfo.name -eq 'fairyfly') 'serverInfo.name is not fairyfly'
    }
    Check 'proto.unsupported_version_400' {
        $r = Invoke-Mcp 'main' 'tools/list' @{} @{ 'MCP-Protocol-Version' = '1999-01-01' }
        Assert-That ($r.Status -eq 400) "status is $($r.Status)"
        Assert-That ((Get-RpcErrorCode $r) -eq -32022) "JSON-RPC code is $(Get-RpcErrorCode $r)"
        Assert-That (@($r.Json.error.data.supported).Count -ge 2) 'error.data.supported is missing'
    }
    Check 'proto.header_mismatch_400' {
        $r = Invoke-Mcp 'main' 'tools/list' @{} @{ 'Mcp-Method' = 'tools/call' }
        Assert-That ($r.Status -eq 400) "status is $($r.Status)"
        Assert-That ((Get-RpcErrorCode $r) -eq -32020) "JSON-RPC code is $(Get-RpcErrorCode $r)"
    }
    Check 'proto.tools_list_sorted' {
        $a = Invoke-Mcp 'main' 'tools/list' @{}
        $b = Invoke-Mcp 'main' 'tools/list' @{}
        $namesA = @($a.Json.result.tools | ForEach-Object { $_.name })
        $namesB = @($b.Json.result.tools | ForEach-Object { $_.name })
        Assert-That ($namesA.Count -eq 20) "expected 20 tools in read-only mode, got $($namesA.Count)"
        Assert-That (($namesA -join ',') -eq ($namesB -join ',')) 'tools/list order differs between two calls'
        $sorted = [string[]]$namesA.Clone()
        [Array]::Sort($sorted, [StringComparer]::Ordinal)
        Assert-That (($sorted -join ',') -eq ($namesA -join ',')) 'tools/list is not sorted by name'
        Assert-That (-not ($namesA -contains 'gui_element_fill')) 'gui_element_fill is listed on a read-only server'
        foreach ($n in $namesA) { Assert-That ($n.StartsWith('gui_')) "tool '$n' does not start with gui_" }
        # NOTE: tools/list is currently NOT filtered by the token's scopes (the screen-only token sees the same
        # 20 tools); scope enforcement happens at tools/call. Only the server-mode visibility is asserted here.
        $st = Invoke-Mcp 'main' 'tools/list' @{} @{ 'MCP-Protocol-Version' = '2026-07-28' }
        Assert-That ($st.Json.result.ttlMs -eq 30000) 'stateless tools/list has no ttlMs 30000'
        Assert-That ($st.Json.result.cacheScope -eq 'private') 'stateless tools/list has no cacheScope private'
    }
    Check 'token.expiry_past_rejected' {
        $text = (& $Exe mcp token create "$Prefix-past" --scope screen --expires 2000-01-01 --output json --no-audit 2>$null | Out-String)
        $j = ConvertFrom-JsonSafe $text
        if ($null -ne $j -and $j.status -eq 'success') { [void]$script:CreatedNames.Add("$Prefix-past") }   # cleaned up in finally
        Assert-That ($null -ne $j -and $j.status -eq 'error') 'creating a token that expired in 2000 did not fail'
        Assert-That ($j.error.code -eq 'INVALID_ARGUMENT') "error code is '$($j.error.code)'"
    }
    Skip-Check 'token.expired_401' 'an already expired token cannot be created (--expires in the past is rejected)'

    # ---- SAP ------------------------------------------------------------------------------------------
    $script:FirstSession = ''
    $r = Invoke-ToolHttp 'main' 'gui_session_list' @{}
    $ids = @([regex]::Matches((Get-ToolText $r), '/app/con\[\d+\]/ses\[\d+\]') | ForEach-Object { $_.Value } | Select-Object -Unique)
    if ($r.Status -ne 200 -or (Test-ToolError $r) -or $ids.Count -lt 1) {
        Write-Host ('FAIL sap.sessions - no SAP GUI session found (HTTP {0} {1})' -f $r.Status, (Get-ErrCode $r))
        $script:Fail++
        $exit = 2
    } else {
        $script:FirstSession = $ids[0]
        $script:Pass++
        Write-Host ('PASS sap.sessions [{0} session(s)]' -f $ids.Count)
        $script:Flags['SapReady'] = $true
    }

    if ($script:Flags['SapReady']) {
        Check 'sap.attach' {
            $r = Invoke-ToolHttp 'main' 'gui_session_attach' @{}
            if ((Test-ToolError $r) -and (Get-ErrCode $r) -eq 'MULTIPLE_SESSIONS') {
                $r = Invoke-ToolHttp 'main' 'gui_session_attach' @{ session_id = $script:FirstSession }
            }
            Assert-That (-not (Test-ToolError $r)) ('attach failed: ' + (Get-ToolText $r))
        }
        Check 'sap.read_call' {
            $r = Invoke-ToolHttp 'main' 'gui_screen_read' @{ no_tabs = $true; only = 'fields'; max_rows = 5 }
            Assert-That ($r.Status -eq 200) "status is $($r.Status)"
            Assert-That (-not (Test-ToolError $r)) ('screen read failed: ' + (Get-ToolText $r))
            Assert-That ((Get-ToolText $r).StartsWith('SAP screen data (untrusted')) 'text does not start with the untrusted-data header'
        }
        Check 'authz.scope_denied' {
            $r = Invoke-ToolHttp 'screen' 'gui_transaction_start' @{ code = '/n' }
            Assert-That ($r.Status -eq 200) "status is $($r.Status)"
            Assert-That (Test-ToolError $r) 'call outside the token scope was not an error'
            Assert-That ((Get-ErrCode $r) -eq 'SCOPE_DENIED') "code is '$(Get-ErrCode $r)'"
            $ok = Invoke-ToolHttp 'screen' 'gui_screen_read' @{ no_tabs = $true; only = 'fields'; max_rows = 3 }
            Assert-That (-not (Test-ToolError $ok)) ('in-scope screen read failed: ' + (Get-ToolText $ok))
        }
        Check 'authz.tcode_denied' {
            $r = Invoke-ToolHttp 'tcode' 'gui_transaction_start' @{ code = "/n$DeniedTcode" }
            Assert-That (Test-ToolError $r) "starting $DeniedTcode was not refused"
            Assert-That ((Get-ErrCode $r) -eq 'TCODE_DENIED') "code is '$(Get-ErrCode $r)'"
            $r2 = Invoke-ToolHttp 'tcode' 'gui_transaction_start' @{ code = $DeniedTcode.ToLowerInvariant() }
            Assert-That ((Get-ErrCode $r2) -eq 'TCODE_DENIED') "lower-case variant: code is '$(Get-ErrCode $r2)'"
        }
        try {
            Check 'authz.tcode_allowed' {
                $r = Invoke-ToolHttp 'tcode' 'gui_transaction_start' @{ code = "/n$AllowedTcode" }
                Assert-That (-not (Test-ToolError $r)) ("starting $AllowedTcode failed: " + (Get-ToolText $r))
            }
        } finally {
            try { [void](Invoke-ToolHttp 'main' 'gui_transaction_start' @{ code = '/n' } @{} 60) } catch { }
        }
        Check 'sse.tools_call' {
            $body = New-RpcBody 'tools/call' @{ name = 'gui_screen_read'; arguments = @{ no_tabs = $true; only = 'fields'; max_rows = 3 } }
            $h = Get-TokenHeader 'main'
            $h['Accept'] = 'text/event-stream'
            $r = Send-Http 'POST' '/mcp' $body $h 'application/json' 130
            Assert-That ($r.Status -eq 200) "status is $($r.Status)"
            Assert-That ($r.Headers['content-type'] -match 'text/event-stream') "Content-Type is '$($r.Headers['content-type'])'"
            Assert-That ($r.Text -match '(?m)^event: message\s*$') 'no "event: message" frame'
            $data = @($r.Text -split "`n" | Where-Object { $_ -like 'data: *' } | ForEach-Object { $_.Substring(6) })
            Assert-That ($data.Count -ge 1) 'no data: line'
            $final = ConvertFrom-JsonSafe $data[$data.Count - 1]
            Assert-That ($null -ne $final -and $final.jsonrpc -eq '2.0' -and (Has-Prop $final 'result')) 'last data frame is not a JSON-RPC result'
            Assert-That ($final.result.isError -ne $true) 'the streamed tool result is an error'
        }
        Check 'authz.rate_limited' {
            for ($i = 1; $i -le 3; $i++) {
                $r = Invoke-ToolHttp 'rate' 'gui_session_list' @{}
                Assert-That (-not (Test-ToolError $r)) ("call $i of 3 was refused: " + (Get-ErrCode $r))
            }
            $r4 = Invoke-ToolHttp 'rate' 'gui_session_list' @{}
            Assert-That (Test-ToolError $r4) 'the 4th call within a minute was not refused'
            Assert-That ((Get-ErrCode $r4) -eq 'RATE_LIMITED') "code is '$(Get-ErrCode $r4)'"
        }
    }

    Check 'token.revoke_effective' {
        $before = Invoke-Mcp 'revoke' 'ping' $null
        Assert-That ($before.Status -eq 200) "ping before revoke: status $($before.Status)"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $text = (& $Exe mcp token revoke "$Prefix-revoke" --output json --no-audit 2>$null | Out-String)
        $j = ConvertFrom-JsonSafe $text
        Assert-That ($null -ne $j -and $j.status -eq 'success') 'token revoke failed'
        $last = $null
        while ($sw.Elapsed.TotalSeconds -lt 8) {
            $last = Invoke-Mcp 'revoke' 'ping' $null
            if ($last.Status -eq 401) { break }
            Start-Sleep -Milliseconds 500
        }
        Assert-That ($last.Status -eq 401) 'the revoked token still works after 8 s'
        Assert-That ($sw.Elapsed.TotalSeconds -le 6.5) ('revocation took {0:N1} s (limit 6 s)' -f $sw.Elapsed.TotalSeconds)
        Assert-That ($last.Json.error_code -eq 'TOKEN_REVOKED') "error_code is '$($last.Json.error_code)'"
    }

    $code1 = Stop-HttpServer $s1
    Check 'server.clean_stop' { Assert-That ($code1 -eq 0) "exit code is $code1 (-1 = had to be killed)" }
    Check 'server.stderr_no_token' { Assert-NoTokenText (Read-SharedText $s1.Err) 'the server log' }

    # ---- audit ------------------------------------------------------------------------------------------
    $records = @(Get-AuditRecords $s1.Audit)
    $toolRecords = @($records | Where-Object { Has-Prop $_ 'tool' })
    Check 'audit.file_and_lifecycle' {
        Assert-That ($records.Count -gt 0) "no audit records in $($s1.Audit)"
        Assert-That (@($records | Where-Object { $_.cmd -eq 'mcp' -and $_.status -eq 'started' }).Count -eq 1) 'no single mcp/started record'
        Assert-That (@($records | Where-Object { $_.cmd -eq 'mcp' -and $_.status -eq 'stopped' }).Count -eq 1) 'no single mcp/stopped record'
    }
    Check 'audit.http_fields' {
        Assert-That ($toolRecords.Count -gt 0) 'no tool records'
        foreach ($rec in $toolRecords) {
            Assert-That ($rec.audit_source -eq 'mcp') "record of $($rec.tool) has audit_source '$($rec.audit_source)'"
            Assert-That ((Has-Prop $rec 'transport') -and $rec.transport -eq 'http') "record of $($rec.tool) has no transport=http"
            Assert-That ((Has-Prop $rec 'principal') -and $rec.principal) "record of $($rec.tool) has no principal"
            Assert-That ((Has-Prop $rec 'remote_addr') -and $rec.remote_addr -eq '127.0.0.1') "record of $($rec.tool) has remote_addr '$($rec.remote_addr)'"
            Assert-That ((Has-Prop $rec 'era') -and $rec.era) "record of $($rec.tool) has no era"
            Assert-That ($rec.read_only -eq $true) "record of $($rec.tool) has read_only != true"
        }
    }
    Check 'audit.principals_and_denials' {
        $principals = @($toolRecords | ForEach-Object { [string]$_.principal } | Select-Object -Unique)
        Assert-That ($principals -contains "$Prefix-main") 'no records for the main token principal'
        if ($script:Flags['SapReady']) {
            foreach ($short in @('screen', 'tcode', 'rate')) { Assert-That ($principals -contains "$Prefix-$short") "no records for principal $Prefix-$short" }
            foreach ($code in @('SCOPE_DENIED', 'TCODE_DENIED', 'RATE_LIMITED')) {
                Assert-That (@($toolRecords | Where-Object { $_.error_code -eq $code }).Count -ge 1) "no audit record with error_code $code"
            }
        }
    }
    Check 'audit.no_token_string' { Assert-NoTokenText (Get-Content -LiteralPath $s1.Audit -Raw -Encoding UTF8) 'the audit file' }
    Check 'audit.no_results_leaked' {
        Assert-That ((Get-Content -LiteralPath $s1.Audit -Raw -Encoding UTF8) -notmatch 'SAP screen data') 'audit file contains screen data'
    }

    # ======================================================================================================
    # Server 2: --allow-write (only proves that a read-only token is refused; nothing reaches SAP)
    # ======================================================================================================
    if (-not $SkipWriteMode) {
        Write-Host ('== server 2: mcp --http --allow-write on 127.0.0.1:{0}, audit file {1}' -f $script:Port, $AuditWrite)
        $s2 = Start-HttpServer '--allow-write' $AuditWrite 'rw'
        Check 'write.banner' {
            $deadline = (Get-Date).AddSeconds(5)
            $banner = ''
            while ((Get-Date) -lt $deadline) { $banner = Read-SharedText $s2.Err; if ($banner -match 'WRITE MODE') { break }; Start-Sleep -Milliseconds 200 }
            Assert-That ($banner -match 'WRITE MODE') 'banner does not announce write mode'
        }
        Check 'write.tools_list' {
            $r = Invoke-Mcp 'main' 'tools/list' @{}
            $names = @($r.Json.result.tools | ForEach-Object { $_.name })
            Assert-That ($names.Count -eq 21) "expected 21 tools, got $($names.Count)"
            Assert-That ($names -contains 'gui_element_fill') 'gui_element_fill is not listed'
        }
        Check 'write.ro_token_refused' {
            # Refused by the token check before any session or SAP element is touched.
            $r = Invoke-ToolHttp 'ro' 'gui_element_fill' @{ element = '/app/con[0]/ses[0]/wnd[0]/usr/txtZMCPSMOKE_NONE'; value = 'ZMCPSMOKE' }
            Assert-That ($r.Status -eq 200) "status is $($r.Status)"
            Assert-That (Test-ToolError $r) 'a read-only token could call gui_element_fill'
            Assert-That ((Get-ErrCode $r) -eq 'READ_ONLY') "code is '$(Get-ErrCode $r)'"
        }
        $code2 = Stop-HttpServer $s2
        Check 'write.clean_stop' { Assert-That ($code2 -eq 0) "exit code is $code2 (-1 = had to be killed)" }
        Check 'write.audit_no_token' {
            Assert-That (Test-Path -LiteralPath $s2.Audit) 'no audit file'
            $raw = Get-Content -LiteralPath $s2.Audit -Raw -Encoding UTF8
            Assert-NoTokenText $raw 'the write-server audit file'
            Assert-That (-not $raw.Contains('ZMCPSMOKE')) 'the fill value appears in the audit file'
            $rec = @(Get-AuditRecords $s2.Audit | Where-Object { (Has-Prop $_ 'tool') -and $_.tool -eq 'gui_element_fill' })
            Assert-That ($rec.Count -ge 1 -and $rec[0].error_code -eq 'READ_ONLY') 'no READ_ONLY audit record for gui_element_fill'
        }
    }
} finally {
    # Never leave a server running, SAP on a side transaction, or a token behind.
    foreach ($srv in @($s1, $s2)) {
        if ($null -ne $srv -and -not $srv.Stopped) { try { [void](Stop-HttpServer $srv) } catch { } }
    }
    try { Remove-SmokeTokens } catch { Write-Host 'WARNING: token cleanup failed; list them with: fairyfly mcp token list' }
    try { Remove-Item -LiteralPath $TempDir -Recurse -Force -ErrorAction SilentlyContinue } catch { }
}

Write-Host ''
Write-Host ('Summary: {0} passed, {1} failed, {2} skipped' -f $script:Pass, $script:Fail, $script:Skip)
if ($exit -eq 2) { Write-Host 'No SAP GUI session found: start SAP GUI, log on, and rerun.'; exit 2 }
if ($script:Fail -gt 0) { exit 1 }
exit 0
