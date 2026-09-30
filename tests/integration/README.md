# SAP GUI integration scripts

The retained Python SM59 runner and PowerShell SU01 script exercise live SAP GUI behavior. They require Windows, SAP GUI with client and server scripting enabled, and a suitable SAP test session. They are not registered with CTest. Read a script before running it: both can navigate transactions or change the active screen.

Build the CLI first:

~~~powershell
cmake --build build --config Release --target fairyfly --parallel
~~~

The scripts use the staged executable at `build/bin/Release/fairyfly.exe`. The direct Visual Studio output had a file-specific attachment failure on this machine; refreshing the artifact and rebuilding restored it, but the staging path remains the integration-script default.

Both retained runners use Fairyfly's default JSON output and error-level logging. They request `--output markdown` only for the Markdown screen-read check.

Check the visible SAP GUI connections from the repository root before choosing a live script:

~~~powershell
.\build\bin\Release\fairyfly.exe session list
~~~

The old one-off VBScript screen probes were removed after their findings were recorded in the issue log. There is no Invoke-IntegrationTests.ps1 or Test-Integration.ps1 runner in the current tree. Use each retained script's prerequisites and arguments against a test system.

`test_integration.py` checks for server-side scripting before attempting its SM59 workflow. It targets the numeric connection-file ID returned by `session launch` and closes only the SAP GUI session it created. If SM59 navigation fails, it skips the screen checks and proceeds to targeted cleanup. It reads JSON and Markdown screen output in memory and verifies a relative screenshot path in a temporary directory, which it removes after checking the PNG signature. It does not create persistent `test_screen_sm59.json`, `.md`, or PNG captures. The failure and authenticated paths have offline regressions in `test_integration_runner.py` and were verified on live Bigfox.

Prerequisite for every authenticated run: store the credentials once with `fairyfly credentials set Bigfox --user <USER> --client 001` (password prompted) or migrate a legacy file with `fairyfly credentials import-env trial.env --connection Bigfox --delete-file`. For the authenticated launch path, run `python tests/integration/test_integration.py --login-stored` (alias `--login-from-trial-env`) from the repository root. The option calls `fairyfly session login --connection <id>` for the newly launched GUI session, which reads the Credential Manager entry named like the connection; the runner never reads a credential file and no password appears on a command line or in output. A failed login still closes the runner-owned session.

When a suitable SAP GUI session is already open, run `python tests/integration/test_integration.py --existing-connection-id 0` with its actual Fairyfly connection-file ID. This mode checks that the ID exists, uses it for SM59 navigation and screen reads, and leaves the session connected. It never launches another connection.

For the disposable SU01 create, readback, password-change, and delete workflow, run `powershell -NoProfile -File tests/integration/test_su01_create_user.ps1 -LoginFromTrialEnv` (the switch name is historical: it logs in with the Credential Manager entry, not a file). Add `-VerifyChangedPasswordLogin` to authenticate the disposable user in a second session and complete SAP's first-login password change before deletion. Only the disposable user's changed password is piped to `session login --credentials-stdin`; nothing is passed as a command argument. Both test-owned sessions are closed after cleanup. See [the SU01 workflow guide](README_SU01_TEST.md) for details.

For SAP-independent checks, build unit_tests and run ctest --test-dir build -C Release --output-on-failure.

After building the Release CLI, `python -m unittest tests/integration/test_cli_logging.py` checks that an error-level environment override leaves routine JSON output free of early informational stderr. The test also passed with a live SM59 session open.


## Bigfox regression suite and build comparison

Two read-only PowerShell 5.1 scripts turn the ad-hoc live checks into a repeatable suite against the Bigfox (SAP A4H) system. They are not registered with CTest. Verification of the checks is done by live runs of the maintainers/orchestrator; see [BASIS_SMOKE_TESTS.md](BASIS_SMOKE_TESTS.md) for the transaction catalogue.

Prerequisites

- Windows, SAP GUI with scripting enabled (client and server), no external PowerShell modules.
- A logged-in Bigfox session open as `/app/con[0]/ses[0]`, sitting on the SAP Easy Access screen, with no popup open. The user needs display authorizations only.
- A built `fairyfly.exe` (`build\Release\fairyfly.exe` by default; `build\bin\Release\fairyfly.exe` is used as a fallback).
- The scripts never read `trial.env` and never print credentials (`trial.env` remains git-ignored).

Transactions used: RZ11, ST22, SM37, SU01 (display), SM59, SE80 (bigfox_regression.ps1); additionally SM50, RZ04, SE16 (TADIR), SE11 (TADIR), SEGW, /IWFND/MAINT_SERVICE, SE38, SICF (compare_builds.ps1).

