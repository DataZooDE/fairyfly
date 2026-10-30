param(
    [int]$Connection = 4,
    [int]$Port = 8397,
    [string]$Fairyfly = '',
    [ValidateRange(1,10)][int]$Pairs = 1,
    [switch]$BindingProbe,
    [switch]$GuardProbe,
    [switch]$RefProbe,
    [switch]$CreateProbe,
    [switch]$ChangeProbe,
    [string]$PartnerId = '',
    [string]$RestoreEmail = ''
)

# Requires a logged-in Bigfox A4H/001/DEVELOPER SAP GUI session at SAP Easy Access.
# Starts a temporary owner-mode MCP endpoint, navigates to BP, reads the real screen,
# compares legacy and summary batch responses, then returns to Easy Access.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$endpoint = "http://127.0.0.1:$Port/mcp"
$name = 'ffbpsummary' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$cfg = Join-Path $env:TEMP ($name + '.yaml')
$server = $null
$token = $null
$lease = $null
$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(60)

function Invoke-Mcp([string]$tool, $arguments, [int]$id) {
    $body = @{ jsonrpc='2.0'; id=$id; method='tools/call'; params=@{ name=$tool; arguments=$arguments } } |
        ConvertTo-Json -Depth 20 -Compress
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, $endpoint)
    $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $token)
    $request.Headers.Accept.ParseAdd('application/json')
    $request.Content = [System.Net.Http.StringContent]::new($body, [Text.Encoding]::UTF8, 'application/json')
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        try {
            $raw = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            $watch.Stop()
            $serverTiming = ''
            if ($response.Headers.Contains('Server-Timing')) {
                $serverTiming = @($response.Headers.GetValues('Server-Timing')) -join ', '
            }
            return [pscustomobject]@{ status=[int]$response.StatusCode; bytes=[Text.Encoding]::UTF8.GetByteCount($raw);
                ms=$watch.Elapsed.TotalMilliseconds; server_timing=$serverTiming; reply=($raw | ConvertFrom-Json) }
        } finally { $response.Dispose() }
    } finally { $request.Dispose() }
}

function Require-Ok($call, [string]$label) {
    if ($call.status -ne 200 -or $call.reply.error -or $call.reply.result.isError) {
        $codes = ''
        if ($call.reply.result.structuredContent -and $call.reply.result.structuredContent.PSObject.Properties['items']) {
            $codes = @($call.reply.result.structuredContent.items | Where-Object { -not $_.ok } |
                ForEach-Object { "$($_.tool):$($_.error_code)" }) -join ','
        }
        if (-not $codes -and $call.reply.result.content[0].text -match 'ERROR ([A-Z_]+):') { $codes = $Matches[1] }
        if (-not $codes -and $call.reply.result.structuredContent.error.code) {
            $codes = $call.reply.result.structuredContent.error.code
        }
        throw "$label failed (HTTP $($call.status); $codes)"
    }
}

function Renew-ProbeLease([int]$baseId) {
    $renew = Invoke-Mcp 'gui_session_lease' @{ action='renew'; connection=$Connection; lease_id=$lease } ($baseId+30)
    if ($renew.status -ne 200 -or $renew.reply.error -or $renew.reply.result.isError) {
        $renew = Invoke-Mcp 'gui_session_lease' @{ action='acquire'; connection=$Connection } ($baseId+31)
        Require-Ok $renew 'Reacquire disposable BP session lease'
        $script:lease = $renew.reply.result.structuredContent.lease_id
        if (-not $script:lease) { throw 'Reacquired lease ID missing' }
    }
    return $renew
}

