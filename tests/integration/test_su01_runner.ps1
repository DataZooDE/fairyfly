# Offline contract test for the disposable SU01 workflow. No SAP connection is used.
$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot 'test_su01_create_user.ps1'
$parseErrors = $null
$tokens = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw 'SU01 runner has a PowerShell parse error' }
$parameterNames = @($ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
if ('FairyflyPath' -notin $parameterNames -or 'ExistingConnectionId' -notin $parameterNames) {
    throw 'Offline runner contract is absent; refusing to invoke a possible live Fairyfly executable'
}
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('fairyfly-su01-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $mock = Join-Path $work 'fairyfly_mock.ps1'
    $env:FAIRYFLY_SU01_MOCK_LOG = Join-Path $work 'calls.txt'
    $env:FAIRYFLY_SU01_MOCK_USER = 'ZFFSU01T001'
    $env:FAIRYFLY_SU01_MOCK_FIRST = 'Fairy'
    $env:FAIRYFLY_SU01_MOCK_LAST = 'Fly'
    $env:FAIRYFLY_SU01_OFFLINE_ONLY = '1'
    @'
$global:LASTEXITCODE = 0
$argv = @($args)
# The workflow now calls noun/verb commands; map them back to the short labels the checks below use.
$legacy = @{ 'session list' = 'list'; 'connection list' = 'connections'; 'session launch' = 'launch'; 'session login' = 'login'; 'session disconnect' = 'disconnect'; 'element get' = 'get'; 'element click' = 'click'; 'element fill' = 'fill'; 'transaction start' = 'tcode'; 'credentials set' = 'credset'; 'credentials delete' = 'creddel' }
if ($argv.Count -gt 1 -and $legacy.ContainsKey("$($argv[0]) $($argv[1])")) { $argv = @($legacy["$($argv[0]) $($argv[1])"]) + @($argv | Select-Object -Skip 2) }
$command = $argv[0]
$target = if ($argv.Count -gt 1) { $argv[1] } else { '' }
$scopeIndex = [Array]::IndexOf($argv, '--connection')
$scope = if ($scopeIndex -ge 0) { $argv[$scopeIndex + 1] } else { '' }
$record = "$command|$target|$scope"
Add-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG -Value $record
if ($command -eq 'credset' -and $env:FAIRYFLY_SU01_MOCK_CREDSET_BAD_OUTPUT -eq '1') {
    # Simulate a successful credential write followed by broken CLI output.
    Write-Output '{'
    return
}
$data = @{}
$status = 'success'
$errorDetail = $null
if ($command -eq 'list') {
    $data = @{ connections = @(@{ index = 0; id = '/app/con[1]'; description = 'Bigfox'; backend_scripting_disabled = $false; session_count = 1 }) }
} elseif ($command -eq 'connections') {
    $data = @{ connections = @(@{ id = 7; connection_id = '/app/con[1]'; session_id = '/app/con[1]/ses[0]'; valid = $true }) }
} elseif ($command -eq 'launch') {
    $launchCount = @((Get-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG) | Where-Object { $_ -match '^launch\|' }).Count
    if ($launchCount -eq 1) { $data = @{ connection_file_id = 8; session_id = '/app/con[2]/ses[0]' } }
    else { $data = @{ connection_file_id = 9; session_id = '/app/con[3]/ses[0]' } }
} elseif ($command -eq 'login') {
    if ($env:FAIRYFLY_SU01_MOCK_LOGIN_FAIL -eq '1') {
        $status = 'error'
        $errorDetail = @{ message = 'Logon did not complete' }
    } elseif ($target -eq '--credentials-stdin') {
        $lines = @($input)
        if ($scope -ne '9' -or
            -not ($lines -match '^Username: ZFFSU01T001$') -or
            -not ($lines -match '^Password: Bb2![0-9a-f]{14}$') -or
            -not ($lines -match '^New Password: Cc3![0-9a-f]{14}$') -or
            -not ($lines -match '^System ID: 001$')) {
            $status = 'error'
            $errorDetail = @{ message = 'Piped user credentials were invalid' }
        }
    } else {
        $data = @{ transaction = 'SESSION_MANAGER' }
    }
} elseif ($command -eq 'credset') {
    $lines = @($input)
    if ($target -ne "FFTEST-$env:FAIRYFLY_SU01_MOCK_USER" -or
        -not ($lines -match '^Cc3![0-9a-f]{14}$')) {
        $status = 'error'; $errorDetail = @{ message = 'Temporary credential was not piped through stdin' }
    }
} elseif ($command -eq 'creddel') {
    if ($target -ne "FFTEST-$env:FAIRYFLY_SU01_MOCK_USER") {
        $status = 'error'; $errorDetail = @{ message = 'Wrong temporary credential deleted' }
    }
} elseif ($command -eq 'disconnect') {
    $data = @{ session_closed = $true; file_deleted = $true }
} elseif ($command -eq 'get') {
    $value = ''
    if ($target -eq 'wnd[0]/sbar') {
        $calls = @(Get-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG)
        if ($calls -match 'click\|wnd\[0\]/tbar\[0\]/btn\[11\]') { $value = "User $env:FAIRYFLY_SU01_MOCK_USER created" }
        if ($calls -match 'click\|wnd\[1\]/tbar\[0\]/btn\[0\]') { $value = 'The password was changed' }
        if ($calls -match 'click\|wnd\[1\]/usr/btnBUTTON_1') { $value = "User $env:FAIRYFLY_SU01_MOCK_USER deleted" }
    } elseif ($target -match 'NAME_FIRST$') { $value = $env:FAIRYFLY_SU01_MOCK_FIRST + ' ' }
    elseif ($target -match 'NAME_LAST$') { $value = $env:FAIRYFLY_SU01_MOCK_LAST + ' ' }
    elseif ($target -match 'SUID_ST_BNAME-BNAME$') { $value = $env:FAIRYFLY_SU01_MOCK_USER }
    $data = @{ value = $value }
} elseif ($command -eq 'screen') {
    $data = @{ transaction = 'SU01'; screen_id = 'wnd[0]'; elements = @() }
} elseif ($command -eq 'fill' -and $target -match '/pwdSUID_ST_NODE_PASSWORD_EXT-PASSWORD2?$') {
    $expected = if ($target.StartsWith('wnd[1]')) { '^Bb2![0-9a-f]{14}$' } else { '^Aa1![0-9a-f]{14}$' }
    if ($argv[2] -cnotmatch $expected) {
        $status = 'error'
        $errorDetail = @{ message = 'Generated password does not meet test policy' }
    }
} elseif ($command -eq 'click' -and $target -eq 'wnd[0]/tbar[1]/btn[7]') {
    $calls = @(Get-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG)
    if (-not ($calls -match 'click\|wnd\[0\]/tbar\[0\]/btn\[11\]') -or
        ($calls -match 'click\|wnd\[1\]/usr/btnBUTTON_1')) {
        $status = 'error'
        $errorDetail = @{ message = "User $env:FAIRYFLY_SU01_MOCK_USER does not exist" }
    }
}
@{ status = $status; data = $data; error = $errorDetail } | ConvertTo-Json -Depth 8 -Compress
'@ | Set-Content -LiteralPath $mock

    $output = & $scriptPath -ExistingConnectionId 7 -FairyflyPath $mock -Username $env:FAIRYFLY_SU01_MOCK_USER -FirstName $env:FAIRYFLY_SU01_MOCK_FIRST -LastName $env:FAIRYFLY_SU01_MOCK_LAST 2>&1 | Out-String
    if (-not (Test-Path -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG)) { throw "Runner failed before calling the CLI: $output" }
    $calls = Get-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG
    foreach ($required in @(
        'fill|wnd[0]/usr/ctxtSUID_ST_BNAME-BNAME',
        'click|wnd[0]/tbar[1]/btn[8]',
        'fill|wnd[0]/usr/tabsTABSTRIP1/tabpADDR/ssubMAINAREA:SAPLSUID_MAINTENANCE:1900/txtSUID_ST_NODE_PERSON_NAME-NAME_FIRST',
        'fill|wnd[0]/usr/tabsTABSTRIP1/tabpADDR/ssubMAINAREA:SAPLSUID_MAINTENANCE:1900/txtSUID_ST_NODE_PERSON_NAME-NAME_LAST',
        'click|wnd[0]/tbar[0]/btn[11]',
        'fill|wnd[0]/usr/tabsTABSTRIP1/tabpLOGO/ssubMAINAREA:SAPLSUID_MAINTENANCE:1101/pwdSUID_ST_NODE_PASSWORD_EXT-PASSWORD',
        'fill|wnd[0]/usr/tabsTABSTRIP1/tabpLOGO/ssubMAINAREA:SAPLSUID_MAINTENANCE:1101/pwdSUID_ST_NODE_PASSWORD_EXT-PASSWORD2',
        'click|wnd[0]/tbar[1]/btn[20]',
        'fill|wnd[1]/usr/subPOPUP:SAPLSUID_MAINTENANCE:1101/pwdSUID_ST_NODE_PASSWORD_EXT-PASSWORD',
        'fill|wnd[1]/usr/subPOPUP:SAPLSUID_MAINTENANCE:1101/pwdSUID_ST_NODE_PASSWORD_EXT-PASSWORD2',
        'click|wnd[1]/tbar[0]/btn[0]',
        'click|wnd[0]/tbar[1]/btn[7]',
        'click|wnd[0]/tbar[1]/btn[14]',
        'click|wnd[1]/usr/btnBUTTON_1'
    )) {
        if (-not ($calls | Where-Object { $_.StartsWith($required) })) { throw "Missing SU01 step $required. Output: $output" }
    }
    if ($calls -match '^(launch|disconnect)\|') { throw 'Existing-session run must not launch or disconnect' }
    if (-not ($calls -match '^connections\|')) { throw 'Existing cache ID was not verified against the saved connection registry' }
    if ($calls | Where-Object { $_ -notmatch '^(list|connections)\|' -and $_ -notmatch '\|7$' }) { throw 'GUI action was not scoped to saved cache ID 7' }
    if (@($calls | Where-Object { $_ -eq 'click|wnd[0]/tbar[1]/btn[7]|7' }).Count -lt 3) { throw 'Expected pre-create, readback, and post-delete Display checks' }
    if ($calls -match 'btnCREATE|txtUSERNAME|btnSAVE') { throw 'Obsolete SU01 control ID was used' }
    Write-Host 'PASS: existing-session SU01 workflow used verified controls and preserved the session.'

    Remove-Item -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG
    $output = & $scriptPath -FairyflyPath $mock -LoginFromTrialEnv -Username $env:FAIRYFLY_SU01_MOCK_USER -FirstName $env:FAIRYFLY_SU01_MOCK_FIRST -LastName $env:FAIRYFLY_SU01_MOCK_LAST 2>&1 | Out-String
    $calls = @(Get-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG)
    if (-not ($calls -match '^login\|--connection\|8$')) { throw "Launch mode did not target its new connection for login: $output" }
    if (-not ($calls -match '^disconnect\|--close-session\|8$')) { throw "Launch mode did not close its own session: $output" }
    if ($calls | Where-Object { $_ -match '^(fill|click|get|tcode)\|' -and $_ -notmatch '\|8$' }) { throw 'Launch-mode GUI action used the wrong saved connection' }
    Write-Host 'PASS: launch-mode SU01 workflow logged in and closed its owned session.'

    Remove-Item -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG
    $output = & $scriptPath -FairyflyPath $mock -LoginFromTrialEnv -VerifyChangedPasswordLogin -Username $env:FAIRYFLY_SU01_MOCK_USER -FirstName $env:FAIRYFLY_SU01_MOCK_FIRST -LastName $env:FAIRYFLY_SU01_MOCK_LAST 2>&1 | Out-String
    $calls = @(Get-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG)
    if (-not ($calls -match '^login\|--credentials-stdin\|9$')) { throw "Changed-password login did not target the disposable user: $output" }
    if (-not ($calls -match '^disconnect\|--close-session\|9$')) { throw "Disposable-user session was not closed: $output" }
    if (-not ($calls -match '^disconnect\|--close-session\|8$')) { throw "Admin session was not closed: $output" }
    if ($calls -match 'Bb2![0-9a-f]{14}') { throw 'Changed password leaked into mock CLI arguments or output' }
    Write-Host 'PASS: changed password authenticated in a second session before user deletion.'

    $gate = Join-Path $work 'mcp_gate_mock.ps1'
    @'
param([string]$Username, [string]$CredentialName, [int]$AdminConnectionId, [string]$ConnectionName, [string]$FairyflyPath)
if ($Username -ne $env:FAIRYFLY_SU01_MOCK_USER -or
    $CredentialName -ne "FFTEST-$Username" -or $AdminConnectionId -ne 8 -or
    $ConnectionName -ne 'Bigfox') { throw 'Wrong disposable-user gate arguments' }
Add-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG -Value "gate|$Username|$AdminConnectionId"
'@ | Set-Content -LiteralPath $gate
    Remove-Item -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG
    $output = & $scriptPath -FairyflyPath $mock -LoginFromTrialEnv -VerifyChangedPasswordLogin `
        -McpGateScript $gate -Username $env:FAIRYFLY_SU01_MOCK_USER `
        -FirstName $env:FAIRYFLY_SU01_MOCK_FIRST -LastName $env:FAIRYFLY_SU01_MOCK_LAST 2>&1 | Out-String
    $calls = @(Get-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG)
    $joined = $calls -join "`n"
    if ($joined -notmatch '(?s)credset\|FFTEST-ZFFSU01T001.*gate\|ZFFSU01T001\|8.*click\|wnd\[0\]/tbar\[1\]/btn\[14\].*creddel\|FFTEST-ZFFSU01T001') {
        throw "MCP gate did not run between verified login and test-user deletion with credential cleanup: $output"
    }
    Write-Host 'PASS: disposable-user MCP gate ran before user deletion and removed its stored credential.'

    Remove-Item -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG
    $env:FAIRYFLY_SU01_MOCK_CREDSET_BAD_OUTPUT = '1'
    try {
        & $scriptPath -FairyflyPath $mock -LoginFromTrialEnv -VerifyChangedPasswordLogin `
            -McpGateScript $gate -Username $env:FAIRYFLY_SU01_MOCK_USER `
            -FirstName $env:FAIRYFLY_SU01_MOCK_FIRST -LastName $env:FAIRYFLY_SU01_MOCK_LAST 2>&1 | Out-String | Out-Null
    } catch {}
    Remove-Item Env:FAIRYFLY_SU01_MOCK_CREDSET_BAD_OUTPUT
    $calls = @(Get-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG)
    if (-not ($calls -match '^credset\|FFTEST-ZFFSU01T001\|') -or
        -not ($calls -match '^creddel\|FFTEST-ZFFSU01T001\|') -or
        -not ($calls -match '^click\|wnd\[0\]/tbar\[1\]/btn\[14\]\|') -or
        ($calls -match '^gate\|')) {
        throw 'Post-write credential failure did not clean the credential and SAP user'
    }
    Write-Host 'PASS: post-write credential output failure cleaned the temporary secret and SAP user.'

    Remove-Item -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG
    $env:FAIRYFLY_SU01_MOCK_LOGIN_FAIL = '1'
    try {
        & $scriptPath -FairyflyPath $mock -LoginFromTrialEnv -Username $env:FAIRYFLY_SU01_MOCK_USER 2>&1 | Out-String | Out-Null
    } catch {}
    Remove-Item Env:FAIRYFLY_SU01_MOCK_LOGIN_FAIL -ErrorAction SilentlyContinue
    $calls = @(Get-Content -LiteralPath $env:FAIRYFLY_SU01_MOCK_LOG)
    if (-not ($calls -match '^login\|--connection\|8$')) { throw 'Failed-login path did not attempt the targeted login' }
    if (-not ($calls -match '^disconnect\|--close-session\|8$')) { throw 'Failed-login path did not close its owned session' }
    if ($calls -match '^(tcode|fill|click|get)\|') { throw 'Failed-login path performed SU01 actions' }
    Write-Host 'PASS: failed launch-mode login closed the owned session without SU01 actions.'
} finally {
    Remove-Item Env:FAIRYFLY_SU01_MOCK_LOG,Env:FAIRYFLY_SU01_MOCK_USER,Env:FAIRYFLY_SU01_MOCK_FIRST,Env:FAIRYFLY_SU01_MOCK_LAST,Env:FAIRYFLY_SU01_OFFLINE_ONLY,Env:FAIRYFLY_SU01_MOCK_LOGIN_FAIL,Env:FAIRYFLY_SU01_MOCK_CREDSET_BAD_OUTPUT -ErrorAction SilentlyContinue
    foreach ($file in @('calls.txt', 'fairyfly_mock.ps1', 'mcp_gate_mock.ps1')) {
        $known = Join-Path $work $file
        if (Test-Path -LiteralPath $known) { Remove-Item -LiteralPath $known -Force }
    }
    Remove-Item -LiteralPath $work -Force
}
