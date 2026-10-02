<#
.SYNOPSIS
  User test of fairyfly over the remote MCP: Claude Code on another machine (default: the Mac "joachims-air") works through
  realistic SAP Basis tasks against the live SAP GUI of this Windows machine, using the https MCP server through an SSH
  reverse tunnel, with READ-ONLY tokens restricted to the T-codes and the SAP system of each task. Every task also asks
  the remote Claude for a candid "tool experience" report (errors, retries, slow or confusing calls, missing capabilities)
  so that bugs and usability problems surface. The server audit trail adds per-task call counts, error codes and durations.

.DESCRIPTION
  Same transport as mcp_remote_claude.ps1 (host name "localhost" in the certificate, NODE_EXTRA_CA_CERTS on the remote
  machine, Claude Code started in a Terminal window of the remote desktop session because SSH sessions cannot use the
  macOS keychain). One mcp setup (UAC), one token per task (read-only, --tcode list, --system A4H/001, expires in 1 day),
  one teardown (UAC) at the end. Nothing is changed in SAP: the tokens cannot call write tools, and the prompts forbid
  changes. Run NON-elevated with a logged-in SAP GUI session.
#>
param(
    [string]$RemoteHost = 'joachims-air',
    [string]$RemoteUser = 'jr',
    [string]$RemoteClaude = '/opt/homebrew/bin/claude',
    [int]$Port = 8443,
    [string[]]$Only = @(),
    [int]$TaskTimeoutMinutes = 12,
    [string]$Exe = '',
    [string]$OutDir = '',
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
if (-not $OutDir) { $OutDir = Join-Path $Root "scratch\usertest-$Stamp" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Temp = Join-Path ([IO.Path]::GetTempPath()) "fairyfly-usertest-$Stamp"
$Cfg = Join-Path $Temp 'mcp.yaml'
$Pem = Join-Path $Temp 'fairyfly-localhost.pem'
$AuditFile = Join-Path $Temp 'audit.jsonl'
$Dest = "$RemoteUser@$RemoteHost"
$RDir = "/tmp/ffusertest-$Stamp"
$Rand = [Guid]::NewGuid().ToString('N').Substring(0, 6)

$Preamble = "You are a SAP Basis administrator's assistant. You are connected to a fairyfly MCP server that drives a LIVE SAP GUI session of a test system (A4H, client 001). " +
            "Your token is READ-ONLY and restricted to the transactions needed for this task. Use ONLY the fairyfly tools. Never change, create, delete, release, lock, unlock or save anything and never press Save. " +
            "If something you need is impossible with these tools, say so plainly instead of improvising. Work efficiently: prefer targeted reads (tab, only, text_contains, max_rows) over dumping whole screens. "
$Postamble = " FINISH with two sections. (1) RESULT: the answer for the administrator, compact. (2) TOOL EXPERIENCE: be candid and specific, as in a bug report: every tool error or retry (with the error code), calls that were slow or returned too much or too little data, confusing or inconsistent output, things you wanted to do but could not with the available tools or the read-only restriction, anything that wasted steps, and a rating 1-5 of how practical this tool set is for this task."
$Tasks = @(
    [pscustomobject]@{ Name = 'strust'; Tcodes = 'STRUST'; Prompt = 'TASK: list all certificates registered in STRUST (every PSE node, each certificate with subject and validity end) and point out expired or soon-expiring ones.' },
    [pscustomobject]@{ Name = 'sessions'; Tcodes = 'SM04,SM50,SM51'; Prompt = 'TASK: who is logged on right now (SM04: users, terminals, types) and what are the work processes doing (SM50: type, status, program, user, runtime)? Also say how many application servers are active (SM51). Flag anything unusual (long-running work processes, many sessions of one user).' },
    [pscustomobject]@{ Name = 'syslog'; Tcodes = 'SM21'; Prompt = 'TASK: review the system log (SM21) for today and the last hours: how many entries, which message classes/problem classes dominate, any errors or warnings an administrator should look at, and which users/programs/processes are involved. Use the selection defaults if you cannot change them.' },
    [pscustomobject]@{ Name = 'dumps'; Tcodes = 'ST22'; Prompt = 'TASK: are there ABAP short dumps (ST22) from today or yesterday? If yes: how many, which runtime errors, which programs and users, and the most likely cause of the most frequent one. If there are none, say so and show how you verified it.' },
    [pscustomobject]@{ Name = 'userinfo'; Tcodes = 'SU01,SU01D'; Prompt = 'TASK: show the master data of the user DEVELOPER (SU01 display): user type, validity period, lock status, assigned roles and profiles, last logon. NOTE: you may not be able to type into fields with a read-only token; if so, find out what is still possible (for example F4 value help or other reads), report what blocks you, and give the best partial answer.' }
)
if ($Only.Count -gt 0) { $Tasks = @($Tasks | Where-Object { $Only -contains $_.Name }) }

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
function Remote([string]$Command, [int]$TimeoutSec = 60) { return Run-Native $Ssh @('-o', 'BatchMode=yes', $Dest, $Command) $TimeoutSec }

$setupDone = $false; $server = $null; $tunnel = $null; $tokenNames = @(); $exit = 0; $summary = @()
New-Item -ItemType Directory -Force -Path $Temp | Out-Null
"# neutral config for the user test" | Set-Content -LiteralPath $Cfg -Encoding ASCII
try {
    Write-Host "== mcp_remote_usertest: Claude Code on $Dest -> https://${Hostname}:$Port/mcp ; tasks: $(($Tasks | ForEach-Object { $_.Name }) -join ', ') ; results in $OutDir"
    $sl = FF @('session', 'list', '--output', 'json') 60
    if ($sl.Out -notmatch 'ses\[') { Write-Host 'PREREQUISITE MISSING: no logged-in SAP GUI session (fairyfly session launch Bigfox --login)'; exit 2 }
    $r = Remote "echo ok; $RemoteClaude --version" 40
    if ($r.Out -notmatch '^ok') { Write-Host "PREREQUISITE MISSING: ssh/claude on $Dest : $($r.Err.Trim())"; exit 2 }
    $r = Run-Native 'netsh.exe' @('http', 'show', 'sslcert') 30
    if ($r.Out -match ":$Port\b") { Write-Host "PREREQUISITE MISSING: an sslcert binding on port $Port exists; run: $Exe mcp teardown --yes"; exit 2 }

    Write-Host 'A UAC prompt will appear now - please approve'
    $r = FF @('mcp', 'setup', '--hostname', $Hostname, '--port', "$Port", '--self-signed', '-c', $Cfg, '--yes', '--output', 'json') 300
    $setupDone = $true
    $sj = $null; try { $sj = $r.Out | ConvertFrom-Json } catch { }
    if ($r.Exit -ne 0 -or $null -eq $sj -or [string]$sj.data.verify.status -ne 'ok') { throw ("setup failed (exit $($r.Exit)): " + $r.Out.Substring(0, [Math]::Min(300, $r.Out.Length))) }
    $r = FF @('mcp', 'cert', 'export', '--format', 'pem', '--out', $Pem, '--output', 'json') 60
    if ($r.Exit -ne 0 -or -not (Test-Path $Pem)) { throw "cert export failed: $($r.Out)" }

    $tokens = @{}
    foreach ($t in $Tasks) {
        $name = "ffut-$Rand-$($t.Name)"
        $r = FF @('mcp', 'token', 'create', $name, '--scope', 'session,screen,element,transaction,popup,key', '--read-only', '--tcode', $t.Tcodes, '--system', 'A4H/001', '--expires', '1d', '--output', 'json') 60
        $tj = $null; try { $tj = $r.Out | ConvertFrom-Json } catch { }
        if ($null -eq $tj -or -not $tj.data.token) { throw "token create failed for $($t.Name): $($r.Out.Substring(0, [Math]::Min(300, $r.Out.Length)))" }
        $tokenNames += $name; $tokens[$t.Name] = [string]$tj.data.token
    }
    $env:FAIRYFLY_AUDIT_FILE = $AuditFile; Remove-Item Env:FAIRYFLY_AUDIT -ErrorAction SilentlyContinue
    $server = Start-Process -FilePath $Exe -ArgumentList @('--log-level', 'debug', 'mcp', '--http', '--tls', '--mcp-host', '+', '--allowed-hosts', $Hostname, '--mcp-port', "$Port", '-c', "`"$Cfg`"") -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $Temp 'server.err') -RedirectStandardOutput (Join-Path $Temp 'server.out')
    Start-Sleep -Seconds 3
    if ($server.HasExited) { throw ('server did not start: ' + (Get-Content (Join-Path $Temp 'server.err') -Raw)) }
    $tunnel = Start-Process -FilePath $Ssh -ArgumentList @('-N', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes', '-o', 'ServerAliveInterval=15', '-R', "127.0.0.1:${Port}:127.0.0.1:$Port", $Dest) -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $Temp 'tunnel.err')
    Start-Sleep -Seconds 4
    if ($tunnel.HasExited) { throw ('tunnel failed: ' + (Get-Content (Join-Path $Temp 'tunnel.err') -Raw)) }
    [void](Remote "umask 077; mkdir -p $RDir" 30)
    $r = Run-Native $Scp @('-o', 'BatchMode=yes', $Pem, "${Dest}:$RDir/fairyfly.pem") 60
    if ($r.Exit -ne 0) { throw "scp failed: $($r.Err)" }
    Write-Host 'server, tunnel and certificate ready'

    foreach ($t in $Tasks) {
        $n = $t.Name; $tok = $tokens[$n]
        Write-Host ("== task {0} (T-codes {1})" -f $n, $t.Tcodes)
        $prompt = $Preamble + $t.Prompt + $Postamble
        $mcpJson = @{ mcpServers = @{ fairyfly = @{ type = 'http'; url = "https://${Hostname}:$Port/mcp"; headers = @{ Authorization = 'Bearer ${FAIRYFLY_TOKEN}' } } } } | ConvertTo-Json -Depth 6 -Compress
        $promptB64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($prompt))
        $command = @"
#!/bin/zsh
export FAIRYFLY_TOKEN='$tok'
export NODE_EXTRA_CA_CERTS='$RDir/fairyfly.pem'
cd '$RDir'
PROMPT="`$(echo '$promptB64' | base64 -d)"
echo "started `$(date +%s)" > '$RDir/$n.status'
'$RemoteClaude' -p "`$PROMPT" --mcp-config '$RDir/mcp.json' --strict-mcp-config --allowedTools 'mcp__fairyfly' --max-turns 120 --debug mcp --output-format text > '$RDir/$n.out' 2> '$RDir/$n.err' < /dev/null
echo "exit `$?" >> '$RDir/$n.status'
D=`$(ls -t ~/.claude/debug/*.txt 2>/dev/null | head -1)
if [ -n "`$D" ]; then grep -E "tool error|CALL_TIMEOUT|SERVER_BUSY|Connection failed|failed after|still running" "`$D" | tail -60 > '$RDir/$n.dbg'; fi
echo "finished `$(date +%s)" >> '$RDir/$n.status'
touch '$RDir/$n.done'
"@
        $command = $command.Replace("`r`n", "`n") + "`n# end"
        $mcpJson | & $Ssh -o BatchMode=yes $Dest "umask 077; cat > $RDir/mcp.json" 2>&1 | Out-Null
        $command | & $Ssh -o BatchMode=yes $Dest "umask 077; cat > $RDir/$n.command; chmod 700 $RDir/$n.command" 2>&1 | Out-Null
        # start every task from the SAP start screen so the tasks do not depend on each other
        [void](FF @('transaction', 'start', '/nS000', '--output', 'json') 60)
        $r = Remote "open -a Terminal $RDir/$n.command && echo opened" 30
        if ($r.Out -notmatch 'opened') { Write-Host "  could not open Terminal on the remote desktop: $($r.Err)"; $exit = 1; continue }
        $deadline = (Get-Date).AddMinutes($TaskTimeoutMinutes); $done = $false
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 15
            $r = Remote "test -f $RDir/$n.done && echo DONE" 30
            if ($r.Out -match 'DONE') { $done = $true; break }
        }
        $out = (Remote "cat $RDir/$n.out 2>/dev/null" 60).Out
        $dbg = (Remote "cat $RDir/$n.dbg 2>/dev/null | cut -c1-260" 30).Out
        $st = (Remote "cat $RDir/$n.status 2>/dev/null" 30).Out
        $errTxt = (Remote "head -c 1500 $RDir/$n.err 2>/dev/null" 30).Out
        $out | Set-Content -LiteralPath (Join-Path $OutDir "$n.result.txt") -Encoding UTF8
        $dbg | Set-Content -LiteralPath (Join-Path $OutDir "$n.client-errors.txt") -Encoding UTF8
        $secs = ''
        if ($st -match 'started (\d+)' ) { $a = [int64]$Matches[1]; if ($st -match 'finished (\d+)') { $secs = [int64]$Matches[1] - $a } }
        Write-Host ("  finished={0} duration={1}s result={2} chars; client-side tool errors/timeouts: {3}" -f $done, $secs, $out.Length, @($dbg -split "`n" | Where-Object { $_.Trim() }).Count)
        if (-not $done) { $exit = 1 }
        if ($errTxt.Trim()) { Write-Host ("  stderr: " + $errTxt.Trim().Substring(0, [Math]::Min(300, $errTxt.Trim().Length))) }
        $summary += [pscustomobject]@{ Task = $n; Done = $done; Seconds = $secs }
    }

    # ---- per-task statistics from the server audit trail ----------------------------------------------------------------------
    Write-Host '== server audit statistics per task (token = task)'
    $recs = @()
    if (Test-Path $AuditFile) { foreach ($l in (Get-Content $AuditFile)) { try { $recs += ($l | ConvertFrom-Json) } catch { } } }
    foreach ($t in $Tasks) {
        $pn = "ffut-$Rand-$($t.Name)"
        $mine = @($recs | Where-Object { $_.principal -eq $pn -and $_.tool })
        $errs = @($mine | Where-Object { $_.status -ne 'success' })
        $byTool = ($mine | Group-Object tool | Sort-Object Count -Descending | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', '
        $codes = ($errs | Group-Object error_code | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', '
        $slow = @($mine | Where-Object { $_.duration_ms -gt 5000 })
        Write-Host ("  {0}: {1} calls, {2} not successful [{3}], {4} slower than 5 s; tools: {5}" -f $t.Name, $mine.Count, $errs.Count, $codes, $slow.Count, $byTool)
    }
    if (($recs | ForEach-Object { $_ | ConvertTo-Json -Compress }) -join "`n" -match 'ffy_[0-9a-f]{8}_') { Write-Host 'FAIL: a token string appears in the audit trail'; $exit = 1 }
    Copy-Item -LiteralPath $AuditFile -Destination (Join-Path $OutDir 'audit.jsonl') -ErrorAction SilentlyContinue
    if (Test-Path (Join-Path $Temp 'server.err')) { Copy-Item -LiteralPath (Join-Path $Temp 'server.err') -Destination (Join-Path $OutDir 'server.err.txt') -ErrorAction SilentlyContinue }
}
catch { Write-Host ("ABORTED: " + $_.Exception.Message); $exit = 1 }
finally {
    Write-Host '== cleanup'
    if ($tunnel -and -not $tunnel.HasExited) { try { $tunnel.Kill() } catch { } }
    if ($server -and -not $server.HasExited) { try { $server.Kill() } catch { } }
    try { [void](Remote "rm -rf $RDir" 30) } catch { }
    foreach ($tn in $tokenNames) { try { [void](FF @('mcp', 'token', 'revoke', $tn, '--output', 'json') 60); [void](FF @('mcp', 'token', 'delete', $tn, '--yes', '--output', 'json') 60) } catch { } }
    Remove-Item Env:FAIRYFLY_AUDIT_FILE -ErrorAction SilentlyContinue
    if ($setupDone) {
        Write-Host 'A UAC prompt will appear now - please approve'
        $r = FF @('mcp', 'teardown', '--hostname', $Hostname, '--port', "$Port", '-c', $Cfg, '--yes', '--output', 'json') 300
        if ($r.Exit -eq 0) { Write-Host 'teardown ok' } else { Write-Host "teardown FAILED (exit $($r.Exit)); run manually: $Exe mcp teardown --yes"; $exit = 1 }
    }
    Remove-Item -LiteralPath $Temp -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "results: $OutDir"
}
exit $exit
