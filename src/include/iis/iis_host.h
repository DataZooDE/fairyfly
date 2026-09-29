#pragma once
// Facade over everything the IIS integration touches on the machine: IIS configuration, the
// certificate store, the firewall, ACLs, files and a TCP probe. The real implementation
// (powershell_host.h) runs constant PowerShell scripts; tests use an in-memory fake, so no unit
// test ever changes or even reads the real system.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fairyfly::iis {

enum class Change { Created, Updated, Unchanged };
const char* change_name(Change change);

struct HostFacts {
    bool elevated = false;
    bool iis_installed = false;          ///< W3SVC service / inetsrv present
    std::string admin_module;            ///< "WebAdministration", "IISAdministration" or ""
    bool url_rewrite = false;
    bool arr_installed = false;
    bool arr_proxy_enabled = false;
    bool ip_security = false;            ///< Web-IP-Security feature (needed for --allow-ip)
};

struct CertInfo {
    std::string thumbprint;              ///< upper-case SHA-1
    std::string subject;
    std::vector<std::string> dns_names;  ///< SAN DNS entries plus the subject CN
    int64_t not_after = 0;               ///< unix seconds
    bool has_private_key = false;
};

struct BindingInfo {
    std::string protocol;                ///< "https"
    std::string host;
    int port = 0;
    std::string thumbprint;
};

struct SiteInfo {
    bool exists = false;
    std::string state;                   ///< "Started", "Stopped", ...
    std::string physical_path;
    std::string app_pool;
    std::vector<BindingInfo> bindings;
};

struct AppPoolInfo {
    bool exists = false;
    std::string runtime_version;         ///< "" = No Managed Code
    std::string identity;                ///< "ApplicationPoolIdentity"
    std::string state;
};

struct SiteSpec {
    std::string name;
    std::string physical_path;
    std::string app_pool;
    std::string hostname;
    int port = 8443;
    std::string thumbprint;
};

/// Thrown by a host operation that failed; the message never contains secrets.
struct HostError {
    std::string code;                    ///< e.g. IIS_SCRIPT_FAILED, IIS_SCRIPT_TIMEOUT
    std::string message;
};

class IisHost {
public:
    virtual ~IisHost() = default;

    // Read-only detection and queries.
    virtual HostFacts detect() = 0;
    virtual SiteInfo get_site(const std::string& site_name) = 0;
    virtual AppPoolInfo get_app_pool(const std::string& pool_name) = 0;
    virtual std::optional<CertInfo> find_certificate(const std::string& thumbprint) = 0;
    /// A certificate previously created by `setup --self-signed` for this host (friendly name marker).
    virtual std::optional<CertInfo> find_self_signed(const std::string& hostname) = 0;
    virtual bool firewall_rule_exists(const std::string& rule_name) = 0;
    virtual bool tcp_reachable(const std::string& host, int port, int timeout_ms) = 0;

    // Read-only helpers on files.
    virtual bool dir_exists(const std::string& path) = 0;
    virtual std::optional<std::string> read_file(const std::string& path) = 0;

    // Changes. Each is idempotent and reports what it did.
    virtual Change create_dir(const std::string& path) = 0;
    virtual Change write_file(const std::string& path, const std::string& content) = 0;
    virtual bool remove_file(const std::string& path) = 0;
    virtual bool remove_dir_if_empty(const std::string& path) = 0;
    virtual CertInfo create_self_signed(const std::string& hostname, int valid_days) = 0;
    virtual Change export_certificate(const std::string& thumbprint, const std::string& cer_path) = 0;
    virtual Change ensure_app_pool(const std::string& pool_name) = 0;
    virtual Change ensure_site(const SiteSpec& spec) = 0;
    /// Adds the server variables to allowedServerVariables of the site and unlocks system.webServer/security/ipSecurity.
    virtual Change ensure_site_config_access(const std::string& site_name, const std::vector<std::string>& server_variables) = 0;
    /// web.config readable by the app pool identity and Administrators/SYSTEM only.
    virtual Change restrict_acl(const std::string& path, const std::string& app_pool) = 0;
    virtual Change ensure_firewall_rule(const std::string& rule_name, int port) = 0;
    virtual bool remove_firewall_rule(const std::string& rule_name) = 0;
    virtual bool remove_site(const std::string& site_name) = 0;
    virtual bool remove_app_pool(const std::string& pool_name) = 0;
    virtual bool remove_certificate(const std::string& thumbprint) = 0;
};

} // namespace fairyfly::iis
