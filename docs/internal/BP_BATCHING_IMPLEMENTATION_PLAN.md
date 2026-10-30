# Fairyfly SAP workflow latency: implementation plan

## Goal and evidence

Reduce the agent time and response size for ordinary SAP workflows while keeping Fairyfly's existing policy, session ownership, screen guard, lease, audit, and error behavior. The initial BP exercise on Bigfox A4H/001 took 55.0 s through nine agent-selected calls, 8.26 s when those nine calls were issued from one agent call, and 6.17 s in one CLI batch. The batch returned 71,452 characters. These are single-run, directional numbers, not latency targets; the measurements and their limits are in [BP_TIMING_2026-10-29.md](BP_TIMING_2026-10-29.md).

The MCP `gui_batch` implementation in `src/mcp/dispatcher.cpp` already pins one owner-mode session, runs each item through `run_single`, and audits each item. It currently concatenates every item's text and cannot pass a result to a later item. `gui_element_fill` already handles multiple fields in one operation. Extend those primitives before adding another public tool.

## Definition of done for every slice

1. **Red:** add a focused failing unit or protocol test for the intended contract. Include a failure case as well as success. Confirm the new test fails for the expected reason before changing production code.
2. **Green:** implement the smallest change that passes that test and the relevant existing tests. Keep defaults compatible for existing MCP clients.
3. **Real SAP:** use a logged-in Bigfox A4H/001 SAP GUI session through Fairyfly's **MCP transport**, run the slice's named BP scenario, and record the MCP request, response, Fairyfly phase timings, response bytes, and actual SAP result. A mock, CLI-only run, or green unit suite does not complete a slice.
4. Check that the live run leaves the session and test data in the expected state. Record a reproducible command and sanitized measurements in `docs/internal/`; never log credentials, bearer tokens, BP email values, or full screen payloads.

The live harness should acquire its own short-lived token and owner-mode lease, use the existing logged-in session, and clean up both. It must identify the exact system/client/user and transaction before writing. Keep nonpersistent scenarios on a Create Organization form and cancel/discard the changes. For persisted create and email-change acceptance, use a designated disposable BP identity and email domain, verify the saved value in a fresh BP read, then restore/delete the test records according to the test-system procedure. If that fixture is unavailable, the persisted scenario remains open and the slice is not done.

## Slice 0 — repeatable live baseline and timing

**Red:** add a protocol-level test for a measurement runner that requires one result per step, marks errors/skips, and rejects a run missing SAP identity or final screen verification. The fixture for this test can simulate transport, but its passing result is only the harness contract.

**Green:** add a Bigfox MCP runner, likely `tests/integration/mcp_live_bp_latency.ps1`, with a safe mode: open BP, read the editable form, open Create Organization, locate the E-mail field, fill a `.invalid` address, cancel, handle the discard confirmation, and verify BP is back with no saved change. Measure client request/response wall time and byte count for each call, plus total workflow time. Run the same sequence as individual MCP calls and as current `gui_batch`; use identical read options and three or more paired repetitions. Save medians and ranges, and retain failures in the report rather than silently excluding them.

**Real SAP gate:** the runner completes both modes against Bigfox and SAP confirms the unsaved changes were discarded. This becomes the repeatable control for later slices. Add a separate agent-driven pass that records the number of agent decisions/tool calls and available model token/latency usage; do not infer model time by subtracting CLI times.

## Slice 1 — small, structured batch results

**Red:** in `tests/unit/test_mcp_dispatcher.cpp` and `test_mcp_tools.cpp`, specify an opt-in `gui_batch` result mode such as `result_mode: "summary"`. Assert ordered per-item status, error code, skip state, and a small selected result; assert that full screen text and images are absent unless requested. Existing calls without this option must retain the current response. Add redaction, cancellation, and owner-mode audit tests.

**Green:** add the schema in `src/mcp/tool_catalog.cpp` and shape the batch result in `src/mcp/dispatcher.cpp`. Return a stable `structuredContent.items` array and concise text. Allow an item to request its full result explicitly (for diagnostics) without forcing all other items to do so. Bound selected values and response sizes; preserve error detail needed to recover. Keep each underlying call's audit and policy path unchanged.

**Real SAP gate:** run the unsaved BP scenario in both legacy and summary modes through MCP. Confirm identical screen transitions, audit item count, and failure behavior; record response bytes and elapsed time. The payload should shrink materially; investigate if it does not.

## Slice 2 — bind a field found earlier in the same batch

**Red:** test a batch that finds exactly one field, binds its element ID to a short name, and uses that name in a later `gui_element_fill`. Test zero and multiple matches, stale screen, cross-session binding, invalid reference, and attempts to inject a tool name or unapproved write through a reference. All must fail closed before the dependent write. Cover owner-mode lease and per-item authorization.

