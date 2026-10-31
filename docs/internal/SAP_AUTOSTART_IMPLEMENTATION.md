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

## 2026-10-31 03:43 checkpoint: gateway status in both trays
- Prior turn made progress: desktop bd47d7c and gateway policy-push commits were pushed; real startup action guard passed.
- Desktop sync_libffl.py ran against committed gateway 496f77d (source changes from f0ee314); destination third_party/libffl was verified before replacement.
- Tray policy-status RED: startup-tray-status-red-tests.log, 1 case / 2 failed assertions. GREEN after wiring gateway_sap_startup_allowed status and both menus: startup-tray-status-green2-tests.log, 139 cases / 902 assertions. Includes fresh-screen denial metadata and transaction continuation checks from Claude review4. Local checkbox stays independent of link's admin cap. Status follows existing 15-second heartbeat.
- First green attempt caught a reserved local-setting-name collision in status JSON and a wrong transaction test argument (tcode vs code); corrected, no test weakened.
- Full suite now running separately, scratch/startup-tray-status-full-tests.log, exec session  (see active tool handle). No live SAP startup overlaps it. Previous full run had one preparation mutex timeout and one trail status polling failure; both need the clean suite result.
- Gateway malformed verified regression committed 496f77d; RED CI41160830054 in progress. Desktop status source sync references this commit. Gateway instruction updates are currently uncommitted. Do not push a green fix until the red test result is recorded.
- Remaining scope unchanged: cached/sticky recovery, HTTP prelogin and batch startup/leases, real link checks, review follow-ups, final green/release/deploy. Production is unchanged.
- Follow-up at 03:45: clean full suite still running as exec session 55556; do not restart it. Desktop push2045995 succeeded. New uncommitted [startup-recovery] case in test_mcp_dispatcher.cpp models a second implicit read after the originally prepared default closes; it is not built/run yet. Build it only after the full suite ends, then implement red/green preserving saved name and identity. Gateway malformed-verification RED CI41160830054 still running (Linux job124418053328; web e2e124418053405); image job green. Gateway instructions+assertions are uncommitted and the malformed parser fix has NOT been applied. Review4 outstanding failure metadata/sticky ordering still requires investigation, not assumed exploitable.

## 2026-10-31 03:55 checkpoint: recovery and review5
- Previous turn was progress (desktop2045995 pushed). Separate full suite finished:1580 cases/1578passed/1failed/1skip;3failedassertions only in Session executor pool bounds the number of sessions. Its immediate first job could finish and permit intended idle eviction. Test now holds a promise until rejection is asserted; production pool unchanged. Startup mutex failure from overlapping run did not recur.
- Gateway malformed verified RED CI41160830054 confirmed4type errors (353cases/352pass). Fixed strict boolean check and added startup/recovery endpoint instructions in1040da0. CI41161502018 is FULLY GREEN (Linux/browser e2e/container/ci-ok). Gateway source clean except evidence logs; feature not deployed.
- Desktop implicit sticky recovery RED scratch/startup-recovery-red-tests.log:4cases/3pass/1fail,2assertionsfailed. Initial implementation green startup-recovery-green-tests.log:64cases/371assertions. Follow-up suppresses nested startup sticky writes, binds final sticky only after task admission, and clears login retry marker only for expected user. startup-recovery-followup-tests.log:64cases/373assertions green. All recovery edits remain UNCOMMITTED.
- Claude review5 completed scratch/claude-startup-review-5.txt. Confirmed high issue wrong-user SSO changed sticky before refusal: now fixed and second-call regression passes. Existing reattach regression and username case sensitivity fixed in source; latest build scratch/startup-recovery-review5-build.log running exec24754, not tested yet. Added startup wiring to existing reattach tests and lowercase username recovery section.
- Review5 still OPEN: nonowner recovery currently reuses owner login_target_guard hook, masking credential errors as OWNER_SESSION_UNAVAILABLE. Need separate recovery guard (handler + dispatcher hook wired run_mcp) or equivalent that preserves credential errors while checking anticipated system/client/user before submission. Recovery currently requires target_resolver saved name: closing via session disconnect deletes file and needs retained startup-default-name provenance in StickyTarget to recover safely. Add tests before fixing. Real same-process MCP close/recover test is NOT run yet.
- Other open: startup failure metadata for nonowner constrained tokens; HTTP prelogin/cached recovery; batch startup/leases; actual signed-link checks; missing-credentials/SSO/concurrency live coverage; final documentation/skill/catalogsync, review, fullgreenrelease/deploy. No new production changes.

