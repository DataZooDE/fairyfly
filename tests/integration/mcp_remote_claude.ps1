<#
.SYNOPSIS
  Lets a Claude Code instance on ANOTHER machine (default: the Mac "joachims-air") use the fairyfly MCP server of this
  Windows machine over https and perform a task against the live SAP GUI (default: list the certificates in STRUST).

.DESCRIPTION
  Like mcp_remote_client.ps1 the server is reached through an SSH reverse tunnel (this machine may be NATed), so the
  certificate and URL use the host name "localhost" (no /etc/hosts change on the remote machine; Node-based clients
  trust the exported certificate via NODE_EXTRA_CA_CERTS). The remote Claude Code cannot log in from an SSH session
  (the macOS keychain is locked there), so the run happens in a Terminal window of the logged-in desktop session
  (`open -a Terminal <script.command>`); its output goes to a file that is read back over SSH.
  The token is read-only, limited to the given T-code and SAP system, expires in 1 day, and is revoked and deleted at the end;
  the files with the token are deleted from the remote machine.
  Flow: prerequisites -> mcp setup (UAC) -> token -> server -> tunnel -> files on the remote machine -> remote Claude run
  -> read the result -> cleanup (teardown = UAC). Run NON-elevated with a logged-in SAP GUI session. Nothing is changed in SAP
  (read tools only; the token cannot call write tools).

.PARAMETER Task       prompt for the remote Claude (default: STRUST certificate inventory).
.PARAMETER Tcode      T-code allowlist of the token (default STRUST).
#>
param(
    [string]$RemoteHost = 'joachims-air',
    [string]$RemoteUser = 'jr',
    [string]$RemoteClaude = '/opt/homebrew/bin/claude',
    [int]$Port = 8443,
    [string]$Tcode = 'STRUST',
    [string]$Task = '',
    [int]$TimeoutMinutes = 15,
    [string]$Exe = '',
    [switch]$NoAllowlist,
    [switch]$Yes
)
$ErrorActionPreference = 'Stop'
$OutputEncoding = New-Object System.Text.UTF8Encoding $false
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Exe) { $Exe = Join-Path $Root 'build\Release\fairyfly.exe' }
$Hostname = 'localhost'
$Ssh = Join-Path $env:SystemRoot 'System32\OpenSSH\ssh.exe'
$Scp = Join-Path $env:SystemRoot 'System32\OpenSSH\scp.exe'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Temp = Join-Path ([IO.Path]::GetTempPath()) "fairyfly-remote-claude-$Stamp"
$Cfg = Join-Path $Temp 'mcp.yaml'
$Pem = Join-Path $Temp 'fairyfly-localhost.pem'
$Dest = "$RemoteUser@$RemoteHost"
$TokenName = 'ffclaude-' + [Guid]::NewGuid().ToString('N').Substring(0, 6)
$RDir = "/tmp/ffclaude-$Stamp"
if (-not $Task) {
    $Task = "You are connected to a fairyfly MCP server that drives a live SAP GUI session (read-only token, restricted to transaction $Tcode). " +
            "Using ONLY its tools, open transaction $Tcode and find ALL certificates that are registered there: for every PSE / node in the left tree " +
            "(for example SSL server Standard, SSL client SSL Client (Anonymous), SSL client (Standard), SAML, WS Security, Database ...) list each certificate " +
            "with subject, issuer and validity end. Work step by step: start the transaction, read the screen, select the tree nodes one by one (the tree can be " +
            "operated with the element tools, using node keys that the screen read reports), and read the certificate list of each node. Do not change, add, delete or import anything and " +
            "never press Save. Finally give a compact table (PSE, certificate subject, issuer, valid to) and say how many certificates you found in total and which PSEs were empty or could not be read."
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
function Send-RemoteText([string]$Text, [string]$Command) {
    # Windows OpenSSH sometimes prints "close - IO is still pending on closed socket" to stderr when stdin closes; with
    # $ErrorActionPreference = Stop that text would abort the script, so run it with Continue.
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $Text | & $Ssh -o BatchMode=yes $Dest $Command 2>&1 | Out-Null } finally { $ErrorActionPreference = $old }
}
function Remote([string]$Command, [int]$TimeoutSec = 60) { return Run-Native $Ssh @('-o', 'BatchMode=yes', $Dest, $Command) $TimeoutSec }