They are read-only apart from selection-screen fills (RZ11 parameter name, SM37 user `*` and from-date, SU01 user `DEVELOPER`, SE16/SE11 table `TADIR`). Buttons that are deliberately never pressed: SM37 btn[46] Release, btn[25] Stop, btn[14] Delete; SU01 btn[8] Create and btn[20] Change Password; any Save. The `--read-only` guard checks only send refused actions and change no state. Every script ends with `transaction start /n` and closes leftover popups even on failure.

### bigfox_regression.ps1

~~~powershell
# plan only, does not call fairyfly
powershell -NoProfile -File tests\integration\bigfox_regression.ps1 -DryRun
# single run
powershell -NoProfile -File tests\integration\bigfox_regression.ps1
# soak: 20 iterations, 2 s slow threshold, log copy
powershell -NoProfile -File tests\integration\bigfox_regression.ps1 -Iterations 20 -SlowMs 2000 -LogFile scratch\soak.log
~~~

Parameters: `-Exe`, `-Iterations` (default 1), `-SlowMs` (default 3000), `-LogFile`, `-NoDestructiveGuardTests` (skip the `--read-only` refusal checks), `-DryRun`.

Output: one line per check, `PASS|FAIL|SKIP <name> [<ms>ms]` (with a reason after `-` for FAIL/SKIP), then a summary. SKIP means a precondition was missing (for example no ST22 dumps today) and does not fail the run; checks that depend on a failed or skipped earlier check are skipped. In soak mode the summary adds the slowest 10 calls (command line, ms, iteration), every call slower than `-SlowMs`, and a list of intermittent checks (passed in some iterations, failed in others). Exit code: 0 all passed, 1 at least one FAIL, 2 no SAP session.

Expected runtime: roughly 1 to 3 minutes per iteration on a responsive Bigfox (about 100 fairyfly calls); soak runs scale linearly.

### compare_builds.ps1

~~~powershell
powershell -NoProfile -File tests\integration\compare_builds.ps1 -OldExe C:\builds\old\fairyfly.exe -NewExe build\Release\fairyfly.exe
powershell -NoProfile -File tests\integration\compare_builds.ps1 -OldExe old.exe -NewExe new.exe -Screens SM50,SE80
~~~

For each of 18 read-only screens the script navigates once with `-NewExe`, then reads `screen read --no-tabs --max-rows 100 --output json` with both builds (best of 2 runs each), and prints Screen / OldMs / NewMs / OldEl / NewEl / OnlyOld / OnlyNew / OldKB / NewKB. The first 8 ids that differ are printed per screen. Exit code 1 if any element id differs, 2 if no SAP session. A screen whose navigation or read fails is reported as SKIP.

## MCP smoke test (mcp_smoke.ps1)

`tests\integration\mcp_smoke.ps1` is a PowerShell 5.1 script that spawns `fairyfly mcp`, speaks newline-delimited JSON-RPC to it and checks the MCP server end to end. It is not registered with CTest and not part of the Bigfox regression suite.

Prerequisites

- A built `fairyfly.exe` (`build\Release\fairyfly.exe`, fallback `build\bin\Release\fairyfly.exe`).
- A logged-in SAP GUI session on the SAP Easy Access screen with no popup; display authorizations are enough (the SM37 selection screen is used). Without a session the script exits with code 2.
- No credentials are read or sent.

What it presses: it navigates with `/nSM37` and `/n`, reads and captures the screen, and (write mode) fills the SM37 job name field with the marker `ZMCPSMOKE`, then restores it to `*`. It never presses Save, Delete, Release, Stop, Create or Change: the Save button, `key send f11`, `multiple_logon=end` and the hidden `gui_element_fill` are only sent to a read-only server, which refuses them before SAP is touched. It always ends with `transaction start /n` and closes the servers, also on failure.

~~~powershell
# plan only, spawns nothing
powershell -NoProfile -File tests\integration\mcp_smoke.ps1 -DryRun
# full run (read-only server, write-mode server, env hard-cap server)
powershell -NoProfile -File tests\integration\mcp_smoke.ps1
# without the --allow-write and FAIRYFLY_READ_ONLY servers
powershell -NoProfile -File tests\integration\mcp_smoke.ps1 -SkipWriteMode -AuditFile scratch\mcp-audit.jsonl
~~~

Parameters: `-Exe`, `-AuditFile` (default a temp file; the write and cap servers use `<name>-write.jsonl` and `<name>-cap.jsonl`), `-DryRun`, `-SkipWriteMode`. Output: `PASS|FAIL|SKIP <name> [<ms>ms]`, then a summary. Exit code: 0 all passed, 1 at least one FAIL, 2 no SAP session. Expected runtime: about a minute on a responsive system. See [docs/MCP.md](../../docs/MCP.md) for the server itself.

