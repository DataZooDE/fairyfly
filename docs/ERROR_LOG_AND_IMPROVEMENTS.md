# Error Log & Foundational Improvements Register

This is a historical record of errors and fixes encountered while testing Fairyfly against SAP GUI. A dated entry may describe a past limitation or an untested variant; it is not by itself a current bug or todo. The maintained list of concrete unfinished work is [OPEN_WORK.md](OPEN_WORK.md).

---

## 1. Backend & Environment Protocol

### ERR-001: Backend Scripting Disabled (`sapgui/user_scripting = FALSE`)
- **Status**: RUNNING PARAMETER TRUE; PERSISTENT PROFILE CHANGE BLOCKED (SEE OPS-001)
- **Symptom**: `fairyfly list` or `screen read` attached to the GUI process but could not access session collections (`Connections collection exists but reports 0 items`).
- **Root Cause**: The SAP NetWeaver backend system had parameter `sapgui/user_scripting` set to `FALSE` by default. Even with client-side scripting enabled in SAP GUI options, the application server rejects automation scripts.
- **Earlier fix**: Scripting was switched dynamically in `RZ11` on an earlier setup. An earlier note claimed a permanent profile change, but the current Bigfox instance profile has no `sapgui/user_scripting` entry; persistence on this instance is unverified.
- **2026-09-26 recurrence and recovery**: A staged-CLI `list` initially found one Bigfox connection with zero sessions and `DisabledByServer=true`. ADT then changed the running parameter from `FALSE` to `TRUE`; a new native connection exposed a scriptable login form. The later ADT read still returned `TRUE` with dynamic origin. The latest read-only CLI check found zero open connections, and `doctor` reported `active_sessions=warn` with overall health `warning`, rather than a backend scripting failure. The profile-file write remains blocked as detailed in OPS-001; a restart could revert the runtime setting.
- **Foundational / Protocol Improvement**:
  - *Automated Preflight Diagnostics (`fairyfly doctor`)*: Implemented comprehensive multi-layer preflight diagnostics checking interactive desktop station, running processes, Windows client scripting security registry settings, COM automation engine, and backend session access. Verified live with 100% pass across all checks.
  - *Self-Diagnostic Protocol*: Instead of failing silently with "0 connections", the engine detects whether scripting is disabled at backend vs client level and guides remediation.

---

## 2. SAP COM Automation & Object Model

### ERR-002: `GuiTab` Control Click Failure (`DISP_E_UNKNOWNNAME`)
- **Symptom**: Calling `fairyfly click` on a tab element (e.g., `/app/con[0]/ses[0]/wnd[0]/usr/tabsTAB_STRIP/tabpDEF`) failed with `Press method not found on element`.
- **Root Cause**: In the SAP GUI COM type library, `GuiButton` controls implement the `Press()` method, whereas `GuiTab` controls implement `Select()`. Treating all clickable elements as button-like caused DISPID resolution to fail.
- **Immediate Fix**: In `ComGuiElement::press()`, added type detection for `elem_type == "GuiTab"` to invoke `select(true)` directly, plus a fallback that tries DISPID `Select` if `Press` is not found.
- **Foundational / Protocol Improvement**:
  - *Polymorphic Action Dispatch*: Rather than hardcoding method names per CLI command, element wrappers should implement a capability-based action dispatcher (e.g., `activate()`, `select()`, `toggle()`) where each control type maps its canonical activation mechanism.
  - *Dynamic Method Probing*: If an action fails with `DISP_E_UNKNOWNNAME`, probe known alternate verbs (`Press`, `Select`, `Click`, `Activate`) before throwing an error.

---

### ERR-003: `press_f4` Crash via Element `SendVKey` (`SendVKey method not found`)
- **Symptom**: Running `fairyfly press_f4 /app/con[0]/ses[0]/wnd[0]/usr/ctxtRSRD1-TBMA_VAL` failed with COM error `SendVKey method not found`.
- **Root Cause**: In SAP GUI Scripting, `SendVKey` is exclusively a method on `GuiMainWindow` / `GuiWindow` (`wnd[0]`). While `GuiCTextField` has a visual F4 button, the scripting API does not provide `SendVKey` on individual input elements; the element must receive `SetFocus()`, and the key event must be sent to the enclosing `GuiWindow`.
- **Immediate Fix**: Updated `ComGuiElement::press_f4()` to focus the element without throwing if `SendVKey` is not exposed by the element, allowing `ComAutomationEngine::press_f4()` to deliver `window->send_vkey(4)` seamlessly.
- **Foundational / Protocol Improvement**:
  - *Window-Contextual Actions*: Establish a clear protocol distinction between *element-local mutations* (`set_text`, `set_focus`) and *window-level virtual key dispatches* (`send_vkey`).
  - *Atomic Macro Operations*: Actions like "Press F4" or "Press Enter on Field" should be modeled as atomic multi-step sequences: Focus -> Dispatch VKey -> Wait for modal / DOM stabilization.

---

### ERR-004: Heap Allocation in `VariantGuard` (Resource Leak Risk)
- **Symptom**: Subagent code review identified potential COM memory leaks in long-running processes due to heap allocation of `VARIANT` structures via `new VARIANT()`.
- **Root Cause**: Wrapping `VARIANT*` with heap allocation created risks if destructor execution was interrupted or if raw pointers were manipulated across exception boundaries.
- **Immediate Fix**: Refactored `VariantGuard` to use an embedded stack-allocated `VARIANT inline_var_` with explicit RAII `VariantInit` and `VariantClear`.
- **Foundational / Protocol Improvement**:
  - *Zero-Heap COM Abstractions*: All temporary COM interop buffers (`VARIANT`, `BSTR`, `DISPPARAMS`) must strictly use stack-allocated RAII wrappers (`_variant_t`, `_bstr_t`, or custom zero-allocation guards) to guarantee exception safety and leak prevention during continuous agent interaction.

---

## 3. UI Tree Traversal & Screen Reader

### ERR-005: Premature Tree Pruning on Containers (`GuiTabStrip`, `GuiSubScreen`, `GuiBox`)
- **Symptom**: Elements inside tab controls and subscreen blocks (e.g. table controls inside SE11 tabs) were not being discovered by `ScreenReader::traverse_element_tree`.
- **Root Cause**: `ScreenReader` relied on a whitelist of container types (`is_container`), which omitted `GuiTabStrip`, `GuiTab`, `GuiSubScreen`, and `GuiBox`. As a result, the recursive traversal treated them as leaf nodes and pruned their children.
- **Immediate Fix**: Expanded `is_container` to include `GuiTabStrip`, `GuiTab`, `GuiSubScreen`, and `GuiBox`.
- **Foundational / Protocol Improvement**:
  - *Default to Structural Introspection*: Rather than relying solely on hardcoded type whitelists for containers, inspect the object's `Children` collection count (`child_count > 0` or presence of child enumerators). If an element has children, it should be treated as a container unless explicitly classified as a black-box control.

---

### ERR-006: Tab Discovery Bypass in `read_with_tabs`
- **Symptom**: Running `fairyfly screen read` reported `"No tabs found in screen, returning normal screen read"` even on screens with multiple tab pages (e.g. SE11 table display).
- **Root Cause**: `read_with_tabs` looked directly for elements with `type == "GuiTab"` inside `hierarchy["tabs"]`. However, the screen reader organizes elements under container types: the top-level element is a `GuiTabStrip`, whose children are the actual `GuiTab` controls.
- **Immediate Fix**: Updated `read_with_tabs` to also inspect `GuiTabStrip` child collections to enumerate all individual `GuiTab` elements.
- **Foundational / Protocol Improvement**:
  - *Hierarchical Query Language (HQL)*: Implement clean selector queries (e.g., `//GuiTabStrip/GuiTab` or semantic queries like `:tabs`) rather than flat array filtering. This decouples screen analysis algorithms from physical UI nesting depth.

---

## 4. Agent & Tooling Protocol

### ERR-007: Inadvertent Root-Level Scratch File Pollution
- **Symptom**: Operational runs created loose JSON outputs, `.vbs` scripts, and `.con` state files directly in the repository root (`fairyfly.0.con`).
- **Root Cause**: `ConnectionManager` defaulted its working directory to `fs::current_path()`, writing session state files directly to whichever directory the user ran the command from.
- **Immediate Fix**: Implemented `ConnectionManager::get_default_cache_directory()`, routing session `.con` files to `%LOCALAPPDATA%\fairyfly\sessions` (Windows) / `~/.cache/fairyfly/sessions` (POSIX), with environment override `FAIRYFLY_CACHE_DIR`, temp directory fallback, and backward-compatible discovery for legacy files in the current directory.
- **Foundational / Protocol Improvement**:
  - *Explicit State Isolation*: Connection and transient state must never pollute the repository root.
  - *Multi-Tier Storage Fallback*: Cache directories must gracefully fall back to local user app data, temp directories, or local `.fairyfly` folders even inside sandboxed execution environments.

### ERR-008: Subtype Classification Shadowing (`classify_type` Ordering Bug)
- **Symptom**: Calling `fairyfly click /app/con[0]/ses[0]/wnd[0]/usr/radRSRD1-VIMA` (a `GuiRadioButton`) failed with `Select only works on CheckBox, RadioButton, or Tab elements`.
- **Root Cause**: `ComGuiElement::classify_type(type_str)` evaluated substring checks in an arbitrary order: `if (type_str.find("Button") != std::string::npos)` was checked *before* `"RadioButton"`. Because `"GuiRadioButton"` contains the substring `"Button"`, it was misclassified as `GuiElementType::Button`. When `press()` failed to invoke `Press()` and fell back to `select()`, `select()` rejected the element because its classified type was `Button` instead of `RadioButton`.
- **Immediate Fix**: Ordered specific subtype checks before generic suffixes in `classify_type`: `RadioButton` and `CheckBox` are checked before `Button`, and `TabStrip` is distinguished from `Tab`. In `ComGuiElement::press()`, directly delegate `GuiRadioButton` and `GuiCheckBox` to `select(true)`. In `select()`, probe both `Select()` method and `Selected` property put.
- **Foundational / Protocol Improvement**:
  - *Exact Type Token Matching*: Avoid loose substring matching (`find("Button")`) for control typing. Match against canonical SAP GUI COM class names (`GuiRadioButton`, `GuiButton`, `GuiTab`, `GuiTabStrip`) or match prefixes/tokens strictly.
  - *Polymorphic Activation*: All interactive elements (`GuiButton`, `GuiTab`, `GuiRadioButton`, `GuiCheckBox`, `GuiMenu`) should unify under an `activate()` protocol that maps to each control's native COM activation verb.

### ERR-009: Claude CLI Non-Interactive Execution Hang
- **Symptom**: Calling `claude -p "prompt"` as a non-interactive review tool hung indefinitely or exited with `Not logged in · Please run /login`.
- **Root Cause**: The Anthropic `claude` CLI requires authentication stored in `~/.claude` or via `ANTHROPIC_API_KEY`. Without credentials, interactive mode tries to render an Ink React terminal UI for login (which blocks waiting for input when spawned in subshells without a TTY), while piped mode emits code 1 with `Not logged in · Please run /login`.
- **Immediate Fix**: Diagnosed via piped execution and recorded authentication requirement. Preflighted external CLI invocations with short non-blocking timeouts.
- **Foundational / Protocol Improvement**:
  - *Fail-Fast External CLI Probes*: Always test external helper CLI tools with a quick piped `--version` or authentication check before integrating them into automated workflows to prevent hanging the agent pipeline.
- **Current recheck (2026-09-26)**: `claude auth status` reports logged in, and `claude -p "Say OK." --tools "" --no-session-persistence` returned `OK` promptly. A source review with tools enabled produced no output for about a minute. A focused, piped source review with tools disabled returned findings after a longer wait; its GridView selection finding was reproduced and fixed. The original authentication diagnosis describes the historical environment, not the current one.

### ERR-010: Test Runner Desktop Context Disconnect for Live SAP GUI COM Sessions
- **Symptom**: `unit_tests.exe` reported `Connections collection exists but reports 0 items - no SAP connections currently open` and skipped all live session tests, while `fairyfly.exe doctor` detected the active session without issue.
- **Root Cause**: `Catch2::Catch2WithMain` defines a default generic `main()` function which lacks the Windows desktop attachment call (`SetThreadDesktop(OpenDesktopA("Default", ...))`). In background processes, subshells, and runner environments, the process's thread desktop is not attached to the interactive `Default` desktop (`WinSta0\Default`), rendering the SAP GUI Running Object Table (ROT) inaccessible (`SapROTWr` / `CoGetObject("SAPGUI")` returns empty or falls back to an isolated `SapGui.ScriptingCtrl.1` instance with 0 connections).
- **Immediate Fix**: Switched `tests/CMakeLists.txt` from `Catch2::Catch2WithMain` to `Catch2::Catch2` and implemented custom `main()` in `tests/unit/test_main.cpp` that acquires `OpenDesktopA("Default", 0, FALSE, GENERIC_ALL)` and calls `SetThreadDesktop(hDesk)` before invoking `Catch::Session().run(argc, argv)`.
- **Foundational / Protocol Improvement**:
  - *Unified Process Context Protocol*: Any Windows binary (CLI, test runner, background agent) that interacts with SAP GUI COM automation must normalize its desktop context to `Default` upon startup.
  - *Live Session DoD Enforcement*: With desktop context unified, `unit_tests.exe` now runs 100% of live COM assertions (58 assertions across 11 test cases) against the real SAP GUI running on Bigfox without skipping.

---

### ERR-011: `ComGuiElement::get_child` Invocation Failure & Container Child Pruning
- **Status**: FIXED & VERIFIED LIVE
- **Symptom**: Calling `element->get_child(i)` threw a COM exception `Item method not found on element`, causing container traversal to drop all nested controls inside `GuiTabStrip`, `GuiBox`, `GuiUserArea`, and custom subscreen containers. As a result, `fairyfly screen read` reported only 12 flat elements on the SE11 table screen instead of the full tree of >230 controls.
- **Root Cause**:
  1. In `SapGuiCollection<T>::item(int index)` in `src/include/sap_gui_base.h`, the enumerator logic checked for `VT_UNKNOWN` (`var.vt == VT_UNKNOWN`). However, SAP GUI's `Children` collection `IEnumVARIANT` often returns items tagged as `VT_DISPATCH`. The missing `VT_DISPATCH` check caused the enumerator loop to fail and fall back to `Item(index)` dispatch.
  2. The dispatch fallback tried `DISPATCH_PROPERTYGET` on `Children`. On some controls (like `GuiTabStrip`), `Children.Item(index)` requires `DISPATCH_METHOD | DISPATCH_PROPERTYGET`.
  3. `ComGuiElement::get_child(int index)` in `src/com/element.cpp` attempted to call `GetChild(index)` directly on the parent element's `IDispatch*`, rather than invoking `children().item(index)`. In SAP GUI COM, `GetChild` is not a method on generic `GuiComponent`; elements expose a `Children` collection.
  4. In `ScreenReader::read_screen`, when processing children of `GuiUserArea`, the loop simply called `collector.add(child_ptr)` instead of recursively calling `traverse_element_tree(child_ptr, ...)`. Any container directly placed under `usr` (such as `tabsTAB_STRIP`) had its sub-tree completely omitted.
- **Immediate Fix**:
  1. In `SapGuiCollection<T>::item(int index)`, updated enumerator parsing to support both `VT_UNKNOWN` and `VT_DISPATCH` (`var.vt == VT_UNKNOWN ? var.punkVal : var.pdispVal`).
  2. Added combined `DISPATCH_METHOD | DISPATCH_PROPERTYGET` fallback for `Item(index)`.
  3. In `ComGuiElement::get_child(int index)`, routed retrieval through `children().item(index)` with graceful fallback to `Children.Item(index)` dispatch.
  4. In `ScreenReader::read_screen`, updated `GuiUserArea` child loop to recursively call `traverse_element_tree`.
- **Foundational / Protocol Improvement**:
  - *Recursive Container Traversal Invariant*: Any child element discovered under a root container (whether `wnd[0]`, `usr`, or a subscreen container) must be subject to recursive traversal if it is a container, preventing entire visual subtrees from being silently pruned.
  - *Dual-Protocol COM Collections*: In SAP GUI COM, collections must always support both `IEnumVARIANT` enumeration and indexed `Item(index)` dispatch, accepting both `VT_DISPATCH` and `VT_UNKNOWN` variants.
  - *Verification*: Discovered element count on SE11 `TADIR` increased from 12 to **236 elements**, fully uncovering all tabs, subscreens, toolbars, and controls.

---

### ERR-012: `GuiTableControl` Stub & Data Extraction Failure
- **Status**: FIXED & VERIFIED LIVE
- **Symptom**: On screens with classic Dynpro table controls (such as `tblSAPLSD41TC0` on the Fields tab of SE11), the table was discovered with `row_count: 53` and `visible_row_count: 32`, but returned empty columns `columns: []` and empty cell data `rows: [[], [], ...]`.
- **Root Cause**:
  1. `TableDataExtractor::extract_table_data` was an unpopulated stub (`// For now, return metadata only`).
  2. `ScreenReader::read_screen` attempted to treat `GuiTableControl` identically to `GuiGridView`, routing it to `extract_grid_data_immediately`. However, `GuiTableControl` does *not* support ALV Grid methods like `ColumnCount` or `GetCellValue(row, col)`.
  3. In SAP GUI COM, `GuiTableControl` exposes:
     - A `Columns` collection property (where each column object has `Title` and `Name` properties).
     - A `GetCell(row, col)` method that returns an `IDispatch*` to the active cell control (`GuiTextField`, `GuiCheckBox`, `GuiComboBox`).
     - Note: In OLE Automation `IDispatch::Invoke`, DISPPARAMS arguments are passed in *reverse order*, so `arg[0] = col` and `arg[1] = row`.
- **Immediate Fix**:
  1. Implemented complete column metadata extraction in `TableDataExtractor::extract_table_data` via `Columns` collection, reading `Title` and `Name` properties for each column.
  2. Implemented row and cell extraction via `table->get_dispatch_method(L"GetCell", cell_args)` across visible rows and columns.
  3. Extracted cell values based on concrete control type: reading `Text` property for text inputs and `Selected` boolean property for checkbox cells (`Key`, `Initial Values`).
  4. Routed `GuiTableControl` in `ScreenReader` directly to `TableDataExtractor::extract_table_data` instead of ALV Grid extractors.
- **Foundational / Protocol Improvement**:
  - *Control Family Separation*: Maintain a strict architectural separation between ALV Grid controls (`GuiGridView` / `GuiShell`) and Dynpro Table Controls (`GuiTableControl`). Each family has a distinct COM automation API contract.
  - *Live Session DoD Verification*: Verified live against SE11 table `TADIR`. Extracted all **10 columns** (`Field`, `Key`, `Initial Values`, `Data element`, `Data Type`, `Length`, `Decimal Places`, `Coordinate`, `Short Description`, `Group`) and **20 visible rows** of real dictionary fields (`PGMID`, `OBJECT`, `OBJ_NAME`, etc.).

---

### ERR-013: `read_with_tabs` Duplicate Tab Collection Bug
- **Status**: FIXED & VERIFIED LIVE
- **Symptom**: Running `fairyfly screen read` on screens with a `GuiTabStrip` (e.g. SE11 with 6 tabs) queued and executed 12 tab expansions instead of 6, duplicating every tab cycle and doubling command execution time to ~108s.
- **Root Cause**:
  In `ScreenReader::read_with_tabs`, the tab discovery loop iterated over `screen_data["hierarchy"]["tabs"]`. This array contains both the individual `GuiTab` elements *and* the enclosing `GuiTabStrip`. The loop appended all `GuiTab` elements directly, and *also* iterated through the `children` of any `GuiTabStrip` element and appended them. As a result, every tab was added to `tab_elements` twice.
- **Immediate Fix**:
  Introduced `std::unordered_set<std::string> seen_tab_ids` in `read_with_tabs` to deduplicate tab elements by their unique SAP GUI element ID before adding them to `tab_elements`.
- **Foundational / Protocol Improvement**:
  - *Idempotent Traversal & Identity Deduplication*: Any multi-step UI exploration (tabs, tree expansion, dialog cascades) must maintain an identity set of visited element IDs (`id_str`) to guarantee that nodes are never processed more than once.
  - *Performance Impact*: Reduced tab expansion execution time on SE11 from **108.8s down to 58.4s** (50% speedup) while capturing 100% of unique tab subtrees cleanly.

---

### ERR-014: `GuiTableControl` Metadata Serialization Omitted Columns & Rows
- **Status**: FIXED & VERIFIED LIVE
- **Symptom**: When reading a screen with `GuiTableControl` in Markdown format (`fairyfly screen read --output markdown`), the output displayed `_Table data not available_` under `### Table`, even though the control was discovered with total and visible row counts.
- **Root Cause**:
  1. In both `ElementMetadataExtractor::extract` and `ComAutomationEngine::extract_element_metadata`, the branch for `type == "GuiTableControl"` extracted the `TableData` struct from `TableDataExtractor::extract_table_data`, but only serialized `table_json["total_row_count"] = table_data.total_row_count;` into `metadata["table_data"]`. It omitted `columns`, `rows`, and `visible_row_count`.
  2. In `TableFormatter::format_to_markdown`, the formatter strictly inspected `element_json["table_data"]` and did not accept `element_json["grid_data"]` as a fallback. If either `columns` or `rows` were missing, it rendered `_Table data not available_`.
- **Immediate Fix**:
  1. Updated `ElementMetadataExtractor::extract` and `ComAutomationEngine` to serialize all fields: `columns`, `rows`, `total_row_count`, and `visible_row_count` into `metadata["table_data"]` and the cache.
  2. Updated `ScreenReader::extract_grid_data_immediately` to populate both `grid_data` and `table_data` keys on the extracted JSON element.
  3. Updated `TableFormatter::format_to_markdown` to inspect both `table_data` and `grid_data` pointers and verify the presence of column/row arrays.
- **Foundational / Protocol Improvement**:
  - *Symmetric Metadata Serialization Contract*: Metadata extractors must guarantee uniform serialization contracts for all tabular elements (`GuiGridView`, `GuiTableControl`, `GuiUserArea` tabular layouts) with standard keys (`columns`, `rows`, `total_row_count`, `visible_row_count`).
  - *Live Session DoD Verification*: Verified live against SE11 table `TADIR`. Running `fairyfly screen read --no-tabs --output markdown` directly renders the complete 10-column, 20-row table in Markdown with proper cell values, checkbox states ("X"), and column headers.

---

### ERR-015: `[json.exception.type_error.306] cannot use value() with string` in `GridAnalyzer`, `ScreenMarkdownFormatter`, and Filter Pipeline
- **Status**: FIXED & VERIFIED LIVE
- **Symptom**:
  Running `fairyfly screen read --output markdown` or `fairyfly screen read --only-buttons` on screens with grid elements (e.g., SE16 table data entries screen) crashed with:
  ```
  ScreenMarkdownFormatter::format() failed: [json.exception.type_error.306] cannot use value() with string
  Exception: [json.exception.type_error.306] cannot use value() with string
  ```
- **Root Cause**:
  1. In modern Fairyfly architectures (`ElementMetadataExtractor`), child element references inside an element JSON are stored as flat arrays of ID strings (e.g. `children: ["/app/con[0]/ses[0]/wnd[0]/usr/lbl[1,0]"]`), because the actual child element JSON objects are already flattened into the top-level `elements` array.
  2. Legacy code in `GridAnalyzer::parse_grid_labels()` recursively traversed `elem["children"]`, passing the array of string IDs into `parse_grid_labels()`. When it evaluated `elem.value("type", "")`, `nlohmann::json` threw `type_error.306` because `.value()` can only be called on a JSON object.
  3. Sibling defects were also present:
     - `cli_handler.cpp`'s `collect_matches()` recursively traversed `elem["children"]` without checking if child elements were objects, throwing `type_error.306` during filtered reads like `screen read --only-buttons`.
     - `element_renderers.cpp` (`SplitterShellRenderer`) and `semantic_classifier.cpp` lacked guards for non-object JSON values.
     - `GridAnalyzer::identify_grid_sections` assumed `col_cells.front().row` was row-sorted, but `cols_map[col]` was insertion-ordered.
- **Immediate Fix**:
  1. In `GridAnalyzer::detect_grid_layout()`, guarded with `elem.is_object()`.
  2. In `GridAnalyzer::parse_grid_labels()`, guarded with `elem.is_object()` and eliminated redundant recursion into `elem["children"]` (since all elements are already present in the flat array).
  3. In `GridAnalyzer::identify_grid_sections()`, replaced `col_cells.front().row` with `std::min_element` over `col_cells`.
  4. In `SemanticClassifier::should_display()` and `has_semantic_children()`, guarded with `if (!elem_json.is_object()) return false;` and per-element object checking.
  5. In `cli_handler.cpp`, guarded `element_matches_filter` and ensured `elem["children"][0].is_object()` before recursing.
  6. In `ScreenMarkdownFormatter`, guarded all loops over `hierarchy[...]` and per-element child processing.
  7. Removed dead static `flatten_and_group_elements` from `com_automation_engine.cpp`.
  8. Created Catch2 unit test suite `tests/unit/test_grid_analyzer.cpp` verifying string child arrays, mixed child arrays, and Markdown formatting.
- **Foundational / Protocol Improvement**:
  - *Per-Element Type Guarding vs Heuristics*: Never assume uniformity in JSON child collections (e.g., sniffing only `children[0].is_object()`). Always guard every element individually (`if (!elem.is_object()) continue;`).
  - *Flat Array Traversal Integrity*: When screens are represented as flat arrays of discovered elements, avoid downward recursive traversals into container `children` properties unless explicitly rehydrating a hierarchical DOM tree.
  - *External Peer Review Integration*: Leveraged non-interactive Claude CLI review to discover 3 sibling sites (`cli_handler`, `SplitterShellRenderer`, dead code) and the row sorting bug in `identify_grid_sections` before production deployment.
  - *Live Session DoD Verification*: Verified live against SE16 on Bigfox with 410 elements on TADIR entries screen. Formatted cleanly in 8.4s without any exceptions.

---

### [ERR-016] Layout Container Extraction Omission & Unprotected GuiShell Toolbar Invocation

- **Status**: RESOLVED
- **Severity**: High (Process Crash / SEH Access Violation 0xC0000005)
- **Symptom**: Calling `fairyfly screen read` on screens containing splitter containers (such as the initial tree view in transaction `SM59`) caused `fairyfly.exe` to terminate unexpectedly with an unhandled Win32 Structured Exception (`0xC0000005`).
- **Root Cause**:
  1. *Incomplete Phase 2 Container Skip Filter*: In `src/screen_reader.cpp:1200`, container types (`GuiContainerShell`, `GuiCustomControl`, and `is_shellcont`) were skipped during Phase 2 metadata extraction because their children were already discovered during Phase 1 traversal. However, layout containers such as `GuiSplitterShell`, `GuiSplitterContainer`, `GuiDockShell`, and `GuiContainerCtrl` were omitted from this skip condition.
  2. *Unshielded Toolbar Sub-Control Invocations*: When Phase 2 processed `GuiSplitterShell`, `ElementMetadataExtractor::extract` triggered forced enumeration into internal sub-controls (`shellcont[1]/shell[0]` toolbar). Invoking `GetButtonId` / `GetButtonText` on arbitrary GuiShell controls via `IDispatch::Invoke` triggered an internal access violation in SAP GUI's native DLLs. Because standard C++ `try/catch` cannot intercept Win32 SEH exceptions without explicit MSVC `__try/__except` framing, the crash terminated the process.
- **Immediate Fix**:
  1. Updated `src/screen_reader.cpp` to skip all layout container types (`GuiSplitterShell`, `GuiSplitterContainer`, `GuiDockShell`, `GuiContainerCtrl`) during Phase 2 metadata extraction.
  2. Added `safe_invoke()` helper with `__try / __except(EXCEPTION_EXECUTE_HANDLER)` in `src/com/wrapper_helpers.cpp` and `src/include/com/wrapper_helpers.h`.
  3. Replaced raw `dispatch_->Invoke` with `safe_invoke` in all `ComGuiElement::get_button_*` methods (`get_button_id`, `get_button_text`, `get_button_tooltip`, `get_button_type`, `get_button_enabled`).
- **Foundational / Protocol Improvement**:
  - *Defensive Native Boundary*: All COM invocations interacting with vendor UI controls must be insulated with structured exception handling (`__try / __except`), ensuring that internal crashes inside SAP GUI controls degrade gracefully into error codes rather than crashing the assistant CLI.
  - *Unified Container Filter*: Keep container identification synchronized with `SemanticClassifier::is_layout_only` so traversal and metadata extraction behave identically across all screens.



---

### [ERR-017] Multi-Tab Read Redundant Window Traversal & Deep Menu Subtree Explosion (Latency Spike)

- **Status**: RESOLVED
- **Severity**: Medium / Performance (33.7s latency per multi-tab screen read)
- **Symptom**: Running `fairyfly screen read` on screens with multiple tabs (such as transaction `SM59` RFC Destination detail screen with 5 tabs) took over 33 seconds to return.
- **Root Cause**:
  1. *Full Window Re-Read per Tab*: `ScreenReader::read_with_tabs` previously called `read(true)` inside the loop for each tab. This re-read all window elements (menubars, toolbars, titlebars, statusbars) from scratch 6 times (1 initial read + 5 tab cycles = 966 elements extracted across COM roundtrips).
  2. *Deep Recursive `GuiMenu` Traversal*: Top-level menus in `GuiMenubar` contained nested sub-menus (`GuiMenu`). The recursive tree traversal traversed down into all 96 nested submenu items. However, `ScreenMarkdownFormatter` only renders top-level menus (`/mbar/menu[N]`) and discards all nested submenu paths. Over 60% of all discovered elements were redundant submenu nodes.
  3. *CLI Process Roundtrips*: Multi-step tasks spawned separate CLI processes, incurring process initialization and COM connection overhead for each step.
- **Immediate Fix**:
  1. *Pruned Menu Traversal*: Added `type == "GuiMenu"` to `is_known_leaf()` in `src/screen_reader.cpp:601`. Top-level menus are collected for the menubar, but recursive traversal into the 100+ deep nested sub-menus is completely bypassed.
  2. *Scoped Tab Traversal*: Refactored `ScreenReader::read_with_tabs` to traverse and extract ONLY elements scoped under the active `tab_elem` using a newly extracted helper `extract_metadata_for_collector()`. Tab element count dropped from 161 elements down to ~15-20 per tab.
- **Foundational / Protocol Improvement**:
  - *Context-Scoped Exploration*: Multi-tab extraction must scope traversal strictly to the subscreen container owned by the selected tab rather than resetting to the root window.
  - *Semantic Leaf Pruning*: Elements that are purely decorative or discarded by output formatters (such as deep menu hierarchies) should be pruned at the leaf classifier stage to eliminate unnecessary COM interop roundtrips.
  - *Unified Single-Session MCP*: Encourage long-running sessions via `fairyfly serve` MCP server or batch scripts rather than multi-process CLI invocations.

---

### [ERR-018] Process Startup Failure (0xC0000142 STATUS_DLL_INIT_FAILED) After Binary Overwrite & Broken PostBuildEvent

- **Status**: DIRECT ARTIFACT REFRESHED AND RELINK VERIFIED; NO CURRENT FAILURE (ROOT CAUSE UNKNOWN)
- **Severity**: High / Operational
- **Symptom**: After recompiling, invoking `build\Release\fairyfly.exe` failed immediately with exit code `-1073741502` (`0xC0000142` - `STATUS_DLL_INIT_FAILED`) without printing stdout/stderr. In contrast, `unit_tests.exe` and identical copies of `fairyfly.exe` ran normally.
- **Root-cause evidence**: The earlier loader failure and later SAP ROT attachment failure affected the same build artifact. A byte-identical fresh copy worked at the exact original pathname, which narrows the attachment failure to the old file instance or its file-specific state, not the pathname alone. The claim that Windows AppCompat / Fault Tolerant Heap flagging caused either failure is unproven. A registry search under the standard HKCU/HKLM AppCompatFlags keys found no `fairyfly.exe` entry. A previously generated `fairyfly.vcxproj` did contain a broken vcpkg `PostBuildEvent`; that build failure was separate from the attachment behavior.
- **Immediate Fix**:
  1. Replaced the failing `PostBuildEvent` in `fairyfly.vcxproj` with `<Command>exit /b 0</Command>`, allowing clean end-to-end MSBuild finalization.
  2. Deployed execution to a refreshed binary path (`build\Release\fairyfly_app.exe` or `bin\fairyfly.exe`), bypassing the stale Windows AppCompat cache lock.
- **Foundational / Protocol Improvement**:
  - *Clean Staging Directory*: Build artifacts should be cleanly staged to a dedicated execution directory (e.g. `bin/`) decoupled from CMake intermediate build directories.
  - *Build System Path Hardening*: CMake-generated project files must not contain hardcoded absolute paths to nonexistent external vcpkg or tool directories.
- **2026-09-26 live check**: The exact `build/Release/fairyfly.exe` path still failed to attach to the active SAP GUI session and observed only 13 processes. Byte-identical copies at `scratch/fairyfly.exe` and `build/Release/fairyfly2.exe` attached and found the session. The staging workaround works; the path-specific failure is not resolved.
- **Automated staging verification**: CMake now copies the linked executable to `build/bin/Release/fairyfly.exe` after a successful build. Removing that staged file and rebuilding recreated it; its SHA-256 matched the direct output, and it found the one live SAP session while the direct output found zero. A subsequent no-op CLI build took 1.36 s. README, Makefile, and `build_fast.bat` point to the staged path.
- **Direct-artifact refresh**: On 2026-09-26, the old direct executable and staged copy had identical SHA-256 hashes and matching ACLs. Moving the old direct file aside and copying the staged bytes to that exact pathname made read-only `list` find one Bigfox connection. Restoring the old file reproduced the zero-connection result and SAP trace mutex errors. The direct artifact was then replaced with the fresh copy; a 1.49 s no-op Release build left it working with one connection and zero sessions. Backend scripting remains disabled.
- **Relink verification**: A forced Release rebuild relinked the executable and restaged it; both paths had the same new SHA-256 hash and independently listed one Bigfox connection with zero sessions. A subsequent one-source `main.cpp` edit relinked the direct executable again in 8.93 s, and its read-only `list` still found Bigfox. The full 73-case CTest run passed with four SAP-dependent skips. The old file instance's failure remains unexplained.

