#pragma once
// Pure core of `fairyfly mcp setup|teardown`: options validation, the Diagnosis facts, MakePlan / MakeTeardownPlan
// (no I/O, no clock, no machine access), the runbook text, the "Left for a human" list, the manifest and the
// text/JSON rendering. SetupService (setup_service.h) gathers the Diagnosis from the hosts and applies a Plan.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/setup/setup_hosts.h"

namespace fairyfly::setup {

// ---- options --------------------------------------------------------------------------------------
enum class CertMode { Unset, SelfSigned, Thumbprint, NoTls };

struct ErrorInfo {
    std::string code;      ///< INVALID_ARGUMENT, CONFIRMATION_REQUIRED, ELEVATION_REQUIRED, READ_ONLY, ...
    std::string message;
    int exit_code = 2;
};

struct Options {
    std::string hostname;                 ///< empty = lower-cased computer DNS name (filled by the service)
    int port = 0;                         ///< 0 = 8443 (TLS) / 8383 (--no-tls)
    bool self_signed = false;             ///< --self-signed
    bool no_tls = false;                  ///< --no-tls
    std::string thumbprint;               ///< --cert-thumbprint
    CertMode cert_mode = CertMode::Unset; ///< derived by validate_options
    std::vector<std::string> allow_ip;
    std::string user;                     ///< --user DOMAIN\user; empty = the current user (SID from the token)
    bool open_firewall = false;
    bool force_binding = false;
    bool dry_run = false;
    bool yes = false;
    bool non_interactive = false;
    bool print_runbook = false;
    bool json = false;                    ///< --output json
    std::string config_path;              ///< mcp.yaml to create (resolved by the command)
    std::string command_line;             ///< human-readable re-run command for ELEVATION_REQUIRED (filled by the command)
};

struct TeardownOptions {
    std::string hostname;
    int port = 0;
    bool keep_cert = false;
    bool keep_firewall = false;
    bool dry_run = false;
    bool yes = false;
    bool non_interactive = false;
    bool json = false;
    std::string config_path;
    std::string command_line;
};

/// Exactly one of --self-signed/--cert-thumbprint/--no-tls; hostname, thumbprint, port, CIDR, user syntax;
/// --no-tls with --open-firewall is refused. Normalises thumbprint (upper-case) and hostname (lower-case).
std::optional<ErrorInfo> validate_options(Options& options);
std::optional<ErrorInfo> validate_teardown_options(TeardownOptions& options);

// ---- manifest -------------------------------------------------------------------------------------
struct Manifest {
    int schema = 1;
    std::string mode;                     ///< tls | no-tls
    std::string hostname;
    int port = 0;
    std::vector<std::string> prefixes;
    std::vector<std::string> ipports;
    std::string sid;
    std::string user;
    std::string cert_mode;                ///< self-signed | thumbprint | none
    std::string thumbprint;
    std::string cer_path;
    std::string firewall_rule;            ///< display name of the rule setup created ("fairyfly MCP HTTPS <port>"); empty = none
    std::string firewall_rule_name;       ///< its unique internal Name ("fairyfly-mcp-https-<port>-<8 hex>"); teardown removes by this only
    std::string appid;
    std::string created_at;
    std::string updated_at;
    bool config_created = false;          ///< setup created mcp.yaml (teardown may delete it while unmodified)
    std::string config_sha256;            ///< sha256 of the text setup wrote
    std::string urlacl_sddl;              ///< SDDL of the URL reservation as setup left it (teardown removes only an unchanged one)
    bool urlacl_created = false;          ///< setup created the reservation (false: it found an existing one)

