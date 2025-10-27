# PowerShell script to compare VBScript and C++ outputs
# This validates that C++ implementation matches VBScript ground truth

param(
    [string]$BuildDir = "../../build/Release"
)

Write-Host "=== SAP GUI Base Objects Test - VBScript vs C++ Comparison ===" -ForegroundColor Cyan
Write-Host ""

# Run VBScript test
Write-Host "Running VBScript ground truth test..." -ForegroundColor Yellow
$vbsOutput = cscript //NoLogo test_base_objects.vbs 2>&1 | Out-String
$vbsOutput | Out-File -FilePath "vbs_output.json" -Encoding UTF8
Write-Host "VBScript output saved to vbs_output.json" -ForegroundColor Green
Write-Host ""

# Run C++ test
Write-Host "Running C++ implementation test..." -ForegroundColor Yellow
$cppExe = Join-Path $BuildDir "test_base_objects_cpp.exe"
if (-not (Test-Path $cppExe)) {
    Write-Host "ERROR: C++ test executable not found at: $cppExe" -ForegroundColor Red
    Write-Host "Please build the project first: cmake --build build --config Release" -ForegroundColor Red
    exit 1
}

$cppOutput = & $cppExe 2>&1 | Out-String
$cppOutput | Out-File -FilePath "cpp_output.json" -Encoding UTF8
Write-Host "C++ output saved to cpp_output.json" -ForegroundColor Green
Write-Host ""

# Parse JSON outputs
try {
    $vbsJson = $vbsOutput | ConvertFrom-Json
    $cppJson = $cppOutput | ConvertFrom-Json
} catch {
    Write-Host "ERROR: Failed to parse JSON outputs" -ForegroundColor Red
    Write-Host "VBScript output:" -ForegroundColor Yellow
    Write-Host $vbsOutput
    Write-Host "C++ output:" -ForegroundColor Yellow
    Write-Host $cppOutput
    exit 1
}

# Compare results
Write-Host "Comparing outputs..." -ForegroundColor Yellow
Write-Host ""

$differences = @()

# Check status
if ($vbsJson.status -ne $cppJson.status) {
    $differences += "Status mismatch: VBS='$($vbsJson.status)' vs CPP='$($cppJson.status)'"
}

# Compare object counts
$vbsCount = $vbsJson.objects.Count
$cppCount = $cppJson.objects.Count

Write-Host "VBScript found $vbsCount objects" -ForegroundColor Cyan
Write-Host "C++ found $cppCount objects" -ForegroundColor Cyan
Write-Host ""

if ($vbsCount -ne $cppCount) {
    $differences += "Object count mismatch: VBS=$vbsCount vs CPP=$cppCount"
    Write-Host "WARNING: Object counts differ!" -ForegroundColor Yellow
} else {
    Write-Host "Object counts match!" -ForegroundColor Green
}
Write-Host ""

# Compare each object
for ($i = 0; $i -lt [Math]::Min($vbsCount, $cppCount); $i++) {
    $vbsObj = $vbsJson.objects[$i]
    $cppObj = $cppJson.objects[$i]

    Write-Host "Object $i - Type: $($vbsObj.object_type)" -ForegroundColor Cyan

    # Compare fields
    $fields = @("object_type", "id", "name", "type", "type_as_number")
    foreach ($field in $fields) {
        $vbsVal = $vbsObj.$field
        $cppVal = $cppObj.$field

        if ($vbsVal -ne $cppVal) {
            $diff = "  Object[$i].$field mismatch: VBS='$vbsVal' vs CPP='$cppVal'"
            $differences += $diff
            Write-Host "  ${field}: MISMATCH - VBS='$vbsVal' vs CPP='$cppVal'" -ForegroundColor Red
        } else {
            Write-Host "  ${field}: $vbsVal" -ForegroundColor Gray
        }
    }
    Write-Host ""
}

# Summary
Write-Host "=== Summary ===" -ForegroundColor Cyan
if ($differences.Count -eq 0) {
    Write-Host "SUCCESS: All outputs match!" -ForegroundColor Green
    Write-Host "VBScript and C++ implementations are consistent." -ForegroundColor Green
    exit 0
} else {
    Write-Host "FAILURE: Found $($differences.Count) difference(s):" -ForegroundColor Red
    foreach ($diff in $differences) {
        Write-Host "  - $diff" -ForegroundColor Yellow
    }
    exit 1
}
