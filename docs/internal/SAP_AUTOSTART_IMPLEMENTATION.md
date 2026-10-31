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
