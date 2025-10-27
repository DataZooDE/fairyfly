# Fairyfly Integration Tests

**Real SAP GUI Testing with Autonomous Diagnostics - NO MOCKS**

These integration tests execute the actual `fairyfly.exe` CLI against real SAP GUI connections. Every operation is traced with detailed diagnostics to enable autonomous debugging and self-sufficiency.

## Prerequisites

1. **SAP GUI** installed with scriptng enabled
2. **SAP Logon** connection configured (tested with "Bigfox")
3. **PowerShell 7.0+** (`pwsh`)
4. **fairyfly.exe** built (Release configuration)

### Enable SAP GUI Scripting

1. Open SAP Logon
2. Go to **Options** → **Accessibility & Scripting**
3. Enable **Enable scripting**
4. Restart SAP GUI if needed

### Verify Connection

In SAP Logon, verify your connection name (e.g., "Bigfox") exists:
- System Description shows the connection
- Status shows the system details (Host, System ID, etc.)

## Running Tests

### Quick Start

```powershell
cd tests\integration
.\Invoke-IntegrationTests.ps1
```

This will:
1. Build fairyfly if needed
2. Run all integration tests
3. Generate diagnostic reports
4. Show pass/fail summary

### Manual Test Execution

```powershell
# Build first
cmake --build build --config Release

# Run tests
cd tests\integration
.\Test-Integration.ps1 -FairyflyExe "..\..\build\Release\fairyfly.exe"
```

### Skip Build

```powershell
.\Invoke-IntegrationTests.ps1 -SkipBuild
```

### Verbose Output

```powershell
.\Test-Integration.ps1 -Verbose
```

## Test Coverage

### Test Suite 1: Connection Management
- **Launch Bigfox connection** - Validates `fairyfly launch Bigfox`
  - Checks connection ID is returned
  - Verifies session is created
  - Captures detailed COM invocation trace

### Test Suite 2: Transaction Execution
- **Execute SM59 (RFC Destinations)** - Validates `fairyfly tcode SM59`
  - Verifies transaction code execution succeeds
  - Captures execution timing
  - Traces method invocation steps

### Test Suite 3: Screen Data Extraction
- **Read SM59 as JSON** - Validates `fairyfly screen read --output json`
  - Extracts screen_id, title, elements
  - Validates JSON structure
  - Saves output for inspection

- **Read SM59 as Markdown** - Validates `fairyfly screen read --output markdown`
  - Generates human-readable Markdown
  - Verifies screen content is present
  - Saves output for inspection

### Test Suite 4: Cleanup
- **Disconnect** - Validates `fairyfly disconnect`
  - Ensures clean disconnection
  - Verifies no resources leak

## Generated Files

Each test run produces:

| File | Purpose |
|------|---------|
| `integration-test-YYYYMMDD-HHmmss.log` | Complete PowerShell transcript |
| `test_launch_response.json` | Connection response with diagnostics |
| `test_tcode_response.json` | Transaction execution response |
| `test_screen_sm59.json` | SM59 screen data as JSON |
| `test_screen_sm59.md` | SM59 screen data as Markdown |
| `test_diagnostics_summary.json` | Aggregated diagnostics from all tests |

### Example: test_diagnostics_summary.json

```json
{
  "TestRun": {
    "Timestamp": "2025-10-26T12:34:56Z",
    "ConnectionName": "Bigfox",
    "PassRate": 100.0,
    "TotalTests": 6,
    "Passed": 6,
    "Failed": 0
  },
  "Diagnostics": {
    "Launch 'Bigfox' connection": {
      "timing_ms": 847,
      "trace": [
        {
          "step": "initialize",
          "message": "Starting launch_connection"
        },
        {
          "step": "get_method_id",
          "method": "OpenConnection",
          "hresult": "0x00000000",
          "success": true
        },
        ...
      ]
    }
  }
}
```

## Expected Output

Successful test run produces:

