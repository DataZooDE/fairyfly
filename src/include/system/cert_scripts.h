#pragma once
// Constant PowerShell scripts for the few machine changes that have no native C++ path here: certificate
// lookup/creation/export/removal in Cert:\LocalMachine\My and the inbound firewall rule. Every script is
// identical for every call; user input travels only as JSON in $env:FAIRYFLY_PS_PARAMS (see the
// *_params helpers, one per script). Results are compact JSON:
//   kFindCert / kFindSelfSigned / kCreateSelfSigned -> {found, thumbprint, subject, dns_names[], not_after (unix s), has_private_key}
//   kExportCert -> {change: created|updated|unchanged}      kRemoveCert -> {removed}
//   kFirewallExists / kFirewallDisplayExists -> {exists}   kFirewallEnsure -> {change}   kFirewallRemove -> {removed}
// Run them with a PowerShellRunner (include/system/powershell_runner.h). Validate inputs with
// include/setup/setup_validators.h first.

#include <string>
#include <utility>
#include <vector>

namespace fairyfly::sys {

extern const std::string kFindCert;          ///< params: thumbprint
extern const std::string kFindSelfSigned;    ///< params: hostname (friendly name "fairyfly-mcp <hostname>")
extern const std::string kCreateSelfSigned;  ///< params: hostname, valid_days
extern const std::string kExportCert;        ///< params: thumbprint, path (DER .cer)
extern const std::string kRemoveCert;        ///< params: thumbprint (also deletes the private key)
extern const std::string kFirewallExists;         ///< params: name (the rule's internal Name)
extern const std::string kFirewallDisplayExists;  ///< params: display_name
extern const std::string kFirewallEnsure;         ///< params: name, display_name, port (inbound TCP, any profile; by Name)
extern const std::string kFirewallRemove;         ///< params: name (by Name only)

std::string find_cert_params(const std::string& thumbprint);
std::string find_self_signed_params(const std::string& hostname);
std::string create_self_signed_params(const std::string& hostname, int valid_days);
std::string export_cert_params(const std::string& thumbprint, const std::string& path);
std::string remove_cert_params(const std::string& thumbprint);
std::string firewall_name_params(const std::string& name);            ///< for kFirewallExists and kFirewallRemove
std::string firewall_display_params(const std::string& display_name); ///< for kFirewallDisplayExists
std::string firewall_ensure_params(const std::string& name, const std::string& display_name, int port);

/// Every script by name, so tests can assert they are free of interpolation.
const std::vector<std::pair<std::string, std::string>>& script_catalog();

} // namespace fairyfly::sys
