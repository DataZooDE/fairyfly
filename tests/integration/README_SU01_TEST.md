# SU01 User Creation Test Script

## Overview

This test script demonstrates the **window management features** added to fairyfly's `click` and `fill` commands:

- ✅ `@active` selector for window-agnostic automation
- ✅ `--wait-for-window` flag for automatic popup detection
- ✅ Window mismatch detection with smart error messages
- ✅ Multi-window workflow automation

## What It Does

The script automates creating a user in SAP transaction SU01 (User Maintenance), which involves:

1. **Launch connection** to SAP system
2. **Navigate to SU01** transaction
3. **Click "Create" button** → Opens user creation dialog (tests `--wait-for-window`)
4. **Fill user details** using `@active` selector (works in any window)
5. **Save user** → May trigger additional dialogs
6. **Verify creation** by reading screen state
7. **Disconnect** cleanly

## Prerequisites

- **SAP GUI** installed and configured
- **SAP connection** profile configured (e.g., "Bigfox")
- **SAP user** with authorization to create users (transaction SU01)
- **fairyfly.exe** built in `build/Release/`

## Usage

### Basic Usage

```powershell
# Run with default settings
.\tests\integration\test_su01_create_user.ps1
```

### Custom Parameters

```powershell
# Specify connection and user details
.\tests\integration\test_su01_create_user.ps1 `
    -ConnectionName "MySAPSystem" `
    -Username "TESTUSER02" `
    -FirstName "John" `
    -LastName "Doe" `
    -Timeout 30000
```

### Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `ConnectionName` | SAP connection profile name | `"Bigfox"` |
| `Username` | Username to create | `"TESTUSER01"` |
| `FirstName` | User's first name | `"Test"` |
| `LastName` | User's last name | `"User"` |
| `Timeout` | Window detection timeout (ms) | `30000` |

## Expected Output

```
╔═══════════════════════════════════════════════════════════════╗
║  SU01 User Creation Test - Window Management Demo             ║
║  Tests: @active selector, --wait-for-window, popup handling   ║
╚═══════════════════════════════════════════════════════════════╝

==> Step 1: Launch SAP connection 'Bigfox'
    → Launch SAP connection
    ✓ Launch SAP connection
      Connection ID: 0
      Session ID: ses[0]

==> Step 2: Navigate to transaction SU01 (User Maintenance)
    → Execute transaction SU01
    ✓ Execute transaction SU01

==> Step 3: Click 'Create' button (should open user creation dialog)
      Testing --wait-for-window flag...
    → Click Create button
    ✓ Click Create button
    ✓ Popup detected: wnd[0] -> wnd[1]

==> Step 4: Fill username field using @active selector
      Demonstrating @active selector (works in any window)
    → Fill username
    ✓ Fill username
    ✓ @active resolved: @active/usr/txtUSERNAME -> wnd[1]/usr/txtUSERNAME

==> Step 5: Fill user details
    → Fill last name
    ✓ Fill last name
    → Fill first name
    ✓ Fill first name

...
```

## What Gets Tested

### 1. Window Detection (`--wait-for-window`)

```powershell
fairyfly click wnd[0]/usr/btnCREATE --wait-for-window --timeout 30000
```

**Result**:
```json
{
  "status": "success",
  "data": {
    "window_changed": true,
    "window_before": "wnd[0]",
    "window_after": "wnd[1]",
    "new_window": "wnd[1]"
  }
}
```

### 2. Active Window Selector (`@active`)

```powershell
fairyfly fill "@active/usr/txtUSERNAME" "TESTUSER01"
```

**Benefit**: Works regardless of whether you're in `wnd[0]`, `wnd[1]`, or `wnd[2]`

**Result**:
```json
{
  "status": "success",
  "data": {
    "element": "wnd[1]/usr/txtUSERNAME",
    "element_requested": "@active/usr/txtUSERNAME",
    "value": "TESTUSER01",
    "window": "wnd[1]"
  }
}
```

### 3. Window Mismatch Detection

If you try to fill a field in `wnd[0]` but popup `wnd[1]` is active:

```json
{
  "status": "error",
  "error": {
    "code": "ELEMENT_NOT_FOUND",
    "window_mismatch": true,
    "requested_window": "wnd[0]",
    "active_window": "wnd[1]",
    "suggestions": [
      "Active window is wnd[1], not wnd[0] - did a popup open?",
      "Try: fairyfly fill wnd[1]/usr/txtUSERNAME \"value\"",
      "Or use: fairyfly fill @active/usr/txtUSERNAME \"value\""
    ]
  }
}
```

## Element Paths for SU01

**Note**: Actual element IDs may vary by SAP version. Use `screen read` to discover correct paths:

```powershell
fairyfly screen read --output json > su01_screen.json
```

Common SU01 elements:
- Create button: `wnd[0]/usr/btn[12]` or `wnd[0]/tbar[1]/btn[8]`
- Username field: `wnd[1]/usr/ctxtBAPIBUSE-BNAME` or similar
- First name: `wnd[1]/usr/txtBAPIUSER-FIRSTNAME`
- Last name: `wnd[1]/usr/txtBAPIUSER-LASTNAME`
- Save button: `wnd[1]/tbar[0]/btn[11]`

## Troubleshooting

### "Element not found" errors

1. **Check element paths** with `screen read`:
   ```powershell
   fairyfly screen read --output markdown
   ```

2. **Use @active selector** to avoid window issues:
   ```powershell
   fairyfly fill "@active/usr/txtFIELD" "value"
   ```

3. **Add window detection** if clicking opens popup:
   ```powershell
   fairyfly click wnd[0]/usr/btn[3] --wait-for-window
   ```

### Timeout waiting for window

Increase timeout if SAP is slow:
```powershell
-Timeout 60000  # 60 seconds
```

### Connection fails

Check SAP connection profile:
```powershell
fairyfly connections list
```

## Advanced Usage

### Chaining Commands in Workflow

```powershell
# Store window info for subsequent commands
$result = fairyfly click wnd[0]/usr/btnOPEN --wait-for-window --output json | ConvertFrom-Json

if ($result.data.window_changed) {
    $newWindow = $result.data.new_window
    Write-Host "Now in window: $newWindow"

    # Use detected window explicitly
    fairyfly fill "$newWindow/usr/txtFILE" "report.xlsx"

    # Or use @active (simpler)
    fairyfly fill "@active/usr/txtFILE" "report.xlsx"
}
```

### Error Handling

```powershell
try {
    $result = fairyfly click "@active/usr/btn[99]" --output json | ConvertFrom-Json

    if ($result.status -ne "success") {
        Write-Error $result.error.message

        if ($result.error.window_mismatch) {
            Write-Warning "Window mismatch detected!"
            Write-Host "Active window: $($result.error.active_window)"
        }
    }
} catch {
    Write-Error "Command failed: $_"
}
```

## See Also

- [Main README](../../README.md) - General fairyfly documentation
- [CLAUDE.md](../../CLAUDE.md) - Architecture and design decisions
- [ideas.md](../../ideas.md) - Detailed window management patterns
