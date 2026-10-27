<#
.SYNOPSIS
  Installs the WiX 5.0.2 .NET tool from a package whose SHA-512 is pinned here (release.yml; tested by
  packaging/sign/tests/Test-ReleaseWorkflow.ps1).
.DESCRIPTION
  Downloads wix.5.0.2.nupkg from nuget.org, compares its SHA-512 with the pin (the packageHash of the nuget.org
  catalog, 2024-10-05), and installs the tool from that verified file only (a NuGet config with no other source).
  A mismatch throws before anything is installed. In release.yml it runs before azure/login, so nothing downloaded
  later shares the job with a signing token.
#>
param(
    [string]$Version = '5.0.2',
    [string]$ExpectedSha512 = 'z+fu1deTcaV+TRCo4+6HrJ48Sdnj+yeQjjMStTI3GW7NxFEPaknYCdxrf0qEvgdXrjYdjLnkZKCsNSqTTOo2OA=='
)
$ErrorActionPreference = 'Stop'
$dir = Join-Path ([IO.Path]::GetTempPath()) "wix-$Version-$PID"
New-Item -ItemType Directory -Force $dir | Out-Null
try {
    $nupkg = Join-Path $dir "wix.$Version.nupkg"
    Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/wix/$Version/wix.$Version.nupkg" -OutFile $nupkg
    $sha = [Convert]::ToBase64String([Security.Cryptography.SHA512]::HashData([IO.File]::ReadAllBytes($nupkg)))
    if ($sha -cne $ExpectedSha512) { throw "wix.$Version.nupkg SHA-512 is $sha, expected $ExpectedSha512; not installed" }
    Write-Host "wix.$Version.nupkg SHA-512 OK ($sha)"
    $config = Join-Path $dir 'nuget.config'
    @"
<?xml version="1.0" encoding="utf-8"?>
<configuration><packageSources><clear /><add key="verified" value="$dir" /></packageSources></configuration>
"@ | Set-Content -LiteralPath $config -Encoding utf8
    dotnet tool install --global wix --version $Version --configfile $config
    if ($LASTEXITCODE -ne 0) { throw "dotnet tool install wix $Version failed: $LASTEXITCODE" }
    # the global tools folder is on PATH on the GitHub runners; make sure for this process too
    $tools = Join-Path $HOME '.dotnet/tools'
    if (($env:PATH -split [IO.Path]::PathSeparator) -notcontains $tools) { $env:PATH = "$tools$([IO.Path]::PathSeparator)$env:PATH" }
}
finally { Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue }
