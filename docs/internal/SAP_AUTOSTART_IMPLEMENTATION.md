# On-demand SAP startup

## Agreed behavior

- Prepare a session only when an operation needs one; never at tray startup.
- Reuse a suitable authenticated session without requiring startup permission.
- Tray-supported modes use a persistent local checkbox, off by default.
- Modes without tray support use `--allow-sap-startup`.
- Unchecked means refuse, with actionable structured errors. Never prompt to override.
- Use the locally configured default SAP Logon connection.
- Log in with local Windows Credential Manager credentials or SSO. Never send secrets through MCP or the gateway.
- Link additionally requires an explicit gateway startup grant. Missing grants from older gateways deny startup.
- Gateway administrators can disable startup independently of business writes.
- Preserve identity, connection, system, transaction, owner and lease restrictions.
- Do not silently replace an explicitly selected connection or terminate another login.

## Delivery gates

1. Red/green tests for shared policy and preparation flow, including concurrency, login failure and retry behavior.
2. Shared CLI/MCP/link lifecycle integration; bounded process startup, reuse, login, readiness verification.
3. Tray checkbox, local default configuration, CLI switch and config propagation to HTTP workers.
4. Gateway tenant setting, administration UI, signed grants and desktop enforcement (including batches).
5. Endpoint documentation and workflow skill: original calls prepare sessions, errors identify corrective action.
6. Real SAP checks across all modes: closed process, logged out, existing session, local denial, admin denial,
   missing credentials and manual-login recovery. Measure original-operation latency and retain evidence.
7. Green desktop and gateway PRs; compatible release order; production deployment and installed-package live checks.

## Evidence so far

- Desktop base updated to d685c0c (merged tray/toast fix PR 134); gateway base fe68f4b.
- First red run: `scratch/startup-red-tests.log`: 4 cases failed, 4 failed assertions, 2 passed assertions.
- Policy, grant serialization, link context and guarded process startup: 11 cases / 30 assertions green.
- Red/green logs: `scratch/startup-{grant,bridge,process}-{red,green}-tests.log`.
- Native process startup locates SAP Logon through App Paths / standard install locations, starts minimized,
  serializes process creation with a named mutex, and waits at most 20 seconds for scripting readiness.
- Gateway PR 167: dce29c6 contains deliberately red admin API tests. CI run 41151436983 finished red:
  351 cases, 350 passed, only the new administrator-setting test failed (5 expected assertions).
  Evidence is `fairyfly-gateway-work/startup-red-ci.log` (untracked). Implementation, persistence, translations,
  tenant cap and browser test pushed as 1f27880; green CI pending. Do not merge before desktop/live acceptance.
- No automatic lifecycle integration or live validation yet. No release/deployment has happened.
- Tray checkbox and default settings have red/green coverage. Both tray controllers use the same HKCU values;
  tray reads ignore the CLI opt-in and headless reads require it. `session default [NAME]` stores/shows the local
  default without enabling startup. `--allow-sap-startup` parses for CLI, MCP and link.
- Combined `[startup],[tray],[command_table]` run: 117 cases / 1371 assertions passed, logged in
  `scratch/startup-cli-green-tests.log`. Earlier red logs: `startup-tray-red-tests.log`,
  `startup-settings-red-tests.log`, `startup-cli-red-tests.log`.
- CLI smoke: `build-msix/Release/fairyfly.exe session default` returned empty default and startup false;
  help shows the new flag. No SAP session was changed.

## Next work

### Recovery and gateway cap checkpoint (2026-10-31, 03:23 local)

- Gateway runtime/old-version regression commit 7ccee01 failed as intended in CI 41157950331:
  352 cases, 350 passed, two failed, eight failed assertions. Evidence:
  `fairyfly-gateway-work/startup-org-runtime-red-ci.log`.
- Gateway c0264ef now reads the organisation cap when signing each call and persists the cap under the
  separate `orgstartup:<domain>` key, which old versions do not rewrite. Organisation policy UI explains
  that startup includes local credentials/SSO. CI 41158666831 completed fully green, including browser e2e.
- Gateway 29e217f adds the next deliberately red tests: default/malformed startup policy frames, initial
  ready policy and live updates when tenant startup changes. CI 41159331236 is running. Implement protocol,
  push/reconnect policy and desktop status after confirming that red result. Do not merge red tests.
- Desktop manual recovery and partial owner enumeration tests failed red (two assertions). MCP now checks
  for a pending marker only after a successful authorized screen call. It tries the preparation lock without
  waiting, verifies the live identity and connection name, then clears only the matching default's marker.
  Ordinary calls with no marker do not add SAP probes. Worker partial enumeration now refuses instead of
  pretending the session list is complete. Green: 25 assertions in the two targeted cases.
- A new consecutive-call test exposed a prelogin session-key transition leaving stale sticky identity
  (one assertion failed). Successful preparation now remembers its final authenticated identity from the
  readiness probe, without another COM read. The live Bigfox two-read check already succeeded before this
  correction (this system's key stayed stable), so the regression covers another supported key lifecycle.
- Owner redaction regression reproduced the already-mapped OWNER_SESSION_UNAVAILABLE metadata leak.
  That code is now included in the boundary redaction set; the regression passes. Endpoint instructions
  now recommend a compact first screen read and explain on-demand startup and local credential management.
- Targeted startup/owner/worker suite: 94 cases / 647 assertions passed. Full suite on these changes:
  1577 cases, 1576 passed, one skipped, 26492 assertions passed, exit 0, in
  `scratch/startup-manual-followup-full-tests.log`.
- Real manual-recovery marker check: explicit MCP reads of current connection 2 cleared the seeded Bigfox
  marker but retained an unrelated Other marker (then removed that test marker). Evidence:
  `scratch/startup-live-mcp-manual-recovery{,-markers}.json`.
- Real retry/recovery scenario used a seeded isolated marker, never an incorrect password. After closing
  the test Easy Access session, two separate MCP startup runs refused SAP_LOGIN_REQUIRED with the same
  open connection 2 (1859ms, then 469ms); session listing confirmed S000/user empty/client 000. Explicit
  local `session login --connection 2` succeeded. Subsequent MCP explicit reads without startup permission
  succeeded (312ms first read), and the marker was cleared. Evidence files:
  `scratch/startup-live-mcp-retry-blocked-{first,second,session}.json`,
  `scratch/startup-live-mcp-explicit-recovery-login.json`,
  `scratch/startup-live-mcp-after-explicit-recovery.json`,
  `scratch/startup-live-mcp-explicit-recovery-marker.json`.
- Current live SAP is logged on at Easy Access; isolated MCP cache connection 2 is current. The HTTP test
  cache is now stale. Reobserve before closing. No business data changed.

Remaining: gateway policy push + vendor sync + tray admin status; MCP cached/sticky logged-out recovery
(same-process stale targets still need integration); owner existing-prelogin path; batch/lease preparation;
fresh-screen action continuation guard; live link/admin/parallel-startup checks; final Claude review; final
green PRs, release, production deployment and installed-package verification. Goal remains active.

### HTTP live acceptance and review checkpoint (2026-10-31, 03:02 local)

- Previous goal turn made progress: desktop ce14560 committed/pushed the login-guard/recovery fixes,
  gateway ddbd947 committed/pushed the organisation cap, and desktop full regression and live reuse passed.
- Real stdio MCP with the test SAP session closed: startup disabled returned structured
  `SAP_AUTOSTART_DISABLED` in 31ms; startup enabled opened/logged into Bigfox A4H/001/DEVELOPER and
  completed the original screen read in 4718ms. Evidence: `scratch/startup-live-mcp-closed-{disabled,enabled}.json`.
- HTTP live reuse exposed `SAP_SESSION_NOT_READY`: owner-filtered listings intentionally contained only session
  IDs. Regression `startup-owner-reuse` failed with two assertions. Internal preparation now retains only
  freshly verified owner target metadata, while ordinary client listings still omit identities. The outer
  call still goes through target/owner authorization. Live reuse passed in 1250ms after this fix.
- Real HTTP owner endpoint with its test session closed: disabled startup gave structured actionable refusal
  in 188ms; enabled startup launched/logged in and completed the original read in 6485ms. Temporary tokens
  were deleted and HTTP processes stopped by the harness. Evidence: `scratch/startup-live-http-closed-*.json`
  and `scratch/startup-live-http-reuse-fixed.json`; harness `scratch/startup_live_http.py`.
- SAP remains at Easy Access / SESSION_MANAGER; HTTP isolated cache connection 0 is current. Other isolated
  caches may contain stale IDs. Reobserve before any close. No business data changed.
- Incomplete discovery red: one case, 9 failed assertions (partial enumeration ignored, malformed fields
  could throw). Now reject partial enumeration, malformed counters/names and incomplete logged-on identities.
  Added ambiguity, default filtering, post-login readiness and post-launch permission/default-change tests.
- Combined startup/owner suite passes 86 cases / 582 assertions. Full regression passed with exit 0:
  1573 cases, 1572 passed, one skipped, 26448 assertions passed, in
  `scratch/startup-discovery-owner-full-tests.log`.
- Stopped-process HTTP live test then exposed a second bug: worker discovery mapped confirmed process absence
  to OWNER_IDENTITY_UNKNOWN, preventing startup. Red worker regression failed one assertion. The worker now
  reports an empty listing only for confirmed SAP_LOGON_NOT_RUNNING; malformed/failed enumeration still refuses.
  Combined startup/owner/worker tests pass 91 cases / 609 assertions in
  `scratch/startup-process-owner-green-tests.log` (full suite above predates this last worker correction).
- Real process test: verified the only session was this task's Easy Access session, closed it, confirmed zero
  sessions, then stopped saplogon PID 7856. HTTP startup disabled now returns SAP_AUTOSTART_DISABLED (406ms).
  With startup enabled, the same original read starts SAP Logon, logs in and returns Easy Access in 5735ms.
  Evidence: `scratch/startup-live-http-process-stopped-{disabled,enabled}-fixed.json`; red evidence retained too.
  SAP is running/logged on again, isolated HTTP connection 0. No business data changed.
- Claude review 3 completed: `scratch/claude-startup-review-3.txt`. It found no latch bypass or repeated-login
  path. Accepted remaining items: preserve the org cap through old-version saves and enforce it at runtime;
  redact already-mapped OWNER_SESSION_UNAVAILABLE metadata; MCP ordinary-call latch clearing. Add a final
  review after those and the outstanding startup integrations are fixed.
- Gateway ddbd947 CI 41157165534 completed green. New red commit 7ccee01 adds runtime signed-grant and
  old-version-save regression tests for Claude's cap findings; CI 41157950331 is running. Do not merge it red.
- Review disposition: the startup grant intentionally authorizes local stored-credential login, as requested;
  it does not require separately exposing a login tool to the assistant. Document this in the org UI too.
  The password-change finding is not currently reachable through automatic startup: both adapters use
  `LoginSource` with no file/stdin, and `StoredCredential` contains no new-password field. Add a regression
  for the password-change-required result rather than assuming the review's stored-new-password path exists.

Remaining scope is unchanged. In addition to the earlier gates, preserve the new HTTP owner-listing regression
in future routing changes. This checkpoint is not release/deployment evidence.

### Review follow-up checkpoint (2026-10-31, 02:51 local)

This checkpoint supersedes the pending status in older entries below. The feature is not released.

- Claude review 2 completed (`scratch/claude-startup-review-2.txt`). Accepted fixes: move the durable
  attempt latch to immediately before the credential-submit action; preserve structured preparation errors;
  recheck policy after native readiness waits; catch failed facts probes; report latch-clear failures.
- Saved CLI recovery: stale/logged-out sessions use guarded preparation, a different saved/default target is
  refused, and verified manual authentication clears only the matching latch. Red: 3 cases / 5 failed assertions;
  green: 3 cases / 14 assertions (`scratch/startup-recovery-{red,green}-tests.log`).
- HTTP implicit missing-session routing now reaches global preparation; explicit/default/sticky targets still
  retain their routing checks. MCP forwards cancellation to native startup. Red: 2 cases / 3 failed assertions;
  green: 5 cases / 35 assertions (`scratch/startup-http-{red,green}-tests.log`). Live HTTP acceptance is pending.
- Before-submit handler tests failed red in 2 cases / 5 assertions; then passed. New MCP tests proved scope
  denial does not record an attempt, failed submission does, and guard hooks are reset. They additionally
  exposed missing structured recovery metadata (2 failed assertions); that is now fixed with owner-boundary
  redaction preserved. Combined targeted run: 43 cases / 205 assertions passed in
  `scratch/startup-review-fixes-green-tests.log`. Full regression passed: 1568 cases, 1567 passed,
  one skipped, 26411 assertions passed. Explicit process exit 0 recorded with
  `scratch/startup-review-fixes-full-confirmed.log` (the first redirected PowerShell run had an ambiguous
  shell status despite the same all-passing Catch2 summary, so the exit code was confirmed by rerun).
- Real SAP follow-up on the new binary: read-only CLI, startup disabled, reused the logged-on Bigfox
  A4H/001 session and returned SAP Easy Access / SESSION_MANAGER in 1029ms. Evidence:
  `scratch/startup-live-review-fixes-reuse{,-timing}.json`. This checks reuse only, not remaining startup paths.
- Gateway organisation-cap red commit 43fadc2: CI 41156195658 failed only the new cap assertions (5 failures,
  351 of 352 cases passed). Green implementation ddbd947 pushed; CI 41157165534 running. It adds persistence,
  IT policy checkbox, propagation to existing tenants and refusal of prohibited department settings.
  Organisation cap defaults to unrestricted for migration; tenant and desktop opt-in still default off.
- Claude review 3 is running read-only on the before-submit/metadata fixes and gateway organisation cap.
  Output: `scratch/claude-startup-review-3.txt`, errors alongside it. No SAP or credential access is authorized
  for that review.
- Updated MCP/link documentation and workflow skill; skill validator passed. Endpoint instructions remain to
  be updated after integration stabilizes. Personal installed skill is not yet replaced.

Still required: MCP cached/sticky/manual-login recovery; owner HTTP live startup and existing prelogin path;
batch/lease preparation; prevent screen actions against an unobserved screen after new startup (Claude 2);
malformed/partial discovery and dynamic-policy tests; link tray admin status; all live acceptance scenarios;
final review, green PRs, release and production deployment. No business data changed during these fixes.

### Current checkpoint (2026-10-31, 02:25 local)

- Shared discovery/reuse/launch/login is now implemented in `session_preparation.*`. The whole sequence holds
  a Windows session-wide mutex; discovery occurs after acquisition. The wait is bounded and cancellable.
- `startup_coordination.*` persists an attempt before automatic credentials are submitted. It uses only a hash
  of the configured connection name. Failed/uncertain attempts require manual login; verified authentication
  clears the latch. This is deliberately conservative across process restarts and new connection windows.
- CLI entry, batch flag inheritance, MCP stdio/link and HTTP global dispatcher now receive the appropriate local
  policy. MCP internal lifecycle calls go through `run_single` admission; link grants are overlaid per call.
- MCP success/explicit-target/permission/scope tests: 4 cases / 22 assertions. Shared safeguard red run: 3 failures;
  green `[startup]`: 34 cases / 145 assertions. Integrated targeted run: 201 cases / 2207 assertions green.
- Full suite before the latest corrections: 1559 cases, 1556 passed, 2 failed, 1 skipped (symlink privilege).
  Failures were centralized environment access in the native launcher and missing `session default` help examples;
  both corrected. Rerun full suite after the native readiness change.
- Gateway signed-grant admission tests committed/pushed as 246fbda; CI run 41154662260. Unit/web build and
  container smoke passed; browser e2e was still running at last check.
- Real SAP baseline: Logon running, no sessions (`scratch/startup-live-baseline.json`). Isolated CLI state proved
  disabled and missing-default errors. Set local default to Bigfox; tray opt-in remains false. A read-only CLI
  `--allow-sap-startup screen read` opened/logged into A4H/001/DEVELOPER and returned SESSION_MANAGER in 8824ms.
  Evidence: `scratch/startup-live-cli-login{,-timing}.json`. No business data changed.
- Real stdio MCP, isolated fresh cache, no startup flag: reused the logged-on Bigfox session and returned
  SAP Easy Access in 1187ms. Evidence: `scratch/startup-live-mcp-reuse.json`; harness `scratch/startup_live_stdio.py`.
  SAP remains logged on; isolated CLI/MCP saved connection id is 0 (separate from the normal installed cache).

Outstanding integration details (must not call this released or complete): native readiness/cancellation;
stale saved CLI/sticky targets; logged-out saved sessions; owner HTTP route bootstrap (coordinator currently
rejects missing identity before global dispatcher); batch startup/leases; fresh-policy validation at each
mutating boundary; remaining discovery robustness tests; gateway org caps; link tray admin status;
instructions/skills/catalog; all live acceptance scenarios; next Claude review; PR/release/deployment.

Checkpoint follow-up:
- Full suite rerun completed green: 1560 cases, 1559 passed, 1 skipped (symlink privilege), all 26354 assertions
  passed. Evidence: `scratch/startup-full-green-tests.log` (process exit 0).
- Gateway CI 41154662260 completed fully green, including browser e2e and ci-ok.
- Native readiness red: one case / two assertions failed. Green `[startup],[handler],[env],[agent_help]`:
  84 cases / 1400 assertions. Native wait now accepts cancellation and cannot create a new process when it is
  only waiting for an already-running SAP Logon. The MCP starter callback still needs to forward cancellation.
- Review 2 launched read-only with Claude CLI and external MCP configuration disabled; output will be in
  `scratch/claude-startup-review-2.txt` / `.err`. It focuses on coordination, retry protection and dispatcher admission.
- Additional known recovery gap: when a person completes manual login on a cached session and preparation is
  skipped, the verified successful ordinary operation must clear the corresponding login latch. Do not clear
  another configured connection's latch or weaken target/owner admission to do this.

- User requested periodic Claude code reviews. Initial read-only review launched with the installed Claude CLI
  on 2026-10-31, covering desktop and gateway policy/lifecycle changes. Output:
  `scratch/claude-startup-review-1.txt` (stderr alongside it). Request another review after transport integration
  and before release; resolve findings and retain the disposition in this log.
- Shared preparation flow red: 6 failing cases; green: 22 cases / 90 assertions. CLI handler integration red:
  2 failing cases / 3 failed assertions; green: 24 cases / 97 assertions in
  `scratch/startup-handler-green-tests.log`. Current adapter reuses authenticated sessions, launches/logs in
  through existing lifecycle handlers, and reports disabled startup for implicit missing-session tasks.
  CLI entry point, MCP admission, concurrency/cancellation, and live acceptance remain outstanding.
- Gateway PR 167 CI run 41152096258 is green, including unit, web, container smoke and browser tests.

- Integrate session discovery/reuse/default-connection launch/login and readiness into CLI and MCP admission.
  The new `ensure_sap_logon` handler method is a guarded primitive, not yet invoked by ordinary operations.
  Never let implicit preparation bypass token scopes/target authorization or owner-mode lease routing.
- Wire the implemented tray checkbox/default (`startup_settings.*`) and global CLI option into execution. Apply gateway permission via
  `CallContext::link_request` and `sap_startup_granted`; the bridge now carries the verified signed grant.
- Show gateway startup prohibition in the link tray without disabling the shared local setting for local MCP.
- Native startup has no cancellation hook yet and only serializes process start, not connection/login;
  cover cancellation and concurrent session preparation before live acceptance.
- Gateway runtime cap currently intersects tenant `allow_sap_startup` with the policy grant. Missing persisted
  tenant field means false. Existing clients omitting the new settings field preserve its stored value.
- `npm` is not on this VM's PATH; gateway web typecheck/build/browser tests will run in repository CI.

## Implementation notes

### Claude review 1 (2026-10-31)

Read-only review completed; stdout remained buffered in the CLI process, so the final assistant response was
recovered from its session transcript into `scratch/claude-startup-review-1-recovered.txt`.

Accepted findings to resolve before release:
- Persist a failed/uncertain automatic-login latch so subsequent calls cannot blindly retry stored credentials.
- Serialize discovery, connection creation and login across processes, with bounded cancellable waits and rediscovery.
- Wait for a running SAP Logon to become scriptable before blaming scripting configuration.
- Add gateway admission tests proving the signed grant is capped independently of writes.
- Preserve opened-connection facts on cancellation and test malformed/ambiguous discovery and unsuccessful readiness.
- Cover stale saved connections and retain explicit target/owner identity protections.

Integration constraints confirmed by review:
- The handler startup adapter is CLI-only. MCP must invoke lifecycle operations through dispatcher admission;
  each call overlays the verified link grant and reauthorizes the final target. Never install the CLI automatic
  startup callback on the shared MCP handler.
- HTTP callers remain subject to session launch/login scopes, system/connection restrictions and owner checks.
  A read-only scope alone does not authorize automatic login. No additional token privilege is silently granted.
- The configured local default controls implicit startup selection. An explicitly attached usable session remains
  selected; another system must not silently replace it.
- Handlers are confined to their COM executor; the boolean recursion guard is not a cross-thread mutex.
- Review org-admin startup caps against existing gateway organization administration before release.

`CommandHandler::resolve_and_validate_connection` is the common selection path, but MCP authorizes
targets before invocation. Session preparation must cooperate with dispatcher admission and reauthorize
the resulting identity rather than opening a session invisibly beneath those checks.
`CommandDispatcher::auto_attach_and_retry` currently handles only local stdio/link and existing sessions.
HTTP owner routing needs explicit coverage. `third_party/libffl/src/policy.*` in the desktop is a vendored
copy of gateway policy code, not a git submodule; keep protocol/grant behavior synchronized.

Gateway deployment must use the repository's GitOps promotion workflow (docs/DEPLOY.md).

## 2026-10-31 03:35 checkpoint: observation guard and policy push
- Desktop fresh-screen action guard RED: 2 cases / 5 failing assertions in scratch/startup-observation-red-tests.log. GREEN: [startup] 54 cases / 296 assertions in startup-observation-green-tests.log. Reads/capture/named transaction may continue; other screen actions return SESSION_PREPARED after authorization.
- Real CLI check: closed verified test Easy Access session, started from new scratch/startup-live-observation-state, requested key send enter with startup enabled. Returned SESSION_PREPARED, connection 0, original_operation_run false. Follow-up screen read confirmed Bigfox A4H/001/DEVELOPER Easy Access. Evidence startup-live-observation-action.json and startup-live-observation-after.json. Current live connection is 0 in that isolated state; older MCP cache is stale.
- Full suite startup-observation-full-tests.log overlapped this live call and hit the shared startup mutex: INVALID full-suite run; rerun sequentially after it completes. Do not report it green.
- Gateway policy push RED CI41159331236: 352 cases, 3 failed, 8 failing assertions. Implementation f0ee314 adds LinkPolicy field, ready/push org cap and Agent accessor; mutation patterns updated through 65c4322. All 375 patterns pass locally; CI41160446118 pending.
- Claude review 4 completed: scratch/claude-startup-review-4.txt. No high severity. Follow up startup failure metadata before final target admission, sticky assignment before admission, malformed org verified boolean, authorization-denial and allowed-continuation regression tests. Vendor sync and tray status still planned, not done. Distinguish pushed admin availability from per-call grant.
- Remaining broad scope from previous checkpoint still applies. No release/deployment yet.