**Green:** extend `gui_batch` items with a small, typed binding contract. Bind only explicitly allowed values from a prior result (initially an element ID and its screen guard); references exist only for that batch and that SAP session. Resolve and validate arguments immediately before each item, then use the existing `run_single` path. Do not add general expressions or allow a reference to choose a tool. Keep the complete list of possible tools visible to the upfront authorization check. Use the existing screen guard for owner-mode calls, and an equivalent live screen identity check for any supported local mode.

**Real SAP gate:** from the real Create Organization screen, find the E-mail control by a stable screen attribute, bind its ID, fill a `.invalid` value, read it back, cancel, and verify discard. Repeat after changing the screen between find and fill; the dependent fill must be refused with no edit on the new screen.

## Slice 3 — guarded, bounded workflows

**Red:** tests for explicit assertions on transaction/screen/status and a conditional step for the known discard popup. Assert that an unexpected screen stops before the next mutation, a missing popup takes the declared path, cancellation reports what ran, and all declared branches obey the same scopes, T-code restrictions, read-only rules, leases, and audits. Include a test where a predeclared Save is refused by policy.

**Green:** add only finite guards and conditions to `gui_batch`; no loop, arbitrary script, dynamically generated tool, or automatic Save. Prevalidate every declared item and possible branch. Evaluate a condition from a fresh, bounded SAP result and report whether an item ran or was skipped. Preserve the current 20-item limit and timeouts unless live measurements show a specific need to change them.

**Real SAP gate:** one MCP request performs the unsaved Create Organization workflow, handles the actual discard popup, and verifies return to BP. Repeat with an unexpected screen to prove it stops. Then, with a disposable test BP, perform an email change and Save through a separately declared, guarded flow; reopen that BP and verify the persisted address, then restore it. Perform a disposable BP creation and read back the created BP number/value, followed by cleanup under the test-system procedure.

## Slice 4 — compact screen references and targeted reads

**Red:** tests for an opt-in screen projection that retains blank editable fields and usable short references while omitting repeated absolute IDs/hierarchy. Test duplicate labels, inactive tabs, inaccessible references, stale screen use, and the explicit diagnostic path to full IDs. Ensure the model-visible form cannot silently hide a field that is needed for the requested write.

**Green:** return short screen-scoped field references backed by exact IDs inside Fairyfly. Support those references in relevant element tools, with session and screen checks. Prefer `only=editable`, `no_tabs=true`, `gui_screen_find`, and the existing multi-field fill in tool guidance and examples; do not change the legacy read default. Profile inactive-tab expansion separately before optimizing it.

**Real SAP gate:** read the actual Create Organization form, identify and fill an empty field from the compact output, verify the value, then discard. Compare payload bytes and agent input tokens with the full editable read. Repeat on a BP email-change screen, including a stale-reference refusal after navigation.

## Slice 5 — phase telemetry and final comparison

Instrumentation may start in Slice 0 where necessary; finish it here. Add correlation IDs and durations for HTTP admission/queue, worker/SAP action and wait, screen collection, formatting, serialization, and total MCP request. Expose safe per-item durations and response byte counts in structured results, without field values. Keep model latency, token usage, and reasoning time in the agent-side runner: Fairyfly cannot measure those internally.

**Red/green:** test monotonic, nonnegative timings, correct item-to-request correlation, size accounting, cancellation/error reporting, and absence of secrets in telemetry. Verify existing output modes remain compatible.

**Real SAP gate:** run repeated paired BP creation and email-change workflows through (a) the current stepwise agent, (b) the new guarded batch with compact results, and (c) a scripted MCP control. Record total wall time, agent calls, model tokens and latency where available, Fairyfly phases, SAP waits, and request/response bytes. Report median and spread, including failed runs. Success means the same SAP outcomes and cleanup, no policy/audit regression, fewer agent round trips, smaller model-visible responses, and an explained latency change; do not require a fixed wall-time threshold because SAP response time varies.

## Suggested implementation order and touch points

Build Slice 0 first, then 1 → 2 → 3 → 4 → 5. The useful early release is Slices 0–2: it reduces response size and lets one request read a field and fill it safely. The full BP routine needs Slice 3. Keep the CLI `batch` syntax unchanged initially; the measured bottleneck is agent/MCP interaction, and a CLI extension can follow only if a live use case needs it.

Primary files: `src/mcp/dispatcher.cpp`, `src/mcp/tool_catalog.cpp`, MCP result shaping/HTTP timing code, `tests/unit/test_mcp_dispatcher.cpp`, policy/auth tests in `tests/unit/test_auth_policy.cpp` and `test_auth_scopes.cpp`, `tests/integration/README.md`, `docs/MCP.md`, and the new live runner. The live runner's exact commands and fixture identity should be documented before its first persisted write.
