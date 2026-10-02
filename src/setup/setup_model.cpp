#include "include/setup/setup_model.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>

#include "include/auth/crypto.h"
#include "include/setup/setup_validators.h"

namespace fairyfly::setup {

using nlohmann::json;

// ---- small helpers ---------------------------------------------------------------------------------
std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string default_hostname(const std::string& dns) { return lower_ascii(dns); }

std::string url_prefix(bool tls, const std::string& hostname, int port) {
    (void)hostname;
    return (tls ? "https://+:" : "http://127.0.0.1:") + std::to_string(port) + "/mcp/";
}

std::string endpoint_url(bool tls, const std::string& hostname, int port) {
    return (tls ? "https://" + hostname : std::string("http://127.0.0.1")) + ":" + std::to_string(port) + "/mcp";
}

std::vector<std::string> ssl_ipports(int port) {
    return {"0.0.0.0:" + std::to_string(port), "[::]:" + std::to_string(port)};
}

std::string sddl_for_sid(const std::string& sid) { return "D:(A;;GX;;;" + sid + ")"; }

bool sddl_covers_sid(const std::string& sddl, const std::string& sid) {
    const std::string s = lower_ascii(sddl);
    if (!sid.empty() && s.find(";;;" + lower_ascii(sid) + ")") != std::string::npos) return true;
    // Everyone (WD) and Authenticated Users (AU) cover any user.
    return s.find(";;;wd)") != std::string::npos || s.find(";;;au)") != std::string::npos;
}

std::string merge_sddl(const std::string& existing, const std::string& sid) {
    if (existing.empty()) return sddl_for_sid(sid);
    if (sddl_covers_sid(existing, sid)) return existing;
    if (existing.rfind("D:", 0) == 0 && existing.find("S:") == std::string::npos && existing.back() == ')')
        return existing + "(A;;GX;;;" + sid + ")";
    return sddl_for_sid(sid);
}

bool san_matches(const std::vector<std::string>& dns_names, const std::string& hostname) {
    const std::string host = lower_ascii(hostname);
    for (const auto& raw : dns_names) {
        const std::string name = lower_ascii(raw);
        if (name == host) return true;
        if (name.size() > 2 && name.rfind("*.", 0) == 0) {
            const std::string suffix = name.substr(1);   // ".example.com"
            if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
                const std::string label = host.substr(0, host.size() - suffix.size());
                if (!label.empty() && label.find('.') == std::string::npos) return true;
            }
        }
    }
    return false;
}

std::string firewall_rule_name(int port) { return "fairyfly MCP HTTPS " + std::to_string(port); }
std::string firewall_internal_name(int port, const std::string& suffix) { return "fairyfly-mcp-https-" + std::to_string(port) + "-" + suffix; }
bool firewall_name_ok(const std::string& name, int* port) {
    static const std::string prefix = "fairyfly-mcp-https-";
    if (name.rfind(prefix, 0) != 0) return false;
    const size_t dash = name.find('-', prefix.size());
    if (dash == std::string::npos || dash == prefix.size() || dash - prefix.size() > 5) return false;
    int value = 0;
    for (size_t i = prefix.size(); i < dash; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        value = value * 10 + (name[i] - '0');
    }
    if (value < 1 || value > 65535) return false;
    const std::string suffix = name.substr(dash + 1);
    if (suffix.size() != 8) return false;
    for (const char c : suffix)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    if (port) *port = value;
    return true;
}
std::string cer_file_name(const std::string& hostname) { return "fairyfly-mcp-" + hostname + ".cer"; }

std::string join_path(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    const char last = dir.back();
    return dir + (last == '\\' || last == '/' ? "" : "\\") + leaf;
}

namespace {
std::string format_time(long long unix_seconds, const char* fmt) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}
} // namespace

std::string iso_utc(long long s) { return format_time(s, "%Y-%m-%dT%H:%M:%SZ"); }
std::string iso_date(long long s) { return format_time(s, "%Y-%m-%d"); }

std::vector<CertProblem> cert_problems(const CertInfo& cert, const std::string& hostname, long long now) {
    std::vector<CertProblem> out;
    if (!cert.has_private_key) out.push_back(CertProblem::NoPrivateKey);
    if (cert.not_after <= now) out.push_back(CertProblem::Expired);
    if (!san_matches(cert.dns_names, hostname)) out.push_back(CertProblem::WrongSan);
    return out;
}

std::string cert_problem_text(const std::vector<CertProblem>& problems) {
    std::string out;
    for (const auto p : problems) {
        if (!out.empty()) out += ", ";
        out += p == CertProblem::NoPrivateKey ? "no private key" : p == CertProblem::Expired ? "expired" : "name does not match the host name";
    }
    return out;
}

const char* elevation_name(ElevationType type) {
    switch (type) {
    case ElevationType::Elevated: return "elevated";
    case ElevationType::AdminFiltered: return "admin_filtered";
    case ElevationType::StandardUser: return "standard_user";
    default: return "unknown";
    }
}

std::string normalize_app_id(const std::string& app_id) {
    std::string s = lower_ascii(app_id);
    if (!s.empty() && s.front() != '{') s = "{" + s + "}";
    return s;
}

bool is_our_app_id(const std::string& app_id) { return normalize_app_id(app_id) == normalize_app_id(kAppId); }

bool parse_ipport(const std::string& ipport, std::string* ip, int* port) {
    const auto colon = ipport.rfind(':');
    if (colon == std::string::npos || colon + 1 >= ipport.size()) return false;
    std::string host = ipport.substr(0, colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    int p = 0;
    for (char c : ipport.substr(colon + 1)) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        p = p * 10 + (c - '0');
        if (p > 65535) return false;
    }
    if (host.empty() || p < 1) return false;
    if (ip) *ip = host;
    if (port) *port = p;
    return true;
}

// ---- validation -------------------------------------------------------------------------------------
namespace {
ErrorInfo invalid(const std::string& message) { return {"INVALID_ARGUMENT", message, 2}; }

bool user_ok(const std::string& u) {
    if (u.empty() || u.size() > 256) return false;
    return std::all_of(u.begin(), u.end(), [](unsigned char c) {
        return std::isalnum(c) || c == ' ' || c == '.' || c == '_' || c == '-' || c == '\\' || c == '@' || c == '$';
    });
}
} // namespace