## 2026-10-31 04:06 checkpoint: recovery guards and startup provenance
- Previous turn was progress: gateway1040da0 green and desktop recovery implementation/tests updated. Latest pre-details startup/reattach/session-executor run:71cases/612assertions green (startup-recovery-review5-tests.log).
- RED details tests:startup-recovery-details-red-tests.log,5cases/2failed/4assertionsfailed: lost cache provenance and masked local credential errors. GREEN:startup-recovery-details-green-tests.log,125cases/1066assertions including login, startup, reattach, owner identity and pool. Guard distinguishes owner redaction from local recovery errors via set_login_target_guard(guard,false), wired through dedicated recovery override; owner hooks unchanged. Startup origin is retained in StickyTarget only after successful original-task admission, allowing deleted saved files to recover the same configured default.
- Real check in progress: verified scratch/startup-live-observation-state connection0 Easy Access SESSION_MANAGER A4H/001/DEVELOPER (startup-live-before-recovery.json). session disconnect --close-session remains pending in exec42428, test fairyfly PID15128 created04:03:34. SAP native window21891324 shows busy/disabled Easy Access, no confirmation dialog. Do NOT restart/kill/retry merely for timeout. Output startup-live-recovery-initial-close.json still empty last checked. Installed trayPID19068 left alone.
- Harness scratch/startup_live_stdio.py now supports --state-name and --close-between-reads with strict verification of test Easy Access before closure and followup timings. After the pending close succeeds, use fresh startup-live-recovery-state with --allow-startup --repeat2 --close-between-reads to keep one MCPprocess alive over close/recovery. That test has NOT run yet. Do not overlap live startup with the full unit suite.
- Build details-green log has final unit_tests binary and focused tests passed; build exec56820 may need terminal poll. Current gatewayPR167greenCI41161502018. No releases/deployments. Remaining HTTP cached/prelogin/batch work and broader live checks still apply. Claude5 high findings fixed; constrained nonowner startup error metadata still needs a regression and decision.
- 04:08 live recovery completed: pending close exec42428 exited0, session closed/filedeleted. Harness exec22736 then passed in one MCPprocess with fresh startup-live-recovery-state: first implicit read4360ms; verified Easy Access, CLI closed session and deleted saved0; second implicit read3891ms successful on new SAP con[1], A4H/001/DEVELOPER Easy Access. Evidence startup-live-mcp-same-process-recovery.json and independent startup-live-recovery-postcheck.json. Current test connection0 resides in startup-live-recovery-state. Full suite now running separately as scratch/startup-recovery-full-tests.log; no live startup overlaps it. Desktop recovery59930d5 pushed and its CI pending.
- Full suite after59930d5:startup-recovery-full-tests.log completed exit0 (exec33892),1582cases/1581passed/1skip,26545assertions allpass. No live startup overlapped.
- HTTP startup-origin recovery RED startup-http-recovery-red-tests.log:1case/2assertionsfailed. Coordinator now sends only missing unauthenticated automatically prepared targets to global admission; explicit/server-default/manual sticky selections remain pinned; saved owner identity still required. GREEN startup-http-recovery-green-tests.log:107cases/745assertions.
- Real same-token/same-server HTTP recovery passed with fresh startup-live-http-recovery-state, restricted temp token A4H/001/DEVELOPER+Bigfox. Initial implicitread4781ms; verified and closed saved0/deletedfile; recoveredimplicitread5282ms, noerrors. Evidence startup-live-http-same-process-recovery.json plus postcheck. Harness exit0, token cleanup had no warning and server terminated. Current test SAP connection0 is in startup-live-http-recovery-state; other test caches stale. These HTTP changes are ready for commit. HTTP prelogin adoption, batch preparation and live signed-link checks remain.