function Open-TestBp([int]$baseId) {
    $renew = Renew-ProbeLease $baseId
    $open = Invoke-Mcp 'gui_batch' @{ connection=$Connection; lease_id=$lease; result_mode='summary'; items=@(
        @{ tool='gui_transaction_start'; arguments=@{ code='BP' } },
        @{ tool='gui_screen_find'; arguments=@{ name_contains='btn[17]' }; bind='open_button';
           expect=@{ transaction='BP' } },
        @{ tool='gui_element_click'; ref='open_button' },
        @{ tool='gui_screen_find'; arguments=@{ id_contains='BUS_JOEL_MAIN-OPEN_NUMBER'; type='GuiCTextField'; probe_all=$true };
           bind='open_number'; expect=@{ title='Open business partner'; transaction='BP' } },
        @{ tool='gui_element_fill'; ref='open_number'; arguments=@{ value=$PartnerId } },
        @{ tool='gui_element_click'; arguments=@{ element='/app/con[0]/ses[0]/wnd[1]/tbar[0]/btn[0]' } }
    ) } $baseId
    Require-Ok $open 'Reopen disposable BP'
    if (@($open.reply.result.structuredContent.items | Where-Object { -not $_.ok }).Count -ne 0) {
        throw 'Reopen disposable BP did not complete every step'
    }
    $mode = Invoke-Mcp 'gui_screen_find' @{ connection=$Connection; name_contains='btn[6]' } ($baseId+20)
    Require-Ok $mode 'Check reopened BP mode'
    $title = $mode.reply.result.structuredContent.data.title
    $extraMs = $mode.ms
    $extraBytes = $mode.bytes
    if ($title -eq "Change Organization: $PartnerId") {
        $display = Invoke-Mcp 'gui_element_click' @{ connection=$Connection; lease_id=$lease;
            element='/app/con[0]/ses[0]/wnd[0]/tbar[1]/btn[6]' } ($baseId+21)
        Require-Ok $display 'Switch reopened BP to Display'
        $extraMs += $display.ms
        $extraBytes += $display.bytes
    } elseif ($title -ne "Display Organization : $PartnerId") {
        throw "Reopened BP has unexpected title '$title'"
    }
    return [pscustomobject]@{ ms=($renew.ms+$open.ms+$extraMs); bytes=($renew.bytes+$open.bytes+$extraBytes) }
}

function Read-TestBpEmail([int]$baseId) {
    $found = Invoke-Mcp 'gui_screen_find' @{ connection=$Connection; id_contains='SZA1_D0100-SMTP_ADDR';
        type='GuiTextField'; probe_all=$true } $baseId
    Require-Ok $found 'Locate disposable BP email'
    $data = $found.reply.result.structuredContent.data
    if ($data.title -ne "Display Organization : $PartnerId" -or $data.element_count -ne 1) {
        throw "The opened display is not the expected organization $PartnerId (title '$($data.title)', matches $($data.element_count))"
    }
    $read = Invoke-Mcp 'gui_element_get' @{ connection=$Connection; element=$data.elements[0].id } ($baseId+1)
    Require-Ok $read 'Read disposable BP email'
    return [pscustomobject]@{ value=$read.reply.result.structuredContent.data.value.Trim();
        ms=($found.ms+$read.ms); bytes=($found.bytes+$read.bytes) }
}