std::optional<ErrorInfo> validate_options(Options& o) {
    const int modes = (o.self_signed ? 1 : 0) + (!o.thumbprint.empty() ? 1 : 0) + (o.no_tls ? 1 : 0);
    if (modes != 1) return invalid("Choose exactly one of --self-signed, --cert-thumbprint THUMBPRINT or --no-tls");
    o.cert_mode = o.no_tls ? CertMode::NoTls : o.self_signed ? CertMode::SelfSigned : CertMode::Thumbprint;
    if (!o.thumbprint.empty()) {
        if (auto e = thumbprint_error(o.thumbprint)) return invalid("--cert-thumbprint: " + *e);
        o.thumbprint = normalize_thumbprint(o.thumbprint);
    }
    if (o.port == 0) o.port = o.no_tls ? kDefaultNoTlsPort : kDefaultTlsPort;
    if (auto e = port_error(o.port)) return invalid("--port: " + *e);
    if (!o.no_tls) {
        if (o.hostname.empty()) return invalid("No host name: pass --hostname");
        o.hostname = lower_ascii(o.hostname);
        if (auto e = hostname_error(o.hostname)) return invalid("--hostname: " + *e);
    } else {
        o.hostname = o.hostname.empty() ? "127.0.0.1" : lower_ascii(o.hostname);
        if (o.open_firewall) return invalid("--open-firewall makes no sense with --no-tls (the listener is loopback only)");
        if (!o.allow_ip.empty()) return invalid("--allow-ip makes no sense with --no-tls (the listener is loopback only)");
    }
    for (const auto& cidr : o.allow_ip)
        if (auto e = cidr_error(cidr)) return invalid("--allow-ip: " + *e);
    if (!o.user.empty() && !user_ok(o.user)) return invalid("--user: expected DOMAIN\\user or a plain account name");
    return std::nullopt;
}

std::optional<ErrorInfo> validate_teardown_options(TeardownOptions& o) {
    if (!o.hostname.empty()) {
        o.hostname = lower_ascii(o.hostname);
        if (auto e = hostname_error(o.hostname)) return invalid("--hostname: " + *e);
    }
    if (o.port != 0)
        if (auto e = port_error(o.port)) return invalid("--port: " + *e);
    return std::nullopt;
}

// ---- manifest ---------------------------------------------------------------------------------------
json Manifest::to_json() const {
    return json{{"schema", schema}, {"mode", mode}, {"hostname", hostname}, {"port", port}, {"prefixes", prefixes},
                {"ipports", ipports}, {"sid", sid}, {"user", user}, {"cert_mode", cert_mode}, {"thumbprint", thumbprint},
                {"cer_path", cer_path}, {"firewall_rule", firewall_rule}, {"firewall_rule_name", firewall_rule_name}, {"appid", appid}, {"created_at", created_at},
                {"updated_at", updated_at}, {"config_created", config_created}, {"config_sha256", config_sha256},
                {"urlacl_sddl", urlacl_sddl}, {"urlacl_created", urlacl_created}};
}

std::optional<Manifest> Manifest::from_json(const json& j) {
    if (!j.is_object() || j.value("schema", 0) != 1) return std::nullopt;
    try {
        Manifest m;
        m.mode = j.value("mode", "");
        m.hostname = j.value("hostname", "");
        m.port = j.value("port", 0);
        m.prefixes = j.value("prefixes", std::vector<std::string>{});
        m.ipports = j.value("ipports", std::vector<std::string>{});
        m.sid = j.value("sid", "");
        m.user = j.value("user", "");
        m.cert_mode = j.value("cert_mode", "");
        m.thumbprint = j.value("thumbprint", "");
        m.cer_path = j.value("cer_path", "");
        m.firewall_rule = j.value("firewall_rule", "");
        m.firewall_rule_name = j.value("firewall_rule_name", "");
        m.appid = j.value("appid", "");
        m.created_at = j.value("created_at", "");
        m.updated_at = j.value("updated_at", "");
        m.config_created = j.value("config_created", false);
        m.config_sha256 = j.value("config_sha256", "");
        m.urlacl_sddl = j.value("urlacl_sddl", "");
        m.urlacl_created = j.value("urlacl_created", false);
        if ((m.mode != "tls" && m.mode != "no-tls") || m.port < 1 || m.port > 65535) return std::nullopt;
        return m;
    } catch (...) {
        return std::nullopt;
    }
}

bool Manifest::same_setup(const Manifest& o) const {
    return mode == o.mode && hostname == o.hostname && port == o.port && prefixes == o.prefixes && ipports == o.ipports &&
           sid == o.sid && user == o.user && cert_mode == o.cert_mode && thumbprint == o.thumbprint && cer_path == o.cer_path &&
           firewall_rule == o.firewall_rule && firewall_rule_name == o.firewall_rule_name && appid == o.appid && config_created == o.config_created &&
           config_sha256 == o.config_sha256 && urlacl_sddl == o.urlacl_sddl && urlacl_created == o.urlacl_created;
}

const StepItem* Plan::step(const std::string& id) const {
    for (const auto& s : steps)
        if (s.id == id) return &s;
    return nullptr;
}
StepItem* Plan::step(const std::string& id) {
    for (auto& s : steps)
        if (s.id == id) return &s;
    return nullptr;
}

// ---- config text -------------------------------------------------------------------------------------
namespace {
std::string list_text(const std::vector<std::string>& v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + v[i];
    return out + "]";
}
} // namespace

std::vector<std::pair<std::string, std::string>> desired_config(bool tls, const std::string& hostname, int port,
                                                                const std::vector<std::string>& allow_ip) {
    std::vector<std::pair<std::string, std::string>> out;
    out.emplace_back("server.tls", tls ? "true" : "false");
    out.emplace_back("server.host", tls ? "+" : "127.0.0.1");
    out.emplace_back("server.port", std::to_string(port));
    if (tls) out.emplace_back("server.allowed_hosts", list_text({hostname}));
    if (!allow_ip.empty()) out.emplace_back("server.allow_ip", list_text(allow_ip));
    return out;
}

std::string render_config_yaml(bool tls, const std::string& hostname, int port, const std::vector<std::string>& allow_ip) {
    std::ostringstream out;
    out << "# fairyfly MCP server configuration, written by 'fairyfly mcp setup'.\n"
           "# Everything not listed here uses the defaults; see 'fairyfly mcp config show' and 'fairyfly mcp config init'.\n"
           "# No secrets belong in this file: create bearer tokens with 'fairyfly mcp token create'.\n"
           "# The server itself is started explicitly with 'fairyfly mcp --http'; these keys only apply to HTTP.\n"
           "server:\n";
    if (tls) {
        out << "  # TLS is terminated by http.sys with the certificate bound by 'fairyfly mcp setup'.\n"
               "  tls: true\n"
               "  # '+' = the http.sys strong wildcard (every address of this machine)\n"
               "  host: '+'\n"
               "  port: " << port << "\n"
               "  # Host header values clients use to reach this server\n"
               "  allowed_hosts: " << list_text({hostname}) << "\n";
        if (!allow_ip.empty())
            out << "  # Only these client addresses/CIDR blocks may connect (loopback is always allowed)\n"
                   "  allow_ip: " << list_text(allow_ip) << "\n";
    } else {
        out << "  # Development mode: plain HTTP on the loopback interface only.\n"
               "  tls: false\n"
               "  host: 127.0.0.1\n"
               "  port: " << port << "\n";
    }
    return out.str();
}

