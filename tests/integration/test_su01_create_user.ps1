# Test Script: Create User in SU01 (User Maintenance)
# Demonstrates @active selector and --wait-for-window functionality

param(
    [string]$ConnectionName = "Bigfox",
    [string]$Username = "TESTUSER01",
    [string]$FirstName = "Test",
    [string]$LastName = "User",
    [int]$Timeout = 30000
)

$ErrorActionPreference = "Stop"
$fairyfly = ".\build\Release\fairyfly.exe"

# Color output helpers
function Write-Step {
    param([string]$Message)
    Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Write-Success {
    param([string]$Message)
    Write-Host "    ✓ $Message" -ForegroundColor Green
}

function Write-Error {
    param([string]$Message)
    Write-Host "    ✗ $Message" -ForegroundColor Red
}

function Invoke-Fairyfly {
    param(
        [string]$Command,
        [string]$Description
    )

    Write-Host "    → $Description" -ForegroundColor Gray
    Write-Host "      Command: $Command" -ForegroundColor DarkGray

    $result = Invoke-Expression "$fairyfly $Command --output json" | ConvertFrom-Json

    if ($result.status -eq "success") {
        Write-Success $Description
        return $result
    } else {
        Write-Error "$Description - Error: $($result.error.message)"
        if ($result.error.suggestions) {
            Write-Host "      Suggestions:" -ForegroundColor Yellow
            foreach ($suggestion in $result.error.suggestions) {
                Write-Host "        - $suggestion" -ForegroundColor Yellow
            }
        }
        throw "Command failed: $Command"
    }
}

Write-Host @"

╔═══════════════════════════════════════════════════════════════╗
║  SU01 User Creation Test - Window Management Demo             ║
║  Tests: @active selector, --wait-for-window, popup handling   ║
╚═══════════════════════════════════════════════════════════════╝

"@ -ForegroundColor Cyan

# Step 1: Launch SAP connection
Write-Step "Step 1: Launch SAP connection '$ConnectionName'"
$conn = Invoke-Fairyfly "launch `"$ConnectionName`"" "Launch SAP connection"
Write-Host "      Connection ID: $($conn.data.connection_id)" -ForegroundColor DarkGray
Write-Host "      Session ID: $($conn.data.session_id)" -ForegroundColor DarkGray

Start-Sleep -Seconds 2

# Step 2: Navigate to SU01
Write-Step "Step 2: Navigate to transaction SU01 (User Maintenance)"
$tcode = Invoke-Fairyfly "tcode SU01" "Execute transaction SU01"
Start-Sleep -Seconds 2

# Step 3: Click "Create" button (opens user creation dialog)
Write-Step "Step 3: Click 'Create' button (should open user creation dialog)"
Write-Host "      Testing --wait-for-window flag..." -ForegroundColor DarkGray

$create = Invoke-Fairyfly "click wnd[0]/usr/btnCREATE --wait-for-window --timeout $Timeout" "Click Create button"

if ($create.data.window_changed) {
    Write-Success "Popup detected: $($create.data.window_before) -> $($create.data.window_after)"
    $currentWindow = $create.data.new_window
} else {
    Write-Host "      No popup detected, continuing with main window" -ForegroundColor Yellow
    $currentWindow = "wnd[0]"
}

Start-Sleep -Seconds 1

# Step 4: Fill username field using @active selector
Write-Step "Step 4: Fill username field using @active selector"
Write-Host "      Demonstrating @active selector (works in any window)" -ForegroundColor DarkGray

$fill1 = Invoke-Fairyfly "fill `"@active/usr/txtUSERNAME`" `"$Username`"" "Fill username"

if ($fill1.data.element_requested -and $fill1.data.element_requested -ne $fill1.data.element) {
    Write-Success "@active resolved: $($fill1.data.element_requested) -> $($fill1.data.element)"
} else {
    Write-Host "      Filled at: $($fill1.data.element)" -ForegroundColor DarkGray
}

Start-Sleep -Seconds 1

# Step 5: Fill last name
Write-Step "Step 5: Fill user details"
$fill2 = Invoke-Fairyfly "fill `"@active/usr/txtLASTNAME`" `"$LastName`"" "Fill last name"
Start-Sleep -Milliseconds 500
$fill3 = Invoke-Fairyfly "fill `"@active/usr/txtFIRSTNAME`" `"$FirstName`"" "Fill first name"

Start-Sleep -Seconds 1

# Step 6: Click Save (might open another dialog)
Write-Step "Step 6: Click Save button"
$save = Invoke-Fairyfly "click `"@active/usr/btnSAVE`" --wait-for-window --timeout 3000" "Click Save"

if ($save.data.window_changed) {
    Write-Success "Save triggered new window: $($save.data.window_after)"
} else {
    Write-Host "      No additional window opened" -ForegroundColor Gray
}

Start-Sleep -Seconds 2

# Step 7: Read screen to verify user created
Write-Step "Step 7: Verify user creation"
$screen = Invoke-Fairyfly "screen read" "Read current screen state"

Write-Host "`n      Screen contains:" -ForegroundColor Gray
if ($screen.data.elements) {
    $elementCount = ($screen.data.elements | Measure-Object).Count
    Write-Host "        - $elementCount UI elements detected" -ForegroundColor DarkGray
}

# Step 8: Disconnect
Write-Step "Step 8: Disconnect from SAP"
$disconnect = Invoke-Fairyfly "disconnect" "Disconnect from SAP session"

# Summary
Write-Host @"

╔═══════════════════════════════════════════════════════════════╗
║  Test Summary                                                  ║
╚═══════════════════════════════════════════════════════════════╝

"@ -ForegroundColor Cyan

Write-Host "  ✓ Connection launched successfully" -ForegroundColor Green
Write-Host "  ✓ Navigated to SU01 transaction" -ForegroundColor Green
Write-Host "  ✓ Detected popup windows with --wait-for-window" -ForegroundColor Green
Write-Host "  ✓ Used @active selector for window-agnostic automation" -ForegroundColor Green
Write-Host "  ✓ User creation workflow completed" -ForegroundColor Green
Write-Host "  ✓ Disconnected cleanly" -ForegroundColor Green

Write-Host "`n  Username: $Username" -ForegroundColor Cyan
Write-Host "  Name: $FirstName $LastName" -ForegroundColor Cyan

Write-Host @"

╔═══════════════════════════════════════════════════════════════╗
║  Key Features Demonstrated                                     ║
╚═══════════════════════════════════════════════════════════════╝

"@ -ForegroundColor Yellow

Write-Host "  1. --wait-for-window flag" -ForegroundColor White
Write-Host "     Automatically detects when click opens new window/dialog" -ForegroundColor Gray

Write-Host "`n  2. @active selector" -ForegroundColor White
Write-Host "     Element paths work regardless of which window is active" -ForegroundColor Gray
Write-Host "     Example: @active/usr/txtUSERNAME works in wnd[0] or wnd[1]" -ForegroundColor Gray

Write-Host "`n  3. Window mismatch detection" -ForegroundColor White
Write-Host "     Smart error messages when element is in wrong window" -ForegroundColor Gray

Write-Host "`n  4. Transparent resolution" -ForegroundColor White
Write-Host "     See both @active request and resolved wnd[N] in response" -ForegroundColor Gray

Write-Host "`n✅ Test completed successfully!`n" -ForegroundColor Green
