<#
.SYNOPSIS
  End-to-end check of the winget manifests and the per-user MSI on a real Windows machine (CI: windows-2022).
.DESCRIPTION
  Renders the manifests for -MsiPath, runs `winget validate`, serves the MSI from a local HTTP server and installs it
  with `winget install --manifest` from an UNELEVATED process of the same user (Invoke-Unelevated.ps1). It then checks
  that the install is per user (folder, PATH, HKCU values, nothing in Program Files, registered for the user's SID and
  not per machine, MSI log without elevation or per-machine markers), uninstalls with winget and checks that
  everything is gone. Finally it upgrades the previous release's MSI (-PreviousMsiUrl) with this one through
  msiexec (the old Start-menu entry "fairyfly (tray)" must go) and uninstalls again. After the winget install it starts the Start-menu
  shortcut exactly as Explorer would (unelevated, new console, its working directory, an isolated LOCALAPPDATA so no
  real pairing is touched) and checks that the tray is still running 10 s later. With -UpgradeChain it then installs
  -MsiPath again and upgrades it with each MSI of the chain in turn (a same-day release YYYY.MM.DD.N, then the next
  day's YYYY.MM.DD): the chain starts from the released MSI of -OldSchemeMsiUrl (MSI versions 26.10.DD[.N], up to
  v2026.10.24.1), which -MsiPath must upgrade. After every step the MSI it replaced, and -SameDayOlder after the first
  step, must be refused as a downgrade (unless it has the same version: a rebuilt MSI of the installed version still
  replaces it). After every step exactly one fairyfly product is registered (the previous one is gone), the exe
  reports the step's version and Add/Remove Programs shows the MSI's ProductVersion as DisplayVersion, which must be
  the DisplayVersion of the step's winget manifest (AppsAndFeaturesEntries). On Windows Server it first shows that the default policy refuses per-user MSI installs by
  non-admins (1625) and then sets DisableMSI=0, the Windows 10/11 default.
  It changes the machine (winget setting LocalManifestFiles, MSI logging policy, installs and removes fairyfly):
  run it on a throw-away CI runner, not on a workstation.
.PARAMETER MsiPath
  The built fairyfly.msi.
.PARAMETER Version
  Calendar version or tag (vYYYY.MM.DD or vYYYY.MM.DD.N) the MSI was built for.
.PARAMETER UpgradeChain
  "version=path-to-msi" entries, in upgrade order after -MsiPath (e.g. 2026.10.24.1=..., 2026.10.25=...).
.PARAMETER SameDayOlder
  "version=path-to-msi" of an earlier release of -Version's day (2026.10.24 for 2026.10.24.1): with -MsiPath installed
  it must be refused. Its payload is never installed, only its ProductVersion matters.
.PARAMETER OldSchemeMsiUrl
  A released MSI of the old version scheme that the chain starts from ('' starts from nothing).