// ---- trust hints and runbooks ---------------------------------------------------------------------------
std::string trust_hints(const std::string& cer_path, const std::string& hostname) {
    const std::string pem = "fairyfly-mcp-" + hostname + ".pem";
    std::ostringstream out;
    out << "Windows client (this file is DER): certutil -addstore -user Root \"" << cer_path << "\"\n"
        << "  (machine-wide, elevated: certutil -addstore -f Root \"" << cer_path << "\")\n"
        << "Linux / curl: openssl x509 -inform der -in fairyfly-mcp-" << hostname << ".cer -out " << pem
        << "; curl --cacert " << pem << " https://" << hostname << ":PORT/mcp\n"
        << "Linux / Node (Claude Code, mcp-remote): export NODE_EXTRA_CA_CERTS=/path/" << pem
        << "  (or 'fairyfly mcp cert export --format pem')\n"
        << "macOS: sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain " << pem << "\n"
        << "Claude Desktop (via mcp-remote): add \"env\": {\"NODE_EXTRA_CA_CERTS\": \"<path to " << pem
        << ">\"} to the server entry of claude_desktop_config.json, then restart Claude Desktop";
    return out.str();
}

std::string make_runbook(const Options& o, const RunbookContext& c) {
    std::ostringstream out;
    const bool tls = o.cert_mode != CertMode::NoTls;
    const std::string prefix = url_prefix(tls, o.hostname, o.port);
    const std::string thumb = c.thumbprint.empty() ? "<THUMBPRINT>" : c.thumbprint;
    out << "# Manual equivalent of 'fairyfly mcp setup'. Run the numbered commands in an ELEVATED PowerShell.\n";
    int n = 1;
    if (tls && o.cert_mode == CertMode::SelfSigned) {
        out << "\n# " << n++ << ". Self-signed certificate (2 years, SAN = host name); note the thumbprint it prints\n"
            << "$c = New-SelfSignedCertificate -Subject 'CN=" << o.hostname << "' -DnsName '" << o.hostname
            << "' -CertStoreLocation Cert:\\LocalMachine\\My -FriendlyName 'fairyfly-mcp " << o.hostname
            << "' -NotAfter (Get-Date).AddDays(730) -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256"
               " -KeyUsage DigitalSignature,KeyEncipherment -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.1')\n"
            << "$c.Thumbprint\n";
    } else if (tls) {
        out << "\n# " << n++ << ". Certificate: use the existing one (must be in LocalMachine\\My with its private key)\n"
            << "Get-Item Cert:\\LocalMachine\\My\\" << thumb << " | Format-List Subject,NotAfter,HasPrivateKey\n";
    }
    out << "\n# " << n++ << ". URL reservation so the server can run WITHOUT elevation\n"
        << "netsh http add urlacl url=" << prefix << " user=" << (c.user.empty() ? "DOMAIN\\user" : c.user) << "\n";
    if (tls) {
        out << "\n# " << n++ << ". TLS binding (IPv4 and IPv6)\n";
        for (const auto& ipport : ssl_ipports(o.port))
            out << "netsh http add sslcert ipport=" << ipport << " certhash=" << thumb << " appid=" << kAppId
                << " certstorename=" << kCertStore << "\n";
        out << "\n# " << n++ << ". Optional: firewall rule (--open-firewall)\n"
            << "New-NetFirewallRule -DisplayName '" << firewall_rule_name(o.port)
            << "' -Direction Inbound -Action Allow -Protocol TCP -LocalPort " << o.port << " -Profile Any\n";
        out << "\n# " << n++ << ". Export the public certificate for the clients\n"
            << "Export-Certificate -Cert Cert:\\LocalMachine\\My\\" << thumb << " -FilePath '"
            << (c.cer_path.empty() ? cer_file_name(o.hostname) : c.cer_path) << "' -Type CERT\n";
    }
    out << "\n# " << n++ << ". mcp.yaml (unelevated; " << (o.config_path.empty() ? "%LOCALAPPDATA%\\fairyfly\\mcp.yaml" : o.config_path) << ")\n";
    for (const auto& [key, value] : desired_config(tls, o.hostname, o.port, o.allow_ip)) out << "#   " << key << ": " << value << "\n";
    out << "\n# Then: fairyfly mcp token create NAME ; fairyfly mcp --http\n";
    if (tls) out << "# Clients must trust the certificate:\n" << [&] {
        std::string hints = trust_hints(c.cer_path.empty() ? cer_file_name(o.hostname) : c.cer_path, o.hostname);
        std::string prefixed;
        std::istringstream in(hints);
        for (std::string line; std::getline(in, line);) prefixed += "#   " + line + "\n";
        return prefixed;
    }();
    return out.str();
}

std::string make_teardown_runbook(const TeardownOptions& o, const Manifest* m) {
    std::ostringstream out;
    const int port = m ? m->port : (o.port ? o.port : kDefaultTlsPort);
    out << "# Manual equivalent of 'fairyfly mcp teardown' (elevated PowerShell)\n";
    for (const auto& ipport : ssl_ipports(port)) out << "netsh http delete sslcert ipport=" << ipport << "\n";
    out << "netsh http delete urlacl url=" << url_prefix(true, "", port) << "\n"
        << "netsh http delete urlacl url=" << url_prefix(false, "", port) << "\n"
        << "Remove-NetFirewallRule -DisplayName '" << firewall_rule_name(port) << "'\n";
    if (m && m->cert_mode == "self-signed" && !m->thumbprint.empty() && !o.keep_cert)
        out << "Remove-Item Cert:\\LocalMachine\\My\\" << m->thumbprint << " -DeleteKey\n";
    return out.str();
}

// ---- MakePlan ------------------------------------------------------------------------------------------
namespace {

StepItem make_step(std::string id, std::string title, std::string status, std::string detail, bool elevated = false) {
    return StepItem{std::move(id), std::move(title), std::move(status), std::move(detail), elevated};
}

bool changes(const std::string& status) { return status == "would_create" || status == "would_update" || status == "would_remove"; }

std::string days_left_text(const CertInfo& c, long long now) {
    const long long days = (c.not_after - now) / 86400;
    return "expires " + iso_date(c.not_after) + " (" + std::to_string(days) + " days)";
}

std::string binding_state_detail(const std::string& ipport, const std::optional<SslBinding>& b) {
    if (!b) return ipport + ": not bound";
    return ipport + ": bound to " + b->thumbprint.substr(0, 8) + "...";
}

std::string elevation_text(ElevationType type) {
    switch (type) {
    case ElevationType::Elevated: return "elevated (no prompt needed)";
    case ElevationType::AdminFiltered: return "administrator, not elevated (one UAC prompt for the machine changes)";
    case ElevationType::StandardUser: return "standard user (one UAC prompt asks for administrator credentials)";
    default: return "unknown";
    }
}

} // namespace

