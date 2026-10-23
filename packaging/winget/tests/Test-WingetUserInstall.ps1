<#
.SYNOPSIS
  End-to-end check of the winget manifests and the per-user MSI on a real Windows machine (CI: windows-2022).
.DESCRIPTION
  Renders the manifests for -MsiPath, runs `winget validate`, serves the MSI from a local HTTP server and installs it
  with `winget install --manifest` from an UNELEVATED process of the same user (Invoke-Unelevated.ps1). It then checks
  that the install is per user (folder, PATH, HKCU values, nothing in Program Files, registered for the user's SID and
  not per machine, MSI log without elevation or per-machine markers), uninstalls with winget and checks that
  everything is gone. On Windows Server it first shows that the default policy refuses per-user MSI installs by
  non-admins (1625) and then sets DisableMSI=0, the Windows 10/11 default.
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
    Invoke-WebRequest "$base/e53e159d00e04f729cc2180cffd1c02e_License1.xml" -OutFile "$dl\license.xml"
    Expand-Archive "$dl\deps.zip" "$dl\deps" -Force
    $deps = Get-ChildItem "$dl\deps" -Recurse -Filter *.appx | Where-Object { $_.FullName -match '\\x64\\' } | ForEach-Object FullName
    Write-Host "dependencies: $($deps -join ', ')"
    # provisioned with its license (an unlicensed Add-AppxPackage fails with "No applicable app licenses found"),
    # then registered for this user; the Appx/DISM modules do not load in PowerShell 7 here, Windows PowerShell has them
    $depList = ($deps | ForEach-Object { "'$_'" }) -join ','
    powershell.exe -NoProfile -NonInteractive -Command "`$ErrorActionPreference='Stop'; Add-AppxProvisionedPackage -Online -PackagePath '$dl\winget.msixbundle' -DependencyPackagePath $depList -LicensePath '$dl\license.xml' | Out-Null; Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe"
    if ($LASTEXITCODE -ne 0) { throw "installing App Installer failed: $LASTEXITCODE" }
    if ($env:PATH -notlike "*$env:LOCALAPPDATA\Microsoft\WindowsApps*") { $env:PATH = "$env:PATH;$env:LOCALAPPDATA\Microsoft\WindowsApps" }
    $winget = Get-Command winget -ErrorAction Stop
}
$wingetExe = $winget.Source
$wingetVersion = (& $wingetExe --version | Out-String).Trim()
Write-Host "winget $wingetVersion at $wingetExe"

