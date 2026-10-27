<#
.SYNOPSIS
  Guards the supply-chain rules of .github/workflows/release.yml (CI: signing.yml; any pwsh 7, no install).
.DESCRIPTION
  The release builds and signs, so what it builds and runs must be exactly the tagged commit:
  - the release tag is github.ref_name and nothing a caller can type (no workflow_dispatch input), and the build and
    sign jobs refuse to run unless github.ref is refs/tags/<tag> (a dispatch on a branch, or a manual tag input
    pointing at another ref, would otherwise build and sign that ref with its own check scripts and signer pin);
  - every checkout is the run's commit (ref: github.sha), never a name that can move or be chosen;
  - every action is pinned to a full commit SHA with a version comment;
  - in the sign job, the ref guard and the verified WiX install (packaging/sign/Install-Wix.ps1, SHA-512 pinned)
    come before azure/login, and no tool is installed unverified anywhere in the workflow.
  Also runs Install-Wix.ps1 with a wrong pin, which must refuse (with -Install it installs WiX for real).
#>
param([switch]$Install)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$wf = Join-Path $root '.github/workflows/release.yml'
$lines = Get-Content $wf
$text = $lines -join "`n"
$failures = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS: $msg" } else { Write-Host "FAIL: $msg"; $script:failures++ }
}

# the jobs as text blocks (two-space indented keys under jobs:)
$jobs = [ordered]@{}; $current = $null; $inJobs = $false
foreach ($l in $lines) {
    if ($l -match '^jobs:') { $inJobs = $true; continue }
    if ($inJobs -and $l -match '^  ([A-Za-z0-9_-]+):\s*$') { $current = $Matches[1]; $jobs[$current] = @(); continue }
    if ($current) { $jobs[$current] += $l }
}
Check ($jobs.Contains('build') -and $jobs.Contains('sign')) "release.yml has the jobs build and sign ($($jobs.Keys -join ', '))"

Write-Host '=== the release tag cannot be chosen ==='
Check ($text -notmatch 'inputs\.tag' -and $text -notmatch 'github\.event\.inputs') 'no workflow_dispatch input feeds the tag or a checkout'
Check ($text -match "(?m)^\s+RELEASE_TAG:\s*\$\{\{\s*github\.ref_name\s*\}\}\s*$") 'RELEASE_TAG is github.ref_name'
foreach ($job in 'build', 'sign') {
    $body = $jobs[$job] -join "`n"
    Check ($body -match 'name: Release ref guard' -and $body -match 'refs/tags/') "job $job has the release ref guard (github.ref must be refs/tags/<tag>)"
}

Write-Host '=== checkouts are the run''s commit ==='
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'uses:\s*actions/checkout@') {
        $window = ($lines[$i..([Math]::Min($i + 6, $lines.Count - 1))] -join "`n")
        Check ($window -match "ref:\s*\$\{\{\s*github\.sha\s*\}\}") "checkout at line $($i + 1) uses ref: github.sha"
    }
}

Write-Host '=== actions pinned to commit SHAs ==='
foreach ($l in $lines | Where-Object { $_ -match '^\s*-?\s*uses:\s*(\S+)' }) {
    $u = $l.Trim()
    Check ($u -match 'uses:\s*[\w.-]+/[\w./-]+@[0-9a-f]{40}\s+#\s*v\d') "pinned: $u"
}

Write-Host '=== the sign job: guard and verified tools before azure/login ==='
$sign = $jobs['sign']
function IndexOf([string[]]$block, [string]$pattern) { for ($i = 0; $i -lt $block.Count; $i++) { if ($block[$i] -match $pattern) { return $i } }; return -1 }
$login = IndexOf $sign 'uses:\s*azure/login@'
$guard = IndexOf $sign 'name: Release ref guard'
$wix = IndexOf $sign 'Install-Wix\.ps1'
Check ($login -ge 0) 'the sign job logs in with azure/login'
Check ($guard -ge 0 -and $guard -lt $login) 'the ref guard runs before azure/login'
Check ($wix -ge 0 -and $wix -lt $login) 'WiX is installed (verified) before azure/login'
Check ($text -notmatch 'dotnet tool install') 'no unverified dotnet tool install in release.yml (packaging/sign/Install-Wix.ps1 instead)'

Write-Host '=== Install-Wix.ps1 ==='
$installer = Join-Path $root 'packaging/sign/Install-Wix.ps1'
Check (Test-Path $installer) 'packaging/sign/Install-Wix.ps1 exists'
if (Test-Path $installer) {
    $err = ''
    try { & $installer -ExpectedSha512 'AAAA' | Out-Host } catch { $err = $_.Exception.Message }
    Check ($err -match 'SHA-512') "a wrong pin is refused before anything is installed (got: '$err')"
    if ($Install) {
        $err = ''
        try { & $installer | Out-Host } catch { $err = $_.Exception.Message }
        Check ($err -eq '') "the pinned WiX installs (got: '$err')"
        $v = (& wix --version | Out-String).Trim()
        Check ($v -like '5.0.2*') "wix --version is 5.0.2 ($v)"
    }
}

if ($failures) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host 'all release workflow checks passed'
