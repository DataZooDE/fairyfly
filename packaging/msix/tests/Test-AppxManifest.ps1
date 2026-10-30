<#
.SYNOPSIS
  Checks the MSIX manifest template packaging/msix/AppxManifest.xml.in as CMake renders it (cmake/msix_manifest.cmake,
  what configuring with -DFAIRYFLY_PACKAGE=msix runs). Any pwsh 7 with cmake on PATH, Windows or Linux (CI: build.yml,
  job msix-manifest).
.DESCRIPTION
  Without identity values (the defaults of the CMake cache variables) the manifest must carry exactly fairyfly's
  Partner Center identity: Name DataZoo.fairyfly, Publisher CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC,
  PublisherDisplayName DataZoo; CMakeLists.txt must take its cache defaults from cmake/msix_manifest.cmake.
  With identity values set the rendered manifest must be well-formed XML with no placeholder left, carry the identity
  (XML-escaped) and the MSIX version of packaging/msix_version.ps1, declare runFullTrust, the tray's StartupTask (instead
  of the MSI's HKCU Run value), the fairyfly.exe execution alias for terminals, and the toast activator with the
  CLSID, COM server argument and application id the code uses (src/include/tray/tray_contract.h). Rendering must
  fail for a placeholder the template does not get a value for, an empty value, an identity name MSIX does not allow
  and a publisher that is no distinguished name.
#>
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot (Join-Path '..' (Join-Path '..' '..')))).Path
$template = Join-Path $repo (Join-Path 'packaging' (Join-Path 'msix' 'AppxManifest.xml.in'))
$cmakeScript = Join-Path $repo (Join-Path 'cmake' 'msix_manifest.cmake')
$msixVersion = Join-Path $repo (Join-Path 'packaging' 'msix_version.ps1')
$failures = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS: $msg" } else { Write-Host "FAIL: $msg"; $script:failures++ }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("ffy-msix-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force $tmp | Out-Null

# fairyfly's product identity in Partner Center (public: it is part of every installed package's full name)
$storeName = 'DataZoo.fairyfly'
$storePublisher = 'CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC'
$storeDisplay = 'DataZoo'

function RenderDefaults([string]$version = '2026.10.24.1') {
    $out = Join-Path $tmp ("AppxManifest-" + [guid]::NewGuid() + ".xml")
    $log = & cmake "-DVERSION=$version" "-DTEMPLATE=$template" "-DOUT=$out" -P $cmakeScript 2>&1 | Out-String
    [pscustomobject]@{ Ok = ($LASTEXITCODE -eq 0); File = $out; Log = $log.Trim() }
}

