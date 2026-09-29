# Disposable SU01 create/change/read/delete workflow.
param(
    [string]$ConnectionName = 'Bigfox',
    [int]$ExistingConnectionId = -1,
    [switch]$LoginFromTrialEnv,
    [switch]$VerifyChangedPasswordLogin,
    [string]$FairyflyPath = (Join-Path $PSScriptRoot '..\..\build\bin\Release\fairyfly.exe'),
    [string]$Username = ('ZFF' + [guid]::NewGuid().ToString('N').Substring(0, 8)).ToUpperInvariant(),
    [string]$FirstName = 'Fairyfly',
    [string]$LastName = 'Exploratory',
    [string]$InitialPassword = ('Aa1!' + [guid]::NewGuid().ToString('N').Substring(0, 14)),
    [string]$ChangedPassword = ('Bb2!' + [guid]::NewGuid().ToString('N').Substring(0, 14))
)

$ErrorActionPreference = 'Stop'
if ($env:FAIRYFLY_SU01_OFFLINE_ONLY -eq '1' -and
    ([System.IO.Path]::GetExtension($FairyflyPath) -ne '.ps1' -or -not (Test-Path -LiteralPath $FairyflyPath))) {
    throw 'Offline test mode requires an existing mock PowerShell CLI'
}
if ($Username -notmatch '^[A-Z0-9_]{1,12}$') { throw 'Username must be uppercase and at most 12 characters' }
if ($ExistingConnectionId -lt -1) { throw 'ExistingConnectionId must be zero or greater' }
if ($ExistingConnectionId -ge 0 -and $LoginFromTrialEnv) { throw 'LoginFromTrialEnv requires a newly launched connection' }
if (-not (Test-Path -LiteralPath $FairyflyPath)) { throw "Fairyfly CLI was not found: $FairyflyPath" }

$userField = 'wnd[0]/usr/ctxtSUID_ST_BNAME-BNAME'
$firstField = 'wnd[0]/usr/tabsTABSTRIP1/tabpADDR/ssubMAINAREA:SAPLSUID_MAINTENANCE:1900/txtSUID_ST_NODE_PERSON_NAME-NAME_FIRST'
$lastField = 'wnd[0]/usr/tabsTABSTRIP1/tabpADDR/ssubMAINAREA:SAPLSUID_MAINTENANCE:1900/txtSUID_ST_NODE_PERSON_NAME-NAME_LAST'
$passwordPrefix = 'wnd[0]/usr/tabsTABSTRIP1/tabpLOGO/ssubMAINAREA:SAPLSUID_MAINTENANCE:1101/pwdSUID_ST_NODE_PASSWORD_EXT-PASSWORD'
$popupPasswordPrefix = 'wnd[1]/usr/subPOPUP:SAPLSUID_MAINTENANCE:1101/pwdSUID_ST_NODE_PASSWORD_EXT-PASSWORD'
$ownedConnection = $false
$creationAttempted = $false
$deleted = $false
$connectionId = $ExistingConnectionId

function Invoke-Fairyfly {
    param([string[]]$Arguments, [string[]]$InputLines, [switch]$AllowFailure)
    $scopedArguments = @($Arguments)
    if ($connectionId -ge 0 -and $Arguments[0] -notin @('list', 'connections', 'launch', 'credentials') -and $Arguments -notcontains '--connection') {
        $scopedArguments += @('--connection', [string]$connectionId)
    }
    if ($InputLines) {
        $raw = $InputLines | & $FairyflyPath @scopedArguments | Out-String -Width 32768
    } else {
        $raw = & $FairyflyPath @scopedArguments | Out-String -Width 32768
    }
    $exitCode = $LASTEXITCODE
    try { $response = $raw | ConvertFrom-Json -ErrorAction Stop }
    catch { throw "Fairyfly $($Arguments[0]) returned invalid JSON (exit $exitCode)" }
    if ($response.status -ne 'success' -or $exitCode -ne 0) {
        if ($AllowFailure) { return $response }
        # Never echo raw CLI error text: password input may be present in it.
        $errorCode = [string]$response.error.code
        throw "Fairyfly $($Arguments[0]) failed at $($Arguments[1]) (code: $errorCode)"
    }
    return $response
}