    nlohmann::json to_json() const;
    static std::optional<Manifest> from_json(const nlohmann::json& j);
    bool same_setup(const Manifest& other) const;   ///< everything but the timestamps
};

// ---- diagnosis ------------------------------------------------------------------------------------
enum class CerState { Missing, Matches, Differs };

struct Diagnosis {
    long long now = 0;
    ElevationType elevation = ElevationType::Unknown;
    std::string sid;
    std::string user;
    std::string identity_error;           ///< --user could not be resolved
    // effective target
    bool tls = true;
    std::string hostname;
    int port = 0;
    std::string prefix;
    std::vector<std::string> ipports;     ///< empty for --no-tls
    // machine facts
    std::optional<Manifest> manifest;
    bool manifest_unreadable = false;
    std::optional<std::string> urlacl_sddl;
    std::map<std::string, std::optional<SslBinding>> ssl;     ///< per ipport
    CertInfo cert;                        ///< the certificate that would be used (by thumbprint or self-signed name)
    CerState cer = CerState::Missing;
    bool firewall_exists = false;         ///< the rule setup recorded (by internal Name) exists
    bool firewall_display_foreign = false; ///< a rule with the display name exists that setup did not create
    std::string firewall_name;            ///< internal Name to use: the recorded one, else a fresh unique one
    bool port_listening = false;
    bool config_exists = false;
    bool config_unreadable = false;
    std::map<std::string, std::string> config_current;   ///< raw YAML values of the keys setup cares about
    TlsProbe tls_probe;                   ///< only when the port is already served (status empty otherwise)
    // paths
    std::string cer_path;
    std::string manifest_path;
    std::string config_path;
};

// ---- plan -----------------------------------------------------------------------------------------
struct CheckItem {
    std::string id;
    std::string status;                   ///< ok | missing | mismatch | warn | info | skip
    std::string detail;
};

struct StepItem {
    std::string id;                       ///< certificate, urlacl, sslcert, firewall, certificate_export, config, manifest
    std::string title;
    std::string status;                   ///< dry run: would_create|would_update|unchanged|skipped|blocked; apply: created|updated|unchanged|skipped|failed
    std::string detail;
    bool elevated = false;
};

struct HumanItem {
    std::string id;
    std::string text;
};

/// Teardown work list (empty for setup).
struct TeardownWork {
    std::vector<std::string> ipports;
    std::vector<std::string> prefixes;
    std::map<std::string, std::string> urlacl_expected;   ///< prefix -> SDDL setup recorded; the child removes only an unchanged one
    std::string firewall_rule;
    std::string cert_thumbprint;
    std::string cer_path;
    std::string manifest_path;
    std::string config_path;              ///< mcp.yaml to delete (created by setup, unmodified)
};

struct Plan {
    std::string operation = "setup";      ///< setup | teardown
    bool tls = true;
    std::string hostname;
    int port = 0;
    std::string url;
    std::string prefix;
    std::vector<std::string> ipports;
    std::string sid;
    std::string user;
    ElevationType elevation = ElevationType::Unknown;
    std::vector<CheckItem> diagnosis;
    std::vector<StepItem> steps;
    std::vector<HumanItem> human;
    std::vector<std::string> next_steps;
    bool nothing = false;                 ///< every step is unchanged/skipped
    bool blocked = false;
    bool needs_elevation = false;         ///< at least one elevated step will change something
    std::optional<ErrorInfo> error;
    // inputs of apply
    CertMode cert_mode = CertMode::Unset;
    bool create_cert = false;
    std::string thumbprint;               ///< certificate to bind when it exists already; empty when it will be created
    std::string sddl;                     ///< urlacl SDDL to set (existing entries kept, our SID added)
    bool replace_urlacl = false;
    bool force_binding = false;
    bool open_firewall = false;
    std::string firewall_rule;            ///< internal Name of the rule (existing recorded one or a fresh unique one)
    std::string firewall_display;         ///< display name for humans
    std::string cer_path;
    std::string manifest_path;
    std::string config_path;
    std::string config_yaml;              ///< text to write when the config step creates the file
    Manifest manifest;                    ///< desired manifest (thumbprint filled in after the certificate exists)
    TeardownWork teardown;

