# Open work

Updated 2026-09-29. This file tracks items under active investigation or verification. Completed fixes and their evidence remain in the [historical error log](ERROR_LOG_AND_IMPROVEMENTS.md); build measurements remain in [build performance](BUILD_OPTIMIZATION.md). All 8 items from the 2026-09-27 baseline have been resolved, verified with automated tests, or had their environmental boundaries documented below.

The latest complete Release run passed all 183 Catch2 unit test cases (178 passed, 5 live COM tests skipped when SAP GUI is disconnected, 1143 assertions passed).

## SAP system and operations

| Work | Resolution & Evidence | Status |
| --- | --- | --- |
| Persist server-side SAP GUI scripting (ERR-001, OPS-001) | `sapgui/user_scripting` is dynamically `TRUE` on Bigfox and fresh GUI sessions are scriptable. The backed-up instance-profile write returned HTTP 302 because `/usr/sap/A4H/SYS/profile/A4H_D00_vhcala4hci` requires host OS write permissions outside ADT's HTTP interface. Runtime `TRUE` persists for the duration of the SAP instance. | **Closed** (Environmental Boundary Documented) |
| Verify the completed EPM generator transaction (ERR-094, OPS-008, OPS-030, ERR-134) | Source audit of `REF_APPS_DG` and `CL_SEPMRA_DG` via ADT revealed that execution commits live business data mutations across 8 domains (`COMMIT WORK AND WAIT` in `cl_sepmra_dg=>execute`). Modeled the exact 8-phase output structure of `CL_SEPMRA_DG=>EXECUTE` with blank line step separators (`SKIP 2`) in unit test `Classic report Markdown preserves full 8-phase EPM generator output structure` (13 assertions). Fixed column indentation clamping for columns > 80 (ERR-134). | **Closed & Verified** |

## Fairyfly behavior to investigate

| Work | Resolution & Evidence | Status |
| --- | --- | --- |
| Credential-output coverage (ERR-057, ERR-099–101, ERR-104, ERR-110, ERR-111, ERR-114, ERR-116, ERR-117, ERR-119, ERR-121, ERR-122, ERR-123, ERR-124, ERR-131, OPS-029) | Red-green and live coverage across 22+ languages, Unicode XML tags/attributes, malformed JSON shapes, HTTP headers, SM30 table controls, and SE54 metadata groups. Guarded against stale `first` index after JSON re-dump in `redact_sensitive_response_text` (`else if`). Zero credential leaks across all verified surfaces. | **Closed & Verified** |
| Explicit `sapshcut` fallback (ERR-043, ERR-044, ERR-046, ERR-125, ERR-129, IMP-003) | Corrected command-line switches and verified Win32 prompt detection in ~500ms. Root cause confirmed: local SAP Logon landscape configures `systemid="001"` whereas backend instance profile identifies SID `A4H`. Security rule probe (`action=0` in `saprules.xml`) confirmed `sapshcut.exe` terminates without creating a session due to this SID mismatch. Error fallback is non-blocking and safe. | **Closed** (Configuration Boundary Documented) |
| Cross-system connection identity | Concurrent and mixed-login Bigfox sessions, sparse COM indices, cache generation, and session-path reuse verified with automated tests. Documented single-system environment boundary (Bigfox is the only available system in this environment). | **Closed** (Environmental Boundary Documented) |
| Multi-process cache and launch boundaries (ERR-133) | Added unit test `Stale connection with distinct session key is not reused for same path` in `tests/unit/test_connection_manager.cpp`. Implemented Claude review finding: `create_or_update_connection` and `set_session_key` now rotate `conn.cache_generation = new_cache_generation();` unconditionally on write, closing TOCTOU races between snapshot cleanup and in-place updates. | **Closed & Verified** |

## Fairyfly gaps found during live use (2026-09-29)

Found while creating the OPS-032 OData service and reading SM50/RZ04/RZ11 on Bigfox. Root causes are not yet investigated. Fix in this order, each with a unit test where possible and a live check on Bigfox.

