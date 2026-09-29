# Basis smoke tests on Bigfox (A4H)

A catalogue of read-only Basis checks for fairyfly on the Bigfox system. All use a logged-in session `/app/con[0]/ses[0]` on the SAP Easy Access screen and a user with display authorizations. Each check navigates with `tcode /n<TCODE>` and returns with `tcode /n`.

Status legend: **verified live** means the check is part of `bigfox_regression.ps1` or `compare_builds.ps1` and is verified by the orchestrator's live run of those scripts (not by the script author, who had no SAP access); **proposed** means not yet verified and not implemented in any script.

| Transaction | What it exercises in fairyfly | fairyfly commands | Pass criterion | Verified status |
|---|---|---|---|---|
| Easy Access | baseline screen read, `tcode /n` | `tcode /n`, `screen read --no-tabs` | success, ids identical between builds | verified live (compare_builds) |
| ST22 selection | range captions, no `%_..._%_APP_%-` carriers, id stability | `tcode /nST22`, `screen read --no-tabs --output markdown/json`, `--probe-all` | markdown has 'Date (to)'; no carrier ids; gated ids equal probe-all ids | verified live (regression) |
| ST22 list and dump | ALV grid read, double-click, menu enumeration, send-key, popup close | `click btnTODAY`, `click <grid> --row 0 --column GPROGRAM --doubleclick`, `screen menu`, `send-key f3`, `send-key bogus`, `click <grid>/btn_&DETAIL`, `close` | dump screen has 'Error analysis' or 'Short Text'; INVALID_VKEY for `bogus`; first `close` succeeds, second returns NO_POPUP; SKIP when no dumps today | verified live (regression) |
| SM37 | checkbox get/find, selection fill, job list, text filter, Job log speed, `--read-only` allowed actions | `get`, `screen find`, `fill`, `click tbar[1]/btn[8]`, `screen read --max-rows 200`, `--text-contains`, `click tbar[1]/btn[47] --wait-for-window`, `send-key f3` | `selected` boolean and non-empty `label`; at least one 'Finished' row; job found by name; Job log `screen_changed` true in under 1500 ms | verified live (regression) |
| SU01 (display) | tab enumeration, `--tab` read, TAB_NOT_FOUND, compact JSON, tab restore | `fill`, `click tbar[1]/btn[7]`, `screen find --type GuiTab`, `screen read --tab tabpACTG`, `--compact` | at least 10 tabs incl. 'Roles'; tab grid with at least 1 row in under 2500 ms; TAB_NOT_FOUND for unknown tab; compact `hierarchy_format` 'ids' and smaller; active tab unchanged (skipped if not exposed) | verified live (regression) |
| RZ11 | fill + click + status bar, markdown read | `fill`, `click btnPANZEIGEN_1000`, `screen read --output markdown` | unknown parameter: status text contains 'not known'; `rdisp/wp_no_dia`: 'Instance Profile' and a number | verified live (regression) |
| SM59 and SE80 | tree screens, unknown-shell cache regression in `batch` | `batch --file`, `screen read` | ST22 grid row count after SM59, SE80 in one process equals a fresh-process read | verified live (regression) |
| SM50 | work process list read | `tcode /nSM50`, `screen read --no-tabs` | id set identical between builds | verified live (compare_builds) |
| RZ04 | operation-mode tree/list | `tcode /nRZ04`, `screen read` | id set identical between builds | verified live (compare_builds) |
| SE16 (TADIR) | fill + F7 table display | `fill ...DATABROWSE-TABLENAME TADIR`, `send-key f7` | id set identical between builds | verified live (compare_builds) |
| SE11 (TADIR) | fill + F7 dictionary display | `fill ...RSRD1-TBMA_VAL TADIR`, `send-key f7` | id set identical between builds | verified live (compare_builds) |
| SEGW | gateway project screen | `tcode /nSEGW`, `screen read` | id set identical between builds | verified live (compare_builds) |
| /IWFND/MAINT_SERVICE | service list (ALV) | `tcode /n/IWFND/MAINT_SERVICE`, `screen read` | id set identical between builds | verified live (compare_builds) |
| SE38 | editor start screen | `tcode /nSE38`, `screen read` | id set identical between builds | verified live (compare_builds) |
| SICF | ICF service tree selection | `tcode /nSICF`, `screen read` | id set identical between builds | verified live (compare_builds) |
| Session hygiene | no popup left, `list --output toon`, `--read-only` refusals, `batch` of 8 read-only commands | `close`, `--output toon list`, `--read-only ...`, `batch --file` | `close` returns NO_POPUP at the end; toon starts with `data:`; refusals return READ_ONLY_REFUSED; all 8 batch statuses success | verified live (regression) |
| SM21 | system log selection and list | `tcode /nSM21`, `screen read`, `screen find` | selection screen readable; list read succeeds | proposed - NOT YET VERIFIED |
| SM04 | user list (ALV) | `tcode /nSM04`, `screen read --max-rows 50` | at least the current user listed | proposed - NOT YET VERIFIED |
| SM51 | server list | `tcode /nSM51`, `screen read` | at least one application server row | proposed - NOT YET VERIFIED |
| SM12 | lock entry selection (display only, never delete locks) | `tcode /nSM12`, `screen read` | selection screen readable; never press Delete | proposed - NOT YET VERIFIED |
| SM66 | global work process overview | `tcode /nSM66`, `screen read` | list readable; ids stable between gated and `--probe-all` | proposed - NOT YET VERIFIED |

Nothing in the catalogue presses Save, Delete, Release, Stop, Create or Change buttons; the only fills are selection-screen search values. Verification status is updated only after the orchestrator has run the scripts live.

## MCP

`mcp_smoke.ps1` (see [README.md](README.md)) checks the MCP server on the SM37 selection screen. Status: implemented; live verification by the orchestrator.

- Protocol: initialize echoes the requested version and reports serverInfo `fairyfly`; `tools/list` has 20 tools with `additionalProperties: false` and annotations and no `sap_fill`; ping; unknown method and `server/discover` give -32601; unknown tool -32602; a malformed line gives -32700 and the server keeps answering.
- SAP reads: `sap_sessions`, `sap_attach`, `sap_tcode /nSM37`, `sap_screen_read` (untrusted-data header, "Simple Job Selection"), `sap_get` on the FINISHED checkbox (`selected`, `label`), `sap_screen_find`, `sap_capture` (numeric scale, PNG), `sap_menu_list`, `sap_close_popup` (NO_POPUP).
- Read-only refusals (nothing pressed): Save button, `send-key f11`, `sap_launch multiple_logon=end` give READ_ONLY_REFUSED; hidden `sap_fill` gives TOOL_UNAVAILABLE_READ_ONLY; `sap_batch` with a refused item reports it per item.
- Shutdown and audit: exit code 0 after stdin closes, every stdout line valid JSON-RPC, one audit record per call with `audit_source: "mcp"`, tool and client, a serve started/stopped pair, no screen text in the audit file.
- Write mode: 21 tools with `--allow-write`; `sap_fill` of the job name marker is redacted in the response and the audit file, then restored; `FAIRYFLY_READ_ONLY=1` with `--allow-write` still lists 20 tools.