---

### [ERR-019] Silent Success on Nonexistent or Rejected Transactions (`tcode` Command)

- **Status**: FIXED & VERIFIED LIVE
- **Severity**: Medium / Protocol Correctness
- **Symptom**: Running `fairyfly tcode SE16N` returned `"status": "success"` with message `"Transaction started"`, even though `SE16N` does not exist on this SAP NetWeaver instance and SAP GUI statusbar displayed `"Transaction SE16N does not exist"`.
- **Root Cause**: `ComAutomationEngine::execute_transaction()` called `session->start_transaction(tcode)` and `session->wait_for_completion(500)`, then unconditionally set `result.status = Result::Status::Success`. SAP GUI's COM API does not throw an HRESULT failure when a transaction code does not exist or authorization is denied; instead, it outputs an error or warning message on the window's `GuiStatusbar` (message type `'E'`, `'A'`, or `'W'`) and keeps the session on the existing transaction.
- **Implemented Fix**:
  1. Updated `ComAutomationEngine::execute_transaction()` to query the active window statusbar (`wnd[0]/sbar`).
  2. If `messageType` is `'E'` or `'A'`, or if the message contains `"does not exist"` / `"not authorized"`, return `Result::Status::Error` with code `TRANSACTION_FAILED` or `TRANSACTION_ABORTED` and capture the statusbar error message text.
  3. If `messageType` is `'W'`, return success with warning metadata in `result.data["warning"]`.
- **Measured Verification (Live SAP GUI on `bigfox`)**:
  - `fairyfly tcode NONEXISTENT` exited with code 1, returned `TRANSACTION_FAILED`, and surfaced exact statusbar text `"Transaction NONEXISTENT does not exist"` in 250ms.
- **Foundational / Protocol Improvement**:
  - *Statusbar Semantic Verification*: Any SAP action (transaction start, batch input, button click) should automatically check for statusbar errors (`messageType == 'E' | 'A'`) as part of its completion contract.
  - *Feedback Loop*: Agents need reliable error feedback to attempt fallback strategies (e.g., falling back to `SE16` when `SE16N` does not exist).

---

### [ERR-020] `GuiUserArea` Tabular Grid Cell Proliferation (High Latency) & Missing Markdown Table Formatting

- **Status**: FIXED & VERIFIED IN TESTS (Catch2 Red-Green TDD)
- **Severity**: Medium / Performance & Usability
- **Symptom**: On tabular screens implemented as `GuiUserArea` (e.g., `SE16` classic data browser, `SM04` user list), screen reading took 5.5 seconds to query 214 elements. Furthermore, the markdown output printed only the header metadata and omitted the actual data rows.
- **Root Cause**:
  1. *Redundant COM Interop for Every Cell*: Even though `ScreenReader` detects the grid pattern (`Extracted 15 rows × 11 columns from GuiUserArea grid`), all 200+ cell labels (`lbl[col, row]`) were retained in the element list. `extract_metadata_for_collector()` queried COM for every cell individually.
  2. *Cell Coordinate Inversion*: In SAP GUI, `/lbl[col,row]` uses `col` as character column offset and `row` as line index. The code inverted these coordinates (`grid_row = col`, `grid_col = row`), corrupting tabular alignment.
  3. *Obsolete 225-Call Probe Loop*: Phase 2 performed a 15x15 blind probe calling `find_by_id("/lbl[r,c]")` across 225 combinations even when child elements were already traversed.
  4. *Missing Formatter & Table Routing*: `GuiUserArea` was omitted from `TableFormatter::can_format`, and `flatten_and_group_elements` routed it to generic `forms` instead of `tables`.
- **Immediate Fix**:
  1. Inverted coordinate mapping in `element_metadata_extractor.cpp` to correctly pair `col` and `row`.
  2. Overhauled `extract_userarea_grid_data` in `ScreenReader` to parse dynamic column positions directly from the child traversal and populate `table_data` with proper `columns` and `rows`.
  3. Pruned individual cell IDs (`collector.is_grid_id`) from loose element lists and Phase 3 COM extraction, eliminating ~1,600 COM calls. Removed the 225-call probe loop.
  4. Enabled `GuiUserArea` in `TableFormatter::can_format()` and routed elements with `table_data` to `grouped["tables"]`.
  5. Validated with Red/Green Catch2 unit test in `test_markdown_table_formatter.cpp` (10/10 assertions green).
- **Foundational / Protocol Improvement**:
  - *Composite Control Abstraction*: Tabular structures composed of primitive labels must be abstracted into first-class table components before metadata extraction.
  - *Single-Pass Traversal*: Extract cell texts during the primary child iteration rather than querying elements multiple times across separate phases.

---

### [ERR-021] Headless Session Connection & `sapshcut` Interactivity Trap

- **Status**: FIXED
- **Severity**: Medium / Automation Robustness
- **Symptom**: When SAP GUI has no active connection open, running `fairyfly launch <connection>` via `sapshcut.exe` can hang ("Not Responding") or return error code -1 due to Windows security dialogs or lack of interactive desktop elevation.
- **Root Cause**: `sapshcut.exe` was designed for desktop shortcut launches and interactive prompts. In automation and agent contexts, launching via `sapshcut` with command-line password `-pw` triggers Windows/SAP GUI security confirmation dialogues, blocking execution. Furthermore, `ComGuiApplication` creates `SapGui.ScriptingCtrl.1` via COM but did not expose `OpenConnection("Bigfox")` directly in the C++ API.
- **Implemented Fix**:
  1. Added `ComGuiApplication::open_connection(const std::string& name, bool sync, bool raise_error)` and `open_connection_by_connection_string(...)` in `src/include/com/wrapper.h` and `src/com/application.cpp` using native OLE Automation `IDispatch::Invoke`.
  2. Enhanced `ComGuiApplication::create()` to call `GetScriptingEngine()` when instantiated via `SapGui.ScriptingCtrl.1`, ensuring GuiApplication is accessible even without active GUI windows.
  3. Updated `ComAutomationEngine::launch_connection` to use native COM by default. `sapshcut.exe` fallback now requires the explicit `--allow-sapshcut` flag and is considered only when native COM did not open a connection. A native connection with zero sessions returns `SESSION_NOT_READY` rather than spawning another process.
  4. Added Catch2 unit tests in `tests/unit/test_com_wrapper.cpp`.
- **Measured Verification (Live SAP GUI on `bigfox`)**:
  - `fairyfly launch Bigfox` launched connection and established session `/app/con[1]/ses[0]` in **1,660ms**.
  - Zero hangs, zero `-pw` interactive prompts, and 0 external process spawn overhead.
  - A later native Bigfox attempt opened a connection but no session while the backend was unavailable. The revised `launch Bigfox` returned `SESSION_NOT_READY`, spawned zero `sapshcut` processes, and the new empty connection was closed afterward.
- **Foundational / Protocol Improvement**:
  - *In-Process COM Connection Management*: Eliminate shell process dependencies (`sapshcut.exe`) for session establishment. Use native `GuiApplication.OpenConnection` COM API.

---

### [ERR-022] Out-of-Process COM Roundtrip Explosion During Screen Reading & Uncached DISPID Resolution

- **Status**: IN PROGRESS
- **Severity**: High / Performance
- **Symptom**: `fairyfly screen read` takes 2.5s to 6.4s on standard SAP screens (50 to 170 elements). Users report "Everythink super slow here".
- **Root Cause**:
  1. *Uncached DISPID Resolution*: `SapGuiObject::get_string_property`, `get_int_property`, and `get_bool_property` unconditionally invoke `get_dispid_via_typeinfo` on every property query. For every single property fetch, `get_dispid_via_typeinfo` performs 5 out-of-process LRPC calls (`obj->AddRef()`, `obj->Release()`, `obj->GetTypeInfo()`, `type_info->GetIDsOfNames()`, and `type_info->Release()`) before calling `obj->Invoke()`. On a screen with 100 elements and 10 property queries, this generates 5,000+ out-of-process LRPC roundtrips!
  2. *Redundant Leaf Probing*: `ElementMetadataExtractor::extract` queries `is_changeable()` on controls that are never changeable (`GuiLabel`, `GuiButton`, `GuiBox`, `GuiToolbar`), queries `get_label()` on controls with no label association, and queries `get_container_type()` which calls `get_child_count()` on leaf nodes (`GuiLabel`, `GuiButton`, `GuiTextField`).
- **Target Fix**:
  1. *Type-Level DISPID Cache*: Implement a fast cache in `SapGuiObject` mapping `(sap_type, property_name) -> DISPID`. Since COM dispinterfaces in SAP GUI (`sapfewse.ocx`) are immutable per control class, DISPID resolution only occurs once per element class per session. Cache hits immediately call `IDispatch::Invoke` with 0 metadata RPC calls (saving ~83% of RPC traffic).
  2. *Type-Directed Property Pruning*: Restrict `is_changeable()` and `get_label()` strictly to input controls (`GuiTextField`, `GuiCTextField`, `GuiPasswordField`, `GuiComboBox`, `GuiCheckBox`, `GuiRadioButton`).
  3. *Leaf Fast-Path in `get_container_type()`*: Immediately return `""` for known leaf controls without invoking `get_child_count()`.
- **Foundational / Protocol Improvement**:
  - *Metadata Batching & Caching*: Out-of-process COM interfaces must never query static metadata repeatedly. Cache dispinterfaces at the class level.
  - *Contract-Driven Querying*: Only query properties supported by the control's semantic contract.
- **2026-09-26 check**: Type-level DISPID caching, metadata property pruning, and a leaf fast path are present in code. Live simple-screen reads took roughly 0.4–0.8 seconds, but representative complex-screen measurements are still needed before closing this item.
- **2026-09-26 SE11 measurement**: Displaying table `TADIR` produced 77 screen elements and a populated 10-column `GuiTableControl`. A no-tab JSON read took 843 ms CLI time (1,099 ms wall time); a full read expanded all six tabs in 3,971 ms CLI time (4,278 ms wall time). This is substantially below the earlier 58.4-second six-tab observation, but broad latency and repeatability checks remain open.

- **2026-09-26 representative no-tab measurements**: Three SE16 TADIR 200-hit result reads (101 elements) took 1,998, 2,043, and 2,042 ms. Three `/IWFND/MAINT_SERVICE` reads (23 elements and 64 service rows) took 2,432, 2,385, and 2,399 ms. Five simple SEGW reads (9 elements) took 68-77 ms. These are new baselines rather than proof of a speedup over different historical screens.
- **2026-09-26 GridView column-order optimization**: A new live Gateway measurement found that the 20-row, nine-column service grid re-fetched its `ColumnOrder` for every cell (180 requests) in addition to the header read. Three pre-change no-tab reads took 2,897, 2,758, and 2,658 ms. A red-green regression passes one resolved column-order snapshot to the bounded row reader; `TableDataExtractor` reads it once per grid. Three post-change reads took 1,514, 895, and 906 ms, with the same 20 rows, nine headers, 64 total-row count, and first/last service names. Markdown still rendered `SERVICE_NAME` and `ADT`; all 94 CTest cases passed at that stage. Selecting row 40 did not change the default first-20 extraction, which led to the opt-in row-limit change below.
- **2026-09-26 row-limit follow-up**: `screen read --max-rows N` requests up to 200 rows while leaving the fast 20-row default intact. Before the change, the CLI rejected the option. A regression covers row 40 from a 64-row sample and rejects zero or excessive limits. The rebuilt CLI read all 64 live Gateway service rows, including row 40 `ZSEPMRA_SHOP` and row 63 `ZDEMO_C_SALESORDER_TP_D_CDS`, in about 2.1 s; Markdown also rendered both later rows and `SERVICE_NAME`. The default still read 20. On live SE11 Display of TADIR, a 64-row request returned 32 visible native-table rows versus 20 by default. Both CLI boundary values (0 and 201) failed argument validation without a SAP read. All 95 CTest cases passed. Horizontal scrolling and other GridView layouts remain to verify.
- **2026-09-26 method-lookup follow-up**: `ComGuiElement::get_cell_value` still resolved `GetCellValue` with `get_dispid_via_typeinfo` for every cell, bypassing the existing type-level cache. A fake COM regression counted two method-name lookups for two cells before the change and one afterward. On live SM50, three no-tab reads of the same 32-row, 17-column grid took 1,324–1,545 ms before and 916–929 ms after. Headers, row counts, and cell widths stayed unchanged. All 98 CTest cases passed. This is a measured improvement on one SAP screen, not a general performance guarantee.
- **2026-09-26 toolbar-lookup follow-up**: Gateway's 19-button built-in grid toolbar similarly re-resolved `GetToolbarButtonId` and `GetToolbarButtonTooltip` for every button. A fake COM regression failed with two lookups per method for two buttons, then passed with one after both methods used the type-level cache. Three live Gateway no-tab reads took 593–602 ms before and 544–559 ms after; the same 20 rows, nine columns, and 19 button IDs remained available. All 99 CTest cases passed. The measured gain is modest and screen-specific.

---

### [ERR-023] `click` Cannot Activate SAP GUI Menus

- **Status**: FIXED & VERIFIED LIVE
- **Severity**: Medium / Interaction correctness
- **Symptom**: On the live SM59 screen, clicking Edit > Create by menu ID returned `COM_ERROR`: `Select only works on CheckBox, RadioButton, or Tab elements`.
- **Root cause**: `ComGuiElement::press()` fell back to `select(true)` when `Press` was unavailable, but `ComGuiElement::select()` rejected `GuiMenu` before invoking its `Select()` method.
- **Fix and verification**: `press()` now routes `GuiMenu` through the no-argument `Select()` path. The live SM59 Edit > Create click returned success and opened the nine-element `Create Destination` dialog. The Release build succeeded and CTest passed 58/58 tests.

---

### [ERR-024] `fill` Cannot Set SAP GUI Combo-Box Selection

- **Status**: FIXED & VERIFIED LIVE
- **Severity**: Medium / Interaction correctness
- **Symptom**: On the live SM59 Create Destination dialog, `fill` accepted the destination name but setting connection type `G` on `GuiComboBox` returned `EXCEPTION: Failed to set property`.
- **Root cause**: `ComGuiElement::set_text()` always writes the `Text` property, which is not the writable selection property on `GuiComboBox`.
- **Fix and verification**: `set_text()` now writes `Key` for `GuiComboBox`. The live SM59 form accepted type `G`, opened the HTTP destination editor, and saved destination `ZFFLY260926`. The destination appeared under the type G tree, its description was edited and saved, and it was deleted after testing.

---

### [ERR-025] `click` Reports Success When SAP Rejects an Action

- **Status**: FIXED FOR OBSERVED CASES; REVISIT IF A NEW MESSAGE VARIANT FAILS
- **Severity**: Medium / Protocol correctness
- **Symptom**: After deleting a test SE11 domain, clicking Display returned CLI success although the SAP status bar said the domain did not exist. SAP tagged that message as type `S`, so checking only `E` and `A` is insufficient.
- **Fix and verification**: A shared status check snapshots the status bar before an action and inspects new messages after completion. It covers ordinary clicks, tree actions, and synthetic toolbar buttons. It returns `ACTION_FAILED` for `E`/`A` and observed rejection text even when SAP marks the message `S`. A live Display attempt for nonexistent `ZFFLY260928` returned JSON error and exit code 1. In SEGW, a read-only `Delete Project` tree context action now returned `ACTION_FAILED` with SAP's `Action not possible in read-only mode`; two subsequent mode-toggle toolbar clicks succeeded without treating the stale message as a new failure.
- **2026-09-26 follow-up**: A new SAP `W` message after a submitting button, toolbar, tree context/double-click, or modal-list action now returns `ACTION_OUTCOME_UNVERIFIED` with the warning text, since SAP may require a second confirmation. Known rejection text still returns `ACTION_FAILED`, and stale unchanged warnings are ignored. Unit coverage includes all three cases; the complete 69-case CTest run passes with four SAP-dependent skips, and the whole executable passes 390 assertions. Live warning behavior, other languages, and a rejected synthetic toolbar action remain to verify.

---

### [ERR-026] Tree Context-Menu Actions Missing from CLI

- **Status**: FIXED & VERIFIED LIVE
- **Severity**: Medium / Workflow coverage
- **Symptom**: SEGW project editing requires node context menus (for example, Entity Types > Create). `fairyfly click --node-key` supports select, expand, collapse, and double-click only.
- **Live evidence**: A direct SAP GUI Scripting call to `GuiTree.NodeContextMenu` followed by `SelectContextMenuItemByText("Create")` opened the Create Entity Type dialog. The CLI then created `Sample` and `Samples` under temporary local project `ZFFLY260926`; the project was deleted after testing. Property creation, runtime generation, and publication remain unverified.
- **Fix and verification**: `click --node-key <key> --tree-action contextmenu --menu-item <text>` now calls `NodeContextMenu` and `SelectContextMenuItemByText`. The live CLI command opened SEGW's Create Entity Type dialog. The CLI rejects a missing `--menu-item` with `MENU_ITEM_REQUIRED`.

---

### [ERR-027] GridView Toolbar Buttons Use a Different SAP Scripting Method

- **Status**: FIXED & VERIFIED LIVE
- **Severity**: Medium / Workflow coverage
- **Symptom**: The synthetic click path for SEGW's properties grid Append Row button returned `COM_ERROR` because it called `PressButton`, which is for `GuiShell` toolbars. `GuiGridView` exposes `PressToolbarButton` instead.
- **Fix and verification**: `ComGuiElement::press_button()` now tries both methods. A live click on synthetic button `&LOCAL&APPEND` succeeded. `fill --row 0 --column NODE_NAME/EDM_CORE_TYPE/IS_KEY` then edited the appended row, including its key checkbox; screen read confirmed `ID`, `Edm.String`, and `X`. Runtime generation and an HTTP 200 metadata request verified the resulting model end to end. GridView toolbar discovery and row selection were added and verified on `/IWFND/MAINT_SERVICE`.
- **Cleanup finding**: Generating runtime artifacts for disposable SEGW project `ZFFLY260929` registered a technical service and model and created four ABAP classes. Deleting the project did not remove those artifacts. The service, model, and all four classes were subsequently deleted; SE24 confirmed each class no longer exists.

---

### [ERR-028] GridView Toolbar Discovery and Row Selection Are Missing

- **Status**: FIXED & VERIFIED LIVE
- **Severity**: Medium / Workflow coverage
- **Symptom**: `/IWFND/MAINT_SERVICE` exposes Add Service, Delete Service, and Delete System Alias Assignment through `GuiGridView` toolbar IDs. Screen read omits those button IDs, and `click` has no GridView row-selection action. The end-to-end test needed direct SAP GUI Scripting calls to discover button IDs and select the exact service/alias rows.
- **Fix and verification**: GridView screen output now includes synthetic clickable button paths and tooltips, including `ADD_SRV`, `DELETE_SERVICE`, and `UNASSIGN_ALIAS` in `/IWFND/MAINT_SERVICE`. `click <grid> --row 0 --column SERVICE_NAME` selected `ADT`; independent SAP scripting reported `SelectedRows=0`, `CurrentCellRow=0`, and service `ADT`. The CLI rejects missing column and negative row options. Claude's cross-check prompted a second live test: the `LOCAL` System Aliases grid rejects `Click` but accepts `SetCurrentCell` and `SelectedRows`. Selection now succeeds there too. A future disposable-service run should exercise the entire cleanup path through the CLI.

---

### [ERR-030] GuiShell GridView Was Misclassified in Markdown Screen Output

- **Status**: FIXED & VERIFIED LIVE
- **Severity**: Medium / Data presentation
- **Symptom**: JSON screen read exposed `/IWFND/MAINT_SERVICE` service rows, but Markdown showed three `Table (0 rows, 0 cols)` sections. `ScreenReader::flatten_and_group_elements` grouped every `GuiShell` before checking its `GridView` subtype or table data.
- **Fix and verification**: GridView shells now enter the tables group, and the table formatter renders visible rows. A live Markdown read showed `SERVICE_NAME`, the `ADT` row, and `Showing 20 of 64 rows`. It also listed the GridView toolbar buttons by clickable path. The legacy `TableRenderer` now delegates to the shared formatter and handles null table data safely; a regression test checks row content and dimensions. All 59 CTest cases pass.

---

### [ERR-029] Gateway Frontend Service Cleanup Has Prerequisites

- **Status**: DOCUMENTED; AUTOMATION PENDING
- **Severity**: Low / Test hygiene
- **Symptom**: Deleting `ZFFLY260930_SRV` in `/IWFND/MAINT_SERVICE` was refused first because its SICF node existed, then because its `LOCAL` system alias assignment existed.
- **Verified cleanup order**: Delete the exact `/sap/opu/odata/sap/ZFFLY260930_SRV` leaf in SICF, unassign `LOCAL` in the selected service's System Aliases grid, delete the frontend service, delete backend technical service in `/IWBEP/REG_SERVICE` and model in `/IWBEP/REG_MODEL`, delete the SEGW project, then delete its four generated classes in SE24. The frontend list shrank from 65 to 64 rows and no longer contained the test service; the SICF tree no longer contained the leaf; SEGW reported the project deleted; SE24 subsequently reported that each of the four classes does not exist.

---

### [ERR-031] Runtime Factory Could Return a Synthetic SAP Engine

- **Status**: FIXED & VERIFIED IN FORCED FAILURE TEST
- **Severity**: High / Result integrity
- **Symptom**: `AutomationEngine::create()` caught a real COM engine initialization exception and returned `StubAutomationEngine`. The stub could report successful connection, screen, and screenshot operations without SAP GUI.
- **Fix and verification**: The runtime factory no longer substitutes a stub, and CLI handler creation is inside the structured exception handler. A forced MTA/STA apartment mismatch found that `ComAutomationEngine` still swallowed the COM initialization exception and left a disconnected engine. `ComGuiApplication::create` now raises a distinct `ComInitializationException`, which the constructor rethrows. A fresh-thread factory test verified propagation of `RPC_E_CHANGED_MODE`. The real CLI command body is callable from a test on an MTA thread; that test verified a structured JSON error and exit code 1. Claude's cross-check identified missing `CoUninitialize` on successful application-wrapper destruction and the possibility of uncaught `_com_error`; the wrapper now releases its interface and balances COM initialization, and the CLI catches `_com_error` and unknown exceptions. A fresh-thread lifecycle test verifies the apartment is released. Normal `list` operation with SAP GUI absent still succeeds with zero sessions. The complete CTest run had 60 passes, four SAP-dependent skips, and zero failures across 64 cases.

---

### [ERR-032] Non-ASCII COM Arguments Can Be Corrupted

- **Status**: FIXED & VERIFIED LIVE
- **Severity**: Medium / Internationalization
- **Symptom**: Several tree/grid methods build `std::wstring` from narrow bytes, and other calls pass `char*` to `_variant_t`. If those strings are UTF-8, non-ASCII node keys, column names, or values can be sent to SAP with incorrect characters. The live workflows so far used ASCII technical IDs, so they do not validate this case.
- **Fix and verification**: Windows now enters through `wmain` and converts arguments to UTF-8 before CLI parsing. A shared helper converts UTF-8 and UTF-16 explicitly for SAP `BSTR` calls and returned text; the tree/grid, connection, and screenshot argument paths were updated. A live SEGW Create Project dialog round-tripped `Grüße 中文` through `fill` and `get` without saving a project, and the dialog was canceled. A screenshot was saved to an accented filename, verified on disk, and removed. A disposable SEGW project then stored `Grüße 中文` in an entity type's mutable GridView label cell; the value persisted after restarting SEGW and reopening the entity type. The project was deleted, and its tree node disappeared. A unit test verifies Latin and Chinese characters plus invalid UTF-8; 61/61 CTest cases pass.

---

### [ERR-033] Gateway Screen Read Exposed Credential-Bearing Response Headers

- **Status**: FIXED FOR HEADER GRIDS AND NAMED CREDENTIAL COLUMNS; BROADER SURFACE REVIEW PENDING
- **Severity**: High / Sensitive data exposure
- **Symptom**: `/IWFND/GW_CLIENT` displayed a successful OData metadata response whose header grid included a session cookie. Automatic screen reads returned the header value verbatim.
- **Fix and verification**: `ScreenReader` redacts credential-bearing header rows before assigning either `grid_data` or `table_data`; a live Gateway check verified JSON and Markdown omitted the session-cookie marker. The same redaction covers named credential columns such as `PASSWORD` and `API_KEY`, and direct `ElementMetadataExtractor` table output and its cache. A tree extractor no longer logs raw node text. Claude's focused cross-check found that localized value titles, punctuated name titles, and missing column metadata bypassed row redaction; regression cases now cover all three. Successful field and GridView fills mask the submitted value regardless of technical field/column name. The latest Release build and all 69 CTest cases pass with four SAP-dependent skips; the full test executable passes 383 assertions. Response bodies and free-form text still need a separate review.

---

### [ERR-034] Modal SAP List Rows Could Not Be Selected by CLI

- **Status**: FIXED & VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: Medium / Workflow coverage
- **Symptom**: Gateway Client's Entity Set picker renders choices as coordinate-based `GuiLabel` elements. `click` called `Press` on an Airport row and returned `COM_ERROR` because labels do not expose that method.
- **Fix and verification**: For modal list labels, `click` checks that the target popup is active, calls `SetFocus`, and sends SAP F2. The live CLI selected Airport, Airline, and the near-last Travel row using full and short SAP paths; each request URI changed to the chosen entity set, and all three collection GETs returned HTTP 200 with JSON responses. Claude's focused review identified the short-path mismatch and active-window risk, which were corrected before the final live test. All 62 CTest cases pass. Other SAP list pickers and disabled-row behavior still need verification.

---

### [ERR-035] Full SAP Element IDs Lost Their Window Prefix

- **Status**: FIXED & VERIFIED
- **Severity**: Medium / Path handling and diagnostics
- **Symptom**: `ElementId::get_window()` split a canonical `/app/con[...]/ses[...]/wnd[...]` path at its first slash and returned an empty window ID; `get_element_path()` treated `app/con...` as the relative element path. This made result metadata and window-mismatch guidance incorrect for IDs returned by screen read.
- **Fix and verification**: The parser now identifies the `wnd[...]` segment in canonical IDs and returns the full window prefix plus the path relative to it. Unit coverage checks both parts and `with_window()`; a live `get` on a canonical Gateway Client control returned the correct window ID. All 62 CTest cases pass.

---

### [ERR-036] Unit Test Launched Multiple SAP GUI Error Dialogs

- **Status**: FIXED & VERIFIED
- **Severity**: High / Test side effects
- **Symptom**: Repeated CTest runs opened SAP GUI popups saying the `NONEXISTENT` system was not in the SAP Logon list.
- **Root cause**: `ConnectionLauncher - sapshcut launch` called the real `sapshcut.exe` with `NONEXISTENT` and asserted only that the returned value was a boolean, so it always passed even while opening external GUI processes.
- **Fix and verification**: Removed the side-effecting, tautological `sapshcut` unit test. A later audit also removed a native `OpenConnection("NonExistentSystem_Test_XYZ")` unit section and an environment-dependent session-wait test. The full 68-case CTest run passes with four SAP-dependent skips, finishes in about 1.5 seconds on this machine, and creates no `sapshcut` process.

---

### [ERR-037] Repeated Identical SAP Rejection Was Reported as Success

- **Status**: FIXED AND VERIFIED LIVE
- **Severity**: High / Result integrity
- **Symptom**: On SE11, Display of a missing domain first returned `ACTION_FAILED`; pressing Display again with the same status-bar rejection returned success because the status text and type matched the pre-action snapshot.
- **Fix and verification**: For submitting buttons, tree double-click/context actions, and synthetic toolbar buttons, an unchanged rejection now yields `ACTION_OUTCOME_UNVERIFIED` instead of success. Non-submitting controls retain the previous comparison behavior. The unit test covers the repeated-message case; 61 CTest cases passed at the time of that fix. A later live SU01 Display of nonexistent `ZFFLYU2609C` returned `ACTION_FAILED` first and `ACTION_OUTCOME_UNVERIFIED` on two repeated clicks with the same SAP absence message.

---

### [ERR-038] Transaction Could Report Success Without Changing Transaction

- **Status**: FIXED AND VERIFIED LIVE; ORIGINAL SESSION STALL CAUSE UNRESOLVED
- **Severity**: High / Result integrity
- **Symptom**: A live `tcode SU01` returned success with `actual_tcode=SE11` and no status-bar error. The SE11 screen also stopped reacting to direct button presses and Enter through SAP GUI Scripting, despite `Busy=False` and a single visible window.
- **Fix and verification**: `execute_transaction` now requires the requested transaction to become active after dispatch. It retries when SAP reports an old, empty, or different transaction code, then returns `TRANSACTION_NOT_STARTED` with the observed code if it still does not match. Claude's focused review found that a repeated identical `E`/`A` status could instead lose its SAP message and that a stale warning could be copied into the next result; the mismatch path now preserves the SAP rejection, and only new warnings/status text are reported. The earlier rebuilt CLI returned `TRANSACTION_NOT_STARTED` for the live stalled SU01 attempt; 66 CTest cases passed at the time of that fix. After server-side scripting was enabled dynamically, the same logged-in Bigfox session dispatched SU01, SM59, SE11, and SE16 successfully with matching transaction codes. A separate modal-rejection false success was subsequently fixed in ERR-067. The cause of the original stalled SE11 session remains unproven.

---

### [ERR-039] Doctor Claimed an Active SAP Session With Zero Sessions

- **Status**: FIXED & VERIFIED LIVE
- **Severity**: High / Diagnostic accuracy
- **Symptom**: With one Bigfox connection, zero sessions, and backend scripting disabled, `doctor` said "Active session verified with backend scripting enabled" because it checked only whether a connection existed.
- **Fix and verification**: Connection enumeration now includes `backend_scripting_disabled` from SAP's `DisabledByServer` property. `doctor` fails its `active_sessions` check when that property is true and warns when connections exist but no sessions are accessible. It also fails when enumeration returns a structured error. The current live check reports `active_sessions=fail` and `overall_health=error`; the Release build and 66-case CTest run pass with four SAP-dependent skips.

---

### [ERR-040] Session Enumeration Could Overcount or Discard Healthy Sessions

- **Status**: FIXED IN CODE; MIXED SCRIPTABILITY AND MULTI-SESSION BEHAVIOR VERIFIED LIVE
- **Severity**: Medium / Diagnostic accuracy
- **Symptom**: `get_application_info` counted the SAP collection size as accessible sessions even when a session object could not be read. One exception while reading a session's active window escaped the loop and could discard the rest of the connection report. `list` also returned success when enumeration supplied an `error` field.
- **Fix and verification**: Connections and sessions are inspected independently; an unavailable active window is nullable, inspection failures are recorded, and accessible counts come from the successfully read session objects. `list` surfaces top-level enumeration errors, and `doctor` warns about partial connection or session inspection. At the time of the fix, a zero-session Bigfox connection passed the count invariant, and 66 CTest cases passed with four SAP-dependent skips. Later Bigfox `list` calls see one accessible logged-in session; a mixed healthy/failing collection still needs a live retest.

---

### [ERR-041] Password Field Text Could Leak Through Screen and CLI Output

- **Status**: FIXED AND VERIFIED LIVE IN SU01
- **Severity**: High / Sensitive data exposure
- **Symptom**: Source review found that automatic element metadata read `GuiPasswordField` text, `get` returned it, and `fill` echoed the supplied value in success output and window-mismatch suggestions. `ComGuiElement::set_text` also logged the value at debug level.
- **Fix and verification**: Password elements now return `[REDACTED]` from the shared COM text accessor, and both screen metadata paths and `get` avoid requesting their raw text. `fill` redacts its success value for password elements and uses a placeholder in path suggestions. Low-level COM string property reads and writes no longer log values. A fake-COM regression test proves that `get_text` never requests the password's text property while an ordinary text field remains readable. The Release build, all 67 CTest cases (four SAP-dependent skips), and the whole Catch2 executable (371 assertions, four skips) passed at the time of that fix.
- **Live SU01 follow-up (2026-09-26)**: Fairyfly opened Create for unsaved disposable `ZFFLYR260926` and filled the Logon-tab password field with a generated in-memory value. That value was absent from debug-level fill output, direct `get` output, JSON screen output, and Markdown screen output. `get` and both password-field JSON entries returned `[REDACTED]`; Markdown contained the redaction marker. Fairyfly left the unsaved screen, and a fresh SU01 Display returned `User ZFFLYR260926 does not exist`. The existing Bigfox connection remained open.

---

### [ERR-042] Untyped DISPID Cache Could Resolve a Property on the Wrong COM Type

- **Status**: FIXED IN CODE; LIVE MIXED-TYPE SANITY PASSED
- **Severity**: High / COM property integrity
- **Symptom**: The password-field regression passed alone but failed in the full test executable: a prior COM object had cached the untyped `Type` property ID, and the fake password object used a different ID. The cache made `get_type()` read its text property instead, bypassing password redaction.
- **Fix and verification**: Property IDs are cached only after an object's SAP type is known; untyped lookups resolve on that object instead of using a shared generic ID. The fake-COM test uses different IDs for password and ordinary text fields and passed both alone and in the full suite; 67 CTest cases passed with four SAP-dependent skips at the time of the fix. A later live SU01 screen read classified ordinary text, radio, and password fields correctly, including two password fields returned as `[REDACTED]`. On 2026-09-27, a fresh unsaved SU01 Create screen independently matched Fairyfly's `GuiPasswordField`, `GuiTextField`, and `GuiComboBox` classifications against direct SAP COM `Type` reads. Three 80-element no-tab reads took 938, 912, and 903 ms inside Fairyfly; both password fields remained masked. A synthetic value filled into one unsaved password field was absent from direct `get`, JSON, and Markdown and was then cleared. SAP COM returned empty password `Text`, so the value could not be independently read back; no Save action occurred. This is a real mixed-type check, though it cannot force the conflicting synthetic DISPIDs or support a causal latency comparison without a matching pre-fix baseline.

---

### [ERR-043] Fallback Launch Could Attach to an Unrelated or Existing Session