Plan MakePlan(const Diagnosis& d, const Options& o) {
    Plan p;
    p.operation = "setup";
    p.tls = d.tls;
    p.hostname = d.hostname;
    p.port = d.port;
    p.prefix = d.prefix;
    p.ipports = d.ipports;
    p.sid = d.sid;
    p.user = d.user;
    p.elevation = d.elevation;
    p.url = endpoint_url(d.tls, d.hostname, d.port);
    p.cert_mode = o.cert_mode;
    p.force_binding = o.force_binding;
    p.open_firewall = o.open_firewall;
    p.cer_path = d.cer_path;
    p.manifest_path = d.manifest_path;
    p.config_path = d.config_path;
    p.firewall_display = firewall_rule_name(d.port);
    p.firewall_rule = d.firewall_name;

    if (!d.identity_error.empty()) {
        p.error = ErrorInfo{"USER_NOT_FOUND", d.identity_error, 2};
        return p;
    }

    // ---- diagnosis items ----
    p.diagnosis.push_back({"elevation", "info", elevation_text(d.elevation)});
    if (d.manifest_unreadable) p.diagnosis.push_back({"setup_manifest", "warn", "unreadable: " + d.manifest_path});
    else if (d.manifest) p.diagnosis.push_back({"setup_manifest", "ok", "recorded setup for " + d.manifest->hostname + ":" + std::to_string(d.manifest->port)});
    else p.diagnosis.push_back({"setup_manifest", "missing", "no setup recorded (" + d.manifest_path + ")"});

    // ---- certificate ----
    const bool tls = d.tls;
    std::string cert_thumb;   // thumbprint to bind when known
    StepItem cert_step;
    if (!tls) {
        cert_step = make_step("certificate", "TLS certificate", "skipped", "--no-tls: plain HTTP on the loopback interface");
    } else if (o.cert_mode == CertMode::SelfSigned) {
        if (!d.cert.found) {
            cert_step = make_step("certificate", "Create self-signed certificate CN=" + d.hostname + " (" + std::to_string(kSelfSignedDays) + " days)",
                                  "would_create", "no certificate 'fairyfly-mcp " + d.hostname + "' in LocalMachine\\My", true);
            p.create_cert = true;
            p.diagnosis.push_back({"certificate", "missing", "no certificate 'fairyfly-mcp " + d.hostname + "' in LocalMachine\\My"});
        } else {
            const auto problems = cert_problems(d.cert, d.hostname, d.now);
            if (problems.empty()) {
                cert_thumb = d.cert.thumbprint;
                cert_step = make_step("certificate", "Self-signed certificate CN=" + d.hostname, "unchanged",
                                      d.cert.thumbprint + ", " + days_left_text(d.cert, d.now));
                p.diagnosis.push_back({"certificate", "ok", d.cert.thumbprint + ", " + days_left_text(d.cert, d.now)});
            } else {
                cert_step = make_step("certificate", "Replace the self-signed certificate CN=" + d.hostname, "would_update",
                                      "existing certificate " + d.cert.thumbprint + " is unusable: " + cert_problem_text(problems), true);
                p.create_cert = true;
                p.diagnosis.push_back({"certificate", "mismatch", d.cert.thumbprint + ": " + cert_problem_text(problems)});
            }
        }
    } else {   // thumbprint mode
        if (!d.cert.found) {
            cert_step = make_step("certificate", "Use certificate " + o.thumbprint, "blocked", "not found in LocalMachine\\My");
            p.diagnosis.push_back({"certificate", "missing", o.thumbprint + " is not in LocalMachine\\My"});
        } else {
            const auto problems = cert_problems(d.cert, d.hostname, d.now);
            if (!problems.empty()) {
                cert_step = make_step("certificate", "Use certificate " + o.thumbprint, "blocked", cert_problem_text(problems));
                p.diagnosis.push_back({"certificate", "mismatch", o.thumbprint + ": " + cert_problem_text(problems)});
            } else {
                cert_thumb = d.cert.thumbprint;
                cert_step = make_step("certificate", "Use certificate " + o.thumbprint, "unchanged",
                                      d.cert.subject + ", " + days_left_text(d.cert, d.now));
                p.diagnosis.push_back({"certificate", "ok", d.cert.thumbprint + ", " + days_left_text(d.cert, d.now)});
            }
        }
    }
    p.thumbprint = cert_thumb;
    p.steps.push_back(cert_step);
    const bool cert_will_be_new = p.create_cert;

    // ---- urlacl ----
    {
        StepItem s = make_step("urlacl", "URL reservation " + d.prefix + " for " + d.user, "", "", true);
        if (!tls) {
            // Loopback prefixes need no reservation on this Windows build (verified by a bind probe); never block on it.
            s.elevated = false;
            if (d.urlacl_sddl) {
                s.status = "unchanged";
                s.detail = "a reservation exists (optional for loopback prefixes)";
                p.diagnosis.push_back({"urlacl", "ok", d.prefix + " is reserved (optional for loopback)"});
            } else {
                s.status = "skipped";
                s.detail = "not required for loopback prefixes on this Windows build (verified by a bind probe)";
                p.diagnosis.push_back({"urlacl", "skip", "not required for loopback prefixes"});
            }
        } else if (!d.urlacl_sddl) {
            s.status = "would_create";
            s.detail = "no reservation; SDDL " + sddl_for_sid(d.sid);
            p.sddl = sddl_for_sid(d.sid);
            p.diagnosis.push_back({"urlacl", "missing", d.prefix + " is not reserved"});
        } else if (sddl_covers_sid(*d.urlacl_sddl, d.sid)) {
            s.status = "unchanged";
            s.elevated = false;
            s.detail = "already reserved for " + d.user;
            p.diagnosis.push_back({"urlacl", "ok", d.prefix + " is reserved for " + d.user});
        } else {
            s.status = "would_update";
            s.detail = "reserved without " + d.user + "; the SID is added to the existing entries";
            p.sddl = merge_sddl(*d.urlacl_sddl, d.sid);
            p.replace_urlacl = true;
            p.diagnosis.push_back({"urlacl", "mismatch", d.prefix + " is reserved, but not for " + d.user});
        }
        p.steps.push_back(s);
    }

    // ---- sslcert ----
    if (!tls) {
        p.steps.push_back(make_step("sslcert", "TLS binding", "skipped", "--no-tls: nothing to bind"));
        p.diagnosis.push_back({"sslcert", "skip", "--no-tls"});
    } else {
        StepItem s = make_step("sslcert", "TLS binding to certificate for " + std::string("0.0.0.0/[::]") + ":" + std::to_string(d.port), "unchanged", "", true);
        bool any_blocked = false, any_create = false, any_update = false;
        std::vector<std::string> skipped_ipports;
        std::string detail;
        std::string diag;
        std::string diag_status = "ok";
        for (const auto& ipport : d.ipports) {
            const auto it = d.ssl.find(ipport);
            const std::optional<SslBinding> b = it == d.ssl.end() ? std::nullopt : it->second;
            std::string line;
            // An address family the previous setup could not bind (IPv6 disabled) stays skipped instead of failing every run.
            const bool skipped_before = d.manifest && d.manifest->port == d.port && d.manifest->hostname == d.hostname &&
                                        std::find(d.manifest->ipports.begin(), d.manifest->ipports.end(), ipport) == d.manifest->ipports.end();
            if (!b && skipped_before) {
                line = ipport + ": unavailable on this machine (skipped by the previous setup)";
                skipped_ipports.push_back(ipport);
            } else if (!b) {
                any_create = true;
                line = ipport + ": not bound -> bind";
                diag_status = "missing";
            } else if (!is_our_app_id(b->app_id)) {
                if (o.force_binding) {
                    any_update = true;
                    line = ipport + ": bound by another application " + b->app_id + " -> replaced (--force-binding)";
                } else {
                    any_blocked = true;
                    line = ipport + ": bound by another application " + b->app_id + " (needs --force-binding)";
                }
                diag_status = "mismatch";
            } else if (!cert_will_be_new && !cert_thumb.empty() && b->thumbprint == cert_thumb) {
                line = ipport + ": bound to the certificate";
            } else {
                any_update = true;
                line = ipport + ": bound to " + b->thumbprint.substr(0, 8) + "... -> rebind";
                if (diag_status == "ok") diag_status = "mismatch";
            }
            detail += (detail.empty() ? "" : "; ") + line;
            diag += (diag.empty() ? "" : "; ") + binding_state_detail(ipport, b);
        }
        if (any_blocked) s.status = "blocked";
        else if (any_create) s.status = "would_create";
        else if (any_update) s.status = "would_update";
        if (s.status == "unchanged") s.elevated = false;
        s.detail = detail;
        p.steps.push_back(s);
        p.diagnosis.push_back({"sslcert", diag_status, diag});
    }

    // ---- firewall ----
    if (!tls) {
        p.steps.push_back(make_step("firewall", "Firewall rule", "skipped", "--no-tls: loopback only"));
    } else if (!o.open_firewall) {
        p.steps.push_back(make_step("firewall", "Firewall rule " + p.firewall_display, "skipped", "--open-firewall not given"));
        p.diagnosis.push_back({"firewall", "skip", "not requested"});
    } else if (d.firewall_exists) {
        p.steps.push_back(make_step("firewall", "Firewall rule " + p.firewall_display, "unchanged", "rule " + p.firewall_rule + " exists"));
        p.diagnosis.push_back({"firewall", "ok", "rule '" + p.firewall_display + "' (" + p.firewall_rule + ") exists"});
    } else if (d.firewall_display_foreign) {
        // Not ours: never claimed, never duplicated, never removed by teardown.
        p.steps.push_back(make_step("firewall", "Firewall rule " + p.firewall_display, "skipped", "a rule with this name already exists and was not created by fairyfly; left untouched"));
        p.diagnosis.push_back({"firewall", "info", "a rule named '" + p.firewall_display + "' exists and was not created by fairyfly setup"});
        p.human.push_back({"firewall_foreign", "A firewall rule named '" + p.firewall_display + "' already exists and was not created by 'mcp setup', so it was neither changed nor duplicated. Check that it allows inbound TCP " +
                                                   std::to_string(d.port) + ", or remove/rename it and run setup with --open-firewall again."});
    } else {
        p.steps.push_back(make_step("firewall", "Firewall rule " + p.firewall_display, "would_create", "inbound TCP " + std::to_string(d.port) + " (rule name " + p.firewall_rule + ")", true));
        p.diagnosis.push_back({"firewall", "missing", "rule '" + p.firewall_display + "' does not exist"});
    }

    // ---- certificate_export ----
    if (!tls) {
        p.steps.push_back(make_step("certificate_export", "Export the public certificate", "skipped", "--no-tls"));
    } else if (cert_will_be_new || (cert_thumb.empty())) {
        // the certificate does not exist yet (or is blocked)
        p.steps.push_back(make_step("certificate_export", "Export the public certificate to " + d.cer_path,
                                    cert_step.status == "blocked" ? "skipped" : (d.cer == CerState::Missing ? "would_create" : "would_update"),
                                    cert_step.status == "blocked" ? "no usable certificate" : "after the certificate exists"));
    } else if (d.cer == CerState::Matches) {
        p.steps.push_back(make_step("certificate_export", "Export the public certificate to " + d.cer_path, "unchanged", "file matches the certificate"));
    } else {
        p.steps.push_back(make_step("certificate_export", "Export the public certificate to " + d.cer_path,
                                    d.cer == CerState::Missing ? "would_create" : "would_update",
                                    d.cer == CerState::Missing ? "not exported yet" : "file holds a different certificate"));
    }

    // ---- config ----
    {
        const auto want = desired_config(tls, d.hostname, d.port, o.allow_ip);
        std::vector<std::string> differing;
        for (const auto& [key, value] : want) {
            const auto it = d.config_current.find(key);
            if (it == d.config_current.end() || it->second != value) differing.push_back(key + ": " + value);
        }
        p.config_yaml = render_config_yaml(tls, d.hostname, d.port, o.allow_ip);
        if (d.config_unreadable) {
            p.steps.push_back(make_step("config", "mcp.yaml", "blocked", "cannot read " + d.config_path));
        } else if (!d.config_exists) {
            p.steps.push_back(make_step("config", "Write " + d.config_path, "would_create", "commented mcp.yaml with server.tls/host/port"));
        } else if (differing.empty()) {
            p.steps.push_back(make_step("config", "mcp.yaml", "unchanged", d.config_path + " already has the settings"));
        } else {
            p.steps.push_back(make_step("config", "mcp.yaml", "skipped", d.config_path + " exists and differs; it is never rewritten"));
            std::string keys;
            for (const auto& k : differing) keys += "\n    " + k;
            p.human.push_back({"config_keys", "Edit " + d.config_path + " and set:" + keys});
        }
    }

    // ---- manifest ----
    {
        Manifest m;
        m.mode = tls ? "tls" : "no-tls";
        m.hostname = d.hostname;
        m.port = d.port;
        m.prefixes = {d.prefix};
        m.ipports = d.ipports;
        if (tls && d.manifest && d.manifest->port == d.port && d.manifest->hostname == d.hostname) {
            m.ipports.clear();   // keep only the address families that were actually bound
            for (const auto& ip : d.ipports)
                if (std::find(d.manifest->ipports.begin(), d.manifest->ipports.end(), ip) != d.manifest->ipports.end() ||
                    (d.ssl.count(ip) && d.ssl.at(ip) && is_our_app_id(d.ssl.at(ip)->app_id)))
                    m.ipports.push_back(ip);
            if (m.ipports.empty()) m.ipports = d.ipports;
        }
        m.sid = d.sid;
        m.user = d.user;
        m.cert_mode = !tls ? "none" : (o.cert_mode == CertMode::SelfSigned ? "self-signed" : "thumbprint");
        m.thumbprint = cert_thumb;
        m.cer_path = tls ? d.cer_path : "";
        {
            const bool ours = tls && o.open_firewall && !d.firewall_display_foreign && !p.firewall_rule.empty();
            m.firewall_rule = ours ? p.firewall_display : "";
            m.firewall_rule_name = ours ? p.firewall_rule : "";
        }
        m.appid = tls ? normalize_app_id(kAppId) : "";
        if (const StepItem* cs = p.step("config"); cs && cs->status == "would_create") {
            m.config_created = true;
            m.config_sha256 = auth::sha256_hex(p.config_yaml);   // replaced by the hash of the bytes on disk after the write
        }
        // URL reservation ownership: teardown removes the reservation only when setup created it and it is unchanged.
        if (tls) {
            const StepItem* us = p.step("urlacl");
            const bool same_prefix = d.manifest && std::find(d.manifest->prefixes.begin(), d.manifest->prefixes.end(), d.prefix) != d.manifest->prefixes.end();
            if (us && us->status == "would_create") {
                m.urlacl_created = true;
                m.urlacl_sddl = p.sddl;   // replaced by what http.sys reports after the change
            } else if (us && us->status == "would_update") {
                m.urlacl_created = same_prefix && d.manifest->urlacl_created && d.urlacl_sddl && *d.urlacl_sddl == d.manifest->urlacl_sddl;
                m.urlacl_sddl = p.sddl;
            } else if (same_prefix && !d.manifest->urlacl_sddl.empty()) {
                m.urlacl_created = d.manifest->urlacl_created;   // an earlier record stays as it is; teardown compares it with the live SDDL
                m.urlacl_sddl = d.manifest->urlacl_sddl;
            } else {
                m.urlacl_created = false;   // an existing reservation that setup found: never removed by teardown
                m.urlacl_sddl = d.urlacl_sddl.value_or("");
            }
        }
        p.manifest = m;
        if (!d.manifest) {
            p.steps.push_back(make_step("manifest", "Record the setup in " + d.manifest_path, "would_create", "used by teardown and doctor"));
        } else {
            Manifest cur = *d.manifest;
            // A firewall rule created by an earlier run stays recorded even when this run did not ask for it.
            if (m.firewall_rule.empty()) {
                p.manifest.firewall_rule = cur.firewall_rule;
                p.manifest.firewall_rule_name = cur.firewall_rule_name;
            }
            p.manifest.created_at = cur.created_at;
            // The config file recorded by an earlier run stays recorded unless this run creates a fresh one.
            if (!p.manifest.config_created) {
                p.manifest.config_created = cur.config_created;
                p.manifest.config_sha256 = cur.config_sha256;
            }
            if (cert_thumb.empty() && !cert_will_be_new) p.manifest.thumbprint = cur.thumbprint;
            const bool same = !cert_will_be_new && cur.same_setup(p.manifest);
            p.steps.push_back(make_step("manifest", "Record the setup in " + d.manifest_path, same ? "unchanged" : "would_update",
                                        same ? "up to date" : "differs from the recorded setup"));
        }
        if (d.manifest && (d.manifest->hostname != d.hostname || d.manifest->port != d.port))
            p.human.push_back({"previous_setup", "A previous setup for " + d.manifest->hostname + ":" + std::to_string(d.manifest->port) +
                                                     " is recorded; it is replaced in the manifest but its reservations stay. Run 'fairyfly mcp teardown' first to remove it cleanly."});
    }

    // ---- port ----
    p.diagnosis.push_back({"port", d.port_listening ? "info" : "ok",
                           d.port_listening ? "port " + std::to_string(d.port) + " is already served (tray or server running)" : "port " + std::to_string(d.port) + " is free"});

    // ---- summary flags ----
    bool nothing = true;
    for (const auto& s : p.steps) {
        if (s.status == "blocked") p.blocked = true;
        if (changes(s.status)) {
            nothing = false;
            if (s.elevated) p.needs_elevation = true;
        }
    }
    p.nothing = nothing && !p.blocked;

    // ---- Left for a human ----
    if (tls) {
        if (o.cert_mode == CertMode::SelfSigned)
            p.human.push_back({"trust_certificate", "Trust the exported certificate " + d.cer_path + " on every client:\n" + [&] {
                                   std::string hints = trust_hints(d.cer_path, d.hostname);
                                   std::string out, line;
                                   std::istringstream in(hints);
                                   while (std::getline(in, line)) out += "    " + line + "\n";
                                   if (!out.empty()) out.pop_back();
                                   return out;
                               }()});
        p.human.push_back({"dns", "Make '" + d.hostname + "' resolve to this machine on every client (DNS record, or a hosts file entry)."});
        if (!o.open_firewall)
            p.human.push_back({"firewall", "Open inbound TCP " + std::to_string(d.port) + " in the firewall for the clients, e.g.: netsh advfirewall firewall add rule name=\"" +
                                               p.firewall_display + "\" dir=in action=allow protocol=TCP localport=" + std::to_string(d.port) + " (or re-run setup with --open-firewall)."});
    }
    if (d.tls_probe.status == "ok" && !d.tls_probe.protocol.empty() && (d.tls_probe.protocol == "TLS 1.0" || d.tls_probe.protocol == "TLS 1.1" || d.tls_probe.protocol.rfind("SSL", 0) == 0))
        p.human.push_back({"schannel", "The server negotiated " + d.tls_probe.protocol + ": raise the machine's Schannel policy to TLS 1.2 or newer (registry SCHANNEL\\Protocols)."});

    // ---- next steps ----
    p.next_steps.push_back("fairyfly mcp token create NAME   (bearer token for a client; shown once)");
    p.next_steps.push_back(tls ? "fairyfly mcp --http   (or --tray --http) serves " + p.url : "fairyfly mcp --http   serves " + p.url);
    p.next_steps.push_back("fairyfly mcp doctor");
    return p;
}