function Start-SU01 {
    [void](Invoke-Fairyfly @('tcode', 'SU01'))
    [void](Invoke-Fairyfly @('fill', $userField, $Username))
}

function Assert-Status {
    param([string]$Pattern, [string]$Phase)
    $message = [string](Invoke-Fairyfly @('get', 'wnd[0]/sbar')).data.value
    if ($message -notmatch $Pattern) { throw "SAP did not confirm $Phase for $Username" }
    Write-Host "${Phase}: $message"
}

function Remove-TestUser {
    Start-SU01
    [void](Invoke-Fairyfly @('click', 'wnd[0]/tbar[1]/btn[14]'))
    [void](Invoke-Fairyfly @('click', 'wnd[1]/usr/btnBUTTON_1'))
    Assert-Status -Pattern ([regex]::Escape($Username) + '.*(?i:deleted|gelöscht)') -Phase 'Delete'
    $script:deleted = $true
}

function Test-ChangedPasswordLogin {
    $userConnectionId = -1
    try {
        $launch = Invoke-Fairyfly @('launch', $ConnectionName)
        if ($null -eq $launch.data.connection_file_id) { throw 'User login launch returned no connection file ID' }
        $userConnectionId = [int]$launch.data.connection_file_id
        if ($userConnectionId -eq $connectionId) { throw 'User login launch reused the admin connection file ID' }
        $sessionId = [string]$launch.data.session_id
        if ($sessionId -notmatch '^/app/con\[\d+\]/ses\[\d+\]$') { throw 'User login launch returned no valid session ID' }
        if ($env:FAIRYFLY_SU01_OFFLINE_ONLY -eq '1') {
            $client = '001'
        } else {
            # The client comes from the Credential Manager entry (no secret is listed).
            $stored = Invoke-Fairyfly @('credentials', 'list')
            $entry = @($stored.data.credentials | Where-Object { $_.connection -eq $ConnectionName }) | Select-Object -First 1
            if (-not $entry -or -not $entry.client) { throw "No stored credential for $ConnectionName; run 'fairyfly credentials set' or 'credentials import-env'" }
            $client = [string]$entry.client
        }
        $postLoginPassword = 'Cc3!' + [guid]::NewGuid().ToString('N').Substring(0, 14)
        $credentials = @("Username: $Username", "Password: $ChangedPassword",
                         "New Password: $postLoginPassword", "System ID: $client")
        [void](Invoke-Fairyfly -Arguments @('login', '--credentials-stdin', '--connection', [string]$userConnectionId) -InputLines $credentials)
        Write-Host "Changed password authenticated $Username in a separate SAP GUI session"
    } finally {
        if ($userConnectionId -ge 0) {
            try {
                $closed = Invoke-Fairyfly @('disconnect', '--close-session', '--connection', [string]$userConnectionId)
                if (-not $closed.data.session_closed -or -not $closed.data.file_deleted) {
                    Write-Warning "Could not verify closure of disposable-user session $userConnectionId"
                }
            } catch { Write-Warning "Could not close disposable-user session $userConnectionId" }
        }
    }
}

