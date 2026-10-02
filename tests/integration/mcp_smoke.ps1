<#
.SYNOPSIS
  Live smoke test of the fairyfly MCP server (`fairyfly mcp`) against a logged-in SAP GUI session.

.DESCRIPTION
  Spawns `fairyfly mcp` with redirected stdio, speaks newline-delimited JSON-RPC (2.0) to it and checks
  protocol behavior, read tools, read-only refusals, the audit trail, and (unless -SkipWriteMode) write mode.

  Prerequisites: Windows PowerShell 5.1, SAP GUI with scripting enabled, a logged-in session (SAP Easy Access,
  no popup), a built fairyfly.exe. No credentials are read or sent; nothing is logged on.

  Presses / changes: navigates with /nSM37 and /n, reads the screen, and in write mode fills the SM37 job name
  field with a marker (ZMCPSMOKE) and restores it to '*'. Never presses Save, Delete, Release, Stop or any other
  button: the refusal checks run against a read-only server, which refuses them before SAP is touched.

  Exit code: 0 all checks passed, 1 at least one check failed, 2 no SAP session (or no session reachable).

.PARAMETER Exe
  Path to fairyfly.exe (default: build\Release\fairyfly.exe, then build\bin\Release\fairyfly.exe under the repo root).
.PARAMETER AuditFile
  Audit file for the first server (default: a temp file). Servers 2 and 3 use <name>-write.jsonl / <name>-cap.jsonl.
.PARAMETER DryRun
  Print the plan and exit without spawning the server.
.PARAMETER SkipWriteMode
  Skip the --allow-write server and the FAIRYFLY_READ_ONLY hard-cap server.