## 2026-10-31 04:37 checkpoint: Claude batch review fixes and live worker integration
- Claude review6 (`scratch/claude-startup-review-6.txt`) found unconditional global batch routing, first-item-only fresh-screen guard, and duplicate first-item rate charging. Routing RED: startup-batch-routing-red-tests.log (4 cases / 1 failed / 6 failed assertions). GREEN routing/batch:40 cases /370 assertions. Planner now retains default/sticky selection and lets coordinator decide global versus session route.
- Budget RED: startup-batch-budget-red-tests.log (1 case /3 failed assertions). Observation+budget RED: startup-batch-observation-red-tests.log (2 cases /4 failed assertions). Private run_single/execute_call/admission_refusal rate_prepaid flag reuses only the first item's paid allowance; admission still charges before preparation. CallState carries startup_changed_screen; owner batch refuses all items if any action needs fresh observation after startup.
- First real HTTP batch after routing fix FAILED BATCH_SESSION_REQUIRED (startup-live-http-batch-review7.json): session workers have no startup_policy hook. Added worker-provider regression (startup-batch-worker-red-tests.log,1 case/2failed assertions); allow implicit resolution for policy.prepare_missing_session too. Focused GREEN startup-batch-worker-green-tests.log:122 cases/866 assertions. Real HTTP existing-session batch GREEN startup-live-http-batch-review7-green.json:two reads in1422ms, A4H/001/DEVELOPER Easy Access. Temporary token/server cleaned up normally.
- Full suite GREEN startup-batch-review-full-tests.log (exec76219 terminal0):1586 cases /1585 passed /1 skipped,26586 assertions passed. Current code and tests remain UNCOMMITTED.
- Claude review7 finished (`scratch/claude-startup-review-7.txt`, exec49810 terminal0): routing and private prepaid rate fix accepted. HIGH remaining: fresh-screen guard only owner HTTP, not stdio/link; read then action or stop_on_error:false can act blindly. Added regression `All MCP transports stop blind batch actions after implicit startup` in test_mcp_dispatcher.cpp for stdio/link x read-first/action-first, but NOT BUILT OR RUN yet. Next: build/run RED once live harness exits, then propagate private batch observation requirement into execute_call/CallState and stop batch unconditionally on SESSION_PREPARED, preserving whole-batch safety. Consider all later unsafe items when a read triggers startup. This fix is NOT implemented.
- Review7 further findings: internal startup lifecycle calls consume caller rate budget (may exhaust low budget midstartup); global startup route has dequeue race with another caller preparing same session; preparation refusals omit SAP context in audit. Review6 low issues hint consistency and all lease-refusal metadata still open. Need resolve/audit with tests before release.
- LIVE RECOVERY CURRENTLY RUNNING exec15973: startup_live_http.py --allow-startup --batch --state-name startup-live-http-batch-state --close-between-reads --evidence startup-live-http-batch-review7-recovery.json. PythonPID6048, HTTPPID17852, workerPIDs12988/18696, CLI closePID19300 created04:35:54. Close verified test Easy Access0 is still pending. DO NOT rebuild while these run; DO NOT retry/kill due observation timeout. Re-poll handle15973. Installed trayPID19068 untouched. Evidence writes when harness completes.
- PR136 c91248e all checks green; PR167 1040da0 all required checks green, both OPEN, neither released/deployed. Full objective still needs remaining prelogin adoption/cached recovery audit/error privacy/live signedlink/missingcredential/concurrency checks, docs/skills/catalog sync, reviews, green final PRs, merge/release/deploy and production real SAP DoD.
- Live recovery exec15973 is now TERMINAL0 (harness process status, not semantic success). First batch succeeded17391ms; verified session close succeeded and deleted cache0; second batch FAILED in15ms with JSON-RPC -32602 / SESSION_ROUTE_CHANGED, message SAP session routing changed before execution. Evidence startup-live-http-batch-review7-recovery.json. No SAP session remains from this test, server/token cleaned up. Rebuild is now safe. This is a new live regression to investigate: cached worker route and startup provenance for a pre-existing default reused by a new server. Do not claim recovery green. Need add coordinator/endpoint regression and preserve safe identity/default recovery rather than loosening explicit target rules.

