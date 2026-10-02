<#
.SYNOPSIS
  End-to-end Definition-of-Done test of the http.sys MCP listener: setup, TLS server, security posture,
  robustness, teardown, and the other live suites. Windows PowerShell 5.1, run NON-elevated.

.DESCRIPTION
  Drives, in this order (each numbered block is one item of the plan's Definition of Done):
    1  doctor.before                  nothing set up yet: urlacl / sslcert / certificate missing, remedy shown, exit 1
    2  setup.dry_run, setup.runbook   plan and runbook without a UAC prompt, nothing changed
    3  setup.apply, setup.idempotent  `mcp setup --yes` (ONE UAC prompt), verify ok, netsh + files; second run "nothing"
    4  doctor.after_setup             all checks pass
    5  server.start, server.banner, tls.handshake, http.plain_on_tls_port    unelevated `mcp --http`, TLS >= 1.2
    6  http.*                         404 / 405 / 415 / 401 / 401 / 403 wrong Host / 413 before the body
    7  token.create, mcp.initialize.*, mcp.tools_list.*, sap.*, sse.tools_call.*, audit.*   real tokens, live SAP read
    8  authz.*, token.revoke          SCOPE_DENIED, READ_ONLY on a write-mode server, revoked token 401 within 6 s
    9  ip.*                           token --ip from the real peer, X-Forwarded-For ignored, server --allow-ip via LAN
   10  pool.slow_bodies, pool.half_header_closed, server.clean_stop   robustness, Ctrl+Break exit 0 within 15 s
   11  teardown.apply, teardown.idempotent, doctor.after_teardown, token.cleanup   `mcp teardown --yes` (ONE UAC prompt)
   12  suites.*                       unit_tests.exe, bigfox_regression.ps1, mcp_smoke.ps1, mcp_http_smoke.ps1 all green

  The script itself is NOT elevated. The two elevated steps (setup --yes, teardown --yes) are performed by the
  fairyfly commands (they self-elevate: one UAC prompt each); the script prints
  "A UAC prompt will appear now - please approve" before each and only asserts on the results.

  Prerequisites (exit 2 otherwise): non-elevated (a warning when elevated: the test is then meaningless), a
  logged-in SAP GUI session (`session list`), curl.exe, no leftover fairyfly urlacl / sslcert / certificate /
  manifest (run `fairyfly mcp teardown --yes` first), a built fairyfly.exe. The Hostname should resolve
  (DNS or hosts file); when it does not, curl uses --resolve to 127.0.0.1 and the checks that need the LAN address or
  the PowerShell client are reported as SKIP with the reason.

  Side effects: a self-signed certificate in LocalMachine\My, a URL ACL and an SSL binding for the port (all
  created by `mcp setup`, all removed by `mcp teardown`), files under %LOCALAPPDATA%\fairyfly, a temp
  config (-c), a temp audit file (FAIRYFLY_AUDIT_FILE) and temporary tokens named ffe2e-<rand>-<role> (deleted
  in finally). If the script aborts after setup ran but before teardown, it prints the recovery command
  `fairyfly mcp teardown --yes` (and runs it in finally when -Yes is given).

  SAP: only the read tools gui_session_list and gui_screen_read, plus refused write attempts (SCOPE_DENIED,
  READ_ONLY, the request never reaches SAP). Nothing is pressed, filled, saved, deleted, released or stopped.

  Trust: curl.exe gets `--cacert <exported PEM>` (never -k; `--ssl-no-revoke` because a self-signed certificate
  has no revocation endpoint). Windows PowerShell 5.1 uses a ServicePointManager callback that accepts only the
  exact expected SHA-1 thumbprint (restored in finally); the raw TLS clients pin the same thumbprint.

  Exit code: 0 every check passed (SKIP only with a printed reason), 1 at least one FAIL, 2 a prerequisite is missing.

.PARAMETER Exe
  Path to fairyfly.exe (default: build\Release\fairyfly.exe, then build\bin\Release\fairyfly.exe under the repo root).
.PARAMETER Hostname
  Host name of the certificate and the endpoint (default: this computer's DNS name, lower case).
.PARAMETER Port
  TLS port (default 8443).
.PARAMETER SkipSuites
  Do not run the final `suites` step (unit tests and the other live suites).
.PARAMETER SkipSse
  Skip the two curl SSE checks.
.PARAMETER DryRun
  Print the plan and the check list, parse only; no side effects (no process is started).
.PARAMETER Yes
  Do not ask for confirmation before the two UAC steps (and run the recovery teardown automatically if needed).
#>
[CmdletBinding()]
param(
    [string]$Exe = '',
    [string]$Hostname = '',
    [int]$Port = 8443,
    [switch]$SkipSuites,
    [switch]$SkipSse,
    [switch]$DryRun,
    [switch]$Yes
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# ======================================================================================================
# ASSUMPTIONS ABOUT THE C++ SIDE (plan: http.sys listener, "CLI surface") - adjust here after merging A/B/C
# Everything the script assumes about flag names, JSON field names and literal texts lives in this block.
# ======================================================================================================
# -- setup / teardown / cert export / doctor command lines (all `--output json` unless marked text)
$A_SetupArgs        = @('mcp', 'setup', '--hostname', '{HOST}', '--port', '{PORT}', '--self-signed')   # + -c cfg, --dry-run|--yes|--print-runbook
$A_TeardownArgs     = @('mcp', 'teardown')                                                              # + --yes | --dry-run
$A_DoctorArgs       = @('mcp', 'doctor')                                                                # + -c cfg
$A_CertExportArgs   = @('mcp', 'cert', 'export', '--format', 'pem', '--out')                            # + <path>
$A_NonInteractive   = '--non-interactive'                       # never prompts: ELEVATION_REQUIRED instead of UAC
# -- JSON shape (envelope {status, data:{...}} like every fairyfly command)
$A_StepsPath        = 'data.steps'                              # array of { id, status }
$A_StepId           = 'id'
$A_StepStatus       = 'status'
$A_StatusWouldCreate = 'would_create'                           # dry-run status of a step that would be created
$A_StatusCreated    = 'created'                                 # apply status of a step that was created
$A_CriticalSteps    = @('certificate', 'urlacl', 'sslcert', 'certificate_export', 'manifest')   # step ids that must be would_create/created
$A_VerifyPath       = 'data.verify'                             # { ok: bool, tls_protocol: string, thumbprint_match: bool }
$A_VerifyOk         = 'status'   # verify.status is the string 'ok' (not a bool)
$A_VerifyThumbMatch = 'thumbprint_match'
$A_SetupThumbPath   = 'data.certificate.thumbprint'             # optional: compared with the PEM thumbprint when present
$A_ChecksPath       = 'data.checks'                             # doctor: array of { id, status, remedy }
$A_CheckId          = 'id'
$A_CheckStatus      = 'status'                                  # pass|ok|skip|info  vs  fail|error|warn|missing
$A_CheckRemedy      = 'remediation'
$A_DoctorSetupIds   = @('urlacl', 'sslcert', 'certificate')     # doctor check ids that reflect setup state
$A_PassStatuses     = @('pass', 'ok', 'skip', 'info')
$A_WarnStatuses     = @('warn', 'warning')
# -- literal texts of the idempotent runs (text output)
$A_TextSetupNothing    = 'nothing - already set up.'
$A_TextTeardownNothing = 'nothing - already removed.'
$A_RunbookMarkers      = @('netsh', 'New-SelfSignedCertificate')
# -- files written by setup
$A_LocalDir         = (Join-Path $env:LOCALAPPDATA 'fairyfly')
$A_CerFile          = 'fairyfly-mcp-{HOST}.cer'                # in $A_LocalDir
$A_ManifestFile     = 'mcp-setup.json'                          # in $A_LocalDir
$A_CertMatch        = 'fairyfly'                                # subject / friendly name fragment of our certificate in LocalMachine\My
# -- server
$A_ServerArgs       = @('mcp', '--http', '--tls', '--mcp-host', '+', '--allowed-hosts', '{HOST}', '--mcp-port', '{PORT}')   # + -c cfg [--allow-write|--allow-ip CIDR]
$A_AllowIpFlag      = '--allow-ip'
$A_WriteFlag        = '--allow-write'
$A_ReadyStatus      = 405                                        # GET /mcp without token answers 405
$A_BannerEndpoint   = 'https://{HOST}:{PORT}/mcp'                # banner text (regex-escaped)
$A_Endpoint         = '/mcp'
$A_TokenIpFlag      = '--ip'
$A_ErrIpNotAllowed  = 'IP_NOT_ALLOWED'                           # token --ip refusal (403)
$A_ErrAddrNotAllowed = 'ADDRESS_NOT_ALLOWED'                     # server --allow-ip refusal (403)
$A_XffHeader        = 'X-Forwarded-For'
$A_WrongHostStatus  = 403
$A_OversizeBytes    = 104857600                                  # declared Content-Length for the 413 check (100 MB)
$A_SessionListArgs  = @('session', 'list', '--output', 'json')   # a session is found by /app/con[N]/ses[M] or "ses[" in the output
$A_SessionRegex     = 'ses\[\d+\]'
# ======================================================================================================

# ---- Exe, host, files --------------------------------------------------------------------------------
if (-not $Exe) {
    $cands = @((Join-Path $RepoRoot 'build\Release\fairyfly.exe'), (Join-Path $RepoRoot 'build\bin\Release\fairyfly.exe'))
    $Exe = $cands[0]
    foreach ($c in $cands) { if (Test-Path -LiteralPath $c) { $Exe = $c; break } }
}
if (-not $Hostname) {
    try { $Hostname = [Net.Dns]::GetHostEntry([Environment]::MachineName).HostName } catch { $Hostname = [Environment]::MachineName }
}
$Hostname = $Hostname.ToLowerInvariant()
function Expand-Tpl([string]$Text) { return $Text.Replace('{HOST}', $Hostname).Replace('{PORT}', [string]$Port) }
function Expand-List([string[]]$List) { return @($List | ForEach-Object { Expand-Tpl $_ }) }

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Rand = [Guid]::NewGuid().ToString('N').Substring(0, 6)
$Prefix = "ffe2e-$Rand"
$TempDir = Join-Path ([IO.Path]::GetTempPath()) "fairyfly-e2e-httpsys-$Stamp"
$Cfg = Join-Path $TempDir 'mcp.yaml'
$PemPath = Join-Path $TempDir 'fairyfly-mcp.pem'
$Audit1 = Join-Path $TempDir 'audit-ro.jsonl'
$Audit2 = Join-Path $TempDir 'audit-rw.jsonl'
$Audit3 = Join-Path $TempDir 'audit-allowip.jsonl'
$CerPath = Join-Path $A_LocalDir (Expand-Tpl $A_CerFile)
$ManifestPath = Join-Path $A_LocalDir $A_ManifestFile
$RecoveryCmd = ('"{0}" mcp teardown --yes' -f $Exe)

$Plan = @(
    ('Exe {0}, host {1}, port {2}, prefix {3}; SkipSuites={4} SkipSse={5}' -f $Exe, $Hostname, $Port, $Prefix, [bool]$SkipSuites, [bool]$SkipSse),
    'prerequisites (exit 2): non-elevated (warn if elevated), SAP session (session list), curl.exe, no leftover fairyfly urlacl/sslcert/cert/manifest',
    '1  doctor.before                 doctor: urlacl/sslcert/certificate not passing, remedy present, exit 1',
    '2  setup.dry_run                 setup --dry-run: steps would_create, exit 0, nothing changed',
    '   setup.runbook                 setup --print-runbook: netsh + New-SelfSignedCertificate lines, nothing changed',
    '3  setup.apply                   setup --yes (ONE UAC PROMPT): steps created, verify ok (401 over TLS, thumbprint, TLS >= 1.2)',
    '   setup.apply_state             urlacl for this user + sslcert (0.0.0.0 and [::]) visible in netsh, .cer, manifest, yaml written',
    '   setup.idempotent              setup --yes --non-interactive: "nothing - already set up.", exit 0, no prompt',
    '   cert.export                   mcp cert export --format pem (trust anchor for curl.exe / thumbprint source)',
    '4  doctor.after_setup            doctor: no failing check, exit 0',
    '5  server.start                  mcp --http --tls unelevated; readiness = HTTP 405 via curl --cacert',
    '   server.banner                 banner names https://<host>:<port>/mcp',
    '   tls.handshake                 TLS >= 1.2, certificate == expected thumbprint, subject/SAN has the host',
    '   http.plain_on_tls_port        plain HTTP on the TLS port is not a 2xx answer',
    '6  http.other_path_404 / get_405 / content_type_415 / no_token_401 / bad_token_401 / wrong_host_403 / oversized_413 (answered before the body)',
    '7  token.create                  tokens <prefix>-main/-all/-screen/-ro/-revoke/-ipnet/-ipok (never printed)',
    '   mcp.initialize.curl / .ps     legacy initialize (curl.exe and PS 5.1 pinned callback)',
    '   mcp.tools_list.curl / .ps     20 tools sorted, no gui_element_fill',
    '   sap.gui_session_list / sap.gui_screen_read   live read on the logged-in session',
    '   sse.tools_call.h2 / .http1    curl -N: text/event-stream with "event: message" frames (unless -SkipSse)',
    '8  authz.scope_denied            screen-only token -> gui_transaction_start -> SCOPE_DENIED',
    '   authz.read_only_refused       write-mode server (--allow-write), read-only token -> gui_element_fill -> READ_ONLY',
    '   token.revoke                  revoked token answers 401 TOKEN_REVOKED within 6 s',
    '9  ip.token_bound_refused        token --ip 203.0.113.0/24 from the real peer -> 403 IP_NOT_ALLOWED',
    '   ip.xff_ignored                token --ip 127.0.0.1 + spoofed X-Forwarded-For -> still 200',
    '   ip.forwarded_cannot_bypass    token --ip 203.0.113.0/24 + X-Forwarded-For: 203.0.113.9 -> 403',
    '   ip.server_allow_ip_lan        server --allow-ip 203.0.113.0/24: LAN address -> 403 ADDRESS_NOT_ALLOWED',
    '   ip.server_allow_ip_loopback   ... loopback still answers',
    '10 pool.slow_bodies              12 slow-body + 12 half-header TLS clients (SslStream, pinned): legit request < 3 s',
    '   pool.half_header_closed       INFO only: how many half-header sockets are still open after 30 s (the kernel closes them at the machine-wide timer); a legitimate request must still be answered',
    '   server.clean_stop (+ .rw, .allowip)   Ctrl+Break: exit 0 within 15 s',
    '   audit.*                       transport http, real remote_addr, principal, no token string',
    '11 teardown.apply                teardown --yes (ONE UAC PROMPT): urlacl, sslcert x2, cert+key, .cer, manifest gone',
    '   teardown.idempotent           teardown --yes --non-interactive: "nothing - already removed."',
    '   token.cleanup                 all ffe2e tokens deleted',
    '   doctor.after_teardown         urlacl/sslcert/certificate missing again, exit 1; no fairyfly cert in LocalMachine\My',
    '12 suites.unit_tests / suites.bigfox_regression / suites.mcp_smoke / suites.mcp_http_smoke   all green (unless -SkipSuites)',
    ('recovery if the script aborts between setup and teardown: {0}' -f $RecoveryCmd),
    'never pressed/filled in SAP: Save, Delete, Release, Stop, any field'
)

if ($DryRun) {
    Write-Host 'mcp_e2e_httpsys.ps1 DRY RUN (nothing is started, no netsh, no certificate, no token, SAP is not touched)'
    Write-Host ('Exe exists: {0}' -f (Test-Path -LiteralPath $Exe))
    Write-Host ('Temp:       {0}' -f $TempDir)
    Write-Host ('.cer:       {0}' -f $CerPath)
    Write-Host ('manifest:   {0}' -f $ManifestPath)
    Write-Host 'Plan:'
    foreach ($l in $Plan) { Write-Host ('  ' + $l) }
    Write-Host 'Assumed setup command line:'
    Write-Host ('  {0} {1} -c <cfg> --yes --output json' -f $Exe, ((Expand-List $A_SetupArgs) -join ' '))
    exit 0
}

# ---- Check framework -----------------------------------------------------------------------------------
$script:Pass = 0; $script:Fail = 0; $script:Skip = 0
$script:Flags = @{}
function Assert-That($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

function Check([string]$Name, [scriptblock]$Body, [switch]$Fatal) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        [void](& $Body)
        $script:Pass++
        Write-Host ('PASS {0} [{1}ms]' -f $Name, $sw.ElapsedMilliseconds)
    } catch {
        $script:Fail++
        Write-Host ('FAIL {0} [{1}ms] - {2}' -f $Name, $sw.ElapsedMilliseconds, $_.Exception.Message)
        if ($Fatal) { throw 'E2E_ABORT' }
    }
}
function Test-CurlHttp2 {
    # curl.exe --version prints a "Features:" line; HTTP2 is listed only when built with nghttp2 / HTTP/2 support.
    try {
        $v = Invoke-Native 'curl.exe' @('--version') 15
        return @(@($v.Out -split "`n" | Where-Object { $_ -match '^\s*Features:' }) -match '\bHTTP2\b').Count -gt 0
    } catch { return $false }
}
function Info-Line([string]$Text) { Write-Host ('INFO {0}' -f $Text) }
function Skip-Check([string]$Name, [string]$Reason) {
    $script:Skip++
    Write-Host ('SKIP {0} - {1}' -f $Name, $Reason)
}
function ConvertFrom-JsonSafe([string]$Text) { try { return ($Text | ConvertFrom-Json) } catch { return $null } }
function Has-Prop($Obj, [string]$Name) { return ($null -ne $Obj -and $null -ne $Obj.PSObject.Properties[$Name]) }
function Get-Path($Obj, [string]$Path) {
    $cur = $Obj
    foreach ($seg in $Path.Split('.')) { if (-not (Has-Prop $cur $seg)) { return $null }; $cur = $cur.$seg }
    return $cur
}

# ---- Native process helper (hard timeouts everywhere) ---------------------------------------------------
function ConvertTo-ArgString([string[]]$Items) {
    $parts = foreach ($a in $Items) {
        if ($a -eq '') { '""' }
        elseif ($a -match '[\s"]') { '"' + (($a -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"' }
        else { $a }
    }
    return ($parts -join ' ')
}
function Invoke-Native([string]$File, [string[]]$ArgList, [int]$TimeoutSec = 60) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = ConvertTo-ArgString $ArgList
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $so = $p.StandardOutput.ReadToEndAsync()
    $se = $p.StandardError.ReadToEndAsync()
    $timedOut = $false
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        $timedOut = $true
        try { $p.Kill() } catch { }
        [void]$p.WaitForExit(5000)
    }
    $code = if ($timedOut) { -999 } else { $p.ExitCode }
    [void]$so.Wait(5000); [void]$se.Wait(5000)
    $out = ''; $err = ''
    if ($so.IsCompleted) { $out = $so.Result }
    if ($se.IsCompleted) { $err = $se.Result }
    return [pscustomobject]@{ Exit = $code; Out = $out; Err = $err; Ms = $sw.ElapsedMilliseconds; TimedOut = $timedOut; Json = (ConvertFrom-JsonSafe $out) }
}
function Invoke-FF([string[]]$FfArgs, [int]$TimeoutSec = 60) { return (Invoke-Native $Exe $FfArgs $TimeoutSec) }

# ---- Windows / netsh state (read-only) -------------------------------------------------------------------
function Test-Elevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Get-NetshText([string]$What) {
    $r = Invoke-Native 'netsh.exe' @('http', 'show', $What) 30
    return $r.Out
}
function Get-UrlAclBlocks {
    # returns one text block per reserved URL whose URL ends in :<Port>/mcp/ (ours)
    $t = Get-NetshText 'urlacl'
    $blocks = @()
    $chunks = [regex]::Split($t, '(?m)^\s*Reserved URL\s*:')
    foreach ($c in $chunks) { if ($c -match [regex]::Escape(":$Port/mcp/")) { $blocks += $c } }
    return $blocks
}
function Get-SslCertBlocks {
    $t = Get-NetshText 'sslcert'
    $blocks = @()
    $chunks = [regex]::Split($t, '(?m)^\s*IP:port\s*:')
    foreach ($c in $chunks) { if ($c -match '^\s*(0\.0\.0\.0|\[::\]):' + $Port + '\b') { $blocks += $c } }
    return $blocks
}
function Get-OurCerts {
    try {
        return @(Get-ChildItem Cert:\LocalMachine\My -ErrorAction Stop | Where-Object {
            ($_.Subject -match $A_CertMatch) -or ($_.FriendlyName -match $A_CertMatch) -or ($script:ExpectedThumb -and $_.Thumbprint -eq $script:ExpectedThumb)
        })
    } catch { return @() }
}
function Get-State {
    return [pscustomobject]@{
        UrlAcl   = @(Get-UrlAclBlocks)
        SslCert  = @(Get-SslCertBlocks)
        Certs    = @(Get-OurCerts)
        Cer      = (Test-Path -LiteralPath $CerPath)
        Manifest = (Test-Path -LiteralPath $ManifestPath)
    }
}
function Format-State($S) {
    return ('urlacl={0} sslcert={1} cert={2} cer={3} manifest={4}' -f $S.UrlAcl.Count, $S.SslCert.Count, $S.Certs.Count, $S.Cer, $S.Manifest)
}
function Assert-StateEmpty($S, [string]$When) {
    Assert-That ($S.UrlAcl.Count -eq 0 -and $S.SslCert.Count -eq 0 -and $S.Certs.Count -eq 0 -and -not $S.Cer -and -not $S.Manifest) ("state not empty ${When}: " + (Format-State $S))
}

# ---- Doctor helpers ---------------------------------------------------------------------------------------
function Get-DoctorChecks($R) {
    $arr = Get-Path $R.Json $A_ChecksPath
    if ($null -eq $arr) { return @() }
    return @($arr)
}
function Get-DoctorCheck($R, [string]$Id) {
    foreach ($c in (Get-DoctorChecks $R)) { if ([string]$c.$A_CheckId -eq $Id) { return $c } }
    return $null
}

# ---- Tokens (memory only) ---------------------------------------------------------------------------------
$script:Tokens = @{}
$script:CreatedNames = New-Object System.Collections.ArrayList
function New-E2eToken([string]$Short, [string[]]$ExtraArgs) {
    $name = "$Prefix-$Short"
    [void]$script:CreatedNames.Add($name)
    $r = Invoke-FF (@('mcp', 'token', 'create', $name) + $ExtraArgs + @('--output', 'json', '--no-audit')) 60
    $j = $r.Json
    if ($null -eq $j -or $j.status -ne 'success' -or -not (Has-Prop $j.data 'token') -or -not $j.data.token) {
        $msg = if ($null -ne $j -and (Has-Prop $j 'error')) { [string]$j.error.message } else { 'unparseable output' }
        throw "token create $Short failed: $msg"
    }
    $script:Tokens[$Short] = @{ Name = $name; Secret = [string]$j.data.token }
}
function Remove-E2eTokens {
    foreach ($name in @($script:CreatedNames)) {
        try { [void](Invoke-FF @('mcp', 'token', 'revoke', $name, '--output', 'json', '--no-audit') 30) } catch { }
        $deleted = $false
        try {
            $d = Invoke-FF @('mcp', 'token', 'delete', $name, '--yes', '--output', 'json', '--no-audit') 30
            $deleted = ($null -ne $d.Json -and $d.Json.status -eq 'success')
        } catch { }
        if (-not $deleted) { try { [void](Invoke-Native 'cmdkey.exe' @("/delete:fairyfly-mcp:$name") 15) } catch { } }
    }
    $script:CreatedNames.Clear()
}
function Assert-NoTokenText([string]$Text, [string]$Where) {
    foreach ($k in $script:Tokens.Keys) {
        $secret = $script:Tokens[$k].Secret
        $tail = if ($secret.Length -gt 13) { $secret.Substring(13) } else { $secret }
        Assert-That (-not $Text.Contains($secret)) "a token string appears in $Where"
        Assert-That (-not $Text.Contains($tail)) "a token secret appears in $Where"
    }
}

# ---- TLS trust: thumbprint-pinned validation -----------------------------------------------------------------
Add-Type -TypeDefinition @'
using System;
using System.Net.Security;
using System.Security.Cryptography.X509Certificates;
public static class FfPin {
    public static string Thumb = "";
    public static bool Validate(object sender, X509Certificate cert, X509Chain chain, SslPolicyErrors errors) {
        if (cert == null || string.IsNullOrEmpty(Thumb)) return false;
        return string.Equals(new X509Certificate2(cert).Thumbprint, Thumb, StringComparison.OrdinalIgnoreCase);
    }
    public static RemoteCertificateValidationCallback GetCallback() { return new RemoteCertificateValidationCallback(Validate); }
}
'@
$script:SavedCallback = [Net.ServicePointManager]::ServerCertificateValidationCallback
$script:SavedProtocol = [Net.ServicePointManager]::SecurityProtocol
$script:ExpectedThumb = ''
function Set-PinnedTrust([string]$Thumb) {
    $script:ExpectedThumb = $Thumb.ToUpperInvariant()
    [FfPin]::Thumb = $script:ExpectedThumb
    [Net.ServicePointManager]::ServerCertificateValidationCallback = [FfPin]::GetCallback()
    $proto = [Net.SecurityProtocolType]::Tls12
    if ([Enum]::GetNames([Net.SecurityProtocolType]) -contains 'Tls13') { $proto = $proto -bor [Net.SecurityProtocolType]::Tls13 }
    [Net.ServicePointManager]::SecurityProtocol = $proto
}
function Get-PemCert([string]$Path) {
    $pem = Get-Content -LiteralPath $Path -Raw -Encoding ASCII
    $b64 = (($pem -replace '-----[^-]+-----', '') -replace '\s', '')
    return (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(, [Convert]::FromBase64String($b64)))
}

# ---- Name resolution -----------------------------------------------------------------------------------------
$script:Resolvable = $false
$script:LanAddress = ''
try {
    $addrs = @([Net.Dns]::GetHostAddresses($Hostname))
    if ($addrs.Count -gt 0) { $script:Resolvable = $true }
    foreach ($a in $addrs) {
        if ($a.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork -and -not [Net.IPAddress]::IsLoopback($a)) { $script:LanAddress = $a.ToString(); break }
    }
} catch { $script:Resolvable = $false }

# ---- curl.exe client (always via --resolve to the chosen address; never -k) -------------------------------------
[System.Net.WebRequest]::DefaultWebProxy = $null
$script:RpcId = 0
function New-RpcBody([string]$Method, $Params = $null) {
    $script:RpcId++
    $msg = [ordered]@{ jsonrpc = '2.0'; id = $script:RpcId; method = $Method }
    if ($null -ne $Params) { $msg['params'] = $Params }
    return ($msg | ConvertTo-Json -Compress -Depth 20)
}
$script:CurlN = 0
function Send-Curl([string]$Method, [string]$Path, $Body = $null, [hashtable]$Headers = @{}, [string]$ContentType = 'application/json',
                   [string]$Address = '127.0.0.1', [string[]]$Extra = @(), [int]$MaxTime = 30) {
    $script:CurlN++
    $n = $script:CurlN
    $hf = Join-Path $TempDir "curl-$n.hdr"; $df = Join-Path $TempDir "curl-$n.dat"; $of = Join-Path $TempDir "curl-$n.out"; $bf = Join-Path $TempDir "curl-$n.body"
    $lines = @()
    if ($null -ne $Body) { $lines += "Content-Type: $ContentType" }
    foreach ($k in $Headers.Keys) { $lines += ('{0}: {1}' -f $k, $Headers[$k]) }
    if ($lines.Count -eq 0) { $lines += 'X-E2e: 1' }
    [IO.File]::WriteAllText($hf, (($lines -join "`r`n") + "`r`n"), [Text.Encoding]::ASCII)
    $a = @('-sS', '--max-time', [string]$MaxTime, '--cacert', $PemPath, '--ssl-no-revoke',
           '--resolve', ('{0}:{1}:{2}' -f $Hostname, $Port, $Address),
           '-X', $Method, '-H', "@$hf", '-D', $df, '-o', $of, '-w', '%{http_code}|%{http_version}')
    if ($null -ne $Body) { [IO.File]::WriteAllText($bf, [string]$Body, (New-Object Text.UTF8Encoding($false))); $a += @('--data-binary', "@$bf") }
    $a += $Extra
    $a += ('https://{0}:{1}{2}' -f $Hostname, $Port, $Path)
    $r = Invoke-Native 'curl.exe' $a ($MaxTime + 15)
    $status = 0; $ver = ''
    if ($r.Out -match '^(\d{3})\|(.*)$') { $status = [int]$Matches[1]; $ver = $Matches[2].Trim() }
    $hdr = @{}
    if (Test-Path -LiteralPath $df) {
        foreach ($line in (Get-Content -LiteralPath $df -Encoding ASCII)) {
            if ($line -match '^([^:\s]+):\s*(.*)$') { $hdr[$Matches[1].ToLowerInvariant()] = $Matches[2].Trim() }
        }
    }
    $text = ''
    if (Test-Path -LiteralPath $of) { $text = [IO.File]::ReadAllText($of, [Text.Encoding]::UTF8) }
    foreach ($f in @($hf, $df, $of, $bf)) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
    return [pscustomobject]@{ Status = $status; Version = $ver; Text = $text; Headers = $hdr; Json = (ConvertFrom-JsonSafe $text); CurlExit = $r.Exit; Err = $r.Err.Trim() }
}

# ---- Windows PowerShell 5.1 client (pinned ServicePointManager callback) -----------------------------------------
function ConvertTo-BodyText($Content) {
    if ($null -eq $Content) { return '' }
    if ($Content -is [byte[]]) { return [Text.Encoding]::UTF8.GetString($Content) }
    return [string]$Content
}
function Send-Ps([string]$Method, [string]$Path, $Body = $null, [hashtable]$Headers = @{}, [string]$ContentType = 'application/json', [int]$TimeoutSec = 30) {
    $p = @{ Uri = ('https://{0}:{1}{2}' -f $Hostname, $Port, $Path); Method = $Method; UseBasicParsing = $true; TimeoutSec = $TimeoutSec; ErrorAction = 'Stop' }
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
        if ($_.ErrorDetails -and $_.ErrorDetails.Message) { $text = [string]$_.ErrorDetails.Message }
        else {
            $reader = New-Object System.IO.StreamReader($resp.GetResponseStream(), [Text.Encoding]::UTF8)
            try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
        }
        foreach ($k in $resp.Headers.AllKeys) { $hdr[$k.ToLowerInvariant()] = [string]$resp.Headers[$k] }
    }
    return [pscustomobject]@{ Status = $status; Text = $text; Headers = $hdr; Json = (ConvertFrom-JsonSafe $text) }
}

function Get-AuthHeader([string]$Short) { return @{ Authorization = ('Bearer ' + $script:Tokens[$Short].Secret) } }
function Invoke-Mcp([string]$Short, [string]$Method, $Params = $null, [hashtable]$Extra = @{}, [string]$Client = 'curl', [string]$Address = '127.0.0.1', [int]$TimeoutSec = 30) {
    $h = @{}
    if ($Short) { $h = Get-AuthHeader $Short }
    foreach ($k in $Extra.Keys) { $h[$k] = $Extra[$k] }
    $body = New-RpcBody $Method $Params
    if ($Client -eq 'ps') { return (Send-Ps 'POST' $A_Endpoint $body $h 'application/json' $TimeoutSec) }
    return (Send-Curl 'POST' $A_Endpoint $body $h 'application/json' $Address @() $TimeoutSec)
}
function Invoke-ToolCall([string]$Short, [string]$Name, $Arguments = @{}, [hashtable]$Extra = @{}, [string]$Client = 'curl', [int]$TimeoutSec = 130) {
    return (Invoke-Mcp $Short 'tools/call' @{ name = $Name; arguments = $Arguments } $Extra $Client '127.0.0.1' $TimeoutSec)
}
function Get-ToolText($R) {
    $j = $R.Json
    if ($null -eq $j -or -not (Has-Prop $j 'result') -or $null -eq $j.result.content) { return '' }
    foreach ($b in @($j.result.content)) { if ($b.type -eq 'text') { return [string]$b.text } }
    return ''
}
function Test-ToolError($R) { $j = $R.Json; return ($null -ne $j -and (Has-Prop $j 'result') -and $j.result.isError -eq $true) }
function Get-ErrCode($R) {
    $t = Get-ToolText $R
    if ($t -match '^ERROR ([A-Z0-9_]+):') { return $Matches[1] }
    return ''
}

# ---- Raw TLS / TCP clients (SslStream with the pinned callback) --------------------------------------------------
function Open-TlsClient([string]$Target = '127.0.0.1') {
    $tcp = New-Object Net.Sockets.TcpClient
    $tcp.NoDelay = $true
    $iar = $tcp.BeginConnect($Target, $Port, $null, $null)
    if (-not $iar.AsyncWaitHandle.WaitOne(5000)) { $tcp.Close(); throw "TCP connect to ${Target}:$Port timed out" }
    $tcp.EndConnect($iar)
    $tcp.ReceiveTimeout = 10000; $tcp.SendTimeout = 10000
    $ssl = New-Object Net.Security.SslStream($tcp.GetStream(), $false, [FfPin]::GetCallback())
    $ssl.AuthenticateAsClient($Hostname)
    return [pscustomobject]@{ Tcp = $tcp; Ssl = $ssl }
}
function Close-Client($C) { try { $C.Ssl.Dispose() } catch { }; try { $C.Tcp.Close() } catch { } }
function Send-RawText($Stream, [string]$Text) { $b = [Text.Encoding]::ASCII.GetBytes($Text); $Stream.Write($b, 0, $b.Length); $Stream.Flush() }
function Read-RawText($Stream, [int]$TimeoutMs = 5000) {
    $Stream.ReadTimeout = $TimeoutMs
    $sb = New-Object Text.StringBuilder
    $buf = New-Object byte[] 4096
    try {
        while ($true) {
            $n = $Stream.Read($buf, 0, $buf.Length)
            if ($n -le 0) { break }
            [void]$sb.Append([Text.Encoding]::ASCII.GetString($buf, 0, $n))
            if ($sb.Length -gt 65536) { break }
            if ($sb.ToString().Contains("`r`n`r`n")) { break }
        }
    } catch { }
    return $sb.ToString()
}
function Test-SocketClosed($Tcp) {
    try {
        if ($Tcp.Client.Poll(100000, [Net.Sockets.SelectMode]::SelectRead)) {
            if ($Tcp.Client.Available -gt 0) { $b = New-Object byte[] 4096; [void]$Tcp.Client.Receive($b); return $false }
            return $true
        }
        return $false
    } catch { return $true }
}

# ---- Server process helpers -----------------------------------------------------------------------------------------
$CtrlCScript = Join-Path $TempDir 'send-ctrlbreak.ps1'
$CtrlCText = @'
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
'@
function Read-SharedText([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try { $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8); return $sr.ReadToEnd() } finally { $fs.Dispose() }
    } catch { return '' }
}
function Start-E2eServer([string[]]$Extra, [string]$AuditPath, [string]$Tag) {
    $out = Join-Path $TempDir "server-$Tag.out.log"
    $err = Join-Path $TempDir "server-$Tag.err.log"
    $argLine = ConvertTo-ArgString ((Expand-List $A_ServerArgs) + @('-c', $Cfg) + $Extra)
    $saved = @{}
    foreach ($n in @('FAIRYFLY_AUDIT', 'FAIRYFLY_READ_ONLY', 'FAIRYFLY_AUDIT_FILE')) { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
    try {
        [Environment]::SetEnvironmentVariable('FAIRYFLY_AUDIT', $null)
        [Environment]::SetEnvironmentVariable('FAIRYFLY_READ_ONLY', $null)
        [Environment]::SetEnvironmentVariable('FAIRYFLY_AUDIT_FILE', $AuditPath)
        $proc = Start-Process -FilePath $Exe -ArgumentList $argLine -PassThru -WindowStyle Hidden -RedirectStandardOutput $out -RedirectStandardError $err
    } finally {
        foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
    }
    $null = $proc.Handle
    $srv = @{ Proc = $proc; Out = $out; Err = $err; Audit = $AuditPath; Stopped = $false; ExitCode = $null; Tag = $Tag }
    $script:Servers += , $srv
    $deadline = (Get-Date).AddSeconds(30)
    $ready = $false
    while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
        $probe = Send-Curl 'GET' $A_Endpoint $null @{} 'application/json' '127.0.0.1' @() 5
        if ($probe.Status -eq $A_ReadyStatus) { $ready = $true; break }
        Start-Sleep -Milliseconds 300
    }
    if (-not $ready) {
        $why = (Read-SharedText $err)
        if ($why.Length -gt 400) { $why = $why.Substring(0, 400) }
        throw "server '$Tag' did not answer HTTP $A_ReadyStatus over TLS on ${Hostname}:$Port : $why"
    }
    return $srv
}
function Stop-E2eServer($Srv) {
    if ($null -eq $Srv) { return $null }
    if ($Srv.Stopped) { return $Srv.ExitCode }
    $proc = $Srv.Proc
    if (-not $proc.HasExited) {
        try {
            Start-Process -FilePath 'powershell.exe' -Wait -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $CtrlCScript), '-TargetPid', [string]$proc.Id)
        } catch { }
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    if (-not $proc.WaitForExit(15000)) {
        try { $proc.Kill() } catch { }
        [void]$proc.WaitForExit(5000)
        $Srv.ExitCode = -1
    } else { $Srv.ExitCode = $proc.ExitCode }
    $Srv.StopMs = $sw.ElapsedMilliseconds
    $Srv.Stopped = $true
    return $Srv.ExitCode
}
function Get-AuditRecords([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $records = @()
    foreach ($line in (Get-Content -LiteralPath $Path -Encoding UTF8)) {
        if ($line.Trim()) { $o = ConvertFrom-JsonSafe $line; if ($null -ne $o) { $records += $o } }
    }
    return $records
}
function Wait-Banner($Srv, [string]$Pattern) {
    $deadline = (Get-Date).AddSeconds(5); $banner = ''
    while ((Get-Date) -lt $deadline) { $banner = Read-SharedText $Srv.Err; if ($banner -match $Pattern) { break }; Start-Sleep -Milliseconds 200 }
    return $banner
}

# ======================================================================================================
# Prerequisites (exit 2)
# ======================================================================================================
$exit = 0
$script:Servers = @()
$script:SetupMayHaveRun = $false
$script:TeardownDone = $false
$savedMcpConfig = [Environment]::GetEnvironmentVariable('FAIRYFLY_MCP_CONFIG')
$savedEnv = @{}
foreach ($n in @('FAIRYFLY_AUDIT_FILE', 'FAIRYFLY_AUDIT', 'FAIRYFLY_READ_ONLY')) { $savedEnv[$n] = [Environment]::GetEnvironmentVariable($n) }

function Exit-Prereq([string[]]$Lines) {
    Write-Host 'PREREQUISITE MISSING:'
    foreach ($l in $Lines) { Write-Host ('  ' + $l) }
    exit 2
}

Write-Host ('== mcp_e2e_httpsys: host {0}, port {1}, token prefix {2}' -f $Hostname, $Port, $Prefix)
if (-not (Test-Path -LiteralPath $Exe)) { Exit-Prereq @("fairyfly.exe not found: $Exe (build it: cmake --build build --config Release --target fairyfly)") }
if (Test-Elevated) { Write-Host 'WARNING: this PowerShell is elevated. The test is meaningless elevated (the server must run unelevated); start a normal prompt.' }
$curlCmd = Get-Command 'curl.exe' -ErrorAction SilentlyContinue
if ($null -eq $curlCmd) { Exit-Prereq @('curl.exe not found in PATH (Windows 10 1803+ ships it in System32).') }

[void](New-Item -ItemType Directory -Force -Path $TempDir)
$CtrlCText | Set-Content -LiteralPath $CtrlCScript -Encoding ASCII

$sess = Invoke-FF $A_SessionListArgs 60
if ($sess.TimedOut -or $sess.Out -notmatch $A_SessionRegex) {
    Remove-Item -LiteralPath $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    Exit-Prereq @('No logged-in SAP GUI session found (fairyfly session list). Start SAP GUI, log on to Bigfox (SAP Easy Access, no popup) and rerun.')
}
$pre = Get-State
if ($pre.UrlAcl.Count -gt 0 -or $pre.SslCert.Count -gt 0 -or $pre.Certs.Count -gt 0 -or $pre.Cer -or $pre.Manifest) {
    Remove-Item -LiteralPath $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    Exit-Prereq @(('Leftover fairyfly setup found (' + (Format-State $pre) + ').'), ('Remove it first: {0}' -f $RecoveryCmd))
}
if (-not $script:Resolvable) {
    Write-Host ('NOTE: host name "{0}" does not resolve: curl uses --resolve to 127.0.0.1; PowerShell-client and LAN-address checks are skipped.' -f $Hostname)
} elseif (-not $script:LanAddress) {
    Write-Host ('NOTE: "{0}" resolves but to no non-loopback IPv4 address: the LAN-address check is skipped.' -f $Hostname)
}

if (-not $Yes) {
    Write-Host ''
    Write-Host 'This test performs TWO elevated steps, each with a UAC prompt: `mcp setup --yes` and `mcp teardown --yes`.'
    Write-Host ('It creates and later removes a self-signed certificate, a URL ACL and an SSL binding for port {0}.' -f $Port)
    $ans = Read-Host 'Continue? (y/N)'
    if ($ans -notmatch '^(y|yes)$') { Remove-Item -LiteralPath $TempDir -Recurse -Force -ErrorAction SilentlyContinue; Exit-Prereq @('Not confirmed.') }
}

# Per-run env: temp audit file for every fairyfly call of this script.
[Environment]::SetEnvironmentVariable('FAIRYFLY_AUDIT_FILE', (Join-Path $TempDir 'audit-cli.jsonl'))
[Environment]::SetEnvironmentVariable('FAIRYFLY_AUDIT', $null)

# Config the setup/doctor/server all use (-c). Pre-created so doctor works before setup; setup never rewrites it.
"server:`r`n  transport: http`r`n  host: '+'`r`n  port: $Port`r`n  tls: true`r`n" | Set-Content -LiteralPath $Cfg -Encoding ASCII

# ======================================================================================================
# Run
# ======================================================================================================
$s1 = $null; $s2 = $null; $s3 = $null
try {
    # ---- 1 doctor.before ------------------------------------------------------------------------------
    Check 'doctor.before' {
        $r = Invoke-FF ($A_DoctorArgs + @('-c', $Cfg, '--output', 'json')) 90
        Assert-That ($null -ne $r.Json) 'doctor output is not JSON'
        Assert-That ($r.Exit -eq 1) "exit code is $($r.Exit), expected 1"
        foreach ($id in $A_DoctorSetupIds) {
            $c = Get-DoctorCheck $r $id
            Assert-That ($null -ne $c) "no doctor check '$id'"
            Assert-That (-not ($A_PassStatuses -contains ([string]$c.$A_CheckStatus).ToLowerInvariant())) "check '$id' is '$($c.$A_CheckStatus)', expected missing/failing"
            Assert-That ([string]$c.$A_CheckRemedy) "check '$id' has no remedy"
        }
    }

    # ---- 2 dry run and runbook ------------------------------------------------------------------------------
    Check 'setup.dry_run' {
        $r = Invoke-FF ((Expand-List $A_SetupArgs) + @('-c', $Cfg, '--dry-run', $A_NonInteractive, '--output', 'json')) 60
        Assert-That ($r.Exit -eq 0) "exit code is $($r.Exit) (a UAC prompt or error?): $($r.Err.Trim())"
        Assert-That ($null -ne $r.Json) 'output is not JSON'
        $steps = @(Get-Path $r.Json $A_StepsPath)
        Assert-That ($steps.Count -gt 0) "no steps under $A_StepsPath"
        foreach ($id in $A_CriticalSteps) {
            $st = @($steps | Where-Object { [string]$_.$A_StepId -eq $id })
            Assert-That ($st.Count -eq 1) "step '$id' missing from the plan"
            Assert-That ([string]$st[0].$A_StepStatus -eq $A_StatusWouldCreate) "step '$id' is '$($st[0].$A_StepStatus)', expected $A_StatusWouldCreate"
        }
        Assert-StateEmpty (Get-State) 'after --dry-run'
    }
    Check 'setup.runbook' {
        $r = Invoke-FF ((Expand-List $A_SetupArgs) + @('-c', $Cfg, '--print-runbook', $A_NonInteractive)) 60
        Assert-That ($r.Exit -eq 0) "exit code is $($r.Exit): $($r.Err.Trim())"
        foreach ($m in $A_RunbookMarkers) { Assert-That ($r.Out.Contains($m)) "runbook lacks '$m'" }
        Assert-StateEmpty (Get-State) 'after --print-runbook'
    }

    # ---- 3 setup --yes (ONE UAC prompt) -------------------------------------------------------------------
    Write-Host 'A UAC prompt will appear now - please approve'
    $script:SetupMayHaveRun = $true
    $script:SetupResult = Invoke-FF ((Expand-List $A_SetupArgs) + @('-c', $Cfg, '--yes', '--output', 'json')) 300
    Check 'setup.apply' {
        $r = $script:SetupResult
        Assert-That (-not $r.TimedOut) 'setup timed out (UAC prompt not answered?)'
        Assert-That ($r.Exit -eq 0) "exit code is $($r.Exit): $($r.Err.Trim())"
        Assert-That ($null -ne $r.Json) 'output is not JSON'
        $steps = @(Get-Path $r.Json $A_StepsPath)
        foreach ($id in $A_CriticalSteps) {
            $st = @($steps | Where-Object { [string]$_.$A_StepId -eq $id })
            Assert-That ($st.Count -eq 1) "step '$id' missing from the result"
            Assert-That ([string]$st[0].$A_StepStatus -eq $A_StatusCreated) "step '$id' is '$($st[0].$A_StepStatus)', expected $A_StatusCreated"
        }
        $v = Get-Path $r.Json $A_VerifyPath
        Assert-That ($null -ne $v) "no verify object under $A_VerifyPath"
        Assert-That ([string]$v.$A_VerifyOk -eq 'ok') 'verify is not ok (401 over TLS, thumbprint, TLS >= 1.2)'
        if (Has-Prop $v $A_VerifyThumbMatch) { Assert-That ($v.$A_VerifyThumbMatch -eq $true) 'verify: thumbprint does not match' }
    } -Fatal
    Check 'setup.apply_state' {
        $s = Get-State
        Assert-That ($s.UrlAcl.Count -ge 1) 'no urlacl for the port in netsh http show urlacl'
        $me = $env:USERNAME
        Assert-That (($s.UrlAcl -join "`n") -match [regex]::Escape($me)) "the urlacl does not name the current user ($me)"
        Assert-That ($s.SslCert.Count -ge 2) "expected sslcert bindings for 0.0.0.0 and [::], found $($s.SslCert.Count)"
        Assert-That ($s.Certs.Count -ge 1) 'no fairyfly certificate in LocalMachine\My'
        Assert-That $s.Cer ".cer not written: $CerPath"
        Assert-That $s.Manifest "manifest not written: $ManifestPath"
        Assert-That (Test-Path -LiteralPath $Cfg) 'config yaml missing'
    }
    Check 'setup.idempotent' {
        $r = Invoke-FF ((Expand-List $A_SetupArgs) + @('-c', $Cfg, '--yes', $A_NonInteractive)) 90
        Assert-That ($r.Exit -eq 0) "exit code is $($r.Exit) (a second run must need no elevation): $($r.Err.Trim())"
        Assert-That ($r.Out.ToLowerInvariant().Contains($A_TextSetupNothing)) "output lacks '$A_TextSetupNothing'"
    }
    Check 'cert.export' {
        $r = Invoke-FF ($A_CertExportArgs + @($PemPath, '--output', 'json')) 60
        Assert-That ($r.Exit -eq 0) "exit code is $($r.Exit): $($r.Err.Trim())"
        Assert-That (Test-Path -LiteralPath $PemPath) 'PEM not written'
        $cert = Get-PemCert $PemPath
        $sanText = ''
        foreach ($e in $cert.Extensions) { try { $sanText += $e.Format($false) } catch { } }
        Assert-That (($cert.Subject -match [regex]::Escape($Hostname)) -or ($sanText -match [regex]::Escape($Hostname))) "certificate names neither subject nor SAN '$Hostname' ($($cert.Subject))"
        Assert-That ($cert.NotAfter -gt (Get-Date)) 'certificate is expired'
        $fromSetup = Get-Path $script:SetupResult.Json $A_SetupThumbPath
        if ($fromSetup) { Assert-That (([string]$fromSetup).Replace(' ', '').ToUpperInvariant() -eq $cert.Thumbprint.ToUpperInvariant()) 'setup thumbprint differs from the exported certificate' }
        Set-PinnedTrust $cert.Thumbprint
        $script:Cert = $cert
    } -Fatal

    # ---- 4 doctor.after_setup ------------------------------------------------------------------------------------
    Check 'doctor.after_setup' {
        $r = Invoke-FF ($A_DoctorArgs + @('-c', $Cfg, '--output', 'json')) 90
        Assert-That ($null -ne $r.Json) 'doctor output is not JSON'
        $checks = @(Get-DoctorChecks $r)
        Assert-That ($checks.Count -gt 0) 'no checks reported'
        $bad = @($checks | Where-Object { -not ($A_PassStatuses -contains ([string]$_.$A_CheckStatus).ToLowerInvariant()) -and -not ($A_WarnStatuses -contains ([string]$_.$A_CheckStatus).ToLowerInvariant()) })
        Assert-That ($bad.Count -eq 0) ('failing checks: ' + (($bad | ForEach-Object { '{0}={1}' -f $_.$A_CheckId, $_.$A_CheckStatus }) -join ', '))
        foreach ($id in $A_DoctorSetupIds) { $c = Get-DoctorCheck $r $id; Assert-That ($null -ne $c -and ($A_PassStatuses -contains ([string]$c.$A_CheckStatus).ToLowerInvariant())) "check '$id' does not pass" }
        Assert-That ($r.Exit -eq 0) "exit code is $($r.Exit)"
    }

    # ---- tokens --------------------------------------------------------------------------------------------------------
    Check 'token.create' {
        New-E2eToken 'main'   @('--scope', 'session,connection,screen,transaction', '--read-only', '--expires', '1d')
        New-E2eToken 'all'    @('--scope', '*', '--yes', '--read-only', '--expires', '1d')
        New-E2eToken 'screen' @('--scope', 'screen', '--read-only', '--expires', '1d')
        New-E2eToken 'ro'     @('--scope', 'session,screen,element', '--read-only', '--expires', '1d')
        New-E2eToken 'revoke' @('--scope', 'screen', '--read-only', '--expires', '1d')
        New-E2eToken 'ipnet'  @('--scope', 'session', $A_TokenIpFlag, '203.0.113.0/24', '--read-only', '--expires', '1d')
        New-E2eToken 'ipok'   @('--scope', 'session', $A_TokenIpFlag, '127.0.0.1', '--read-only', '--expires', '1d')
    } -Fatal

    # ======================================================================================================
    # Server 1: read-only, TLS (5-10)
    # ======================================================================================================
    Write-Host ('== server 1: mcp --http --tls (read-only, unelevated) on https://{0}:{1}/mcp' -f $Hostname, $Port)
    $script:StartError = $null
    Check 'server.start' { $script:S1 = Start-E2eServer @() $Audit1 'ro' } -Fatal
    $s1 = $script:S1
    Check 'server.banner' {
        $banner = Wait-Banner $s1 ([regex]::Escape((Expand-Tpl $A_BannerEndpoint)))
        Assert-That ($banner -match [regex]::Escape((Expand-Tpl $A_BannerEndpoint))) ("banner does not name " + (Expand-Tpl $A_BannerEndpoint))
        Assert-That ($banner -match 'read-only guard') 'banner does not say read-only'
    }
    Check 'tls.handshake' {
        $c = Open-TlsClient
        try {
            $proto = [int]$c.Ssl.SslProtocol
            Assert-That ($proto -ge 3072) "negotiated protocol $($c.Ssl.SslProtocol) is older than TLS 1.2"
            $remote = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($c.Ssl.RemoteCertificate)
            Assert-That ($remote.Thumbprint -eq $script:Cert.Thumbprint) 'the server presented a different certificate than the exported one'
            Assert-That (($remote.Subject -eq $script:Cert.Subject)) 'subject differs'
            Write-Host ('     negotiated {0}, subject {1}' -f $c.Ssl.SslProtocol, $remote.Subject)
        } finally { Close-Client $c }
    }
    Check 'http.plain_on_tls_port' {
        $tcp = New-Object Net.Sockets.TcpClient
        try {
            $tcp.Connect('127.0.0.1', $Port)
            $tcp.ReceiveTimeout = 5000
            $ns = $tcp.GetStream()
            Send-RawText $ns ("GET /mcp HTTP/1.1`r`nHost: {0}:{1}`r`nConnection: close`r`n`r`n" -f $Hostname, $Port)
            $resp = Read-RawText $ns 5000
            Assert-That ($resp -notmatch '^HTTP/1\.[01] 2\d\d') "plain HTTP on the TLS port answered: $($resp.Split("`r")[0])"
        } finally { $tcp.Close() }
    }

    # ---- 6 HTTP-level rejections (curl.exe --cacert) ------------------------------------------------------
    Check 'http.other_path_404' {
        $r = Send-Curl 'POST' '/other' (New-RpcBody 'ping') @{} 'application/json'
        Assert-That ($r.Status -eq 404) "status is $($r.Status) $($r.Err)"
    }
    Check 'http.get_405' {
        $r = Send-Curl 'GET' $A_Endpoint $null @{}
        Assert-That ($r.Status -eq 405) "status is $($r.Status) $($r.Err)"
    }
    Check 'http.content_type_415' {
        $r = Send-Curl 'POST' $A_Endpoint (New-RpcBody 'ping') (Get-AuthHeader 'main') 'text/plain'
        Assert-That ($r.Status -eq 415) "status is $($r.Status)"
    }
    Check 'http.no_token_401' {
        $r = Send-Curl 'POST' $A_Endpoint (New-RpcBody 'ping') @{}
        Assert-That ($r.Status -eq 401) "status is $($r.Status)"
        Assert-That ($r.Json.error_code -eq 'AUTH_REQUIRED') "error_code is '$($r.Json.error_code)'"
    }
    Check 'http.bad_token_401' {
        $r = Send-Curl 'POST' $A_Endpoint (New-RpcBody 'ping') @{ Authorization = ('Bearer ffy_00000000_' + ('A' * 43)) }
        Assert-That ($r.Status -eq 401) "status is $($r.Status)"
        Assert-That ($r.Json.error_code -eq 'TOKEN_INVALID') "error_code is '$($r.Json.error_code)'"
    }
    Check 'http.wrong_host_403' {
        # A forged Host header (HTTP/1.1, real token): refused by the Host allow-list, not routed elsewhere.
        $h = Get-AuthHeader 'main'; $h['Host'] = 'evil.example'
        $r = Send-Curl 'POST' $A_Endpoint (New-RpcBody 'ping') $h 'application/json' '127.0.0.1' @('--http1.1') 15
        Assert-That ($r.Status -eq $A_WrongHostStatus) "status is $($r.Status) (expected $A_WrongHostStatus)"
    }
    Check 'http.oversized_413' {
        # Headers only: declared Content-Length far above the limit, no body is ever sent. The 413 must arrive anyway.
        $c = Open-TlsClient
        try {
            $req = "POST /mcp HTTP/1.1`r`nHost: {0}:{1}`r`nAuthorization: Bearer {2}`r`nContent-Type: application/json`r`nContent-Length: {3}`r`nConnection: close`r`n`r`n" -f $Hostname, $Port, $script:Tokens['main'].Secret, $A_OversizeBytes
            Send-RawText $c.Ssl $req
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $resp = Read-RawText $c.Ssl 8000
            Assert-That ($resp -match '^HTTP/1\.[01] 413') "answer is: $($resp.Split("`r")[0])"
            Assert-That ($sw.ElapsedMilliseconds -lt 5000) "413 took $($sw.ElapsedMilliseconds) ms (the body was awaited?)"
        } finally { Close-Client $c }
    }

    # ---- 7 protocol, tools, SAP ---------------------------------------------------------------------------------------
    Check 'mcp.initialize.curl' {
        $r = Invoke-Mcp 'main' 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'e2e-curl'; version = '1.0' } }
        Assert-That ($r.Status -eq 200) "status is $($r.Status) $($r.Err)"
        Assert-That ($r.Json.result.protocolVersion -eq '2025-06-18') "protocolVersion is '$($r.Json.result.protocolVersion)'"
        Assert-That ($r.Json.result.serverInfo.name -eq 'fairyfly') 'serverInfo.name is not fairyfly'
    }
    function Assert-ToolsList($R) {
        Assert-That ($R.Status -eq 200) "status is $($R.Status) $($R.Err)"
        $names = @($R.Json.result.tools | ForEach-Object { $_.name })
        Assert-That ($names.Count -eq 20) "expected 20 tools, got $($names.Count)"
        $sorted = [string[]]$names.Clone()
        [Array]::Sort($sorted, [StringComparer]::Ordinal)
        Assert-That (($sorted -join ',') -eq ($names -join ',')) 'tools/list is not sorted by name'
        Assert-That (-not ($names -contains 'gui_element_fill')) 'gui_element_fill is listed on a read-only server'
    }
    Check 'mcp.tools_list.curl' { Assert-ToolsList (Invoke-Mcp 'all' 'tools/list' @{}) }
    if ($script:Resolvable) {
        Check 'mcp.initialize.ps' {
            $r = Invoke-Mcp 'main' 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'e2e-ps'; version = '1.0' } } @{} 'ps'
            Assert-That ($r.Status -eq 200) "status is $($r.Status)"
            Assert-That ($r.Json.result.serverInfo.name -eq 'fairyfly') 'serverInfo.name is not fairyfly'
        }
        Check 'mcp.tools_list.ps' { Assert-ToolsList (Invoke-Mcp 'all' 'tools/list' @{} @{} 'ps') }
    } else {
        Skip-Check 'mcp.initialize.ps' "host name '$Hostname' does not resolve (PowerShell cannot pin an IP: the Host header would not match)"
        Skip-Check 'mcp.tools_list.ps' "host name '$Hostname' does not resolve"
    }

    $script:SapReady = $false
    Check 'sap.gui_session_list' {
        $r = Invoke-ToolCall 'main' 'gui_session_list' @{}
        $ids = @([regex]::Matches((Get-ToolText $r), '/app/con\[\d+\]/ses\[\d+\]') | ForEach-Object { $_.Value } | Select-Object -Unique)
        Assert-That ($r.Status -eq 200 -and -not (Test-ToolError $r) -and $ids.Count -ge 1) 'no SAP GUI session reachable through the HTTP server'
        $script:FirstSession = $ids[0]
        $script:SapReady = $true
    }
    if ($script:SapReady) {
        Check 'sap.gui_screen_read' {
            $a = Invoke-ToolCall 'main' 'gui_session_attach' @{}
            if ((Test-ToolError $a) -and (Get-ErrCode $a) -eq 'MULTIPLE_SESSIONS') { $a = Invoke-ToolCall 'main' 'gui_session_attach' @{ session_id = $script:FirstSession } }
            Assert-That (-not (Test-ToolError $a)) ('attach failed: ' + (Get-ToolText $a))
            $r = Invoke-ToolCall 'main' 'gui_screen_read' @{ no_tabs = $true; only = 'fields'; max_rows = 5 }
            Assert-That ($r.Status -eq 200) "status is $($r.Status)"
            Assert-That (-not (Test-ToolError $r)) ('screen read failed: ' + (Get-ToolText $r))
            Assert-That ((Get-ToolText $r).StartsWith('SAP screen data (untrusted')) 'text does not start with the untrusted-data header'
        }
        if ($SkipSse) {
            Skip-Check 'sse.tools_call.h2' '-SkipSse'
            Skip-Check 'sse.tools_call.http1' '-SkipSse'
        } else {
            foreach ($mode in @(@('h2', @('--http2', '-N')), @('http1', @('--http1.1', '-N')))) {
                $label = $mode[0]; $flags = $mode[1]
                if ($label -eq 'h2' -and -not (Test-CurlHttp2)) {
                    Skip-Check 'sse.tools_call.h2' 'the installed curl.exe was built without HTTP/2 (curl --version lists no HTTP2 feature): SSE over HTTP/2 remains UNVERIFIED by this run; use a curl with HTTP2 support to check it'
                    continue
                }
                Check ("sse.tools_call.$label") {
                    $body = New-RpcBody 'tools/call' @{ name = 'gui_screen_read'; arguments = @{ no_tabs = $true; only = 'fields'; max_rows = 3 } }
                    $h = Get-AuthHeader 'main'; $h['Accept'] = 'text/event-stream'
                    $r = Send-Curl 'POST' $A_Endpoint $body $h 'application/json' '127.0.0.1' $flags 130
                    Assert-That ($r.Status -eq 200) "status is $($r.Status) $($r.Err)"
                    if ($label -eq 'h2') { Assert-That ($r.Version -eq '2') "HTTP version is '$($r.Version)', expected 2" }
                    else { Assert-That ($r.Version -eq '1.1') "HTTP version is '$($r.Version)', expected 1.1" }
                    Assert-That ($r.Headers['content-type'] -match 'text/event-stream') "Content-Type is '$($r.Headers['content-type'])'"
                    Assert-That ($r.Text -match '(?m)^event: message\s*$') 'no "event: message" frame'
                    $data = @($r.Text -split "`n" | Where-Object { $_ -like 'data: *' } | ForEach-Object { $_.TrimEnd("`r").Substring(6) })
                    Assert-That ($data.Count -ge 1) 'no data: line'
                    $final = ConvertFrom-JsonSafe $data[$data.Count - 1]
                    Assert-That ($null -ne $final -and $final.jsonrpc -eq '2.0' -and (Has-Prop $final 'result') -and $final.result.isError -ne $true) 'last data frame is not a successful JSON-RPC result'
                }
            }
        }
    } else {
        foreach ($n in @('sap.gui_screen_read', 'sse.tools_call.h2', 'sse.tools_call.http1')) { Skip-Check $n 'no SAP session reachable (sap.gui_session_list failed)' }
    }

    # ---- 8 authorization -------------------------------------------------------------------------------------------------
    if ($script:SapReady) {
        Check 'authz.scope_denied' {
            $r = Invoke-ToolCall 'screen' 'gui_transaction_start' @{ code = '/n' }
            Assert-That ($r.Status -eq 200 -and (Test-ToolError $r)) 'a call outside the token scope was not refused'
            Assert-That ((Get-ErrCode $r) -eq 'SCOPE_DENIED') "code is '$(Get-ErrCode $r)'"
        }
    } else {
        Skip-Check 'authz.scope_denied' 'no SAP session reachable'
    }

    # ---- 9 IP (token binding, forwarded header) -------------------------------------------------------------------------
    Check 'ip.token_bound_refused' {
        $r = Invoke-Mcp 'ipnet' 'ping' $null
        Assert-That ($r.Status -eq 403) "status is $($r.Status)"
        Assert-That ($r.Json.error_code -eq $A_ErrIpNotAllowed) "error_code is '$($r.Json.error_code)'"
    }
    Check 'ip.xff_ignored' {
        $plain = Invoke-Mcp 'ipok' 'ping' $null
        Assert-That ($plain.Status -eq 200) "plain ping status is $($plain.Status) $($plain.Json.error_code)"
        $r = Invoke-Mcp 'ipok' 'ping' $null @{ $A_XffHeader = '203.0.113.9' }
        Assert-That ($r.Status -eq 200) "spoofed $A_XffHeader changed the outcome: status $($r.Status) $($r.Json.error_code)"
    }
    Check 'ip.forwarded_cannot_bypass' {
        $r = Invoke-Mcp 'ipnet' 'ping' $null @{ $A_XffHeader = '203.0.113.9' }
        Assert-That ($r.Status -eq 403) "status is $($r.Status): the forged header must not satisfy --ip"
        Assert-That ($r.Json.error_code -eq $A_ErrIpNotAllowed) "error_code is '$($r.Json.error_code)'"
    }

    # ---- 8 revoke ----------------------------------------------------------------------------------------------------------
    Check 'token.revoke' {
        $before = Invoke-Mcp 'revoke' 'ping' $null
        Assert-That ($before.Status -eq 200) "ping before revoke: status $($before.Status)"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $rv = Invoke-FF @('mcp', 'token', 'revoke', "$Prefix-revoke", '--output', 'json', '--no-audit') 30
        Assert-That ($null -ne $rv.Json -and $rv.Json.status -eq 'success') 'token revoke failed'
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

    # ---- 10 robustness: slow bodies and half headers ----------------------------------------------------------------------
    $script:Slow = @(); $script:Half = @()
    try {
        Check 'pool.slow_bodies' {
            for ($i = 0; $i -lt 12; $i++) {
                $c = Open-TlsClient; $script:Slow += , $c
                Send-RawText $c.Ssl ("POST /mcp HTTP/1.1`r`nHost: {0}:{1}`r`nContent-Type: application/json`r`nContent-Length: 1000`r`n`r`n{{`"jsonrpc`"" -f $Hostname, $Port)
            }
            for ($i = 0; $i -lt 12; $i++) {
                $c = Open-TlsClient; $script:Half += , $c
                Send-RawText $c.Ssl ("POST /mcp HTTP/1.1`r`nHost: {0}:{1}`r`nContent-Type: application/json`r`n" -f $Hostname, $Port)
            }
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $r = Invoke-Mcp 'main' 'ping' $null
            Assert-That ($r.Status -eq 200) "legitimate request status is $($r.Status) $($r.Err)"
            Assert-That ($sw.ElapsedMilliseconds -lt 3000) "legitimate request took $($sw.ElapsedMilliseconds) ms with 24 stalled clients"
        }
        Check 'pool.half_header_closed' {
            $hc = @($script:Half)
            Assert-That ($hc.Count -eq 12) 'the half-header clients were not opened'
            $deadline = (Get-Date).AddSeconds(30)
            $closed = @{}
            while ((Get-Date) -lt $deadline -and $closed.Count -lt $hc.Count) {
                for ($i = 0; $i -lt $hc.Count; $i++) {
                    if (-not $closed.ContainsKey($i) -and (Test-SocketClosed $hc[$i].Tcp)) { $closed[$i] = $true }
                }
                Start-Sleep -Milliseconds 300
            }
            # Informational (measured, see docs/MCP_REMOTE.md threat model): http.sys applies the per-URL-group HeaderWait only to
            # requests it has routed; a socket with a PARTIAL header is closed by the machine-wide connection timer (default 120 s),
            # so it normally stays open for this 30 s window. That costs a kernel connection, not a fairyfly worker.
            Assert-That ($closed.Count -le $hc.Count) 'inconsistent socket bookkeeping'
            Info-Line ("half-header sockets: {0} of {1} still open after 30 s, {2} closed by the kernel (expected: all open until the machine timer, ~120 s)" -f ($hc.Count - $closed.Count), $hc.Count, $closed.Count)
            $r = Invoke-Mcp 'main' 'ping' $null
            Assert-That ($r.Status -eq 200) "server unhealthy after the stall test: status $($r.Status)"
        }
    } finally {
        foreach ($c in @($script:Slow + $script:Half)) { Close-Client $c }
    }

    # ---- stop server 1 (Ctrl+Break) and audit -----------------------------------------------------------------------------
    $code1 = Stop-E2eServer $s1
    Check 'server.clean_stop' {
        Assert-That ($code1 -eq 0) "exit code is $code1 (-1 = had to be killed)"
        Assert-That ($s1.StopMs -le 15000) "shutdown took $($s1.StopMs) ms"
    }
    Check 'audit.http_fields' {
        $records = @(Get-AuditRecords $s1.Audit)
        $tool = @($records | Where-Object { Has-Prop $_ 'tool' })
        Assert-That ($tool.Count -gt 0) "no tool records in the audit file"
        foreach ($rec in $tool) {
            Assert-That ((Has-Prop $rec 'transport') -and $rec.transport -eq 'http') "record of $($rec.tool) has no transport=http"
            Assert-That ((Has-Prop $rec 'principal') -and $rec.principal) "record of $($rec.tool) has no principal"
            Assert-That ((Has-Prop $rec 'remote_addr') -and $rec.remote_addr) "record of $($rec.tool) has no remote_addr"
            Assert-That ($rec.remote_addr -ne '203.0.113.9') 'a forwarded header was recorded as the remote address'
        }
        Assert-That (@($tool | Where-Object { $_.remote_addr -eq '127.0.0.1' }).Count -ge 1) 'no record with the real peer address 127.0.0.1'
    }
    Check 'audit.no_token_string' {
        Assert-NoTokenText (Get-Content -LiteralPath $s1.Audit -Raw -Encoding UTF8) 'the audit file'
        Assert-NoTokenText (Read-SharedText $s1.Err) 'the server log'
    }

    # ======================================================================================================
    # Server 2: --allow-write (a read-only token must be refused before SAP is reached)
    # ======================================================================================================
    Write-Host ('== server 2: mcp --http --tls --allow-write on https://{0}:{1}/mcp' -f $Hostname, $Port)
    Check 'server.start.rw' { $script:S2 = Start-E2eServer @($A_WriteFlag) $Audit2 'rw' } -Fatal
    $s2 = $script:S2
    Check 'authz.read_only_refused' {
        $r = Invoke-ToolCall 'ro' 'gui_element_fill' @{ element = '/app/con[0]/ses[0]/wnd[0]/usr/txtZE2E_NONE'; value = 'ZE2E' }
        Assert-That ($r.Status -eq 200 -and (Test-ToolError $r)) 'a read-only token could call gui_element_fill'
        Assert-That ((Get-ErrCode $r) -eq 'READ_ONLY') "code is '$(Get-ErrCode $r)'"
    }
    $code2 = Stop-E2eServer $s2
    Check 'server.clean_stop.rw' { Assert-That ($code2 -eq 0) "exit code is $code2 (-1 = had to be killed)" }
    Check 'authz.read_only_audit' {
        $raw = Get-Content -LiteralPath $s2.Audit -Raw -Encoding UTF8
        Assert-That (-not $raw.Contains('ZE2E')) 'the fill value appears in the audit file'
        $rec = @(Get-AuditRecords $s2.Audit | Where-Object { (Has-Prop $_ 'tool') -and $_.tool -eq 'gui_element_fill' })
        Assert-That ($rec.Count -ge 1 -and $rec[0].error_code -eq 'READ_ONLY') 'no READ_ONLY audit record for gui_element_fill'
    }

    # ======================================================================================================
    # Server 3: --allow-ip 203.0.113.0/24 (loopback always allowed, the LAN address is not)
    # ======================================================================================================
    Write-Host ('== server 3: mcp --http --tls {0} 203.0.113.0/24' -f $A_AllowIpFlag)
    Check 'server.start.allowip' { $script:S3 = Start-E2eServer @($A_AllowIpFlag, '203.0.113.0/24') $Audit3 'allowip' } -Fatal
    $s3 = $script:S3
    Check 'ip.server_allow_ip_loopback' {
        $r = Invoke-Mcp 'main' 'ping' $null
        Assert-That ($r.Status -eq 200) "loopback status is $($r.Status) $($r.Json.error_code): loopback must always be allowed"
    }
    if ($script:LanAddress) {
        Check 'ip.server_allow_ip_lan' {
            $r = Invoke-Mcp 'main' 'ping' $null @{} 'curl' $script:LanAddress
            Assert-That ($r.Status -eq 403) "status via $($script:LanAddress) is $($r.Status) $($r.Err)"
            Assert-That ($r.Json.error_code -eq $A_ErrAddrNotAllowed) "error_code is '$($r.Json.error_code)'"
        }
    } else {
        Skip-Check 'ip.server_allow_ip_lan' ("host name '$Hostname' has no resolvable non-loopback IPv4 address")
    }
    $code3 = Stop-E2eServer $s3
    Check 'server.clean_stop.allowip' { Assert-That ($code3 -eq 0) "exit code is $code3 (-1 = had to be killed)" }

    # ======================================================================================================
    # 11 teardown (ONE UAC prompt), then clean-slate assertions
    # ======================================================================================================
    Remove-E2eTokens
    Check 'token.cleanup' {
        $r = Invoke-FF @('mcp', 'token', 'list', '--output', 'json', '--no-audit') 30
        Assert-That ($r.Exit -eq 0) "token list exit code is $($r.Exit)"
        Assert-That (-not $r.Out.Contains($Prefix)) "tokens with prefix $Prefix are still listed"
    }
    Write-Host 'A UAC prompt will appear now - please approve'
    $script:TeardownResult = Invoke-FF (($A_TeardownArgs) + @('-c', $Cfg, '--yes', '--output', 'json')) 300
    Check 'teardown.apply' {
        $r = $script:TeardownResult
        Assert-That (-not $r.TimedOut) 'teardown timed out (UAC prompt not answered?)'
        Assert-That ($r.Exit -eq 0) "exit code is $($r.Exit): $($r.Err.Trim())"
        $script:TeardownDone = $true
        Assert-StateEmpty (Get-State) 'after teardown'
    }
    Check 'teardown.idempotent' {
        $r = Invoke-FF (($A_TeardownArgs) + @('-c', $Cfg, '--yes', $A_NonInteractive)) 90
        Assert-That ($r.Exit -eq 0) "exit code is $($r.Exit) (a second run must need no elevation): $($r.Err.Trim())"
        Assert-That ($r.Out.ToLowerInvariant().Contains($A_TextTeardownNothing)) "output lacks '$A_TextTeardownNothing'"
    }
    Check 'doctor.after_teardown' {
        $r = Invoke-FF ($A_DoctorArgs + @('-c', $Cfg, '--output', 'json')) 90
        Assert-That ($null -ne $r.Json) 'doctor output is not JSON'
        Assert-That ($r.Exit -eq 1) "exit code is $($r.Exit), expected 1"
        foreach ($id in $A_DoctorSetupIds) {
            $c = Get-DoctorCheck $r $id
            Assert-That ($null -ne $c -and -not ($A_PassStatuses -contains ([string]$c.$A_CheckStatus).ToLowerInvariant())) "check '$id' still passes after teardown"
        }
        Assert-That ((Get-OurCerts).Count -eq 0) 'a fairyfly certificate is still in LocalMachine\My'
    }

    # ======================================================================================================
    # 12 the other suites
    # ======================================================================================================
    $suiteNames = @('suites.unit_tests', 'suites.bigfox_regression', 'suites.mcp_smoke', 'suites.mcp_http_smoke')
    if ($SkipSuites) {
        foreach ($n in $suiteNames) { Skip-Check $n '-SkipSuites' }
    } else {
        Write-Host '== suites: unit tests and the other live suites (each must be green)'
        # The child suites must not be influenced by a leftover default %LOCALAPPDATA%\fairyfly\mcp.yaml: point them at a private,
        # comment-only config (all defaults). It is deliberately an EXISTING file: fairyfly treats an explicitly requested
        # (-c or FAIRYFLY_MCP_CONFIG) but missing config file as CONFIG_NOT_FOUND (exit 2), which would fail the stdio suites.
        $script:SuiteConfigPath = Join-Path $TempDir 'suites-neutral-mcp.yaml'
        "# neutral configuration for the child suites of the e2e run: every setting keeps its default`r`n" | Set-Content -LiteralPath $script:SuiteConfigPath -Encoding ASCII
        [Environment]::SetEnvironmentVariable('FAIRYFLY_MCP_CONFIG', $script:SuiteConfigPath)
        # A stale empty connection can push the live session off /app/con[0]: tell bigfox_regression where it really is.
        try {
            $sl = (& $Exe session list --output json 2>$null | Out-String) | ConvertFrom-Json
            $firstSes = @($sl.data.connections | ForEach-Object { $_.sessions } | Where-Object { $_ } | ForEach-Object { $_.session_id }) | Select-Object -First 1
            if ($firstSes) { [Environment]::SetEnvironmentVariable('FAIRYFLY_TEST_SESSION', [string]$firstSes) }
        } catch { }
        Check 'suites.unit_tests' {
            $ut = $null
            foreach ($c in @('build\Release\unit_tests.exe', 'build\bin\Release\unit_tests.exe', 'build\tests\Release\unit_tests.exe')) {
                $p = Join-Path $RepoRoot $c
                if (Test-Path -LiteralPath $p) { $ut = $p; break }
            }
            Assert-That ($null -ne $ut) 'unit_tests.exe not found (cmake --build build --config Release --target unit_tests)'
            [Environment]::SetEnvironmentVariable('FAIRYFLY_AUDIT', '0')
            try { $r = Invoke-Native $ut @() 900 } finally { [Environment]::SetEnvironmentVariable('FAIRYFLY_AUDIT', $null) }
            $tail = ($r.Out -split "`n" | Select-Object -Last 3) -join ' | '
            Assert-That ($r.Exit -eq 0) "unit_tests exit code $($r.Exit): $tail"
            Write-Host ('     ' + $tail.Trim())
        }
        foreach ($suite in @(@('suites.bigfox_regression', 'bigfox_regression.ps1', @('-Exe', $Exe)), @('suites.mcp_smoke', 'mcp_smoke.ps1', @('-Exe', $Exe)), @('suites.mcp_http_smoke', 'mcp_http_smoke.ps1', @('-Exe', $Exe)))) {
            $sn = $suite[0]; $sf = $suite[1]; $sa = $suite[2]
            Check $sn {
                $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot $sf)) + $sa
                $r = Invoke-Native 'powershell.exe' $args2 1800
                $summary = ($r.Out -split "`n" | Where-Object { $_ -match '^Summary:' } | Select-Object -Last 1)
                Assert-That (-not $r.TimedOut) "$sf timed out"
                if ($r.Exit -eq 2) { throw "$sf reported a missing prerequisite (exit 2): $(($r.Out -split "`n" | Select-Object -Last 4) -join ' | ')" }
                Assert-That ($r.Exit -eq 0) "$sf exit code $($r.Exit) $summary"
                Write-Host ('     ' + $summary)
            }
        }
    }
} catch {
    if ($_.Exception.Message -eq 'E2E_ABORT') {
        Write-Host 'ABORTED after a fatal failure; cleaning up.'
    } else {
        $script:Fail++
        Write-Host ('FAIL e2e.aborted - {0}' -f $_.Exception.Message)
    }
} finally {
    foreach ($srv in @($script:Servers)) {
        if ($null -ne $srv -and -not $srv.Stopped) { try { [void](Stop-E2eServer $srv) } catch { } }
    }
    try { Remove-E2eTokens } catch { Write-Host 'WARNING: token cleanup failed; list them with: fairyfly mcp token list' }
    [Net.ServicePointManager]::ServerCertificateValidationCallback = $script:SavedCallback
    [Net.ServicePointManager]::SecurityProtocol = $script:SavedProtocol
    foreach ($n in $savedEnv.Keys) { [Environment]::SetEnvironmentVariable($n, $savedEnv[$n]) }
    [Environment]::SetEnvironmentVariable('FAIRYFLY_MCP_CONFIG', $savedMcpConfig)
    try { Remove-Item -LiteralPath $TempDir -Recurse -Force -ErrorAction SilentlyContinue } catch { }
    if ($script:SetupMayHaveRun -and -not $script:TeardownDone) {
        Write-Host ''
        Write-Host 'The run stopped after setup and before a successful teardown: the certificate, URL ACL and SSL binding may still exist.'
        Write-Host ('Recovery command:  {0}' -f $RecoveryCmd)
        if ($Yes) {
            Write-Host 'A UAC prompt will appear now - please approve'
            try {
                $rec = Invoke-FF @('mcp', 'teardown', '--yes') 300
                if ($rec.Exit -eq 0) { Write-Host 'Recovery teardown finished.' } else { Write-Host ('Recovery teardown failed (exit {0}); run the command above manually.' -f $rec.Exit) }
            } catch { Write-Host 'Recovery teardown could not be started; run the command above manually.' }
        }
    }
}

Write-Host ''
Write-Host ('Summary: {0} passed, {1} failed, {2} skipped' -f $script:Pass, $script:Fail, $script:Skip)
if ($script:Fail -gt 0) { exit 1 }
exit 0