#>
param(
    [Parameter(Mandatory)][string]$MsiPath,
    [Parameter(Mandatory)][string]$Version,
    [string]$WorkDir = (Join-Path $env:TEMP 'ffy-winget-test'),
    [int]$Port = 8765,
    [string]$WingetRelease = 'v1.29.380',
    # the last release whose Start-menu entry was "fairyfly (tray)" (mcp --tray); '' skips the upgrade check
    [string]$PreviousMsiUrl = 'https://github.com/DataZooDE/fairyfly/releases/download/v2026.10.23/fairyfly.msi',
    [string[]]$UpgradeChain = @(),
    [string]$SameDayOlder = '',
    # the last release with the MSI version 26.10.DD[.N] (Windows Installer ignored N): the chain upgrades it
    [string]$OldSchemeMsiUrl = 'https://github.com/DataZooDE/fairyfly/releases/download/v2026.10.24.1/fairyfly.msi'
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
$installedStarter = Join-Path $installDir 'fairyfly-tray.exe'   # the shortcut's target: a Windows-subsystem starter, no console
$startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$lnk = Join-Path $startMenu 'fairyfly.lnk'
$oldLnk = Join-Path $startMenu 'fairyfly (tray).lnk'   # up to v2026.10.23: `mcp --tray`, which exits at once without --http
$shortcutArguments = 'link run --tray'
function Get-FairyflyShortcuts {
    $shell = New-Object -ComObject WScript.Shell
    Get-ChildItem $startMenu -Filter *.lnk -ErrorAction SilentlyContinue | ForEach-Object {
        $s = $shell.CreateShortcut($_.FullName)
        [pscustomobject]@{ Path = $_.FullName; Name = $_.Name; TargetPath = $s.TargetPath; Arguments = $s.Arguments; WorkingDirectory = $s.WorkingDirectory }
    } | Where-Object { $_.TargetPath -ieq $installedStarter }
}
function Get-FairyflyProcesses {
    @(Get-CimInstance Win32_Process -Filter "Name = 'fairyfly.exe'" | Where-Object { $_.ExecutablePath -ieq $installedExe })
}
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

    Step 'Start-menu shortcut: started as Explorer starts it, the tray stays up'
    $shortcuts = @(Get-FairyflyShortcuts)
    foreach ($sc in $shortcuts) { Write-Host "  $($sc.Name): TargetPath=$($sc.TargetPath) Arguments='$($sc.Arguments)' WorkingDirectory=$($sc.WorkingDirectory)" }
    Assert ($shortcuts.Count -eq 1) 'exactly one Start-menu shortcut starts fairyfly-tray.exe'
    $sc = $shortcuts[0]
    Assert (@(Get-FairyflyProcesses).Count -eq 0) 'no fairyfly process before the shortcut is started'
    # an isolated profile folder: no pairing, logs or audit trail of the runner's user are read or written
    $trayHome = Join-Path $WorkDir 'shortcut-localappdata'
    New-Item -ItemType Directory -Force $trayHome | Out-Null
    $savedLocalAppData = $env:LOCALAPPDATA
    $env:LOCALAPPDATA = $trayHome
    try {
        $started = & $unelevated -CommandLine "`"$($sc.TargetPath)`" $($sc.Arguments)" -WorkingDirectory $sc.WorkingDirectory -NoWait
    } finally { $env:LOCALAPPDATA = $savedLocalAppData }
    Start-Sleep -Seconds 10
    $alive = @(Get-FairyflyProcesses)
    foreach ($p in $alive) {
        $gp = Get-Process -Id $p.ProcessId -ErrorAction SilentlyContinue
        Write-Host "  running: pid $($p.ProcessId) (parent $($p.ParentProcessId), session $($p.SessionId)) window '$($gp.MainWindowTitle)': $($p.CommandLine)"
    }
    Get-Process explorer -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  explorer pid $($_.Id) session $($_.SessionId)" }
    Write-Host "  this script: session $((Get-Process -Id $PID).SessionId)"
    Get-ChildItem $trayHome -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "  --- $($_.FullName)"; Get-Content $_.FullName -Tail 20 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "    $_" } }
    Assert ($alive.Count -ge 1) "the shortcut's fairyfly (started pid $started) is still running after 10 s (a tray app stays up)"
    Assert ($sc.Arguments -eq $shortcutArguments) "shortcut Arguments = '$shortcutArguments' (the cloud link tray)"
    Assert ($sc.Name -eq (Split-Path $lnk -Leaf)) "shortcut name $($sc.Name)"
    Assert ($sc.WorkingDirectory.TrimEnd('\') -ieq $installDir) "shortcut WorkingDirectory = $installDir"
    Assert (@($alive | Where-Object { $_.CommandLine -like "*$shortcutArguments*" }).Count -ge 1) "the running process is the link tray ($shortcutArguments)"
    Assert (@($alive | Where-Object { (Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue).MainWindowTitle }).Count -eq 0) 'no visible window (no error message box): it sits in the tray'
    $linkLog = Join-Path $trayHome 'fairyfly\logs\link.log'
    Assert (Test-Path $linkLog) 'the tray logs to the isolated LOCALAPPDATA (link.log)'
    # The icon itself can only be checked where an unelevated process can reach the taskbar. On the hosted runners
    # Explorer runs at HIGH integrity (the runner account's full administrator token), so UIPI refuses the medium-integrity
    # tray's Shell_NotifyIcon (error 5); on a user's desktop Explorer is unelevated. The tray then keeps running and
    # retrying (logged).
    if (-not ('Ffy.Integrity' -as [type])) {
        Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
namespace Ffy { public static class Integrity {
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr p, uint access, out IntPtr t);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr t, int cls, IntPtr info, int len, out int ret);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid, uint n);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    /// Mandatory integrity level RID of a process (0x2000 medium, 0x3000 high), -1 unknown.
    public static int Of(int pid) {
        IntPtr p = OpenProcess(0x1000, false, pid); if (p == IntPtr.Zero) return -1;   // PROCESS_QUERY_LIMITED_INFORMATION
        IntPtr t = IntPtr.Zero, buf = Marshal.AllocHGlobal(256);
        try {
            if (!OpenProcessToken(p, 8, out t)) return -1;   // TOKEN_QUERY
            int ret; if (!GetTokenInformation(t, 25, buf, 256, out ret)) return -1;   // TokenIntegrityLevel
            IntPtr sid = Marshal.ReadIntPtr(buf);
            int n = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
            return Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(n - 1)));
        } finally { if (t != IntPtr.Zero) CloseHandle(t); CloseHandle(p); Marshal.FreeHGlobal(buf); }
    } } }
'@
    }
    $traySessions = @($alive | ForEach-Object { $_.SessionId })
    $explorerHere = @(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object { $traySessions -contains $_.SessionId })
    $explorerLevels = @($explorerHere | ForEach-Object { [Ffy.Integrity]::Of($_.Id) })
    $trayLevels = @($alive | ForEach-Object { [Ffy.Integrity]::Of([int]$_.ProcessId) })
    Write-Host ("  Explorer in the tray's session: {0}, integrity: {1}; tray integrity: {2}" -f $explorerHere.Count,
        (($explorerLevels | ForEach-Object { '0x{0:X}' -f $_ }) -join ','), (($trayLevels | ForEach-Object { '0x{0:X}' -f $_ }) -join ','))
    Assert (@($trayLevels | Where-Object { $_ -ne 0x2000 }).Count -eq 0) 'the shortcut started the tray at medium integrity (unelevated)'
    if ($explorerHere.Count -gt 0 -and @($explorerLevels | Where-Object { $_ -gt 0x2000 }).Count -eq 0) {
        Assert ((Get-Content $linkLog -Raw) -match 'tray: notification icon added') 'the tray icon is in the taskbar (link.log)'
    } else {
        Assert ((Get-Content $linkLog -Raw) -match 'tray: the notification icon could not be added yet') 'no reachable taskbar here: the tray says so in link.log and keeps running'
        Write-Host '  (icon check skipped: no taskbar an unelevated process can reach)'
    }
    Assert (-not (Test-Path (Join-Path $trayHome 'fairyfly\link\state.json'))) 'nothing was paired'
    $alive | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    $deadline = (Get-Date).AddSeconds(15)
    while (@(Get-FairyflyProcesses).Count -gt 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
    Assert (@(Get-FairyflyProcesses).Count -eq 0) 'tray stopped before the uninstall'

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
    $code = Run-Unelevated "`"$wingetExe`" uninstall --manifest `"$manifestDir`" --silent --disable-interactivity --accept-source-agreements --log `"$(Join-Path $WorkDir 'msi-uninstall.log')`"" 'winget-uninstall'
    Assert ($code -eq 0) 'winget uninstall (unelevated) exit code 0'
    Assert (-not (Test-Path $installDir)) "install folder removed: $installDir"
    Assert ((Get-UserPathRaw) -notlike '*Programs\fairyfly*') 'user PATH entry removed'
    Assert (-not (Test-Path 'HKCU:\Software\DataZoo\fairyfly')) 'HKCU\Software\DataZoo\fairyfly removed'
    Assert (@(Find-UninstallEntries 'HKLM' $productCode).Count -eq 0) 'Windows Installer ARP entry removed'
    Assert (-not (Test-Path $userData)) 'per-user product registration removed'
    Assert (-not (Test-Path $lnk)) 'Start-menu shortcut removed'

    if ($PreviousMsiUrl) {
        Step "Upgrade from the previous release ($PreviousMsiUrl) with msiexec, unelevated"
        $prevMsi = Join-Path $WorkDir 'previous.msi'
        $ProgressPreference = 'SilentlyContinue'
        for ($try = 1; ; $try++) {
            try { Invoke-WebRequest $PreviousMsiUrl -OutFile $prevMsi -UseBasicParsing; break }
            catch { if ($try -ge 4) { throw }; Write-Host "  download failed ($($_.Exception.Message)), retry $try"; Start-Sleep -Seconds (5 * $try) }
        }
        $code = Run-Unelevated "start `"`" /wait msiexec.exe /i `"$prevMsi`" /qn /l*v `"$(Join-Path $WorkDir 'msi-install-previous.log')`"" 'msi-install-previous'
        Assert ($code -eq 0) 'previous release installed (unelevated msiexec)'
        Assert (Test-Path $oldLnk) 'the previous release has the Start-menu entry "fairyfly (tray)"'
        $code = Run-Unelevated "start `"`" /wait msiexec.exe /i `"$MsiPath`" /qn /l*v `"$(Join-Path $WorkDir 'msi-upgrade.log')`"" 'msi-upgrade'
        Assert ($code -eq 0) 'this MSI upgrades it (unelevated msiexec, MajorUpgrade)'
        Assert (-not (Test-Path $oldLnk)) 'upgrade removed the old Start-menu entry "fairyfly (tray)"'
        Assert (Test-Path $lnk) "upgrade added the Start-menu entry $(Split-Path $lnk -Leaf)"
        $arp = @(Find-UninstallEntries 'HKLM' $productCode)
        Assert ($arp.Count -eq 1 -and $arp[0].PSChildName -eq $productCode) 'one fairyfly product after the upgrade (the old one was removed, not installed beside it)'
        $code = Run-Unelevated "start `"`" /wait msiexec.exe /x `"$MsiPath`" /qn /l*v `"$(Join-Path $WorkDir 'msi-upgrade-uninstall.log')`"" 'msi-upgrade-uninstall'
        Assert ($code -eq 0) 'uninstalled again (unelevated msiexec /x)'
        Assert (-not (Test-Path $installDir) -and -not (Test-Path $lnk) -and -not (Test-Path $oldLnk)) 'nothing left after the upgrade check'
    }
    if ($UpgradeChain) {
        $chain = @([pscustomobject]@{ Version = $Version.TrimStart('v'); Msi = $MsiPath })
        foreach ($entry in $UpgradeChain) {
            $v, $p = $entry -split '=', 2
            $chain += [pscustomobject]@{ Version = $v.TrimStart('v'); Msi = (Resolve-Path $p).Path }
        }
        Step "Upgrade chain (unelevated msiexec): $(($chain | ForEach-Object Version) -join ' -> ')"
        $upgradeCode = '{AEC9807E-BC74-4802-8816-25F3AB39BA29}'
        # the Windows Installer API itself (msi.dll), not its COM wrapper: RelatedProducts through IDispatch came back null
        if (-not ('Ffy.Msi' -as [type])) {
            Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
namespace Ffy { public static class Msi {
    [DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern uint MsiEnumRelatedProducts(string upgradeCode, uint reserved, uint index, StringBuilder product);
    [DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern int MsiQueryProductState(string product);
    [DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern uint MsiGetProductInfo(string product, string property, StringBuilder value, ref uint len);
    /// ProductCodes of all installed products with this UpgradeCode (any context visible to this user).
    public static string[] Related(string upgradeCode) {
        var list = new List<string>();
        for (uint i = 0; ; i++) {
            var sb = new StringBuilder(39);
            uint r = MsiEnumRelatedProducts(upgradeCode, 0, i, sb);
            if (r == 259) break;   // ERROR_NO_MORE_ITEMS
            if (r != 0) throw new Exception("MsiEnumRelatedProducts: " + r);
            list.Add(sb.ToString());
        }
        return list.ToArray();
    }
    /// INSTALLSTATE: 5 = installed, -1 = unknown (not registered)
    public static int State(string product) { return MsiQueryProductState(product); }
    public static string Info(string product, string property) {
        uint len = 256; var sb = new StringBuilder((int)len);
        uint r = MsiGetProductInfo(product, property, sb, ref len);
        return r == 0 ? sb.ToString() : "<error " + r + ">";
    }
} }
'@
        }
        function Get-RelatedProducts { @([Ffy.Msi]::Related($upgradeCode)) }
        function Get-PathEntries { @(([Environment]::ExpandEnvironmentVariables((Get-UserPathRaw)) -split ';') | Where-Object { $_.TrimEnd('\') -ieq $installDir }) }
        Assert (@(Get-RelatedProducts).Count -eq 0) 'no fairyfly product registered before the chain'
        $previous = $null
        if ($OldSchemeMsiUrl) {
            $oldMsi = Join-Path $WorkDir 'old-scheme.msi'
            $ProgressPreference = 'SilentlyContinue'
            for ($try = 1; ; $try++) {
                try { Invoke-WebRequest $OldSchemeMsiUrl -OutFile $oldMsi -UseBasicParsing; break }
                catch { if ($try -ge 4) { throw }; Write-Host "  download failed ($($_.Exception.Message)), retry $try"; Start-Sleep -Seconds (5 * $try) }
            }
            $code = Run-Unelevated "start `"`" /wait msiexec.exe /i `"$oldMsi`" /qn /l*v `"$(Join-Path $WorkDir 'msi-chain-old-scheme.log')`"" 'msi-chain-old-scheme'
            Assert ($code -eq 0) "released old-scheme MSI installed ($OldSchemeMsiUrl)"
            $related = @(Get-RelatedProducts)
            Assert ($related.Count -eq 1) "one fairyfly product: $($related -join ', ')"
            $oldVersion = (& $installedExe --version | Out-String).Trim()
            $previous = [pscustomobject]@{ Version = $oldVersion; ProductCode = $related[0]; Msi = $oldMsi }
            Write-Host "  released $oldVersion is MSI $([Ffy.Msi]::Info($related[0], 'VersionString')) ($($related[0])); the chain upgrades it"
        }
        $refusals = 0
        function Assert-Refused([string]$msi, [string]$label, $current) {
            $script:refusals++
            $log = Join-Path $WorkDir "msi-downgrade-$script:refusals.log"
            $code = Run-Unelevated "start `"`" /wait msiexec.exe /i `"$msi`" /qn /l*v `"$log`"" "msi-downgrade-$script:refusals"
            $text = Get-Content $log -Raw
            $text -split "`r?`n" | Select-String 'WIX_DOWNGRADE_DETECTED|WIX_UPGRADE_DETECTED|A newer version|Property\(S\): ProductVersion =|Installation success or error status' |
                Select-Object -First 8 | ForEach-Object { Write-Host "  log: $($_.Line.Trim())" }
            # `start /wait` does not pass msiexec's exit code on: the log's final status is the installer's result
            $status = ([regex]::Matches($text, 'Installation success or error status: (\d+)') | Select-Object -Last 1).Groups[1].Value
            Assert ($status -and $status -ne '0') "$label over $($current.Version) is refused (installer status $status)"
            Assert ($text -match 'Adding WIX_DOWNGRADE_DETECTED property') 'FindRelatedProducts detected the downgrade (WIX_DOWNGRADE_DETECTED)'
            $related = @(Get-RelatedProducts)
            Assert ($related.Count -eq 1 -and $related[0] -eq $current.ProductCode) "still exactly the product $($current.ProductCode): $($related -join ', ')"
            $exeVersion = (& $installedExe --version | Out-String).Trim()
            Assert ($exeVersion -eq $current.Version) "installed fairyfly --version still $exeVersion"
        }
        for ($i = 0; $i -lt $chain.Count; $i++) {
            $c = $chain[$i]
            $dir = Join-Path $WorkDir "chain-$i"
            $ci = & $generator -Version $c.Version -MsiPath $c.Msi -OutDir $dir -InstallerUrl "http://127.0.0.1:$Port/chain-$i.msi"
            & $wingetExe validate --manifest $dir --disable-interactivity | Out-Null
            Assert ($LASTEXITCODE -eq 0) "winget validate of the $($c.Version) manifests"
            $manifestYaml = Get-Content (Join-Path $dir 'DataZoo.fairyfly.installer.yaml') -Raw
            $versionYaml = Get-Content (Join-Path $dir 'DataZoo.fairyfly.yaml') -Raw
            Assert ($versionYaml -match "(?m)^PackageVersion: $([regex]::Escape($c.Version))\s*$") "manifest PackageVersion $($c.Version)"
            $null = $manifestYaml -match '(?m)^\s*DisplayVersion: (\S+)\s*$'; $manifestDisplay = $Matches[1]
            $log = Join-Path $WorkDir "msi-chain-$i.log"
            $code = Run-Unelevated "start `"`" /wait msiexec.exe /i `"$($c.Msi)`" /qn /l*v `"$log`"" "msi-chain-$i"
            Assert ($code -eq 0) "$($c.Version) (MSI $($ci.MsiVersion), $($ci.ProductCode)) installed, unelevated msiexec exit 0"
            $logText = Get-Content $log -Raw
            $logText -split "`r?`n" | Select-String 'WIX_UPGRADE_DETECTED|Migrating feature settings|RemoveExistingProducts. Return|Property\(S\): ProductVersion =|Installation success or error status' |
                Select-Object -First 12 | ForEach-Object { Write-Host "  log: $($_.Line.Trim())" }
            if ($previous) {
                # FindRelatedProducts sets WIX_UPGRADE_DETECTED (MajorUpgrade) to the product it will remove
                Assert ($logText -match "Adding WIX_UPGRADE_DETECTED property\. Its value is '$([regex]::Escape($previous.ProductCode))'") "FindRelatedProducts found $($previous.Version)'s product $($previous.ProductCode) (an upgrade, not an install beside it)"
                Assert ([Ffy.Msi]::State($previous.ProductCode) -eq -1) "the previous product $($previous.ProductCode) ($($previous.Version)) is no longer registered (ProductState -1)"
            }
            $related = @(Get-RelatedProducts)
            Assert ($related.Count -eq 1 -and $related[0] -eq $ci.ProductCode) "exactly one product with fairyfly's UpgradeCode: $($related -join ', ')"
            $arp = @(Find-UninstallEntries 'HKLM' $ci.ProductCode)
            foreach ($e in $arp) { Write-Host ("  uninstall entry {0}: DisplayName={1} DisplayVersion={2} Version=0x{3:X8}" -f $e.PSChildName, $e.GetValue('DisplayName'), $e.GetValue('DisplayVersion'), $e.GetValue('Version')) }
            Assert ($arp.Count -eq 1 -and $arp[0].PSChildName -eq $ci.ProductCode) 'exactly one Add/Remove Programs entry, the new product'
            $shown = [string]$arp[0].GetValue('DisplayVersion')
            Write-Host "  Windows shows DisplayVersion '$shown' for MSI ProductVersion '$($ci.MsiVersion)' (fairyfly $($c.Version))"
            Assert ($shown -eq $ci.MsiVersion) "Add/Remove Programs DisplayVersion = MSI ProductVersion $($ci.MsiVersion)"
            Assert ($manifestDisplay -eq $shown) "winget AppsAndFeaturesEntries DisplayVersion $manifestDisplay = what Windows shows"
            $apiVersion = [Ffy.Msi]::Info($ci.ProductCode, 'VersionString')
            Assert ($apiVersion -eq $ci.MsiVersion) "Installer API VersionString $apiVersion = MSI ProductVersion"
            $exeVersion = (& $installedExe --version | Out-String).Trim()
            Assert ($exeVersion -eq $c.Version) "installed fairyfly --version = $exeVersion"
            Assert (@(Get-FairyflyShortcuts).Count -eq 1 -and (Test-Path $lnk)) 'one Start-menu shortcut'
            Assert ((Get-PathEntries).Count -eq 1) 'the install folder is on the user PATH exactly once'
            $files = @(Get-ChildItem $installDir -File).Name
            Assert ($files.Count -eq 2 -and $files -contains 'fairyfly.exe' -and $files -contains 'fairyfly-tray.exe') "only fairyfly.exe and its tray starter fairyfly-tray.exe in $installDir ($($files -join ', '))"
            $current = [pscustomobject]@{ Version = $c.Version; ProductCode = $ci.ProductCode; Msi = $c.Msi }
            # downgrades are refused: the MSI this step replaced (a rebuilt MSI of the same version is not a downgrade)
            if ($previous -and $previous.Version -ne $c.Version) { Assert-Refused $previous.Msi "the MSI of $($previous.Version) (a downgrade)" $current }
            if ($i -eq 0 -and $SameDayOlder) {
                $ov, $op = $SameDayOlder -split '=', 2
                Assert-Refused (Resolve-Path $op).Path "the MSI of $ov (MSI $((& (Join-Path $here '..\..\msi_version.ps1') -Version $ov).Trim()), same day, a downgrade)" $current
            }
            $previous = $current
        }
        $code = Run-Unelevated "start `"`" /wait msiexec.exe /x `"$($previous.Msi)`" /qn /l*v `"$(Join-Path $WorkDir 'msi-chain-uninstall.log')`"" 'msi-chain-uninstall'
        Assert ($code -eq 0) "uninstalled $($previous.Version) (unelevated msiexec /x)"
        Assert (@(Get-RelatedProducts).Count -eq 0) 'no fairyfly product registered after the chain'
        Assert (@(Find-UninstallEntries 'HKLM' $previous.ProductCode).Count -eq 0) 'no Add/Remove Programs entry left'
        Assert (-not (Test-Path $installDir) -and -not (Test-Path $lnk) -and (Get-PathEntries).Count -eq 0 -and -not (Test-Path 'HKCU:\Software\DataZoo\fairyfly')) 'nothing left after the chain (folder, shortcut, PATH, HKCU)'
    }
} finally {
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}
Write-Host "`nALL CHECKS PASSED (winget $wingetVersion, ProductCode $productCode)"
