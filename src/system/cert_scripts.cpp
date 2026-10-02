#include "include/system/cert_scripts.h"

#include <nlohmann/json.hpp>

#include "include/system/powershell_runner.h"

namespace fairyfly::sys {

using nlohmann::json;

namespace {
// Cert-Json turns an X509Certificate2 into the object every certificate script returns.
const char kCertHelpers[] = R"PS(
function Cert-Json($c) {
  $names = @()
  if ($c.DnsNameList) { foreach ($n in $c.DnsNameList) { $names += $n.Unicode } }
  $cn = $c.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
  $hasSan = @($c.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.17' }).Count -gt 0
  if ($cn -and -not $hasSan) { $names += $cn }
  [ordered]@{
    thumbprint = $c.Thumbprint.ToUpper()
    subject = $c.Subject
    dns_names = @($names | Select-Object -Unique)
    not_after = [int64][DateTimeOffset]::new($c.NotAfter.ToUniversalTime()).ToUnixTimeSeconds()
    has_private_key = [bool]$c.HasPrivateKey
  }
}
)PS";
} // namespace

// Parameters are read from $env:FAIRYFLY_PS_PARAMS (JSON); results are compact JSON. The text is identical for every call.
const std::string kFindCert = std::string(kPsPreamble) + kCertHelpers + R"PS(
$c = Get-Item -LiteralPath ('Cert:\LocalMachine\My\' + $p.thumbprint) -ErrorAction SilentlyContinue
if ($null -eq $c) { Out-Json ([ordered]@{ found = $false }) }
else { $o = Cert-Json $c; $o['found'] = $true; Out-Json $o }
)PS";

const std::string kFindSelfSigned = std::string(kPsPreamble) + kCertHelpers + R"PS(
$c = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.FriendlyName -eq ('fairyfly-mcp ' + $p.hostname) } | Sort-Object NotAfter -Descending | Select-Object -First 1
if ($null -eq $c) { Out-Json ([ordered]@{ found = $false }) }
else { $o = Cert-Json $c; $o['found'] = $true; Out-Json $o }
)PS";

const std::string kCreateSelfSigned = std::string(kPsPreamble) + kCertHelpers + R"PS(
$c = New-SelfSignedCertificate -Subject ('CN=' + $p.hostname) -DnsName $p.hostname -CertStoreLocation 'Cert:\LocalMachine\My' `
  -FriendlyName ('fairyfly-mcp ' + $p.hostname) -NotAfter (Get-Date).AddDays([int]$p.valid_days) `
  -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyUsage DigitalSignature, KeyEncipherment `
  -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.1')
$o = Cert-Json $c
$o['found'] = $true
Out-Json $o
)PS";

const std::string kExportCert = std::string(kPsPreamble) + kCertHelpers + R"PS(
$c = Get-Item -LiteralPath ('Cert:\LocalMachine\My\' + $p.thumbprint)
$change = 'created'
if (Test-Path -LiteralPath $p.path) {
  $existing = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $p.path
  if ($existing.Thumbprint -eq $c.Thumbprint) { $change = 'unchanged' } else { $change = 'updated' }
}
if ($change -ne 'unchanged') { Export-Certificate -Cert $c -FilePath $p.path -Type CERT -Force | Out-Null }
Out-Json ([ordered]@{ change = $change })
)PS";

const std::string kRemoveCert = std::string(kPsPreamble) + kCertHelpers + R"PS(
$path = 'Cert:\LocalMachine\My\' + $p.thumbprint
if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -DeleteKey; Out-Json ([ordered]@{ removed = $true }) }
else { Out-Json ([ordered]@{ removed = $false }) }
)PS";

// Rules are addressed by their internal Name only (validated, never a wildcard); the display name is for humans.
const std::string kFirewallExists = std::string(kPsPreamble) + R"PS(
$r = Get-NetFirewallRule -Name $p.name -ErrorAction SilentlyContinue
Out-Json ([ordered]@{ exists = ($null -ne $r) })
)PS";

const std::string kFirewallDisplayExists = std::string(kPsPreamble) + R"PS(
$r = Get-NetFirewallRule -DisplayName $p.display_name -ErrorAction SilentlyContinue
Out-Json ([ordered]@{ exists = ($null -ne $r) })
)PS";

const std::string kFirewallEnsure = std::string(kPsPreamble) + R"PS(
$r = Get-NetFirewallRule -Name $p.name -ErrorAction SilentlyContinue
if ($null -eq $r) {
  New-NetFirewallRule -Name $p.name -DisplayName $p.display_name -Direction Inbound -Action Allow -Protocol TCP -LocalPort ([int]$p.port) -Profile Any | Out-Null
  Out-Json ([ordered]@{ change = 'created' })
} else {
  $pf = $r | Get-NetFirewallPortFilter
  if ([string]$pf.LocalPort -eq [string]$p.port) { Out-Json ([ordered]@{ change = 'unchanged' }) }
  else { $pf | Set-NetFirewallPortFilter -LocalPort ([int]$p.port); Out-Json ([ordered]@{ change = 'updated' }) }
}
)PS";

const std::string kFirewallRemove = std::string(kPsPreamble) + R"PS(
$r = Get-NetFirewallRule -Name $p.name -ErrorAction SilentlyContinue
if ($null -eq $r) { Out-Json ([ordered]@{ removed = $false }) }
else { $r | Remove-NetFirewallRule; Out-Json ([ordered]@{ removed = $true }) }
)PS";

std::string find_cert_params(const std::string& thumbprint) { return json{{"thumbprint", thumbprint}}.dump(); }

std::string find_self_signed_params(const std::string& hostname) { return json{{"hostname", hostname}}.dump(); }

std::string create_self_signed_params(const std::string& hostname, int valid_days) {
    return json{{"hostname", hostname}, {"valid_days", valid_days}}.dump();
}

std::string export_cert_params(const std::string& thumbprint, const std::string& path) {
    return json{{"thumbprint", thumbprint}, {"path", path}}.dump();
}

std::string remove_cert_params(const std::string& thumbprint) { return json{{"thumbprint", thumbprint}}.dump(); }

std::string firewall_name_params(const std::string& name) { return json{{"name", name}}.dump(); }

std::string firewall_display_params(const std::string& display_name) { return json{{"display_name", display_name}}.dump(); }

std::string firewall_ensure_params(const std::string& name, const std::string& display_name, int port) {
    return json{{"name", name}, {"display_name", display_name}, {"port", port}}.dump();
}

const std::vector<std::pair<std::string, std::string>>& script_catalog() {
    static const std::vector<std::pair<std::string, std::string>> catalog = {
        {"find_cert", kFindCert},
        {"find_self_signed", kFindSelfSigned},
        {"create_self_signed", kCreateSelfSigned},
        {"export_cert", kExportCert},
        {"remove_cert", kRemoveCert},
        {"firewall_exists", kFirewallExists},
        {"firewall_display_exists", kFirewallDisplayExists},
        {"firewall_ensure", kFirewallEnsure},
        {"firewall_remove", kFirewallRemove},
    };
    return catalog;
}

} // namespace fairyfly::sys

