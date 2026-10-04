# Plan: make Fairyfly easier for agents to operate

Date: 2026-10-03. Scope: CLI help implementation plus a proposed roadmap for CLI and MCP usability, especially for smaller models. Phase 1 is implemented and validated; phases 2–6 are planned.

## Evidence from the two SM59 traces

Sources: [successful inventory](traces/sm59_destinations_openinference.json) and [unsuccessful attempt](traces/sm59_claude_haiku_failed_attempt.json). Both are retrospective summaries, not complete instrumented execution logs. Their explanations should be treated as hypotheses unless supported by recorded commands/results.

| Observed behavior | Product problem | Proposed response |
|---|---|---|
| Successful attempt tried `--connection 0`, which had no saved connection file | SAP enumeration indices look like Fairyfly connection IDs | Return explicit target identity and ready-to-use connection arguments |
| Successful attempt needed several help calls to discover tree actions | Root help exposes groups without teaching the complete workflow | Generate comprehensive help from the registered commands |
| Both attempts initially saw only seven SM59 categories | A collapsed tree can look like a complete result | Expose tree expansion/completeness metadata and bounded traversal |
| Failed attempt used `--key` and grid `--doubleclick` on a tree | Actions and their argument constraints are hard to distinguish | Typed control capabilities and corrective errors with exact valid syntax |
| Failed attempt parsed a large screen dump and guessed property names | Results are too broad and schemas are hard to discover | Focused reads, explicit subtype metadata, schema/examples generated from shared definitions |
| Failed attempt tried a transaction after launching a session | Authentication readiness is unclear | Explicit session state and a suggested next action |
| Failed attempt ended with a stale session | Recovery depends on remembering a multi-step process | State-aware recovery guidance; do not silently retarget another session |
| Successful attempt discarded six expansion responses in a PowerShell loop | Multi-step inspection requires shell orchestration and can hide partial failures | Sequential bounded tree traversal with a result for every expansion |

The failed trace does not establish that argument order caused the `--key` error: `--key` itself is unsupported. Nor does it establish whether the disconnect was a timeout, crash, manual closure, or another cause. Bash alone is not proof of failure on Windows; the recorded error establishes that the executable was not found on PATH. Reading a prior task's trace should not be a prerequisite for using the CLI.

## Phase 1 — comprehensive generated help (implemented)

`fairyfly --help` should teach the operating workflow and show every public command, including nested MCP commands, in one response. `<group> --help` should show that group's descendants; a leaf command's help should remain focused.

Implementation:

- Walk the live CLI11 command tree in `src/commands/cli_app.cpp`. Generate syntax, positional arguments, options, validators and declared defaults from their existing registrations.
- Attach usage notes and examples as footers in the owning command's `setup_cli` or MCP setup helper. Avoid a second command manual or hand-maintained command list in the help renderer.
- Explain session paths versus saved connection IDs, launch versus login, tree versus grid actions, exact node-key whitespace, collapsed trees, result checking, targeted reads, and batch execution.
- Preserve example whitespace verbatim. CLI11's default footer wrapping collapses spaces inside quoted SAP tree keys; override that formatting.
- Keep internal diagnostic options hidden. New public subcommands and options appear automatically.
- Add coverage that walks all public commands, checks help coverage and example presence, verifies nested/scoped help, and parses command examples without executing them. A synthetic new command proves the renderer does not need a manual entry.

Acceptance: root help contains all public descendants and their local documentation; every executable leaf has an example; examples parse; quoted node keys survive unchanged; help runs without SAP interaction. Build and targeted help/CLI tests must pass.

Validation completed: Release CLI and unit-test targets built successfully. The `[agent_help],[command_table],[batch]` selection passed 32 test cases with 1,301 assertions. Root help renders 52 command/group sections; exact padded tree keys are preserved. No live SAP action was needed for these checks. Run `.\build\Release\fairyfly.exe --help` to read the generated guide.

Known limit: option metadata can be generated automatically, but workflow explanations still require maintenance alongside the command code. Parsing examples catches syntax drift, not every semantic behavior change. Full help is deliberately comprehensive; focused help remains useful when context is limited.

## Phase 2 — targeting and recovery (highest follow-up priority)

Owner code: connection manager, session list/attach/login handlers, shared result types, MCP discovery and authorization.

1. Add an explicit saved `connection_id` where a live session has a matching saved record; report null when unattached. Keep `session_id`, SAP enumeration index, system and client separately named. Never equate the array index with a saved ID.
2. Include `state` such as `ready`, `login_required`, `popup_blocked`, `busy`, `disconnected` or `unknown`, using observed evidence. Do not infer readiness from a window title alone. Include the reason when state cannot be established.
3. Add structured recovery information to errors: `retryable`, `reason`, and `suggested_actions` containing an argv array and, where applicable, MCP tool name/arguments. Missing saved IDs suggest discovery/attach; login-required errors suggest login; ambiguous sessions require explicit selection.
4. Preserve current targeting guarantees. Never recover a stale ID by selecting the first live session. Filter all identities and suggestions under existing MCP scope/connection/owner restrictions and lease rules.

Acceptance: fresh unattached, saved live, stale saved, login screen, popup and multiple-session fixtures lead to the correct next step. No test may route to another SAP session silently. Existing authorization and session-lease checks remain in force.

## Phase 3 — bounded tree inspection (largest SM59 call reduction)

Owner code: element read/click handlers, SAP COM tree wrapper, CLI command registration, MCP tool definitions, result renderers.

Extend the existing tree read operation before adding another general-purpose tool family:

```text
fairyfly element get TREE --list-nodes --expand-depth 1 --max-nodes 500 --connection C
```

This is proposed syntax, not currently supported. `C` and `TREE` come from discovery. The matching MCP read operation should accept the same bounded traversal options.