Step 'winget settings --enable LocalManifestFiles (admin setting, elevated)'
& $wingetExe settings --enable LocalManifestFiles
Assert ($LASTEXITCODE -eq 0) 'LocalManifestFiles enabled'
$msiPolicyKey = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Installer'
$msiPolicy = Get-ItemProperty $msiPolicyKey -ErrorAction SilentlyContinue
Write-Host "Windows Installer machine policy: $(if ($msiPolicy) { ($msiPolicy.PSObject.Properties | Where-Object Name -notlike 'PS*' | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ', ' } else { '(not configured)' })"
$os = Get-CimInstance Win32_OperatingSystem
$isServer = $os.ProductType -ne 1
Write-Host "OS: $($os.Caption) $($os.Version), server SKU: $isServer"

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
# a real Python (the hosted tool cache), not the Microsoft Store stub in WindowsApps
$python = (Get-Command python -All -CommandType Application | Where-Object Source -notlike '*\WindowsApps\*' | Select-Object -First 1).Source
if (-not $python) { throw 'no Python for the local HTTP server' }
Write-Host "python: $python $(& $python --version 2>&1)"
$server = Start-Process $python -ArgumentList @('-m', 'http.server', "$Port", '--bind', '127.0.0.1', '--directory', $wwwDir) -PassThru -WindowStyle Hidden `
    -RedirectStandardError (Join-Path $WorkDir 'http.err.txt') -RedirectStandardOutput (Join-Path $WorkDir 'http.out.txt')
try {
    $deadline = (Get-Date).AddSeconds(30)
    while ($true) {
        try { $r = Invoke-WebRequest $installerUrl -Method Head -UseBasicParsing -TimeoutSec 5; if ($r.StatusCode -eq 200) { break } } catch { $lastError = $_.Exception.Message }
        if ((Get-Date) -gt $deadline -or $server.HasExited) {
            Write-Host "server exited: $($server.HasExited) $(if ($server.HasExited) { $server.ExitCode }); last error: $lastError"
            Get-Content (Join-Path $WorkDir 'http.err.txt'), (Join-Path $WorkDir 'http.out.txt') -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  http> $_" }
            throw 'local HTTP server did not come up'
        }
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

    # verbose MSI log (/l*v) through winget's --custom; the path has no spaces (8.3 TEMP)
    $installArgs = { param($log) "`"$wingetExe`" install $common --scope user --custom `"/l*v $log`"" }
    if ($isServer -and ($null -eq $msiPolicy -or $null -eq $msiPolicy.DisableMSI)) {
        Step 'Windows Server default: per-user MSI installs by non-admins are blocked (DisableMSI not configured)'
        $code = Run-Unelevated (& $installArgs (Join-Path $WorkDir 'msi-install-serverdefault.log')) 'winget-install-serverdefault'
        $blockedLog = (Get-Content (Join-Path $WorkDir 'msi-install-serverdefault.log'), (Join-Path $WorkDir 'winget-install-serverdefault.out.txt') -ErrorAction SilentlyContinue) -join "`n"
        $blockedLog -split "`r?`n" | Select-String 'DisableMSI|1625|policy' | Select-Object -First 8 | ForEach-Object { Write-Host "  log: $($_.Line.Trim())" }
        Assert ($code -ne 0 -and $blockedLog -match '1625') 'unelevated install refused with 1625 "forbidden by system policy" on the Server default'
        Assert (-not (Test-Path $installedExe)) 'nothing installed by the refused attempt'
        # DisableMSI=0 ("never"): Windows Installer enabled for all users, the default of Windows 10/11
        New-Item $msiPolicyKey -ErrorAction SilentlyContinue | Out-Null
        Set-ItemProperty $msiPolicyKey -Name DisableMSI -Value 0 -Type DWord
        Write-Host 'set HKLM\SOFTWARE\Policies\Microsoft\Windows\Installer DisableMSI=0 (as on Windows 10/11 clients)'
    }

    Step 'winget install --manifest from an UNELEVATED process (same user, no admin rights)'
    $wingetLog = Join-Path $WorkDir 'msi-install.log'
    $code = Run-Unelevated (& $installArgs $wingetLog) 'winget-install'
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
    $hkcu = @(Find-UninstallEntries 'HKCU' $productCode)
    foreach ($e in $hklm + $hkcu) {
        Write-Host ("  uninstall entry {0}: DisplayName={1} DisplayVersion={2} InstallLocation={3} UninstallString={4}" -f $e.Name, $e.GetValue('DisplayName'), $e.GetValue('DisplayVersion'), $e.GetValue('InstallLocation'), $e.GetValue('UninstallString'))
    }
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $packed = -join (($productCode.Trim('{}') -replace '-', '') | ForEach-Object {
        $g = $_; $parts = @($g.Substring(0, 8), $g.Substring(8, 4), $g.Substring(12, 4))
        ($parts | ForEach-Object { -join ($_.ToCharArray()[($_.Length - 1)..0]) }) -join ''
        for ($i = 16; $i -lt 32; $i += 2) { $g[$i + 1]; $g[$i] }
    })
    $userData = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Installer\UserData\$sid\Products\$packed\InstallProperties"
    Write-Host "  Windows Installer per-user registration (the service's store for this user's SID): $userData exists=$(Test-Path $userData)"
    # Windows Installer (the msiserver service, as SYSTEM) keeps the Add/Remove Programs entry of EVERY MSI product
    # under HKLM\...\Uninstall\{ProductCode}, per-user products included; the package itself never writes HKLM.
    # Per-user vs per-machine is decided by the product's registration: UserData\<user SID> (per-user) versus
    # UserData\S-1-5-18 (per-machine), and by the Installer API's AssignmentType (0 = per-user, 1 = per-machine).
    Assert ($hkcu.Count -eq 0) 'no HKCU uninstall key (Windows Installer does not use one for MSI products)'
    Assert ($hklm.Count -eq 1 -and $hklm[0].PSChildName -eq $productCode) 'the only uninstall entry is the Windows Installer ARP entry {ProductCode}'
    Assert (Test-Path $userData) "product registered for this user's SID only: UserData\$sid"
    Assert (-not (Test-Path "HKLM:\Software\Microsoft\Windows\CurrentVersion\Installer\UserData\S-1-5-18\Products\$packed")) 'no per-machine registration (UserData\S-1-5-18)'
    $msiApi = New-Object -ComObject WindowsInstaller.Installer
    $assignment = $msiApi.GetType().InvokeMember('ProductInfo', 'GetProperty', $null, $msiApi, @($productCode, 'AssignmentType'))
    $location = $msiApi.GetType().InvokeMember('ProductInfo', 'GetProperty', $null, $msiApi, @($productCode, 'InstallLocation'))
    Write-Host "  Installer API: AssignmentType=$assignment InstallLocation=$location"
    Assert ("$assignment" -eq '0') 'Installer API: AssignmentType 0 = per-user product'
    Assert (Test-Path 'HKCU:\Software\DataZoo\fairyfly') 'HKCU\Software\DataZoo\fairyfly written'
    Assert (-not (Test-Path 'HKLM:\Software\DataZoo')) 'HKLM\Software\DataZoo not written'
    Assert (Test-Path $lnk) 'Start-menu shortcut in the user profile'

    Step 'MSI log: per-user, unprivileged install'
    $logs = @($wingetLog) | Where-Object { Test-Path $_ }
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
    $code = Run-Unelevated "`"$wingetExe`" uninstall $common --custom `"/l*v $(Join-Path $WorkDir 'msi-uninstall.log')`"" 'winget-uninstall'
    Assert ($code -eq 0) 'winget uninstall (unelevated) exit code 0'
    Assert (-not (Test-Path $installDir)) "install folder removed: $installDir"
    Assert ((Get-UserPathRaw) -notlike '*Programs\fairyfly*') 'user PATH entry removed'
    Assert (-not (Test-Path 'HKCU:\Software\DataZoo\fairyfly')) 'HKCU\Software\DataZoo\fairyfly removed'
    Assert (@(Find-UninstallEntries 'HKLM' $productCode).Count -eq 0) 'Windows Installer ARP entry removed'
    Assert (-not (Test-Path $userData)) 'per-user product registration removed'
    Assert (-not (Test-Path $lnk)) 'Start-menu shortcut removed'
} finally {
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}
Write-Host "`nALL CHECKS PASSED (winget $wingetVersion, ProductCode $productCode)"