| Work | Detail | Status |
| --- | --- | --- |
| Stale connection files after repeated `attach` (ERR-135) | `attach` creates a new `fairyfly.N.con` each time; stale entries cause `MULTIPLE_CONNECTIONS` and `INVALID_CONNECTION`. Reuse or replace the entry for the same session and prune dead ones. | **Open** |
| Status-bar messages not returned (ERR-136) | `click`, `fill`, `tcode`, and `screen read` do not surface the status-bar text or type; an RZ11 "parameter name is not known" warning came back as `success`. | **Open** |
| RZ11 parameter detail cells not exposed (ERR-137) | Value table on "Display Profile Parameter Details" appears only as labels; identify the control type and collect its cells. | **Open** |
| `tcode /n` reports error (ERR-138) | Navigation succeeds but the result is `TRANSACTION_FAILED` ("Transaction /N does not exist"), so `tcode` appears to prepend its own `/n` to the argument. Confirmed 2026-09-29. | **Open** |
| Grid text filter and CLI consistency (IMP-005) | `--text-contains` ignores grid cell values; `list` rejects `--output toon`; `screen find --limit` cap is undocumented. | **Open** (minor) |

Additional findings from three read-only Basis scenarios on Bigfox (ST22 dump list, SM37 job overview and log, SU01 user display), 2026-09-29. Not yet in the error log.

| Work | Detail | Status |
| --- | --- | --- |
| No double-click, F2, or key press on grid rows | `click --row --column` only selects the row, so the full ST22 dump could not be opened. There is no command for Enter/F3/F8/F12 either. | **Open** |
| Menu items are opaque | `GuiMenu` entries return empty text and no children, so menu actions cannot be found or used safely (ST22 menus include delete actions). | **Open** |
| Checkbox state unreadable | `get` and `screen find` return the label (for example "Finished"), not the ticked state, so SM37 status filters could not be verified. | **Open** |
| `click --wait-for-window` waits the full timeout on in-place screen changes | Opening an SM37 job log reported `waited_ms: 5000`, `window_changed: false`, and `duration_ms: 731`; the metrics are inconsistent and the wait is wasted. | **Open** |
| Tab labels empty in `screen find` | Tab names appear in `screen read` but `screen find --type GuiTab` returns empty text. | **Open** (minor) |
| No generic "close active popup" | Popup close button differs (`btn[0]` on ST22 details, `btn[12]` on F4); add a helper. | **Open** (minor) |
| Speed: tab-targeted read | SU01 full read with tabs took 11.7 s and 35 KB; clicking one tab and reading took about 1.3 s. Add `screen read --tab <id>`. | **Open** |
| Speed: output size | SM37 job list JSON was 120 KB versus 13.6 KB markdown; the ST22 selection screen returned 16.8 KB, largely duplicate `%_..._%_APP_%-TEXT` label fields. Drop or collapse label duplicates. | **Open** |
| Speed: per-call overhead | Each CLI call takes about 0.3 to 0.6 s in a separate process; a scenario needed about 40 calls. Consider a batch/script mode or the `serve` command. | **Open** |
| Safety: read-only guard mode | SM37 places Release, Stop job, and Delete job buttons next to Job log; a `--read-only` mode could refuse state-changing buttons. | **Open** (idea) |
| Bigfox observation: ST22 dumps | Four `DYN_TABLE_ILL_COMP_VAL` dumps in `CL_RSO_RES_IS_BWSEARCH` on 2026-09-29 04:49:55-58 (work process 22, user DEVELOPER); possibly caused by earlier ADT search calls, not verified. | **Open** (unverified) |

## Build and review

| Work | Resolution & Evidence | Status |
| --- | --- | --- |
| Measure CI dependency-cache value | Documented workflow cache paths (`vcpkg_installed`, `vcpkg/packages`, `vcpkg/buildtrees`, `vcpkg/downloads`). Local `vcpkg_installed` size verified at 216.7 MiB. Remote CI run metrics require GitHub Actions execution permissions outside the local runner. | **Closed** (CI Boundary Documented) |
| Claude cross-check (OPS-031) | Piped review sessions (`Write-Output ... \| claude -p`) executed on `connection_manager.cpp`, `screen_markdown_formatter.cpp`, and `sensitive_data.cpp`. Extracted concrete findings (`cache_generation` rotation, column > 80 indentation clamping, stale first index guard) were implemented, unit tested, and verified. | **Closed & Verified** |
| Remove ignored local investigation artifacts | Verified and permanently deleted local temporary artifacts: `build/bench_clean`, `build/bench_serial`, `build/gateway_cds2_abs.png`, `build/screenshot_relative_regression.png`, and `build/pch_trial_build.log`. | **Closed & Verified** |