- Return parent relationships, subtype, expanded state and whether children are loaded when the SAP control exposes those facts. Use `unknown` when it cannot establish them.
- Expand sequentially in the target session, track visited nodes, and bound depth, node count, number of expansions and elapsed time. Never double-click or execute a node as part of traversal.
- Return the outcome of each expansion, partial failures, `complete_for_requested_depth`, truncation reasons and a continuation strategy. Do not call a partial tree a complete system inventory.
- Report which display state changed. Any restore option must be explicit and best-effort; restoration failure must be visible.
- Recheck identity and apply existing authorization/lease policy because expansion changes shared GUI state, even though it does not edit SAP destination configuration.

Acceptance: an SM59 fixture with seven collapsed categories returns all expected destinations at the requested depth without a shell loop. Test lazy-loaded children, empty categories, duplicate labels, padded keys, failed expansions, bounds, busy controls and session replacement. A later live check must compare against the current system, not assume the historical count of 57 remains valid.

## Phase 4 — clearer actions and smaller results

Owner code: screen discovery, element metadata, action validation, CLI/MCP error formatting.

- Return a small control summary with `type`, `subtype` and supported actions. A tree should advertise node-list and node-expand operations; a grid should advertise row/column operations.
- Validate action/subtype combinations before COM mutation. A tree supplied with grid `--doubleclick` should receive a precise tree-action example, with the missing node-key requirement. Unsupported `--key` can suggest `--node-key`; suggestions must never execute an action automatically.
- Add an optional result projection or focused summary mode shared by CLI and MCP. Preserve target identity, status, partial-result indicators and continuation information even in compact output.
- Keep one canonical copy of each control in compact results. Include small examples of result shapes beside command documentation so agents do not guess `rows` on a tree.
- Report unknown or ambiguous controls rather than constructing element IDs from labels.

Acceptance: mistaken flags and subtype combinations cause no SAP action and give one actionable correction. Token/byte counts for focused SM59 reads are materially lower than full-screen reads, while all required IDs and completeness indicators remain available.

## Phase 5 — share documentation and schemas across CLI and MCP

Phase 1 uses existing CLI11 metadata. A subsequent metadata refactor should also address drift between CLI and MCP:

- Introduce command-owned structured examples, argument descriptions, constraints, result shapes and action effects. Generate CLI help and MCP descriptions/examples from this metadata where their contracts match.
- Add a machine-readable command description export using that same source. Decide the public command/flag name during implementation; avoid a second independently maintained schema.
- Represent conditional requirements such as tree node keys versus grid row/column arguments explicitly. Do not pretend that CLI positional arguments and MCP objects are identical transports.
- Offer smaller task/family documentation views while retaining complete root help. Measure whether smaller models perform better with full help, a task view, or direct MCP schemas.
- For safe repeated workflows, consider recipes that bind previous results explicitly. Current batch is sequential argv execution and does not interpolate earlier responses. Do not add hidden retries or implicit business actions to it.

Acceptance: changing a shared argument updates its CLI and MCP documentation in one place. Contract checks cover CLI-only and MCP-only commands explicitly. New workflow helpers retain per-action results and existing audit/authorization behavior.

## Phase 6 — measured agent evaluations and real tracing

Use the two traces as regression scenarios, not as quantitative proof of model quality. Capture a reproducible baseline before claiming efficiency gains.

Evaluation matrix:

| Dimension | Cases |
|---|---|
| Session state | Ready, unattached, login required, popup, stale target, multiple targets |
| Control type | Collapsed tree, loaded tree, grid, ordinary field |
| Documentation | Old baseline, comprehensive help, focused help, MCP schemas |
| Agent | At least two smaller models and one stronger reference model, versions recorded |
| Failure injection | Missing executable, wrong flag, padded key, truncated result, expansion failure |

Record task completion, destination precision/recall, tool calls, failed calls, input/output tokens, wall time and cost per successful task. Use repeated runs with fixed fixtures and report variance; avoid comparing one successful attempt with one failed attempt as a benchmark. Separate offline fixtures from explicitly scoped live SAP checks.

Proposed release gates: all deterministic targeting/tree cases pass; no unauthorized action or incorrect retargeting; smaller-model completion improves over baseline; successful SM59 inventory uses at most six calls after acquiring a ready target when bounded traversal is available. Treat token/cost reduction targets as hypotheses until the baseline is measured.

Add optional runtime OpenTelemetry spans carrying OpenInference attributes: one agent/task parent, CLI/MCP operation spans, and child COM operations where instrumentation is feasible. Include valid nonzero IDs, actual start/end timestamps, build version, operation status, counts and durations. Separate observations from explanatory summaries. Keep credentials and ordinary screen contents out by default; opt-in content capture must use existing redaction. The current trace files lack the timing and complete call boundaries needed for a faithful runtime trace, and should not be presented as directly importable OTLP exports.

Acceptance: a trace viewer can import an actual exported trace, correlate calls across CLI/MCP/COM boundaries, and identify partial failures without exposing credentials. Tracing disabled adds no remote dependency and has negligible overhead.

## Delivery order

1. Comprehensive generated help is complete; use it as the baseline for subsequent evaluations.
2. Implement targeting/state/error guidance; this prevents acting on the wrong session.
3. Implement bounded tree reads to remove the repeated SM59 exploration loop.
4. Improve subtype-aware actions and compact results.
5. Unify CLI/MCP metadata and add task views where evaluations justify them.
6. Add runtime tracing and run repeated model evaluations; feed measured failures back into the earlier phases.

The existing [parallel sessions plan](MCP_PARALLEL_SESSIONS_PLAN.md) is a separate effort. These changes should integrate with its ownership/lease constraints rather than introduce another session lifecycle or concurrency mechanism.
