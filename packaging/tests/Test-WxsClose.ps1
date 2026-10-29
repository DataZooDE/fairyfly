<#
.SYNOPSIS
  Checks that packaging/fairyfly.wxs ends the running fairyfly before an install, upgrade or uninstall replaces or
  removes its files (CI: build.yml, job msix-manifest; any pwsh 7, no WiX needed). The real installer run is in
  build.yml (build-windows: install, start the tray and `fairyfly mcp`, uninstall, nothing of it left running).
.DESCRIPTION
  Live 2026-10-29: after `msiexec /x` the link tray of the removed MSI kept running from the deleted fairyfly.exe and
  blocked the Microsoft Store tray. The close step must
  - run the starter the MSI itself carries (a <Binary> of $(var.TrayExePath)), not the installed one: on an upgrade the
    installed starter is the old version's, so a fix in the starter would only help one release later;
  - name the install folder ([INSTALLFOLDER]): the temporary copy of the starter does not run from it, and only
    fairyfly.exe and fairyfly-tray.exe from that folder are ended (a Microsoft Store fairyfly.exe keeps running);
  - run before InstallValidate on install and upgrade (--close, `NOT Installed`) and on uninstall (--uninstall), never
    on a repair or modify (no file of the same version is replaced, and a silent repair would not start the trays
    again) nor on the removal of the old product inside an upgrade (its files are already closed).
  util:CloseApplication is not used: it finds processes by file name only and would also end the Store package's
  fairyfly.exe (the migration from the MSI to the Store is the case this protects).
#>
param([string]$Wxs = (Join-Path (Split-Path $PSScriptRoot -Parent) 'fairyfly.wxs'))
$ErrorActionPreference = 'Stop'
$failures = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS: $msg" } else { Write-Host "FAIL: $msg"; $script:failures++ }
}

$xml = [xml](Get-Content -Raw -LiteralPath $Wxs)
$ns = New-Object Xml.XmlNamespaceManager $xml.NameTable
$ns.AddNamespace('w', 'http://wixtoolset.org/schemas/v4/wxs')

$binary = $xml.SelectSingleNode("//w:Binary[@SourceFile='`$(var.TrayExePath)']", $ns)
Check ($null -ne $binary) 'the MSI carries its own fairyfly-tray.exe (<Binary SourceFile="$(var.TrayExePath)">)'
$binaryId = if ($binary) { $binary.GetAttribute('Id') } else { '<none>' }

function CloseAction([string]$id, [string]$verb) {
    $ca = $xml.SelectSingleNode("//w:CustomAction[@Id='$id']", $ns)
    Check ($null -ne $ca) "custom action $id exists"
    if (-not $ca) { return }
    Check ($ca.GetAttribute('BinaryRef') -eq $binaryId) "$id runs the MSI's own starter (BinaryRef=$binaryId), not the installed one"
    Check (-not $ca.HasAttribute('Directory')) "$id does not run a file from the install folder"
    $cmd = $ca.GetAttribute('ExeCommand')
    Check ($cmd -match "^$verb ") "$id runs '$verb': '$cmd'"
    # [INSTALLFOLDER] ends with '\': '\"' would escape the quote; '\\"' is one backslash and the closing quote
    Check ($cmd -match '"\[INSTALLFOLDER\]\\"$') "$id names the install folder as `"[INSTALLFOLDER]\`": '$cmd'"
    Check ($ca.GetAttribute('Execute') -eq 'immediate') "$id runs immediately (before InstallValidate checks files in use)"
    Check ($ca.GetAttribute('Return') -eq 'ignore') "$id never fails the installation"
}
CloseAction 'CloseTrays' '--close'
CloseAction 'UninstallCleanup' '--uninstall'

function Scheduled([string]$id) { $xml.SelectSingleNode("//w:InstallExecuteSequence/w:Custom[@Action='$id']", $ns) }
$close = Scheduled 'CloseTrays'
$uninstall = Scheduled 'UninstallCleanup'
Check ($close -and $close.GetAttribute('Before') -eq 'InstallValidate') 'CloseTrays is scheduled before InstallValidate'
Check ($uninstall -and $uninstall.GetAttribute('Before') -eq 'InstallValidate') 'UninstallCleanup is scheduled before InstallValidate'
if ($close) {
    # a first install or a major upgrade (the new product is not installed yet); not a repair or modify, which replaces
    # no file of the same version and would leave the trays closed (a silent repair never starts them again)
    Check ($close.GetAttribute('Condition') -eq 'NOT Installed') "CloseTrays runs on install and upgrade only: '$($close.GetAttribute('Condition'))'"
}
if ($uninstall) {
    Check ($uninstall.GetAttribute('Condition') -eq 'REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE') "UninstallCleanup runs on an uninstall only: '$($uninstall.GetAttribute('Condition'))'"
}
$byName = $xml.SelectNodes("//*[local-name()='CloseApplication']")
Check ($byName.Count -eq 0) 'no util:CloseApplication (it matches by file name and would end the Store fairyfly.exe too)'

if ($failures) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host 'all checks passed'
