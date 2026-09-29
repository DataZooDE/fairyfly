#pragma once
// Orchestration of `fairyfly mcp iis setup|status|remove` on top of IisHost. Pure logic: all effects go
// through the injected host, credential store and clocks, so every path is unit-testable with fakes.

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/credential_store.h"
#include "include/iis/iis_host.h"
#include "include/iis/web_config.h"

namespace fairyfly::iis {

inline constexpr const char* kProxySecretCredential = "fairyfly-mcp-proxy";  ///< Credential Manager name (target "fairyfly:fairyfly-mcp-proxy")
inline constexpr const char* kManifestFile = "fairyfly-iis.json";
inline constexpr int kCertWarnDays = 30;

struct SetupOptions {
    std::string hostname;
    int port = 8443;
    std::string upstream = "http://127.0.0.1:8383";
    std::string site_name = "fairyfly-mcp";
    std::string site_path = "C:\\inetpub\\fairyfly-mcp";
    std::string cert_thumbprint;          ///< existing LocalMachine\My certificate
    bool self_signed = false;
    std::vector<std::string> allow_ips;
    bool allow_any_ip = false;
    bool rotate_secret = false;
    bool open_firewall = false;
    bool dry_run = false;
    bool yes = false;
};

struct StatusOptions {
    std::string site_name = "fairyfly-mcp";
    std::string site_path = "C:\\inetpub\\fairyfly-mcp";
};

struct RemoveOptions {
    std::string site_name = "fairyfly-mcp";
    std::string site_path = "C:\\inetpub\\fairyfly-mcp";
    bool remove_cert = false;
    bool remove_firewall = false;
    bool delete_secret = false;
    bool dry_run = false;
    bool yes = false;
};

struct IisContext {
    IisHost& host;
    cred::CredentialStore& secrets;
    std::function<std::string()> generate_secret;   ///< 32 CSPRNG bytes, hex encoded
    std::function<int64_t()> now;                   ///< unix seconds
};

/// Outcome of one operation; the command layer maps it to a Result.
struct Report {
    bool ok = true;
    std::string error_code;
    std::string error_message;
    nlohmann::json data = nlohmann::json::object();   ///< success data, or error details when !ok
};

/// One prerequisite line: id, label, ok, detail, fix (hint text, never executed).
nlohmann::json build_checklist(const HostFacts& facts, bool needs_ip_security);
/// Ids of failed items in a checklist produced by build_checklist.
std::vector<std::string> missing_ids(const nlohmann::json& checklist);

/// Validates every option; returns the first problem as {field, message}, or nullopt.
struct OptionProblem {
    std::string field;
    std::string message;
};
std::optional<OptionProblem> validate(const SetupOptions& o);
std::optional<OptionProblem> validate_site(const std::string& site_name, const std::string& site_path);

Report run_setup(IisContext& ctx, const SetupOptions& options);
Report run_status(IisContext& ctx, const StatusOptions& options);
Report run_remove(IisContext& ctx, const RemoveOptions& options);

/// Name of the firewall rule for a port.
std::string firewall_rule_name(int port);
std::string manifest_path(const std::string& site_path);
std::string web_config_path(const std::string& site_path);

} // namespace fairyfly::iis