// ---- MakeTeardownPlan -----------------------------------------------------------------------------------
Plan MakeTeardownPlan(const TeardownDiagnosis& d, const TeardownOptions& o) {
    Plan p;
    p.operation = "teardown";
    p.hostname = d.hostname;
    p.port = d.port;
    p.elevation = d.elevation;
    p.manifest_path = d.manifest_path;
    p.cer_path = d.cer_path;
    const bool have_manifest = d.manifest.has_value();
    p.tls = !have_manifest || d.manifest->mode == "tls";
    p.url = have_manifest ? endpoint_url(p.tls, d.manifest->hostname, d.manifest->port) : std::string();
    p.diagnosis.push_back({"elevation", "info", elevation_text(d.elevation)});
    p.diagnosis.push_back({"setup_manifest", have_manifest ? "ok" : "missing",
                           have_manifest ? "recorded setup for " + d.manifest->hostname + ":" + std::to_string(d.manifest->port) : "no manifest at " + d.manifest_path});

    const bool anything_found = !d.urlacls.empty() || !d.ssl_ours.empty() || d.firewall_exists;
    if (!have_manifest && !d.explicit_target) {
        if (anything_found) {
            p.error = ErrorInfo{"MANIFEST_MISSING", "No setup manifest at " + d.manifest_path + ", but fairyfly reservations exist. Pass --hostname and --port to remove them.", 2};
            return p;
        }
        p.steps.push_back(make_step("sslcert", "TLS binding", "unchanged", "not bound"));
        p.steps.push_back(make_step("urlacl", "URL reservation", "unchanged", "not reserved"));
        p.steps.push_back(make_step("firewall", "Firewall rule", "unchanged", "no rule"));
        p.steps.push_back(make_step("certificate", "Certificate", "unchanged", "nothing recorded"));
        p.steps.push_back(make_step("certificate_export", "Exported certificate", "unchanged", "no file"));
        p.steps.push_back(make_step("config", "mcp.yaml", "unchanged", "no manifest"));
        p.steps.push_back(make_step("manifest", "Setup manifest", "unchanged", "no manifest"));
        p.nothing = true;
        return p;
    }

    // sslcert
    for (const auto& ip : d.ssl_ours) {
        p.teardown.ipports.push_back(ip);
    }
    if (!d.ssl_ours.empty()) {
        std::string detail;
        for (const auto& ip : d.ssl_ours) detail += (detail.empty() ? "" : ", ") + ip;
        p.steps.push_back(make_step("sslcert", "Remove TLS binding(s)", "would_remove", detail, true));
    } else if (!d.ssl_foreign.empty()) {
        std::string detail;
        for (const auto& ip : d.ssl_foreign) detail += (detail.empty() ? "" : ", ") + ip;
        p.steps.push_back(make_step("sslcert", "TLS binding", "skipped", detail + " belongs to another application; left untouched"));
    } else {
        p.steps.push_back(make_step("sslcert", "TLS binding", "unchanged", "not bound"));
    }
    // urlacl: only a reservation that setup created and that still has the SDDL setup recorded
    {
        std::vector<std::string> foreign;
        for (const auto& u : d.urlacls) {
            bool ours = have_manifest && d.manifest->urlacl_created && !d.manifest->urlacl_sddl.empty() &&
                        std::find(d.manifest->prefixes.begin(), d.manifest->prefixes.end(), u) != d.manifest->prefixes.end();
            if (ours) {
                const auto cur = d.urlacl_sddl.find(u);
                if (cur != d.urlacl_sddl.end() && cur->second != d.manifest->urlacl_sddl) ours = false;   // changed since setup
            }
            if (ours) {
                p.teardown.prefixes.push_back(u);
                p.teardown.urlacl_expected[u] = d.manifest->urlacl_sddl;
            } else {
                foreign.push_back(u);
            }
        }
        std::string removed_detail, foreign_detail;
        for (const auto& u : p.teardown.prefixes) removed_detail += (removed_detail.empty() ? "" : ", ") + u;
        for (const auto& u : foreign) foreign_detail += (foreign_detail.empty() ? "" : ", ") + u;
        if (!p.teardown.prefixes.empty())
            p.steps.push_back(make_step("urlacl", "Remove URL reservation(s)", "would_remove", removed_detail, true));
        else if (!foreign.empty())
            p.steps.push_back(make_step("urlacl", "URL reservation", "skipped", foreign_detail + ": not created by setup or changed since; left untouched"));
        else
            p.steps.push_back(make_step("urlacl", "URL reservation", "unchanged", "not reserved"));
        if (!foreign.empty()) {
            std::string text = "Not removed: the URL reservation(s) below were not created by 'mcp setup' or were changed since. Remove them yourself if you no longer need them:";
            for (const auto& u : foreign) text += "\n    netsh http delete urlacl url=" + u;
            p.human.push_back({"urlacl_foreign", text});
        }
    }
    // firewall: only the rule whose unique internal Name the manifest records (never by display name)
    if (o.keep_firewall) {
        p.steps.push_back(make_step("firewall", "Firewall rule", "skipped", "--keep-firewall"));
    } else if (!have_manifest) {
        p.steps.push_back(make_step("firewall", "Firewall rule", "skipped", "no manifest: nothing shows that a rule was created by setup"));
    } else if (!d.firewall_rule.empty() && d.firewall_exists) {
        p.teardown.firewall_rule = d.firewall_rule;
        p.steps.push_back(make_step("firewall", "Remove firewall rule " + d.firewall_rule, "would_remove", "inbound TCP " + std::to_string(d.port), true));
    } else if (!d.firewall_rule.empty()) {
        p.steps.push_back(make_step("firewall", "Firewall rule", "unchanged", "rule " + d.firewall_rule + " is already gone"));
    } else if (!d.firewall_legacy_display.empty()) {
        p.steps.push_back(make_step("firewall", "Firewall rule", "skipped", "recorded by display name only (older setup); cannot tell it is the one setup created"));
        p.human.push_back({"firewall_legacy", "The manifest records a firewall rule by display name only ('" + d.firewall_legacy_display +
                                                   "'). It was not removed because that name could belong to somebody else's rule. If it is yours: Remove-NetFirewallRule -DisplayName '" + d.firewall_legacy_display + "'"});
    } else {
        p.steps.push_back(make_step("firewall", "Firewall rule", "skipped", "setup did not create one"));
    }
    // certificate
    if (o.keep_cert) {
        p.steps.push_back(make_step("certificate", "Certificate", "skipped", "--keep-cert"));
    } else if (!have_manifest) {
        p.steps.push_back(make_step("certificate", "Certificate", "skipped", "unknown without a manifest; remove it yourself if you created it"));
    } else if (d.manifest->cert_mode != "self-signed") {
        p.steps.push_back(make_step("certificate", "Certificate", "skipped",
                                    d.manifest->cert_mode == "thumbprint" ? "user-supplied certificate is never removed" : "none was set up"));
    } else if (!d.cert.found) {
        p.steps.push_back(make_step("certificate", "Certificate", "unchanged", "already gone"));
    } else if (d.cert.friendly_name != "fairyfly-mcp " + d.manifest->hostname) {
        p.steps.push_back(make_step("certificate", "Certificate", "skipped", d.cert.thumbprint + " does not carry the fairyfly friendly name; left untouched"));
    } else {
        p.teardown.cert_thumbprint = d.cert.thumbprint;
        p.steps.push_back(make_step("certificate", "Remove self-signed certificate and its key", "would_remove", d.cert.thumbprint, true));
    }
    // certificate_export: only the file at the path derived from the validated host name, holding the recorded certificate
    if (!d.cer_refusal.empty()) {
        p.steps.push_back(make_step("certificate_export", "Exported certificate", "skipped", d.cer_refusal + "; not deleted"));
        p.human.push_back({"cer_path_untrusted", "Not deleted: " + d.cer_refusal + ". Delete the exported certificate yourself if you no longer need it."});
    } else if (d.cer_exists && !d.cer_matches) {
        p.steps.push_back(make_step("certificate_export", "Exported certificate", "skipped", d.cer_path + " does not hold the certificate setup recorded; left untouched"));
        p.human.push_back({"cer_file_foreign", d.cer_path + " does not hold the certificate that setup exported (or no certificate was recorded); it was not deleted. Delete it yourself if you no longer need it."});
    } else if (d.cer_exists) {
        p.teardown.cer_path = d.cer_path;
        p.steps.push_back(make_step("certificate_export", "Delete " + d.cer_path, "would_remove", "exported public certificate"));
    } else {
        p.steps.push_back(make_step("certificate_export", "Exported certificate", "unchanged", "no file"));
    }
    // config: only a file setup created and nobody has edited since
    if (!have_manifest || !d.manifest->config_created) {
        p.steps.push_back(make_step("config", "mcp.yaml", "skipped", "not created by setup; left untouched"));
    } else if (!d.config_exists) {
        p.steps.push_back(make_step("config", "mcp.yaml", "unchanged", "already gone"));
    } else if (!d.manifest->config_sha256.empty() && d.config_sha256 == d.manifest->config_sha256) {
        p.teardown.config_path = d.config_path;
        p.steps.push_back(make_step("config", "Delete " + d.config_path, "would_remove", "mcp.yaml written by setup, unmodified"));
    } else {
        p.steps.push_back(make_step("config", "mcp.yaml", "skipped", d.config_path + " was modified after setup; left in place"));
        p.human.push_back({"config_modified", "Review " + d.config_path + " and delete it yourself if you no longer need it: it was changed after 'mcp setup' created it."});
    }
    // manifest
    if (have_manifest) p.steps.push_back(make_step("manifest", "Delete " + d.manifest_path, "would_remove", "setup manifest"));
    else p.steps.push_back(make_step("manifest", "Setup manifest", "unchanged", "no manifest"));

    bool nothing = true;
    for (const auto& s : p.steps)
        if (changes(s.status)) {
            nothing = false;
            if (s.elevated) p.needs_elevation = true;
        }
    p.nothing = nothing;
    if (d.config_says_tls && p.teardown.config_path.empty())
        p.human.push_back({"config_tls", "Edit " + d.config_path + ": it still says 'tls: true' (and server.host: '+'); the server will not start until you set tls: false or run setup again."});
    if (d.cer_exists || !p.teardown.cert_thumbprint.empty())
        p.human.push_back({"untrust_certificate", "Remove the trusted fairyfly certificate from every client that imported it."});
    return p;
}