$setupDone = $false; $server = $null; $tunnel = $null; $tokenCreated = $false; $exit = 0
New-Item -ItemType Directory -Force -Path $Temp | Out-Null
"# neutral config for the remote Claude run" | Set-Content -LiteralPath $Cfg -Encoding ASCII
try {
    Write-Host "== mcp_remote_claude: Claude Code on $Dest -> https://${Hostname}:$Port/mcp (reverse SSH tunnel), task: $Tcode certificates"
    $sl = FF @('session', 'list', '--output', 'json') 60
    if ($sl.Out -notmatch 'ses\[') { Write-Host 'PREREQUISITE MISSING: no logged-in SAP GUI session (fairyfly session launch Bigfox --login)'; exit 2 }
    $r = Remote "echo ok; $RemoteClaude --version" 40
    if ($r.Out -notmatch '^ok') { Write-Host "PREREQUISITE MISSING: ssh/claude on $Dest : $($r.Err.Trim())"; exit 2 }
    Write-Host ("remote claude: " + ($r.Out -split "`n" | Select-Object -Skip 1 | Select-Object -First 1).Trim())
    $r = Run-Native 'netsh.exe' @('http', 'show', 'sslcert') 30
    if ($r.Out -match ":$Port\b") { Write-Host "PREREQUISITE MISSING: an sslcert binding on port $Port exists; run: $Exe mcp teardown --yes"; exit 2 }

    Write-Host 'A UAC prompt will appear now - please approve'
    $r = FF @('mcp', 'setup', '--hostname', $Hostname, '--port', "$Port", '--self-signed', '-c', $Cfg, '--yes', '--output', 'json') 300
    $setupDone = $true
    $sj = $null; try { $sj = $r.Out | ConvertFrom-Json } catch { }
    if ($r.Exit -ne 0 -or $null -eq $sj -or [string]$sj.data.verify.status -ne 'ok') { throw ("setup failed (exit $($r.Exit)): " + $r.Out.Substring(0, [Math]::Min(300, $r.Out.Length))) }
    Write-Host 'setup ok'
    $r = FF @('mcp', 'cert', 'export', '--format', 'pem', '--out', $Pem, '--output', 'json') 60
    if ($r.Exit -ne 0 -or -not (Test-Path $Pem)) { throw "cert export failed: $($r.Out)" }

    $tokArgs = @('mcp', 'token', 'create', $TokenName, '--scope', 'session,screen,element,transaction,popup,key', '--read-only', '--expires', '1d', '--output', 'json')
    if (-not $NoAllowlist) { $tokArgs = @('mcp', 'token', 'create', $TokenName, '--scope', 'session,screen,element,transaction,popup,key', '--read-only', '--tcode', $Tcode, '--system', 'A4H/001', '--expires', '1d', '--output', 'json') }
    $r = FF $tokArgs 60
    $tj = $null; try { $tj = $r.Out | ConvertFrom-Json } catch { }
    if ($null -eq $tj -or -not $tj.data.token) { throw "token create failed: $($r.Out.Substring(0, [Math]::Min(300, $r.Out.Length)))" }
    $tokenCreated = $true; $Token = [string]$tj.data.token
    $auditFile = Join-Path $Temp 'audit.jsonl'
    $env:FAIRYFLY_AUDIT_FILE = $auditFile; Remove-Item Env:FAIRYFLY_AUDIT -ErrorAction SilentlyContinue
    $server = Start-Process -FilePath $Exe -ArgumentList @('--log-level', 'debug', 'mcp', '--http', '--tls', '--mcp-host', '+', '--allowed-hosts', $Hostname, '--mcp-port', "$Port", '-c', "`"$Cfg`"") -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $Temp 'server.err') -RedirectStandardOutput (Join-Path $Temp 'server.out')
    Start-Sleep -Seconds 3
    if ($server.HasExited) { throw ('server did not start: ' + (Get-Content (Join-Path $Temp 'server.err') -Raw)) }
    $tunnel = Start-Process -FilePath $Ssh -ArgumentList @('-N', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes', '-o', 'ServerAliveInterval=15', '-R', "127.0.0.1:${Port}:127.0.0.1:$Port", $Dest) -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $Temp 'tunnel.err')
    Start-Sleep -Seconds 4
    if ($tunnel.HasExited) { throw ('tunnel failed: ' + (Get-Content (Join-Path $Temp 'tunnel.err') -Raw)) }
    Write-Host 'server and tunnel up'

    # ---- files on the remote machine (private directory) -------------------------------------------------------------------
    [void](Remote "umask 077; mkdir -p $RDir" 30)
    $r = Run-Native $Scp @('-o', 'BatchMode=yes', $Pem, "${Dest}:$RDir/fairyfly.pem") 60
    if ($r.Exit -ne 0) { throw "scp failed: $($r.Err)" }
    $mcpJson = @{ mcpServers = @{ fairyfly = @{ type = 'http'; url = "https://${Hostname}:$Port/mcp"; headers = @{ Authorization = 'Bearer ${FAIRYFLY_TOKEN}' } } } } | ConvertTo-Json -Depth 6 -Compress
    $taskB64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Task))
    $command = @"
#!/bin/zsh
export FAIRYFLY_TOKEN='$Token'
export NODE_EXTRA_CA_CERTS='$RDir/fairyfly.pem'
cd '$RDir'
PROMPT="`$(echo '$taskB64' | base64 -d)"
echo "started `$(date)" > '$RDir/status.txt'
'$RemoteClaude' -p "`$PROMPT" --mcp-config '$RDir/mcp.json' --strict-mcp-config --allowedTools 'mcp__fairyfly' --max-turns 80 --debug mcp --output-format text > '$RDir/out.txt' 2> '$RDir/err.txt' < /dev/null
echo "exit `$?" >> '$RDir/status.txt'
D=`$(ls -t ~/.claude/debug/*.txt 2>/dev/null | head -1)
if [ -n "`$D" ]; then grep -i -E "mcp|fairyfly|tls|cert|ssl|econn|fetch" "`$D" | tail -80 > '$RDir/debug_excerpt.txt'; fi
echo "finished `$(date)" >> '$RDir/status.txt'
touch '$RDir/done'
"@
    $command = $command.Replace("`r`n", "`n")
    Send-RemoteText $mcpJson "umask 077; cat > $RDir/mcp.json"
    Send-RemoteText ($command + "`n# end") "umask 077; cat > $RDir/run.command; chmod 700 $RDir/run.command"
    $r = Remote "ls $RDir" 30
    Write-Host ("remote files: " + (($r.Out -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join ', '))
    $r = Remote "open -a Terminal $RDir/run.command && echo opened" 30
    if ($r.Out -notmatch 'opened') { throw "could not open Terminal on the remote desktop: $($r.Err)" }
    Write-Host "Claude Code is running in a Terminal window on $RemoteHost (it appears on that desktop); waiting up to $TimeoutMinutes minutes ..."

    $deadline = (Get-Date).AddMinutes($TimeoutMinutes); $done = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 15
        $r = Remote "test -f $RDir/done && echo DONE; cat $RDir/status.txt 2>/dev/null | tail -1" 30
        if ($r.Out -match 'DONE') { $done = $true; break }
    }
    $out = (Remote "cat $RDir/out.txt 2>/dev/null" 60).Out
    $err = (Remote "head -c 6000 $RDir/err.txt 2>/dev/null" 30).Out
    $status = (Remote "cat $RDir/status.txt 2>/dev/null" 30).Out
    $dbg = (Remote "cat $RDir/debug_excerpt.txt 2>/dev/null | cut -c1-300" 30).Out
    Write-Host '---------------- remote status ----------------'; Write-Host $status.Trim()
    if ($err.Trim()) { Write-Host '---------------- remote stderr (head) ----------------'; Write-Host $err.Trim() }
    $srvErr = if (Test-Path (Join-Path $Temp 'server.err')) { (Get-Content (Join-Path $Temp 'server.err') -Tail 25) -join "`n" } else { '' }
    if ($dbg.Trim()) { Write-Host '---------------- remote Claude debug (mcp/tls lines) ----------------'; Write-Host $dbg.Trim() }
    Write-Host '---------------- server stderr (tail) ----------------'; Write-Host $srvErr
    Write-Host '---------------- RESULT FROM THE REMOTE CLAUDE ----------------'
    Write-Host $out
    Write-Host '---------------- end of result ----------------'
    if (-not $done) { Write-Host 'WARNING: the remote run did not finish within the timeout'; $exit = 1 }
    $audit = if (Test-Path $auditFile) { Get-Content $auditFile } else { @() }
    $calls = @($audit | Where-Object { $_ -match '"audit_source":"mcp"' -and $_ -match '"tool"' })
    Write-Host ("server audit: {0} MCP call records (transport http); tools used: {1}" -f $calls.Count, ((($calls | ForEach-Object { if ($_ -match '"tool":"([a-z_]+)"') { $Matches[1] } }) | Group-Object | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', '))
    if (($audit -join "`n") -match [regex]::Escape($Token)) { Write-Host 'FAIL: the token string appears in the audit trail'; $exit = 1 }
}
catch { Write-Host ("ABORTED: " + $_.Exception.Message); $exit = 1 }
finally {
    Write-Host '== cleanup'
    if ($tunnel -and -not $tunnel.HasExited) { try { $tunnel.Kill() } catch { } }
    if ($server -and -not $server.HasExited) { try { $server.Kill() } catch { } }
    try { [void](Remote "rm -rf $RDir" 30) } catch { }
    if ($tokenCreated) { try { [void](FF @('mcp', 'token', 'revoke', $TokenName, '--output', 'json') 60); [void](FF @('mcp', 'token', 'delete', $TokenName, '--yes', '--output', 'json') 60) } catch { } }
    Remove-Item Env:FAIRYFLY_AUDIT_FILE -ErrorAction SilentlyContinue
    if ($setupDone) {
        Write-Host 'A UAC prompt will appear now - please approve'
        $r = FF @('mcp', 'teardown', '--hostname', $Hostname, '--port', "$Port", '-c', $Cfg, '--yes', '--output', 'json') 300
        if ($r.Exit -eq 0) { Write-Host 'teardown ok' } else { Write-Host "teardown FAILED (exit $($r.Exit)); run manually: $Exe mcp teardown --yes"; $exit = 1 }
    }
    Remove-Item -LiteralPath $Temp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $exit