try {
    @('server:', "  port: $Port", 'owner:', '  sap_identities: [A4H/001/DEVELOPER]') |
        Set-Content -LiteralPath $cfg -Encoding ascii
    $server = Start-Process -FilePath $exe -ArgumentList @('mcp','--http','--allow-write','--port',"$Port",'-c',$cfg) `
        -WindowStyle Hidden -PassThru
    $ready = $false
    for ($i=0; $i -lt 40; $i++) {
        if ($server.HasExited) { throw 'Temporary MCP server exited during startup' }
        try {
            $probe = Invoke-WebRequest -Uri $endpoint -Method Post -ContentType 'application/json' `
                -Body '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' -UseBasicParsing -TimeoutSec 2
        } catch {
            if ($_.Exception.Response -and [int]$_.Exception.Response.StatusCode -eq 401) { $ready = $true; break }
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $ready) { throw 'Temporary MCP endpoint did not become ready' }
    $created = & $exe mcp token create $name --scope batch,screen,transaction,session.lease,element,key,popup `
        --system A4H/001 --sap-identity A4H/001/DEVELOPER --connections Bigfox --expires 1h --output json
    if ($LASTEXITCODE -ne 0) { throw 'Could not create temporary MCP token' }
    $token = ($created | ConvertFrom-Json).data.token
    $leaseCall = $null
    for ($i=0; $i -lt 30; $i++) {
        $leaseCall = Invoke-Mcp 'gui_session_lease' @{ action='acquire'; connection=$Connection } 1
        if ($leaseCall.status -ne 401) { break }
        Start-Sleep -Milliseconds 250
    }
    Require-Ok $leaseCall 'Lease acquisition'
    $lease = $leaseCall.reply.result.structuredContent.lease_id
    if (-not $lease) { throw 'Lease ID missing' }

    $items = @(
        @{ tool='gui_transaction_start'; arguments=@{ code='BP' } },
        @{ tool='gui_screen_read'; arguments=@{ only='editable'; no_tabs=$true; format='json' } },
        @{ tool='gui_screen_read'; arguments=@{ only='buttons'; no_tabs=$true; format='json' }; include_result=$true }
    )
    $runs = @()
    for ($pair=1; $pair -le $Pairs; $pair++) {
        $single = @()
        $single += Invoke-Mcp 'gui_transaction_start' @{ code='BP'; connection=$Connection; lease_id=$lease } (20+$pair*3)
        $single += Invoke-Mcp 'gui_screen_read' @{ only='editable'; no_tabs=$true; format='json'; connection=$Connection } (21+$pair*3)
        $single += Invoke-Mcp 'gui_screen_read' @{ only='buttons'; no_tabs=$true; format='json'; connection=$Connection } (22+$pair*3)
        foreach ($step in $single) { Require-Ok $step 'Individual BP step' }
        $full = Invoke-Mcp 'gui_batch' @{ connection=$Connection; lease_id=$lease; items=$items } (100+$pair)
        Require-Ok $full 'Legacy BP batch'
        $summary = Invoke-Mcp 'gui_batch' @{ connection=$Connection; lease_id=$lease; items=$items; result_mode='summary' } (200+$pair)
        Require-Ok $summary 'Summary BP batch'
        $entries = $summary.reply.result.structuredContent.items
        if ($entries.Count -ne 3 -or @($entries | Where-Object { -not $_.ok }).Count -ne 0) {
            throw 'Summary did not report three successful BP steps'
        }
        if ($entries[0].result -or $entries[1].result -or -not $entries[2].result) {
            throw 'Summary exposed an unselected result or omitted the selected result'
        }
        if (@($entries | Where-Object { $null -eq $_.duration_ms -or $null -eq $_.text_bytes -or
            $null -eq $_.phases.sap_action_ms -or $null -eq $_.phases.format_ms -or
            $_.duration_ms -lt 0 -or $_.text_bytes -le 0 }).Count -ne 0) {
            throw 'Summary item timing or rendered size is missing'
        }
        if ($summary.bytes -ge $full.bytes) { throw 'Summary response was not smaller than the legacy response' }
        $runs += [pscustomobject]@{ pair=$pair; individual_ms=[math]::Round(($single | Measure-Object -Property ms -Sum).Sum,1);
            individual_bytes=($single | Measure-Object -Property bytes -Sum).Sum;
            legacy_ms=[math]::Round($full.ms,1); legacy_bytes=$full.bytes;
            summary_ms=[math]::Round($summary.ms,1); summary_bytes=$summary.bytes;
            summary_http=$summary.server_timing;
            summary_item_ms=@($entries | ForEach-Object { $_.duration_ms });
            summary_item_text_bytes=@($entries | ForEach-Object { $_.text_bytes });
            summary_item_phases=@($entries | ForEach-Object { $_.phases }) }
    }
    $bindingOk = $null
    if ($BindingProbe) {
        $boundItems = @(
            @{ tool='gui_transaction_start'; arguments=@{ code='BP' } },
            @{ tool='gui_screen_find'; arguments=@{ name_contains='btn[29]' }; bind='create_organization' },
            @{ tool='gui_element_click'; ref='create_organization' },
            @{ tool='gui_screen_find'; arguments=@{ id_contains='SZA1_D0100-SMTP_ADDR'; type='GuiTextField'; probe_all=$true }; bind='email' },
            @{ tool='gui_element_fill'; arguments=@{ value='fairyfly-timing@example.invalid' }; ref='email' }
        )
        $bound = Invoke-Mcp 'gui_batch' @{ connection=$Connection; lease_id=$lease; items=$boundItems; result_mode='summary' } 6
        Require-Ok $bound 'Bound Create Organization batch'
        $boundEntries = $bound.reply.result.structuredContent.items
        if ($boundEntries.Count -ne 5 -or @($boundEntries | Where-Object { -not $_.ok }).Count -ne 0) {
            throw 'Bound batch did not complete all five steps'
        }
        $cancel = Invoke-Mcp 'gui_key_send' @{ key='f12'; connection=$Connection; lease_id=$lease } 7
        Require-Ok $cancel 'Cancel unsaved organization'
        $popup = Invoke-Mcp 'gui_screen_find' @{ type='GuiButton'; connection=$Connection } 8
        Require-Ok $popup 'Inspect discard confirmation'
        if ($popup.reply.result.structuredContent.data.screen_id -match '/wnd\[1\]$') {
            if ($popup.reply.result.structuredContent.data.title -ne 'Cancel Editing of Business Partner') {
                throw 'Unexpected SAP confirmation dialog'
            }
            $yes = $popup.reply.result.structuredContent.data.screen_id + '/usr/btnSPOP-OPTION1'
            $dismiss = Invoke-Mcp 'gui_element_click' @{ element=$yes; connection=$Connection; lease_id=$lease } 9
            Require-Ok $dismiss 'Discard confirmation'
        }
        $back = Invoke-Mcp 'gui_screen_find' @{ name_contains='btn[29]'; connection=$Connection } 10
        Require-Ok $back 'Verify BP return'
        if ($back.reply.result.structuredContent.data.transaction -ne 'BP' -or
            $back.reply.result.structuredContent.data.element_count -ne 1) {
            throw 'SAP did not return to the BP start screen'
        }
        $bindingOk = $true
    }
    $guardOk = $null
    $guardMs = $null
    $guardBytes = $null
    if ($GuardProbe) {
        $null = Renew-ProbeLease 1000
        $startBp = Invoke-Mcp 'gui_transaction_start' @{ code='BP'; connection=$Connection; lease_id=$lease } 110
        Require-Ok $startBp 'Prepare guarded BP start'
        $startFind = Invoke-Mcp 'gui_screen_find' @{ name_contains='btn[29]'; connection=$Connection } 111
        Require-Ok $startFind 'Observe guarded BP start'
        $startTitle = $startFind.reply.result.structuredContent.data.title
        if (-not $startTitle -or $startFind.reply.result.structuredContent.data.transaction -ne 'BP') {
            throw 'The guarded run has no verified BP start view'
        }
        $guardedItems = @(
            @{ tool='gui_transaction_start'; arguments=@{ code='BP' } },
            @{ tool='gui_screen_find'; arguments=@{ name_contains='btn[29]' }; bind='create_organization';
               expect=@{ title=$startTitle; transaction='BP' } },
            @{ tool='gui_element_click'; ref='create_organization' },
            @{ tool='gui_screen_find'; arguments=@{ id_contains='SZA1_D0100-SMTP_ADDR'; type='GuiTextField'; probe_all=$true };
               bind='email'; expect=@{ title='Create Organization'; transaction='BP' } },
            @{ tool='gui_element_fill'; arguments=@{ value='fairyfly-timing@example.invalid' }; ref='email' },
            @{ tool='gui_key_send'; arguments=@{ key='f12' } },
            @{ tool='gui_screen_find'; arguments=@{ id_contains='btnSPOP-OPTION1'; type='GuiButton'; probe_all=$true };
               bind='discard' },
            @{ tool='gui_element_click'; ref='discard'; when=@{ title='Cancel Editing of Business Partner' } },
            @{ tool='gui_screen_find'; arguments=@{ name_contains='btn[29]' };
               expect=@{ title=$startTitle; transaction='BP' } }
        )
        $null = Renew-ProbeLease 1040
        $guarded = Invoke-Mcp 'gui_batch' @{ connection=$Connection; lease_id=$lease; items=$guardedItems; result_mode='summary' } 11
        Require-Ok $guarded 'Guarded Create Organization flow'
        $guardEntries = $guarded.reply.result.structuredContent.items
        if ($guardEntries.Count -ne 9 -or @($guardEntries | Where-Object { -not $_.ok }).Count -ne 0) {
            throw 'Guarded flow did not complete all nine steps'
        }
        $guardOk = $true
        $guardMs = [math]::Round($guarded.ms,1)
        $guardBytes = $guarded.bytes
    }
    $refResult = $null
    if ($RefProbe) {
        $null = Renew-ProbeLease 1080
        $prepareRef = Invoke-Mcp 'gui_transaction_start' @{ code='BP'; connection=$Connection; lease_id=$lease } 298
        Require-Ok $prepareRef 'Prepare reference probe BP view'
        $refStart = Invoke-Mcp 'gui_screen_find' @{ name_contains='btn[29]'; connection=$Connection } 299
        Require-Ok $refStart 'Observe reference probe return view'
        $refStartTitle = $refStart.reply.result.structuredContent.data.title
        if (-not $refStartTitle -or $refStart.reply.result.structuredContent.data.transaction -ne 'BP') {
            throw 'Reference probe has no verified BP return view'
        }
        $openItems = @(
            @{ tool='gui_transaction_start'; arguments=@{ code='BP' } },
            @{ tool='gui_screen_find'; arguments=@{ name_contains='btn[29]' }; bind='create_organization' },
            @{ tool='gui_element_click'; ref='create_organization' }
        )
        $opened = Invoke-Mcp 'gui_batch' @{ connection=$Connection; lease_id=$lease; items=$openItems; result_mode='summary' } 301
        Require-Ok $opened 'Open Create Organization for reference probe'
        $fullForm = Invoke-Mcp 'gui_screen_read' @{ only='editable'; no_tabs=$true; format='json'; connection=$Connection } 302
        Require-Ok $fullForm 'Full editable form read'
        $shortForm = Invoke-Mcp 'gui_screen_read' @{ only='editable'; no_tabs=$true; projection='refs'; connection=$Connection } 303
        Require-Ok $shortForm 'Projected editable form read'
        $fields = $shortForm.reply.result.structuredContent.fields
        $email = @($fields | Where-Object { $_.name -eq 'SZA1_D0100-SMTP_ADDR' -and $_.type -eq 'GuiTextField' })
        $otherEmail = @($fields | Where-Object { $_.name -eq 'SZA11_0100-SMTP_ADDR' -and $_.type -eq 'GuiTextField' })
        if ($fields.Count -lt 40 -or $email.Count -ne 1 -or -not $email[0].ref) {
            throw 'Projected Create Organization fields were missing or ambiguous'
        }
        if ($otherEmail.Count -ne 1 -or $otherEmail[0].ref -eq $email[0].ref) {
            throw 'Distinct E-mail controls received an ambiguous short reference'
        }
        if ($shortForm.reply.result.content[0].text -match '/app/con\[' -or $shortForm.bytes -ge $fullForm.bytes) {
            throw 'Projected screen exposed an absolute ID or was not smaller'
        }
        $filled = Invoke-Mcp 'gui_element_fill' @{ element=$email[0].ref; value='fairyfly-timing@example.invalid';
            connection=$Connection; lease_id=$lease } 304
        Require-Ok $filled 'Fill by short field reference'
        $readBack = Invoke-Mcp 'gui_element_get' @{ element=$email[0].ref; connection=$Connection } 305
        Require-Ok $readBack 'Read back by short field reference'
        if ($readBack.reply.result.content[0].text -notmatch 'fairyfly-timing@example.invalid') {
            throw 'Short reference read-back did not confirm the value'
        }
        $cancel = Invoke-Mcp 'gui_key_send' @{ key='f12'; connection=$Connection; lease_id=$lease } 306
        Require-Ok $cancel 'Cancel projected form edit'
        $popup = Invoke-Mcp 'gui_screen_find' @{ type='GuiButton'; connection=$Connection } 307
        Require-Ok $popup 'Inspect projected form discard confirmation'
        if ($popup.reply.result.structuredContent.data.title -ne 'Cancel Editing of Business Partner') {
            throw 'Expected BP discard dialog did not appear'
        }
        $yes = $popup.reply.result.structuredContent.data.screen_id + '/usr/btnSPOP-OPTION1'
        $dismiss = Invoke-Mcp 'gui_element_click' @{ element=$yes; connection=$Connection; lease_id=$lease } 308
        Require-Ok $dismiss 'Discard projected form edit'
        $back = Invoke-Mcp 'gui_screen_find' @{ name_contains='btn[29]'; connection=$Connection } 309
        Require-Ok $back 'Verify BP after projected form discard'
        if ($back.reply.result.structuredContent.data.title -ne $refStartTitle -or
            $back.reply.result.structuredContent.data.transaction -ne 'BP' -or
            $back.reply.result.structuredContent.data.element_count -ne 1) {
            throw 'BP did not return after projected form discard'
        }
        $stale = Invoke-Mcp 'gui_element_get' @{ element=$email[0].ref; connection=$Connection } 310
        if (-not $stale.reply.result.isError -or $stale.reply.result.content[0].text -notmatch 'SCREEN_CHANGED') {
            throw 'A reference from the discarded form remained usable on the BP start screen'
        }
        $refResult = [pscustomobject]@{ fields=$fields.Count; full_bytes=$fullForm.bytes;
            projected_bytes=$shortForm.bytes; filled_and_discarded=$true }
    }
    $createResult = $null
    if ($CreateProbe) {
        $createName = 'FAIRYFLY TDD ' + (Get-Date -Format 'yyyyMMddHHmmss')
        $createEmail = 'fairyfly.' + [Guid]::NewGuid().ToString('N').Substring(0, 12) + '@example.invalid'
        $openCreate = Invoke-Mcp 'gui_batch' @{ connection=$Connection; lease_id=$lease; result_mode='summary'; items=@(
            @{ tool='gui_transaction_start'; arguments=@{ code='BP' } },
            @{ tool='gui_screen_find'; arguments=@{ name_contains='btn[29]' }; bind='create_organization';
               expect=@{ title='Maintain Business Partner'; transaction='BP' } },
            @{ tool='gui_element_click'; ref='create_organization' },
            @{ tool='gui_screen_find'; arguments=@{ id_contains='BUT000-NAME_ORG1'; type='GuiTextField'; probe_all=$true };
               expect=@{ title='Create Organization'; transaction='BP' } }
        ) } 401
        Require-Ok $openCreate 'Open disposable BP creation'
        $createForm = Invoke-Mcp 'gui_screen_read' @{ only='editable'; no_tabs=$true; projection='refs'; connection=$Connection } 402
        Require-Ok $createForm 'Read disposable BP creation form'
        $createFields = $createForm.reply.result.structuredContent.fields
        $required = @('BUT000-NAME_ORG1','ADDR1_DATA-COUNTRY','ADDR1_DATA-CITY1',
            'ADDR1_DATA-POST_CODE1','SZA1_D0100-SMTP_ADDR')
        $refs = @{}
        foreach ($fieldName in $required) {
            $match = @($createFields | Where-Object { $_.name -eq $fieldName -and $_.ref })
            if ($match.Count -ne 1) { throw "Creation field $fieldName was not uniquely available" }
            $refs[$fieldName] = $match[0].ref
        }
        $fill = Invoke-Mcp 'gui_element_fill' @{ connection=$Connection; lease_id=$lease; fields=@(
            @{ element=$refs['BUT000-NAME_ORG1']; value=$createName },
            @{ element=$refs['ADDR1_DATA-COUNTRY']; value='DE' },
            @{ element=$refs['ADDR1_DATA-CITY1']; value='Berlin' },
            @{ element=$refs['ADDR1_DATA-POST_CODE1']; value='10115' },
            @{ element=$refs['SZA1_D0100-SMTP_ADDR']; value=$createEmail }
        ) } 403
        Require-Ok $fill 'Fill disposable BP creation fields'
        $save = Invoke-Mcp 'gui_element_click' @{ connection=$Connection; lease_id=$lease;
            element='/app/con[0]/ses[0]/wnd[0]/tbar[0]/btn[11]' } 404
        Require-Ok $save 'Save disposable BP'
        $saveStatus = $save.reply.result.structuredContent.data.status_bar
        if ($saveStatus.message_type -ne 'S' -or $saveStatus.text -notmatch '^Business partner (\d+) created$') {
            throw 'SAP did not confirm a new BP number after Save'
        }
        $script:PartnerId = $Matches[1]
        $fixtureDir = Join-Path $PSScriptRoot '..\..\scratch'
        [void](New-Item -ItemType Directory -Path $fixtureDir -Force)
        $fixturePath = Join-Path $fixtureDir ("bp_test_fixture_$PartnerId.json")
        @{ system='A4H'; client='001'; partner_id=$PartnerId;
           created_utc=(Get-Date).ToUniversalTime().ToString('o'); cleanup_status='retained for cleanup' } |
            ConvertTo-Json | Set-Content -LiteralPath $fixturePath -Encoding utf8
        $createdOpen = Open-TestBp 405
        $createdRead = Read-TestBpEmail 406
        if ($createdRead.value -ne $createEmail) { throw 'Created BP email did not persist after reopening' }
        $createResult = [pscustomobject]@{ save_ms=[math]::Round($save.ms,1); save_bytes=$save.bytes;
            partner_id=$PartnerId; persisted_email_verified=$true; status_type=$saveStatus.message_type;
            form_fields=$createFields.Count; reopen_ms=[math]::Round($createdOpen.ms,1);
            read_bytes=$createdRead.bytes }
    }
    $changeResult = $null
    if ($ChangeProbe) {
        if ($PartnerId -notmatch '^\d{1,10}$') { throw 'ChangeProbe requires a numeric -PartnerId' }
        $firstOpen = Open-TestBp 501
        $originalRead = Read-TestBpEmail 502
        $originalEmail = $originalRead.value
        if (-not $originalEmail.EndsWith('@example.invalid')) {
            throw 'The disposable BP has an unexpected email domain'
        }
        $restoreTarget = if ($RestoreEmail) { $RestoreEmail } else { $originalEmail }
        if (-not $restoreTarget.EndsWith('@example.invalid')) {
            throw 'Restore email must use the disposable domain'
        }
        $newEmail = 'fairyfly.change.' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '@example.invalid'
        $toChange = Invoke-Mcp 'gui_element_click' @{ connection=$Connection; lease_id=$lease;
            element='/app/con[0]/ses[0]/wnd[0]/tbar[1]/btn[6]' } 503
        Require-Ok $toChange 'Switch disposable BP to Change'
        $changeForm = Invoke-Mcp 'gui_screen_read' @{ only='editable'; no_tabs=$true; projection='refs'; connection=$Connection } 504
        Require-Ok $changeForm 'Read disposable BP change form'
        if ($changeForm.reply.result.structuredContent.title -ne "Change Organization: $PartnerId") {
            throw 'The change form is not for the expected BP'
        }
        $changeField = @($changeForm.reply.result.structuredContent.fields | Where-Object {
            $_.name -eq 'SZA1_D0100-SMTP_ADDR' -and $_.type -eq 'GuiTextField' })
        if ($changeField.Count -ne 1) { throw 'Change email field was not uniquely available' }
        $changed = Invoke-Mcp 'gui_element_fill' @{ element=$changeField[0].ref; value=$newEmail;
            connection=$Connection; lease_id=$lease } 505
        Require-Ok $changed 'Change disposable BP email'
        $savedChange = Invoke-Mcp 'gui_element_click' @{ element='/app/con[0]/ses[0]/wnd[0]/tbar[0]/btn[11]';
            connection=$Connection; lease_id=$lease } 506
        Require-Ok $savedChange 'Save disposable BP email change'
        $secondOpen = Open-TestBp 507
        $changedRead = Read-TestBpEmail 508
        if ($changedRead.value -ne $newEmail) {
            throw 'Changed email did not persist after reopening the BP'
        }
        $toRestore = Invoke-Mcp 'gui_element_click' @{ connection=$Connection; lease_id=$lease;
            element='/app/con[0]/ses[0]/wnd[0]/tbar[1]/btn[6]' } 509
        Require-Ok $toRestore 'Switch disposable BP to Change for restore'
        $restoreForm = Invoke-Mcp 'gui_screen_read' @{ only='editable'; no_tabs=$true; projection='refs'; connection=$Connection } 510
        Require-Ok $restoreForm 'Read BP form for email restore'
        $restoreField = @($restoreForm.reply.result.structuredContent.fields | Where-Object {
            $_.name -eq 'SZA1_D0100-SMTP_ADDR' -and $_.type -eq 'GuiTextField' })
        if ($restoreField.Count -ne 1) { throw 'Restore email field was not uniquely available' }
        $restored = Invoke-Mcp 'gui_element_fill' @{ element=$restoreField[0].ref; value=$restoreTarget;
            connection=$Connection; lease_id=$lease } 511
        Require-Ok $restored 'Restore original disposable BP email'
        $savedRestore = Invoke-Mcp 'gui_element_click' @{ element='/app/con[0]/ses[0]/wnd[0]/tbar[0]/btn[11]';
            connection=$Connection; lease_id=$lease } 512
        Require-Ok $savedRestore 'Save restored disposable BP email'
        $thirdOpen = Open-TestBp 513
        $restoredRead = Read-TestBpEmail 514
        if ($restoredRead.value -ne $restoreTarget) {
            throw 'Original email was not restored after reopening the BP'
        }
        $changeResult = [pscustomobject]@{ partner_id=$PartnerId; persisted_change=$true; restored=$true;
            open_ms=@($firstOpen.ms,$secondOpen.ms,$thirdOpen.ms) | ForEach-Object { [math]::Round($_,1) };
            save_ms=@([math]::Round($savedChange.ms,1),[math]::Round($savedRestore.ms,1));
            read_bytes=@($originalRead.bytes,$changedRead.bytes,$restoredRead.bytes) }
    }
    $null = Renew-ProbeLease 1120
    $exitBp = Invoke-Mcp 'gui_key_send' @{ key='f3'; connection=$Connection; lease_id=$lease } 601
    Require-Ok $exitBp 'Leave BP after live probe'
    $finalScreen = Invoke-Mcp 'gui_screen_find' @{ type='GuiButton'; connection=$Connection } 602
    Require-Ok $finalScreen 'Verify final SAP screen'
    if ($finalScreen.reply.result.structuredContent.data.transaction -ne 'SESSION_MANAGER') {
        throw 'The live probe did not return SAP to Easy Access'
    }
    [pscustomobject]@{ system='A4H'; client='001'; transaction='BP'; final_transaction='SESSION_MANAGER'; pairs=$Pairs;
        runs=$runs; steps=$entries.Count; all_steps_ok=$true;
        binding_probe=$bindingOk; guard_probe=$guardOk; guard_ms=$guardMs; guard_bytes=$guardBytes;
        ref_probe=$refResult;
        create_probe=$createResult; change_probe=$changeResult } | ConvertTo-Json -Compress -Depth 6
} finally {
    if ($token -and $lease) {
        try { Invoke-Mcp 'gui_transaction_start' @{ code='S000'; connection=$Connection; lease_id=$lease } 4 | Out-Null } catch {}
        try { Invoke-Mcp 'gui_session_lease' @{ action='release'; connection=$Connection; lease_id=$lease } 5 | Out-Null } catch {}
    }
    try { & $exe mcp token delete $name --yes --output json | Out-Null } catch {}
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
    $client.Dispose()
    Remove-Item -LiteralPath $cfg -ErrorAction SilentlyContinue
}