// ---- report -----------------------------------------------------------------------------------------------
std::string nothing_message(const std::string& operation) {
    return operation == "teardown" ? "nothing - already removed." : "nothing - already set up.";
}

Report report_from_plan(const Plan& plan, bool dry_run) {
    Report r;
    r.operation = plan.operation;
    r.dry_run = dry_run;
    r.url = plan.url;
    r.elevation = elevation_name(plan.elevation);
    r.diagnosis = plan.diagnosis;
    r.steps = plan.steps;
    r.human = plan.human;
    r.next_steps = plan.operation == "setup" ? plan.next_steps : std::vector<std::string>{};
    r.error = plan.error;
    r.nothing = plan.nothing;
    r.hostname = plan.hostname;
    return r;
}

json report_to_json(const Report& r) {
    json out;
    out["status"] = r.error ? "error" : "success";
    if (r.has_data) {
        json data;
        data["operation"] = r.operation;
        data["dry_run"] = r.dry_run;
        data["url"] = r.url;
        data["elevation"] = r.elevation;
        data["nothing_to_do"] = r.nothing;
        data["diagnosis"] = json::array();
        for (const auto& c : r.diagnosis) data["diagnosis"].push_back({{"id", c.id}, {"status", c.status}, {"detail", c.detail}});
        data["steps"] = json::array();
        for (const auto& s : r.steps)
            data["steps"].push_back({{"id", s.id}, {"title", s.title}, {"status", s.status}, {"detail", s.detail}, {"elevated", s.elevated}});
        data["human"] = json::array();
        for (const auto& h : r.human) data["human"].push_back({{"id", h.id}, {"text", h.text}});
        if (r.verify)
            data["verify"] = {{"status", r.verify->status}, {"protocol", r.verify->protocol}, {"http_status", r.verify->http_status},
                              {"thumbprint_match", r.verify->thumbprint_match}, {"detail", r.verify->detail}};
        data["next_steps"] = r.next_steps;
        if (!r.runbook.empty()) data["runbook"] = r.runbook;
        out["data"] = data;
    }
    if (r.error) out["error"] = {{"code", r.error->code}, {"message", r.error->message}};
    return out;
}

