# placeholder (red): the check release.yml did until now, signtool verify /pa only
param([Parameter(Mandatory)][string[]]$Path, [Parameter(Mandatory)][string]$ExpectedOrganization, [string]$ExpectedCountry = 'DE')
$ErrorActionPreference = 'Stop'
$signtool = (Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe | Where-Object { $_.FullName -like '*\x64\*' } | Sort-Object FullName | Select-Object -Last 1).FullName
foreach ($p in $Path) {
    & $signtool verify /pa /v $p | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "signtool verify failed for $p" }
}