```
╔════════════════════════════════════════════════════════════════════════════╗
║          Fairyfly Integration Test Suite - Real SAP GUI Testing            ║
║                    NO MOCKS - Autonomous Diagnostics                       ║
╚════════════════════════════════════════════════════════════════════════════╝

============================ PRE-FLIGHT CHECKS =============================

✅ fairyfly.exe found: ..\..\build\Release\fairyfly.exe
✅ Connection: Bigfox
✅ Log file: integration-test-20251026-123456.log
✅ Test mode: Real SAP GUI (NO MOCKS)

======================== TEST SUITE 1: CONNECTION MANAGEMENT ========================

  🔧 Executing: fairyfly.exe launch Bigfox --output json --verbose
  📤 Exit Code: 0
  ✅ PASS: Launch 'Bigfox' connection
     💬 ✓ Connection Name: Bigfox | Connection ID: con[0] | Session ID: ses[0]

  ━━━━━ DIAGNOSTICS ━━━━━
  ⏱ Duration: 847 ms
  📋 Execution Trace:
     → initialize - Starting launch_connection
     → launch_start - Bigfox
     → get_app_object - success
     → get_method_id [success] - OpenConnection
       • hresult: 0x00000000
       • dispid: 123
     → prepare_args - true, Bigfox
     → invoke_open_connection [success]
       • hresult: 0x00000000
     → get_connection_object [success]
       • connection_id: con[0]
     → check_sessions
       • session_count: 1
     → get_session [success]
       • session_id: ses[0]
     → complete
       • duration_ms: 847

======================== TEST SUITE 2: TRANSACTION EXECUTION (SM59) ========================

  ✅ PASS: Execute SM59 transaction
     💬 ✓ Transaction: SM59 executed successfully

======================== TEST SUITE 3: SCREEN DATA EXTRACTION ========================

  ✅ PASS: Read SM59 screen as JSON
  ✅ PASS: Read SM59 screen as Markdown

======================== TEST SUITE 4: CLEANUP ========================

  ✅ PASS: Disconnect from SAP
     💬 Disconnected cleanly

======================== TEST SUMMARY ========================

Total Tests:  6
Passed:       6 (100.0%)
Failed:       0 (green)

📋 Test Details:
  ✅ Launch 'Bigfox' connection
  ✅ Execute SM59 transaction
  ✅ Read SM59 screen as JSON
  ✅ Read SM59 screen as Markdown
  ✅ Disconnect from SAP

📁 Generated Files:
  📄 integration-test-20251026-123456.log
  📄 test_launch_response.json
  📄 test_tcode_response.json
  📄 test_screen_sm59.json
  📄 test_screen_sm59.md
  📄 test_diagnostics_summary.json

╔════════════════════════════════════════════════════════════════════════════╗
║  ✅ ALL INTEGRATION TESTS PASSED                                          ║
║     Real SAP GUI operations validated successfully                         ║
╚════════════════════════════════════════════════════════════════════════════╝
```

## Diagnostics for Autonomous Debugging

Each test response includes a `diagnostics` section with:

1. **Execution Trace** - Step-by-step log of all COM operations
2. **HRESULT Codes** - Windows error codes for debugging
3. **Timing Information** - Operation duration in milliseconds
4. **Parameter Values** - What was passed to each COM method
5. **State Transitions** - Connection/session creation and changes

### Example Diagnostic Trace

```json
"diagnostics": {
  "timing_ms": 847,
  "trace": [
    {
      "step": "initialize",
      "message": "Starting launch_connection"
    },
    {
      "step": "launch_start",
      "connection_name": "Bigfox"
    },
    {
      "step": "get_method_id",
      "method": "OpenConnection",
      "hresult": "0x00000000",
      "dispid": 123,
      "success": true
    },
    {
      "step": "invoke_open_connection",
      "hresult": "0x00000000",
      "success": true
    },
    {
      "step": "get_connection_object",
      "connection_id": "con[0]",
      "status": "success"
    },
    {
      "step": "check_sessions",
      "session_count": 1
    },
    {
      "step": "get_session",
      "session_id": "ses[0]",
      "status": "success"
    },
    {
      "step": "complete",
      "duration_ms": 847
    }
  ]
}
```

Claude can autonomously analyze these traces to:
- Identify exact failure points
- Map HRESULT errors to causes
- Detect timing issues
- Validate all steps completed
- Extract performance metrics

## Troubleshooting

### "Connection failed"

**Check:**
1. Connection name is correct (case-sensitive): `Bigfox` not `bigfox`
2. SAP Logon is running
3. Connection is configured in SAP Logon
4. SAP system is accessible/online

**Diagnostics:**
Review `test_diagnostics_summary.json` for the exact HRESULT from `invoke_open_connection`.

### "No sessions available"

**Check:**
1. Connection is fully established
2. SAP system logged in
3. At least one session active
4. Not connection lock timeout

**Diagnostics:**
Check `check_sessions` step shows `session_count > 0` in trace.

### "Screen read timeout"

**Check:**
1. Increase timeout in test if SAP is slow
2. Ensure transaction is fully loaded
3. Check SAP server performance
4. Look at `wait_for_session` timing

**Diagnostics:**
Review timing_ms in diagnostics - if > 5000ms, SAP may be slow.

### "JSON parse error"

**Check:**
1. Screen extraction succeeded (check JSON file exists)
2. Valid UTF-8 encoding
3. Complete JSON structure (no truncation)

**Solution:**
Run with `-Verbose` flag to see raw output.

## Performance Baseline

Expected timings for real SAP operations:

| Operation | Typical Duration |
|-----------|------------------|
| Launch connection | 500-1000 ms |
| Execute transaction | 100-500 ms |
| Read screen JSON | 200-800 ms |
| Disconnect | 50-200 ms |

If timings exceed 2x baseline, check SAP server performance.

## Key Principles

✅ **NO MOCKS** - All tests use real SAP COM API
✅ **AUTONOMOUS** - Detailed diagnostics enable self-debugging
✅ **REPRODUCIBLE** - Same results every test run
✅ **OBSERVABLE** - All inputs, outputs, and timings logged
✅ **MAINTAINABLE** - Generated diagnostics reveal issues immediately

## Next Steps

After successful integration tests:

1. Review `test_diagnostics_summary.json` for performance baseline
2. Store baseline for regression detection
3. Add more SAP transactions (SU01, SE38, etc.)
4. Implement CI/CD integration with GitHub Actions
5. Add performance monitoring and alerts
