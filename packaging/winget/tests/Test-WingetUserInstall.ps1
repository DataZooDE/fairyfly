<#
.SYNOPSIS
  End-to-end check of the winget manifests and the per-user MSI on a real Windows machine (CI: windows-2022).
.DESCRIPTION
  Renders the manifests for -MsiPath, runs `winget validate`, serves the MSI from a local HTTP server and installs it
  with `winget install --manifest` from an UNELEVATED process of the same user (Invoke-Unelevated.ps1). It then checks
  that the install is per user (folder, PATH, HKCU only, no Program Files, no HKLM uninstall entry, MSI log without
  per-machine markers), uninstalls with winget and checks that everything is gone.
  It changes the machine (winget setting LocalManifestFiles, MSI logging policy, installs and removes fairyfly):
  run it on a throw-away CI runner, not on a workstation.
.PARAMETER MsiPath
  The built fairyfly.msi.
.PARAMETER Version
  Calendar version or tag (vYYYY.MM.DD) the MSI was built for.
#>
param(
    [Parameter(Mandatory)][string]$MsiPath,
    [Parameter(Mandatory)][string]$Version,
    [string]$WorkDir = (Join-Path $env:TEMP 'ffy-winget-test'),
    [int]$Port = 8765,
    [string]$WingetRelease = 'v1.29.380'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$Version = $Version.TrimStart('v')
$MsiPath = (Resolve-Path $MsiPath).Path
$unelevated = Join-Path $here 'Invoke-Unelevated.ps1'
$generator = Join-Path $here '..\New-WingetManifest.ps1'

function Step([string]$name) { Write-Host "`n=== $name ===" }
function Pass([string]$msg) { Write-Host "PASS: $msg" }
function Assert([bool]$cond, [string]$msg) { if (-not $cond) { throw "FAIL: $msg" }; Pass $msg }
function Run-Unelevated([string]$cmd, [string]$name) {
    Write-Host "unelevated> $cmd"
    $code = & $unelevated -CommandLine $cmd -OutFile (Join-Path $WorkDir "$name.out.txt") -TimeoutSeconds 900
    return [int]$code
}

$installDir = Join-Path $env:LOCALAPPDATA 'Programs\fairyfly'
$installedExe = Join-Path $installDir 'fairyfly.exe'
$lnk = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\fairyfly (tray).lnk'
$uninstallRoots = @{
    HKLM = @('HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')
    HKCU = @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall')
}
function Find-UninstallEntries([string]$hive, [string]$productCode) {
    foreach ($root in $uninstallRoots[$hive]) {
        if (-not (Test-Path $root)) { continue }
        Get-ChildItem $root | Where-Object {
            $_.PSChildName -eq $productCode -or ($_.GetValue('DisplayName') -like 'fairyfly*')
        }
    }
}
function Get-UserPathRaw {
    $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment')
    $raw = $k.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    $k.Close(); return [string]$raw
}

Remove-Item $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$wwwDir = Join-Path $WorkDir 'www'
$manifestDir = Join-Path $WorkDir 'manifest'
New-Item -ItemType Directory -Force $wwwDir | Out-Null
Copy-Item $MsiPath (Join-Path $wwwDir 'fairyfly.msi')

Step 'Context: elevated runner, unelevated token for the install'
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "runner user $env:USERNAME, elevated administrator: $admin"
$code = Run-Unelevated 'whoami /groups /fo csv /nh' 'whoami'
$groups = Get-Content (Join-Path $WorkDir 'whoami.out.txt') -Raw
Assert ($code -eq 0) 'whoami runs with the basic-user token'
Assert ($groups -match '"BUILTIN\\Administrators"[^\r\n]*Group used for deny only') 'unelevated token: Administrators is deny-only'
Assert ($groups -match 'Mandatory Label\\Medium Mandatory Level') 'unelevated token: Medium integrity level'

Step 'winget available'
$winget = Get-Command winget -ErrorAction SilentlyContinue
$wingetOk = $false
if ($winget) {
    $v = (& $winget.Source --version | Out-String).Trim()
    Write-Host "preinstalled winget $v at $($winget.Source)"
    $wingetOk = ($v -match '^v?(\d+)\.(\d+)') -and ([int]$Matches[1] -gt 1 -or [int]$Matches[2] -ge 10)
}
if (-not $wingetOk) {
    Write-Host "installing winget $WingetRelease (App Installer) from github.com/microsoft/winget-cli"
    $dl = Join-Path $WorkDir 'winget-dl'; New-Item -ItemType Directory -Force $dl | Out-Null
    $base = "https://github.com/microsoft/winget-cli/releases/download/$WingetRelease"
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest "$base/DesktopAppInstaller_Dependencies.zip" -OutFile "$dl\deps.zip"
    Invoke-WebRequest "$base/Microsoft.DesktopAppInstaller_8wekyb3d8bbwe.msixbundle" -OutFile "$dl\winget.msixbundle"
    Expand-Archive "$dl\deps.zip" "$dl\deps" -Force
    $deps = Get-ChildItem "$dl\deps" -Recurse -Filter *.appx | Where-Object { $_.FullName -match '\\x64\\' } | ForEach-Object FullName
    Write-Host "dependencies: $($deps -join ', ')"
    Add-AppxPackage -Path "$dl\winget.msixbundle" -DependencyPath $deps -ForceApplicationShutdown
    $env:PATH = "$env:LOCALAPPDATA\Microsoft\WindowsApps;$env:PATH"
    $winget = Get-Command winget -ErrorAction Stop
}
$wingetExe = $winget.Source
$wingetVersion = (& $wingetExe --version | Out-String).Trim()
Write-Host "winget $wingetVersion at $wingetExe"

Step 'winget settings --enable LocalManifestFiles (admin setting, elevated)'
& $wingetExe settings --enable LocalManifestFiles
Assert ($LASTEXITCODE -eq 0) 'LocalManifestFiles enabled'
# verbose MSI logs for every msiexec run on this runner (written to the installing user's %TEMP%)
New-Item 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Installer' -Force | Out-Null
Set-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Installer' -Name Logging -Value 'voicewarmupx'

Step 'Render the manifests (New-WingetManifest.ps1, local installer URL)'
$installerUrl = "http://127.0.0.1:$Port/fairyfly.msi"
$info = & $generator -Version $Version -MsiPath (Join-Path $wwwDir 'fairyfly.msi') -OutDir $manifestDir -InstallerUrl $installerUrl
Get-ChildItem $manifestDir | ForEach-Object { Write-Host "--- $($_.Name)"; Get-Content $_.FullName | ForEach-Object { Write-Host "  $_" } }
$files = (Get-ChildItem $manifestDir -Filter *.yaml).Name | Sort-Object
Assert (($files -join ',') -eq 'DataZoo.fairyfly.installer.yaml,DataZoo.fairyfly.locale.en-US.yaml,DataZoo.fairyfly.yaml') "three manifest files: $($files -join ', ')"
$sha = (Get-FileHash (Join-Path $wwwDir 'fairyfly.msi') -Algorithm SHA256).Hash
$installerYaml = Get-Content (Join-Path $manifestDir 'DataZoo.fairyfly.installer.yaml') -Raw
Assert ($installerYaml -match "InstallerSha256: $sha") "InstallerSha256 is the MSI's SHA-256 $sha"
Assert ($info.ProductCode -match '^\{[0-9A-F-]{36}\}$') "ProductCode read from the MSI: $($info.ProductCode)"
Assert ($installerYaml -match [regex]::Escape("ProductCode: '$($info.ProductCode)'")) 'ProductCode in the installer manifest'
Assert ($installerYaml -match 'UpgradeCode: ''\{AEC9807E-BC74-4802-8816-25F3AB39BA29\}''') 'UpgradeCode in the installer manifest'
$productCode = $info.ProductCode

Step 'winget validate'
& $wingetExe validate --manifest $manifestDir --disable-interactivity
Assert ($LASTEXITCODE -eq 0) "winget validate exit 0 (winget $wingetVersion)"

Step 'Clean state before the install'
Assert (-not (Test-Path $installDir)) "nothing at $installDir yet"
Assert ((Get-UserPathRaw) -notlike '*Programs\fairyfly*') 'user PATH has no fairyfly entry yet'
Assert ($env:PATH -notlike '*Programs\fairyfly*') 'this process PATH has no fairyfly entry (so the later PATH check is meaningful)'

Step "Serve the MSI at $installerUrl"
$python = (Get-Command python -ErrorAction Stop).Source
$server = Start-Process $python -ArgumentList @('-m', 'http.server', "$Port", '--bind', '127.0.0.1', '--directory', $wwwDir) -PassThru -WindowStyle Hidden `
    -RedirectStandardError (Join-Path $WorkDir 'http.err.txt') -RedirectStandardOutput (Join-Path $WorkDir 'http.out.txt')
try {
    $deadline = (Get-Date).AddSeconds(30)
    while ($true) {
        try { $r = Invoke-WebRequest $installerUrl -Method Head -UseBasicParsing; if ($r.StatusCode -eq 200) { break } } catch {}
        if ((Get-Date) -gt $deadline) { throw 'local HTTP server did not come up' }
        Start-Sleep -Milliseconds 500
    }
    Pass 'HTTP server serves fairyfly.msi'

    $common = "--manifest `"$manifestDir`" --silent --disable-interactivity --accept-package-agreements"

    Step 'winget install from the ELEVATED runner must be refused (ElevationRequirement: elevationProhibited)'
    & $wingetExe install --manifest $manifestDir --silent --disable-interactivity --accept-package-agreements --scope user
    $elevatedCode = $LASTEXITCODE
    Write-Host ("elevated winget install exit code {0} = 0x{0:X8}" -f $elevatedCode)
    Assert ($elevatedCode -ne 0) 'winget refuses the install from an administrator context'
    Assert (-not (Test-Path $installedExe)) 'nothing was installed by the refused elevated attempt'

    Step 'winget install --manifest from an UNELEVATED process (same user, no admin rights)'
    $wingetLog = Join-Path $WorkDir 'msi-install.log'
    $code = Run-Unelevated "`"$wingetExe`" install $common --scope user --log `"$wingetLog`"" 'winget-install'
    Get-ChildItem $env:TEMP -Filter 'MSI*.LOG' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 3 |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $WorkDir "policy-$($_.Name)") }
    Assert ($code -eq 0) 'winget install (unelevated) exit code 0'

    Step 'Per-user install checks'
    Assert (Test-Path $installedExe) "fairyfly.exe installed at $installedExe"
    $exeVersion = (& $installedExe --version | Out-String).Trim()
    Assert ($exeVersion -eq $Version) "installed fairyfly --version = $exeVersion"

    $machinePath = [Environment]::GetEnvironmentVariable('PATH', 'Machine')
    $userPath = [Environment]::ExpandEnvironmentVariables((Get-UserPathRaw))
    Write-Host "HKCU\Environment Path = $(Get-UserPathRaw)"
    Assert ($userPath -like '*Programs\fairyfly*') 'HKCU\Environment Path contains the install folder'
    $psi = [Diagnostics.ProcessStartInfo]::new('cmd.exe', '/d /c where fairyfly && fairyfly --version')
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $psi.Environment['PATH'] = "$machinePath;$userPath"
    $p = [Diagnostics.Process]::Start($psi); $out = $p.StandardOutput.ReadToEnd() + $p.StandardError.ReadToEnd(); $p.WaitForExit()
    Write-Host "fresh process (PATH = Machine + HKCU\Environment):`n$out"
    $lines = $out -split "`r?`n" | Where-Object { $_ }
    Assert ($p.ExitCode -eq 0) 'fresh process: fairyfly found on the rebuilt PATH and --version exit 0'
    Assert ($lines[0] -ieq $installedExe) "fresh process resolves fairyfly to $($lines[0])"
    Assert ($lines[-1].Trim() -eq $Version) "fresh process: fairyfly --version = $($lines[-1].Trim())"

    $pf = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432) | Where-Object { $_ } | Select-Object -Unique
    $inPf = $pf | ForEach-Object { Get-ChildItem $_ -Filter '*fairyfly*' -Depth 1 -ErrorAction SilentlyContinue }
    Assert (-not $inPf) "nothing named fairyfly under $($pf -join ', ')"
    $hklm = @(Find-UninstallEntries 'HKLM' $productCode)
    Assert ($hklm.Count -eq 0) 'no HKLM uninstall entry'
    $hkcu = @(Find-UninstallEntries 'HKCU' $productCode)
    Assert ($hkcu.Count -eq 1 -and $hkcu[0].PSChildName -eq $productCode) "HKCU uninstall entry $($hkcu.Name)"
    Write-Host ("  DisplayName={0} DisplayVersion={1} Publisher={2} InstallLocation={3}" -f $hkcu[0].GetValue('DisplayName'), $hkcu[0].GetValue('DisplayVersion'), $hkcu[0].GetValue('Publisher'), $hkcu[0].GetValue('InstallLocation'))
    Assert (Test-Path 'HKCU:\Software\DataZoo\fairyfly') 'HKCU\Software\DataZoo\fairyfly written'
    Assert (-not (Test-Path 'HKLM:\Software\DataZoo')) 'HKLM\Software\DataZoo not written'
    Assert (Test-Path $lnk) 'Start-menu shortcut in the user profile'

    Step 'MSI log: per-user, unprivileged install'
    $logs = @($wingetLog) + @(Get-ChildItem $WorkDir -Filter 'policy-MSI*.LOG' | ForEach-Object FullName) | Where-Object { Test-Path $_ }
    Assert ($logs.Count -gt 0) "MSI logs: $($logs -join ', ')"
    $log = ($logs | ForEach-Object { Get-Content $_ -Raw }) -join "`n"
    $log -split "`r?`n" | Select-String -Pattern 'ALLUSERS|Privileged|AdminUser|user privileges|elevated privileges|per-machine|Per-machine|MSIINSTALLPERUSER|Installation success or error status|Installation completed' |
        Select-Object -First 40 | ForEach-Object { Write-Host "  log: $($_.Line.Trim())" }
    Assert ($log -match 'Installation success or error status: 0|Installation completed successfully') 'log: installation succeeded'
    Assert ($log -notmatch 'Property\([SC]\): ALLUSERS = [12]') 'log: ALLUSERS is empty (not 1 or 2)'
    Assert ($log -notmatch 'Property\([SC]\): Privileged = 1') 'log: Privileged is not set (no elevation)'
    Assert ($log -notmatch 'Property\([SC]\): AdminUser = 1') 'log: AdminUser is not set (installing user is not an admin)'
    Assert ($log -notmatch '(?i)per-machine') 'log: no per-machine markers'
    if ($log -match "Running product '[^']+' with (user|elevated) privileges[^\r\n]*") { Write-Host "  log: $($Matches[0])"; Assert ($Matches[1] -eq 'user') 'log: product ran with user privileges' }

    Step 'winget uninstall --manifest (unelevated)'
    $code = Run-Unelevated "`"$wingetExe`" uninstall $common --log `"$(Join-Path $WorkDir 'msi-uninstall.log')`"" 'winget-uninstall'
    Assert ($code -eq 0) 'winget uninstall (unelevated) exit code 0'
    Assert (-not (Test-Path $installDir)) "install folder removed: $installDir"
    Assert ((Get-UserPathRaw) -notlike '*Programs\fairyfly*') 'user PATH entry removed'
    Assert (-not (Test-Path 'HKCU:\Software\DataZoo\fairyfly')) 'HKCU\Software\DataZoo\fairyfly removed'
    Assert (@(Find-UninstallEntries 'HKCU' $productCode).Count -eq 0) 'HKCU uninstall entry removed'
    Assert (-not (Test-Path $lnk)) 'Start-menu shortcut removed'
} finally {
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}
Write-Host "`nALL CHECKS PASSED (winget $wingetVersion, ProductCode $productCode)"