## 2026-10-31 04:43 checkpoint: all transports, cached recovery and startup budgets
- Previous turn made progress (code, RED/GREEN and live evidence). The all-transport regression failed as intended: startup-batch-all-transports-red-tests.log,1 case/8 failed assertions across stdio/link x read-first/action-first with stop_on_error:false. Added private batch_observation_required propagation to run_single/execute_call/CallState and unconditional stop on SESSION_PREPARED. GREEN startup-batch-all-transports-green-tests.log:101 cases/723 assertions.
- New RED startup-cache-budget-red-tests.log:2 cases/11 failed assertions. One requested operation with budget1 could not complete SSO because internal lifecycle calls were charged again; cached implicit coordinator routes remained on a closed worker. Internal bounded startup calls now reuse the original admitted rate allowance while still applying all scope/target/owner/login checks. Explicit calls remain charged. Implicit startup-capable route admission now probes fresh facts; explicit and server-default routes retain caching. This intentionally adds an admission probe to implicit calls so recovery chooses the right executor before queuing. GREEN startup-cache-budget-green-tests.log:120 cases/866 assertions, including scope-denial tests and exhausted-budget/no-launch followup.
- Real stdio cold blind-batch guard passed: startup-live-stdio-batch-observation.json,4438ms, SESSION_PREPARED on the initial read, later gui_key_send skipped despite stop_on_error:false. No business action ran. Test harness now has --blind-batch using --allow-write solely for this guard test.
- Real HTTP existing-session batch + same-server/token recovery passed: startup-live-http-batch-cache-recovery-green.json,first3875ms; verified test Easy Access0 closed/cachedeleted; recoveredbatch6609ms, both successful. Exec87994 terminal0, temporarytoken/server cleaned up. Current SAP is A4H/001/DEVELOPER Easy Access, saved0 in startup-live-http-batch-state. Earlier recovery failure SESSION_ROUTE_CHANGED is resolved by fresh implicit route admission in this scenario.
- Full suite is running scratch/startup-batch-cache-full-tests.log, exec49280; no live startup overlap. Claude review8 is running exec63908, prompt/output scratch/claude-startup-review-8*.txt. Inspect both before committing this uncommitted batch. Current build is startup-cache-budget-green-build.log final unit_tests line; focus tests and live checks above used that binary.
- Remaining scope unchanged: prelogin owner adoption (currently filtered from internal discovery, likely duplicate window), cached/no-sticky identity/default audit, constrained nonowner failuremetadata, remaining review8/low findings (audit context, consistent lease metadata, global queue race), real signedlink permissions/oldgateway/missingcredentials/concurrency, documentation/skill/catalogsync, finalPRgreen/release/deploy/installedproductionDoD. No release yet.
- Full suite terminal0 exec49280:1588 cases/1587 passed/1 skipped,26643 assertions all passed. New prelogin live RED: verified current Easy Access in startup-live-prelogin-before.json, closed own test0 successfully (startup-live-prelogin-close.json), then session launch Bigfox without --login opened /app/con[0]/ses[0], saved0 (startup-live-prelogin-launch.json). HTTP implicit startup read failed OWNER_SESSION_UNAVAILABLE in891ms (startup-live-http-prelogin-red.json). SAP remains at this test prelogin window, cache0 startup-live-http-batch-state. No adoption fix yet. Do not treat it as authenticated Easy Access for cleanup checks.
- Claude review8 terminal0 exec63908 accepted all new fixes with no release blocker in this batch; HTTP recovery rerun completed green as recorded above. Remaining low followups: audit rewrite can mask SESSION_PREPARED stop reason; owner batch preparation refusal audit lacks context; global startup route dequeue race and worker closure after fresh admission require careful scheduling followup. Positive live two-read batches passed. Ready to commit current batch fixes; broader prelogin/signedlink/release gates remain incomplete.