#>
[CmdletBinding()]
param(
    [string]$Exe = '',
    [string]$AuditFile = '',
    [switch]$DryRun,
    [switch]$SkipWriteMode
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# ---- SM37 element ids (selection screen) -------------------------------------------------------------
$FinishedId = '/app/con[0]/ses[0]/wnd[0]/usr/chkBTCH2170-FINISHED'
$JobNameId  = '/app/con[0]/ses[0]/wnd[0]/usr/txtBTCH2170-JOBNAME'
$SaveButton = '/app/con[0]/ses[0]/wnd[0]/tbar[0]/btn[11]'
$Marker     = 'ZMCPSMOKE'

# ---- Exe and audit file resolution --------------------------------------------------------------------
if (-not $Exe) {
    $candidates = @((Join-Path $RepoRoot 'build\Release\fairyfly.exe'), (Join-Path $RepoRoot 'build\bin\Release\fairyfly.exe'))
    $Exe = $candidates[0]
    foreach ($c in $candidates) { if (Test-Path -LiteralPath $c) { $Exe = $c; break } }
}
if (-not $AuditFile) {
    $AuditFile = Join-Path ([IO.Path]::GetTempPath()) ('fairyfly-mcp-smoke-{0}.jsonl' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$AuditBase  = $AuditFile -replace '\.jsonl$', ''
$AuditWrite = $AuditBase + '-write.jsonl'
$AuditCap   = $AuditBase + '-cap.jsonl'

# ---- Plan (kept in sync with the checks below) --------------------------------------------------------
$Plan = @(
    'server 1: mcp (read-only default), FAIRYFLY_AUDIT_FILE=<AuditFile>',
    '  protocol.initialize            echoes requested version, serverInfo.name = fairyfly',
    '  protocol.tools_list            20 tools, additionalProperties false, annotations, no gui_element_fill',
    '  protocol.ping',
    '  protocol.unknown_method        -32601',
    '  protocol.server_discover       -32601',
    '  protocol.unknown_tool          -32602',
    '  protocol.malformed_json        -32700',
    '  protocol.alive_after_malformed ping still answered',
    '  sap.sessions                   >= 1 session (exit 2 when none)',
    '  sap.attach                     gui_session_attach (falls back to the first session id on MULTIPLE_SESSIONS)',
    '  sap.tcode_sm37                 gui_transaction_start /nSM37',
    '  sap.screen_read                untrusted header, "Simple Job Selection"',
    '  sap.get_checkbox               FINISHED checkbox: selected (bool) and label',
    '  sap.screen_find                id_contains chkBTCH2170 returns matches',
    '  sap.capture_png                scale 0.4 -> image block, PNG magic',
    '  sap.menu_list                  menu tree',
    '  readonly.click_save            READ_ONLY_REFUSED (nothing pressed)',
    '  readonly.send_key_f11          READ_ONLY_REFUSED',
    '  readonly.launch_end            READ_ONLY_REFUSED',
    '  readonly.hidden_fill           TOOL_UNAVAILABLE_READ_ONLY',
    '  batch.mixed                    tcode /n, send_key f11 (refused), sessions; stop_on_error=false',
    '  sap.close_popup_none           NO_POPUP',
    '  final.tcode_home               gui_transaction_start /n',
    '  server.exit_code_0             stdin closed after all responses, exit 0',
    '  server.stdout_clean            every stdout line is valid JSON-RPC',
    '  audit.*                        one record per call, audit_source mcp, tool/client, mcp started+stopped,',
    '                                 read_only true, refusals carry error_code, no screen text leaked',
    'server 2: mcp --allow-write (skipped with -SkipWriteMode)',
    '  write.tools_list               21 tools incl. gui_element_fill',
    '  write.attach / write.tcode_sm37',
    '  write.fill_echo_not_audited    fill job name = ZMCPSMOKE; echoed in the response (read back), never in the audit; restore "*" and /n',
    '  write.exit_and_audit',
    'server 3: mcp --allow-write with FAIRYFLY_READ_ONLY=1 (skipped with -SkipWriteMode)',
    '  cap.env_hard_cap               still 20 tools, no gui_element_fill; exit 0',
    'never pressed: Save, Delete, Release, Stop, Create, Change; no credentials are used'
)

if ($DryRun) {
    Write-Host 'mcp_smoke.ps1 DRY RUN (no server is spawned, SAP is not touched)'
    Write-Host ('Exe:       {0}  (exists: {1})' -f $Exe, (Test-Path -LiteralPath $Exe))
    Write-Host ('AuditFile: {0}' -f $AuditFile)
    if (-not $SkipWriteMode) { Write-Host ('           {0}' -f $AuditWrite); Write-Host ('           {0}' -f $AuditCap) }
    Write-Host ('SkipWriteMode: {0}' -f [bool]$SkipWriteMode)
    Write-Host 'Plan:'
    foreach ($line in $Plan) {
        if ($SkipWriteMode -and ($line -like 'server 2*' -or $line -like 'server 3*' -or $line -like '  write.*' -or $line -like '  cap.*')) { continue }
        Write-Host $line
    }
    exit 0
}

if (-not (Test-Path -LiteralPath $Exe)) { Write-Host "fairyfly.exe not found: $Exe"; exit 1 }

# ---- Server and JSON-RPC helpers ----------------------------------------------------------------------
function Start-Server([string]$Arguments, [string]$AuditPath, [hashtable]$ExtraEnv = @{}) {
    if (Test-Path -LiteralPath $AuditPath) { Remove-Item -LiteralPath $AuditPath -Force }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $Arguments
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    foreach ($name in @('FAIRYFLY_AUDIT', 'FAIRYFLY_READ_ONLY')) {
        if ($psi.EnvironmentVariables.ContainsKey($name)) { $psi.EnvironmentVariables.Remove($name) }
    }
    $psi.EnvironmentVariables['FAIRYFLY_AUDIT_FILE'] = $AuditPath
    foreach ($k in $ExtraEnv.Keys) { $psi.EnvironmentVariables[$k] = [string]$ExtraEnv[$k] }
    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.BeginErrorReadLine()   # drain stderr (logs) so the server never blocks on a full pipe
    return @{ Proc = $proc; Pending = $null; Lines = (New-Object System.Collections.ArrayList); NextId = 1
              Calls = 0; Eof = $false; Stopped = $false; ExitCode = $null; Audit = $AuditPath }
}

function Send-Line($Srv, [string]$Line) {
    $Srv.Proc.StandardInput.WriteLine($Line)
    $Srv.Proc.StandardInput.Flush()
}

function Read-ServerLine($Srv, [int]$TimeoutMs) {
    if ($Srv.Eof) { return $null }
    if ($null -eq $Srv.Pending) { $Srv.Pending = $Srv.Proc.StandardOutput.ReadLineAsync() }
    if (-not $Srv.Pending.Wait($TimeoutMs)) { return $null }
    $line = $Srv.Pending.Result
    $Srv.Pending = $null
    if ($null -eq $line) { $Srv.Eof = $true; return $null }
    [void]$Srv.Lines.Add($line)
    return $line
}

function ConvertFrom-JsonSafe([string]$Text) {
    try { return ($Text | ConvertFrom-Json) } catch { return $null }
}

function Invoke-Rpc($Srv, [string]$Method, $Params = $null, [int]$TimeoutMs = 60000) {
    $id = $Srv.NextId
    $Srv.NextId = $id + 1
    $msg = [ordered]@{ jsonrpc = '2.0'; id = $id; method = $Method }
    if ($null -ne $Params) { $msg['params'] = $Params }
    Send-Line $Srv ($msg | ConvertTo-Json -Compress -Depth 20)
    while ($true) {
        $line = Read-ServerLine $Srv $TimeoutMs
        if ($null -eq $line) { return $null }
        $obj = ConvertFrom-JsonSafe $line
        if ($null -ne $obj -and $null -ne $obj.PSObject.Properties['id'] -and $obj.id -eq $id) { return $obj }
    }
}

function Invoke-Tool($Srv, [string]$Name, $Arguments = @{}, [int]$TimeoutMs = 130000) {
    $r = Invoke-Rpc $Srv 'tools/call' @{ name = $Name; arguments = $Arguments } $TimeoutMs
    if ($null -ne $r -and $null -ne $r.PSObject.Properties['result']) { $Srv.Calls = $Srv.Calls + 1 }
    return $r
}

function Initialize-Server($Srv, [string]$Version = '2025-06-18') {
    $r = Invoke-Rpc $Srv 'initialize' @{ protocolVersion = $Version; capabilities = @{}
                                         clientInfo = @{ name = 'mcp-smoke'; version = '1.0' } }
    Send-Line $Srv '{"jsonrpc":"2.0","method":"notifications/initialized"}'
    return $r
}

function Stop-Server($Srv) {
    if ($null -eq $Srv) { return $null }
    if ($Srv.Stopped) { return $Srv.ExitCode }
    try { $Srv.Proc.StandardInput.Close() } catch { }
    if (-not $Srv.Proc.WaitForExit(20000)) {
        try { $Srv.Proc.Kill() } catch { }
        $Srv.ExitCode = -1
    } else {
        for ($i = 0; $i -lt 50; $i++) { if ($null -eq (Read-ServerLine $Srv 500)) { break } }
        $Srv.ExitCode = $Srv.Proc.ExitCode
    }
    $Srv.Stopped = $true
    return $Srv.ExitCode
}

function Get-ToolText($R) {
    if ($null -eq $R -or $null -eq $R.PSObject.Properties['result'] -or $null -eq $R.result.content) { return '' }
    foreach ($b in @($R.result.content)) { if ($b.type -eq 'text') { return [string]$b.text } }
    return ''
}
function Test-ToolError($R) {
    return ($null -ne $R -and $null -ne $R.PSObject.Properties['result'] -and $R.result.isError -eq $true)
}
function Get-ErrCode($R) {
    $t = Get-ToolText $R
    if ($t -match '^ERROR ([A-Z0-9_]+):') { return $Matches[1] }
    if ($t -match '^(CALL_TIMEOUT|SERVER_BUSY)') { return $Matches[1] }
    return ''
}
function Get-RpcErrorCode($R) {
    if ($null -eq $R -or $null -eq $R.PSObject.Properties['error']) { return $null }
    return [int]$R.error.code
}
function Get-JsonAfterHeader([string]$Text) {
    $i = $Text.IndexOf("`n{")
    if ($i -lt 0) { $i = $Text.IndexOf('{') } else { $i = $i + 1 }
    if ($i -lt 0) { return $null }
    return (ConvertFrom-JsonSafe $Text.Substring($i))
}

# ---- Check framework ----------------------------------------------------------------------------------
$script:Pass = 0; $script:Fail = 0; $script:Skip = 0
$script:Flags = @{ SapReady = $false; Sm37 = $false }

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

function Get-AuditRecords([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $records = @()
    foreach ($line in (Get-Content -LiteralPath $Path -Encoding UTF8)) {
        if ($line.Trim()) { $obj = ConvertFrom-JsonSafe $line; if ($null -ne $obj) { $records += $obj } }
    }
    return $records
}
function Has-Prop($Obj, [string]$Name) { return ($null -ne $Obj.PSObject.Properties[$Name]) }

$s1 = $null; $s2 = $null; $s3 = $null
$exit = 0
try {
    # ======================================================================================================
    # Server 1: read-only (default)
    # ======================================================================================================
    Write-Host ('== server 1: mcp (read-only), audit file {0}' -f $AuditFile)
    $s1 = Start-Server 'mcp' $AuditFile

    Check 'protocol.initialize' {
        $r = Initialize-Server $s1 '2025-06-18'
        Assert-That ($null -ne $r) 'no initialize response'
        Assert-That ($r.result.protocolVersion -eq '2025-06-18') "protocolVersion is '$($r.result.protocolVersion)'"
        Assert-That ($r.result.serverInfo.name -eq 'fairyfly') "serverInfo.name is '$($r.result.serverInfo.name)'"
    }
    Check 'protocol.tools_list' {
        $r = Invoke-Rpc $s1 'tools/list' @{}
        $tools = @($r.result.tools)
        Assert-That ($tools.Count -eq 20) "expected 20 tools, got $($tools.Count)"
        foreach ($t in $tools) {
            Assert-That ($t.inputSchema.additionalProperties -eq $false) "$($t.name): additionalProperties is not false"
            Assert-That ($null -ne $t.annotations) "$($t.name): no annotations"
        }
        Assert-That (-not (@($tools | Where-Object { $_.name -eq 'gui_element_fill' }))) 'gui_element_fill must not be listed in read-only mode'
    }
    Check 'protocol.ping' {
        $r = Invoke-Rpc $s1 'ping' $null
        Assert-That ($null -ne $r -and $null -ne $r.PSObject.Properties['result']) 'no ping result'
    }
    Check 'protocol.unknown_method' {
        $c = Get-RpcErrorCode (Invoke-Rpc $s1 'no/such/method' @{})
        Assert-That ($c -eq -32601) "expected -32601, got $c"
    }
    Check 'protocol.server_discover' {
        $c = Get-RpcErrorCode (Invoke-Rpc $s1 'server/discover' @{})
        Assert-That ($c -eq -32601) "expected -32601, got $c"
    }
    Check 'protocol.unknown_tool' {
        $c = Get-RpcErrorCode (Invoke-Rpc $s1 'tools/call' @{ name = 'gui_no_such_tool'; arguments = @{} })
        Assert-That ($c -eq -32602) "expected -32602, got $c"
    }
    Check 'protocol.malformed_json' {
        Send-Line $s1 '{"jsonrpc":"2.0","id":'
        $line = Read-ServerLine $s1 15000
        Assert-That ($null -ne $line) 'no reply to the malformed line'
        $obj = ConvertFrom-JsonSafe $line
        Assert-That ($null -ne $obj -and $null -ne $obj.PSObject.Properties['error']) 'reply is not an error object'
        Assert-That ($obj.error.code -eq -32700) "expected -32700, got $($obj.error.code)"
    }
    Check 'protocol.alive_after_malformed' {
        $r = Invoke-Rpc $s1 'ping' $null 15000
        Assert-That ($null -ne $r -and $null -ne $r.PSObject.Properties['result']) 'server did not answer ping after a malformed line'
    }

    # ---- SAP ------------------------------------------------------------------------------------------
    $script:FirstSession = ''
    $r = Invoke-Tool $s1 'gui_session_list' @{}
    $sessionText = Get-ToolText $r
    $ids = @([regex]::Matches($sessionText, '/app/con\[\d+\]/ses\[\d+\]') | ForEach-Object { $_.Value } | Select-Object -Unique)
    if ((Test-ToolError $r) -or $ids.Count -lt 1) {
        Write-Host ('FAIL sap.sessions - no SAP GUI session found ({0})' -f (Get-ErrCode $r))
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
            $r = Invoke-Tool $s1 'gui_session_attach' @{}
            if ((Test-ToolError $r) -and (Get-ErrCode $r) -eq 'MULTIPLE_SESSIONS') {
                $r = Invoke-Tool $s1 'gui_session_attach' @{ session_id = $script:FirstSession }
            }
            Assert-That (-not (Test-ToolError $r)) ('attach failed: ' + (Get-ToolText $r))
        }
        Check 'sap.tcode_sm37' {
            $r = Invoke-Tool $s1 'gui_transaction_start' @{ code = '/nSM37' }
            Assert-That (-not (Test-ToolError $r)) ('tcode failed: ' + (Get-ToolText $r))
            $script:Flags['Sm37'] = $true
        }
        Check 'sap.screen_read' {
            $r = Invoke-Tool $s1 'gui_screen_read' @{ no_tabs = $true; only = 'fields'; max_rows = 5 }
            Assert-That (-not (Test-ToolError $r)) ('screen read failed: ' + (Get-ToolText $r))
            $t = Get-ToolText $r
            Assert-That ($t.StartsWith('SAP screen data (untrusted')) 'text does not start with the untrusted-data header'
            Assert-That ($t.Contains('Simple Job Selection')) 'screen text lacks "Simple Job Selection"'
        } 'Sm37'
        Check 'sap.get_checkbox' {
            $r = Invoke-Tool $s1 'gui_element_get' @{ element = $FinishedId }
            Assert-That (-not (Test-ToolError $r)) ('get failed: ' + (Get-ToolText $r))
            $doc = Get-JsonAfterHeader (Get-ToolText $r)
            Assert-That ($null -ne $doc) 'result text has no JSON'
            $d = $doc.data
            if ($null -eq $d) { $d = $doc }
            Assert-That (Has-Prop $d 'selected') 'no "selected" property'
            Assert-That ($d.selected -is [bool]) '"selected" is not a boolean'
            Assert-That (Has-Prop $d 'label') 'no "label" property'
        } 'Sm37'
        Check 'sap.screen_find' {
            $r = Invoke-Tool $s1 'gui_screen_find' @{ id_contains = 'chkBTCH2170' }
            Assert-That (-not (Test-ToolError $r)) ('find failed: ' + (Get-ToolText $r))
            Assert-That ((Get-ToolText $r).Contains('chkBTCH2170')) 'no chkBTCH2170 match in the result'
        } 'Sm37'
        Check 'sap.capture_png' {
            $r = Invoke-Tool $s1 'gui_screen_capture' @{ scale = 0.4 }
            Assert-That (-not (Test-ToolError $r)) ('capture failed: ' + (Get-ToolText $r))
            $img = @($r.result.content | Where-Object { $_.type -eq 'image' })
            Assert-That ($img.Count -ge 1) 'no image block'
            $bytes = [Convert]::FromBase64String([string]$img[0].data)
            $magic = @(0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)
            Assert-That ($bytes.Length -gt 8) 'image too small'
            for ($i = 0; $i -lt 8; $i++) { Assert-That ($bytes[$i] -eq $magic[$i]) 'image data is not a PNG' }
        }
        Check 'sap.menu_list' {
            $r = Invoke-Tool $s1 'gui_menu_list' @{}
            Assert-That (-not (Test-ToolError $r)) ('menu list failed: ' + (Get-ToolText $r))
            Assert-That ((Get-ToolText $r) -match '"children"') 'result is not a menu tree'
        } 'Sm37'

        # ---- read-only refusals (server 1 is read-only: nothing is pressed) ----------------------------
        Check 'readonly.click_save' {
            $r = Invoke-Tool $s1 'gui_element_click' @{ element = $SaveButton }
            Assert-That (Test-ToolError $r) 'click on Save was not an error'
            Assert-That ((Get-ErrCode $r) -eq 'READ_ONLY_REFUSED') "code is '$(Get-ErrCode $r)'"
        }
        Check 'readonly.send_key_f11' {
            $r = Invoke-Tool $s1 'gui_key_send' @{ key = 'f11' }
            Assert-That (Test-ToolError $r) 'send_key f11 was not an error'
            Assert-That ((Get-ErrCode $r) -eq 'READ_ONLY_REFUSED') "code is '$(Get-ErrCode $r)'"
        }
        Check 'readonly.launch_end' {
            $r = Invoke-Tool $s1 'gui_session_launch' @{ name = 'ZMCPSMOKE_NOSYSTEM'; multiple_logon = 'end' }
            Assert-That (Test-ToolError $r) 'launch multiple_logon=end was not an error'
            Assert-That ((Get-ErrCode $r) -eq 'READ_ONLY_REFUSED') "code is '$(Get-ErrCode $r)'"
        }
        Check 'readonly.hidden_fill' {
            $r = Invoke-Tool $s1 'gui_element_fill' @{ element = $JobNameId; value = $Marker }
            Assert-That (Test-ToolError $r) 'hidden gui_element_fill was not an error'
            Assert-That ((Get-ErrCode $r) -eq 'TOOL_UNAVAILABLE_READ_ONLY') "code is '$(Get-ErrCode $r)'"
        }

        # ---- batch ------------------------------------------------------------------------------------
        Check 'batch.mixed' {
            $items = @(@{ tool = 'gui_transaction_start'; arguments = @{ code = '/n' } },
                       @{ tool = 'gui_key_send'; arguments = @{ key = 'f11' } },
                       @{ tool = 'gui_session_list'; arguments = @{} })
            $r = Invoke-Tool $s1 'gui_batch' @{ items = $items; stop_on_error = $false }
            Assert-That (Test-ToolError $r) 'batch with a refused item must be isError'
            $summary = @(ConvertFrom-JsonSafe ((Get-ToolText $r) -split "`n" | Select-Object -First 1))
            Assert-That ($summary.Count -eq 3) "expected 3 items, got $($summary.Count)"
            $s1.Calls = $s1.Calls + 2   # each item is audited as its own call
            Assert-That ($summary[0].ok -eq $true) 'item 1 (tcode /n) failed'
            Assert-That ($summary[1].ok -eq $false -and $summary[1].error_code -eq 'READ_ONLY_REFUSED') 'item 2 was not refused'
            Assert-That ($summary[2].ok -eq $true) 'item 3 (sessions) failed'
        }
        Check 'sap.close_popup_none' {
            $r = Invoke-Tool $s1 'gui_popup_close' @{}
            Assert-That (Test-ToolError $r) 'close_popup without a popup should be an error'
            Assert-That ((Get-ErrCode $r) -eq 'NO_POPUP') "code is '$(Get-ErrCode $r)'"
        }
        Check 'final.tcode_home' {
            $r = Invoke-Tool $s1 'gui_transaction_start' @{ code = '/n' }
            Assert-That (-not (Test-ToolError $r)) ('tcode /n failed: ' + (Get-ToolText $r))
        }
    }

    # ---- shutdown and stdout hygiene --------------------------------------------------------------------
    $code1 = Stop-Server $s1
    Check 'server.exit_code_0' { Assert-That ($code1 -eq 0) "exit code is $code1" }
    Check 'server.stdout_clean' {
        Assert-That ($s1.Lines.Count -gt 0) 'no stdout lines captured'
        foreach ($line in $s1.Lines) {
            $obj = ConvertFrom-JsonSafe $line
            Assert-That ($null -ne $obj -and $obj.jsonrpc -eq '2.0') ('non JSON-RPC stdout line: ' + $line.Substring(0, [Math]::Min(80, $line.Length)))
        }
    }

    # ---- audit assertions ---------------------------------------------------------------------------------
    $records = @(Get-AuditRecords $s1.Audit)
    $toolRecords = @($records | Where-Object { Has-Prop $_ 'tool' })
    Check 'audit.file_exists' { Assert-That (Test-Path -LiteralPath $s1.Audit) "no audit file at $($s1.Audit)"; Assert-That ($records.Count -gt 0) 'audit file is empty' }
    Check 'audit.one_record_per_call' {
        Assert-That ($toolRecords.Count -eq $s1.Calls) "expected $($s1.Calls) tool records, found $($toolRecords.Count)"
    }
    Check 'audit.source_mcp' {
        $bad = @($records | Where-Object { -not (Has-Prop $_ 'audit_source') -or $_.audit_source -ne 'mcp' })
        Assert-That ($bad.Count -eq 0) "$($bad.Count) record(s) without audit_source mcp"
    }
    Check 'audit.tool_and_client' {
        $bad = @($toolRecords | Where-Object { -not $_.tool -or -not (Has-Prop $_ 'client') -or -not $_.client })
        Assert-That ($bad.Count -eq 0) "$($bad.Count) tool record(s) lack tool or client"
    }
    Check 'audit.serve_started_stopped' {
        Assert-That (@($records | Where-Object { $_.cmd -eq 'mcp' -and $_.status -eq 'started' }).Count -eq 1) 'no single mcp/started record'
        Assert-That (@($records | Where-Object { $_.cmd -eq 'mcp' -and $_.status -eq 'stopped' }).Count -eq 1) 'no single mcp/stopped record'
    }
    Check 'audit.read_only_true' {
        Assert-That ($toolRecords.Count -gt 0) 'no tool records'
        Assert-That ($toolRecords[0].read_only -eq $true) 'first tool record has read_only != true'
    }
    Check 'audit.refusals_have_error_code' {
        $errors = @($toolRecords | Where-Object { $_.status -eq 'error' })
        $bad = @($errors | Where-Object { -not (Has-Prop $_ 'error_code') -or -not $_.error_code })
        Assert-That ($bad.Count -eq 0) "$($bad.Count) error record(s) without error_code"
        if ($script:Flags['SapReady']) {
            $ro = @($errors | Where-Object { $_.error_code -eq 'READ_ONLY_REFUSED' }).Count
            $hid = @($errors | Where-Object { $_.error_code -eq 'TOOL_UNAVAILABLE_READ_ONLY' }).Count
            Assert-That ($ro -ge 4) "expected >= 4 READ_ONLY_REFUSED records, found $ro"
            Assert-That ($hid -ge 1) "expected >= 1 TOOL_UNAVAILABLE_READ_ONLY record, found $hid"
        }
    }
    Check 'audit.no_results_leaked' {
        $raw = Get-Content -LiteralPath $s1.Audit -Raw -Encoding UTF8
        Assert-That ($raw -notmatch 'SAP screen data') 'audit file contains screen data'
    }

    # ======================================================================================================
    # Server 2: --allow-write
    # ======================================================================================================
    if (-not $SkipWriteMode -and $script:Flags['SapReady']) {
        Write-Host ('== server 2: mcp --allow-write, audit file {0}' -f $AuditWrite)
        $s2 = Start-Server 'mcp --allow-write' $AuditWrite
        $script:Flags['Sm37'] = $false
        Check 'write.tools_list' {
            $r = Initialize-Server $s2
            Assert-That ($null -ne $r) 'no initialize response'
            $tools = @((Invoke-Rpc $s2 'tools/list' @{}).result.tools)
            Assert-That ($tools.Count -eq 21) "expected 21 tools, got $($tools.Count)"
            Assert-That (@($tools | Where-Object { $_.name -eq 'gui_element_fill' }).Count -eq 1) 'gui_element_fill is not listed'
        }
        Check 'write.attach' {
            $r = Invoke-Tool $s2 'gui_session_attach' @{}
            if ((Test-ToolError $r) -and (Get-ErrCode $r) -eq 'MULTIPLE_SESSIONS') {
                $r = Invoke-Tool $s2 'gui_session_attach' @{ session_id = $script:FirstSession }
            }
            Assert-That (-not (Test-ToolError $r)) ('attach failed: ' + (Get-ToolText $r))
        }
        Check 'write.tcode_sm37' {
            $r = Invoke-Tool $s2 'gui_transaction_start' @{ code = '/nSM37' }
            Assert-That (-not (Test-ToolError $r)) ('tcode failed: ' + (Get-ToolText $r))
            $script:Flags['Sm37'] = $true
        }
        try {
            Check 'write.fill_echo_not_audited' {
                $r = Invoke-Tool $s2 'gui_element_fill' @{ element = $JobNameId; value = $Marker }
                Assert-That (-not (Test-ToolError $r)) ('fill failed: ' + (Get-ToolText $r))
                $raw = $r | ConvertTo-Json -Depth 20 -Compress
                # since 2026.10: a non-credential field echoes the value read back from the control; the audit trail must still never contain it (checked in write.exit_and_audit)
                Assert-That ($raw.Contains($Marker)) 'the fill result should echo the value read back from the control'
            } 'Sm37'
        } finally {
            if ($script:Flags['Sm37']) {
                # restore the field to its default and go home, whatever happened above
                try { [void](Invoke-Tool $s2 'gui_element_fill' @{ element = $JobNameId; value = '*' }) } catch { }
                try { [void](Invoke-Tool $s2 'gui_transaction_start' @{ code = '/n' }) } catch { }
            }
        }
        $code2 = Stop-Server $s2
        Check 'write.exit_and_audit' {
            Assert-That ($code2 -eq 0) "exit code is $code2"
            Assert-That (Test-Path -LiteralPath $s2.Audit) 'no audit file'
            $raw = Get-Content -LiteralPath $s2.Audit -Raw -Encoding UTF8
            Assert-That (-not $raw.Contains($Marker)) 'the fill value appears in the audit file'
            $fillRecords = @(Get-AuditRecords $s2.Audit | Where-Object { (Has-Prop $_ 'tool') -and $_.tool -eq 'gui_element_fill' })
            Assert-That ($fillRecords.Count -ge 1) 'no gui_element_fill audit record'
            Assert-That ($fillRecords[0].read_only -eq $false) 'gui_element_fill record has read_only != false'
            foreach ($line in $s2.Lines) {
                $obj = ConvertFrom-JsonSafe $line
                Assert-That ($null -ne $obj -and $obj.jsonrpc -eq '2.0') 'non JSON-RPC stdout line on the write server'
            }
        }

        # ==================================================================================================
        # Server 3: FAIRYFLY_READ_ONLY=1 caps --allow-write
        # ==================================================================================================
        Write-Host ('== server 3: mcp --allow-write with FAIRYFLY_READ_ONLY=1, audit file {0}' -f $AuditCap)
        $s3 = Start-Server 'mcp --allow-write' $AuditCap @{ FAIRYFLY_READ_ONLY = '1' }
        Check 'cap.env_hard_cap' {
            $r = Initialize-Server $s3
            Assert-That ($null -ne $r) 'no initialize response'
            $tools = @((Invoke-Rpc $s3 'tools/list' @{}).result.tools)
            Assert-That ($tools.Count -eq 20) "expected 20 tools with FAIRYFLY_READ_ONLY=1, got $($tools.Count)"
            Assert-That (@($tools | Where-Object { $_.name -eq 'gui_element_fill' }).Count -eq 0) 'gui_element_fill is listed despite FAIRYFLY_READ_ONLY=1'
            $code3 = Stop-Server $s3
            Assert-That ($code3 -eq 0) "exit code is $code3"
        }
    }
} finally {
    # Always leave SAP on the Easy Access screen and never leave a server running.
    foreach ($srv in @($s1, $s2)) {
        if ($null -ne $srv -and -not $srv.Stopped -and $script:Flags['SapReady']) {
            try {
                if (-not $srv.Proc.HasExited) { [void](Invoke-Tool $srv 'gui_transaction_start' @{ code = '/n' } 30000) }
            } catch { }
        }
    }
    foreach ($srv in @($s1, $s2, $s3)) {
        if ($null -ne $srv -and -not $srv.Stopped) { try { [void](Stop-Server $srv) } catch { } }
    }
}

Write-Host ''
Write-Host ('Summary: {0} passed, {1} failed, {2} skipped' -f $script:Pass, $script:Fail, $script:Skip)
if ($exit -eq 2) { Write-Host 'No SAP GUI session found: start SAP GUI, log on, and rerun.'; exit 2 }
if ($script:Fail -gt 0) { exit 1 }
exit 0
