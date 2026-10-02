#pragma once
// In-memory fakes of the setup hosts. Used by tests/unit/test_mcp_setup.cpp; other streams may use them for
// their own tests. The fakes model the elevation requirement: http.sys, certificate and firewall changes throw
// ACCESS_DENIED unless the fake elevator is "elevated" (which it is while an elevated child plan runs).

#include <algorithm>
#include <functional>
#include <map>

#include "include/setup/setup_hosts.h"

namespace fairyfly::setup {

struct FakeElevator : Elevator {
    ElevationType type = ElevationType::StandardUser;
    bool elevated_child_running = false;
    std::string sid = "S-1-5-21-1-2-3-1001";
    std::string name = "CORP\\jr";
    std::map<std::string, std::string> known_users;      ///< user -> SID
    bool decline = false;                                ///< the user cancels the UAC prompt
    int child_exit_code = 0;
    long long pid = 4242;
    std::vector<nlohmann::json> plans_seen;              ///< plans handed to run_elevated (parsed plan_text)
    std::vector<ElevatedRequest> requests_seen;          ///< the full requests
    std::function<nlohmann::json(const ElevatedRequest&)> child;   ///< the elevated child: gets exactly what the command line + file would carry

    bool is_elevated() override { return elevated_child_running || type == ElevationType::Elevated; }
    ElevationType elevation_type() override { return is_elevated() ? ElevationType::Elevated : type; }
    std::string current_user_sid() override { return sid; }
    std::string current_user_name() override { return name; }
    std::optional<std::string> resolve_user_sid(const std::string& user) override {
        const auto it = known_users.find(user);
        if (it == known_users.end()) return std::nullopt;
        return it->second;
    }
    long long process_id() override { return pid; }
    ElevatedRun run_elevated(const ElevatedRequest& request) override {
        requests_seen.push_back(request);
        plans_seen.push_back(nlohmann::json::parse(request.plan_text, nullptr, false));
        ElevatedRun run;
        if (decline) {
            run.declined = true;
            return run;
        }
        run.launched = true;
        elevated_child_running = true;
        try {
            if (child) run.result = child(request);
        } catch (...) {
            elevated_child_running = false;
            throw;
        }
        elevated_child_running = false;
        run.exit_code = child_exit_code;
        return run;
    }
};

struct FakeHttpSys : HttpSysConfig {
    explicit FakeHttpSys(FakeElevator& e) : elevator(e) {}
    FakeElevator& elevator;
    std::map<std::string, std::string> urlacls;      ///< prefix -> SDDL
    std::map<std::string, SslBinding> bindings;      ///< ipport -> binding
    std::vector<std::string> calls;
    std::string fail_ipport;                         ///< set_sslcert on this ipport throws

    void need_admin() const {
        if (!elevator.is_elevated()) throw HostError("ACCESS_DENIED", "Access is denied (elevation required)");
    }
    std::optional<std::string> query_urlacl(const std::string& prefix) override {
        const auto it = urlacls.find(prefix);
        if (it == urlacls.end()) return std::nullopt;
        return it->second;
    }
    void add_urlacl(const std::string& prefix, const std::string& sddl) override {
        need_admin();
        if (urlacls.count(prefix)) throw HostError("ALREADY_EXISTS", "urlacl exists");
        urlacls[prefix] = sddl;
        calls.push_back("add_urlacl " + prefix + " " + sddl);
    }
    bool remove_urlacl(const std::string& prefix) override {
        need_admin();
        calls.push_back("remove_urlacl " + prefix);
        return urlacls.erase(prefix) > 0;
    }
    std::optional<SslBinding> query_sslcert(const std::string& ipport) override {
        const auto it = bindings.find(ipport);
        if (it == bindings.end()) return std::nullopt;
        return it->second;
    }
    void set_sslcert(const std::string& ipport, const std::string& thumbprint) override {
        need_admin();
        if (ipport == fail_ipport) throw HostError("HOST_ERROR", "cannot bind " + ipport);
        bindings[ipport] = SslBinding{ipport, thumbprint, normalize_app_id(kAppId), kCertStore};
        calls.push_back("set_sslcert " + ipport + " " + thumbprint);
    }
    bool remove_sslcert(const std::string& ipport) override {
        need_admin();
        calls.push_back("remove_sslcert " + ipport);
        return bindings.erase(ipport) > 0;
    }
};

struct FakeSystem : SystemProbe {
    std::map<std::string, std::string> files;
    std::vector<int> listening;                      ///< ports that accept connections
    TlsProbe tls;                                    ///< answer of tls_probe
    long long clock = 1800000000;
    std::string dns_name = "sapbox.corp.example";
    std::string lad = "C:\\Users\\jr\\AppData\\Local";
    bool tls_probe_called = false;

    bool tcp_listening(const std::string&, int port) override {
        return std::find(listening.begin(), listening.end(), port) != listening.end();
    }
    TlsProbe tls_probe(const std::string&, int, const std::string& expected) override {
        tls_probe_called = true;
        TlsProbe out = tls;
        if (out.status.empty()) out.status = "unreachable";
        out.thumbprint_match = out.status == "ok" && out.thumbprint == expected;
        return out;
    }
    std::optional<std::string> read_file(const std::string& path) override {
        const auto it = files.find(path);
        if (it == files.end()) return std::nullopt;
        return it->second;
    }
    bool write_file(const std::string& path, const std::string& text) override {
        files[path] = text;
        return true;
    }
    bool remove_file(const std::string& path) override { return files.erase(path) > 0; }
    bool file_exists(const std::string& path) override { return files.count(path) != 0; }
    long long now() override { return clock; }
    std::string computer_dns_name() override { return dns_name; }
    std::string local_app_data() override { return lad; }
};

struct FakeCertStore : CertStore {
    explicit FakeCertStore(FakeElevator& e, FakeSystem* s = nullptr) : elevator(e), sys(s) {}
    FakeElevator& elevator;
    FakeSystem* sys;                                 ///< exported .cer files live in the fake file system
    std::vector<CertInfo> certs;
    std::vector<std::string> calls;
    long long clock = 1800000000;
    int counter = 0;