## 2026-10-31 13:38 checkpoint: live prelogin and multi-owner review
- User explicitly approved closing/recreating the idle Easy Access session. Approved setup completed: startup-live-prelogin-approved-before.json / -close.json / -launch.json; new Bigfox login window /app/con[0]/ses[0], saved0 in startup-live-http-batch-state.
- Real HTTP prelogin adoption GREEN: startup-live-http-prelogin-green.json,4282ms, no error. Independent session list confirmed exactly1 session at same /app/con[0]/ses[0], A4H/001/DEVELOPER, SESSION_MANAGER/SAP Easy Access. No duplicate window. Live test server/temp token cleaned up; current SAP remains logged in idle. No business data changed.
- Prelogin implementation remains UNCOMMITTED. Broker includes verified empty-user targets privately; authenticated candidates preferred; single-owner trusted handler adoption preserves saved receipt; multi-owner requires exact owned claim plus fresh generation/name/identity checks. Prior full GREEN startup-prelogin-full-tests.log:1592cases/1591passed/1skip,26685assertions. Added boundary tests GREEN startup-prelogin-boundary-tests.log:14cases/197assertions.
- Claude10 review completed with no security blocker, functional blocker foreign/expired/unclaimed login windows blocking other tokens. New RED startup-prelogin-filter-red-tests.log:6cases/1failed,15failedassertions. Filter added: multi-owner internal discovery retains only an owned_prelogin_target. First green attempt still RED (12failedassertions) because cached A4H/000 target vetoed fresh launch. Internal owner startup launch now uses fresh handler launch guards rather than cached other-window facts; explicit launches unchanged. Broader build/test running startup-prelogin-filter-green2-build.log / -tests.log, exec34852. Do not claim green until terminal result.
- Claude11 read-only review of this change running exec49732, scratch/claude-startup-review-11*. No release/deploy yet. Current desktop pushed db9aca4, gateway1040da0. Workspace permissions now require escalation for edits/build/git/live work in linked worktrees outside original fairyfly root. Do not modify original dirty repo or evade permissions.
- Latest broader GREEN terminal0 exec34852: startup-prelogin-filter-green2-tests.log,111cases/896assertions. Full GREEN terminal0 exec60943:startup-prelogin-final-full-tests.log,1594cases/1593passed/1skip,26742assertions. No live SAP test overlapped unit suite. Current pushed desktopdb9aca4 and gateway1040da0 both all required CI green; uncommitted prelogin work not yet pushed/released.
- Claude11 completed terminal0 exec49732, scratch/claude-startup-review-11.txt. No security blocker: existing pre-open scope/name/token/capacity/wiring gates and fresh launch guard preserve isolation. Remaining functional release findings: (1) failed fresh launches leave unclaimed windows; multi-owner filter hides them so repeated requests can open more without claim cap (existing explicit leak now reachable through implicit startup). Need bounded failure handling, avoid closing adopted/unrelated/authenticated windows blindly; consider exact fresh-window cleanup or durable startup failure block. (2) raw-id owned claim whose live generation changed blocks until15min; consider pruning exact stale claim in listing then fresh launch, with fresh generation verification. Existing test intentionally refuses stale generation, must RED new recovery before change. Additional coverage requested: normal fresh000/S000->guardedlogin (current fresh mock SSO), wrong-system/foreign-user/cancel guard denial, explicit cached000 launch remains vetoed. Review lows: ambiguous duplicate claims and executor-serialization assertion. No live multi-owner test yet. Single-owner approved live acceptance remains green4282ms, exactlyone same-id session.

