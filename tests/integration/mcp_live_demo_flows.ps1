param([int]$Connection=4, [int]$Port=8398, [string]$Fairyfly='build-msix/Release/fairyfly.exe')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path
$endpoint = "http://127.0.0.1:$Port/mcp"
$name = 'ffdemoflows' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
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

    $script:seq=100
    $script:metrics=@()
    function Call([string]$tool, $argsMap) {
        $script:seq++
        $argsMap.connection=$Connection
        if ($tool -ne 'gui_screen_find') { $argsMap.lease_id=$lease }
        $r=Invoke-Mcp $tool $argsMap $script:seq
        Require-Ok $r $tool
        $script:metrics += [pscustomobject]@{tool=$tool;ms=[math]::Round($r.ms,1);bytes=$r.bytes}
        $structured=$r.reply.result.structuredContent
        if ($structured.text -and -not $structured.data) {
            $jsonStart=$structured.text.IndexOf("`n{")
            if ($jsonStart -ge 0) { return ($structured.text.Substring($jsonStart+1) | ConvertFrom-Json) }
        }
        return $structured
    }
    $initial=Call 'gui_screen_find' @{type='GuiButton'}
    if ($initial.data.transaction -ne 'SESSION_MANAGER') { throw 'Start at SAP Easy Access to avoid abandoning other work' }
    $started=$true
    $null=Call 'gui_transaction_start' @{code='SE16'}
    $null=Call 'gui_screen_find' @{id_contains='DATABROWSE-TABLENAME'}
    $generation=Call 'gui_element_fill' @{fields=@(@{element='/app/con[0]/ses[0]/wnd[0]/usr/ctxtDATABROWSE-TABLENAME';value='TDEVC'});then_key='enter'}
    $selected=Call 'gui_screen_find' @{type='GuiButton'}
    if ($selected.data.title -eq 'Select Fields for Selection') {
        $generation=Call 'gui_key_send' @{key='enter';window=$selected.data.screen_id}
        $selected=Call 'gui_screen_find' @{type='GuiButton'}
    }
    if ($selected.data.title -notlike '*Selection Screen*') { throw 'SE16 selection screen not reached' }
    $generationEvidence=$generation.data.status_message
    $null=Call 'gui_transaction_start' @{code='SE16'}
    $null=Call 'gui_element_fill' @{fields=@(@{element='/app/con[0]/ses[0]/wnd[0]/usr/ctxtDATABROWSE-TABLENAME';value='TSTCT'});then_key='enter'}
    $null=Call 'gui_element_fill' @{fields=@(
        @{element='/app/con[0]/ses[0]/wnd[0]/usr/ctxtI1-LOW';value='EN'},
        @{element='/app/con[0]/ses[0]/wnd[0]/usr/ctxtI2-LOW';value='SE*'},
        @{element='/app/con[0]/ses[0]/wnd[0]/usr/txtMAX_SEL';value='50'});then_key='f8'}
    $page=Call 'gui_screen_read' @{only='tables';no_tabs=$true;skip_trees=$true;max_rows=5;text_contains='SE91';format='json'}
    $tables=@($page.data.elements | Where-Object {$_.table_data})
    if ($tables.Count -ne 1) { throw 'Expected one filtered SE16 table' }
    $t=$tables[0].table_data
    if ($t.returned -ne 1 -or $t.rows_fetched -ne 5 -or $t.next_offset -ne 5 -or $t.text_filter_scope -ne 'fetched_rows') {
        throw 'Incorrect filtered page metadata'
    }
    $empty=Call 'gui_screen_read' @{only='tables';no_tabs=$true;skip_trees=$true;max_rows=5;offset=5;text_contains='NO_SUCH_DEMO_VALUE';format='json'}
    if ($empty.data.table_text_filter_scope -ne 'fetched_rows' -or @($empty.data.elements).Count -ne 0) { throw 'Empty page scope not preserved' }
    $null=Renew-ProbeLease 200
    $null=Call 'gui_transaction_start' @{code='SE11'}
    $field=Call 'gui_screen_find' @{id_contains='RSRD1-TBMA_VAL';type='GuiCTextField'}
    if ($field.data.element_count -ne 1) { throw 'Dictionary field not unique' }
    $tableField=$field.data.elements[0].id
    $batch=Call 'gui_batch' @{result_mode='summary';stop_on_error=$true;items=@(
        @{tool='gui_element_f4';arguments=@{element=$tableField}},
        @{tool='gui_screen_find';arguments=@{id_contains='KEY1-LOW';type='GuiTextField'};bind='criterion';expect=@{transaction='SE11';title='Repository Info System: Find Tables'}},
        @{tool='gui_element_fill';ref='criterion';arguments=@{value='TSTC*'}},
        @{tool='gui_key_send';arguments=@{key='f8';window='/app/con[0]/ses[0]/wnd[1]'}},
        @{tool='gui_screen_read';arguments=@{only='tables';no_tabs=$true;skip_trees=$true;max_rows=5;format='json'};include_result=$true}
    )}
    if (@($batch.items | Where-Object {-not $_.ok}).Count -ne 0) { throw 'F4 batch failed' }
    $null=Call 'gui_popup_close' @{vkey=12}
    $null=Call 'gui_element_fill' @{fields=@(@{element=$tableField;value='TSTCT'})}
    $button=Call 'gui_screen_find' @{id_contains='PUSHSHOW';type='GuiButton'}
    $null=Call 'gui_element_click' @{element=$button.data.elements[0].id}
    $current=Call 'gui_screen_read' @{only='tables';no_tabs=$true;skip_trees=$true;max_rows=5;format='json'}
    $null=Renew-ProbeLease 300
    $named=Call 'gui_screen_read' @{only='tables';tab='tabpDEF';skip_trees=$true;max_rows=5;format='json'}
    if (@($current.data.elements).Count -eq 0 -or @($named.data.elements).Count -eq 0) { throw 'Dictionary table read empty' }
    $null=Call 'gui_transaction_start' @{code='/n'}
    $last=Call 'gui_screen_find' @{type='GuiButton'}
    if ($last.data.transaction -ne 'SESSION_MANAGER') { throw 'Cleanup screen not verified' }
    [pscustomobject]@{passed=$true;generation_status=$generationEvidence;filtered_page=@{returned=$t.returned;rows_fetched=$t.rows_fetched;next_offset=$t.next_offset};f4_steps=$batch.items.Count;final_transaction=$last.data.transaction;metrics=$metrics} | ConvertTo-Json -Depth 8
} finally {
    if ($token -and $lease) {
        if ($started) { try { Invoke-Mcp 'gui_transaction_start' @{code='/n';connection=$Connection;lease_id=$lease} 900 | Out-Null } catch {} }
        try { Invoke-Mcp 'gui_session_lease' @{action='release';connection=$Connection;lease_id=$lease} 901 | Out-Null } catch {}
    }
    try { & $exe mcp token delete $name --yes --output json | Out-Null } catch {}
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
    $client.Dispose()
    Remove-Item -LiteralPath $cfg -ErrorAction SilentlyContinue
}

