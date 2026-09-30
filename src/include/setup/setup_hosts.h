#pragma once
// Host interfaces of the http.sys setup layer. Everything that touches the machine (http.sys configuration,
// the LocalMachine\My store, the firewall, elevation, sockets, files, the TLS round trip) sits behind one of
// these, so the planner and the service are exercised with the in-memory fakes of fake_hosts.h. The real
// implementations live in src/setup/*_win.cpp (native Win32 wherever possible, PowerShell only for
// New-SelfSignedCertificate, certificate removal and the firewall).

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fairyfly::sys { class PowerShellRunner; }

namespace fairyfly::setup {

/// Fixed application id under which fairyfly registers its sslcert bindings. A binding with a different id
/// belongs to somebody else and is never replaced without --force-binding.
inline constexpr const char* kAppId = "{8e2b5c3a-4d17-4f6a-b9c0-7a1d3e5f2b64}";
inline constexpr const char* kCertStore = "MY";
inline constexpr int kDefaultTlsPort = 8443;
inline constexpr int kDefaultNoTlsPort = 8383;
inline constexpr int kSelfSignedDays = 730;
inline constexpr int kExpiryWarnDays = 30;

/// Failure of a host operation. `code` is stable (ACCESS_DENIED, ALREADY_EXISTS, NOT_FOUND, HOST_ERROR,
/// PS_SCRIPT_FAILED ...); the message never contains secrets.
class HostError : public std::runtime_error {
public:
    HostError(std::string code, const std::string& message) : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const { return code_; }

private:
    std::string code_;
};

// ---- http.sys configuration (urlacl + sslcert) ----------------------------------------------------
struct SslBinding {
    std::string ipport;        ///< "0.0.0.0:8443" or "[::]:8443"
    std::string thumbprint;    ///< upper-case SHA-1 hex
    std::string app_id;        ///< "{guid}" lower-case
    std::string store;         ///< "MY"
};

class HttpSysConfig {
public:
    virtual ~HttpSysConfig() = default;
    /// SDDL of the reservation of exactly `prefix` ("https://+:8443/mcp/"); nullopt = not reserved.
    virtual std::optional<std::string> query_urlacl(const std::string& prefix) = 0;
    /// Reserves `prefix` with the SDDL. Throws HostError (ACCESS_DENIED without elevation, ALREADY_EXISTS).
    virtual void add_urlacl(const std::string& prefix, const std::string& sddl) = 0;
    virtual bool remove_urlacl(const std::string& prefix) = 0;
    virtual std::optional<SslBinding> query_sslcert(const std::string& ipport) = 0;
    /// Creates or replaces the binding of `ipport` (fairyfly AppId, store MY, no client certificate negotiation).
    virtual void set_sslcert(const std::string& ipport, const std::string& thumbprint) = 0;
    virtual bool remove_sslcert(const std::string& ipport) = 0;
};

// ---- certificate store ---------------------------------------------------------------------------
struct CertInfo {
    bool found = false;
    std::string thumbprint;
    std::string subject;
    std::string friendly_name;
    std::vector<std::string> dns_names;   ///< SAN DNS names (CN when the certificate has no SAN)
    long long not_after = 0;              ///< unix seconds
    bool has_private_key = false;
};

enum class CerFormat { Der, Pem };
enum class CerFileState { Missing, Matches, Differs };

class CertStore {
public:
    virtual ~CertStore() = default;
    virtual CertInfo find_by_thumbprint(const std::string& thumbprint) = 0;
    /// Newest certificate in LocalMachine\My with FriendlyName "fairyfly-mcp <hostname>".
    virtual CertInfo find_self_signed(const std::string& hostname) = 0;
    virtual CertInfo create_self_signed(const std::string& hostname, int valid_days) = 0;
    virtual CerFileState cer_file_state(const std::string& thumbprint, const std::string& path, CerFormat format) = 0;
    /// Writes the public certificate; returns created | updated | unchanged.
    virtual std::string export_cer(const std::string& thumbprint, const std::string& path, CerFormat format) = 0;
    /// Deletes the certificate and its private key; false when it was not there.
    virtual bool remove(const std::string& thumbprint) = 0;
};

// ---- firewall ------------------------------------------------------------------------------------
class Firewall {
public:
    virtual ~Firewall() = default;
    virtual bool exists(const std::string& rule_name) = 0;
    /// Inbound TCP allow rule; returns created | updated | unchanged.
    virtual std::string ensure(const std::string& rule_name, int port) = 0;
    virtual bool remove(const std::string& rule_name) = 0;
};

// ---- elevation ------------------------------------------------------------------------------------
enum class ElevationType { Elevated, AdminFiltered, StandardUser, Unknown };
const char* elevation_name(ElevationType type);   ///< elevated | admin_filtered | standard_user | unknown

struct ElevatedRun {
    bool declined = false;        ///< the user cancelled the UAC prompt (ERROR_CANCELLED)
    bool launched = false;
    int exit_code = 0;
    nlohmann::json result;        ///< contents of the result file (null when none was written)
    std::string error;
};

class Elevator {
public:
    virtual ~Elevator() = default;
    virtual bool is_elevated() = 0;
    virtual ElevationType elevation_type() = 0;
    virtual std::string current_user_sid() = 0;
    virtual std::string current_user_name() = 0;                                 ///< DOMAIN\user
    virtual std::optional<std::string> resolve_user_sid(const std::string& user) = 0;
    /// Runs `<exe> mcp setup --apply-plan <planfile> --result-file <resultfile>` elevated (one UAC prompt):
    /// the plan is written to a file, the child writes its per-step results to the result file.
    virtual ElevatedRun run_elevated(const nlohmann::json& plan) = 0;
};

// ---- system probes --------------------------------------------------------------------------------
struct TlsProbe {
    std::string status;           ///< ok | unreachable | handshake_failed | error
    std::string protocol;         ///< TLS 1.2, TLS 1.3, TLS 1.0 ...
    std::string thumbprint;       ///< SHA-1 of the presented certificate
    bool thumbprint_match = false;
    int http_status = 0;
    std::string detail;
};

class SystemProbe {
public:
    virtual ~SystemProbe() = default;
    virtual bool tcp_listening(const std::string& host, int port) = 0;
    /// TLS handshake pinned by thumbprint: any certificate error is accepted, but the presented certificate's
    /// SHA-1 must equal `expected_thumbprint` (thumbprint_match). No secret is ever sent.
    virtual TlsProbe tls_probe(const std::string& host, int port, const std::string& expected_thumbprint) = 0;
    virtual std::optional<std::string> read_file(const std::string& path) = 0;
    virtual bool write_file(const std::string& path, const std::string& text) = 0;
    virtual bool remove_file(const std::string& path) = 0;
    virtual bool file_exists(const std::string& path) = 0;
    virtual long long now() = 0;                     ///< unix seconds
    virtual std::string computer_dns_name() = 0;
    virtual std::string local_app_data() = 0;        ///< %LOCALAPPDATA%
};

// ---- verification round trip ----------------------------------------------------------------------
struct VerifyRequest {
    bool tls = true;
    std::string hostname;
    int port = 0;
    std::string expected_thumbprint;
    std::string owner_sid;        ///< SID the urlacl was created for (an in-process server only works for that user)
};

struct VerifyResult {
    std::string status;           ///< ok | failed | skipped
    std::string protocol;
    int http_status = 0;
    bool thumbprint_match = false;
    std::string detail;
};

class VerifyHost {
public:
    virtual ~VerifyHost() = default;
    /// Proves the listener end to end: an unauthenticated POST must answer 401 AUTH_REQUIRED with a
    /// WWW-Authenticate header. Binds a deny-all in-process server for the round trip, or, when the prefix is
    /// already served (tray running), talks to the running server.
    virtual VerifyResult round_trip(const VerifyRequest& request) = 0;
};

/// The hosts a SetupService works with.
struct Hosts {
    HttpSysConfig& http;
    CertStore& certs;
    Firewall& firewall;
    Elevator& elevator;
    SystemProbe& sys;
    VerifyHost& verify;
};

// ---- real implementations (Windows) ---------------------------------------------------------------
std::unique_ptr<HttpSysConfig> make_real_http_sys_config();
std::unique_ptr<CertStore> make_real_cert_store(sys::PowerShellRunner& runner);
std::unique_ptr<Firewall> make_real_firewall(sys::PowerShellRunner& runner);
std::unique_ptr<Elevator> make_real_elevator();
std::unique_ptr<SystemProbe> make_real_system_probe();
std::unique_ptr<VerifyHost> make_real_verify_host();

/// Pure helpers shared by the real hosts and tests.
std::string normalize_app_id(const std::string& app_id);   ///< lower-case, braces
bool is_our_app_id(const std::string& app_id);
/// Parses "0.0.0.0:8443" / "[::]:8443". False when malformed.
bool parse_ipport(const std::string& ipport, std::string* ip, int* port);

} // namespace fairyfly::setup