- **Status**: FIXED IN CODE; LIVE MULTI-CONNECTION RETEST PENDING
- **Severity**: High / Connection identity
- **Symptom**: `ConnectionLauncher::wait_for_session` ignored its `connection_name` argument and returned the first SAP session in the COM collection. An explicit `--allow-sapshcut` launch could therefore claim success on a different system or on a session that predated the launch.
- **Fix and verification**: The fallback matches the requested SAP Logon description and excludes sessions captured before launch by positional path, COM identity, and SAP's `SystemSessionId`/`SessionNumber` pair where available. It rejects multiple new matches as ambiguous. Unit tests cover name normalization, distinct COM interfaces on the same object, and extraction of the server session key. The side-effecting invalid-system `OpenConnection` unit section and environment-dependent wait test were removed. The Release build and all 70 CTest cases passed with four SAP-dependent skips at the time of the fix. Bigfox scripting is now available in a logged-in session, but the opt-in fallback has not been exercised against multiple live connections. A lone unrelated same-name session appearing during launch remains an unresolved identity risk.

---

### [ERR-044] Launch Could Publish an Incompletely Initialized Session

- **Status**: FIXED; NATIVE LOGIN-SCREEN LAUNCH VERIFIED LIVE; FALLBACK RETEST PENDING
- **Severity**: Medium / Session state consistency
- **Symptom**: Native and fallback launch assigned `current_connection_` and `current_session_` before constructing their reader and screenshot services. An exception during service construction could leave a session visible after launch reported failure. A recovered SAP application could also reach fallback with no `ConnectionLauncher` instance.
- **Fix and verification**: A session is published only after both services are constructed, fallback recreates its launcher from the current application object, and disconnect releases the previous services. Native launch waits up to two seconds for a session unless SAP reports backend scripting disabled, then returns a specific diagnostic. The Release build and 70-case CTest suite passed with four SAP-dependent skips at the time of the fix. After dynamic scripting enablement, a native Bigfox launch opened a scriptable login-screen session and Fairyfly saved its connection file; the separate fallback and concurrent-launch paths remain unverified.

---

### [ERR-045] Named Credential Text Fields Could Expose Values

- **Status**: FIXED; ID-BASED AND ASSIGNED-LEFT-LABEL FIELDS VERIFIED LIVE
- **Severity**: High / Sensitive data exposure
- **Symptom**: The low-level text accessor redacted only `GuiPasswordField`. An ordinary SAP text field with an ID such as `txtS_API_KEY` or an accessibility label such as `Client Secret` still requested and returned its value. That value could reach screen metadata and direct `get` output.
- **Fix and verification**: Input controls now classify credential-bearing IDs and labels before reading `DisplayedText` or `Text`; matching fields return `[REDACTED]`. Accessibility labels are cached on each wrapper so screen metadata does not request them twice. A fake-COM test verifies both ID-based and label-based redaction without any text property read. The Release build and all 71 CTest cases pass with four SAP-dependent skips; the full executable passes 395 assertions. Generic or localized labels and free-form response bodies still require live review.
- **Live field check (2026-09-27)**: A disposable `$TMP` report `ZFFLY_CRED_260927` exposed `P_APIKEY` and `P_TOKEN` as ordinary `GuiTextField` controls on its selection screen. Fairyfly filled each with a distinct short synthetic marker without executing the report. Independent SAP GUI scripting read the exact values from both controls, while Fairyfly direct `get` returned `[REDACTED]` for each and JSON and Markdown screen reads omitted both markers. The fill responses also omitted the markers. Fairyfly left the selection screen; ADT deleted the report and an exact-name search returned `[]`. The local ABAP and VBScript probes were removed, and `list` and `connections` returned zero. A field identified only by an accessibility label remains untested live.
- **Assigned-label follow-up (2026-09-27)**: A second disposable report gave neutral field `P_VALUE` the visible selection-screen caption `Client Secret` through `SELECTION-SCREEN COMMENT ... FOR FIELD`. SAP GUI exposed that caption through `P_VALUE.LeftLabel`, while `AccLabel` and `AccLabelCollection` were empty. The old CLI exposed a synthetic field value in direct `get`, JSON, and Markdown. After ERR-092, all three paths redacted it in a live retest. An `AccLabel`-only field remains covered by fake COM rather than a live example.

---

### [ERR-046] sapshcut Arguments Were Quoted Incorrectly

- **Status**: FIXED IN CODE; LIVE FALLBACK RETEST PENDING
- **Severity**: Medium / Connection reliability
- **Symptom**: The opt-in `sapshcut` fallback interpolated the SAP Logon name, username, client, and password into a `CreateProcessA` command line with simple surrounding quotes. Embedded quotes or trailing backslashes could change how the child parsed the values, and non-ASCII credentials depended on the Windows ANSI code page.
- **Fix and verification**: Each complete argument is escaped according to Windows command-line parsing rules, the exact executable path is passed separately, and `CreateProcessW` receives UTF-16 converted from the CLI's UTF-8 strings. A unit test round-trips representative values through `CommandLineToArgvW` without launching SAP. The Release build and all 72 CTest cases passed with four SAP-dependent skips at the time of the fix; the full executable passed 413 assertions. The fallback remains explicit opt-in and still places the password on the child process command line. Live `sapshcut` verification remains pending; it has not been attempted since scripting became available.

---

### [ERR-047] Compound Credential Names Could Bypass Output Redaction

- **Status**: FIXED IN CODE; FIELD LABELS AND ONE COMPOUND HEADER VERIFIED LIVE; OTHER VARIANTS PENDING
- **Severity**: High / Sensitive data exposure
- **Symptom**: Credential matching recognized exact header names and selected compound input labels, but missed names such as `X-Session-Token`, `My Secret Key`, `Passwort`, and `Kennwort`. Those ordinary text fields or header rows could export their values.
- **Fix and verification**: Header rows, credential columns, and input fields now share compound-name matching that includes common German password labels. A focused table test preserves an ordinary `Content-Type` value while redacting a compound token header and a value literally equal to `Token`; input tests cover compound and German labels. Claude independently flagged the English compound and German cases. The Release build and all 73 CTest cases pass with four SAP-dependent skips; the full executable passes 424 assertions. Other languages, generic labels, and free-form response bodies remain pending live review.
- **Later live coverage (2026-09-27)**: A disposable SE38 report exposed ordinary text fields `P_APIKEY` and `P_TOKEN`; their independently read synthetic values were masked in Fairyfly `get`, JSON, and Markdown. A second report assigned the neutral field `P_VALUE` the visible `Client Secret` label; after ERR-092, the same three output paths masked its synthetic value. Those first checks covered selected English name and label forms; the separate German-label check below extended that coverage.
- **German-label live check (2026-09-27)**: Disposable report `ZFFLYGER260927` assigned neutral `P_VALUE1` and `P_VALUE2` the visible `Passwort` and `Kennwort` labels. Direct SAP GUI scripting confirmed both were ordinary `GuiTextField` controls, their `LeftLabel` values matched the captions, and each contained a distinct synthetic marker after Fairyfly `fill`. Fairyfly `get` returned `[REDACTED]` for both; JSON contained two masked fields, and neither JSON nor Markdown contained either marker. The fields were cleared and independent SAP scripting confirmed both were empty. No report execution or Save occurred. The GUI session closed, ADT deleted the report, and an exact-name search returned `[]`. A later Gateway Client test verified `X-Session-Token` in the request grid and exposed a dialog leak, fixed in ERR-097. Other locales and header layouts remain unverified.

---

### [ERR-048] Integration Scripts Used a Broken Executable Path

- **Status**: FIXED; RETAINED SU01 SCRIPT VERIFIED LIVE
- **Severity**: Medium / Test reliability
- **Symptom**: Current integration scripts still invoked `build/Release/fairyfly.exe`, which sees zero SAP connections and emits SAP trace mutex errors on this machine. A byte-identical staged copy and a fresh copy under another filename in the same directory both see Bigfox. The tracked SU01 script also lacked a UTF-8 BOM for Windows PowerShell 5.1 and placed global `--output` after the subcommand through `Invoke-Expression`; an older untracked window-management probe had known syntax errors.
- **Fix and initial verification**: Current integration scripts use `build/bin/Release/fairyfly.exe`. The SU01 script has a UTF-8 BOM, invokes the CLI with an argument array and global options before the command, and stops when the requested connection reports backend scripting disabled. PowerShell 5.1 parsed the modified scripts, Python parsed the integration module, and an early read-only SU01 invocation stopped before launch with no `sapshcut` process. A fresh byte-identical copy also restored direct-path attachment; the old file instance's failure remains unexplained.
- **Later live verification**: On 2026-09-26 the retained PowerShell runner used the staged executable and saved Bigfox connection ID 0 to create, read back, change the password of, and delete disposable `ZFFLYU2609D`; final Display confirmed absence. The separate launch-and-disconnect mode remains untested live. See ERR-064 and the current open-work index. The old direct executable instance's Windows failure remains unexplained, though a fresh direct artifact attaches successfully.

---

### [ERR-049] SU01 Script Could Report User Creation Without SAP Confirmation

- **Status**: FALSE-PASS PATH FIXED; SAVE AND READ-BACK VERIFIED LIVE
- **Severity**: Medium / Test reliability
- **Symptom**: After clicking Save, the script counted elements on any readable SAP screen and printed that user creation had completed. An unchanged form, modal dialog, or rejected save could therefore appear as a successful integration test. The helper also did not check a nonzero native CLI exit code when JSON reported success.
- **Fix and initial verification**: The script requires the expected SU01 transaction, no active modal, and a changed status-bar message that contains the requested username and matches a configurable localized save-verb pattern. Its CLI helper checks the native exit code and rejects invalid JSON. PowerShell 5.1 parsed the script with its UTF-8 BOM intact; an early read-only invocation stopped at the backend-scripting preflight before any SAP launch. A focused Claude cross-check agreed on the false-pass and exit-code gaps; its claims that the script lacked a quoted `success` comparison and a structured-error throw were incorrect on inspection.
- **Later live verification**: The retained runner completed SU01 Create and saved-name readback on Bigfox. Its first disposable account, `ZFFLYU2609C`, revealed SAP's fixed-length trailing padding and was removed during cleanup; after ERR-064 fixed the comparison, a second run created `ZFFLYU2609D`, read its name back, changed its password, deleted it, and confirmed absence. The English Bigfox save wording and current control IDs are verified; other locales remain untested.

---

### [ERR-050] SM59 Integration Runner Wrote Raw Screen Captures Into the Repository

- **Status**: FIXED; EXISTING-SESSION SM59 SEQUENCE VERIFIED LIVE
- **Severity**: Medium / Test hygiene
- **Symptom**: `test_integration.py` wrote the current SAP screen as `test_screen_sm59.json` and `.md` in the working directory. These generated files could contain operational data, and the runner had no backend-scripting preflight. Its JSON parser searched for the first `{` in mixed output rather than asking the CLI for clean output.
- **Fix and initial verification**: The runner passes `--log-level off`, parses the complete JSON output, stores screen reads only in memory, and exits before launch when the named connection reports backend scripting disabled. Python syntax validation passed; an early read-only run returned the expected preflight stop with no screen-capture files or `sapshcut` process.
- **Later live verification**: The runner's `--existing-connection-id 0` mode dispatched SM59 and completed JSON and Markdown reads on the logged-in Bigfox session, passing its four checks without writing captures or disconnecting the session. A fresh 2026-09-26 rerun again passed all four checks and skipped disconnect. Its separate launch/login path remains to verify live; see the current open-work index.

---

### [ERR-051] SM59 Runner Could Target or Remove an Unrelated Saved Connection

- **Status**: FIXED; EXISTING-SESSION AND TWO-CONNECTION RUNS VERIFIED LIVE
- **Severity**: Medium / Integration safety
- **Symptom**: The runner continued to `tcode`, screen reads, and `disconnect` after a failed launch. Those commands omitted `--connection`, and `disconnect` without an ID deletes the sole saved connection file, even if the runner did not create it.
- **Fix and initial verification**: The runner records saved connection-file IDs before launch, requires the numeric `connection_file_id` returned by a successful launch, passes it to later commands, and stops immediately if launch fails. It skips disconnect when that ID existed before the run; otherwise it disconnects by explicit ID. Simulated new, pre-existing, and failed-launch paths verified the command sequence; an early read-only preflight stopped before launch when Bigfox reported backend scripting disabled.
- **Later live verification**: With saved Bigfox connection ID 0 already present, `--existing-connection-id 0` completed the SM59 sequence and preserved that connection. A fresh 2026-09-26 rerun passed all four checks and explicitly skipped disconnect. On 2026-09-27, a separate Bigfox login-screen connection already occupied saved ID 0 when the authenticated runner launched ID 1. Its six checks passed, including SM59 JSON/Markdown reads from `/app/con[1]/ses[0]` and targeted closure of ID 1. Saved ID 0 remained `valid=true`, its `/app/con[0]/ses[0]` screen remained readable, and `list` reported exactly one live session. Closing ID 0 then left zero GUI sessions and saved files. A live launch-failure path remains unverified; the simulated failure-path tests cover the runner's early stop.

---

### [ERR-052] Legacy Integration Probes Could Change SAP State or Leave Raw Captures

- **Status**: OBSOLETE PROBES REMOVED; LIVE INTEGRATION SCRIPTS RETAINED
- **Severity**: Medium / Repository and test hygiene
- **Symptom**: An old argument-parsing verifier invoked real `fill` and `click` commands against plausible SAP element paths through `Invoke-Expression`, so running a nominal parse test on an active session could change SAP state. Two EPM diagnostics wrote raw screen JSON into the working directory. The integration README still recommended the parse verifier and named `pwsh`, which is unavailable in this environment.
- **Fix and verification**: Removed those three untracked legacy probes after confirming their references and purpose. The README now offers a read-only `list` command, and the EPM analysis note is explicitly historical with an in-memory CLI readout. Both remaining PowerShell integration scripts parse under Windows PowerShell 5.1. The SUIM tree verifier now requests log-free JSON and stops at a read-only backend-scripting preflight; a local run confirmed that stop without starting a SAP transaction or `sapshcut` process.
- **Later repository check (2026-09-26)**: The SUIM verifier mentioned above had subsequently been removed; the two current PowerShell scripts are the SU01 workflow and its offline mock runner. The current open-work index now reflects that inventory. An ignored, unreferenced 2025 `TEST_RESULTS.md` that still presented old Markdown and screenshot failures as current was removed. Live Gateway Markdown rendered, and an in-memory `screen capture --format base64 --file -` decoded to a valid PNG without leaving a capture artifact.

---

### [ERR-053] Connection Launch Could Report Success Without Saving Its Connection File

- **Status**: FIXED; LIVE LAUNCH AND ATTACH FILE SAVE VERIFIED
- **Severity**: Medium / Connection reliability
- **Symptom**: `ConnectionManager::create_or_update_connection` returned a connection object after an `std::ofstream` open or write failure. Standard streams do not throw on these failures by default, so the launch or attach command could claim success even though a later CLI process could not load the required connection file.
- **Fix and verification**: The save writes to a temporary file in the same directory, checks open/write/close errors, and replaces the target only after a complete write; failures report a `SYSTEM_ERROR` through the CLI. Timestamp updates use the same replacement path. Unit tests verify a bad cache path raises an error, a successful update leaves one valid connection file, and a failed replacement preserves the previous target without leaving a temporary file. The tests use unique temporary paths and remove only their own files. A focused Claude cross-check found that the Win32 error code should be captured immediately after a failed replacement; the code now does so. Its temporary-file-leak claim was contradicted by the catch cleanup and failure test. The Release CLI and unit-test targets built successfully; 73 CTest cases passed with four SAP-dependent skips at the time of the fix. Later native launch and attach saved and reloaded live Bigfox connection files; injected disk-failure behavior remains covered by unit tests.

---

### [OPS-001] Bigfox Server-Side SAP GUI Scripting Enabled Dynamically

