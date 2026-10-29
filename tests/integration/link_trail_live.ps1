<#
.SYNOPSIS
  Live check of the audit trail with screenshots (docs/LINK.md "Audit trail") on a separate TEST pairing.

.DESCRIPTION
  Never touches this desktop's real pairing: everything runs with -State (a file outside
  %USERPROFILE%\.fairyfly\link\, checked before anything starts) and FAIRYFLY_LINK_FORBID_DEFAULT_STATE=1, so every
  `fairyfly link` verb refuses the real state even if a path slipped through. It never stops, pauses, resumes or
  unpairs another link, never kills fairyfly by name (only the process it started) and starts no tray and no HTTP
  endpoint.

  Part 1, offline (SAP GUI only, no gateway): runs the hidden unit test "[trail-live]" against the active window of the
  first SAP GUI session, once with blurring on and once off, and checks:
    1. the screen's elements were read and its password/bank fields located (trail-check.json, no values in it);
    2. with blurring on, the screenshot is sent with at least one field filled, or dropped with a reason;
    3. with blurring off, bank fields are no longer counted, passwords still are.
  Open the written trail-check-blur.png / trail-check-noblur.png and compare them with the SAP window: every
  password and bank field must be covered by a solid grey box, nothing else.
  Screen to open first: one with a password or bank field, e.g.
    - XK03 / FK03 (display vendor) -> a vendor -> "Payment transactions" (LFBK-BANKS/BANKL/BANKN, IBAN), or
    - SU01 -> a user -> "Logon data" (password fields), or the SAP logon screen of a second connection.
  Part 1 changes nothing in SAP.

  Part 2, online (needs -Gateway and FFY_ACCESS_TOKEN, an access token of the same user for <gateway>/mcp): runs
  `fairyfly link run --state <State>` (pairs that state first with -Pair, QR + number on the phone), then asks you to
  turn the audit trail on for THIS TEST desktop in the web app (Admin > Desktops) and checks:
    4. the console line "Audit trail ON: ..." of the running link;
    5. `link status --state` shows "audit_trail": "on since ..." and the status file carries the policy;
    6. a gui_screen_read through the gateway works; you then confirm in the web app (Audit log > the session >
       View) that the step has before/after screenshots with password and bank fields blurred;
    7. turning the trail off: the console line "Audit trail off", `link status` "off";
    8. after the test stops its own link process: "not running".
  Part 2 only reads the SAP screen. With -Fill <element id> it also fills that field with -FillValue (needs the test
  pairing's grant for writes, `link run --allow-write` is passed, and the desktop approval dialog appears) and you
  confirm the field change old -> new in the session review.

  Exit code: 0 all checks passed, 1 at least one check failed, 2 prerequisite missing or the state path refused.

.PARAMETER State
  The test pairing's state file, e.g. C:\Tools\fairyfly-link-trail-test\state.json. Required; refused when it lies in
  %USERPROFILE%\.fairyfly\link\.
.PARAMETER Exe
  fairyfly.exe of your test build (default: build\Release\fairyfly.exe under the repo root).
.PARAMETER UnitTests
  unit_tests.exe of the same build (default: build\Release\unit_tests.exe, then build\tests\Release\unit_tests.exe).
.PARAMETER OutDir
  Where the screenshots and reports go (default: a new folder under %TEMP%).
.PARAMETER Gateway
  Web address of the gateway for part 2 (default: FAIRYFLY_LINK_GATEWAY). Without it only part 1 runs.
.PARAMETER Pair
  Pair the test state first (`link pair --state`), when it is not paired yet.
.PARAMETER Unpair
  Unpair the test state at the end (remove the test desktop in the web app as well).
.PARAMETER Fill
  Optional element id to fill in part 2 (e.g. wnd[0]/usr/txtSOME-FIELD on a test screen), with -FillValue.
.PARAMETER KeepScreenshots
  Keep the trail-check PNGs at the end (the unblurred-bank "noblur" one shows real bank data; by default both are
  deleted when the script ends).
.PARAMETER DryRun
  Print the plan and exit.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$State,
    [string]$Exe = '',
    [string]$UnitTests = '',
    [string]$OutDir = '',
    [string]$Gateway = $env:FAIRYFLY_LINK_GATEWAY,
    [switch]$Pair,
    [switch]$Unpair,
    [string]$Fill = '',
    [string]$FillValue = '',
    [switch]$KeepScreenshots,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$script:failures = 0
function Pass([string]$what) { Write-Host "PASS  $what" -ForegroundColor Green }
function Fail([string]$what) { Write-Host "FAIL  $what" -ForegroundColor Red; $script:failures++ }
function Check([bool]$ok, [string]$what) { if ($ok) { Pass $what } else { Fail $what } }
function Ask([string]$question) { $a = Read-Host "$question [y/n]"; return $a -match '^(y|yes|j|ja)$' }

# ---- state isolation first: resolve, print, refuse the real pairing ----------------------------------------------
$State = [IO.Path]::GetFullPath($State)
$real = [IO.Path]::GetFullPath((Join-Path $env:USERPROFILE '.fairyfly\link'))
Write-Host "state: $State"
if ($State.TrimEnd('\').StartsWith($real.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $State.TrimEnd('\').Equals($real.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) {
    Write-Host "REFUSED: $State lies in $real (the pairing this desktop relies on). Use a state file of your own." -ForegroundColor Red
    exit 2
}
$env:FAIRYFLY_LINK_FORBID_DEFAULT_STATE = '1'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $Exe) {
    foreach ($c in @('build\Release\fairyfly.exe', 'build\bin\Release\fairyfly.exe')) { $p = Join-Path $root $c; if (Test-Path $p) { $Exe = $p; break } }
}
if (-not $UnitTests) {
    foreach ($c in @('build\Release\unit_tests.exe', 'build\tests\Release\unit_tests.exe')) { $p = Join-Path $root $c; if (Test-Path $p) { $UnitTests = $p; break } }
}
if (-not $OutDir) { $OutDir = Join-Path ([IO.Path]::GetTempPath()) ("ffy-trail-live-" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
if ($DryRun) {
    Write-Host "Plan: state $State (FAIRYFLY_LINK_FORBID_DEFAULT_STATE=1); out $OutDir"
    Write-Host "  part 1: $UnitTests [trail-live] with blurring on and off -> trail-check-*.png/json"
    Write-Host "  part 2: $Exe link run --state <State>; trail on in the web app; notice, link status, gui_screen_read via $Gateway/mcp; trail off; stop"
    exit 0
}
if (-not $Exe -or -not (Test-Path $Exe)) { Write-Host "fairyfly.exe not found; build it or pass -Exe"; exit 2 }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$env:FAIRYFLY_AUDIT_FILE = Join-Path $OutDir 'audit.jsonl'   # keeps the user's real audit log out of it

function Ffy([string[]]$argv) {
    $out = & $Exe @argv --state $State --output json 2>$null
    if (-not $out) { return $null }
    return ($out | Out-String | ConvertFrom-Json)
}
function WaitFor([scriptblock]$cond, [int]$seconds = 20) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 500 }
    return (& $cond)
}

# the guard itself: the real pairing is refused by fairyfly too
$guard = & $Exe link status --output json 2>$null | Out-String
Check ($guard -match 'STATE_GUARD') "fairyfly refuses the real pairing state while FAIRYFLY_LINK_FORBID_DEFAULT_STATE=1"

# ---- part 1: blurring of the current SAP screen (offline) ---------------------------------------------------------
if (-not $UnitTests -or -not (Test-Path $UnitTests)) {
    Fail "unit_tests.exe not found (pass -UnitTests); part 1 skipped"
} else {
    foreach ($mode in @('blur', 'noblur')) {
        $dir = Join-Path $OutDir $mode
        $env:FAIRYFLY_TRAIL_CHECK_DIR = $dir
        $env:FAIRYFLY_TRAIL_CHECK_BLUR = $(if ($mode -eq 'blur') { '1' } else { '0' })
        $env:FAIRYFLY_AUDIT = '0'
        & $UnitTests '[trail-live]' | Out-Host
        Remove-Item Env:FAIRYFLY_AUDIT -ErrorAction SilentlyContinue
        $json = Join-Path $dir 'trail-check.json'
        if (-not (Test-Path $json)) { Fail "[$mode] no report: is SAP GUI running with a logged-in session and scripting enabled?"; continue }
        $r = Get-Content $json -Raw | ConvertFrom-Json
        Write-Host "[$mode] window $($r.window) '$($r.title)' ($($r.transaction)), image $($r.image.width)x$($r.image.height), window rect $($r.window_rect -join ',')"
        foreach ($f in $r.fields) { Write-Host ("        {0,-60} {1,-18} password={2} bank={3} rect={4}" -f $f.id, $f.type, $f.password, $f.bank, ($f.rect -join ',')) }
        if ($r.sent) {
            Copy-Item (Join-Path $dir 'trail-check.png') (Join-Path $OutDir "trail-check-$mode.png") -Force
            Pass "[$mode] screenshot sent, $($r.blurred) field(s) filled: open $(Join-Path $OutDir "trail-check-$mode.png")"
        } else {
            Write-Host "[$mode] screenshot dropped: $($r.dropped_because)" -ForegroundColor Yellow
        }
        $sensitive = @($r.fields).Count
        if ($mode -eq 'blur') {
            Check ($sensitive -gt 0) "[blur] the screen has password or bank fields (open one of the screens in the help first)"
            Check ($r.sent -or $r.dropped_because) "[blur] sent with the fields filled, or dropped with a reason"
            $script:blurCount = $sensitive
        } else {
            $pw = @($r.fields | Where-Object { $_.password }).Count
            Check ($sensitive -eq $pw) "[noblur] only password fields are blurred without blurring ($pw of $sensitive)"
        }
    }
    Remove-Item Env:FAIRYFLY_TRAIL_CHECK_DIR, Env:FAIRYFLY_TRAIL_CHECK_BLUR -ErrorAction SilentlyContinue
    if (Test-Path (Join-Path $OutDir 'trail-check-blur.png')) {
        Check (Ask "Open $(Join-Path $OutDir 'trail-check-blur.png'): is every password and bank field covered by a grey box, and nothing else?") "the blurred screenshot covers exactly the sensitive fields"
    }
}

# ---- part 2: through a gateway with the trail on for this test desktop --------------------------------------------
if (-not $Gateway) {
    Write-Host "No -Gateway: part 2 (notice, status, recorded steps) skipped"
} elseif (-not $env:FFY_ACCESS_TOKEN) {
    Fail "Set FFY_ACCESS_TOKEN to an access token for $Gateway/mcp (part 2 skipped)"
} else {
    $Gateway = $Gateway.TrimEnd('/')
    $status = Ffy @('link', 'status')
    if ((-not $status -or -not $status.data.paired) -and $Pair) {
        Write-Host "Pairing the TEST state $State with $Gateway (QR code + number on your phone)..."
        & $Exe link pair --gateway $Gateway --state $State
        $status = Ffy @('link', 'status')
    }
    if (-not $status -or -not $status.data.paired) { Write-Host "The test state is not paired: rerun with -Pair"; exit 2 }
    if ($status.data.state -ne $State) { Write-Host "link status reports another state ($($status.data.state)); stopping"; exit 2 }
    Pass "test pairing: device $($status.data.device) as $($status.data.account) in $($status.data.organisation)"
    if ($status.data.link -ne 'not running') { Write-Host "A link with this test state is already running; stop it first"; exit 2 }

    $rpcId = 0
    function Mcp([string]$method, $params) {
        $script:rpcId++
        $body = @{ jsonrpc = '2.0'; id = $script:rpcId; method = $method; params = $params } | ConvertTo-Json -Depth 10 -Compress
        $headers = @{ Authorization = "Bearer $($env:FFY_ACCESS_TOKEN)"; Accept = 'application/json, text/event-stream' }
        $r = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$Gateway/mcp" -Headers $headers -ContentType 'application/json' -Body $body
        return ($r.Content | ConvertFrom-Json)
    }
    $log = Join-Path $OutDir 'link-run.err.log'
    $outLog = Join-Path $OutDir 'link-run.out.log'
    $runArgs = @('link', 'run', '--state', ('"' + $State + '"'))   # Start-Process joins without quoting
    if ($Fill) { $runArgs += '--allow-write' }
    $proc = $null
    try {
        $proc = Start-Process -FilePath $Exe -ArgumentList $runArgs -PassThru -WindowStyle Hidden -RedirectStandardError $log -RedirectStandardOutput $outLog
        Check (WaitFor { (Get-Content $log -Raw -ErrorAction SilentlyContinue) -match [regex]::Escape("state: $State") } 10) "link run prints its state first"
        Check (WaitFor { (Ffy @('link', 'status')).data.link -eq 'connected' } 30) "the test link reports connected"

        Write-Host "`nNow turn the audit trail ON for the desktop '$($status.data.device)' (this test desktop ONLY) in $Gateway (Admin > Desktops)."
        Read-Host "Press Enter when it is on"
        Check (WaitFor { (Get-Content $log -Raw -ErrorAction SilentlyContinue) -match 'Audit trail ON: ' } 30) "the console shows the notice 'Audit trail ON: ...'"
        Check (WaitFor { "$((Ffy @('link', 'status')).data.audit_trail)" -like 'on since*' } 15) "link status: audit_trail on since ..."
        $st = Get-Content "$State.status.json" -Raw | ConvertFrom-Json
        Check ($st.trail.on -eq $true) "the status file carries the policy for the tray"

        $init = Mcp 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'link_trail_live.ps1'; version = '1' } }
        Check ($null -ne $init.result) "MCP initialize through the gateway"
        $read = Mcp 'tools/call' @{ name = 'gui_screen_read'; arguments = @{} }
        Check ($read.result -and -not $read.result.isError) "gui_screen_read through the gateway"
        Check (-not ($read | ConvertTo-Json -Depth 20).Contains('fairyfly/changes')) "the assistant's answer carries no trail data"
        if ($Fill) {
            Write-Host "Filling $Fill (approve the change in the dialog on this desktop)..."
            $fill = Mcp 'tools/call' @{ name = 'gui_element_fill'; arguments = @{ element = $Fill; value = $FillValue } }
            Check ($fill.result -and -not $fill.result.isError) "gui_element_fill through the gateway"
        }
        $logText = Get-Content $log -Raw
        $outText = Get-Content $outLog -Raw -ErrorAction SilentlyContinue
        Check (-not ($FillValue -and ($logText.Contains($FillValue) -or "$outText".Contains($FillValue)))) "the link's console and log hold no field value"
        Check (Ask "In $Gateway open Audit log > this session > View: does the step 'Read screen ...' have before and after screenshots with the password and bank fields blurred$(if ($Fill) { ', and the fill step the change old -> new' })?") "the session review shows the recorded steps"

        Write-Host "`nNow turn the audit trail OFF for this test desktop."
        Read-Host "Press Enter when it is off"
        Check (WaitFor { (Get-Content $log -Raw) -match 'Audit trail off' } 30) "the console shows 'Audit trail off'"
        Check (WaitFor { (Ffy @('link', 'status')).data.audit_trail -eq 'off' } 15) "link status: audit_trail off"
    } catch {
        Fail "unexpected error: $($_.Exception.Message)"
    } finally {
        if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force; $proc.WaitForExit(10000) | Out-Null }  # only our own process
    }
    Check (WaitFor { (Ffy @('link', 'status')).data.link -eq 'not running' } 75) "after the test link ends: not running"
    if ($Unpair) {
        & $Exe link unpair --state $State | Out-Null
        Write-Host "Unpaired the test state; remove the test desktop in the web app as well."
    }
}

Remove-Item Env:FAIRYFLY_AUDIT_FILE, Env:FAIRYFLY_LINK_FORBID_DEFAULT_STATE -ErrorAction SilentlyContinue
if (-not $KeepScreenshots) {
    Get-ChildItem $OutDir -Recurse -Filter 'trail-check*.png' -ErrorAction SilentlyContinue | Remove-Item -Force
    Write-Host "Deleted the trail-check screenshots (they can show SAP data); -KeepScreenshots keeps them"
}
Write-Host "Output: $OutDir"
if ($script:failures -gt 0) { Write-Host "$script:failures check(s) failed"; exit 1 }
Write-Host "all checks passed"
exit 0