    const StepItem* step(const std::string& id) const;
    StepItem* step(const std::string& id);
};

Plan MakePlan(const Diagnosis& diagnosis, const Options& options);

struct TeardownDiagnosis {
    long long now = 0;
    ElevationType elevation = ElevationType::Unknown;
    std::optional<Manifest> manifest;
    bool manifest_unreadable = false;
    bool explicit_target = false;         ///< --hostname/--port given
    std::string hostname;
    int port = 0;
    std::vector<std::string> urlacls;             ///< reserved candidate prefixes
    std::map<std::string, std::string> urlacl_sddl;   ///< current SDDL per reserved prefix (absent when it could not be read)
    std::vector<std::string> ssl_ours;            ///< ipports bound with our AppId
    std::vector<std::string> ssl_foreign;         ///< ipports bound by somebody else (never touched)
    std::string firewall_rule;                    ///< internal Name recorded in the manifest (empty when none/invalid)
    std::string firewall_legacy_display;          ///< display name recorded by an older setup without an internal Name
    bool firewall_exists = false;
    CertInfo cert;                                ///< certificate named in the manifest
    bool cer_exists = false;
    bool cer_matches = false;                     ///< the file at cer_path holds the certificate the manifest records
    std::string cer_refusal;                      ///< non-empty: the manifest cer_path is not trusted (why)
    bool config_says_tls = false;
    bool config_exists = false;
    std::string config_sha256;                    ///< sha256 of the current file text (empty when missing/unreadable)
    std::string cer_path;
    std::string manifest_path;
    std::string config_path;
};

Plan MakeTeardownPlan(const TeardownDiagnosis& diagnosis, const TeardownOptions& options);

// ---- helpers (pure, unit-tested) ------------------------------------------------------------------
std::string lower_ascii(std::string s);
std::string default_hostname(const std::string& computer_dns_name);
std::string url_prefix(bool tls, const std::string& hostname, int port);   ///< https://+:8443/mcp/ | http://127.0.0.1:8383/mcp/
std::string endpoint_url(bool tls, const std::string& hostname, int port); ///< https://host:8443/mcp
std::vector<std::string> ssl_ipports(int port);                             ///< 0.0.0.0:PORT and [::]:PORT
std::string sddl_for_sid(const std::string& sid);                           ///< D:(A;;GX;;;SID)
bool sddl_covers_sid(const std::string& sddl, const std::string& sid);
std::string merge_sddl(const std::string& existing, const std::string& sid);
bool san_matches(const std::vector<std::string>& dns_names, const std::string& hostname);
std::string firewall_rule_name(int port);                                   ///< display name: fairyfly MCP HTTPS <port>
std::string firewall_internal_name(int port, const std::string& suffix);    ///< fairyfly-mcp-https-<port>-<suffix>
bool firewall_name_ok(const std::string& name, int* port = nullptr);       ///< fairyfly-mcp-https-<port>-<8 lower-case hex>
std::string cer_file_name(const std::string& hostname);                     ///< fairyfly-mcp-<host>.cer
std::string join_path(const std::string& dir, const std::string& leaf);
std::string iso_utc(long long unix_seconds);
std::string iso_date(long long unix_seconds);

enum class CertProblem { NoPrivateKey, Expired, WrongSan };
std::vector<CertProblem> cert_problems(const CertInfo& cert, const std::string& hostname, long long now);
std::string cert_problem_text(const std::vector<CertProblem>& problems);

/// The mcp.yaml text setup writes when the file does not exist (commented, secret free).
std::string render_config_yaml(bool tls, const std::string& hostname, int port, const std::vector<std::string>& allow_ip);
/// Keys (as "server.port: 8443") that setup wants in the config file, in file order.
std::vector<std::pair<std::string, std::string>> desired_config(bool tls, const std::string& hostname, int port,
                                                                const std::vector<std::string>& allow_ip);

struct RunbookContext {
    std::string user;                     ///< user name for netsh (DOMAIN\user)
    std::string thumbprint;               ///< placeholder when unknown
    std::string cer_path;
};
/// Manual equivalents of every step, for --print-runbook.
std::string make_runbook(const Options& options, const RunbookContext& context);
std::string make_teardown_runbook(const TeardownOptions& options, const Manifest* manifest);

// ---- report ---------------------------------------------------------------------------------------
struct VerifyOut {
    std::string status;                   ///< ok | failed | skipped
    std::string protocol;
    int http_status = 0;
    bool thumbprint_match = false;
    std::string detail;
};

struct Report {
    std::string operation = "setup";
    bool dry_run = false;
    std::string url;
    std::string elevation;
    std::vector<CheckItem> diagnosis;
    std::vector<StepItem> steps;
    std::vector<HumanItem> human;
    std::optional<VerifyOut> verify;
    std::vector<std::string> next_steps;
    std::optional<ErrorInfo> error;
    bool nothing = false;
    bool has_data = true;
    std::string runbook;                  ///< --print-runbook
    std::string hostname;
};

Report report_from_plan(const Plan& plan, bool dry_run);
nlohmann::json report_to_json(const Report& report);
std::string render_plan_text(const Plan& plan, bool dry_run);
std::string render_result_text(const Report& report);   ///< steps after apply, verify, human items, next steps
std::string nothing_message(const std::string& operation);   ///< "nothing - already set up." / "nothing - already removed."

/// Manual trust commands per client platform for an exported certificate.
std::string trust_hints(const std::string& cer_path, const std::string& hostname);

} // namespace fairyfly::setup