try {
    $listed = Invoke-Fairyfly @('list')
    if ($ExistingConnectionId -ge 0) {
        $saved = Invoke-Fairyfly @('connections')
        $matching = @($saved.data.connections | Where-Object { $_.id -eq $ExistingConnectionId -and $_.valid })
        if ($matching.Count -ne 1) { throw "Existing connection file ID $ExistingConnectionId has no valid live session" }
        $guiConnection = @($listed.data.connections | Where-Object { $_.id -eq $matching[0].connection_id -and $_.session_count -gt 0 })
        if ($guiConnection.Count -ne 1) { throw "Saved connection file ID $ExistingConnectionId is not in the GUI connection list" }
        if ($guiConnection[0].backend_scripting_disabled) { throw 'SAP GUI scripting is disabled on the existing connection' }
        Write-Host "Using existing SAP connection file ID $ExistingConnectionId"
    } else {
        $entry = @($listed.data.connections | Where-Object { $_.description -eq $ConnectionName -and $_.backend_scripting_disabled })
        if ($entry.Count -gt 0) { throw "SAP GUI scripting is disabled for $ConnectionName" }
        $launch = Invoke-Fairyfly @('launch', $ConnectionName)
        if ($null -eq $launch.data.connection_file_id) { throw 'Launch did not return a connection file ID' }
        $connectionId = [int]$launch.data.connection_file_id
        $ownedConnection = $true
        Write-Host "Launched SAP connection index $connectionId"
        if ($LoginFromTrialEnv) {
            # Credential Manager entry named like the launched connection (no file, no password here).
            [void](Invoke-Fairyfly @('login', '--connection', [string]$connectionId))
            Write-Host 'Completed SAP GUI login for the launched session'
        }
    }

    # A caller-supplied name must never cause cleanup of a pre-existing user.
    Start-SU01
    $prior = Invoke-Fairyfly @('click', 'wnd[0]/tbar[1]/btn[7]') -AllowFailure
    if ($prior.status -eq 'success') { throw "User $Username already exists; refusing to modify it" }
    if ([string]$prior.error.message -notmatch '(?i)does not exist|not found|existiert nicht|nicht vorhanden') {
        throw "Could not prove that $Username is absent before creation"
    }

    Start-SU01
    [void](Invoke-Fairyfly @('click', 'wnd[0]/tbar[1]/btn[8]'))
    [void](Invoke-Fairyfly @('fill', $firstField, $FirstName))
    [void](Invoke-Fairyfly @('fill', $lastField, $LastName))
    [void](Invoke-Fairyfly @('click', 'wnd[0]/usr/tabsTABSTRIP1/tabpLOGO'))
    [void](Invoke-Fairyfly @('fill', $passwordPrefix, $InitialPassword))
    [void](Invoke-Fairyfly @('fill', ($passwordPrefix + '2'), $InitialPassword))
    $creationAttempted = $true
    [void](Invoke-Fairyfly @('click', 'wnd[0]/tbar[0]/btn[11]'))
    Assert-Status -Pattern ([regex]::Escape($Username) + '.*(?i:created|angelegt)') -Phase 'Create'

    Start-SU01
    [void](Invoke-Fairyfly @('click', 'wnd[0]/tbar[1]/btn[7]'))
    $savedFirst = ([string](Invoke-Fairyfly @('get', $firstField)).data.value).TrimEnd()
    $savedLast = ([string](Invoke-Fairyfly @('get', $lastField)).data.value).TrimEnd()
    if ($savedFirst -ne $FirstName -or $savedLast -ne $LastName) { throw 'Displayed SAP user details differ from saved input' }
    Write-Host "Display readback matched $Username"

    Start-SU01
    [void](Invoke-Fairyfly @('click', 'wnd[0]/tbar[1]/btn[20]'))
    [void](Invoke-Fairyfly @('fill', $popupPasswordPrefix, $ChangedPassword))
    [void](Invoke-Fairyfly @('fill', ($popupPasswordPrefix + '2'), $ChangedPassword))
    [void](Invoke-Fairyfly @('click', 'wnd[1]/tbar[0]/btn[0]'))
    Assert-Status -Pattern '(?i)password.*changed|kennwort.*geändert' -Phase 'Password change'

    if ($VerifyChangedPasswordLogin) { Test-ChangedPasswordLogin }

    Remove-TestUser
    Start-SU01
    $afterDelete = Invoke-Fairyfly @('click', 'wnd[0]/tbar[1]/btn[7]') -AllowFailure
    if ($afterDelete.status -eq 'success' -or
        [string]$afterDelete.error.message -notmatch '(?i)does not exist|not found|existiert nicht|nicht vorhanden') {
        throw "Could not prove that $Username is absent after deletion"
    }
    Write-Host "PASS: $Username was created, read back, had its password changed, and was deleted"
} finally {
    if ($creationAttempted -and -not $deleted) {
        try { Remove-TestUser } catch { Write-Warning "Cleanup failed for disposable user $Username; manual deletion is required" }
    }
    if ($ownedConnection) {
        try {
            $closed = Invoke-Fairyfly @('disconnect', '--close-session')
            if (-not $closed.data.session_closed -or -not $closed.data.file_deleted) {
                Write-Warning "Could not verify closure of owned connection index $connectionId"
            }
        } catch { Write-Warning "Could not close owned connection index $connectionId" }
    }
}