function Render([string]$tpl,[string]$name, [string]$publisher, [string]$display, [string]$version = '2026.10.24.1') {
    $out = Join-Path $tmp ("AppxManifest-" + [guid]::NewGuid() + ".xml")
    $log = & cmake "-DVERSION=$version" "-DTEMPLATE=$tpl" "-DOUT=$out" "-DIDENTITY_NAME=$name" "-DPUBLISHER=$publisher" `
                   "-DPUBLISHER_DISPLAY_NAME=$display" -P $cmakeScript 2>&1 | Out-String
    [pscustomobject]@{ Ok = ($LASTEXITCODE -eq 0); File = $out; Log = $log.Trim() }
}

try {
    Write-Host '=== rendered with the defaults: the Partner Center identity ==='
    $r = RenderDefaults
    Check $r.Ok "cmake renders the template without identity values$(if (-not $r.Ok) { ": $($r.Log)" })"
    $d = $null
    try { if (Test-Path $r.File) { $d = [xml](Get-Content $r.File -Raw) } } catch { Write-Host "  XML error: $($_.Exception.Message)" }
    $did = $null; $ddisplay = $null
    if ($d -and $d.DocumentElement) {
        $dns = New-Object Xml.XmlNamespaceManager($d.NameTable)
        $dns.AddNamespace('f', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
        $did = $d.SelectSingleNode('/f:Package/f:Identity', $dns)
        $ddisplay = $d.SelectSingleNode('/f:Package/f:Properties/f:PublisherDisplayName', $dns)
    }
    Check ($did -and $did.Name -ceq $storeName) "default Identity Name '$($did.Name)' is exactly '$storeName'"
    Check ($did -and $did.Publisher -ceq $storePublisher) "default Identity Publisher '$($did.Publisher)' is exactly '$storePublisher'"
    Check ($ddisplay -and $ddisplay.InnerText -ceq $storeDisplay) "default PublisherDisplayName '$($ddisplay.InnerText)' is exactly '$storeDisplay'"
    # The package family name is <Name>_<publisher id>, the publisher id the first 8 bytes of SHA-256 over the
    # Publisher (UTF-16LE) in base32 (0-9, a-z without i l o u). Partner Center lists DataZoo.fairyfly_73q587949tsjt.
    $pfn = ''
    if ($did) {
        $hash = [Security.Cryptography.SHA256]::HashData([Text.Encoding]::Unicode.GetBytes($did.Publisher))
        $bits = (($hash[0..7] | ForEach-Object { [Convert]::ToString($_, 2).PadLeft(8, '0') }) -join '') + '0'
        $alphabet = '0123456789abcdefghjkmnpqrstvwxyz'
        $pfn = $did.Name + '_' + ((0..12 | ForEach-Object { $alphabet[[Convert]::ToInt32($bits.Substring($_ * 5, 5), 2)] }) -join '')
    }
    Check ($pfn -ceq 'DataZoo.fairyfly_73q587949tsjt') "package family name '$pfn' is Partner Center's DataZoo.fairyfly_73q587949tsjt"
    $defaultText = if (Test-Path $r.File) { Get-Content $r.File -Raw } else { '' }
    Check ($defaultText -and $defaultText -cnotmatch 'PLACEHOLDER') 'no PLACEHOLDER in the default manifest'
    $cmakeLists = Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw
    foreach ($v in 'IDENTITY_NAME', 'PUBLISHER', 'PUBLISHER_DISPLAY_NAME') {
        $pattern = 'set\(FAIRYFLY_MSIX_' + $v + ' "\$\{FAIRYFLY_MSIX_DEFAULT_' + $v + '\}" CACHE STRING'
        Check ($cmakeLists -match $pattern) "CMakeLists.txt: the cache variable FAIRYFLY_MSIX_$v defaults to FAIRYFLY_MSIX_DEFAULT_$v"
    }
    Check ($cmakeLists -cnotmatch 'PLACEHOLDER') 'CMakeLists.txt has no PLACEHOLDER identity left'

    Write-Host "`n=== rendered with other identity values (the cache variables stay overridable) ==="
    $name = 'DataZoo.fairyfly'
    $publisher = 'CN=1A2B3C4D-0000-4000-8000-ABCDEF012345, O=Data & Zoo'
    $display = 'DataZoo <GmbH> & Co'
    $r = Render $template $name $publisher $display
    Check $r.Ok "cmake renders the template$(if (-not $r.Ok) { ": $($r.Log)" })"
    $text = if (Test-Path $r.File) { Get-Content $r.File -Raw } else { '' }
    Check ($text -notmatch '@[A-Za-z_][A-Za-z0-9_]*@') 'no @NAME@ placeholder is left'
    Check ($text -cnotmatch 'PLACEHOLDER') 'no PLACEHOLDER value when the identity is set'
    $xml = $null
    try { if ($text) { $xml = [xml]$text } } catch { Write-Host "  XML error: $($_.Exception.Message)" }
    if ($xml -and -not $xml.DocumentElement) { $xml = $null }   # [xml]'' is an empty document, not a manifest
    Check ($null -ne $xml -and $xml.DocumentElement.LocalName -eq 'Package') 'the manifest is well-formed XML with a Package root'

    if ($xml) {
        $ns = New-Object Xml.XmlNamespaceManager($xml.NameTable)
        $ns.AddNamespace('f', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
        $ns.AddNamespace('uap', 'http://schemas.microsoft.com/appx/manifest/uap/windows10')
        $ns.AddNamespace('uap5', 'http://schemas.microsoft.com/appx/manifest/uap/windows10/5')
        $ns.AddNamespace('desktop', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10')
        $ns.AddNamespace('desktop4', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/4')
        $ns.AddNamespace('com', 'http://schemas.microsoft.com/appx/manifest/com/windows10')
        $ns.AddNamespace('rescap', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities')
        $ns.AddNamespace('virtualization', 'http://schemas.microsoft.com/appx/manifest/virtualization/windows10')
        function One([string]$xpath) { $xml.SelectSingleNode($xpath, $ns) }

        $id = One '/f:Package/f:Identity'
        Check ($id -and $id.Name -eq $name) "Identity Name '$($id.Name)'"
        Check ($id -and $id.Publisher -eq $publisher) "Identity Publisher '$($id.Publisher)' (XML-escaped and read back)"
        $expectedVersion = (& $msixVersion -Version '2026.10.24.1' | Out-String).Trim()
        Check ($id -and $id.Version -eq $expectedVersion -and $expectedVersion -eq '2026.10.2401.0') "Identity Version '$($id.Version)' = msix_version.ps1 '$expectedVersion'"
        Check ($id -and $id.ProcessorArchitecture -eq 'x64') 'x64 package'
        $tile = One "/f:Package/f:Applications/f:Application[@Id='Tray']/uap:VisualElements/uap:DefaultTile"
        Check ($null -ne $tile -and $tile.Wide310x150Logo -eq 'Assets\Wide310x150Logo.png') 'the tray tile has the wide logo (Assets\Wide310x150Logo.png)'
        Check ((One '/f:Package/f:Properties/f:PublisherDisplayName').InnerText -eq $display) 'PublisherDisplayName (XML-escaped and read back)'
        Check ($null -ne (One "/f:Package/f:Capabilities/rescap:Capability[@Name='runFullTrust']")) 'runFullTrust'

        $tray = One "/f:Package/f:Applications/f:Application[@Executable='fairyfly-tray.exe']"
        Check ($null -ne $tray -and $tray.EntryPoint -eq 'Windows.FullTrustApplication') 'Application for the tray (fairyfly-tray.exe, full trust)'
        $startup = $xml.SelectSingleNode("/f:Package/f:Applications/f:Application[@Executable='fairyfly-tray.exe']/f:Extensions/desktop:Extension[@Category='windows.startupTask']", $ns)
        Check ($null -ne $startup -and $startup.Executable -eq 'fairyfly-tray.exe') 'the tray starts at logon through a StartupTask'
        $task = if ($startup) { $startup.SelectSingleNode('desktop:StartupTask', $ns) } else { $null }
        Check ($null -ne $task -and $task.TaskId -and $task.Enabled -eq 'true') "StartupTask '$($task.TaskId)' enabled"
        $toast = One "//desktop:ToastNotificationActivation"
        $class = One "//com:ExeServer[@Executable='fairyfly-tray.exe' and @Arguments='-Embedding']/com:Class"
        Check ($toast -and $class -and $toast.ToastActivatorCLSID -eq $class.Id -and $class.Id -eq 'DAAF1065-EF17-4CFF-B3D2-EFAF70594959') 'toast activator CLSID declared as COM server and toast activation (tray_contract.h)'

        # the code's side of the toast activation (src/include/tray/tray_contract.h): the CLSID the link registers its
        # class object under, the argument the tray starter takes as COM's start, and the application whose AUMID
        # (<PackageFamilyName>!Tray) the Store build's toasts use (src/include/link/toast_identity.h)
        $contract = Get-Content (Join-Path $repo (Join-Path 'src' (Join-Path 'include' (Join-Path 'tray' 'tray_contract.h')))) -Raw
        function ContractValue([string]$name) {
            if ($contract -match ('inline constexpr wchar_t ' + $name + '\[\] = L"([^"]*)";')) { $Matches[1] } else { $null }
        }
        $codeClsid = ContractValue 'kToastActivatorClsid'
        $codeArg = ContractValue 'kComServerArgument'
        $codeApp = ContractValue 'kPackageTrayApplicationId'
        Check ($null -ne $codeClsid -and $codeClsid -match '^\{[0-9A-Fa-f-]{36}\}$') "tray_contract.h kToastActivatorClsid '$codeClsid'"
        $bare = if ($codeClsid) { $codeClsid.Trim('{', '}') } else { '' }
        Check ($class -and $class.Id -eq $bare) "manifest com:Class Id '$($class.Id)' = the code's kToastActivatorClsid '$codeClsid'"
        Check ($toast -and $toast.ToastActivatorCLSID -eq $bare) "manifest ToastActivatorCLSID '$($toast.ToastActivatorCLSID)' = the code's kToastActivatorClsid"
        $server = One "//com:ExeServer[com:Class]"
        Check ($server -and $codeArg -and $server.Arguments -ceq $codeArg) "manifest com:ExeServer Arguments '$($server.Arguments)' = the code's kComServerArgument '$codeArg' (fairyfly-tray.exe starts the link tray for it)"
        $toastApp = One "/f:Package/f:Applications/f:Application[f:Extensions/desktop:Extension/desktop:ToastNotificationActivation]"
        Check ($toastApp -and $codeApp -and $toastApp.Id -ceq $codeApp) "the application declaring the toast activation, '$($toastApp.Id)', is the code's kPackageTrayApplicationId '$codeApp' (AUMID <PackageFamilyName>!$codeApp)"
        Check ($toastApp -and $toastApp.Executable -eq $server.Executable -and $toastApp.Id -eq ($xml.SelectSingleNode("/f:Package/f:Applications/f:Application[f:Extensions/com:Extension/com:ComServer]", $ns)).Id) 'the COM server is declared by the same application (fairyfly-tray.exe)'

        $cli = One "/f:Package/f:Applications/f:Application[@Executable='fairyfly.exe']"
        Check ($null -ne $cli) 'Application for the CLI (fairyfly.exe)'
        $alias = One "//uap5:Extension[@Category='windows.appExecutionAlias' and @Executable='fairyfly.exe']/uap5:AppExecutionAlias"
        Check ($null -ne $alias -and $alias.GetAttribute('Subsystem', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/4') -eq 'console') 'the alias runs fairyfly.exe as a console program'
        Check ($null -ne (One "//uap5:ExecutionAlias[@Alias='fairyfly.exe']")) 'execution alias fairyfly.exe (terminals, stdio MCP clients)'
        $ids = @($xml.SelectNodes('/f:Package/f:Applications/f:Application', $ns) | ForEach-Object { $_.Id })
        Check ($ids.Count -eq 2 -and ($ids | Select-Object -Unique).Count -eq 2) "two applications with distinct ids ($($ids -join ', '))"

        # the data directory is %USERPROFILE%\.fairyfly (docs/DATA_DIR.md), which is not virtualized on any Windows
        # version: no exclusion under AppData (it only worked on Windows 11) and no restricted capability for it
        Check ($xml.SelectNodes('//virtualization:ExcludedDirectory', $ns).Count -eq 0) 'no file system virtualization exclusion (the data lives in %USERPROFILE%\.fairyfly)'
        Check ($null -eq (One "/f:Package/f:Capabilities/rescap:Capability[@Name='unvirtualizedResources']")) 'no unvirtualizedResources (a restricted capability the Store would have to approve)'
        $caps = @($xml.SelectNodes('/f:Package/f:Capabilities/*', $ns) | ForEach-Object { $_.Name })
        Check ($caps.Count -eq 1 -and $caps[0] -eq 'runFullTrust') "runFullTrust is the only capability ($($caps -join ', '))"
        $ignorable = $xml.DocumentElement.IgnorableNamespaces -split ' '
        foreach ($prefix in 'uap', 'uap5', 'desktop', 'desktop4', 'com', 'rescap') {
            Check ($ignorable -contains $prefix) "namespace $prefix is ignorable (older Windows skips what it does not know)"
        }
    }

    Write-Host "`n=== refusals ==="
    $r = Render $template $name $publisher ''
    Check (-not $r.Ok -and $r.Log -match 'PUBLISHER_DISPLAY_NAME') 'an empty PublisherDisplayName is refused'
    foreach ($bad in 'ab', 'DataZoo fairyfly', 'DataZoo_fairyfly', ('x' * 51)) {
        $r = Render $template $bad $publisher $display
        Check (-not $r.Ok -and $r.Log -match 'IDENTITY_NAME') "identity name '$bad' is refused"
    }
    foreach ($bad in '', 'DataZoo GmbH', 'O=DataZoo') {
        $r = Render $template $name $bad $display
        Check (-not $r.Ok -and $r.Log -match 'PUBLISHER') "publisher '$bad' is refused"
    }
    $r = Render $template $name $publisher $display '2026.02.30'
    Check (-not $r.Ok) 'a version that is no date is refused'
    # a placeholder the CMake function has no value for would be configured to an empty string: refused instead
    $extra = Join-Path $tmp 'extra.xml.in'
    Set-Content -Path $extra -Value ((Get-Content $template -Raw) -replace '<DisplayName>fairyfly</DisplayName>', '<DisplayName>@FAIRYFLY_MSIX_DISPLAY_NAME@</DisplayName>') -NoNewline
    $r = Render $extra $name $publisher $display
    Check (-not $r.Ok -and $r.Log -match 'FAIRYFLY_MSIX_DISPLAY_NAME') 'a placeholder without a value is refused, not rendered empty'
} finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures) { Write-Host "`n$failures check(s) FAILED"; exit 1 }
Write-Host "`nALL APPXMANIFEST CHECKS PASSED"
exit 0