    CertInfo find_by_thumbprint(const std::string& thumbprint) override {
        for (const auto& c : certs)
            if (c.thumbprint == thumbprint) return c;
        return {};
    }
    CertInfo find_self_signed(const std::string& hostname) override {
        CertInfo best;
        for (const auto& c : certs)
            if (c.friendly_name == "fairyfly-mcp " + hostname && (!best.found || c.not_after > best.not_after)) best = c;
        return best;
    }
    CertInfo create_self_signed(const std::string& hostname, int valid_days) override {
        if (!elevator.is_elevated()) throw HostError("ACCESS_DENIED", "Access is denied (elevation required)");
        CertInfo c;
        c.found = true;
        c.thumbprint = std::string(36, 'A') + (std::to_string(1000 + ++counter));
        c.subject = "CN=" + hostname;
        c.friendly_name = "fairyfly-mcp " + hostname;
        c.dns_names = {hostname};
        c.not_after = clock + static_cast<long long>(valid_days) * 86400;
        c.has_private_key = true;
        certs.push_back(c);
        calls.push_back("create_self_signed " + hostname);
        return c;
    }
    CerFileState cer_file_state(const std::string& thumbprint, const std::string& path, CerFormat) override {
        const auto it = sys->files.find(path);
        if (it == sys->files.end()) return CerFileState::Missing;
        return it->second == "cer:" + thumbprint ? CerFileState::Matches : CerFileState::Differs;
    }
    std::optional<std::string> file_thumbprint(const std::string& path) override {
        const auto it = sys->files.find(path);
        if (it == sys->files.end() || it->second.rfind("cer:", 0) != 0) return std::nullopt;
        return it->second.substr(4);
    }
    std::string export_cer(const std::string& thumbprint, const std::string& path, CerFormat format) override {
        const auto state = cer_file_state(thumbprint, path, format);
        sys->files[path] = "cer:" + thumbprint;
        calls.push_back("export_cer " + path);
        return state == CerFileState::Missing ? "created" : state == CerFileState::Matches ? "unchanged" : "updated";
    }
    bool remove(const std::string& thumbprint) override {
        if (!elevator.is_elevated()) throw HostError("ACCESS_DENIED", "Access is denied (elevation required)");
        calls.push_back("remove_cert " + thumbprint);
        const auto before = certs.size();
        certs.erase(std::remove_if(certs.begin(), certs.end(), [&](const CertInfo& c) { return c.thumbprint == thumbprint; }),
                    certs.end());
        return certs.size() != before;
    }
};

struct FakeFirewall : Firewall {
    struct Rule {
        std::string display;
        int port = 0;
    };
    explicit FakeFirewall(FakeElevator& e) : elevator(e) {}
    FakeElevator& elevator;
    std::map<std::string, Rule> rules;               ///< internal Name -> rule
    std::vector<std::string> calls;
    bool exists(const std::string& name) override { return rules.count(name) != 0; }
    bool display_name_exists(const std::string& display_name) override {
        for (const auto& [name, rule] : rules)
            if (rule.display == display_name) return true;
        return false;
    }
    std::string ensure(const std::string& name, const std::string& display_name, int port) override {
        if (!elevator.is_elevated()) throw HostError("ACCESS_DENIED", "Access is denied (elevation required)");
        calls.push_back("firewall_ensure " + name);
        const auto it = rules.find(name);
        if (it == rules.end()) { rules[name] = Rule{display_name, port}; return "created"; }
        if (it->second.port == port) return "unchanged";
        it->second.port = port;
        return "updated";
    }
    bool remove(const std::string& name) override {
        if (!elevator.is_elevated()) throw HostError("ACCESS_DENIED", "Access is denied (elevation required)");
        calls.push_back("firewall_remove " + name);
        return rules.erase(name) > 0;
    }
};

struct FakeVerify : VerifyHost {
    VerifyResult result{"ok", "TLS 1.3", 401, true, ""};
    std::vector<VerifyRequest> requests;
    std::function<void()> on_round_trip;             ///< lets a test change the machine while the probe runs
    VerifyResult round_trip(const VerifyRequest& request) override {
        requests.push_back(request);
        if (on_round_trip) on_round_trip();
        VerifyResult out = result;
        if (out.status == "ok") out.thumbprint_match = out.thumbprint_match && (!request.tls || !request.expected_thumbprint.empty());
        return out;
    }
};

/// A complete fake machine.
struct FakeMachine {
    FakeElevator elevator;
    FakeHttpSys http{elevator};
    FakeSystem sys;
    FakeCertStore certs{elevator, &sys};
    FakeFirewall firewall{elevator};
    FakeVerify verify;
    Hosts hosts() { return Hosts{http, certs, firewall, elevator, sys, verify}; }
};

} // namespace fairyfly::setup