## 2026-10-31 13:54 checkpoint: stale claims and durable launch retry protection
- Previous goal turn was progress: live single-owner prelogin acceptance plus new red/green cases. Current permissions unrestricted/never approval; do not pass sandbox_permissions.
- Stale claim RED startup-stale-claim-red-tests.log:6cases/1failed,4failedassertions. Multi-owner listing now prunes only this token's exact claim when fresh facts prove same saved connection with a different nonempty session_identity. Unknown facts still fail closed. GREEN stale case included in subsequent runs.
- Failed-launch initial fixture lacked target_resolver and stopped before launch; corrected fixture. Authoritative RED startup-launch-retry-red2-tests.log:1case/2failedassertions,2launches instead of1 and no durable marker. GREEN startup-launch-retry-green-tests.log:113cases/914assertions,exec14370terminal0.
- Added LoginAttempts::begin_launch with separate durable marker (root/startup-launch-attempts/startup-login-attempts/hash). Credential begin remains independent. pending detects either; clear removes both after verified readiness/manual authorized screen read. MCP records only after lifecycle admission/policy/process readiness and only fresh implicit launch, never adoption. CLI same before handle_launch. Failure/cancellation leaves marker, next fresh launch returns SAP_STARTUP_REQUIRES_ATTENTION with local manual-login instructions. Documentation and repo skill updated. No automatic SAP window cleanup.
- Real HTTP same-server/token recovery GREEN startup-live-http-launch-marker-recovery.json: initial656ms, verified Easy Access0 closed, recovery3016ms. Independent Python subprocess postcheck startup-live-launch-marker-postcheck.json confirms exactly1 /app/con[0]/ses[0] A4H/001/DEVELOPER SESSION_MANAGER, no launch/login hash markers remain. Server/temp token cleanup normal. No business data changed. Current test cache0 startup-live-http-batch-state.
- Full suite now running scratch/startup-launch-retry-full-tests.log exec26012. Claude12 read-only review running exec90168, prompt/output scratch/claude-startup-review-12*. No live startup overlaps full suite. Do not restart either on observation timeout. Work uncommitted. git diff --check clean apart from LF/CRLF advisory.
- External state advanced independently: installed fairyfly process8996 from package2026.10.3100.0 (started13:13:30). Latest desktop release v2026.10.31 published10:29:50Z, targetmain. Our feature still not deployed. Preserve current tray/pairing; inspect latest release/base before final integration/update. gh release view gateway returned release not found; inspect tags/GitOps release pin later rather than relying on old summary.
- Remaining release gates unchanged: latest review findings, extra multi-owner fresh-login/guard-denial coverage and live acceptance, signed-link local/admin policy checks, docs/personal installed skill/catalogsync, final greenPRs merge/release/deploy and installed production live verification. Do not mark full goal complete.
- Claude12 complete (exec90168 terminal0): stale-claim fix accepted; found cross-owner marker clearing. RED startup-owner-marker-red-tests.log:1case/1failedassertion; other owner's successful read erased caller's marker. MCP now scopes both login and launch attempt keys by authenticated principal for multi-owner HTTP (connection + newline + principal:id); single-owner and local modes retain shared default key. Both readiness and manual-read clearing use same key. Added late cancellation check before marker creation. GREEN startup-owner-marker-green-tests.log:113cases/917assertions,exec24612terminal0. Full before this scoped-key change:startup-launch-retry-full-tests.log,1596cases/1595passed/1skip,26760assertions,exec26012terminal0.
- Claude12 low followups still to cover: CLI manual marker clearing needs zero-wait preparation lock and post-success timing; clear() should attempt both marker removals even if one errors; null-store launch fail-closed consistency (production always wires store). No new release yet. Further multi-owner live and normal fresh credential-login case remain required.
- Gateway feature merged latest origin/main f58faff cleanly and pushed37006ede6d35d2c1acf7c85fbb952ea3c765454a, CI pending. Latest gateway main has releasedv2026.10.31.4. Desktop SSH fetch failed terminally; HTTPS gh credential helper succeeded, fetchedmain3fca4de (release2026.10.31 + Windows10 minimum change). Integrate that base after committing current prelogin work. Do not use stale pre-fetch gateway pin or desktop package version for final release.