namespace {
std::string pad(std::string s, size_t w) {
    if (s.size() < w) s.resize(w, ' ');
    return s;
}

void append_steps(std::ostringstream& out, const std::vector<StepItem>& steps) {
    for (const auto& s : steps) {
        out << "  [" << pad(s.status, 12) << "] " << pad(s.id, 18) << " " << s.title;
        if (s.elevated && (s.status.rfind("would", 0) == 0)) out << "  (admin)";
        out << "\n";
        if (!s.detail.empty()) out << "      " << s.detail << "\n";
    }
}

void append_human(std::ostringstream& out, const std::vector<HumanItem>& human) {
    if (human.empty()) return;
    out << "\nLeft for a human\n";
    int n = 1;
    for (const auto& h : human) out << "  " << n++ << ". " << h.text << "\n";
}
} // namespace

std::string render_plan_text(const Plan& plan, bool dry_run) {
    std::ostringstream out;
    out << "fairyfly mcp " << plan.operation << ": plan" << (plan.url.empty() ? "" : " for " + plan.url) << (dry_run ? " (dry run, nothing is changed)" : "") << "\n";
    out << "elevation: "
        << (!plan.needs_elevation && plan.elevation != ElevationType::Elevated
                ? std::string("not needed for this plan (") + elevation_name(plan.elevation) + ")"
                : elevation_text(plan.elevation))
        << "\n\nDiagnosis\n";
    for (const auto& c : plan.diagnosis) out << "  [" << pad(c.status, 8) << "] " << pad(c.id, 15) << " " << c.detail << "\n";
    out << "\nPlan\n";
    append_steps(out, plan.steps);
    append_human(out, plan.human);
    return out.str();
}

std::string render_result_text(const Report& r) {
    std::ostringstream out;
    out << "\nResult\n";
    append_steps(out, r.steps);
    if (r.verify) {
        out << "\nVerify: " << r.verify->status;
        if (!r.verify->protocol.empty()) out << ", " << r.verify->protocol;
        if (r.verify->http_status) out << ", HTTP " << r.verify->http_status;
        if (r.verify->status == "ok" || r.verify->status == "failed") out << ", thumbprint " << (r.verify->thumbprint_match ? "matches" : "does not match");
        if (!r.verify->detail.empty()) out << " (" << r.verify->detail << ")";
        out << "\n";
    }
    append_human(out, r.human);
    if (!r.next_steps.empty()) {
        out << "\nNext\n";
        for (const auto& n : r.next_steps) out << "  " << n << "\n";
    }
    return out.str();
}

} // namespace fairyfly::setup
