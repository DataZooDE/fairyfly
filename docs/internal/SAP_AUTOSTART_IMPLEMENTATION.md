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

`CommandHandler::resolve_and_validate_connection` is the common selection path, but MCP authorizes
targets before invocation. Session preparation must cooperate with dispatcher admission and reauthorize
the resulting identity rather than opening a session invisibly beneath those checks.
`CommandDispatcher::auto_attach_and_retry` currently handles only local stdio/link and existing sessions.
HTTP owner routing needs explicit coverage. `third_party/libffl/src/policy.*` in the desktop is a vendored
copy of gateway policy code, not a git submodule; keep protocol/grant behavior synchronized.

Gateway deployment must use the repository's GitOps promotion workflow (docs/DEPLOY.md).