## HTTP MCP smoke test (mcp_http_smoke.ps1)

`tests\integration\mcp_http_smoke.ps1` starts `fairyfly mcp --http --no-tls` (http.sys, plain HTTP, loopback only) on the fixed port 18383 (a read-only server, an `--allow-write` server, and a server with `--allow-ip 203.0.113.0/24`), creates temporary bearer tokens with `mcp token create` and checks the HTTP surface over `Invoke-WebRequest`: 401/405/415/404, both protocol eras, `tools/list` order, a real `gui_screen_read`, `SCOPE_DENIED`, `TCODE_DENIED` (SM50 allowed, SE16 refused), `RATE_LIMITED`, revocation within 6 s, SSE framing, that a spoofed `X-Forwarded-For` is ignored (`http.xff_ignored`, `http.forwarded_cannot_bypass_ip`: the token `--ip` binding uses the real peer address), that the server `--allow-ip` list never locks out loopback (`http.allow_ip_loopback_always`), the audit fields (`principal`, `transport: http`, `remote_addr`, no token string) and a clean Ctrl+C shutdown. Readiness is an HTTP request answered with 405, not a TCP connect. Deployment guide: [docs/MCP_REMOTE.md](../../docs/MCP_REMOTE.md).

Dev reservation (one time, elevated, required): http.sys has no ephemeral ports and needs a URL reservation for every non-elevated listener, even on loopback, so the port is fixed and the reservation must exist:

~~~powershell
netsh http add urlacl url=http://127.0.0.1:18383/mcp/ user=%USERDOMAIN%\%USERNAME%
# remove later: netsh http delete urlacl url=http://127.0.0.1:18383/mcp/
~~~

If the server fails with `BIND_FAILED` reason `no_url_reservation`, the script prints that exact command and exits 2. With another `-Port`, reserve that port instead.

Prerequisites: as for `mcp_smoke.ps1` (logged-in SAP Easy Access session, Windows PowerShell 5.1, built exe) plus the dev reservation above and the Windows Credential Manager of the current user, where the tokens live. It presses and fills nothing; it starts SM50 (display) with the T-code-restricted token and returns to `/n`. Tokens are named `ffsmoke-<random>-<role>`, are never printed, and are revoked and deleted (`cmdkey /delete:fairyfly-mcp:<name>`) in a `finally` block. A temporary yaml config and audit file keep the user's real ones out of the run. The expired-token check is reported as SKIP (a past `--expires` is rejected at creation; that rejection is checked instead).

~~~powershell
powershell -NoProfile -File tests\integration\mcp_http_smoke.ps1 -DryRun
powershell -NoProfile -File tests\integration\mcp_http_smoke.ps1
powershell -NoProfile -File tests\integration\mcp_http_smoke.ps1 -SkipWriteMode -Port 18383 -AllowedTcode SM50 -DeniedTcode SE16
~~~

Parameters: `-Exe`, `-Port` (default 18383, fixed), `-AuditFile` (write server uses `<name>-write.jsonl`, the allow-ip server `<name>-allowip.jsonl`), `-AllowedTcode`, `-DeniedTcode`, `-DryRun`, `-SkipWriteMode`. Output as for `mcp_smoke.ps1`; exit codes 0 all passed, 1 a FAIL, 2 a prerequisite is missing (no SAP session, or no URL reservation).

## End-to-end http.sys test (mcp_e2e_httpsys.ps1) - the Definition of Done

`tests\integration\mcp_e2e_httpsys.ps1` is the acceptance test of the http.sys listener and of `mcp setup|doctor|teardown`. It must pass end to end on the maintainer's machine before the feature counts as done. It walks the twelve Definition-of-Done items and prints one named PASS/FAIL/SKIP line per check:

