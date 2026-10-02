<#
.SYNOPSIS
  Remote client test of the fairyfly MCP server: runs the checks FROM another machine (default: the Mac "joachims-air" on
  the tailnet) over SSH, against the https listener of this Windows machine.

.DESCRIPTION
  The Windows machine may sit behind NAT (this dev box is a VirtualBox guest), so the remote machine cannot connect to it
  directly. The script therefore opens an SSH REVERSE tunnel from here to the remote machine
  (ssh -N -R 8443:127.0.0.1:8443): the remote machine's localhost:8443 reaches the server, and the remote curl uses
  `--resolve <host>:8443:127.0.0.1` so the host name still matches the certificate. What this tests for real: TLS and
  certificate trust with the remote machine's own TLS stack (curl on macOS), bearer auth, scopes, read-only, SSE, size
  and Host checks, a live SAP read. What it cannot test: network-level IP binding (the server sees loopback). That needs
  the remote machine to connect directly (Tailscale inside this machine, or a port forward).

  Flow: prerequisites -> mcp setup (ONE UAC prompt) -> token -> server (unelevated) -> tunnel -> copy the certificate ->
  remote checks over SSH -> cleanup (tunnel, server, token, mcp teardown = ONE UAC prompt).
  SSH uses the Windows OpenSSH client and therefore the 1Password SSH agent (1Password may ask for approval).
  Run it NON-elevated, with a logged-in SAP GUI session (read tools only; nothing is changed in SAP).

.PARAMETER RemoteHost   SSH host of the remote client machine (default joachims-air).
.PARAMETER RemoteUser   SSH user (default jr).
.PARAMETER Hostname     Name used in the certificate and URL (default: lower-case computer name).
.PARAMETER Port         https port (default 8443, also the remote tunnel port).
.PARAMETER Exe          fairyfly.exe (default build\Release\fairyfly.exe).
.PARAMETER Yes          do not ask before the two UAC steps.
.PARAMETER DryRun       print the plan only.
#>
param(
    [string]$RemoteHost = 'joachims-air',
    [string]$RemoteUser = 'jr',
    [string]$Hostname = '',
    [int]$Port = 8443,
    [string]$Exe = '',
    [switch]$Yes,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Exe) { $Exe = Join-Path $Root 'build\Release\fairyfly.exe' }
if (-not $Hostname) { $Hostname = [Environment]::MachineName.ToLowerInvariant() }
$Ssh = Join-Path $env:SystemRoot 'System32\OpenSSH\ssh.exe'
$Scp = Join-Path $env:SystemRoot 'System32\OpenSSH\scp.exe'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Temp = Join-Path ([IO.Path]::GetTempPath()) "fairyfly-remote-$Stamp"
$Cfg = Join-Path $Temp 'mcp.yaml'
$Pem = Join-Path $Temp "fairyfly-$Hostname.pem"
$AuditFile = Join-Path $Temp 'audit.jsonl'
$Dest = "$RemoteUser@$RemoteHost"
$TokenName = 'ffremote-' + [Guid]::NewGuid().ToString('N').Substring(0, 6)

if ($DryRun) {
    "mcp_remote_client.ps1 DRY RUN: would test https://${Hostname}:$Port/mcp from $Dest over a reverse SSH tunnel"
    '  steps: prerequisites, mcp setup (UAC), token, server, tunnel, copy certificate, remote checks, cleanup (teardown, UAC)'
    exit 0
}