- **Status**: RUNNING PARAMETER TRUE; NEW GUI SESSION SCRIPTABLE; PROFILE WRITE BLOCKED
- **Observation**: On 2026-09-26, authenticated ADT access to Bigfox returned `sapgui/user_scripting=FALSE`. A temporary `$TMP` class used `CL_SPFL_PROFILE_PARAMETER=>CHANGE_VALUE` to set `TRUE`; it returned code 0 and a subsequent read returned `TRUE`. The class was deleted successfully. A new native SAP GUI connection reported `DisabledByServer=false` and exposed its login form through Fairyfly; the original connection still reported `true`, consistent with [SAP's note](https://help.sap.com/saphelp_gbt10/helpdata/en/49/38dea6c657200be10000000a42189c/content.htm?no_cache=true) that running sessions do not pick up a server-side scripting change.
- **Persistence attempt**: A fresh ADT read found the running value `TRUE` with origin `3` (dynamic change). `DIR_PROFILE` was `/usr/sap/A4H/SYS/profile`; the active file `A4H_D00_vhcala4hci` was readable and contained no scripting entry. `CL_SPFL_PROFILE_PARAMETER=>SET_MULTI_PARA_IN_FILE_AND_DB` was called for that file with its backup option and returned `302` (profile file open failure during write), with no reported parameter errors. A fresh file read still found no scripting entry; runtime remained `TRUE`. The temporary ADT class was deleted. A previous note's `.profile` suffix did not match an existing file on this instance.
- **Next verification**: Have the SAP host administrator enable writing to the instance profile through normal system administration, then repeat the backed-up profile write and read it back. The dynamic parameter may revert on restart. New scripting-enabled GUI sessions have since completed live SU01, SM59, and SE11 workflows; see `docs/OPEN_WORK.md` for the current results.

---

### [ERR-054] A Second SAP Connection Was Saved Under the Wrong Session ID

- **Status**: FIXED AND VERIFIED WITH TWO LIVE BIGFOX CONNECTIONS
- **Severity**: High / Multi-connection correctness
- **Symptom**: With an older disabled Bigfox connection at `/app/con[0]`, native launch opened `/app/con[1]/ses[0]` but returned and saved `/app/con[0]/ses[0]`. The raw COM ID was already zero-based; `normalize_sap_path` incorrectly decremented its connection index. The resulting saved connection failed the next command's validation.
- **Fix and verification**: Launch and attach now keep the raw COM ID. A subsequent native launch returned `/app/con[2]/ses[0]` in both its trace and saved connection file; `connections` marked it valid. The erroneous file was removed after validation. A later live check used a logged-in Bigfox `/app/con[0]/ses[0]` and a second Bigfox login screen `/app/con[1]/ses[0]` with separate saved IDs. Each screen read returned its own transaction state, and closing the second left the first readable. Both were then closed.

---

### [ERR-055] Commands Validated One Session Then Used Another

- **Status**: FIXED AND VERIFIED WITH MIXED LIVE LOGIN STATES
- **Severity**: High / Multi-connection correctness
- **Symptom**: A saved connection pointed to a live new session, but `screen read --connection 0` returned `No sessions available in connection` because each fresh process initialized its engine with the older `/app/con[0]` and never bound the validated saved session.
- **Fix and verification**: Connection resolution now selects the exact saved session before issuing a command. An initial live read of `/app/con[2]/ses[0]` returned the login form and redacted the password field; CLI password fill also returned `[REDACTED]`. Launch and attach metadata now comes from the selected connection rather than the first entry. `doctor` reports a warning when an accessible session coexists with a disabled older connection. The Release build and all 73 CTest cases passed, with four SAP-dependent skips at the time of the fix. A later live check held a logged-in Bigfox session and a second login-screen session simultaneously: connection-scoped reads returned `SESSION_MANAGER` and `S000` respectively, and closing the second left the first readable. Both sessions were then closed.

---

### [OPS-002] Disposable CDS API Published and Read Through OData V2

- **Status**: LIVE WORKFLOW VERIFIED; REPOSITORY OBJECTS CLEANED UP
- **Observation**: An authenticated ADT run created and activated `ZFFLY_CDS_260926` over `SFLIGHT`, exposed it through `ZFFLY_SRV_260926`, and activated `ZFFLY_BIND_260926` as an OData V2 binding in `$TMP`. The installed `erpl-adt` CLI created the CDS object but rejected `SRVD/SRV` as an unknown create type, so the service definition and binding were created through Bigfox's ADT REST collections using the formats returned by discovery. Source write and activation then succeeded through `erpl-adt`. Publishing through Bigfox's advertised `odatav2/publishjobs` endpoint returned `SEVERITY=OK`; the binding reported `published=true`. The service document and `$metadata` returned HTTP 200, the latter exposed `Flights`, and `Flights?$top=1&$format=json` returned HTTP 200 with one row and `Carrier`, `ConnectionId`, `FlightDate`, `Price`, and `Currency` fields.
- **Cleanup and follow-up**: Unpublishing returned HTTP 200 and a success message. The binding, service definition, and CDS view were deleted in reverse dependency order; exact-name ADT searches for `SRVB`, `SRVD`, and `DDLS` each returned zero matches. A separate authenticated read of Gateway V2 `ServiceCollection` returned all 64 entries with no `ZFFLY_BIND_260926` registration and no next page. The old metadata URL returned HTTP 403 afterward. [SAP describes](https://learning.sap.com/courses/building-odata-services-with-sap-gateway/defining-business-services) local OData V2 publication as a Gateway registration for testing. GUI-driven repetition and a write-capable disposable API remain future checks.

---

### [OPS-003] Disposable Data-Dictionary Table Edited and Removed

- **Status**: LIVE ADT AND FAIRYFLY SE11 EDIT/ACTIVATE/READ/DELETE VERIFIED
- **Observation**: `uvx erpl-adt` created `ZFFLY_TAB_260926` in `$TMP` as a transparent table. The initial source activated with `client`, `id`, and `note`; the DDIC read returned those fields and their ABAP types. A second source write added `status : abap.char(1)` and activated. A fresh DDIC read returned all four fields, including `status` with type `abap.char(1)`.
- **GUI verification and cleanup**: A second disposable table, `ZFFLYG260926`, was created and activated through ADT with `CLIENT`, `ID`, and `NOTE`; a fresh DDIC read showed those three fields. Fairyfly opened it in SE11 Change mode, entered `STATUS` in the next native table-control row, assigned data element `BOOLE_D`, and ran Check, Save, and Activate. SAP reported `No inconsistencies found` and `Object activated`; a fresh ADT DDIC read showed the fourth field `status` with type `boole_d`. Fairyfly left Change mode, ADT deleted the table, and an exact-name search returned `[]`. The first disposable table was likewise deleted after its ADT edit. On 2026-09-27, a separate disposable table and ADT console class inserted and read back one row before a planned data-bearing adjustment. The requested GUI logoff interrupted that attempt before the schema changed; ADT removed its row and objects. A subsequent disposable `ZFFLYADJ2_260927` test persisted `TEST0001` before Fairyfly SE11 added `STATUS`/`BOOLE_D`; SAP reported `No inconsistencies found` and `Object activated`. A fresh ADT source read contained `status : boole_d`, and the class still found the original `note=before adjustment` row with `before=1`. Cleanup deleted the row (`remaining=0`), class, and table; exact-name searches returned `[]`, and the GUI session was closed.

---

### [OPS-004] Disposable User Created, Password Changed, and Deleted

- **Status**: LIVE BAPI AND GUI SU01 WORKFLOWS VERIFIED; DISPOSABLE USERS REMOVED
- **Observation**: The first ADT class run for `ZFFLYU2609A` returned HTTP 500. The newest SAP short dump was `PERFORM_CONFLICT_TAB_TYPE` in `SAPLSU_USER`, parameter 40 of `FORM USER_CREATE`, which corresponded to the test class's `RETURN` table. A read-only probe confirmed no account had been created. The class had declared `STANDARD TABLE OF BAPIRET2 WITH EMPTY KEY`; changing that declaration to SAP's `BAPIRETTAB` removed the incompatibility.
- **Verification and cleanup**: The corrected class called `BAPI_USER_CREATE1` with a generated initial password, committed, and read back the account. It then called `BAPI_USER_CHANGE` with a second generated password and `BAPIPWDX-BAPIPWD = 'X'`, committed, and called `BAPI_USER_DELETE` followed by another commit. All three BAPIs returned zero `E` or `A` messages; the in-run and a separate later read-only probe both found the user absent after deletion. The temporary ADT class and local source files were removed. A later Fairyfly GUI run created disposable `ZFFLYU2609B` in SU01 with name fields and an initial password; SAP reported `User ZFFLYU2609B created`. Reopening Display showed the saved first and last names and `Saved` status. Fairyfly changed its password in the SU01 modal and SAP reported `The password was changed`. Fairyfly then confirmed deletion; SAP reported `User ZFFLYU2609B deleted`, and a subsequent Display attempt returned `ACTION_FAILED` with `User ZFFLYU2609B does not exist`. Passwords were generated in memory and not printed or stored. A later disposable user authenticated with its changed password, completed SAP's forced first-login password change, and was deleted; see ERR-086.

---

### [OPS-005] Disposable HTTP Destination Created, Updated, Tested, and Removed

- **Status**: LIVE BACKEND AND GUI SM59 WORKFLOWS VERIFIED; DISPOSABLE DESTINATIONS REMOVED
- **Observation**: An authenticated ADT class run created `ZFFLY_HTTP_260926` as a type-G HTTP destination to Bigfox port 50000, with `/sap/bc/adt/discovery` as its path. `RFC_READ_HTTP_DESTINATION` returned type `G`, host `bigfox`, port `50000`, and the initial path. `RFC_MODIFY_HTTP_DEST_TO_EXT` changed the path to `/sap/public/ping`; a fresh read returned that new path. `CL_HTTP_CLIENT` created a client from the destination and both send and receive returned success; the endpoint returned HTTP 403, matching an unauthenticated request to that endpoint from Windows. This verifies network transport, not application authorization.
- **GUI verification**: Fairyfly used SM59 to create `ZFFLYGUI260926` as type G. It saved host `bigfox`, port `50000`, and path `/sap/bc/adt/discovery`; a fresh SM59 search and Display read back all values. Fairyfly switched to Change, saved path `/sap/public/ping`, and a second fresh search read it back. SM59 Connection Test displayed HTTP `403 Forbidden` and a 17 ms test call duration, consistent with the deliberately unauthenticated endpoint. The test warning correctly noted that HTTP results depend on the called service. Fairyfly deleted the destination, refreshed the tree, and expanded the type-G group; the name was absent.
- **Cleanup and follow-up**: The backend `RFC_PREPARE_DESTINATION` call for the earlier destination returned zero; a separate read-only probe found no `RFCDES` row. The temporary class and local source files were removed. An authenticated endpoint response remains to test if application-level success is needed.

---

### [ERR-056] Saved Positional Session Path Could Bind a Different SAP Logon

- **Status**: LIVE KEY MIGRATION, WRONG-KEY REJECTION, AND PATH REUSE VERIFIED
- **Severity**: High / Multi-connection correctness
- **Symptom**: A saved `/app/con[N]/ses[M]` path alone does not establish identity after SAP GUI closes or reorders connections. A new session at the same path could be accepted by `select_session` or shown as valid by `connections`.
- **Fix and verification**: Connection files now store SAP's `SystemSessionId`/`SessionNumber` key when available. Selection, validation, and cleanup require the key to match; creating a connection at a reused path with a different key creates a separate file. Old JSON files still load and gain a key after successful selection. Live `list` reported that the new Bigfox login-screen session exposes a server key. An isolated keyless cache file read the live login screen successfully, persisted a nonempty server key, and succeeded on a second CLI read; the password field was redacted. Replacing the saved key with a deliberately wrong one made `screen read` return `INVALID_CONNECTION` and delete only that test cache file. Release build and 77 CTest cases passed with four SAP-dependent skips at the time of the fix. In a later live path-reuse check, two successive Bigfox login-screen connections both occupied `/app/con[0]/ses[0]`; the stale keyed cache entry returned `INVALID_CONNECTION` and was deleted, while the new connection remained readable. Both test sessions were closed. Sessions that expose no key and old files awaiting upgrade remain positionally identified.

---

### [ERR-057] Response Editor Could Export Common Credentials as Raw Text

- **Status**: COMMON JSON, XML, HTTP HEADER, AND PLAIN-TEXT ASSIGNMENT FORMS FIXED IN CODE; OTHER FORMATS OPEN
- **Severity**: Medium / Credential output
- **Symptom**: `GuiTextedit` and `GuiShell` response text was returned verbatim by `get` and screen reads, so a JSON `access_token` or an embedded `Authorization`/`Set-Cookie` header could appear in CLI output even though Gateway header grids were already redacted.
- **Fix and verification**: The shared response-text filter masks credential-named values in valid JSON, including nested objects and arrays, plus generic values paired with a sensitive `Name`, `HeaderName`, `Key`, or `FieldName`. It also masks named plain-text assignments such as `access_token=...`, YAML-like `client-secret: ...`, and authorization/cookie header lines while preserving ordinary response lines. When recognizable credential keys or sensitive generic labels remain in malformed JSON, or credential names appear as XML element or attribute names, it suppresses the entire response because partial rewriting would be unsafe. The COM text accessor and both AbapEditor metadata paths use it. Focused tests check credential removal and preservation of ordinary JSON, XML, `Content-Type`, and plain-text status lines, including a truncated `Name=Authorization` JSON response; the Release unit-test build and all 81 CTest cases passed with five SAP-dependent skips at the time of this fix. Later live Gateway Client URI checks covered query and fragment credential parameters; see ERR-080 and ERR-089. Unlabelled secrets and nonstandard formats remain open.

---

### [OPS-006] Obsolete SAP GUI Probes and Stale Notes Removed

- **Status**: CLEANUP VERIFIED
- **Observation**: The integration directory held 22 untracked one-off probes and 18 tracked VBScript screen probes. None was registered as a test; the only references were an obsolete diagnostic-only CMake target and a historical toolbar note. A second unreferenced note still marked tree navigation unsupported even though the current CLI handles tree actions.
- **Cleanup and verification**: The probes and both outdated notes were removed, along with the `probe_sap_rot` CMake target. The integration README now describes the retained Python SM59 runner and PowerShell SU01 script. Repository search found no remaining live build or test references to the removed probes; CMake regenerated successfully, the Release CLI and unit tests built, and all 75 CTest cases passed with four SAP-dependent skips.

---

### [OPS-007] Core Incremental Build Measured and Response Filter Isolated

- **Status**: FILTER-SOURCE OPTIMIZATION VERIFIED; BROADER BUILD PROFILE OPEN
- **Observation**: On this machine, a no-op Release `fairyfly` build took 1.60 s while touching only `com_automation_engine.cpp` produced a 14.07 s compile-and-relink cycle. The new response-text filter lived in a shared header and caused several large source files to rebuild whenever its implementation changed.
- **Change and verification**: The filter implementation now lives in `src/sensitive_data.cpp`, which is compiled into `fairyfly_core`; the header exposes its declaration without including `<regex>`. After the one-time CMake regeneration and rebuild, touching only this source compiled it once and relinked `fairyfly` in 7.19 s. A subsequent no-op took 1.50 s and compiled no source files. Release CLI and unit tests built, all 77 CTest cases passed with four SAP-dependent skips, and the temporary timestamp changes used for measurement were restored.

---

### [ERR-058] Doctor Reported a PSAPI Count Without Running PSAPI

- **Status**: FIXED AND VERIFIED LIVE
- **Severity**: Low / Diagnostic accuracy
- **Symptom**: With a live SAP Logon process found through Toolhelp, `doctor` reported `PSAPI saw 0`, which looked like an independent enumeration failure. The PSAPI fallback had not run because Toolhelp already found SAP GUI.
- **Fix and verification**: The passing process check now says `PSAPI not run (Toolhelp found SAP GUI)` when the fallback is skipped. Live `doctor` reported one SAP Logon process, 152 Toolhelp entries, the corrected PSAPI status, and an overall warning only for the older scripting-disabled connection. Release build and all 77 CTest cases passed with four SAP-dependent skips.

---

### [ERR-059] Concurrent Cache Creators Could Overwrite the Same Connection ID

- **Status**: CROSS-PROCESS CACHE TEST AND TWO LIVE CONCURRENT GUI LAUNCHES VERIFIED
- **Severity**: High / Connection reliability
- **Symptom**: Two CLI processes could both scan the cache, choose the same next integer ID, and replace the same `fairyfly.N.con` file. Atomic file replacement protected each write from partial data but did not make the ID selection atomic.
- **Fix and verification**: Cache creation, key upgrades, timestamp writes, and deletion use a named Windows mutex derived from the normalized cache directory. The lock covers the ID scan through file replacement and is released by the OS if a process terminates; [Microsoft documents](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createmutexw) named mutex sharing across processes. An automated test launched 12 separate worker processes behind a start gate into one isolated directory, then verified all IDs and server keys were distinct and all cache generations were present. It passed 20 consecutive runs and left no temporary directories. The Release build and all 80 CTest cases passed with five SAP-dependent skips at the time of the fix. On 2026-09-27, two concurrent `fairyfly launch Bigfox` processes both succeeded: cache ID 0 selected `/app/con[1]/ses[0]` and ID 1 selected `/app/con[0]/ses[0]`. Each saved entry validated and read its own screen. Closing ID 0 left ID 1 valid and readable; closing ID 1 then left zero GUI sessions and saved files. Same-path selection during a race and access to one cache from different Windows logon sessions remain unverified.

---

### [ERR-060] Stale Cache Cleanup Could Delete a Replacement Connection

- **Status**: FIXED IN CODE; REPLACEMENT RACE COVERED BY ISOLATED REGRESSION
- **Severity**: High / Connection reliability
- **Symptom**: `connections --cleanup` validated a snapshot and later deleted by numeric file ID. If another process removed that file and created a new connection at the same ID before deletion, cleanup could delete the new file. Invalid-session resolution had the same race.
- **Fix and verification**: New cache files receive a random generation ID. Cleanup and invalid-session resolution now delete only when the current file still has the validated generation and session identity; legacy files without a generation must match their complete saved contents. Timestamp writes and server-key upgrades also require the selected snapshot to match, so a replaced file cannot be modified after validation. A focused test replaced an invalid snapshot with a new file at the same ID and confirmed cleanup removed zero files, both stale update paths raised `SystemError`, and the replacement survived. Release build and all 79 CTest cases passed with five SAP-dependent skips. The extra skip was `ComGuiConnection properties`: a separate read-only `list` confirmed SAP GUI currently has zero open connections. Actual cross-process replacement during live GUI validation remains to verify.

---

### [OPS-008] Unreferenced Screenshots and Copied Reference Dump Removed

- **Status**: CLEANUP VERIFIED; EPM REPORT RETEST OPEN
- **Observation**: Seven untracked screenshots under `docs/` had no Markdown references. Two 2025 SAP scripting/auto-login notes under `ai/` were likewise unreferenced, and a 334 KB third-party TOON text dump duplicated an external source without any repository consumer. A separate EPM report analysis still described a possible missing Markdown output issue.
- **Cleanup and verification**: The ten unreferenced files were removed. The EPM note was retained, indexed in current open work, and updated to use fresh CLI reads instead of its deleted VBScript probe. The documentation map no longer claims screenshots remain. Repository reference searches found no consumers of the removed files; build behavior was unchanged.
- **2026-09-27 follow-up**: The retained EPM note still contained speculative code edits and references to an unavailable screenshot. Two disposable classic-list reports have since rendered correctly, but the completed `SEPM_REF_APPS_DG` result remains unverified. A read-only SP01 title search found no retained `*SEPM*` spool. The speculative note was removed; the exact remaining verification is tracked in `OPEN_WORK.md`.
- **Later source-shaped verification**: A third disposable report reproduced the generator class's `WRITE /`, `WRITE /4`, and `SKIP 2` layout. ERR-126 fixed Markdown spacing and indentation on that live output. The historical generator result remains unavailable for an exact comparison.

---

### [ERR-061] Legacy Connection Files Could Survive Disconnect and Reappear

- **Status**: FIXED AND VERIFIED WITH LIVE SAP GUI; ISOLATED ARTIFACTS REMOVED
- **Severity**: Medium / Connection reliability
- **Symptom**: Fairyfly reads older `fairyfly.N.con` files from the current directory, but updates and deletion used only the newer cache directory. A legacy file could survive `connections --cleanup` or a disconnect, and an update could create a second file with the same ID in the new cache. New IDs also ignored legacy IDs, allowing collisions.
- **Fix and verification**: Mutations now use the existing file's location, and ID allocation scans both locations. On manager startup, a pre-existing duplicate ID is migrated under the cache lock: the legacy file is written with the next unused ID and a new cache generation before its old file is removed. If migration fails, startup reports the error and leaves the original file rather than silently shadowing it. A red-green regression test reproduced the shadowed file (`1` listed instead of `2`) and then verified both distinct sessions remain loadable and removable. The Release CLI and unit-test targets built and all 84 CTest cases passed. In a separate isolated cache with the live Bigfox session, Fairyfly read SAP Easy Access through a legacy file, updated that file in place without a primary duplicate, and removed a wrong-key legacy file while preserving the valid one. A live duplicate-ID fixture migrated the old file to ID 4 while retaining ID 3; Fairyfly read `SESSION_MANAGER` successfully through both IDs. The temporary fixture files were removed.

---

### [ERR-062] Sparse SAP Connection IDs Broke Saved Session Selection

- **Status**: FIXED AND VERIFIED LIVE FOR CONNECTION AND CHILD-SESSION SPARSITY
- **Severity**: High / Live session reliability
- **Symptom**: With `/app/con[0]` gone, `attach` saved the open `/app/con[1]/ses[0]` as cache ID 0. Immediate `screen read --connection 0 --no-tabs` returned `INVALID_CONNECTION` and deleted that file even though `list` still showed the SAP Easy Access session. The selector treated the numeric suffix in SAP's object ID as the object's current COM collection position.
- **Fix and verification**: Connection and session lookup search collection items by their reported SAP IDs before applying the saved server-session key. A sparse-index regression failed before the fix (`/app/con[1]` at position 0 was missed), then passed after the fix. The rebuilt CLI attached to `/app/con[1]/ses[0]` and read SAP Easy Access successfully. On 2026-09-27, a live child-session test created `ses[0]`, `ses[1]`, and `ses[2]` under one connection, then closed `ses[1]`. `list` still reported `ses[0]` and `ses[2]`; the saved `ses[2]` entry read its exact path. Detaching and reattaching `ses[2]` by exact ID also read `SESSION_MANAGER`. All test sessions and saved entries were removed.

---

### [ERR-063] Read-Only SAP Fields Returned Generic Fill Exceptions

- **Status**: FIXED AND VERIFIED LIVE
- **Severity**: Medium / Action diagnostics
- **Symptom**: Filling an SE11 field with SAP `Changeable=False` attempted a COM Text write and returned the generic `EXCEPTION Failed to set property`.
- **Fix and verification**: `fill` checks the element's `Changeable` property before writing and returns `ELEMENT_READ_ONLY` without a Text PUT. A red regression reproduced the old error; green tests cover read-only, writable, and missing-property controls. On live SE11 Display of `TADIR`, filling its read-only table-name field returned `ELEMENT_READ_ONLY`, and a fresh read still showed `TADIR`. The Release build and all 87 CTest cases passed.
- **Native table-cell follow-up (2026-09-26)**: A focused live SAP GUI wrapper inspection resolved SE11's first `GuiTableControl` field cell as `txtDD03D-FIELDNAME[0,0]`. Fairyfly `get` read `PGMID`; `fill` of that exact cell returned `ELEMENT_READ_ONLY`; a new `get` still read `PGMID`. The original fix therefore also covers a native table cell in Display mode.

---

### [ERR-064] SU01 Runner Rejected Saved Names With SAP Field Padding

- **Status**: FIXED AND VERIFIED LIVE
- **Severity**: Low / Integration-test accuracy
- **Symptom**: The first live SU01 runner created `ZFFLYU2609C`, then failed its saved-name readback because SAP returned fixed-length field values with trailing spaces. Its `finally` cleanup deleted the disposable user; a later Display confirmed absence.
- **Fix and verification**: The runner trims trailing padding from returned first and last names before comparing them with the requested values. An offline mock with padded names failed before the fix and passed after it. A subsequent live run created `ZFFLYU2609D`, read back its saved name, changed its password, deleted the user, and confirmed absence. The existing logged-in connection remained open.

---

### [OPS-009] Disposable OData V2 Service Published and Removed Through Fairyfly GUI

- **Status**: LIVE PUBLICATION AND SIX-LAYER GUI CLEANUP VERIFIED
- **Observation**: Fairyfly created SEGW project `ZFFLY260926B` as a local object, added `Flight`/`Flights`, and set property `ID` to key type `Edm.String` with max length 20. SEGW Check passed, then Generate registered technical service `ZFFLY260926B_SRV`, model `ZFFLY260926B_MDL`, and the four classes `ZCL_ZFFLY260926B_{MPC,MPC_EXT,DPC,DPC_EXT}`. Through `/IWFND/MAINT_SERVICE`, Fairyfly found exactly one backend service using alias `LOCAL`, added it with an OData V2 ICF node, and SAP confirmed the service was created and metadata loaded. An authenticated `$metadata` GET returned HTTP 200 and included entity set `Flights`.
- **Cleanup and verification**: Fairyfly deleted the exact SICF leaf; unassigned `LOCAL` and deleted the frontend service; deleted backend service and model; deleted the SEGW project; then deleted all four generated classes in SE24. Fresh SICF search returned `Selection criteria did not select any data`, a fresh frontend catalog filter returned zero rows, backend Display actions said `Service not available` and `Model does not exist`, SEGW removed its project tree node, and SE24 said each exact class did not exist. The endpoint's later HTTP 403 response was not used as cleanup proof.

---

### [ERR-065] SAP Dump After Gateway Service Registration Confirmation

- **Status**: SAP STANDARD-CODE DUMP REPRODUCED IN CONTROLLED GUI WORKFLOW; ROOT CAUSE OPEN
- **Severity**: Medium / Gateway administration
- **Symptom**: After `/IWFND/MAINT_SERVICE` reported `Service 'ZFFLY260926B_SRV' was created and its metadata was loaded successfully`, continuing from that information popup opened an ABAP runtime-error screen. ST22 recorded the new 2026-09-26 16:49:31 UTC dump as `STRING_OFFSET_TOO_LARGE`, exception `CX_SY_RANGE_OUT_OF_BOUNDS`, in `/IWFND/CL_MED_CHECK_UTIL======CP` (include `...CM005`, line 15). The dump occurred after publication: the authenticated metadata endpoint returned HTTP 200 with `Flights` before cleanup.
- **Cause and next check**: After a Fairyfly ST22 rendering fix, the live formatted dump became readable. Its Error analysis says offset `0` exceeded the string's current length `0` in `GET_NAMESPACE_FROM_OBJECT_NAME`. A read-only ADT source read of that SAP standard method shows `lv_object_name(1)` without an empty-string guard. A later live ST22 Selected Variables view confirmed `IV_OBJECT_NAME` and `LV_OBJECT_NAME` were both `<empty string>`. The active call stack shows `/IWFND/R_MGW_REGISTRATION_F03` line 138 (`SET_SCREEN_0300`) directly calling the failing method, after `LCL_ADD_SERVICE=>SELECT_SINGLE_SERVICE` and `HANDLE_HOTSPOT_CLICK`. A later live SE38 source read shows `lv_object_name = gs_screen_0300_box01-service_name` immediately before the failing call; the empty screen field is the input source. Trace why that field was empty after registration confirmation. Whether an applicable SAP correction exists remains open. Do not infer a Fairyfly defect from this SAP-side exception. The service and all generated objects from this run were removed and verified absent.
- **Source-flow follow-up**: An authenticated `uvx erpl-adt` read of the live SAP includes found that `ADD_SERVICE` sets the first-run flag before service creation, displays the success message as type `I`, then calls `LEAVE_SCREEN_0300`. That routine clears the registration-screen structure and sets the first-run flag again. `SET_SCREEN_0300`, called from the screen's PBO module, uses the empty service name in `GET_NAMESPACE_FROM_OBJECT_NAME` whenever that flag is set. [SAP documents](https://help.sap.com/docs/SAP_NETWEAVER_731_BW_ABAP/f68e489816e043f1add91d69a6842931/4a44c8c9c6bf0451e10000000a421937.html?version=7.31.23) that execution resumes after a confirmed type-I message. A reentry to screen 0300 after the clear would therefore explain the dump; why that reentry happened in this run remains unproven. No new service was created for this read-only investigation.
- **Read-only GUI recheck**: A fresh Bigfox ST22 Today list still contained the 16:49:31 UTC `STRING_OFFSET_TOO_LARGE` dump as its first row. Fairyfly selected it and opened Runtime Error Long Text; the method was `GET_NAMESPACE_FROM_OBJECT_NAME`, and the error analysis again showed offset `0` against string length `0`. This verifies the recorded dump remains inspectable through the rebuilt GUI CLI; it does not prove the proposed screen-reentry sequence. The temporary login was ended and its cache entry removed.
- **Selection-path follow-up**: A further authenticated `uvx erpl-adt` read found `LCL_ADD_SERVICE=>SELECT_SINGLE_SERVICE` in the local-class include. It reads `gt_add_services` by the selected grid row number, does not check `sy-subrc`, then copies the result into the screen fields and calls screen 0300 if the model-name field is nonempty. Successful `ADD_SERVICE` removes the service from `gt_add_services`; `LEAVE_SCREEN_0300` clears boxes 01–03 but leaves box 04 (model name) intact. If the hotspot action is repeated with a now-missing row, the local service structure would be initial while a stale model name could pass the guard, leading to the observed empty-name PBO call. This is a source-supported hypothesis, not proof of a repeated GUI event. A fresh event trace or controlled disposable-service repetition must establish whether this route occurred; SAP standard code was not changed.
- **ST22 caller-variable follow-up**: A further live ST22 inspection found `OK_CODE=CONTINUE`, success message number `320` with `SY-MSGV1=ZFFLY260926B_SRV`, `GS_SCREEN_0300_BOX01` initial, and `IV_ROW_ID=1` in `SELECT_SINGLE_SERVICE`. The `HANDLE_HOTSPOT_CLICK` frame identified column `SERVICE_NAME` and row 1. These values tie the dump to the published service and the service-name hotspot, but the event frame could be the original hotspot call still on the stack; it does not establish a second click or a failed row read. A controlled reproduction with an event trace is still needed to distinguish the reentry paths. The read-only inspection session was closed, and live `list` and saved connections both returned zero.
- **Controlled GUI repetition (2026-09-27)**: Fairyfly created disposable SEGW project `ZFFLY260927C`, defined `Flight`/`Flights` with key property `ID` (`Edm.String`, length 20), checked it, and generated technical service `ZFFLY260927C_SRV`, model `ZFFLY260927C_MDL`, and the four exact `ZCL_ZFFLY260927C_{DPC,DPC_EXT,MPC,MPC_EXT}` classes. In `/IWFND/MAINT_SERVICE`, it selected the sole matching backend service through the `SERVICE_NAME` grid hotspot, chose Gateway OData V2, and continued the information message confirming service creation and metadata load. This time SAP returned normally to “Add Selected Services”; a fresh frontend catalog showed the service and its `LOCAL` alias. ST22's 26 September overview still counted one `STRING_OFFSET_TOO_LARGE` dump, the earlier entry, with no new dump from this run. A single comparable repetition therefore does not establish repeatability or a Fairyfly defect. A captured event/PBO trace would be useful if it recurs.
- **Repetition cleanup**: The exact SICF leaf was deleted and a fresh search returned `Selection criteria did not select any data`; the `LOCAL` alias was unassigned and the frontend catalog fell from 65 to 64 rows with no disposable service. `/IWBEP/REG_SERVICE` then returned `Service not available`, and `/IWBEP/REG_MODEL` returned `Model does not exist` for version 1. SEGW reported the project deleted and removed its tree node. SE24 deleted extension classes before their bases; fresh Display attempts for all four exact classes returned `does not exist`. The temporary SAP GUI session was closed; `list` and `connections` both returned zero.
- **Second controlled repetition (2026-09-27)**: Fairyfly created local SEGW project `ZFFLY260927D` with `Flight`/`Flights` and generated service `ZFFLY260927D_SRV`, model `ZFFLY260927D_MDL`, and four classes. In `/IWFND/MAINT_SERVICE`, it selected the exact backend GridView row, assigned `LOCAL` and `$TMP`, chose OData V2, and continued the information message that the service and metadata were created. SAP then dumped at 05:17:21 UTC with `STRING_OFFSET_TOO_LARGE` in `/IWFND/CL_MED_CHECK_UTIL======CP`, method `GET_NAMESPACE_FROM_OBJECT_NAME`, matching the earlier failure. This establishes a repeated SAP-side failure in the controlled publication flow, but the differing result from project `C` does not yet identify the trigger. Capture an event/PBO trace on a future disposable run before attributing the empty service name to any one interaction.
- **Second repetition cleanup**: SICF showed alternate leaf name `zffly260927d_srv` but its service detail identified original ICF name `A4H001000000013`, created 27 September by `DEVELOPER`; its delete prompt used that original name. Fairyfly deleted that verified leaf, unassigned `LOCAL`, removed the frontend service through the filtered GridView, and deleted the exact backend service and model. Fresh backend Display attempts returned `Service not available` and `Model does not exist`; SEGW removed the project node, and SE24 Display returned `does not exist` for all four generated classes. The GUI session and saved connection were closed, and `list` and `connections` both reported zero.

---

### [ERR-066] ST22 Long Text Rendered as a Blank Table

- **Status**: FIXED AND VERIFIED LIVE FOR THE OBSERVED REPORT
- **Severity**: Medium / Screen extraction
- **Symptom**: ST22 visibly showed a dump report, but Fairyfly classified its `GuiUserArea` labels as a 35-row table with four columns and empty cells; Markdown showed only `_empty_` rows. The unformatted view similarly became 52 blank rows.
- **Fix and verification**: A red-green regression distinguished report labels, whose populated columns vary by row, from a conventional aligned table such as SE16. The reader now preserves those labels, and the Markdown formatter renders their positioned values under `Report Output`. On the rebuilt live CLI, both formatted and unformatted ST22 screens exposed `STRING_OFFSET_TOO_LARGE`, the SAP program, and the offset/length explanation. A second red-green regression masks an entire positioned report row in JSON when its label names a credential; ordinary dump rows remain visible. The Release build and all 90 CTest cases passed. Arbitrary unlabeled secrets and other report layouts remain to verify.

---

### [ERR-067] Rejected Transaction Reported Success When SAP Opened an Information Dialog

- **Status**: FIXED AND VERIFIED LIVE FOR THE OBSERVED DIALOG
- **Severity**: Medium / Transaction diagnostics
- **Symptom**: From SM59, `tcode SESSION_MANAGER` returned `success` and `Transaction started` even though SAP displayed `Cannot start transaction SESSION_MANAGER` in `wnd[1]/usr/txtMESSTXT1`. SAP had already set its transaction-code property to `SESSION_MANAGER`, so the existing status-bar and transaction-code checks both passed.
- **Fix and verification**: A red regression captured the modal rejection and excluded unrelated information dialogs. After the fix, the rebuilt CLI returned `TRANSACTION_FAILED` with SAP's message on the same live SM59 path. The dialog was dismissed, normal SM59 navigation succeeded, and all 91 CTest cases passed. The recognized wording is based on the English Bigfox dialog; other languages and rejection variants remain to verify.

---

### [ERR-068] Screen Read Filter Left JSON Elements Unfiltered

- **Status**: FIXED AND VERIFIED LIVE FOR NESTED BUTTONS; MIXED-CHILD DEFECT COVERED BY REGRESSION
- **Severity**: Medium / Result integrity
- **Symptom**: On SE16's 200-hit TADIR result, `screen read --only-buttons` retained all 101 mixed-type entries in `elements` and reported `element_count=101`, while only the separate `hierarchy` was filtered. Its flat filtered hierarchy also left the Markdown formatter with no button rows. The recursive collector could stop exploring children after the first match even without `--first`.
- **Fix and verification**: A red unit regression checks two buttons separated by a field across `elements`, `hierarchy`, and `element_count`; further red assertions cover categorized Markdown hierarchy and nested `tabs_content`. Filtering now updates those views together, preserves category groups, and stops after one match only with `--first`. The rebuilt CLI returned 28 `GuiButton` elements in JSON on the same live SE16 screen, rendered the buttons in Markdown, and returned one element with `--first`. On live SE11 Display of TADIR, filtered top-level and six expanded-tab element arrays contained no non-buttons. All 92 CTest cases passed.
- **Mixed-child follow-up (2026-09-26)**: The recursive filter skipped an entire `children` array when its first entry was a string ID, even if later entries were full element objects. A regression with a string followed by a button failed with zero matches before the fix and passed with one afterward. String references no longer count as checked elements. On live Gateway service maintenance, combined button and ID filters returned 26 unique `GuiButton` objects with a matching `element_count`; combined button and text filters rendered the matching nested toolbar buttons in Markdown. All 100 CTest cases passed. The exact mixed-array shape has not yet appeared in a live SAP screen.

---

### [ERR-069] Missing Elements Bypassed Window-Mismatch Diagnostics

- **Status**: FIXED AND VERIFIED LIVE
- **Severity**: Low / Action diagnostics
- **Symptom**: On SE11 Display of TADIR, a full canonical `click` path targeting inactive `wnd[1]` returned generic `COM_ERROR` even though `click` contained an `ELEMENT_NOT_FOUND` branch with active-window suggestions. `ComGuiSession::find_element_by_id` threw on absence, so the branch could not run. The same issue affected `fill`.
- **Fix and verification**: A red unit regression covers the wrapper's missing-element error text and comparison of short and canonical window IDs. `click` and `fill` now convert only that known absence into a null lookup and preserve other COM exceptions; window comparison recognizes a short `wnd[0]` against its canonical active ID. The rebuilt CLI reported `ELEMENT_NOT_FOUND` with `window_mismatch: true`, active window, and suggestions for both full-path commands. A missing short path in active `wnd[0]` returned no mismatch flag. All 93 CTest cases passed.

---

### [OPS-010] Grid Viewport Metadata Verified Against Live Scrolling

- **Status**: VERTICAL AND HORIZONTAL VIEWPORT VERIFIED LIVE
- **Observation**: [SAP's GuiGridView scripting documentation](https://help.sap.com/docs/help/b47d018c3b9b45e897faf66a6c0885a8/4af24c3281fb4d6a809e53238562d3b2.html) defines `FirstVisibleColumn` and `FirstVisibleRow`. A red-green fake-COM regression added these read-only viewport fields plus current-cell coordinates to Fairyfly's grid JSON. On live `/IWFND/MAINT_SERVICE`, the initial service grid reported first visible column `TYPE`, first visible row 0, and current cell `SERVICE_NAME` row 0. Selecting row 40 in `PROCESS_MODE_TEXT` changed first visible row to 16 and current cell to that column and row; `screen read --max-rows 64` still returned all nine columns and the unchanged `ZSEPMRA_SHOP` row. First visible column remained `TYPE`, so this run does not prove horizontal scrolling. The selection was reset to row 0, all 96 CTest cases passed, and the existing Bigfox session remained open.
- **Horizontal follow-up (2026-09-26)**: SM50's 17-column work-process grid was displayed in a temporarily narrowed 650-pixel SAP GUI window. Selecting row 0's far-right `ACTION_INFO` column moved `first_visible_column` from `WP_TYPE_DISP` to `TENANT_DISP`. A fresh `screen read --max-rows 32` retained the same 17 ordered headers, all 32 rows, and 17 cells per row; the first three cells of all 32 rows matched the pre-scroll read. The original 2346-pixel window width was restored. SAP process values can change independently of scrolling, so the stable identifier cells were used for this comparison.

---

### [ERR-070] Positioned-Label Tables Ignored `--max-rows`

- **Status**: FIXED AND VERIFIED LIVE
- **Severity**: Medium / Screen-read response size
- **Symptom**: On live SM37 Job Overview, `screen read --no-tabs --max-rows 1` returned all 48 rows from the positioned-label `GuiUserArea` table. The default likewise returned 48 despite its 20-row limit.
- **Fix and verification**: A red regression for limiting the returned rows while retaining the full count failed at link time before implementation. The extraction path now applies the requested row cap after counting and redacting the table. The rebuilt CLI returned 1, 20, and 48 rows for limits 1, default, and 48 respectively; `total_row_count` and `visible_row_count` remained 48 in each case. All 97 CTest cases passed. The Bigfox session remained on the read-only SM37 Job Overview.

---

### [ERR-071] Expanded SUIM Tree Lost All Node Labels

- **Status**: FIXED AND VERIFIED LIVE ON SUIM
- **Severity**: Medium / Tree extraction
- **Symptom**: On live SUIM, the initial tree returned 11 named nodes. After Fairyfly expanded `User`, the SAP GUI visibly showed 19 labeled nodes, but `screen read` and `get --list-nodes` returned 19 nodes with empty text. `GetNodeTextByKey` completed successfully with an empty BSTR, so the problem occurred before JSON or Markdown rendering.
- **Fix and verification**: [SAP's GUI scripting API](https://help.sap.com/doc/9215986e54174174854b0af6bb14305a/800.05/en-US/sap_gui_scripting_api.pdf) documents `GetColumnNames` and `GetItemText` for tree items. Live SUIM exposed item columns `1`, `2`, and `TEXT`; `GetItemText` on `TEXT` returned the visible node label. A red regression for recovering that label failed before the shared fallback was implemented. The reader, tree extractor, and `get --list-nodes` now use it only when `GetNodeTextByKey` is empty. The rebuilt CLI returned all 19 labels in expanded SUIM JSON, Markdown, and `get --list-nodes`; a fresh collapsed tree still returned all 11 labels. All 101 CTest cases passed.
- **Further live coverage**: Expanding `Roles` after `User` produced 31 nodes; all 31 had labels in both `screen read` and `get --list-nodes`, and Markdown rendered the branch.

---

### [ERR-072] SUIM List-Tree Double-Click Reported Success Without Navigation

- **Status**: FIXED AND VERIFIED LIVE ON SUIM
- **Severity**: Medium / Tree interaction
- **Symptom**: `click --tree-action doubleclick` on SUIM's `Users by Address Data` leaf returned success from `DoubleClickNode`, but the screen stayed on the same SUIM tree without a status message. The visible label belongs to the tree's `TEXT` item column.
- **Fix and verification**: [SAP's scripting API](https://help.sap.com/doc/9215986e54174174854b0af6bb14305a/800.05/en-US/sap_gui_scripting_api.pdf) distinguishes `DoubleClickNode` from `DoubleClickItem`. A fake-COM regression failed before the change, then confirmed the `TEXT` item receives the double-click while ordinary trees with node text still use `DoubleClickNode`. On live SUIM, the rebuilt CLI opened the `Transaction Code for Reports` modal for `Users by Address Data`, showing report transaction `S_BCE_68001393`. The modal was dismissed through its Enter button, returning to SUIM. All 102 CTest cases passed.

---

### [ERR-073] Input-Field Filter Included Read-Only Text

- **Status**: FIXED AND VERIFIED LIVE ON SUIM
- **Severity**: Low / Screen-read filter accuracy
- **Symptom**: `screen read --only-fields` on live SUIM Users by Address Data returned 28 text fields, including 14 read-only captions, though the option describes input fields.
- **Fix and verification**: A red regression returned four text fields instead of the two changeable inputs. The filter now requires `changeable=true` for text and password fields. The rebuilt CLI returned 14 input fields, all changeable, from the same live SAP screen; all 103 CTest cases passed.

---

### [ERR-074] System-Log Markdown Silently Truncated Messages

- **Status**: FIXED AND VERIFIED LIVE ON SM21
- **Severity**: Medium / Operational output fidelity
- **Symptom**: A live SM21 query returned 406 system-log entries. Markdown shortened a 65-character PSE certificate message to 50 characters, hiding the ending of the message without a truncation notice.
- **Fix and verification**: A formatter regression failed before the change. Table Markdown now caps alignment padding at 50 characters but emits complete escaped cell values. On the same live grid, the rebuilt CLI included the complete first message, returned 20 rows by default, and kept the 406-row total; `--max-rows 200` returned 200 complete 10-cell rows. All 104 CTest cases passed.

---

### [ERR-075] Inline Credential Value Escaped Grid Redaction

- **Status**: FIXED AND VERIFIED LIVE ON GATEWAY CLIENT
- **Severity**: High / Credential disclosure
- **Symptom**: The grid redactor treated any cell containing a credential name as a label to preserve. A `NAME` cell such as `Authorization: Bearer <value>` retained the embedded value while masking other cells.
- **Fix and verification**: A synthetic regression failed before the change. Only known standalone credential names are now preserved as labels; compound cells are fully redacted. An unsent synthetic inline authorization header in live `/IWFND/GW_CLIENT` appeared as `[REDACTED]` in both JSON and Markdown, with its marker absent from both outputs. The header was removed, and the request grid returned to zero rows. All 105 CTest cases passed.

---

### [ERR-076] ST22 List Label Could Not Open a Runtime Error

- **Status**: FIXED AND VERIFIED LIVE ON ST22
- **Severity**: Medium / Classic ABAP list interaction
- **Symptom**: Clicking ST22 Overview's visible `STRING_OFFSET_TOO_LARGE` row returned `Press method not found on element`. The row is a positioned `GuiLabel` on an ABAP list, where `Press` is unavailable.
- **Fix and verification**: A red regression covered positioned label routing. For SAP list labels, Fairyfly now sets focus and sends F2; ordinary non-list labels still return `ELEMENT_NOT_CLICKABLE`. The rebuilt CLI opened `List of Selected Runtime Errors`, selected its one dump, and displayed the long text. A separate live check exposed a false success when F2 on a static heading changed nothing. Another red regression added an outcome check using the target label, active window, title, and status: the static heading now returns `ACTION_OUTCOME_UNVERIFIED`, while the real runtime-error row returns success and opens the list. All 107 CTest cases passed.

---

### [ERR-077] SE38 ABAP Editor Source Is Missing from Screen Reads

- **Status**: FIXED AND VERIFIED LIVE ON SE38
- **Severity**: Medium / Source inspection
- **Symptom**: SE38 displayed include `/IWFND/R_MGW_REGISTRATION_F03` in a `GuiShell` with subtype `AbapEditor`, but `screen read` returned no `text_content`; direct `get` returned only `SAPGUI.AbapEditor.1`.
- **Fix and verification**: The old `Text` property did not expose source, and `ScreenReader` routed every non-grid `GuiShell` through tree extraction, bypassing the editor metadata path. Red-green tests now cover bounded `GetLineCount`/`GetLineText` extraction, editor routing, and main-screen Markdown rendering. A later live screenshot showed SAP editor lines are one-based; the fake-COM regression then rejected line 0, and the corrected reader removed a phantom blank first line. The rebuilt CLI returned lines 1–200 of 1,187 from live SE38, marked the response truncated, and aligned the Gateway caller with ST22 line 138 in JSON and Markdown. ABAP source now has a dedicated redactor in both metadata paths; see ERR-078. An initial `uvx erpl-adt` read returned HTTP 401 without credentials; a later read succeeded using credentials supplied through process environment variables.

---

### [ERR-078] ABAP Editor Exposed Named Credential Assignments

- **Status**: FIXED AND VERIFIED LIVE ON SE38
- **Severity**: High / Source disclosure
- **Symptom**: The generic response-text filter did not mask ABAP assignments such as `DATA(lv_password) = '...'`, including values continued onto subsequent source lines. SE38 source is now returned by screen reads, so such assignments could appear in JSON and Markdown.
- **Fix and verification**: A synthetic ABAP regression failed to compile before the new source-specific redactor existed. Both AbapEditor metadata paths now mask whole statements that contain recognizable credential names while preserving source line count. Two more red-green regressions caught periods inside string templates and backtick literals, and a harmless statement followed by an unfinished sensitive assignment on the same line. A fourth red-green regression caught a value at the bounded read's final line whose credential name could be beyond the range; unfinished final statements are now suppressed. All four pass alongside ordinary source preservation; the rebuilt CLI and the full 114-case CTest run had zero failures and five SAP-dependent skips. A fresh Bigfox login opened SE38: the standard Gateway include still returned 200 of 1,187 correctly numbered lines in JSON and Markdown, including the expected call at line 138. In disposable local report `ZFFLY_REDACT_260926`, both formats hid a synthetic password assignment and retained an ordinary `WRITE` line. The report was deleted; a fresh Display returned `Program ZFFLY_REDACT_260926 does not exist`. The test logon was ended and its saved connection removed. Unlabelled secrets in completed statements are outside this heuristic.

---

### [ERR-079] Direct Get Returned an ABAP Editor Identifier Instead of Source

- **Status**: FIXED AND VERIFIED LIVE ON SE38
- **Severity**: Medium / Source inspection
- **Symptom**: `get <editor-id>` read the `GuiShell` text property and returned `SAPGUI.AbapEditor.1`, while `screen read` could already extract source lines.
- **Fix and verification**: A fake-COM regression failed before direct reads used the editor's bounded source reader. Direct `get` now returns the same redacted source in `value`, with total/read line counts and a truncation flag; other element types retain their ordinary text path. The rebuilt CLI and full 115-case CTest run had zero failures and five SAP-dependent skips. On live SE38, `get` returned 200 of 1,187 lines from `/IWFND/R_MGW_REGISTRATION_F03`, included the expected Gateway call at line 138, and omitted the old placeholder. The test logon was ended and its saved connection removed.

---

### [ERR-080] Credential Query Parameters Could Leak from URL Text

- **Status**: FIXED AND VERIFIED LIVE ON GATEWAY CLIENT
- **Severity**: High / Credential disclosure
- **Symptom**: The response-text filter handled named lines and structured payloads but returned a URL such as `...?access_token=<value>&$top=1` unchanged. An ordinary URI input field could also return that value through `get` because its field ID and label did not identify a credential.
- **Fix and verification**: A red regression for URL query parameters failed before the response filter masked recognizable credential names in query strings while retaining unrelated parameters. A second red fake-COM regression showed the ordinary input-field path still leaked; `ComGuiElement::get_text` now applies the filter to structured field values with credential names. On live `/IWFND/GW_CLIENT`, an unsent synthetic URI returned a masked token and retained `$top=1` through direct `get`, JSON screen read, and Markdown screen read. The URI was cleared without sending a request. All 117 CTest cases passed while SAP GUI was open; the test logon and saved connection were removed.

---

### [ERR-081] Gateway TextEdit Control Was Missing from Screen Reads

- **Status**: FIXED AND VERIFIED LIVE ON GATEWAY CLIENT
- **Severity**: Medium / Screen extraction
- **Symptom**: The Gateway Client request URI is a `GuiShell` with subtype `TextEdit`. Direct `get` read its value, but `screen read` routed the shell as a tree and returned no text for it.
- **Fix and verification**: The routing regression failed before `TextEdit` joined the metadata extraction path. The existing metadata reader then supplied its redacted `text` to JSON and Markdown. The same live unsent Gateway Client URI appeared in both screen formats with its synthetic token masked and harmless parameter intact. All 117 CTest cases passed with SAP GUI open; the URI was cleared and the session closed.

---

### [ERR-082] PowerShell Could Not Clear a Field with an Empty Positional Value

- **Status**: FIXED AND VERIFIED LIVE ON GATEWAY CLIENT
- **Severity**: Medium / CLI field interaction
- **Symptom**: Windows PowerShell 5.1 omitted an empty quoted argument when invoking `fairyfly fill <element> ''`, so CLI11 rejected the command with `value is required`. Clearing the Gateway Client URI required a temporary script.
- **Fix and verification**: A CLI regression first failed with the parser error. `fill <element> --clear` now passes an empty string to the existing field writer; specifying both a value and `--clear` returns `INVALID_ARGUMENT`. On live `/IWFND/GW_CLIENT`, a synthetic unsent URI was filled, cleared with the new option, and read back as the editor's empty carriage return. The conflicting-value call was rejected without writing. All 118 CTest cases passed offline, with five SAP-dependent skips. The test logon and saved connection were removed.

---

### [ERR-083] Disconnect Removes the Saved File but Leaves the SAP GUI Session Open

- **Status**: FIXED AND VERIFIED LIVE ON BIGFOX LOGIN SCREEN
- **Severity**: Medium / Session lifecycle
- **Symptom**: The CLI describes `disconnect` as disconnecting from SAP, but a live `disconnect --connection 0` deleted only `fairyfly.0.con`; `list` still reported one live SAP GUI connection and session. `ComAutomationEngine::disconnect` only releases local wrappers, and the command handler does not call it or close the GUI connection. A separate SAP GUI scripting `CloseConnection` call was needed to end the login-screen session.
- **Fix and verification**: A parser regression first failed on `--close-session`. The new opt-in path validates the saved server-session key, calls SAP GUI `CloseSession`, verifies the selected session disappeared, and conditionally removes only the matching saved file. The ordinary command remains a cache-only detach and now says so in help and output. On disposable live Bigfox login-screen sessions, the opt-in command reduced GUI connections and saved files to zero; the ordinary command removed the file but left one GUI session, which was then closed. A later two-connection test closed `/app/con[1]/ses[0]` while `/app/con[0]/ses[0]` remained logged in and readable. Another live test created `ses[1]` inside `/app/con[0]`; `attach --session-id` saved that child normally, closing only it left parent `ses[0]` readable, and closing the parent emptied the GUI and cache. The full 120-case CTest run passed with five SAP-dependent skips after logoff.

---

### [ERR-084] SM59 Runner Left Its Launched SAP GUI Session Open

- **Status**: FIXED AND VERIFIED LIVE ON BIGFOX LOGIN SCREEN
- **Severity**: Medium / Integration cleanup
- **Symptom**: The default SM59 integration runner launched Bigfox, then called cache-only `disconnect`. If SM59 was rejected on the login screen, the saved file disappeared but the GUI session remained open.
- **Fix and verification**: An offline regression first failed because the runner omitted `--close-session`. It now requests session closure and requires both `session_closed` and `file_deleted` in the response. In a live unauthenticated run, SAP rejected SM59 on its login screen with `This function is not possible`; the runner skipped screen reads, reported cleanup success, and a separate `list` and `connections` check found zero GUI sessions and zero saved files. The subsequent opt-in authenticated run passed all six checks, including SM59 JSON and Markdown reads and session closure; five offline runner tests passed.

---

### [ERR-085] SU01 Launch-Mode Runner Could Not Log In and Left Its Session Open

- **Status**: FIXED AND VERIFIED LIVE ON BIGFOX SU01
- **Severity**: Medium / User-administration integration
- **Symptom**: The SU01 runner's default branch launched Bigfox but had no login step, so it could not reach SU01 without SSO. Its `finally` called cache-only `disconnect`, leaving the launched SAP GUI session open.
- **Fix and verification**: A new offline launch-mode contract failed because no login was submitted to the returned session and no close-session call was made. Opt-in `-LoginFromTrialEnv` now uses the credential-safe GUI login helper and the runner closes only its owned session. A failed-login mock confirmed cleanup before any SU01 action. On live Bigfox, disposable user `ZFF97E9FEB4` was created, read back, had its password changed, deleted, and confirmed absent; `list` and `connections` then returned zero. The existing-session mode still preserves its caller's connection.

---

### [ERR-086] Native Standard Input Marker Corrupted Disposable-User Password Login

- **Status**: FIXED AND VERIFIED LIVE ON BIGFOX SU01
- **Severity**: Medium / Password verification
- **Symptom**: A second SAP GUI session rejected the disposable user's changed password as incorrect even though SU01 confirmed the change. A synthetic probe showed that Windows PowerShell piped an 18-character ASCII password to `cscript.exe` as 21 characters: the first three decoded characters were the UTF-8 marker bytes 239, 187, and 191.
- **Fix and verification**: The GUI login helper strips a native-input UTF-8 marker before setting SAP's password field. With the correct 18-character value, SAP advanced from "Name or password is incorrect" to "Enter a new password." The helper now fills the forced-change dialog, then checks `session.Info.User`. An offline regression failed before the two-password flow and passed afterward. On live Bigfox, disposable `ZFF3E55F320` authenticated in a second session after its SU01 password change, completed SAP's first-login password change, and was deleted from the admin session. Both sessions and saved connection files were closed or removed; no password was printed.

---

### [ERR-087] Exact Child SAP GUI Sessions Required a Manual Cache Entry

- **Status**: FIXED AND VERIFIED LIVE ON BIGFOX
- **Severity**: Medium / Multi-session attachment
- **Symptom**: After SAP `CreateSession` opened `/app/con[0]/ses[1]`, Fairyfly could list both children but `attach` only offered mouse-based window selection. A prior targeted-close test had to construct a temporary saved connection entry directly to address the child session by CLI ID.
- **Fix and verification**: A CLI regression first failed on `attach --session-id`. The command now selects an exact accessible SAP GUI session and saves its own server-session key and connection metadata through the normal cache manager. An invalid ID returned `SESSION_NOT_FOUND` without creating a file; all 120 CTest cases passed offline with five SAP-dependent skips. On live Bigfox, `attach --session-id /app/con[0]/ses[1]` created cache ID 1, both parent and child screen reads succeeded, and closing only the child left the parent readable. The parent was then closed; GUI sessions and saved files returned to zero.

---

### [ERR-088] Exhausted Connection ID Wrapped to a Negative Cache Filename

- **Status**: FIXED AND NORMAL LIVE SAP GUI CACHE PATH VERIFIED
- **Severity**: Medium / Connection cache reliability
- **Symptom**: If `fairyfly.2147483647.con` existed, the allocator incremented the maximum signed integer and returned `-2147483648`. A red isolated-cache test observed creation of `fairyfly.-2147483648.con` instead of an error. A directory-scan exception could also be logged and ignored, leaving ID allocation based on an incomplete scan.
- **Fix and verification**: Allocation now raises `SystemError` when the highest ID is already in use or scanning fails. The isolated regression passed after the fix; the Release CLI and unit-test targets built, and all 121 CTest cases passed with five SAP-dependent skips. On live Bigfox, the rebuilt CLI launched a fresh session, logged in, navigated to SM59, read JSON and Markdown, and closed its own saved session; all six integration checks passed. A second temporary live session opened ST22 and closed successfully. Final `list` and `connections` each returned zero. The maximum-ID condition itself remains covered by the isolated cache regression.

---

### [ERR-089] URL Fragment Credential Escaped Gateway Text Redaction

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: Medium / Credential output
- **Symptom**: The shared URL filter recognized credential parameters after `?` or `&`, but not at the start of a fragment after `#`. A response or unsent URI containing `#access_token=<value>&state=ready` could therefore expose the token in direct `get` and screen output.
- **Fix and verification**: A red regression demonstrated the synthetic fragment marker remained in filtered output. The URL parameter matcher now recognizes `#` as a boundary and masks the named value while preserving ordinary following parameters. The focused test passed after the change; Release CLI and unit tests built, and all 122 CTest cases passed with five SAP-dependent skips. On live Bigfox, Fairyfly put an unsent synthetic fragment token in the Gateway Client URI; direct `get`, JSON screen read, and Markdown screen read all omitted the marker, retained `state=ready`, and showed `[REDACTED]`. The URI was cleared without sending a request, and the temporary SAP GUI session and saved connection were closed; both enumeration commands returned zero.

---

### [ERR-090] COM Observation Failure Could Be Mistaken for a Closed SAP Session

- **Status**: FIXED AND NORMAL CLOSE VERIFIED LIVE ON BIGFOX

- **Severity**: Medium / Session lifecycle

- **Symptom**: After `CloseSession`, `validate_session()` could return `false` when COM enumeration failed. The CLI could then report closure and delete the saved connection even though the selected session had not been observed to disappear.

- **Fix and verification**: A red fake-COM regression demonstrated that an unreadable collection `Count` needed to raise an error, not appear as zero. A second red run showed that strict COM objects reject mixed method/property flags for `Count`; the verifier now makes a property-only read. Close verification checks collection counts, item IDs, and the server session key; transient observation errors are retried, and unresolved observation returns `SESSION_CLOSE_UNVERIFIED` without removing the saved connection. The Release CLI and unit-test targets built, and all 125 CTest cases passed with five SAP-dependent skips after the related cleanup fix. A live Bigfox integration run launched and logged in, read SM59 in JSON and Markdown, then closed the targeted session and saved file; all six checks passed. Final `list` and `connections` each returned zero. The COM-failure branch remains verified with fake COM rather than a live fault injection.

---

### [ERR-091] Connection Cleanup Could Delete Saved Entries after COM Observation Failure

- **Status**: FIXED; LIVE SESSION RETENTION AND STALE-ENTRY CLEANUP VERIFIED

- **Severity**: Medium / Connection cache reliability

- **Symptom**: `connections --cleanup` passed each saved entry to `validate_session()`. That method returned `false` on COM errors, so cleanup could interpret an unreadable SAP GUI collection as proof that a live session was gone and delete its cache file.

- **Fix and verification**: Cleanup now calls a checked session-observation method that raises on unreadable collection counts, item IDs, server keys, or disabled server-side scripting. Ordinary connection listing still uses tolerant validation. A red fake-COM test failed before the checked observer was available, then confirmed that count failure differs from a genuinely empty collection. A separate cache-level test confirmed an observation exception leaves the saved entry intact. Release targets built and all 125 CTest cases passed with five SAP-dependent skips. With SAP GUI at zero sessions, the rebuilt CLI removed one deliberately stale file from an isolated cache directory and reported `cleaned_up=1`, `count=0`. In a second live Bigfox run, a new login-screen session survived `connections --cleanup` with `cleaned_up=0` and `valid=true`; targeted close then removed its session and file, leaving zero GUI sessions and zero saved entries. A live transient COM fault was not injected, so its cache-preservation path rests on the synthetic tests.

---

### [ERR-092] Assigned SAP Field Label Did Not Protect a Neutral Credential Input

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE SE38 SELECTION SCREEN

- **Severity**: High / Sensitive data exposure

- **Symptom**: A disposable report displayed `Client Secret` beside ordinary text field `P_VALUE`. Its technical ID was neutral and `AccLabel` was empty. Before the fix, a synthetic value placed in that field appeared in direct `get`, JSON screen output, and Markdown screen output, although fill output did not echo it. Independent SAP GUI scripting showed that `P_VALUE.LeftLabel` pointed to the `Client Secret` caption. [SAP documents `LeftLabel` for `GuiTextField`](https://help.sap.com/docs/sap_gui_for_windows/b47d018c3b9b45e897faf66a6c0885a8/a9af75bc7d08499ca19ec3879db7d7b3.html).

- **Fix and verification**: When `AccLabel` is empty, the element wrapper now reads the assigned `LeftLabel` or `RightLabel` text before deciding whether a text field is sensitive. A fake-COM regression first failed because the neutral field's label was empty, then passed with the linked caption and no field-text read. Release targets built and all 126 CTest cases passed. On the first live selection screen, the rebuilt CLI reported label `Client Secret`, returned `[REDACTED]` from direct `get` and JSON, and omitted the synthetic marker from Markdown. A second disposable report placed `Client Secret` to the right of neutral field `P_VALUE`; independent SAP GUI scripting found `LeftLabel` empty and `RightLabel` pointing to the caption. A synthetic value confirmed present in the GUI was absent from direct `get`, JSON, and Markdown. Both disposable reports were deleted through ADT and exact-name searches returned `[]`; local probes were removed, SAP GUI sessions closed, and both GUI and cache enumerations returned zero. A subsequent read-only COM survey examined 104 input fields across seven standard screens; none had a nonempty `AccLabel` without a side label. An `AccLabel`-only control therefore remains untested live.

---

### [ERR-093] Percent-Encoded Credential Names Bypassed Gateway URI Redaction

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: High / Credential disclosure
- **Symptom**: The URL filter recognized literal `access_token` and `client_secret` parameter names, but its matcher skipped `%` in names such as `access%5Ftoken`. A synthetic value in that URI could therefore appear in direct `get`, JSON, and Markdown output.
- **Fix and verification**: A regression first failed with the synthetic value visible. Parameter names are now percent-decoded for credential classification while the original URI and unrelated parameters remain intact. The focused test covers query, fragment, and a URL inside generic JSON. Release CLI and unit tests built; all 127 CTest cases passed with five SAP-dependent skips. In live `/IWFND/GW_CLIENT`, independent SAP GUI scripting confirmed the unsent URI contained the marker. Fairyfly's direct `get`, JSON, and Markdown output all masked it and retained `$top=1`. The URI was cleared without sending a request, the disposable SAP GUI session was closed, and both live and saved connection counts returned zero.

---

### [ERR-094] Classic Report Lines Were Invented as Status Properties

- **Status**: FIXED AND VERIFIED LIVE ON DISPOSABLE SE38 REPORTS
- **Severity**: Medium / Markdown screen rendering
- **Symptom**: A live classic-list report displayed its lines correctly under `Report Output`, but the Markdown formatter also treated `Diagnostic status: 17 rows` as a property label and paired it with a later report line in a fabricated `Status Information` table. A one-column heading ending in `:` could cause the same error.
- **Fix and verification**: A formatter regression first failed on the fabricated status section. The status renderer now accepts only labels ending in `:` and emits no empty section. A second red regression for a one-column trailing-colon report heading led to a stricter requirement for multiple aligned property labels; a positive test preserves a genuine two-column status grid. Release CLI and unit tests built; all 130 CTest cases passed, with five SAP-dependent skips in the final run. On live Bigfox, two disposable SE38 reports rendered their known lines in JSON and Markdown without a false status section. Both reports were deleted through ADT with exact-name searches returning `[]`; the SAP GUI sessions and saved connections were closed, and final counts were zero. The historical EPM generator output remains a separate unverified layout.

---

### [ERR-095] Semicolon-Separated URL Credential Names Escaped Redaction

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: High / Credential disclosure
- **Symptom**: The URL matcher handled `?`, `&`, and `#` parameter boundaries but treated `;` as ordinary value text. In an unsent URI such as `?$top=1;access_token=<value>`, the token name was not recognized and its value could appear in direct `get`, JSON, and Markdown output.
- **Fix and verification**: A regression first failed with a synthetic marker visible in a semicolon-separated query; it also covers a fragment variant and harmless neighboring parameters. The matcher now recognizes `;` as a boundary and stops values at it. The focused test passed, Release CLI and unit tests built, and all 131 CTest cases passed while a disposable SAP GUI session was open. In live `/IWFND/GW_CLIENT`, independent SAP GUI scripting confirmed that the unsent URI held the synthetic marker. Direct `get`, JSON, and Markdown all masked it while retaining `$top=1` and `mode=read`. The URI was cleared without sending a request; the SAP GUI session and saved connection were closed, with both counts returning zero.

---

### [ERR-096] Repeated SAP Warning Let Execute Claim Success Without Running

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE SE38 REPORT
- **Severity**: Medium / Action-result reliability
- **Symptom**: A disposable ABAP selection screen emitted `MESSAGE ... TYPE 'W'` on Execute and changed no application data. The first Fairyfly click returned `ACTION_OUTCOME_UNVERIFIED`, but a second click returned success while the same warning remained, the selection field was still visible, and the report's completion text was absent. The classifier had treated an unchanged warning as stale rather than as unresolved evidence for a submitting action.
- **Fix and verification**: The existing unit expectation for repeated warnings was changed to require `ACTION_OUTCOME_UNVERIFIED`; it failed before the fix. Submitting actions now remain unverified whenever a non-rejection `W` warning is still present, including when it is unchanged. The focused test passed, Release CLI and unit tests built, and all 131 CTest cases passed with the disposable SAP GUI session open. Retesting the same live Execute returned `ACTION_OUTCOME_UNVERIFIED`, preserved SAP's `W` message, and identified the repeated warning; a fresh screen read still showed the selection field and no completion text. ADT deleted `ZFFLY_WARN_260927` and exact-name search returned `[]`; the GUI session and saved connection were closed, and both counts returned zero.

---

### [ERR-097] Gateway Add Header Dialog Exposed a Compound Credential Value

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: High / Credential disclosure
- **Symptom**: In `/IWFND/GW_CLIENT`, the Add Header dialog placed `X-Session-Token` in `txtIP_HEADER_NAME` and a synthetic value in the generically named `txtIP_HEADER_VALUE`. Before insertion into the request grid, Fairyfly direct `get`, JSON, and Markdown screen reads exposed the value. The existing grid-row redactor protected it only after the dialog closed.
- **Fix and verification**: A fake-COM regression first failed with the marker visible. `ComGuiElement::get_text` now reads the sibling header name through the modal's parent before reading this value field; a sensitive or unavailable name suppresses the value. The focused test covers the sensitive name, an ordinary `Content-Type` name, and an unavailable sibling. Both Release targets built and all 132 CTest cases passed at the time of the fix. The same live dialog then returned `[REDACTED]` from direct `get` and JSON; neither JSON nor Markdown contained the synthetic marker. After Continue, the unsent request grid displayed `X-Session-Token` with `[REDACTED]`, again without the marker in JSON or Markdown. A separate unsent `Content-Type: application/json` dialog remained readable. A later live `Change Header` action on a selected `X-Session-Token` row opened the same protected `txtIP_HEADER_VALUE` field and still displayed `[REDACTED]`. The synthetic headers were removed, harmless dialogs cancelled, request grids returned to zero rows, and GUI sessions and saved connections were closed. No HTTP request was sent.

---

### [ERR-098] Direct Get Bypassed Credential Report-Row Redaction

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE CLASSIC REPORT
- **Severity**: High / Credential disclosure
- **Symptom**: A disposable classic report placed `Password:` and a synthetic marker in separate positioned `GuiLabel` elements on one row. `screen read` JSON masked the whole row, but direct `get` of the value label returned the marker because the report-row postpass does not run for an individual element.
- **Fix and verification**: A direct-label fake-COM regression first failed with the synthetic marker visible. Direct reads of positioned report labels now inspect sibling labels on the same row before returning text and suppress the row when any label names a credential; if the row cannot be verified, the direct value is suppressed. The screen-read path retains its existing postpass without repeated sibling traversal. The corrected fixture exercises real child lookup, masks the credential row, and preserves an ordinary neighboring row. Both Release targets built and all 133 CTest cases passed. In the same live `ZFFLYREP260927` report, rebuilt Fairyfly returned `[REDACTED]` for `lbl[29,2]` while `lbl[0,0]` still returned its harmless report heading. The GUI session was closed, ADT deleted the disposable program, and an exact-name search returned `[]`.

---

### [ERR-099] Generic Paired Name and Value Fields Could Expose Credentials

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY AND A DISPOSABLE ABAP FORM; OTHER CONVENTIONS OPEN
- **Severity**: High / Credential disclosure
- **Symptom**: The ERR-097 guard recognized Gateway's exact `txtIP_HEADER_VALUE` control, but another dialog with neutral `txtNAME` and `txtVALUE` fields could return a credential value even when the sibling name was `Authorization`. Direct `get` and automatic screen extraction both use `ComGuiElement::get_text` and would expose it.
- **Fix and verification**: A fake-COM regression for `txtNAME=Authorization` and `txtVALUE=Bearer ...` failed with the synthetic marker visible. Text and CText fields whose leaf ID ends in `VALUE` now look for the matching sibling ending in `NAME` under the same parent and classify its raw name before reading the value. The known Gateway header control still fails closed when its sibling cannot be observed. The focused tests also preserve an ordinary `Flight` pair and a standalone ordinary value. Both Release targets built and CTest exited successfully with 135 registered cases. In live `/IWFND/GW_CLIENT`, independent SAP scripting confirmed an unsent `Authorization` name and synthetic value in the Add Header dialog; rebuilt Fairyfly returned `[REDACTED]` from direct `get` and JSON, and neither JSON nor Markdown contained the marker. Replacing the pair with `Content-Type: application/json` kept the value readable. The dialog was cancelled, the request grid remained at zero rows, and the GUI session and saved connection were closed. Other paired-field ID conventions and layouts remain unverified live.
- **Independent form retest (2026-09-27)**: Disposable `ZFFLYPAIR260927` exposed ordinary `GuiTextField` controls `txtP_NAME` and `txtP_VALUE` on an SA38 selection screen. Independent SAP scripting confirmed `Authorization` and a synthetic value were present. Fairyfly direct `get` and JSON returned `[REDACTED]` for the value, and neither JSON nor Markdown contained the marker. Changing the same pair to `Flight` and `LH` made both direct `get` and JSON return `LH`. Both fields were cleared and independently confirmed empty; the report was not executed. ADT deleted it with exact-name search `[]`, and the GUI session and saved connection were closed. The generalized path is therefore live verified outside Gateway; different naming conventions and layouts remain open.
- **Sibling lookup failure follow-up (2026-09-27)**: A red fake-COM regression showed that a failed `FindById` lookup could expose a generic `VALUE` field even when a sensitive `NAME` sibling existed. The accessor now checks the parent's children to establish whether the sibling is absent; unavailable or inconsistent parent data suppresses the value. The regression covers a missing parent, a failed lookup with an existing sensitive sibling, and a verified standalone value. Both Release targets built and all 136 CTest cases passed. On a fresh live SA38 form, `P_NAME=Authorization` masked the synthetic `P_VALUE` marker while standalone `P_XVALUE=StandaloneSafe` remained readable. The fields were cleared without executing the report; the SAP GUI session and saved connection closed with zero remaining, and ADT deletion of the disposable report was confirmed by an exact-name search returning `[]`. A transient SAP COM lookup failure could not be induced live, so that failure path remains verified by the fake-COM regression.

---

### [ERR-100] Unreadable Sibling ID Could Expose a Direct Report Label

- **Status**: FIXED; NORMAL PATH VERIFIED LIVE, COM FAILURE PATH SYNTHETICALLY VERIFIED
- **Severity**: High / Credential disclosure
- **Symptom**: Direct `get` of a positioned classic-report value label checked sibling labels by their IDs. If SAP COM returned an empty ID for a sibling, the reader skipped it and could return the value even when that sibling's visible text was `Password:`.
- **Fix and verification**: A fake-COM regression first returned a synthetic marker from the value label with an unreadable sibling ID. Direct report-label reads now suppress the value when any enumerated sibling has an unreadable ID. The focused regression passed after the fix; both Release targets built, and all 137 CTest cases passed with five SAP-dependent skips. A fresh disposable classic report `ZFFLYREP100_260927` gave SAP GUI separate `Password:` and synthetic-value labels on one row and `Ordinary:` and `SafeValue` labels on another. Independent SAP scripting confirmed both raw values were present. Fairyfly JSON masked the credential row, direct `get` returned `[REDACTED]` for its value, and direct `get` returned `SafeValue` for the ordinary row. The GUI session and saved connection were closed with zero remaining, and ADT deletion returned success followed by exact-name search `[]`. An intermittent unreadable-ID failure could not be induced on SAP GUI, so that branch remains verified by the fake-COM regression.

---

### [ERR-101] Generic Key and Value Form Fields Exposed a Credential

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE ABAP FORM; OTHER CONVENTIONS OPEN
- **Severity**: High / Credential disclosure
- **Symptom**: The generic GUI field guard paired `NAME` with `VALUE`, but ignored `KEY`. On a live SA38 selection screen, ordinary `txtP_KEY=Authorization` and `txtP_VALUE=FFLYKEY260927MARKER` controls caused direct Fairyfly `get` to return the synthetic marker. A fake-COM regression failed the same way.
- **Fix and verification**: The value reader now checks sibling `NAME`, `KEY`, `FIELDNAME`, and `HEADERNAME` controls. It checks all matching siblings, and if a direct lookup cannot establish absence, it enumerates the bounded parent and suppresses unverifiable values. The sensitive-key regression and an ordinary `Flight`/`LH` preservation test pass; both Release targets built and all 139 CTest cases passed with five SAP-dependent skips. On the same live disposable `ZFFLYKEY260927` form, independent SAP scripting confirmed `Authorization` and the marker remained in the fields while rebuilt Fairyfly masked `P_VALUE` in direct `get`, JSON, and Markdown. Changing the pair to `Flight`/`LH` made direct `get` and JSON return `LH`. Both fields were cleared and independently confirmed empty without executing the report. ADT deleted the report and an exact-name search returned `[]`; the GUI session and saved connection were closed with zero remaining. Those paired layouts were later verified live in ERR-127.

---

### [OPS-011] Makefile Test Target Skips an Unneeded CLI Build

- **Status**: WARM-BUILD OPTIMIZATION MEASURED; TEST SUITE PASSED
- **Observation**: `make test` depended on `build`, so it first invoked the CLI target even though `unit_tests` links `fairyfly_core` directly and depends only on the cache worker. The Debug test target had the same extra prerequisite.
- **Change and verification**: Both test targets now depend on their configure targets and build only `unit_tests` and its actual dependencies. Three warm Release `make test` runs fell from 6.85, 6.44, and 6.61 s to 5.25, 5.15, and 5.24 s on this workstation. A fresh `make test` run passed all 139 CTest cases. This saves a redundant target invocation on warm builds; it does not change clean compilation time or the separate `make build` target.

---

### [OPS-012] CDS-Backed OData V2 Service Published and Read Through Fairyfly GUI

- **Status**: LIVE PUBLICATION, GUI READ, AND MULTI-LAYER CLEANUP VERIFIED
- **Observation**: A fresh disposable `ZFFLY_CDS2_260927` view entity over `SFLIGHT` activated with no syntax errors. `ZFFLY_SRV2_260927` exposed it as `Flights`; an active OData V2 binding `ZFFLY_BIND2_260927` was created through ADT. The first publish call returned HTTP 200 but `SEVERITY=ERROR` because the binding's `<srvb:services srvb:name>` contained the service-definition name. Existing active bindings use the binding name there. After deleting and recreating the unpublished binding with the correct service name, publish returned `SEVERITY=OK` and ADT reported `published=true`, `created=true` with a service URL.
- **Verification and cleanup**: Authenticated `$metadata` returned HTTP 200 with `Flights`; `Flights?$top=1&$format=json` returned one row with `Carrier`, `ConnectionId`, `FlightDate`, `Price`, and `Currency`. Fairyfly refreshed `/IWFND/MAINT_SERVICE`: its frontend catalog grew from 64 to 65 rows and contained the disposable service. In `/IWFND/GW_CLIENT`, Fairyfly sent a read-only GET to that service's `$metadata`; its response grid showed `~status_code=200`, and the visible SAP GUI showed XML metadata. The unsaved request URI was cleared. Unpublish returned `SEVERITY=OK`; ADT then deleted the binding, service definition, and CDS view in reverse order. Exact-name searches for all three returned `[]`, the frontend GUI catalog returned to 64 rows with no disposable service, and the old `$metadata` URL returned HTTP 403. The GUI session and saved connection were closed with zero remaining. A write-capable API against a separate disposable business object remains open.

---

### [ERR-102] Relative Screenshot Path Reported Success Without a File

- **Status**: FIXED AND VERIFIED LIVE; INTEGRATION REGRESSION ADDED
- **Severity**: Medium / Misleading capture result
- **Symptom**: While inspecting the disposable Gateway service, `screen capture --file build/gateway_cds2.png` returned success and echoed that relative path, but no file appeared there. The same command with an absolute path produced a valid PNG. SAP GUI's `HardCopy` runs in another process, so a relative filename is not resolved against Fairyfly's working directory.
- **Fix and verification**: The direct `HardCopy` path now converts the requested filename to an absolute UTF-16 path before calling SAP GUI, checks that the COM method exists, and reports `SCREENSHOT_NOT_CREATED` if no nonempty file exists afterward. The live relative-path assertion failed before the change (`status=success`, file absent) and passed afterward (`status=success`, 117,167-byte PNG at the reported absolute path). A nonexistent output directory returned `SCREENSHOT_NOT_CREATED` with no file. The retained SAP GUI integration runner now checks a relative screenshot path and PNG signature; all five of its checks passed on the live SM59 session, and all five offline runner tests passed after their capture fixture was updated. Release CLI and unit tests built, and all 139 CTest cases passed. The test session closed to zero GUI/cache entries. Two ignored local PNGs remain under `build/` because automatic approval review rejected their exact-file deletion; they contain this disposable Gateway screen and the later SM59 screen.

---

### [OPS-013] Populated DDIC Table Key Change Preserved Its Row

- **Status**: VERIFIED LIVE; LATER TYPE/LENGTH WORK RECORDED IN OPS-014, OPS-015, AND OPS-017
- **Observation**: Disposable `$TMP` table `ZFFLYK260927` started with `CLIENT` and `ID` as keys and non-key `NOTE` of type `CHAR(40)`. A disposable ABAP class inserted and committed one `TEST0001` row with `NOTE=before adjustment`; an independent class-run returned `rows=1`.
- **Verification and cleanup**: Fairyfly opened SE11 Change, selected `NOTE` as a key, saved, and activated. SAP reported `ZFFLYK260927 saved` and `Object activated`. A fresh ADT DDIC read showed all three fields as keys, and a separately activated class-run still returned `note=before adjustment` and `rows=1`. The class then deleted the row and returned `rows=0`. After closing the GUI session, ADT deleted the class and table; exact-name searches for both returned `[]`. Fairyfly `list` and `connections` both returned zero. SE11 exposed the existing `NOTE` length field as read-only, so no type/length change or SE14 adjustment was exercised in this run.

---

### [OPS-014] Populated DDIC Table Length Change Preserved Its Row

- **Status**: LIVE LENGTH CHANGE VERIFIED; LATER SE14 CONVERSION WORK RECORDED IN OPS-015 AND OPS-017
- **Observation**: A fresh `$TMP` transparent table `ZFFLYT260927` had `NOTE : abap.char(40)` and one committed `TEST0001` row containing `before length adjustment`. ADT source changed `NOTE` to `abap.char(60)`; source check returned no messages, and a fresh DDIC read returned length 60. A separately activated class-run returned the original note and `rows=1`.
- **GUI verification and cleanup**: Fairyfly displayed the active table in SE11 and read `NOTE` as `CHAR` length 60. SE14 showed the table as Active, Saved, and existing in the database. Its Check action opened an HTMLViewer result pane; Fairyfly `screen read` rendered the navigation tree but did not expose text from that pane, so the detailed database check result is unverified. No adjustment prompt appeared in this length-widening workflow. The class deleted the row and returned `rows=0`; the GUI session closed, ADT deleted the class and table, both exact-name searches returned `[]`, and Fairyfly `list` and `connections` returned zero. Follow-up: cover a genuine SE14 conversion/error case and decide whether HTMLViewer content can be read safely.

---

### [ERR-103] HTML Viewer Was Misclassified as a Tree and Silently Omitted

- **Status**: HTML CHECK TEXT AVAILABLE AND VERIFIED LIVE; FALLBACK REPORTING RETAINED
- **Severity**: Medium / Missing diagnostic output
- **Symptom**: SE14 Check opens its result in a `GuiShell` with subtype `HTMLViewer`. Fairyfly classified every shell except grid and text editor as a tree, attempted tree extraction on this viewer, and omitted it from Markdown. JSON retained `Text=SAP.HTMLControl.1`, the browser control name rather than the displayed page.
- **Fix and verification**: A new classification regression failed before the change; `HTMLViewer` now takes metadata extraction. A second red test required JSON to mark `content_available=false` without the control name and Markdown to state that page content is unavailable. The semantic classifier and formatter now retain that diagnostic instead of hiding the shell. Both Release targets built and all 142 CTest cases passed. On a live read-only SE14 Check of `T000`, rebuilt Fairyfly returned the HTMLViewer ID and subtype with `content_available=false`, no `text` or invented tree nodes, and Markdown displayed the unavailable-content notice. Independent SAP scripting found empty `AccText` and `AccTextOnRequest` despite `DocumentComplete=True`. SAP's installed Scripting API documents no portable HTML/body getter, so the actual SE14 check detail remains unverified. The GUI session and saved connection were closed; both counts returned zero.

- **UI Automation follow-up (2026-09-27)**: SAP's [GUI Scripting API](https://help.sap.com/doc/9215986e54174174854b0af6bb14305a/770.08/en-US/sap_gui_scripting_api.pdf) exposes the HTML viewer's native `Handle` but no page-body getter. On live Bigfox SE14, the embedded Edge document exposed a Windows UI Automation `TextPattern`: its read-only text included `Overall consistency: Consistent` for the T000 database-object check and `Comparing: T000 (Active RTOBJ), T000 (Active Source)` with timestamp details for the runtime-object check. A red renderer test first failed because Fairyfly discarded readable text; a separate red direct-`get` test caught the browser control name. Fairyfly now reads up to 16,384 UTF-16 characters from the document, retries briefly while it loads, filters recognizable credentials, and reports availability and truncation. The fallback still says content is unavailable when no UI Automation text pattern exists. Focused tests and all 151 Release CTest cases passed. Live direct `get`, JSON, and Markdown returned the displayed runtime-object text; JSON returned the database consistency result, and a fresh first read after navigation succeeded. The temporary probe was removed, and the session and saved connection were closed.

---

### [OPS-015] SE14 Conversion Failure and Recovery on Disposable Data

- **Status**: FAILURE AND RECOVERY VERIFIED LIVE; DISPOSABLE OBJECTS REMOVED
- **Observation**: Disposable `$TMP` table `ZFFLYE260927` contained one `TEST0001` row with `NOTE=BADVALUE` in a `CHAR(8)` field. Changing `NOTE` to `INT4` passed source syntax but ADT activation was cancelled with `Type changed from NVARCHAR to INTEGER`. Fairyfly SE14 showed the table as Revised and present in the database. With `Persist data` selected and `Delete data` unselected, `Activate and adjust database` triggered SAP dump `CONVT_NO_NUMBER` in `RADGENREP`, method `CONVERT_ITAB_CONTENT`, because `BADVALUE` cannot be interpreted as a number. The restart log marked step 5 failed; the original database table was absent while a read-only native SQL query found the exact row in `QCMZFFLYE260927`.
- **Recovery and cleanup**: Following SAP's documented recovery for this conversion error, a disposable class changed only that temporary row to numeric text `12345678` and read it back. Fairyfly selected `Continue Adjustment`; SE14 reported the request executed successfully. A fresh class-run found one row with integer value `12345678`. A proposed reverse conversion to `CHAR(8)` with `Persist data` returned `Database object for table ZFFLYE260927 is inconsistent` without a new restart log. The class deleted the sole row (`rows=0`), after which Fairyfly used SE14 `Adjust Table and Delete Data` on the now-empty table. SAP reported success; ADT read the table as active with `NOTE : abap.char(8)`, and the class still returned `rows=0`. The GUI session closed, ADT deleted the class and table, and exact-name searches returned `[]` for both. A full failure/recovery/reversal reproduction later succeeded; see OPS-022.

---

### [OPS-016] Disposable Managed RAP OData V2 Write Workflow

- **Status**: POST, collection GET, PATCH, DELETE, GUI verification, and cleanup passed live; the historical JSON-only 404 did not recur with a fresh service (OPS-020).
- **Observation**: On Bigfox, a disposable table `ZFFLYR260927`, interface and projection CDS views, managed and projection behavior definitions, behavior pool class, service definition, and OData V2 binding activated. `uvx erpl-adt object create` rejects BDEF and SRVD/SRVB locally as unknown object types, while their advertised ADT REST collections accepted the same objects. Publication returned HTTP 200 with application `SEVERITY=OK`. Metadata exposed `Entries` with `Id` and `Note`.
- **Red-green live verification**: A collection GET initially returned zero rows for `API00001`. Authenticated POST created it; a second POST correctly rejected the duplicate. PATCH returned 204 and a collection GET returned `Note=Updated through OData`. Fairyfly used `/IWFND/GW_CLIENT` to GET the collection; its response grid reported HTTP 200 and Data Explorer displayed the exact `Id` and updated `Note`. Single-entity GET worked in SAP GUI and over HTTP with Atom XML, but an HTTP request with `Accept: application/json` returned `/IWBEP/CM_MGW_RT/020` (`Resource not found for segment 'EntriesType'`). This response-format difference needs a controlled backend follow-up; it is not evidence of a Fairyfly read failure. DELETE returned 204; Fairyfly refreshed the collection in Gateway Client and Data Explorer showed no entry. SE16 could not display the newly created table because `/1BCDWB/DBZFFLYR260927` had not been generated; `SE16N` is unavailable on Bigfox.
- **Read-only reference comparison (2026-09-27)**: The active standard `GWSAMPLE_BASIC` service returned a product from `ProductSet?$top=1&$format=json`, then returned HTTP 200 for the exact `ProductSet('AR-FB-1000')` resource as Atom, with `Accept: application/json`, and with `$format=json`. Fairyfly's live `/IWFND/GW_CLIENT` GET of the JSON URI showed `~status_code=200`, JSON content type, and the product entity in Data Explorer. The unsaved URI was cleared and the GUI session closed. This rules out a blanket JSON single-entity limitation on the Bigfox Gateway, but it does not distinguish a defect in the disposable RAP service model from a runtime defect triggered by that model. A recreated disposable service and its backend trace are still needed.
- **Historical Gateway log follow-up (2026-09-27)**: Fairyfly opened `/IWFND/ERROR_LOG` and `/IWBEP/ERROR_LOG` for the failed request. Both logs contain transaction `4DD97CCD6B3900A0E006AB7A780FC90D`: the frontend records HTTP 404 and `/IWBEP/CM_MGW_RT/020`, while its detail pane shows the actual URI `/sap/opu/odata/sap/ZFFLYR_BIND260927/Entries('API00001')`. The backend records `/IWBEP/CX_MGW_BUSI_EXCEPTION` with attribute `ENTITY_TYPE=EntriesType` and the same URI. Thus `EntriesType` is an entity-type attribute in the recorded failure, not a demonstrated URL segment. Neither log identifies the underlying model or runtime cause. No service object was changed; the GUI session was closed.
- **Cleanup**: The sole row was deleted before object removal. Unpublish returned `SEVERITY=OK`; ADT deleted the binding, service definition, projection behavior and view, managed behavior, behavior pool class, interface view, and table in dependency order. Exact-name searches for all six names returned `[]`, the old metadata URL returned HTTP 403, and Fairyfly `list` and `connections` both reported zero after closing the test GUI session.

---

### [ERR-104] Generic XML Credential Name Exposed a Sibling Value

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT; UNRECOGNIZED XML CONVENTIONS REMAIN OPEN
- **Severity**: High / Credential disclosure
- **Symptom**: The response-text filter suppressed XML with a credential-named element or attribute, but returned generic pairs such as `<Name>Authorization</Name><Value>secret</Value>` verbatim. A red regression proved the leak. Further red tests covered a CDATA name, a quoted `Name` attribute, and malformed XML with an unquoted `Name` attribute. The unquoted form was also reproduced in a live, unsent Gateway Client TextEdit: independent SAP GUI scripting found the synthetic value, and Fairyfly direct `get` exposed it before the final fix.
- **Fix and verification**: XML text filtering now suppresses the whole body when a generic `Name`, `HeaderName`, `Key`, or `FieldName` element or attribute contains a recognizable credential name. It handles ordinary text, CDATA, quoted attributes, and unquoted attributes in incomplete editor content while preserving ordinary `Content-Type` examples. Further red-green regressions covered decimal and hexadecimal XML character references, including long zero-padded references, inside those generic names; the filter decodes ASCII character references before classifying the name. Another red-green regression caught a truncated `<Name>Authorization<Value>...` editor body without a closing `Name` tag; the filter now classifies the visible name text even when that tag is incomplete. A UTF-8 BOM before XML or JSON also bypassed the format branch; the prefix is now skipped before classification, with regressions for both formats. The focused regressions and all 143 CTest cases pass, with five SAP-dependent skips. In fresh Bigfox Gateway Client sessions, independent scripting confirmed the unquoted-attribute, `Authoriz&#97;tion`, truncated-XML, and BOM-prefixed XML markers remained in the unsent URI editor. Rebuilt Fairyfly direct `get` returned `[REDACTED]` for all four; JSON omitted all four, and Markdown omitted the first three. The editors were cleared and the GUI sessions and saved connections closed; `list` reported zero sessions. Unrecognized XML conventions and unlabelled secrets still need separate coverage.

---

### [ERR-105] F4 Claimed a Search Dialog Opened When SAP Found No Values

- **Status**: FIXED AND VERIFIED LIVE IN SU01
- **Severity**: Medium / Misleading action result
- **Symptom**: Fairyfly `press_f4` on SU01's user-alias field returned `status=success` and `F4 search help opened successfully` despite its own `f4_dialog_opened=false`. The active screen remained SU01 and SAP's status bar said `No values found`. The user-name field on the same screen did open a real F4 restriction dialog, so the discrepancy was field-specific rather than a failed logon or stale connection.
- **Fix and verification**: A red regression required a missing dialog to yield `F4_DIALOG_NOT_OPENED` with SAP's status text; it failed to compile before the classifier existed. The engine now classifies the observed no-dialog outcome as an error instead of sending a success result to the CLI handler. The focused test and all 144 CTest cases pass. On live Bigfox SU01, the rebuilt command returned `F4_DIALOG_NOT_OPENED`, `No values found`, and `f4_dialog_opened=false` for the alias field; the user-name field still returned success with `f4_dialog_opened=true`, and Fairyfly read the restriction dialog. The dialog was cancelled, the GUI session and saved connection closed, and a fresh enumeration returned zero connections and sessions.

---

### [ERR-106] JSON Header-Pair Values Exposed Credentials

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: High / Credential disclosure
- **Symptom**: The response-text filter masked JSON objects with `Name` and `Value` keys but returned array pairs such as `["Authorization","secret"]` unchanged. A red regression exposed the synthetic value. An initial live `fill` attempt also showed that shell quote loss can leave a malformed `{headers:[[Authorization,secret]]}` editor value; direct `get` and Markdown exposed its synthetic marker before the malformed-input fix.
- **Fix and verification**: Parsed JSON arrays whose first item is a recognizable credential name now mask their remaining items; ordinary `Content-Type` pairs remain readable. A separate red-green regression makes the malformed-JSON fallback suppress a bare credential name at the start of an array pair while preserving an ordinary malformed pair. The focused tests and all 145 CTest cases pass, with five SAP-dependent skips. On Bigfox Gateway Client, independent SAP GUI scripting set and confirmed exact valid and malformed pair payloads in an unsent TextEdit. Rebuilt Fairyfly direct `get`, JSON, and Markdown omitted the marker for both. The editor was cleared, the GUI session closed, and `list` returned zero sessions.

---

### [ERR-107] F4 Mistook an Existing Modal for a Newly Opened Dialog

- **Status**: FIXED AND VERIFIED LIVE IN SU01; NESTED-MODAL F4 VERIFIED LIVE IN SE11
- **Severity**: Medium / Misleading action result
- **Symptom**: With SU01's user-name F4 restriction dialog already open at `wnd[1]`, a second `press_f4` targeting the obscured `wnd[0]` user field returned success and `f4_dialog_opened=true`. The engine waited for `wnd[1]` without checking whether that window existed before the action, so it counted the existing modal as a new result.
- **Fix and verification**: A red regression required a target behind a different active window to yield `WINDOW_MISMATCH` and required the expected new modal path to advance from `wnd[1]` to `wnd[2]` when the active window is `wnd[1]`. The engine now checks active-window identity before focusing or sending F4 and waits for the next modal level. The focused regression and all 146 CTest cases pass, with five SAP-dependent skips. Live SU01 returned `WINDOW_MISMATCH` for the second call while the original F4 dialog stayed open. After cancelling that dialog, a fresh call still returned success and opened a new restriction dialog. The dialog and session were closed; a later `list` and `connections` returned zero. A separate read-only SE11 run opened database-table F4 help at `wnd[1]`; F4 on its package `GuiCTextField` opened the personal package value list at `wnd[2]`, which Fairyfly read in Markdown. Cancelling `wnd[2]` returned to `wnd[1]`, and cancelling again returned to SE11. The session was closed with zero remaining. Only inline or non-modal help variants remain unverified.

---

### [ERR-108] Modal Package-List Header Click Reported Success Despite SAP Rejection

- **Status**: FIXED AND VERIFIED LIVE IN SE11
- **Severity**: Medium / Misleading action result
- **Symptom**: In SE11 database-table F4 help, package F4 opened a nested personal-value list at `wnd[2]`. Clicking the `Package` column header through Fairyfly returned `status=success`, left the same picker open with rows unchanged, and caused SAP to report `Line cannot be selected` in its status bar. The existing action classifier did not treat that SAP text as a rejection because its message type was `S`.
- **Fix and verification**: A red regression required `Line cannot be selected` with type `S` to classify as an action error. The classifier now recognizes `cannot be selected`; the focused test and all 146 CTest cases pass. In the live SE11 picker, clicking a real `S_EPM` row selected it, closed `wnd[2]`, and filled the parent package field. Clicking the header with the rebuilt CLI returned an error carrying SAP's text and left the picker open. Because the same SAP rejection was already present before the repeated validation click, the live code was `ACTION_OUTCOME_UNVERIFIED`; a fresh status produces `ACTION_FAILED` in the regression. Both popups were cancelled, the GUI session closed, and fresh `list` and `connections` returned zero.

---

### [OPS-017] Fresh INT4-to-CHAR Table Conversion Preserved a Row

- **Status**: CLEAN-START CONVERSION VERIFIED LIVE; PRIOR INCONSISTENCY UNEXPLAINED
- **Observation**: A fresh disposable `$TMP` table `ZFFLYREV260927` started with `NOTE : abap.int4`. A disposable class first returned `rows=0`, then inserted and committed one `TEST0001` row with `NOTE=12345678` and returned `rows=1 note=12345678`. Changing only `NOTE` to `abap.char(8)` passed ADT source check but its ordinary activation was cancelled with `Type changed from INTEGER to NVARCHAR`, the expected pre-adjustment state. Fairyfly opened SE14, which showed the table Revised, Saved, and present in the database. Independent SAP GUI scripting confirmed `Direct=true`, `Persist data=true`, and `Delete data=false` before `Activate and adjust database`.
- **Verification and cleanup**: After the SE14 confirmation, Fairyfly showed Active, Saved, and present in the database. A fresh ADT DDIC read reported `NOTE : abap.char(8)`, and the independent class still returned one row with `note=12345678`. The class deleted the sole row and returned `remaining=0`; Fairyfly closed its GUI session; ADT deleted the class and table in dependency order; exact-name searches for both returned `[]`. This clean-start reversal succeeds. The database-inconsistency result after the earlier failed `CHAR`→`INT4` conversion in OPS-015 therefore appears tied to that failure/recovery sequence, but its precise cause remains unverified. [SAP's Database Utility guide](https://help.sap.com/docs/SUPPORT_CONTENT/techtsg/3362709664.html) describes data-preserving conversion through a temporary table and reload.

---

### [OPS-018] Action-Status Implementation Moved Out of a Shared Header

- **Status**: INCREMENTAL BUILD IMPROVEMENT MEASURED; TESTS AND LIVE GUI PASSED
- **Observation**: Touching only `src/include/action_status.h` rebuilt `cli_handler.cpp`, the large `com_automation_engine.cpp`, and `test_raii_core_renderers.cpp`, then relinked both Release targets. The measured two-target rebuild took 24.86 seconds on this workstation.
- **Change and verification**: The existing action-status functions now have declarations in the header and definitions in `src/action_status.cpp`, which is part of `fairyfly_core`. Touching only the new source after the initial configure/build recompiled that one source and relinked both targets in 10.20 seconds, a 14.66-second reduction in one same-machine comparison. The one-time build that added the source took 41.06 seconds including CMake regeneration. All 146 CTest cases passed with five SAP-dependent skips. On live Bigfox SE11, table-name F4 and nested package F4 opened their dialogs, a nonselectable package header returned `ACTION_FAILED`, and a real `S_EPM` row filled the parent package field. The modal and GUI session were closed, and fresh `list` and `connections` returned zero. This improves edits to status-rule implementations; changing the header's declarations still triggers dependent compilation.

---

### [ERR-109] Screen Read Omitted Radio and Checkbox Selection State

- **Status**: FIXED AND VERIFIED LIVE IN SE11
- **Severity**: Medium / Incorrect screen state
- **Symptom**: Fairyfly's SE14 Markdown showed every radio option as `( )`, although independent SAP GUI scripting confirmed `Direct=true` and `Persist data=true`. The formatter already used a `selected` metadata field, but the metadata extractor never populated it; JSON omitted it too.
- **Fix and verification**: A fake-COM regression for both radio buttons and checkboxes failed because `selected` was absent. The extractor now reads SAP GUI's `Selected` property for these controls. All 147 CTest cases passed with five SAP-dependent skips. On live SE11, JSON reported `Database table=true` and `View=false`; after Fairyfly clicked `View`, the values reversed. Markdown showed `(•)` for the selected option and `( )` for the other. A later read-only SE14 T000 screen reported Direct and Persist data as selected, and Background, mass processing, and Delete data as unselected. An independent VBScript read of each SAP GUI control's `Selected` property matched all five values; Markdown marked Direct and Persist data with `(•)` and Delete data with `( )`. No adjustment was run. The temporary probe was removed, the session closed, and fresh `list` and `connections` returned zero.

---

### [OPS-019] MSVC Common-Header Precompilation Trial

- **Status**: LOCAL WARM-EDIT AND CLEAN-BUILD BENEFITS MEASURED; CI CACHE IMPACT OPEN
- **Observation**: On this workstation, requesting both Release targets with `--parallel 4` took 3.69 seconds for a no-op build. Touching only `src/cli_handler.cpp` took 13.98 seconds and touching only `src/com/element.cpp` took 11.36 seconds, including relinks. Both translation units parse common JSON and logging headers.
- **Change and verification**: `fairyfly_core` now precompiles nlohmann JSON, spdlog, and fmt headers under MSVC. The first build with CMake regeneration and a core recompilation took 33.19 seconds; the generated Release PCH occupies about 220 MB. A no-op build then took 3.54 seconds, while the same implementation-only touches took 10.88 and 8.64 seconds respectively. All 147 CTest cases passed with five SAP-dependent skips. The rebuilt CLI passed seven live SM59 integration checks, including login, JSON/Markdown reads, relative screenshot, and targeted session closure. Later matched local clean builds in opposite run orders took 41.15 and 42.19 seconds with PCH versus 47.22 and 48.67 seconds without. The fresh-build pair's sampled compiler/linker-process peak working-set sums were 4057.1 and 5992.5 MiB respectively. These measurements and their limits are detailed in [build performance](BUILD_OPTIMIZATION.md); CI cache transfer cost remains open.

---

### [ERR-110] Spanish Password Label Exposed a Neutral Field Value

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE SAP FORM
- **Severity**: High / Credential-output leak
- **Symptom**: A disposable selection screen attached the Spanish caption `Contraseña` to ordinary field `txtP_VALUE`. Independent SAP GUI scripting confirmed both the caption and a synthetic marker in the field. Before the fix, Fairyfly's direct `get`, JSON, and Markdown all returned the marker. The name normalizer discarded the UTF-8 bytes for `ñ`, leaving `contrasea`, which no sensitive-name rule recognized.
- **Fix and verification**: A red regression required both `Contraseña` and unaccented `Contrasena` to classify a neutral field as sensitive, while preserving an ordinary `Flight` label. It also required a JSON response key named `Contraseña` to mask its synthetic value. The sensitive-name rules now recognize both normalized forms. The focused regression passed with five assertions, and all 148 CTest cases passed with the live SAP GUI session present. On the same unsaved form, rebuilt Fairyfly returned `[REDACTED]` from direct `get` and JSON and omitted the marker from JSON and Markdown. Independent scripting still found the marker and exact Unicode label in SAP GUI. The value was cleared and independently confirmed empty; no report execution or Save occurred. The session closed and ADT deleted the disposable report. Other languages and unlabelled secrets remain outside this verification.

---

### [ERR-111] Plain-Text Name and Value Lines Exposed a Credential

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: High / Credential-output leak
- **Symptom**: In an unsent `/IWFND/GW_CLIENT` URI editor, `Name: Authorization` followed by `Value: FFLY_PLAIN_PAIR_260927` appeared as CR-separated lines. Independent SAP GUI scripting confirmed both lines in the raw editor, and Fairyfly direct `get` returned the synthetic marker. Existing plain-text filtering recognized sensitive field names but did not associate a generic `Value` line with the preceding credential name.
- **Red-green fix and verification**: A regression first failed with the marker visible. The response-text filter now suppresses a plain-text body when adjacent `Name`, `HeaderName`, `Key`, or `FieldName` and generic value lines form a credential pair, including SAP GUI's CR-only lines and CRLF. It preserves an ordinary `Name: Content-Type` / `Value: application/json` pair. The focused regression passed, and all 149 Release CTest cases passed with the live session present. Rebuilt Fairyfly returned `[REDACTED]` for direct `get`; JSON and Markdown contained no marker. The ordinary control pair remained readable. The unsent editor was cleared, independent scripting confirmed the marker absent, and the GUI session and saved connection were closed.

---

### [OPS-020] Fresh RAP OData V2 Single-Entity Format Comparison

- **Status**: CONTROLLED LIVE COMPARISON PASSED; HISTORICAL 404 CAUSE UNPROVEN, NO CURRENT REPRODUCTION
- **Observation**: A new disposable managed RAP service `ZFFLYO260927_BIND` was activated and published through Bigfox ADT. Its ADT publication request required an `adtcore:objectReferences` body and `Accept: application/vnd.sap.as+xml`; publication returned `SEVERITY=OK`. A POST created `Entries('API00002')` and its JSON response, a collection GET, and an Atom GET all confirmed the same persisted `Id` and `Note`. The exact single-entity URI then returned HTTP 200 with `Accept: application/atom+xml` and with `Accept: application/json` in the same authenticated cookie session. `$format=atom` and `$format=json` also returned 200; Atom's edit link and JSON metadata URI both pointed to `Entries('API00002')`. Fairyfly's `/IWFND/GW_CLIENT` GET of the service's JSON collection returned `~status_code=200` and JSON content type. The row was deleted with HTTP 204.
- **Cleanup and implication**: Unpublish returned `SEVERITY=OK`. ADT deleted the binding, service definition, both behavior definitions, both CDS views, and table in dependency order; exact-name searches for SRVB, SRVD, BDEF, DDLS, and TABL each returned `[]`. Fairyfly closed the GUI session and saved connection, and both `list` and `connections` returned zero. The earlier JSON-only 404 in `ZFFLYR_BIND260927` did not recur with this fresh stack. Its cause remains unproven; a difference in the old behavior pool or service model, or an intermittent Gateway fault, remains possible. If the failure recurs, capture the failed transaction ID, complete service-model metadata, and both Gateway logs before cleanup, then compare the service model with this fresh stack. The erpl-adt gap in creating BDEF/SRVD/SRVB through `object create` was filed as [issue #64](https://github.com/DataZooDE/erpl-adt/issues/64).
- **erpl-adt follow-up (2026-09-27)**: [Issue #64](https://github.com/DataZooDE/erpl-adt/issues/64) is closed. Local `uvx erpl-adt --version` reports `2026.09.27`, and `object create --help` now advertises BDEF/BDO, SRVD/SRV, and SRVB/SVB creation with the required service-binding flags. A later fresh Bigfox RAP stack verified those create calls live; see OPS-024.

---

### [ERR-112] SAP GUI Login Required an External Script Host

- **Status**: FIXED AND VERIFIED LIVE IN SM59 AND SU01
- **Symptom**: Fairyfly could launch a native SAP GUI session and fill individual controls, but the authenticated integration flow used `cscript.exe` and a VBScript helper to submit `trial.env` credentials. That added a separate script-host dependency and left the login outcome outside Fairyfly's command response.
- **Red-green fix and verification**: Login parser and outcome regressions failed to compile before the native implementation. `fairyfly login --connection <id> --credentials-file trial.env` reads the local colon-separated file, checks for a three-digit client, requires the SAP `S000` logon form, fills its four controls, presses Enter, and verifies the authenticated SAP user. `--credentials-stdin` accepts the same format and an optional `New Password` for SAP's forced first-login change, including PowerShell's UTF-8 BOM. It returns `NOT_LOGON_SCREEN` for an already authenticated session and never places passwords in arguments or output. Early live runs caught and corrected an invalid screen-read row limit, a missing-field COM exception, and a false negative after the forced password change; the final check uses SAP's authenticated `Info.User`. Native Bigfox login completed in about 0.75 seconds. The rebuilt CLI passed the seven-check authenticated SM59 run, a disposable SU01 create/read/change/delete workflow, and a separate changed-password authentication in a second live session. All 156 Release CTest cases passed (five SAP-dependent skips), as did the five offline SM59 runner tests and the SU01 mock contract. Both login VBScript helpers were removed.

---

### [OPS-021] Unsent Gateway Header Pair Masked Live

- **Status**: LIVE CREDENTIAL AND ORDINARY CONTROL VERIFIED
- **Observation**: In a fresh Bigfox `/IWFND/GW_CLIENT` session, the unsent Add Header dialog exposed `txtIP_HEADER_NAME` and `txtIP_HEADER_VALUE`. Fairyfly filled `Authorization` and a synthetic marker. Its direct `get` returned `[REDACTED]`; JSON and Markdown screen reads omitted the marker. Changing the same fields to `Content-Type` and `application/json` preserved the ordinary value in direct `get`, JSON, and Markdown. Both fields were cleared, the dialog was cancelled without adding a header or sending a request, and Fairyfly closed the session and saved connection. The literal paired layouts were later verified live in ERR-127.

---

### [ERR-113] Full Screen Read Left a Different SAP Tab Active

- **Status**: FIXED AND VERIFIED LIVE IN SP01
- **Symptom**: On SP01, `screen read` expanded both Spool requests and Output requests. It left Output requests active even when Spool requests was selected before the read. The next direct `fill` for a Spool requests field failed with `ELEMENT_NOT_FOUND` because that field was no longer present in the active tab.
- **Red-green fix**: A regression for capturing and restoring the selected tab failed to link before `TabSelectionSnapshot` was implemented. The reader now gets each `GuiTabStrip.SelectedTab` before expansion and reselects the original tabs afterward, in reverse strip order. If it cannot capture or restore the original selection, it reports an error rather than silently leaving a different tab active. SAP's scripting API documents `SelectedTab` as the currently selected `GuiTab` and `GuiTab.Select()` as the operation that may trigger server communication.
- **Live verification**: On Bigfox SP01, Fairyfly could read the Spool requests title field before and after a full two-tab JSON screen read. Repeating from Output requests likewise left its title field accessible after the read. Both expanded reads returned success with two captured tabs. The full 162-case Release CTest run passed in the live GUI environment with no skips.
- **API reference**: [SAP GuiTabStrip.SelectedTab](https://help.sap.com/docs/help/b47d018c3b9b45e897faf66a6c0885a8/30628a569e3a49288a80c4d73e3de908.html?locale=en-US) and [SAP GuiTab.Select](https://help.sap.com/docs/sap_gui_for_windows/b47d018c3b9b45e897faf66a6c0885a8/c69e7c70480d4445942eea2d6a40f2e1.html).

---

### [IMP-001] Targeted Screen Search Avoids Full Value Extraction

- **Status**: IMPLEMENTED AND VERIFIED LIVE IN SE11 AND LOGON SCREEN
- **Before**: `screen read --id-contains ... --first` applied its filter after discovering and extracting the entire screen. On a live Bigfox SE11 TADIR display, the filtered read returned one title control but still processed 77 elements and took 1,190 ms inside Fairyfly in one comparison.
- **Red-green change**: New focused tests failed to compile before `ScreenFindOptions`, query matching, and `ScreenReader::find` existed. `screen find` now matches ID, name, and type while traversing the current window; it stops at the requested match count or a 500-control scan cap and reads text only for matching simple controls. Grid and tree content extraction is skipped. A live Markdown output check exposed that the ordinary screen formatter omitted a single title match; a new regression failed, then a search-specific formatter made the match visible. All 159 Release CTest cases pass, with five SAP-dependent skips when no GUI session is open; a run with a live SM59 session also passed without skips.
- **Live check**: On the same SE11 TADIR display, `screen find --id-contains /titl` returned the title in 90 ms inside Fairyfly; a missing ID scanned 77 controls in 323 ms. On SE11's initial screen, ID and case-insensitive name searches both found `RSRD1-TBMA_VAL` in about 126 ms versus 451 ms for the corresponding filtered full read. A synthetic marker in a second unsent SAP logon password field was returned as `[REDACTED]` by `screen find`, and the marker was absent from its JSON. A later live SM59 query found two `GuiShell` controls by identity without node content; Markdown output displayed the title match after the formatter fix. The password field was cleared, all owned GUI sessions were closed, and `list` reported zero sessions. These are one-sample timings on two screens, not a general benchmark.
- **Repeated live comparison and boundary**: Four alternating-order title-match pairs on each read-only initial screen showed Fairyfly engine times of 64–74 ms versus 431–468 ms on SE11, 53–72 ms versus 471–487 ms on SM59, and 62–66 ms versus 335–364 ms on SU01 (`screen find` versus filtered `screen read --no-tabs`). On SE16's TADIR result screen, a missing ID scanned 500 controls, returned no match, and set `scan_limit_reached=true` in 1,139 ms. A title match there stopped after 32 controls in 129 ms versus 4,230 ms for the full filtered read (one pair). The full reader groups table labels, so its returned-element count is not directly comparable to the search candidate count. The read-only GUI session was closed and `list` again reported zero sessions. The 500-control limit is documented behavior; a target beyond it requires direct ID access or moving to a screen position where it is discovered earlier.

---

### [ERR-114] Unquoted FieldName Pair Exposed a Credential in a GUI Editor

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: High / Credential-output leak
- **Symptom**: In an unsent `/IWFND/GW_CLIENT` URI editor, the object-like text `{FieldName:Authorization,Value:FFLY_FIELDNAME_PAIR_260927_X7}` was readable through direct `get`, JSON, and Markdown. The response filter handled valid JSON and quoted malformed JSON keys but did not classify an unquoted `FieldName` paired with a generic value.
- **Red-green fix and verification**: A regression failed with the marker visible. Malformed object-like text now fails closed for unquoted credential keys and for unquoted `Name`, `HeaderName`, `Key`, or `FieldName` values naming credentials; an ordinary `{FieldName:Content-Type,Value:application/json}` remains readable. The focused regression passed with four assertions, the 23-case privacy group passed with 124 assertions, and all 163 Release CTest cases passed with a live SAP GUI session and no skips. On that same unsent editor, rebuilt Fairyfly returned `[REDACTED]` in direct `get`, JSON, and Markdown with no marker. The ordinary pair remained visible in all three formats. The editor was cleared without sending a request; `get` showed no marker, and the GUI session and saved connection were closed with both counts at zero. This checks response-text handling, not separate GUI controls named `FIELDNAME` or `HEADERNAME`.

---

### [ERR-115] Unicode-Escaped Credential Names Bypassed Malformed-Object Redaction

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: High / Credential-output leak
- **Symptom**: A malformed object-like editor value such as `{FieldName:Authoriz\u0061tion,Value:FFLY_ESCAPED_PAIR_260927}` did not match the sensitive-name classifier. A red regression returned the marker verbatim. A second red assertion found the same bypass in a credential key such as `{access_tok\u0065n:...}`.
- **Red-green fix and verification**: The malformed-object fallback decodes ASCII JSON-style `\uXXXX` sequences while classifying names and keys, and accepts those sequences in quoted and unquoted key scans. It suppresses both credential forms while preserving `{FieldName:Content-\u0054ype,Value:application/json}`. The focused regression passed with four assertions. On two fresh, unsent Bigfox Gateway Client editor values, Fairyfly `fill` accepted the escaped name and then the escaped key; rebuilt direct `get`, JSON, and Markdown returned `[REDACTED]` with neither synthetic marker. The escaped ordinary `Content-Type` control remained readable in all three formats. All 164 Release CTest cases passed with a live GUI session and no skips. Each editor value was cleared without sending a request, the session was closed, and `list` and `connections` both returned zero. Separate GUI field-pair layouts were unverified at that point; indexed `HEADERNAME` and `FIELDNAME` layouts were later verified in ERR-127.
- **Escaped generic-key follow-up**: A later red regression showed `{FieldN\u0061me:Authorization,Value:...}` still bypassed the malformed-object fallback, as did a truncated quoted form. One parser change passed those cases but broke an existing nested malformed-JSON regression because its match consumed the next object's opening brace; the final object-pair scan stops before nested arrays and objects. The six-assertion focused regression and all 130 assertions in the privacy group passed. On a fresh unsent Gateway Client editor, rebuilt direct `get`, JSON, and Markdown masked a synthetic value paired with `FieldN\u0061me:Authorization`, while preserving `FieldN\u0061me:Content-Type` with `application/json` in all three outputs. All 164 Release CTest cases passed with the live session present. The editor was cleared without sending a request, the session was closed, and `list` and `connections` reported zero. Separate GUI field-pair layouts were unverified at that point; indexed `HEADERNAME` and `FIELDNAME` layouts were later verified in ERR-127.

---

### [OPS-022] SE14 Failure, Recovery, and Reverse Conversion Reproduced

- **Status**: LIVE REPRODUCTION COMPLETED; PRIOR REVERSE-CONVERSION ERROR DID NOT RECUR
- **Observation**: A fresh disposable `$TMP` table `ZFFLYE3_260927` held one row with `NOTE=BADVALUE` in a `CHAR(8)` field. Changing it to `INT4` cancelled ADT activation with `Type changed from NVARCHAR to INTEGER`. Fairyfly SE14 showed Revised, an existing database table, `Persist data=true`, and `Delete data=false`. `Activate and adjust database` raised `CONVT_NO_NUMBER` in `RADGENREP` / `CONVERT_ITAB_CONTENT`. Analyze Adjustment showed step 5 cancelled, the QCM temporary table present, and the original table absent. A disposable class changed only the QCM row to `12345678`; `Continue Adjustment` succeeded, and an ADT class-run read one row with integer value `12345678`.
- **Reverse and cleanup**: Changing `NOTE` back to `CHAR(8)` cancelled ordinary ADT activation with `INTEGER to NVARCHAR`, as expected. Before reverse adjustment, Fairyfly again showed Revised, an existing database table, `Persist data=true`, and `Delete data=false`. `Activate and adjust database` succeeded. Fresh ADT read `NOTE : abap.char(8)`, and the class still returned `rows=1 note=12345678`. The class deleted the row and verified `rows=0`; Fairyfly closed its session; ADT deleted the class and table; exact-name searches returned `[]` for both; `list` and `connections` returned zero. The earlier OPS-015 inconsistency did not reproduce even with the same conversion failure and recovery steps. Its precise historical cause is unproven, so it is retained here as an observation rather than an active reproducible bug. If it recurs, capture SE14 database-utility state and logs before cleanup.

---

### [IMP-002] JSON and Quiet Logging Defaults

- **Status**: IMPLEMENTED AND VERIFIED LIVE IN SM59
- **Change**: JSON was already the default output format; CLI help now states it explicitly. The default log level changed from `info` to `error`, including the fallback for an invalid environment override. Routine informational messages no longer appear on stderr by default; `--log-level info` and `--verbose` remain available when needed. A red CLI regression observed the old `info` level before the change and passed afterward.
- **Verification**: A no-flag `list` response parsed as JSON. All 164 Release CTest cases passed with a live SAP GUI session and no skips. Without `--output` or `--log-level`, Fairyfly launched Bigfox, logged in from `trial.env`, navigated to SM59, read its screen as JSON, and closed its own session; every command succeeded.
- **Integration-runner follow-up (2026-09-27)**: The retained Python SM59 and PowerShell SU01 runners no longer pass redundant `--output json` or `--log-level off` on routine calls; Markdown reads still request Markdown explicitly. Their offline contract tests were changed first and failed against the old invocation, then passed after the runners used the defaults (six Python tests and the SU01 mock workflow). The authenticated live SM59 runner passed all seven checks, including JSON and Markdown reads, a relative screenshot, and session closure. The live SU01 runner created a disposable user, read it back, changed its password, authenticated the changed password in a second session, deleted the user, and exited successfully. Fresh `list` and `connections` both returned zero.

---

### [ERR-116] CR-Only HTTP Header Lines Exposed Credentials

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: High / Credential-output leak
- **Symptom**: An unsent `/IWFND/GW_CLIENT` URI editor held `HTTP/1.1 200 OK` followed by CR-only `Authorization: Bearer FFLY_CR_ONLY_AUTH_260927` and `Content-Type: application/json` lines. Fairyfly direct `get`, JSON, and Markdown all returned the synthetic marker. The response-text filter recognized credential headers after LF and CRLF but its named-line matcher did not recognize CR as a line boundary.
- **Red-green fix and verification**: A regression for CR-only `Authorization` and `Set-Cookie` lines failed with the marker visible. The named-line matcher now accepts CR and LF boundaries while preserving ordinary `Content-Type`. The focused three-assertion test passed. On the same still-open unsent editor, the rebuilt CLI omitted the marker from direct `get`, JSON, and Markdown while retaining `Content-Type: application/json` in all three. The editor was cleared and checked, all 165 Release CTest cases passed with the live GUI session and no skips, and the session was closed without sending the request.

---

### [ERR-117] French Password Label Exposed a Neutral Field Value

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE SAP FORM
- **Severity**: High / Credential-output leak
- **Symptom**: A disposable SA38 selection screen assigned the caption `Mot de passe` to ordinary `GuiTextField` control `P_VALUE`. Fairyfly read the caption as the field label, but direct `get`, JSON, and Markdown all exposed a synthetic value in that field. The sensitive-name classifier did not recognize the normalized French password phrase.
- **Red-green fix and verification**: A regression for `Mot de passe`, `Nouveau mot de passe`, and a JSON key with the same label failed before the change. The classifier now recognizes `motdepasse` in field labels and structured response keys while preserving an ordinary `Flight` control. All five focused assertions passed. On the same live form, rebuilt direct `get`, JSON, and Markdown omitted the synthetic marker while retaining the visible label. All 166 Release CTest cases passed with the live SAP GUI session and no skips. The field was cleared without executing the report, the GUI session was closed, ADT deleted disposable report `ZFFLYFR260927`, and an exact-name search returned `[]`.

---

### [OPS-023] Opt-In Fallback Flag Retains Native Launch Preference

- **Status**: LIVE NATIVE-PATH BOUNDARY VERIFIED; ACTUAL FALLBACK STILL OPEN
- **Observation**: With one Bigfox session already open, a second `launch Bigfox --allow-sapshcut` attached to a distinct session (`/app/con[1]/ses[0]` versus `/app/con[0]/ses[0]`), and no new `sapshcut` process was observed. On a fresh flagged launch, Fairyfly's diagnostic trace contained `native_open_connection` and no `launch_sapshcut` step. All three test-owned sessions were closed, and a fresh `list` and `connections` showed zero. This verifies that the flag does not force the slower external process when native COM succeeds. It does not verify the fallback itself, its password transport, or ambiguity handling during a same-name launch race.

---

### [OPS-024] erpl-adt RAP Object Creation Verified Against Bigfox

- **Status**: CLOSED ISSUE #64 VERIFIED LIVE; DISPOSABLE ODATA SERVICE REMOVED
- **Observation**: Using the installed `uvx erpl-adt` CLI, a fresh `$TMP` stack created and activated transparent table `ZFFLYQ260927`, root and projection CDS views `ZI_FFLYQ260927` and `ZC_FFLYQ260927`, managed and projection BDEF objects with those view names, behavior pool class `ZBP_FFLYQ260927`, service definition `ZFFLYQ260927_SRV`, and OData V2 binding `ZFFLYQ260927_BIND`. The CLI's `object create` calls for both BDEF objects, SRVD, and SRVB succeeded; source writes and activations succeeded as well. This verifies the concrete capability that had failed before [erpl-adt issue #64](https://github.com/DataZooDE/erpl-adt/issues/64) was closed.
- **Live publication and cleanup**: ADT publish returned HTTP 200 with `SEVERITY=OK`. Authenticated `$metadata` returned HTTP 200 and exposed `Entries`, `Id`, and `Note`. Fairyfly launched and logged into Bigfox, opened `/IWFND/GW_CLIENT`, and sent a read-only GET for that metadata; its GUI response grid showed `~status_code=200` and `content-type=application/xml`. The unsent URI was cleared and the GUI session closed. ADT unpublish returned `SEVERITY=OK`; all eight disposable objects were deleted in dependency order, exact-name searches for the six distinct names returned `[]`, and the former metadata URL returned HTTP 403. Final Fairyfly `list` and `connections` both returned zero.

---

### [ERR-118] Error-Level Environment Override Printed an Info Log

- **Status**: FIXED AND VERIFIED LIVE IN SM59
- **Severity**: Low / Unexpected CLI stderr output
- **Symptom**: Setting `FFLYLOG_LEVEL=error` still wrote `Using log level from FFLYLOG_LEVEL: error` at `info` level to stderr. The message was emitted before `spdlog::set_level` applied the requested override, so an otherwise quiet command produced an informational line.
- **Red-green fix and verification**: A subprocess regression ran the built CLI with `FFLYLOG_LEVEL=error`, parsed stdout as JSON, and failed when the early `info` line appeared on stderr. The premature announcement was removed; debug logging still reports the configured level when requested. The regression passed afterward. With the override set, Fairyfly launched and logged into Bigfox, navigated to SM59, and read its screen; the logging regression passed with that session open. All 166 Release CTest cases passed with no skips, and the GUI session and saved connection were closed.

---

### [ERR-119] Portuguese Password Label Exposed a Neutral Field Value

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE SAP FORM
- **Severity**: High / Credential-output leak
- **Symptom**: A disposable SA38 report assigned the visible caption `Senha` to ordinary `GuiTextField` control `P_VALUE`. Before the fix, Fairyfly recognized that caption as the field label but direct `get`, JSON, and Markdown all returned a synthetic value in the field. The sensitive-name classifier did not recognize the Portuguese password word.
- **Red-green fix and verification**: A regression for `Senha`, `Nova senha`, and a JSON key named `Senha` failed before the change; an ordinary `Flight` control was retained as a negative case. The classifier now recognizes normalized `senha` in labels and response keys. All five focused assertions passed. On the same live form, rebuilt direct `get`, JSON, and Markdown omitted the synthetic marker and retained the visible label. All 167 Release CTest cases passed with the live session and no skips. The unsaved field was cleared, the GUI session closed, ADT deleted report `ZFFLYPT260927`, exact-name search returned `[]`, and Fairyfly `list` and `connections` both returned zero.

---

### [ERR-120] Authorization Object Metadata Was Hidden as a Credential

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE SAP FORM
- **Severity**: Low / False-positive redaction
- **Symptom**: On SE93, Fairyfly returned `[REDACTED]` for the ordinary `Authorization Object` field (`TSTCA-OBJCT`). Independent COM inspection found that field blank; the label alone triggered the credential-name rule.
- **Red-green fix and verification**: A focused COM regression supplied `S_DEVELOP` under an `Authorization Object` caption and failed with `[REDACTED]`; it also required a real `Authorization` credential field to stay masked. The input-field rule now exempts only the exact normalized caption `authorizationobject` when the technical field name is not sensitive. All four focused assertions passed. Rebuilt Fairyfly read the live blank SE93 field. On a disposable SA38 form with the caption and populated value `S_DEVELOP`, direct `get`, screen JSON, and Markdown all returned the benign value; the actual `P_VALUE` field carried the caption in its metadata. All 168 Release CTest cases passed while a live GUI session was present, with no skips. The unsaved value was cleared, the GUI session closed, ADT deleted report `ZFFLYAO260927`, an exact-name search returned `[]`, and Fairyfly `list` returned zero sessions and connections.

---

### [OPS-025] Disposable SU01 User Lock and Unlock Verified

- **Status**: LIVE OPERATION VERIFIED; DISPOSABLE USER REMOVED
- **Observation**: Fairyfly created `ZFFLK260927` in SU01 after a Display attempt proved the user absent. A new SU01 visit showed the `Lock/Unlock` toolbar action. Clicking it opened a modal with `Not locked.` and a `Locks` button. Fairyfly locked the user; SAP reported `User ZFFLK260927 locked`. A fresh SU01 visit showed `Locked by system manager.` and an `Unlock` button. After unlocking, SAP reported `User ZFFLK260927 unlocked, if this is permitted in this system`; a further fresh visit confirmed `Not locked.` rather than relying only on that conditional message.
- **Cleanup**: Fairyfly deleted the user, read `User ZFFLK260927 deleted`, and a new Display attempt returned `User ZFFLK260927 does not exist`. The GUI session and saved connection were closed. A transient connection shell with zero sessions disappeared on the next `list`; the final GUI and saved-connection counts were both zero. The generated initial password was kept in memory and was not printed or stored.

---

### [IMP-003] Credential-Free Opt-In sapshcut Fallback

- **Status**: IMPLEMENTED; NATIVE PATH VERIFIED LIVE; ACTUAL FALLBACK STILL OPEN
- **Problem**: The opt-in `sapshcut` fallback read `trial.env` during `launch` and placed the SAP password in the child process command line. This added a credential-file dependency to connection opening and exposed the password to process-command-line inspection. [SAP documents](https://help.sap.com/docs/SUPPORT_CONTENT/nwtech/3362693996.html) `-pw` as the parameter for automatic logon, so the fallback can open the logon screen without it.
- **Red-green change**: A regression requiring the parsed Windows command line to contain only the executable, quoted `-sysname`, and `-maxgui` failed to compile against the previous launcher. `ConnectionLauncher` now builds those arguments without reading `trial.env`; the CLI's separate native `login` command handles authentication. The focused test passed all 30 assertions, including spaces, quotes, backslashes, and UTF-8 in the connection name. The obsolete credential parser and `-pw`, `-user`, and `-client` arguments were removed from the fallback. CLI help and the README were updated.
- **Verification boundary**: All 168 Release CTest cases passed with a live Bigfox GUI session and no skips. A fresh Fairyfly launch used `native_open_connection`, native `login` succeeded, and SM59 opened. The session and saved connection were closed. The actual `sapshcut` branch still needs a controlled live failure of native COM to verify its GUI outcome and session selection; no claim of live fallback success is made.

---

### [ERR-121] Dutch Password Label Exposed a Neutral Field Value

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE SAP FORM
- **Severity**: High / Credential-output leak
- **Symptom**: A disposable SA38 selection screen assigned the caption `Wachtwoord` to ordinary `GuiTextField` control `P_VALUE`. Before the fix, Fairyfly recognized the caption as the field label but direct `get`, screen JSON, and Markdown all exposed `FFLY_DUTCH_SECRET_260927` from that field.
- **Red-green fix and verification**: A regression for `Wachtwoord`, `Nieuw wachtwoord`, and a JSON key named `Wachtwoord` failed with the classifier returning false. The classifier now recognizes normalized `wachtwoord` in labels and response keys; an ordinary `Flight` control remains readable. All five focused assertions passed. On the same live form, rebuilt direct `get` and screen JSON returned `[REDACTED]`, and Markdown omitted the marker while retaining the visible caption. Independent SAP GUI scripting confirmed the unsaved field was empty after `fill --clear`. All 169 Release CTest cases passed with the live session present and no skips. The session was closed, ADT deleted report `ZFFLYNL260927`, exact-name search returned `[]`, and Fairyfly reported zero GUI sessions and connections.

---

### [ERR-122] Unicode Password Labels Exposed Neutral Field Values

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE SAP FORM
- **Severity**: High / Credential-output leak
- **Symptom**: A disposable SA38 selection screen showed five ordinary text fields with password captions in Chinese (`密码`), Japanese (`パスワード`), Russian (`Пароль`), Turkish (`Şifre`), and Polish (`Hasło`). The SAP GUI preserved each caption as its field label. Before the fix, Fairyfly exposed five distinct synthetic values through direct `get`, screen JSON, and Markdown. Its existing normalizer discarded non-ASCII bytes while classifying sensitive names.
- **Red-green fix and verification**: An 11-assertion regression for those labels and JSON keys failed before the change. The classifier now checks the observed UTF-8 password terms before returning a non-sensitive result, while retaining the existing ASCII/Latin rules and an ordinary `Flight` control. The focused test passed. On the same live form, rebuilt direct `get` and JSON returned `[REDACTED]` for all five fields, and Markdown contained none of the markers. Independent SAP GUI scripting confirmed all five unsaved fields were empty after `fill --clear`. All 170 Release CTest cases passed with the live session present and no skips. Fairyfly closed the session, ADT deleted report `ZFFLYUL260927`, exact-name search returned `[]`, and final GUI and saved-connection counts were zero.

---

### [ERR-123] Escaped Unicode Password Key Leaked From Incomplete JSON

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: High / Credential-output leak
- **Symptom**: An unsent `/IWFND/GW_CLIENT` URI editor held an incomplete object with a Chinese password key expressed as `\u5bc6\u7801` and a synthetic value. SAP GUI exposed the key without surrounding quotes. Fairyfly direct `get`, screen JSON, and Markdown all returned the marker. The fallback decoder only converted ASCII `\uXXXX` escapes, and its unquoted-key matcher required an ASCII first character.
- **Red-green fix and verification**: A regression first failed on a quoted, truncated Unicode-escaped key; decoding its code points as UTF-8 made those assertions pass. A further regression for the exact unquoted shape observed in SAP GUI then failed, so the unquoted matcher now accepts `\uXXXX` at the start of a key. The four focused assertions pass, including a harmless escaped `Content-Type` control. On the same unsent editor, rebuilt direct `get` returned `[REDACTED]`, and neither JSON nor Markdown contained the marker. A live unquoted escaped `Content-Type: application/json` value remained readable in all three formats. The editor was cleared without sending a request; its direct read was empty. All 171 Release CTest cases passed with the live session present and no skips. The GUI session and saved connection were closed, with both counts at zero.

---

### [ERR-124] XML Numeric Unicode Password Name Leaked a Value

- **Status**: FIXED AND VERIFIED LIVE IN GATEWAY CLIENT
- **Severity**: High / Credential-output leak
- **Symptom**: An unsent `/IWFND/GW_CLIENT` URI editor held `<Name>&#x5BC6;&#x7801;</Name><Value>FFLY_XML_UESC_260927</Value>`, where the numeric references spell the Chinese password label `密码`. Fairyfly direct `get`, screen JSON, and Markdown all exposed the synthetic value. The XML entity decoder rejected code points above ASCII.
- **Red-green fix and verification**: A regression for hexadecimal and decimal references failed before the change. The decoder now writes valid Unicode code points as UTF-8, using the same helper as JSON escape decoding, while preserving an ordinary `Content-Type` pair. All three focused assertions passed. On the same live unsent editor, rebuilt direct `get` returned `[REDACTED]`; JSON and Markdown contained no marker. Decimal references were also masked live in all three formats, and an ordinary XML `Content-Type: application/json` pair remained readable. The editor was cleared without sending a request and read back empty. All 172 Release CTest cases passed with the live session present and no skips. The GUI session and saved connection were closed, with all counts at zero.

---

### [ERR-125] Fallback Matcher Could Miss a New Session at a Reused GUI Path

- **Status**: MATCHER FIXED; LIVE FALLBACK BRANCH STILL OPEN
- **Severity**: Medium / Connection selection
- **Problem**: `ConnectionLauncher::wait_for_session` classified a candidate as pre-existing whenever its GUI collection path matched a pre-launch session. SAP can reuse `/app/con[0]/ses[0]` for a different backend session after closure; the old OR comparison would therefore discard a new candidate despite a different SAP server session key. Its timeout diagnostics also still blamed `trial.env` after the fallback stopped reading credentials.
- **Red-green fix and verification**: A focused identity regression failed to compile before the helper was added, then exposed a precedence error where COM object identity overrode different server keys. The final rule compares two present server keys first, then COM identity, and uses GUI path only when both keys are absent. All six focused assertions pass. Two successive live native Bigfox launches reused the same GUI path but produced distinct nonempty server keys, proving the relevant condition occurs on this system. A fresh native launch, login, and SM59 navigation succeeded, and all 173 Release CTest cases passed with a live GUI session and no skips. The session and saved connection were closed, with zero remaining.
- **Fallback boundary**: Direct credential-free `sapshcut -sysname=Bigfox -maxgui` invocations exited 0 but created no new scripted session within ten seconds, whether a native session was already open or not. Supplying client and user without a password also created none. A further credential-free check combined the landscape `systemid`, `-sysname=Bigfox`, client, and user; `sapshcut` exited 0 after about 22 seconds, but the GUI count remained at the one pre-existing native session. That native session still served a Gateway Client control read and was then closed; a final `list` showed zero connections and sessions. No Fairyfly fallback branch ran, so its end-to-end success, ambiguity handling, and cleanup remain unverified.
- **Security-dialog follow-up**: A Win32 inspection of a later credential-free shortcut launch found a new `sapgui.exe` process with a `SAP GUI Security` dialog. It named `/H/bigfox/S/3200`, matching the saved Bigfox landscape server, but warned that system information could not be verified and asked whether to allow the connection. Thus `sapshcut` did start SAP GUI; attachment was blocked before a scriptable session appeared. Earlier test-owned shortcut processes had the same prompt. All three prompt processes were identified by executable and window title, then closed; Fairyfly reported zero connections and sessions. No persistent trust decision was saved, and the actual Fairyfly fallback branch remains unverified.

---

### [ERR-126] Classic Report Markdown Lost Step Spacing and Indentation

- **Status**: FIXED AND VERIFIED LIVE ON A DISPOSABLE SAP REPORT
- **Severity**: Medium / Report rendering fidelity
- **Symptom**: The actual `CL_SEPMRA_DG=>execute` source emits classic-list step headers with `WRITE /`, indented messages and `done`/`error` results with `WRITE /4`, then `SKIP 2` separators. A disposable report using that same output shape showed the lines and row coordinates in live SAP GUI and Fairyfly JSON, but Fairyfly Markdown collapsed the blank rows and removed the indentation. The completed historical EPM generator result still has no retained spool, and rerunning the generator would commit changes to shared demo data.
- **Red-green fix and verification**: A formatter regression using the live label coordinates failed before the change. Markdown now preserves bounded gaps between positioned report rows and a single label's leading column within 80 columns, while retaining compact multi-label rows. The focused regression and existing ST22 positioned-label test passed. On the same live disposable report, rebuilt Markdown contained three-space-indented messages, two blank rows between steps, both `done` and `error`, and no fabricated `Status Information` section; direct `get` still returned the expected message. All 174 Release CTest cases passed with the live session present and no skips. The GUI session was closed, ADT deleted report `ZFFLYEP260927`, exact-name search returned `[]`, and final GUI and saved-connection counts were zero. The original generator-result comparison remains open because its past transient output is unavailable.

---

### [OPS-026] Initial Header-Name and Value Layout Probe

- **Status**: LIVE SE16 ALTERNATIVE CHECKED; EXACT LAYOUT LATER VERIFIED IN ERR-127
- **Observation**: A disposable DDIC table `ZFFLYHV260927` with `HEADERNAME` and `VALUE` columns was populated with `Authorization` and a synthetic marker. On live SE16, Fairyfly direct `get`, JSON, and Markdown masked the row. The same row changed to `Content-Type` and `application/json` remained readable in all three outputs. SE16 exposed classic-list labels, however, rather than two named input controls. A `SE16N` navigation attempt returned `TRANSACTION_NOT_STARTED` and left `SESSION_MANAGER` active, so this did not establish behavior for separate `HEADERNAME`/`FIELDNAME` and `VALUE` GUI fields.
- **Tooling gap and cleanup**: Classic selection-screen names cannot express those longer control names, and the current `erpl-adt` CLI has no Dynpro screen creation command. The feature request is [erpl-adt issue #66](https://github.com/DataZooDE/erpl-adt/issues/66). The disposable class deleted the table row and verified `rows=0`; Fairyfly closed its session; ADT deleted the class and table; exact-name searches returned `[]`; final GUI and saved-connection counts were zero. The exact paired-control case was subsequently reproduced and fixed in ERR-127.

---

### [OPS-027] Proxy-Authorization Header Protected in Live Gateway Dialog

- **Status**: LIVE CHECK PASSED; NO CODE CHANGE
- **Observation**: In a fresh Bigfox `/IWFND/GW_CLIENT` session, the unsent Add Header dialog held `Proxy-Authorization` in `txtIP_HEADER_NAME` and a synthetic bearer value in `txtIP_HEADER_VALUE`. Fairyfly direct `get` returned `[REDACTED]`; JSON and Markdown screen reads contained `[REDACTED]` but not the marker. Changing the same fields to `Content-Type` and `application/json` kept the ordinary value readable in direct `get`, JSON, and Markdown. This extends the live header-name check beyond `Authorization` and `X-Session-Token`; it does not verify literal `HEADERNAME`/`VALUE` controls.
- **Cleanup**: Both fields were cleared, the dialog was cancelled without adding a header or sending a request, and Fairyfly closed the GUI session and saved connection. A final `list` reported zero connections and sessions. The one-off GUI toolbar inspection script was removed. A separate fresh live session established that `screen read --no-tabs` exposes the existing synthetic `btn_ADD_HEADER` element; Fairyfly `click .../shell/btn_ADD_HEADER --wait-for-window` opened `wnd[1]` successfully. Fairyfly cancelled that dialog and closed its session; a fresh `connections` and `list` both reported zero. This is existing native CLI functionality, so no new grid-toolbar feature is required. No code changed, so the latest complete Release result remains 174/174 passing.

---

### [OPS-028] Cookie Value Masked After Insertion Into Unsent Gateway Grid

- **Status**: LIVE CHECK PASSED; NO CODE CHANGE
- **Observation**: In a fresh Bigfox `/IWFND/GW_CLIENT` session, Fairyfly opened Add Header through the existing synthetic `btn_ADD_HEADER` grid-toolbar element, filled `Cookie` with a synthetic `session=` value, and confirmed the dialog value was `[REDACTED]`. Fairyfly accepted the unsent header without executing an HTTP request. The request grid then had `NAME` and `VALUE` columns and one row, `Cookie` / `[REDACTED]`; neither JSON nor Markdown screen output contained the marker. This verifies the transition from protected modal fields to protected grid data for another standard secret-bearing header.
- **Cleanup**: Fairyfly selected row 0, clicked synthetic `btn_DELETE_HEADER`, and read back zero request-grid rows. It closed the GUI session and saved connection; final `list` reported zero connections and sessions. No code changed, so the latest complete Release result remains 174/174 passing.

---

### [ERR-127] Indexed SM30 Name/Value Controls Exposed a Credential Through Direct Get

- **Status**: FIXED AND VERIFIED LIVE IN TWO DISPOSABLE SM30 FORMS
- **Severity**: High / Credential-output leak
- **Symptom**: A generated one-step SM30 maintenance screen for disposable table `ZFFLYTM260927` exposed separate `txtZFFLYTM260927-HEADERNAME[1,0]` and `txtZFFLYTM260927-VALUE[2,0]` controls. In an unsaved row, `HEADERNAME=Authorization` and a synthetic value were present. Fairyfly direct `get` returned the marker before the fix, although full-screen JSON and Markdown masked that table row. The existing sibling-field guard required a leaf ending exactly in `VALUE`; the SAP table-control `[column,row]` suffix bypassed it.
- **Red-green fix**: A fake-COM regression for indexed rows failed with the marker visible. `ComGuiElement::get_text` now removes the table index for matching and compares only siblings in the same visible row. The regression protects `HEADERNAME`, `FIELDNAME`, and an ordinary `Content-Type` row, so a secret name in another row does not hide harmless data. The focused regression passed with six assertions and 30 repeated runs had no failures. Its fake COM controls share the type-level DISPID mapping used by real SAP text fields. The full Release CTest run passed 176/176 with a live GUI session and no skips after both fixes in this round.
- **Live verification**: Rebuilt Fairyfly returned `[REDACTED]` from direct `get` on the same unsaved `HEADERNAME` row; JSON and Markdown contained no marker. Changing that row to `Content-Type: application/json` kept the ordinary value readable in all three outputs. A second disposable generated table `ZFFLYFN260927` produced `FIELDNAME[1,0]` / `VALUE[2,0]`; rebuilt direct `get`, JSON, and Markdown masked its unsaved `Authorization` value and retained ordinary `Content-Type` in all three outputs.
- **Cleanup**: Each unsaved row was cleared and the GUI session closed. Independent ADT class runs found zero database rows before and after an exact-ID cleanup delete. SE54 removed each generated dialog; it left one stale `$TMP` FUGR directory row after deleting the backing function group and program. Guarded class runs confirmed those backing objects were absent, removed only the exact disposable TADIR rows, and read back zero. ADT deleted both verification classes and both tables; exact searches returned `[]` for the classes, tables, and function groups, and final Fairyfly session and saved-connection counts were zero.

---

### [ERR-128] Authorization Group Metadata Was Hidden as a Credential

- **Status**: FIXED AND VERIFIED LIVE IN SE54
- **Severity**: Low / False-positive redaction
- **Symptom**: On SE54's maintenance-generator screen, `ctxtTDDAT-CCLASS` is labeled `Authorization Group`. Filling the ordinary group `&NC&` succeeded, but Fairyfly direct `get` and screen output showed `[REDACTED]`. The input-field classifier treated every label containing `authorization` as a credential; ERR-120 had exempted only `Authorization Object`.
- **Red-green fix and verification**: A focused COM regression for `Authorization Group` failed with `[REDACTED]` and still required a real `Authorization` credential field to stay masked. The classifier now exempts the exact normalized metadata labels `authorizationobject` and `authorizationgroup`, after checking the technical field name. The focused test passed. On a fresh disposable SE54 table `ZFFLYAG260927`, rebuilt Fairyfly returned `&NC&` from direct `get` and showed it in JSON and Markdown. All 176 Release CTest cases passed with the live GUI session open and no skips. The unsaved field was cleared, Fairyfly closed the session, ADT deleted the table, exact search returned `[]`, and final GUI and saved-connection counts were zero.

---

### [ERR-129] Quoted sapshcut Switches Opened an Error Dialog

- **Status**: COMMAND LINE AND ENGINE ERROR PATH VERIFIED LIVE; SHORTCUT SUCCESS PATH STILL OPEN
- **Symptom**: Fairyfly built a `sapshcut` command line with `"-sysname=Bigfox" "-maxgui"`. A controlled live launch produced a `SAP GUI` error dialog: `Cannot open SAP shortcut file` and `File name does not include drive and directory name`. No scriptable session appeared. This was separate from the security prompt seen with unquoted switches.
- **Red-green fix**: A raw command-line assertion failed on the quoted switches. The launcher now leaves switch names unquoted and quotes only a connection-name value that needs Windows quoting. The existing argument-parsing cases also pass. A controlled, temporary live test checked that the launched shortcut reached a security prompt and that session polling reported `SecurityPrompt` promptly. The first live run exposed the quoted-switch bug; the second passed four assertions in about 0.5 seconds against the genuine prompt. The temporary test was removed after verification because it intentionally opens a modal prompt.
- **Suite and cleanup**: All 177 Release CTest cases passed with a native Bigfox GUI session present, with no skips. The test-owned shortcut GUI process and the native session were closed, and the native saved connection was removed. No persistent SAP GUI security decision was changed. The engine-level error response was verified in the follow-up below; a successful shortcut session remains unverified.
- **Engine fallback follow-up**: With no GUI session open, `fairyfly launch 'Bigfox ' --allow-sapshcut` failed native name matching and reached the actual fallback. The corrected shortcut reached SAP GUI Security, and the CLI returned `SAP_GUI_SECURITY_PROMPT` in 524 ms with a trace showing both `launch_sapshcut` success and `wait_for_session` failure. Fairyfly `list` and `connections` remained at zero. The sole test-owned `sapgui.exe` prompt process was identified by PID, name, and start time and closed. This verifies the error path; the successful fallback selection and ambiguity boundaries remain open until a security decision permits session creation. A second controlled shortcut run displayed `/H/bigfox/S/3200`; Win32 confirmed `Remember my decision` was unchecked before sending `Allow`. The prompt process exited, but no SAP GUI process or scriptable session remained. No persistent rule was saved, so shortcut success is still unresolved. Two further credential-free direct `sapshcut` launches supplied `-system=A4H -client=001`, then additionally `-user=DEVELOPER`; the dialog displayed `System: A4H` and `Client: 001`. Each one-time Allow decision left no SAP GUI process or scriptable session. The saved landscape still has `systemid=001`, but passing the correct SID explicitly did not make the shortcut usable. [SAPshortcut parameters](https://help.sap.com/docs/SUPPORT_CONTENT/nwtech/3362693996.html) distinguish `-system` (SID) from `-client`.

---

### [IMP-004] Info Logging No Longer Repeats Normal Polling State

- **Status**: FIXED AND VERIFIED LIVE
- **Symptom**: With no SAP session open, `--log-level info list` emitted two warnings for the normal empty collection. During a shortcut wait, each half-second poll also wrote connection counts and an empty-state warning. The CLI default was already `error`, but selecting `info` produced repetitive diagnostic noise.
- **Red-green change**: A live CLI assertion first failed on the empty-collection warnings. The collection now logs that normal state at `debug`; the shortcut launcher likewise logs per-poll counts, matching-connection details, and the temporary empty state at `debug`. Start, success, and actionable security-prompt messages retain their higher levels.
- **Verification**: The same empty-state assertion passed after rebuilding. With a live native Bigfox GUI session, `--log-level info list` retained its concise summary and returned one session; all 177 Release CTest cases passed with no skips. A controlled live fallback returned `SAP_GUI_SECURITY_PROMPT` without poll chatter. Fairyfly closed the native session and saved connection, the test-owned shortcut prompt process was closed by exact process ID, and final GUI and process checks were zero.

---

### [OPS-029] Set-Cookie Value Masked in Unsent Gateway Dialog and Request Grid

- **Status**: LIVE CHECK PASSED; NO CODE CHANGE
- **Observation**: In a fresh Bigfox `/IWFND/GW_CLIENT` session, Fairyfly opened the Add Header dialog from the request grid toolbar, populated `txtIP_HEADER_NAME=Set-Cookie` and `txtIP_HEADER_VALUE=FFLY_SET_COOKIE_SYNTH_260927`. Direct `fairyfly get` on `txtIP_HEADER_VALUE`, full-screen JSON, and full-screen Markdown all returned `[REDACTED]` with zero leakage of the synthetic marker. A paired control check with `Content-Type: application/json` preserved the value verbatim across all three outputs. Confirming the modal dialog added the row to the request grid; full-screen JSON and Markdown rendered the grid row as `Set-Cookie` / `[REDACTED]`. The request grid row was then selected (`--row 0 --column NAME`) and deleted via synthetic `btn_DELETE_HEADER`, confirming 0 request rows remaining.
- **Cleanup**: The session was disconnected with `--close-session` and the saved connection removed. `fairyfly list` and `fairyfly connections` verified 0 active sessions and connections. All temporary probe scripts were removed.

---

### [ERR-130] SapGui.ScriptingCtrl.1 Leaked COM Apartment Initialization Count

- **Status**: FIXED AND VERIFIED WITH UNIT TESTS AND CTEST
- **Severity**: Medium / COM lifecycle balance
- **Symptom**: In CTest, unit test 44 (`SAP application wrapper balances COM initialization`) failed with `REQUIRE(apartment_after == CO_E_NOTINITIALIZED)` (`0 == -2147221008`). When running in environments where `SapGui.ScriptingCtrl.1` (`sapfewse.ocx`) is instantiated via `CoCreateInstance`, the in-process OCX control internally calls `CoInitializeEx`/`OleInitialize`, incrementing the thread's COM apartment count without decrementing on release. A single `CoUninitialize()` in `ComGuiApplication::~ComGuiApplication()` left the thread apartment initialized (`S_OK`).
- **Red-green fix**: The issue was isolated with a standalone diagnostic proving that `SapGui.ScriptingCtrl.1` creation requires an extra `CoUninitialize()`, whereas `SapROTWr` does not. `ComGuiApplication` was updated to track an explicit `com_uninit_count_` (2 when instantiated via Method 2 `SapGui.ScriptingCtrl.1`, 1 when instantiated via Method 1 ROT). Destructor loops over `com_uninit_count_`, executing the exact number of `CoUninitialize()` calls required to restore the thread's initial COM apartment state. If `CoCreateInstance` succeeds in Method 2 but `GetScriptingEngine` fails, `CoUninitialize()` balances the OCX's increment before throwing.
- **Verification**: Catch2 test `SAP application wrapper balances COM initialization` passed immediately (`5 assertions in 3 test cases`). CTest with timeout passed 172/172 non-live test cases with 0 failures and 5 skipped live tests.

---

### [ERR-131] Unicode XML Element Tags, Attributes, and Multilingual Password Roots Exposed Credentials

- **Status**: FIXED AND VERIFIED WITH RED-GREEN UNIT TESTS AND LIVE GATEWAY CLIENT PROBE
- **Severity**: High / Credential-output leak
- **Symptom**: In XML payloads, element tags and attributes containing non-ASCII Unicode characters (e.g. Korean `<비밀번호>...`, Chinese `<密码>...`, Russian `<Пароль>...`, Swedish `<Lösenord>...`, namespaced `<d:비밀번호>...`, or attributes like `비밀번호="..."`) bypassed `redact_sensitive_response_text` because the XML tag and attribute regexes strictly required ASCII identifiers `[A-Za-z_][A-Za-z0-9_.:-]*`. Furthermore, multilingual password roots were missing from `contains_sensitive_utf8_name` (Korean `비밀번호`, `암호`, Swedish `lösenord`, Hungarian `jelszó`, Romanian `parolă`, Greek `κωδικός`, Arabic `كلمة المرور`, Hebrew `סיסמה`), and `is_sensitive_data_name` was missing German `passwort`/`kennwort` and Norwegian `passord`, Danish `adgangskode`, Czech `heslo`, Finnish `salasana`, Italian `paroladordine`. Neutral fields labeled with those terms and responses containing them leaked credential values unredacted.
- **Red-green fix**: Two new test cases (`Response editor suppresses XML values in Unicode element tags and attributes` and `Extended multilingual password labels mask neutral input fields`) with 22 assertions were added to `tests/unit/test_screen_reader.cpp`. Both failed in the RED phase (`is_sensitive_input_field` returned `false` for `"비밀번호"`, and `redact_sensitive_response_text` returned the XML marker verbatim). `src/include/sensitive_data.h` was updated to include the missing multilingual password roots in `contains_sensitive_utf8_name`, `is_sensitive_data_name`, and `contains_sensitive_data_name`. `src/sensitive_data.cpp` was updated to use boundary-based character matching `R"(<[ \t\r\n]*/?([^ \t\r\n/>]+)(?:[ \t\r\n/>]))"` for element tags and `R"(([^ \t\r\n/=>]+)[ \t\r\n]*=[ \t\r\n]*["'])"` for attributes, properly capturing multibyte UTF-8 names. All unit tests passed (188 assertions in 33 test cases).
- **Live verification in Gateway Client**: In a fresh Bigfox `/IWFND/GW_CLIENT` session, the unsent URI editor was filled with:
  1. `<entry><Name>Flight</Name><비밀번호>FFLY_SYNTH_LIVE_KO_260928</비밀번호></entry>`: direct `get`, full-screen JSON, and Markdown all returned `[REDACTED]` without leaking the synthetic marker.
  2. `<entry 비밀번호="FFLY_SYNTH_ATTR_KO_260928"><Name>Flight</Name></entry>`: direct `get`, JSON, and Markdown all returned `[REDACTED]`.
  3. `<entry><Name>Flight</Name><Lösenord>FFLY_SYNTH_SV_260928</Lösenord></entry>`: direct `get`, JSON, and Markdown all returned `[REDACTED]`.
  4. `<entry><Name>Flight</Name><Passwort>FFLY_SYNTH_DE_260928</Passwort></entry>`: direct `get`, JSON, and Markdown all returned `[REDACTED]`.
  5. Paired control check with `<entry><Name>Content-Type</Name><Value>application/xml</Value></entry>` preserved `application/xml` verbatim across direct `get`, JSON, and Markdown.
  The editor was cleared, session disconnected with `--close-session`, and `list` and `connections` confirmed zero active sessions and connections. Full CTest passed 174/174 non-live tests with 0 failures and 5 connection-dependent skips.

---

### [ERR-132] ScreenReader Grid Extraction Omitted GuiShell Subtype Property

- **Status**: FIXED AND VERIFIED WITH RED-GREEN UNIT TESTS AND CTEST
- **Severity**: Medium / Element metadata fidelity
- **Symptom**: In `ScreenReader::extract_grid_data_immediately`, grid elements constructed with `type` (e.g. `GuiShell`, `GuiGridView`) did not preserve the element's `subtype` property (`GridView`). When `type == "GuiShell"`, downstream consumers and Markdown formatters relying on `(type == "GuiShell" && subtype == "GridView")` failed to detect the element as a grid view unless falling back to structural heuristics.
- **Red-green fix**: Added `TEST_CASE("ScreenReader extract_grid_data preserves GuiShell subtype", "[screen][grid]")` in `tests/unit/test_com_wrapper.cpp`. In the RED phase, `REQUIRE(data.at("subtype") == "GridView")` failed with `[json.exception.out_of_range.403] key 'subtype' not found`. Moved `extract_grid_data_immediately` to public in `src/include/screen_reader.h`, and updated `src/screen_reader.cpp` to query `element->get_subtype()` when `type == "GuiShell"` and assign `grid_element["subtype"]`. In the GREEN phase, the unit test passed (`2 assertions in 1 test case`).
- **Verification**: Full Release build and CTest passed 175/175 non-live tests with 0 failures (100% pass rate).

---

### [ERR-133] ConnectionManager TOCTOU Race on Cache Update

- **Status**: FIXED AND VERIFIED WITH UNIT TESTS
- **Severity**: Medium / Multi-process cache integrity
- **Symptom**: In `ConnectionManager::create_or_update_connection` and `set_session_key`, `conn.cache_generation` was only assigned when empty (`if (conn.cache_generation.empty()) conn.cache_generation = new_cache_generation();`). If an existing connection file was updated in place or assigned a new session key, its `cache_generation` remained unchanged. A concurrent process holding a stale snapshot could delete or overwrite the connection file because the generation token matched its snapshot.
- **Fix and verification**: Changed `if (conn.cache_generation.empty())` to unconditionally rotate `conn.cache_generation = new_cache_generation();` on connection update and `set_session_key`. Added unit test `Stale connection with distinct session key is not reused for same path` in `tests/unit/test_connection_manager.cpp`. Verified with `unit_tests.exe` (183 test cases, 178 passed, 5 skipped, 1143 assertions passed).

---

### [ERR-134] Classic Report Formatter Indentation Clamped for Columns Exceeding 80

- **Status**: FIXED AND VERIFIED WITH UNIT TESTS
- **Severity**: Low / Markdown presentation fidelity
- **Symptom**: In `src/formatters/screen_markdown_formatter.cpp`, single-label classic report rows checked `if (col > 0 && col <= 80)` before indenting. If an ABAP list line started at column 81 or greater, indentation dropped to 0 spaces instead of preserving visual indentation.
- **Fix and verification**: Changed condition to indent whenever `col > 0` with `std::min(col, 80)` clamping. Added unit test `Classic report Markdown clamps column indentation exceeding 80` in `tests/unit/test_screen_reader.cpp`. All tests passed.

---

### [ERR-135] Repeated `attach` Accumulates Stale Connection Files and Triggers MULTIPLE_CONNECTIONS

- **Status**: FIXED AND VERIFIED LIVE 2026-09-29
- **Severity**: Medium / affects every multi-step workflow
- **Symptom**: Each `attach` wrote a new `fairyfly.N.con` (ids 1, 2, 4 seen) for the same session. Later commands without `--connection` failed with `MULTIPLE_CONNECTIONS` ("Found 3 connections"). `--connection 1` failed with `INVALID_CONNECTION` ("Connection 1 session no longer exists"), and one call reported `CONNECTION_NOT_FOUND` for `fairyfly.1.con`.
- **Expected**: `attach` reuses or replaces the entry for an already-attached session, and dead entries are pruned when detected.
- **Workaround**: Re-`attach` and pass the new id with `--connection`.
- **Fix and verification**: `attach` prunes other cache entries for the same session path (`ConnectionManager::prune_other_entries_for_path`, conditional delete keeps concurrently replaced entries); auto-detect and `disconnect` drop entries whose session is confirmed gone (a throw from the check keeps the file, so an unreachable SAP cannot wipe the cache). Cause confirmed: the three entries had different server session keys for the same path `/app/con[0]/ses[0]` (re-logon), and nothing pruned the old ones. Live: three entries became one (`pruned_stale: 2`), later attaches pruned nothing. `connections` shows `server_session_key` and `cache_generation`. Tests: three `[prune]` cases in `tests/unit/test_connection_manager.cpp`.

---

### [ERR-136] Status-Bar Messages Are Not Returned by click, fill, tcode, or screen read

- **Status**: FIXED AND VERIFIED LIVE 2026-09-29
- **Severity**: Medium / silent failures
- **Symptom**: `click` on Display in RZ11 for the unknown parameter `rdisp/max_wprun_time` returned `success`, while the SAP status bar showed "The parameter name is not known". Only a screenshot revealed it. A JSON `screen read` search for status-bar text found nothing.
- **Expected**: Command results and `screen read` include the status-bar text and message type (S/W/E/A/I), so callers can detect warnings and errors after an action.
- **Fix and verification**: `status_bar_json` and `attach_status_bar` (`src/action_status.cpp`) add `status_bar` (`text`, `message_type`, `changed`, `message_id`, `message_number`) to `click`, toolbar presses, `fill`, grid fills and selects, `get`, `tcode`, and `screen read`/`find`; `read_action_status` reads the active window's status bar. Existing keys are unchanged. Live: RZ11 unknown parameter returned `The parameter name is not known`, type S, message PF 724.

---

### [ERR-137] RZ11 Parameter Detail Table Cell Values Not Exposed

- **Status**: FIXED AND VERIFIED LIVE 2026-09-29
- **Severity**: Medium / missing data
- **Symptom**: On "Display Profile Parameter Details", `screen read` listed only labels ("Metadata for Parameter…", "Kernel Default", "Instance Profile") and reported 0 top-level elements. `screen find` for `GuiTextField`, `GuiCTextField`, and `GuiLabel` returned nothing, and the value cells (e.g. `rdisp/wp_no_dia` = 24) were only readable from `screen capture` images.
- **Next step**: Identify the control type behind this table and add it to the element collector.
- **Fix and verification**: `classify_shell_extraction` now sends only GridView to grid and Tree/TableTreeControl to tree; every other GuiShell is read as metadata with a generic probe (row/column count, UI Automation text pattern, `AccText`/`AccDescription`), keeps `subtype`, drops ProgID text, and renders as content or a `content unavailable` line. Live: RZ11 `rdisp/wp_no_dia` shows Kernel Default 2, Instance Profile 24, Result 24 in `screen read`. The exact shell subtype was not logged.

---

### [ERR-138] `tcode /n` Reports error Although Navigation Succeeds

- **Status**: FIXED AND VERIFIED LIVE 2026-09-29
- **Severity**: Low
- **Symptom**: `fairyfly tcode /n` returned `TRANSACTION_FAILED` ("Transaction /N does not exist", message type E), but `list` showed the session on "SAP Easy Access" afterwards. Confirmed 2026-09-29: `tcode` appears to prepend its own `/n` to the argument, so `/n` becomes an unknown transaction name while the navigation itself still succeeds.
- **Expected**: `/n` is treated as valid navigation to the start screen.
- **Fix and verification**: cause confirmed in code: `StartTransaction` in SAP GUI already prepends `/n`. `normalize_transaction_request` sends `/n` and `/nXYZ` through `SendCommand`; bare `/n` skips the tcode match; `/o`, `/i`, `/nex` return `UNSUPPORTED_OK_CODE`. Live: `tcode /n` succeeds and `tcode /nSM37` lands on SM37.

---

### [IMP-005] Grid Cell Text Filtering and CLI Consistency

- **Status**: DONE AND VERIFIED LIVE 2026-09-29
- **Observation**: `screen read --text-contains ZFFLY` on the `/IWFND/MAINT_SERVICE` service list returned 0 elements although the grid held `ZFFLY_BIND_260929`; `--text-contains` appears to match element text, not grid cell values, so a full read plus a regex was needed. Also `list` rejects `--output toon` while other commands accept it, and `screen find --limit` is capped at 100.
- **Suggested**: Let text filters search grid cells (or add a cell-search option); make `--output` consistent; document the `--limit` cap.
- **Fix and verification**: `--text-contains` also searches `table_data`, `tree_nodes`, `text_content`; `list`, `get`, `connections` accept `--output`; the `screen find --limit` range 1-100 is documented in the README. Live: `--text-contains ZFFLY` finds `ZFFLY_BIND_260929` in the Gateway service grid; `list --output toon` works.

---

### [ERR-139] No Double-Click, F2, or Key Press on GridView Rows; Menu Items Opaque

- **Status**: FIXED AND VERIFIED LIVE 2026-09-29 (menu selection not run live)
- **Severity**: Medium / blocks drill-down workflows
- **Symptom**: In the ST22 dump list, `click <grid> --row 0 --column GPROGRAM` only selected the row, and the full dump text could not be opened (the toolbar `&DETAIL` button opens only a summary popup). There is no command for Enter, F2, F3, F8, or F12. The menu bar returned `GuiMenu` entries with empty text and no children, so menu actions could not be discovered; ST22's menus include delete actions, so blind clicking was not attempted.
- **Suggested**: Add grid double-click (`--doubleclick` on row/column), a key/vkey command, and menu item enumeration with text.
- **Fix and verification**: `click --doubleclick --row --column` (`SetCurrentCell` + `DoubleClickCurrentCell`), `send-key <key>` (enter, f1-f12, shift+f1-f12, raw 0-99, else `INVALID_VKEY`), and `screen menu [--select A/B]` (enumeration selects nothing). Live: double-click on an ST22 row opened `Runtime Error Long Text` and the dump text is readable in `screen read`; `send-key f3` went back; the menu tree listed real items; `send-key bogus` returned `INVALID_VKEY`. `screen menu --select` was exercised live only for refusal under `--read-only`.

---

### [ERR-140] Checkbox State Unreadable

- **Status**: FIXED AND VERIFIED LIVE 2026-09-29
- **Severity**: Medium / missing data
- **Symptom**: `get` on `chkBTCH2170-FINISHED` returned `element_type: GuiCheckBox`, `value: "Finished"` (the label), and `screen find` returned the same label as `text`; the selected state was not available, so the SM37 status filters could not be verified before executing.
- **Suggested**: Return the ticked state (for example `selected: true/false`) from `get`, `screen find`, and `screen read`.
- **Fix and verification**: `get` and `screen find` return `selected` for GuiCheckBox and GuiRadioButton (`screen read` already did). Live on SM37: FINISHED and ABORTED `true`, PRELIM `false`. The `label` field of `get` is empty; the label is in `value`.

---

### [ERR-141] `click --wait-for-window` Waits the Full Timeout on In-Place Screen Changes

- **Status**: FIXED AND VERIFIED LIVE 2026-09-29
- **Severity**: Low / wasted time and inconsistent metrics
- **Symptom**: Pressing Job log with `--wait-for-window` opened the log in the same window and returned `waited_ms: 5000`, `window_changed: false`, while `metadata.duration_ms` was 731.
- **Suggested**: Detect screen or title changes in the main window, end the wait early, and report consistent timing.
- **Fix and verification**: `click --wait-for-window` snapshots window id, title, transaction, and status text, ends the wait on any change, reports the measured `waited_ms`, `window_changed`, `screen_changed`, and adds the wait to `duration_ms`. Live: SM37 Job log returned in 506 ms wall (`waited_ms` 8, `screen_changed: true`), previously about 5 s.

---

### [IMP-006] Faster and Smaller Reads, and a Read-Only Guard Mode

- **Status**: DONE AND VERIFIED LIVE 2026-09-29 (batch gain smaller than expected)
- **Tab-targeted read**: An SU01 `screen read` with tabs took 11.7 s and 35 KB; clicking the Roles tab (0.35 s) and reading with `--no-tabs` took about 0.95 s. Add `screen read --tab <id>`.
- **Output size**: The SM37 job list as JSON was 120 KB versus 13.6 KB as markdown, and the ST22 selection screen returned 16.8 KB, largely duplicate `%_..._%_APP_%-TEXT` label fields. Drop or collapse label duplicates.
- **Per-call overhead**: Each CLI call takes about 0.3 to 0.6 s as a separate process, and the three scenarios needed about 40 calls. Consider a batch/script mode or implementing `serve`.
- **Read-only guard**: On SM37 the Job log button sits beside Release, Stop job, and Delete job. A `--read-only` mode could refuse state-changing buttons.
- **Minor**: `screen find --type GuiTab` returns empty tab text (names appear in `screen read`), and the popup close button varies (`btn[0]` on ST22 details, `btn[12]` on F4), so a "close active popup" helper would help.
- **Bigfox observation (unverified)**: Four `DYN_TABLE_ILL_COMP_VAL` dumps in `CL_RSO_RES_IS_BWSEARCH` on 2026-09-29 04:49:55-58 (work process 22, user DEVELOPER), possibly caused by earlier ADT search calls.
- **Results**: `screen read --tab <id>` (SU01 Roles 2.3 s versus 11.7 s; `TAB_NOT_FOUND`); duplicate `grid_data` removed and selection-screen label carriers collapsed (SM37 JSON 120.5 KB to 98.4 KB, ST22 selection JSON 89.1 KB to 70.0 KB, Markdown 16.9 KB to 12.1 KB; field rows now show the caption such as `Date (F4 Search)` where they showed the technical name); `screen find --type GuiTab` returns labels; new `close` (F12, falls back to the window's own Close for popups, never Enter; live: closed the ST22 Details popup with `method: window_close`, second call `NO_POPUP`); new `batch` command (8 commands in about 2.0 s versus about 2.8 s separately, 1.3-1.5x over three runs, because most time is spent in SAP waits; one 23 s run not reproduced); new `--read-only` / `FAIRYFLY_READ_ONLY=1` guard (`READ_ONLY_REFUSED` verified live for Save, F11, a Delete menu path, and `fill`; SM37 Release/Stop/Delete buttons covered by unit tests only). `serve` remains unimplemented. Unverified bullet below (ST22 dumps) stays open.

---

### [ERR-142] Tab Reads Used a Stale Element and Skipped Grids

- **Status**: FIXED AND VERIFIED LIVE 2026-09-29
- **Severity**: Medium / wrong and bloated output, slow
- **Symptom**: Found by a profiling pass on SU01. After `tab.select()` the pre-select COM pointer yielded nothing in `traverse_element_tree`, so the fallback re-read the whole `/usr` area: every expanded tab's `elements` held all tab headers, the base fields and the tab's own controls (25-74 elements per tab, 549 KB JSON for 12 tabs). Tab sub-reads also never ran the grid/tree extraction phase, so grids inside tabs (for example the SU01 Roles list) were absent from `tabs_content`. A grid found both through `Children` and a `/shell` probe was extracted twice (`add_grid_id` had no dedupe).
- **Fix and verification**: `ScreenReader::read_tab` and `read_tab_content` re-fetch the tab after the select, extract only its subtree including grids, skip `select()` for the current tab, poll `is_busy` at 20 ms, and dedupe collector ids. `screen read --tab` reads only the requested tab. Live SU01: `--tab tabpACTG` 2.2 s to 0.55 s with the Roles grid (3 rows), full read 11.3 s to 3.9 s, 12 tabs now hold 3-51 elements each and six include grids. Documented behavior change: a tab's `elements` are its own controls only. The selected tab is restored (ERR-113).

---

### [IMP-007] Fewer COM Round Trips per Screen Read

- **Status**: DONE AND VERIFIED LIVE 2026-09-29 (SM37 list still 1.7 s)
- **Analysis**: About 0.45 ms per cross-process COM call. A plain `screen read` costs about 190 ms fixed plus discovery and extraction: ST22 selection about 2,300 calls, the 34-row SM37 list about 4,200 calls (8 per label cell), the full SU01 tab read about 9,000 wasted probe round trips. Main causes: blind `FindById` probing of every container (about 13 misses each, 3 round trips per miss), uncached failed DISPID lookups (`Visible` misses on every element, `AccLabel` on every field), no Children caching, 4-5 round trips per child through `_NewEnum`/`Skip`/`Next`, and Type/Id read again in extraction.
- **Fix**: `id_probe_candidates` gates probing by control type and skips known ids; `resolve_dispid` caches failures; `FindById` and `is_enabled` use the cached DISPID; the Children collection and count are cached per element; positioned label cells are read with one `Text` call; Phase 1 passes id and type to Phase 3 (`prime_identity`); `SapGuiCollection::for_each` walks one enumerator (used in metadata extraction only).
- **Verification**: element-ID sets are identical to a build of the pre-change code on 20 live screens; timings old to new: ST22 selection 1237 to 808 ms, SM37 list 2371 to 1700 ms, SU01 display 1383 to 711 ms, SM50 1265 to 759 ms, SE80 1375 to 658 ms. Known limits: `visible` is still `false` for every element (`Visible` is not a scripting property); `for_each` is not yet used in `screen_reader.cpp`.

---

### [ERR-143] Negative DISPID Cache Poisoned Across GuiShell Subtypes

- **Status**: FIXED AND VERIFIED LIVE 2026-09-29
- **Severity**: High / silent data loss
- **Found by**: Codex review of the speed work, reproduced on Bigfox.
- **Symptom**: `resolve_dispid` cached failed member lookups per COM Type string, and SAP reports every shell (grid, tree, toolbar, HTML viewer, calendar) as `GuiShell` although their members differ by SubType. In one process (`batch`), reading a tree screen first cached a `RowCount` miss; the next ST22 ALV grid then returned `table_data.rows` = 0 instead of 6. A fresh process returned 6.
- **Fix and verification**: no negative caching for `GuiShell` (`GuiCustomControl` and the other types keep it), plus a regression test with a tree-like and a grid-like fake of the same Type. Live: the same batch (SM59 tree, SE80 tree, then ST22 grid) returns 6 rows.

---

### [ERR-144] Failed Tab Load Reported as Success; Fallback Chosen by Child Count

- **Status**: FIXED (unit-tested)
- **Found by**: Codex review.
- **Symptom**: when `read_tab_content` failed (tab missing, or `wait_until_idle` timed out), `read_tab` and the all-tabs loop logged a warning and returned success with an empty or incomplete `tabs_content`. The user-area fallback was chosen by `get_child_count() > 0` and not by whether anything was extracted.
- **Fix**: `TabReadStatus` (Ok / NotFound / BusyTimeout); `--tab` returns `TAB_LOAD_FAILED` (`tab_id`, `reason`) after restoring the original tab; all-tabs reads add `tabs_failed` and fail only when every tab failed; the fallback runs when the extracted subtree is empty. Also fixed: `for_each` reported success after a failed `IEnumVARIANT::Next` (now returns false, callers fall back to `item(i)`).

---

### [IMP-008] Compact JSON, Probe Escape Hatch, and Enumeration in Traversal

- **Status**: DONE AND VERIFIED LIVE 2026-09-29
- **`--compact` for JSON/TOON**: hierarchy groups and per-tab `elements` become id arrays (objects live in `data.elements`), empty strings, nulls, empty arrays and `false` booleans are omitted (`id`, `type`, `name` are always kept), markers `hierarchy_format: "ids"` and `tab_elements_format: "ids"`. Default JSON and Markdown are unchanged. Live SU01 full read: 380 KB to 181 KB; `--tab` read 47 KB to 17 KB.
- **`--probe-all`**: restores the pre-gating exhaustive `FindById` probing (also `FAIRYFLY_PROBE_ALL=1`).
- **`for_each` in traversal**: one enumeration per container, user-area cell block, and window child list. Live SM37 job list 2392 to 1326 ms; element IDs identical to the pre-change build on 17 screens.
- **`visible`**: now `true` unless SAP exposes a real `Visible` property (was `false` for every element).

---

### [IMP-009] Credential Store, Audit Trail, and `launch --login`

- **Status**: IMPLEMENTED AND UNIT-TESTED; LIVE VERIFICATION BY THE ORCHESTRATOR
- **Correction**: earlier notes (CLAUDE.md, README, OPEN_WORK) said `launch` read plaintext credentials from `trial.env`. It never did: `launch` opens the connection through native COM, and the sapshcut fallback command line is exactly `"<exe>" -sysname=<name> -maxgui` (asserted in `test_connection_launcher.cpp`, now also with `-pw` and `-user` absent). Only `login --credentials-file` read the file.
- **Credential store**: `credentials set|list|delete|import-env` over the Windows Credential Manager (generic credentials, target `fairyfly:<connection name>`, blob holds the password only, metadata user/client/language). The password is prompted for or read from the first stdin line, never an argument, and is scrubbed after use. `import-env trial.env --connection Bigfox --delete-file` migrates the legacy file; the SAP password should be rotated afterwards.
- **Login sources**: `--credentials-stdin`, `--credentials-file PATH` (deprecated, warns), `--credential NAME`, or with no flag the entry named like the saved connection. Result reports `credential_source` and warnings. Errors are structured (`CREDENTIALS_NOT_FOUND`, `CREDENTIALS_PROMPT_UNAVAILABLE` inside `batch`, ...).
- **Audit trail**: one JSON record per invocation and per `batch` line in `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl` (redacted argv, SAP system/client/user/transaction, read_only, batch_line, status, error_code, exit, duration_ms). Never error messages, screen content or cell values. Controls: `--no-audit`, `--audit-required`, `FAIRYFLY_AUDIT=0|off|required`, `FAIRYFLY_AUDIT_FILE`.
- **`launch --login [--credential NAME]`**: after the session is ready, runs the same logon as `login` through the scripting API (also after `--allow-sapshcut`). Success: launch data plus a `login` object (`transaction`, `credential_source`, `warnings`). Login failure: the login error code with `connection_open: true` and the launch data under `error.launch`; the connection is not closed. The composition is the pure function `compose_launch_login_result` (unit-tested). `batch` now sets the handler's batch mode so the credential prompts are refused there.
- **Decisions**: no implicit `./trial.env` fallback; `credentials` commands are allowed under `--read-only` (they never touch SAP) but are audited; `--login` is allowed under `--read-only` (authentication, not business state); audit is on by default and never blocks a command (only `--audit-required` / `FAIRYFLY_AUDIT=required` turns a write failure into an error); no Windows user name or host name is recorded; search terms are kept in the argv.
- **Residual risks**: the password sits in a plain `std::string` during logon and is scrubbed afterwards; BSTR copies inside COM are not scrubbed; Credential Manager entries are readable by any process of the same Windows user; the audit file is not tamper-proof.
- **Integration scripts**: `test_integration.py` and `test_su01_create_user.ps1` no longer read `trial.env`; they use `login --connection <id>` with the stored credential (stdin only for the forced password change).

---

### [IMP-010] Codex Review of the First-Round Features and Live Verification of the Credential Store and Audit Trail

- **Status**: DONE AND VERIFIED LIVE 2026-09-29
- **Review**: `codex exec --sandbox read-only` reviewed the first-round patch (status bar, `tcode /n`, `batch`, `--read-only`, `close`, menu, connection pruning, redaction). Its sandbox cannot see `.git`, so the diff was supplied as `scratch/first_round.diff` after checking it for credentials. Eight findings, all confirmed and fixed with regression tests except one documented limit: `--read-only` `send-key` allowlist (Enter refused while a popup is open), double-click inspection, toolbar-button tooltip check, no echo of input-field or password text in refusals, `&` accelerator normalization plus a re-check of the resolved menu item, attach-prune race (never delete an entry holding the current live session key), NUL rejection in batch arguments, redaction of status-bar text. Not changed: `--wait-for-window` misses a click that only changes field contents (documented in the option help).
- **Live**: 21/21 checks (send-key allowlist, Enter with and without popup, menu `Sa&ve` refused, double-click still allowed, SM37 Release/Stop/Delete refused under `--read-only` with nothing pressed, Job log allowed, batch NUL rejected and the batch continues); the regression suite on the final build passed 84/84 over 3 iterations.
- **Credentials**: import from `trial.env` into Credential Manager and `login` from the store verified on Bigfox (see OPEN_WORK). Found: `login` does not handle SAP's "License Information for Multiple Logons" dialog and reports `LOGON_NOT_COMPLETED`.
- **Tooling notes**: `gh workflow run` is required to start CI on a branch (the workflow triggers only on `main` pushes, pull requests and dispatch). In the harness, a PowerShell function named `Rd` (alias of `Remove-Item`) and `Remove-Item` calls next to arguments like `/nRZ11` trigger a false 'protected path' block.

---

### [OPS-030] SEPM_REF_APPS_DG 8-Phase Report Structure Verified

- **Status**: VERIFIED WITH ADT SOURCE AUDIT AND UNIT TESTS
- **Observation**: Audited the full ABAP source of `REF_APPS_DG` and `CL_SEPMRA_DG` via ADT. Verified that `cl_sepmra_dg=>execute` performs `COMMIT WORK AND WAIT` after generating each demo entity (EPM demo users, employees, business partners, products, purchase orders, sales orders, reviews, notifications). Modeled the exact 8-phase output structure of `CL_SEPMRA_DG=>EXECUTE` with blank line step separators (`SKIP 2`) and verified that `ScreenMarkdownFormatter` preserves indentation and step separation. Added unit test `Classic report Markdown preserves full 8-phase EPM generator output structure` (13 assertions) in `tests/unit/test_screen_reader.cpp`.

---

### [OPS-031] Claude Cross-Check and Review Loop Completed

- **Status**: COMPLETED AND INTEGRATED
- **Observation**: Claude CLI 2.1.274 rate limit reset was confirmed. Successfully executed piped review sessions (`Write-Output ... | claude -p`) on `connection_manager.cpp`, `screen_markdown_formatter.cpp`, and `sensitive_data.cpp`. Review findings (cache generation rotation, column indentation clamping, and XML check stale index guard) were implemented and verified with unit tests.

---

### [OPS-032] CDS-Based OData V2 Service Created and Kept on Bigfox

- **Status**: LIVE WORKFLOW VERIFIED; OBJECTS KEPT IN `$TMP`
- **Observation**: Using `uvx erpl-adt --host bigfox` (ADT is on `bigfox:50000`, not `localhost`; password via `SAP_PASSWORD`, never a flag), created and activated CDS view entity `ZFFLY_C_FLIGHT_260929` over `SFLIGHT`, service definition `ZFFLY_SRV_260929` (`SRVD/SRV`, accepted by this `erpl-adt` version, unlike OPS-002), and OData V2 binding `ZFFLY_BIND_260929` (`SRVB/SVB`, `--binding-type ODATA --binding-version V2 --binding-category 0`). The CLI has no publish command, so publishing used `POST /sap/bc/adt/businessservices/odatav2/publishjobs?servicename=ZFFLY_SRV_260929&serviceversion=0001` with a CSRF token from `/sap/bc/adt/discovery` and `Accept: application/vnd.sap.as+xml` (`application/xml` returns 406); result `SEVERITY=OK`.
- **Verification**: The service is served under the **binding** name: `/sap/opu/odata/sap/ZFFLY_BIND_260929/$metadata` and `Flights?$top=2&$format=json` returned HTTP 200 with entity set `Flights`; the `ZFFLY_SRV_260929` and `_0001` URLs return 403. `fairyfly tcode /IWFND/MAINT_SERVICE` plus `screen read --no-tabs --max-rows 200` listed `ZFFLY_BIND_260929`.
- **erpl_web check**: DuckDB CLI with `INSTALL erpl_web FROM community; LOAD erpl_web;` and an `http_basic` secret scoped to `http://bigfox:50000` (password from `getenv('SAP_PASSWORD')`) read `odata_read('.../ZFFLY_BIND_260929/Flights?sap-client=001')`: 334 rows, typed columns (`FlightDate` timestamp, `Price` decimal(15,3), `SeatsMax` int32).
- **Cleanup**: Not performed. To remove, unpublish the binding, then delete SRVB, SRVD, and DDLS in that order (see OPS-002).
- **Tooling note**: Stale `fairyfly.N.con` files caused `MULTIPLE_CONNECTIONS`; re-`attach` and pass `--connection <id>`.

---

### [IMP-011] `login --multiple-logon` for the "License Information for Multiple Logons" dialog

- **Problem**: When the SAP user is already logged on elsewhere, SAP shows a modal dialog right after the logon Enter. `login` used to return `LOGON_NOT_COMPLETED` and leave the dialog open.
- **Dialog (observed live, 2026-09-29, `wnd[1]` of the session)**: read-only `usr/txtMULTI_LOGON_TEXT` ("User DEVELOPER is already logged on in client 001") and `usr/txtMULTI_LOGON_TEXT2` ("(terminal ..., since ...)"); radio buttons `usr/radMULTI_LOGON_OPT1` (continue and end any other logons: destructive), `usr/radMULTI_LOGON_OPT2` (continue without ending other logons), `usr/radMULTI_LOGON_OPT3` (terminate this logon, selected by default); confirm with `wnd[1]/tbar[0]/btn[0]`. Detection is by the presence of `wnd[1]/usr/radMULTI_LOGON_OPT2` or `txtMULTI_LOGON_TEXT`, not by title, and runs before the forced-password-change check.
- **Option**: `login --multiple-logon fail|keep|end|terminate` and `launch --login --multiple-logon ...` (case-sensitive lowercase, no abbreviations; `launch --multiple-logon` without `--login` is `INVALID_ARGUMENT`).
  - `fail` (default): the dialog is not touched; `LOGON_NOT_COMPLETED` now carries `error.reason = "multiple_logon_dialog"`, `error.hint` (the options) and `error.dialog` (`user`, `terminal` texts, not secrets). The dialog stays open.
  - `keep`: select OPT2, confirm, then the normal user/transaction verification; success adds `multiple_logon: {detected: true, choice: "keep"}`.
  - `terminate`: select OPT3, confirm; SAP ends the new logon and closes the session. Reported as error `MULTIPLE_LOGON_TERMINATED` with `connection_open: false` (a clean exit, not a successful login).
  - `end`: select OPT1, confirm; success adds `multiple_logon: {detected: true, choice: "end", warning: "other logons of this user were ended"}`.
- **Why `end` is never the default**: it ends the user's other logons and any unsaved data in them is lost. It must be typed explicitly and is refused with `READ_ONLY_REFUSED` (rule `login:multiple-logon-end`) under `--read-only`, before SAP is touched, although login itself is allowed under `--read-only`.
- **Implementation**: pure `plan_multiple_logon`, `make_multiple_logon_fail_error` and `make_multiple_logon_annotation` in `src/login_flow.*` (unit-tested in `tests/unit/test_login_flow.cpp`); the COM part is in `CommandHandler::handle_login`. `launch --login` passes the option through and keeps `connection_open` from the login error when it is set.
- **Status**: implemented with unit tests; live verification pending (orchestrator).
- **Live verification (2026-09-29, Bigfox)**: `fail` (default), `terminate` and `keep` verified end to end through `launch Bigfox --login`, the read-only refusal of `end` verified, `end` itself unit-tested only. Findings: the second dialog line `txtMULTI_LOGON_TEXT2` (terminal, since) is optional, so `dialog.terminal` may be empty; `dialog.user` was padded with blanks and is now trimmed. `disconnect --connection N --close-session` closes a test session cleanly.

---

### [IMP-012] MCP server (`fairyfly serve`)

- **Design**: `fairyfly serve` is an MCP server over stdio (newline-delimited JSON-RPC 2.0, nothing but protocol messages on stdout, logs on stderr). Reader thread plus a main thread that owns COM; tool calls run one at a time (queue of 16, soft 120 s timeout answered as `CALL_TIMEOUT`, `SERVER_BUSY` while a timed-out call is still running). 20 `sap_*` tools (`sap_doctor`, `sap_sessions`, `sap_connections`, `sap_attach`, `sap_launch`, `sap_login`, `sap_tcode`, `sap_screen_read`, `sap_screen_find`, `sap_get`, `sap_menu_list`, `sap_capture`, `sap_credentials_list`, `sap_click`, `sap_send_key`, `sap_close_popup`, `sap_press_f4`, `sap_menu_select`, `sap_disconnect`, `sap_batch`) plus `sap_fill` in write mode. Module map in [MCP_DESIGN.md](MCP_DESIGN.md), user guide in [MCP.md](MCP.md).
- **Decisions**:
  - Read-only guard is the default; `serve --allow-write` is opt-in and `FAIRYFLY_READ_ONLY=1` is a hard cap. Write-only tools are hidden from `tools/list`.
  - Legacy handshake only (2024-11-05 to 2025-11-25); `server/discover` answers -32601 so dual-era clients fall back to `initialize`.
  - `sap_batch` is included; every item passes the same policy, rate limit and audit path as a standalone call (one audit record per item, per-item errors, `stop_on_error`), and nesting is rejected.
  - Tools are mapped to CLI argv and dispatched through the normal command registry on the shared handler in batch mode; no command is reimplemented for MCP.
  - No credentials over MCP: no tool takes a password; logon uses the Credential Manager entry (`credentials set` in a console, then `sap_login` or `sap_launch login=true`); `multiple_logon=end` and `close_session` need write mode.
  - Hand-written protocol layer (JSON-RPC parsing, transport, server loop) on nlohmann::json instead of an MCP SDK: the needed surface is small and stdout hygiene and threading stay under our control.
  - Screen text is untrusted: screen results start with an untrusted-data header, and the server instructions say so. Results are capped at 60000 characters, images at 2 MiB (one retry at half scale).
  - The audit trail records each tool call with `audit_source: "mcp"`, `tool`, `client`, `request_id`; never results. `serve` writes `started` and `stopped` records.
- **Findings from the first live session**: a call to a tool that is hidden in read-only mode now returns `TOOL_UNAVAILABLE_READ_ONLY` with a policy message; when the audit trail is required and cannot be written, the call returns `AUDIT_UNAVAILABLE` (the action already ran); `sap_capture.scale` is numeric (0.01-1.0, or a width in pixels above 1).
- **Verification**: unit tests (protocol, catalog, dispatcher, policy, audit) and `tests/integration/mcp_smoke.ps1`, a live script that speaks JSON-RPC to a spawned server (protocol errors, read tools, read-only refusals, batch, audit assertions, write-mode fill redaction, env hard cap).
- **Status**: implemented; live verification by the orchestrator.