1. `doctor.before`: nothing set up, urlacl/sslcert/certificate missing with a remedy, exit 1.
2. `setup.dry_run`, `setup.runbook`: plan and runbook without any change or prompt.
3. `setup.apply`, `setup.apply_state`, `setup.idempotent`, `cert.export`: `mcp setup --yes`, verify by a real TLS round trip, urlacl for the user and sslcert visible in `netsh`, `.cer`, manifest and yaml written; the second run says "nothing - already set up.".
4. `doctor.after_setup`: no failing check.
5. `server.start`, `server.banner`, `tls.handshake`, `http.plain_on_tls_port`: unelevated `mcp --http`, TLS >= 1.2, the exact certificate, plain HTTP on the TLS port refused.
6. `http.other_path_404`, `http.get_405`, `http.content_type_415`, `http.no_token_401`, `http.bad_token_401`, `http.wrong_host_403`, `http.oversized_413` (answered before the body is sent).
7. `token.create`, `mcp.initialize.curl|ps`, `mcp.tools_list.curl|ps`, `sap.gui_session_list`, `sap.gui_screen_read`, `sse.tools_call.h2|http1`, `audit.http_fields`, `audit.no_token_string`.
8. `authz.scope_denied`, `authz.read_only_refused` (write-mode server, nothing reaches SAP), `token.revoke` (401 within 6 s).
9. `ip.token_bound_refused`, `ip.xff_ignored`, `ip.forwarded_cannot_bypass`, `ip.server_allow_ip_loopback`, `ip.server_allow_ip_lan` (server `--allow-ip`, reached through the LAN address with `curl --resolve`).
10. `pool.slow_bodies` (12 slow-body and 12 half-header TLS clients, SslStream with a thumbprint-pinned callback; a legitimate request stays under 3 s), `pool.half_header_closed` (informational: prints how many half-header sockets are still open after 30 s, because http.sys closes them only at the machine-wide connection timer, about 120 s; a legitimate request must still be answered), `server.clean_stop` (Ctrl+Break, exit 0 within 15 s; also `.rw` and `.allowip`).
11. `teardown.apply`, `teardown.idempotent` ("nothing - already removed."), `token.cleanup`, `doctor.after_teardown`: back to the initial state, no fairyfly certificate left in `LocalMachine\My`.
12. `suites.unit_tests`, `suites.bigfox_regression`, `suites.mcp_smoke`, `suites.mcp_http_smoke`: all must be green (run these with the dev reservation for port 18383 in place; `-SkipSuites` skips them and reports SKIP).

~~~powershell
# plan and check list only: parses, starts nothing, touches nothing
powershell -NoProfile -File tests\integration\mcp_e2e_httpsys.ps1 -DryRun
# full run (asks once for confirmation, then two UAC prompts)
powershell -NoProfile -File tests\integration\mcp_e2e_httpsys.ps1
# unattended confirmation, without the other suites and the SSE checks
powershell -NoProfile -File tests\integration\mcp_e2e_httpsys.ps1 -Yes -SkipSuites -SkipSse -Hostname myhost.example.com
~~~

Parameters: `-Exe`, `-Hostname` (default: this computer's DNS name, lower case; must be the name in the certificate and should resolve), `-Port` (default 8443), `-SkipSuites`, `-SkipSse`, `-DryRun`, `-Yes`. Exit codes: 0 every check passed (a SKIP always prints its reason), 1 at least one FAIL, 2 a prerequisite is missing.

Prerequisites: Windows PowerShell 5.1 started NON-elevated (the script warns when elevated: the server must run unelevated, otherwise the test proves nothing), UAC available, a logged-in SAP session (Bigfox, SAP Easy Access, no popup), `curl.exe`, a built `fairyfly.exe` (and `unit_tests.exe` for the suites step), and a clean machine: no fairyfly URL ACL, SSL binding, certificate or `mcp-setup.json` from an earlier run (the script checks read-only via `netsh http show urlacl|sslcert` and exits 2 with the teardown command otherwise).

UAC prompts: the script itself never elevates. `mcp setup --yes` and `mcp teardown --yes` self-elevate, so exactly two UAC prompts appear (the script prints `A UAC prompt will appear now - please approve` before each); the script only asserts on the results. Without `-Yes` it asks for confirmation once before starting.

Recovery: the script runs setup and teardown around the server checks. If it aborts after setup but before a successful teardown (a FAIL that stops the run, Ctrl+C, a declined prompt), the `finally` block stops servers, deletes all `ffe2e-<rand>-*` tokens, restores the TLS callback and prints the recovery command; with `-Yes` it runs it (one more UAC prompt):

~~~powershell
build\Release\fairyfly.exe mcp teardown --yes
~~~

Isolation: a temp config (`-c`), a temp audit file (`FAIRYFLY_AUDIT_FILE`) and temp files under `%TEMP%\fairyfly-e2e-httpsys-*` keep the real `mcp.yaml` and audit trail out of the run; tokens are held in variables only and never printed. Trust: `curl.exe` gets `--cacert <exported PEM>` (never `-k`; `--ssl-no-revoke` because a self-signed certificate has no revocation endpoint), Windows PowerShell 5.1 and the raw TLS clients accept only the exact expected SHA-1 thumbprint. In SAP it only reads (`gui_session_list`, `gui_screen_read`) and attempts refused writes; it never presses Save, Delete, Release or Stop. All flag names, JSON field names and literal texts the script assumes about the CLI are collected in the marked `ASSUMPTIONS ABOUT THE C++ SIDE` block at the top of the script.