$script:Fail = 0; $script:Pass = 0; $script:Skip = 0
function Result([string]$Name, [string]$State, [string]$Detail = '') {
    switch ($State) { 'ok' { $script:Pass++; Write-Host ("PASS {0}" -f $Name) } 'skip' { $script:Skip++; Write-Host ("SKIP {0} - {1}" -f $Name, $Detail) } default { $script:Fail++; Write-Host ("FAIL {0} - {1}" -f $Name, $Detail) } }
}
function Run-Native([string]$File, [string[]]$Arguments, [int]$TimeoutSec = 120) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = ($Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $p = [Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { try { $p.Kill() } catch { }; return [pscustomobject]@{ Exit = -1; Out = $o.Result; Err = 'timeout' } }
    return [pscustomobject]@{ Exit = $p.ExitCode; Out = $o.Result; Err = $e.Result }
}
function FF([string[]]$Arguments, [int]$TimeoutSec = 300) { return Run-Native $Exe $Arguments $TimeoutSec }

$setupDone = $false; $server = $null; $tunnel = $null; $tokenCreated = $false
New-Item -ItemType Directory -Force -Path $Temp | Out-Null
"# neutral config for the remote client test" | Set-Content -LiteralPath $Cfg -Encoding ASCII
try {
    Write-Host "== mcp_remote_client: $Dest -> https://${Hostname}:$Port/mcp (reverse SSH tunnel)"
    # ---- prerequisites --------------------------------------------------------------------------------------------------------
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if ((New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Write-Host 'WARNING: running elevated; the test is meant to run non-elevated' }
    $sl = FF @('session', 'list', '--output', 'json') 60
    if ($sl.Out -notmatch 'ses\[') { Write-Host 'PREREQUISITE MISSING: no logged-in SAP GUI session (fairyfly session launch Bigfox --login)'; exit 2 }
    $r = Run-Native $Ssh @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', $Dest, 'echo ok; uname -sm; sw_vers -productVersion; curl --version | head -1; command -v openssl') 40
    if ($r.Out -notmatch '^ok') { Write-Host "PREREQUISITE MISSING: ssh to $Dest failed: $($r.Err.Trim())"; exit 2 }
    Write-Host ("remote: " + (($r.Out -split "`n" | Select-Object -Skip 1 | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join ' | '))
    $r = Run-Native 'netsh.exe' @('http', 'show', 'sslcert') 30
    if ($r.Out -match ":$Port\b") { Write-Host "PREREQUISITE MISSING: an sslcert binding on port $Port exists; run: $Exe mcp teardown --yes"; exit 2 }
    if (-not $Yes) { Write-Host 'Two UAC prompts will appear (setup now, teardown at the end). Press Enter to continue.'; [void][Console]::ReadLine() }

    # ---- setup (UAC) ------------------------------------------------------------------------------------------------------------
    Write-Host 'A UAC prompt will appear now - please approve'
    $r = FF @('mcp', 'setup', '--hostname', $Hostname, '--port', "$Port", '--self-signed', '-c', $Cfg, '--yes', '--output', 'json') 300
    $setupDone = $true
    $sj = $null; try { $sj = $r.Out | ConvertFrom-Json } catch { }
    if ($r.Exit -ne 0 -or $null -eq $sj -or [string]$sj.data.verify.status -ne 'ok') { Result 'setup.apply' 'fail' ("exit $($r.Exit): " + $r.Out.Substring(0, [Math]::Min(300, $r.Out.Length))); throw 'setup failed' }
    Result 'setup.apply' 'ok'
    $r = FF @('mcp', 'cert', 'export', '--format', 'pem', '--out', $Pem, '--output', 'json') 60
    if ($r.Exit -ne 0 -or -not (Test-Path $Pem)) { Result 'cert.export' 'fail' $r.Out; throw 'cert export failed' }
    Result 'cert.export' 'ok'

    # ---- token + server -------------------------------------------------------------------------------------------------------
    $r = FF @('mcp', 'token', 'create', $TokenName, '--scope', 'session,screen,menu', '--read-only', '--expires', '1d', '--output', 'json') 60
    $tj = $null; try { $tj = $r.Out | ConvertFrom-Json } catch { }
    if ($null -eq $tj -or -not $tj.data.token) { Result 'token.create' 'fail' 'no token in the output'; throw 'token failed' }
    $tokenCreated = $true; $Token = [string]$tj.data.token
    Result 'token.create' 'ok'
    $env:FAIRYFLY_AUDIT_FILE = $AuditFile; Remove-Item Env:FAIRYFLY_AUDIT -ErrorAction SilentlyContinue
    $server = Start-Process -FilePath $Exe -ArgumentList @('mcp', '--http', '--tls', '--mcp-host', '+', '--allowed-hosts', $Hostname, '--mcp-port', "$Port", '-c', "`"$Cfg`"") -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $Temp 'server.err') -RedirectStandardOutput (Join-Path $Temp 'server.out')
    Start-Sleep -Seconds 3
    if ($server.HasExited) { Result 'server.start' 'fail' (Get-Content (Join-Path $Temp 'server.err') -Raw); throw 'server did not start' }
    Result 'server.start' 'ok'

    # ---- tunnel + certificate to the remote machine ---------------------------------------------------------------------
    $tunnel = Start-Process -FilePath $Ssh -ArgumentList @('-N', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes', '-o', 'ServerAliveInterval=15', '-R', "127.0.0.1:${Port}:127.0.0.1:$Port", $Dest) -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $Temp 'tunnel.err')
    Start-Sleep -Seconds 4
    if ($tunnel.HasExited) { Result 'tunnel' 'fail' (Get-Content (Join-Path $Temp 'tunnel.err') -Raw); throw 'tunnel failed' }
    Result 'tunnel' 'ok'
    $remotePem = "/tmp/fairyfly-$Hostname-$Stamp.pem"
    $r = Run-Native $Scp @('-o', 'BatchMode=yes', $Pem, "${Dest}:$remotePem") 60
    if ($r.Exit -ne 0) { Result 'cert.copy' 'fail' $r.Err; throw 'scp failed' }
    Result 'cert.copy' 'ok'

    # ---- checks run ON the remote machine ------------------------------------------------------------------------------
    $remote = @'
set -u
HOST='__HOST__'; PORT='__PORT__'; PEM='__PEM__'; TOK='__TOKEN__'
URL="https://$HOST:$PORT/mcp"
C() { curl -sS --max-time 40 --cacert "$PEM" --resolve "$HOST:$PORT:127.0.0.1" "$@"; }
code() { C -o /tmp/ff_body.$$ -w '%{http_code}' "$@" 2>/tmp/ff_err.$$; }
res() { echo "RESULT|$1|$2|$3"; }
J='Content-Type: application/json'
# trust: without the certificate the connection must be refused (proves the check is real), with it it must work
curl -sS --max-time 20 --resolve "$HOST:$PORT:127.0.0.1" -o /dev/null -w '%{http_code}' "$URL" >/tmp/ff_o.$$ 2>/tmp/ff_e.$$; rc=$?
if [ $rc -ne 0 ]; then res tls.untrusted_rejected ok "curl exit $rc"; else res tls.untrusted_rejected fail "connected without trusting the certificate (http $(cat /tmp/ff_o.$$))"; fi
s=$(code "$URL"); if [ "$s" = 405 ]; then res tls.trusted_get_405 ok ""; else res tls.trusted_get_405 fail "http $s $(head -c 200 /tmp/ff_err.$$)"; fi
C -v -o /dev/null "$URL" 2>/tmp/ff_v.$$; proto=$(grep -m1 -E 'SSL connection using|TLSv1|TLS 1' /tmp/ff_v.$$ | head -1); subj=$(grep -m1 -E 'subject:' /tmp/ff_v.$$ | head -1)
res tls.protocol info "${proto:-unknown} ${subj}"
s=$(code -X POST -H "$J" -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' "$URL"); if [ "$s" = 401 ]; then res auth.no_token_401 ok ""; else res auth.no_token_401 fail "http $s"; fi
s=$(code -X POST -H "$J" -H 'Authorization: Bearer ffy_deadbeef_x' -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' "$URL"); if [ "$s" = 401 ]; then res auth.bad_token_401 ok ""; else res auth.bad_token_401 fail "http $s"; fi
s=$(code -X POST -H "$J" -H 'Host: evil.example' -H "Authorization: Bearer $TOK" -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' "$URL"); if [ "$s" = 403 ]; then res http.wrong_host_403 ok ""; else res http.wrong_host_403 fail "http $s"; fi
s=$(code -X POST -H "$J" -H "Authorization: Bearer $TOK" -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"mac-curl","version":"1"}}}' "$URL"); body=$(cat /tmp/ff_body.$$)
if [ "$s" = 200 ] && echo "$body" | grep -q '"serverInfo"'; then res mcp.initialize ok ""; else res mcp.initialize fail "http $s $(echo "$body" | head -c 200)"; fi
s=$(code -X POST -H "$J" -H "Authorization: Bearer $TOK" -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' "$URL"); body=$(cat /tmp/ff_body.$$)
n=$(echo "$body" | grep -o '"name":"gui_[a-z_0-9]*"' | sort -u | wc -l | tr -d ' ')
if [ "$s" = 200 ] && [ "$n" -gt 0 ] && ! echo "$body" | grep -q '"name":"gui_element_fill"'; then res mcp.tools_list ok "$n tools, no write tool"; else res mcp.tools_list fail "http $s tools=$n"; fi
if echo "$body" | grep -q 'gui_transaction_start'; then res authz.scope_hidden fail "out-of-scope tool listed"; else res authz.scope_hidden ok "transaction tools not listed for this token"; fi
s=$(code -X POST -H "$J" -H "Authorization: Bearer $TOK" -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"gui_screen_read","arguments":{"no_tabs":true,"max_rows":3}}}' "$URL"); body=$(cat /tmp/ff_body.$$)
if [ "$s" = 200 ] && echo "$body" | grep -q 'SAP screen data'; then res sap.screen_read ok "$(echo "$body" | grep -o 'connection [0-9]*, [^\\]*' | head -1)"; else res sap.screen_read fail "http $s $(echo "$body" | head -c 200)"; fi
s=$(code -X POST -H "$J" -H "Authorization: Bearer $TOK" -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"gui_transaction_start","arguments":{"code":"SM50"}}}' "$URL"); body=$(cat /tmp/ff_body.$$)
if echo "$body" | grep -qE 'SCOPE_DENIED|not found|unknown tool|-32602'; then res authz.scope_denied ok ""; else res authz.scope_denied fail "http $s $(echo "$body" | head -c 160)"; fi
s=$(code -X POST -H "$J" -H "Authorization: Bearer $TOK" -d '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"gui_element_fill","arguments":{"element":"/app/con[0]/ses[0]/wnd[0]/usr/txtX","value":"x"}}}' "$URL"); body=$(cat /tmp/ff_body.$$)
if echo "$body" | grep -qE 'READ_ONLY|TOOL_UNAVAILABLE|SCOPE_DENIED|not found|-32602'; then res authz.write_refused ok ""; else res authz.write_refused fail "http $s $(echo "$body" | head -c 160)"; fi
C -N -o /tmp/ff_sse.$$ -D /tmp/ff_sse_h.$$ -X POST -H "$J" -H 'Accept: text/event-stream' -H "Authorization: Bearer $TOK" -d '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"_meta":{"progressToken":"p1"},"name":"gui_screen_read","arguments":{"no_tabs":true,"max_rows":2}}}' "$URL" 2>/tmp/ff_err.$$
if grep -qi 'text/event-stream' /tmp/ff_sse_h.$$ && grep -q 'event: message' /tmp/ff_sse.$$; then res sse.tools_call ok "$(grep -c '^event:' /tmp/ff_sse.$$) events"; else res sse.tools_call fail "$(head -c 200 /tmp/ff_sse_h.$$ | tr '\r\n' ' ')"; fi
C --http2 -N -o /tmp/ff_sse2.$$ -D /tmp/ff_sse2_h.$$ -X POST -H "$J" -H 'Accept: text/event-stream' -H "Authorization: Bearer $TOK" -d '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"_meta":{"progressToken":"p2"},"name":"gui_screen_read","arguments":{"no_tabs":true,"max_rows":2}}}' "$URL" 2>/tmp/ff_err.$$
first=$(head -1 /tmp/ff_sse2_h.$$ | tr -d '')
if echo "$first" | grep -q '^HTTP/2 200' && grep -q 'event: message' /tmp/ff_sse2.$$; then res sse.http2 ok "$first"; else res sse.http2 fail "$first $(head -c 160 /tmp/ff_err.$$)"; fi
C --http2 -o /tmp/ff_h2.$$ -D /tmp/ff_h2_h.$$ -X POST -H "$J" -H "Authorization: Bearer $TOK" -d '{"jsonrpc":"2.0","id":8,"method":"tools/list"}' "$URL" 2>/tmp/ff_err.$$
first=$(head -1 /tmp/ff_h2_h.$$ | tr -d '')
if echo "$first" | grep -q '^HTTP/2 200' && grep -q '"tools"' /tmp/ff_h2.$$; then res http2.tools_list ok "$first"; else res http2.tools_list fail "$first $(head -c 160 /tmp/ff_err.$$)"; fi
head -c 1200000 /dev/zero | tr '\0' 'a' > /tmp/ff_big.$$; s=$(code -X POST -H "$J" -H "Authorization: Bearer $TOK" --data-binary @/tmp/ff_big.$$ "$URL")
if [ "$s" = 413 ]; then res http.oversized_413 ok ""; else res http.oversized_413 fail "http $s"; fi
rm -f /tmp/ff_*.$$
# the token is revoked from the Windows side after this script returns; the revoked check is done by the caller
'@
    $remote = $remote.Replace('__HOST__', $Hostname).Replace('__PORT__', "$Port").Replace('__PEM__', $remotePem).Replace('__TOKEN__', $Token).Replace("`r`n", "`n")
    $r = $remote | & $Ssh -o BatchMode=yes $Dest 'bash -s' 2>&1 | Out-String
    $seen = @{}
    foreach ($line in ($r -split "`n")) {
        if ($line -match '^RESULT\|([^|]+)\|([^|]+)\|(.*)$') {
            $n = $Matches[1]; $st = $Matches[2]; $d = $Matches[3].Trim(); $seen[$n] = $true
            if ($st -eq 'info') { Write-Host ("INFO {0}: {1}" -f $n, $d) } elseif ($st -eq 'ok') { Result $n 'ok' } else { Result $n 'fail' $d }
        }
    }
    if ($seen.Count -lt 12) { Result 'remote.checks_ran' 'fail' ("only $($seen.Count) results; output: " + $r.Substring(0, [Math]::Min(400, $r.Length))) }

    # ---- revoke, then the remote must be refused -----------------------------------------------------------------------
    [void](FF @('mcp', 'token', 'revoke', $TokenName, '--output', 'json') 60)
    Start-Sleep -Seconds 6
    $chk = @"
curl -sS --max-time 20 --cacert '$remotePem' --resolve '${Hostname}:${Port}:127.0.0.1' -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' -H 'Authorization: Bearer $Token' -d '{"jsonrpc":"2.0","id":9,"method":"tools/list"}' 'https://${Hostname}:${Port}/mcp'
"@
    # PowerShell appends CRLF to text piped into a native command: end with a comment so the stray CR is harmless
    $chk = $chk.Replace("`r`n", "`n") + "`n# end"
    $code = ($chk | & $Ssh -o BatchMode=yes $Dest 'bash -s' 2>&1 | Out-String).Trim()
    if ($code -eq '401') { Result 'auth.revoked_401' 'ok' } else { Result 'auth.revoked_401' 'fail' "http $code" }
    $audit = if (Test-Path $AuditFile) { Get-Content $AuditFile -Raw } else { '' }
    if ($audit -match '"transport":"http"' -and $audit -notmatch [regex]::Escape($Token)) { Result 'audit.http_no_token' 'ok' } else { Result 'audit.http_no_token' 'fail' 'audit missing http records or contains the token' }
}
catch { Write-Host ("ABORTED: " + $_.Exception.Message); $script:Fail++ }
finally {
    Write-Host '== cleanup'
    if ($tunnel -and -not $tunnel.HasExited) { try { $tunnel.Kill() } catch { } }
    if ($server -and -not $server.HasExited) { try { $server.Kill() } catch { } }
    try { [void](Run-Native $Ssh @('-o', 'BatchMode=yes', $Dest, "rm -f /tmp/fairyfly-*.pem") 30) } catch { }
    if ($tokenCreated) { try { [void](FF @('mcp', 'token', 'delete', $TokenName, '--yes', '--output', 'json') 60) } catch { } }
    Remove-Item Env:FAIRYFLY_AUDIT_FILE -ErrorAction SilentlyContinue
    if ($setupDone) {
        Write-Host 'A UAC prompt will appear now - please approve'
        $r = FF @('mcp', 'teardown', '--hostname', $Hostname, '--port', "$Port", '-c', $Cfg, '--yes', '--output', 'json') 300
        if ($r.Exit -eq 0) { Result 'teardown.apply' 'ok' } else { Result 'teardown.apply' 'fail' ("exit $($r.Exit); run manually: $Exe mcp teardown --yes") }
    }
    Remove-Item -LiteralPath $Temp -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host ("Summary: {0} passed, {1} failed, {2} skipped" -f $script:Pass, $script:Fail, $script:Skip)
}
if ($script:Fail -gt 0) { exit 1 } else { exit 0 }
