# Basis smoke tests on Bigfox (A4H)

A catalogue of read-only Basis checks for fairyfly on the Bigfox system. All use a logged-in session `/app/con[0]/ses[0]` on the SAP Easy Access screen and a user with display authorizations. Each check navigates with `transaction start /n<TCODE>` and returns with `transaction start /n`.

Status legend: **verified live** means the check is part of `bigfox_regression.ps1` or `compare_builds.ps1` and is verified by the orchestrator's live run of those scripts (not by the script author, who had no SAP access); **proposed** means not yet verified and not implemented in any script.

| Transaction | What it exercises in fairyfly | fairyfly commands | Pass criterion | Verified status |
|---|---|---|---|---|
| Easy Access | baseline screen read, `transaction start /n` | `transaction start /n`, `screen read --no-tabs` | success, ids identical between builds | verified live (compare_builds) |
| ST22 selection | range captions, no `%_..._%_APP_%-` carriers, id stability | `transaction start /nST22`, `screen read --no-tabs --output markdown/json`, `--probe-all` | markdown has 'Date (to)'; no carrier ids; gated ids equal probe-all ids | verified live (regression) |
| ST22 list and dump | ALV grid read, double-click, menu enumeration, send-key, popup close | `element click btnTODAY`, `element click <grid> --row 0 --column GPROGRAM --doubleclick`, `menu list`, `key send f3`, `key send bogus`, `element click <grid>/btn_&DETAIL`, `popup close` | dump screen has 'Error analysis' or 'Short Text'; INVALID_VKEY for `bogus`; first `popup close` succeeds, second returns NO_POPUP; SKIP when no dumps today | verified live (regression) |
| SM37 | checkbox get/find, selection fill, job list, text filter, Job log speed, `--read-only` allowed actions | `element get`, `screen find`, `element fill`, `element click tbar[1]/btn[8]`, `screen read --max-rows 200`, `--text-contains`, `element click tbar[1]/btn[47] --wait-for-window`, `key send f3` | `selected` boolean and non-empty `label`; at least one 'Finished' row; job found by name; Job log `screen_changed` true in under 1500 ms | verified live (regression) |
| SU01 (display) | tab enumeration, `--tab` read, TAB_NOT_FOUND, compact JSON, tab restore | `element fill`, `element click tbar[1]/btn[7]`, `screen find --type GuiTab`, `screen read --tab tabpACTG`, `--compact` | at least 10 tabs incl. 'Roles'; tab grid with at least 1 row in under 2500 ms; TAB_NOT_FOUND for unknown tab; compact `hierarchy_format` 'ids' and smaller; active tab unchanged (skipped if not exposed) | verified live (regression) |
| RZ11 | fill + click + status bar, markdown read | `element fill`, `element click btnPANZEIGEN_1000`, `screen read --output markdown` | unknown parameter: status text contains 'not known'; `rdisp/wp_no_dia`: 'Instance Profile' and a number | verified live (regression) |
| SM59 and SE80 | tree screens, unknown-shell cache regression in `batch` | `batch --file`, `screen read` | ST22 grid row count after SM59, SE80 in one process equals a fresh-process read | verified live (regression) |
| SM50 | work process list read | `transaction start /nSM50`, `screen read --no-tabs` | id set identical between builds | verified live (compare_builds) |
| RZ04 | operation-mode tree/list | `transaction start /nRZ04`, `screen read` | id set identical between builds | verified live (compare_builds) |
| SE16 (TADIR) | fill + F7 table display | `element fill ...DATABROWSE-TABLENAME TADIR`, `key send f7` | id set identical between builds | verified live (compare_builds) |
| SE11 (TADIR) | fill + F7 dictionary display | `element fill ...RSRD1-TBMA_VAL TADIR`, `key send f7` | id set identical between builds | verified live (compare_builds) |
| SEGW | gateway project screen | `transaction start /nSEGW`, `screen read` | id set identical between builds | verified live (compare_builds) |
| /IWFND/MAINT_SERVICE | service list (ALV) | `transaction start /n/IWFND/MAINT_SERVICE`, `screen read` | id set identical between builds | verified live (compare_builds) |
| SE38 | editor start screen | `transaction start /nSE38`, `screen read` | id set identical between builds | verified live (compare_builds) |
| SICF | ICF service tree selection | `transaction start /nSICF`, `screen read` | id set identical between builds | verified live (compare_builds) |
| Session hygiene | no popup left, `session list --output toon`, `--read-only` refusals, `batch` of 8 read-only commands | `popup close`, `--output toon session list`, `--read-only ...`, `batch --file` | `popup close` returns NO_POPUP at the end; toon starts with `data:`; refusals return READ_ONLY_REFUSED; all 8 batch statuses success | verified live (regression) |
| SM21 | system log selection and list | `transaction start /nSM21`, `screen read`, `screen find` | selection screen readable; list read succeeds | verified live 2026-09-29 (Bigfox A4H): selection screen readable (From/To date and time fields); `key send F8` opens "Syslog messages" |
| SM04 | user list (ALV) | `transaction start /nSM04`, `screen read --max-rows 50` | at least the current user listed | verified live 2026-09-29: grid read, 2 rows, DEVELOPER listed (incl. the background user session) |
| SM51 | server list | `transaction start /nSM51`, `screen read` | at least one application server row | verified live 2026-09-29: 1 row, `vhcala4hci_A4H_00`, Active, services Dialog Batch Update Upd2 Spool ICM |
| SM12 | lock entry selection (display only, never delete locks) | `transaction start /nSM12`, `screen read` | selection screen readable; never press Delete | verified live 2026-09-29: selection screen readable (client, user, table, argument, limit); nothing pressed |
| SM66 | global work process overview | `transaction start /nSM66`, `screen read` | list readable; ids stable between gated and `--probe-all` | verified live 2026-09-29: grid read; gated and `--probe-all` output identical in size with the same grid ids |

Nothing in the catalogue presses Save, Delete, Release, Stop, Create or Change buttons; the only fills are selection-screen search values. Verification status is updated only after the orchestrator has run the scripts live.

## MCP

`mcp_smoke.ps1` (see [README.md](README.md)) checks the MCP server on the SM37 selection screen. Status: implemented; live verification by the orchestrator.

- Protocol: initialize echoes the requested version and reports serverInfo `fairyfly`; `tools/list` has 20 tools with `additionalProperties: false` and annotations and no `gui_element_fill`; ping; unknown method and `server/discover` give -32601; unknown tool -32602; a malformed line gives -32700 and the server keeps answering.
- SAP reads: `gui_session_list`, `gui_session_attach`, `gui_transaction_start /nSM37`, `gui_screen_read` (untrusted-data header, "Simple Job Selection"), `gui_element_get` on the FINISHED checkbox (`selected`, `label`), `gui_screen_find`, `gui_screen_capture` (numeric scale, PNG), `gui_menu_list`, `gui_popup_close` (NO_POPUP).
- Read-only refusals (nothing pressed): Save button, `key send f11`, `gui_session_launch multiple_logon=end` give READ_ONLY_REFUSED; hidden `gui_element_fill` gives TOOL_UNAVAILABLE_READ_ONLY; `gui_batch` with a refused item reports it per item.
- Shutdown and audit: exit code 0 after stdin closes, every stdout line valid JSON-RPC, one audit record per call with `audit_source: "mcp"`, tool and client, an mcp started/stopped pair, no screen text in the audit file.
- Write mode: 21 tools with `--allow-write`; `gui_element_fill` of the job name marker is redacted in the response and the audit file, then restored; `FAIRYFLY_READ_ONLY=1` with `--allow-write` still lists 20 tools.
