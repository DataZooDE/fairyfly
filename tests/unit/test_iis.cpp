// Unit tests of the IIS reverse-proxy integration. Everything runs against fakes: no test touches
// IIS, certificates, the registry, the firewall or a real PowerShell.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "include/credential_store.h"
#include "include/iis/iis_service.h"
#include "include/iis/powershell_host.h"
#include "include/iis/validators.h"
#include "include/iis/web_config.h"

using namespace fairyfly::iis;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr int64_t kNow = 1'800'000'000;  // fixed "now"
constexpr int64_t kDay = 86400;

// ---------------------------------------------------------------------------------------------
// Fake host

class FakeIisHost final : public IisHost {
public:
    HostFacts facts_ = all_ok();
    std::map<std::string, SiteInfo> sites;
    std::map<std::string, AppPoolInfo> pools;
    std::map<std::string, CertInfo> certs;      // thumbprint -> cert
    std::map<std::string, std::string> files;
    std::set<std::string> dirs, firewall, acl_done, config_access_done;
    bool upstream_up = true;
    std::string fail_on;                        // op name that throws HostError
    std::vector<std::string> mutations;         // every state-changing call, in order
    int self_signed_created = 0;

    static HostFacts all_ok() {
        HostFacts f;
        f.elevated = f.iis_installed = f.url_rewrite = f.arr_installed = f.arr_proxy_enabled = f.ip_security = true;
        f.admin_module = "WebAdministration";
        return f;
    }

    void mutate(const std::string& op) {
        if (fail_on == op) throw HostError{"IIS_SCRIPT_FAILED", "simulated failure in " + op};
        mutations.push_back(op);
    }

    HostFacts detect() override { return facts_; }
    SiteInfo get_site(const std::string& n) override { return sites.count(n) ? sites[n] : SiteInfo{}; }
    AppPoolInfo get_app_pool(const std::string& n) override { return pools.count(n) ? pools[n] : AppPoolInfo{}; }
    std::optional<CertInfo> find_certificate(const std::string& t) override {
        return certs.count(t) ? std::optional<CertInfo>(certs[t]) : std::nullopt;
    }
    std::optional<CertInfo> find_self_signed(const std::string& host) override {
        for (const auto& [t, c] : certs) {
            if (c.subject == "CN=" + host + " (fairyfly)") return c;
        }
        return std::nullopt;
    }
    bool firewall_rule_exists(const std::string& r) override { return firewall.count(r) > 0; }
    bool tcp_reachable(const std::string&, int, int) override { return upstream_up; }
    bool dir_exists(const std::string& p) override { return dirs.count(p) > 0; }
    std::optional<std::string> read_file(const std::string& p) override {
        return files.count(p) ? std::optional<std::string>(files[p]) : std::nullopt;
    }
    Change create_dir(const std::string& p) override {
        if (dirs.count(p)) return Change::Unchanged;
        mutate("create_dir");
        dirs.insert(p);
        return Change::Created;
    }
    Change write_file(const std::string& p, const std::string& c) override {
        const auto it = files.find(p);
        if (it != files.end() && it->second == c) return Change::Unchanged;
        mutate("write_file:" + p.substr(p.find_last_of('\\') + 1));
        const Change r = it == files.end() ? Change::Created : Change::Updated;
        files[p] = c;
        return r;
    }
    bool remove_file(const std::string& p) override {
        mutate("remove_file");
        return files.erase(p) > 0;
    }
    bool remove_dir_if_empty(const std::string& p) override {
        mutate("remove_dir");
        return dirs.erase(p) > 0;
    }
    CertInfo create_self_signed(const std::string& host, int days) override {
        mutate("create_self_signed");
        ++self_signed_created;
        CertInfo c;
        c.thumbprint = std::string(36, 'A') + std::to_string(1000 + self_signed_created);
        c.subject = "CN=" + host + " (fairyfly)";
        c.dns_names = {host};
        c.not_after = kNow + days * kDay;
        c.has_private_key = true;
        certs[c.thumbprint] = c;
        return c;
    }
    Change export_certificate(const std::string& t, const std::string& path) override {
        const std::string content = "CER:" + t;
        if (files.count(path) && files[path] == content) return Change::Unchanged;
        mutate("export_certificate");
        files[path] = content;
        return Change::Created;
    }
    Change ensure_app_pool(const std::string& n) override {
        const auto it = pools.find(n);
        if (it != pools.end() && it->second.runtime_version.empty() && it->second.identity == "ApplicationPoolIdentity") return Change::Unchanged;
        mutate("ensure_app_pool");
        const Change r = it == pools.end() ? Change::Created : Change::Updated;
        pools[n] = AppPoolInfo{true, "", "ApplicationPoolIdentity", "Started"};
        return r;
    }
    Change ensure_site(const SiteSpec& s) override {
        const auto it = sites.find(s.name);
        SiteInfo want{true, "Started", s.physical_path, s.app_pool, {{"https", s.hostname, s.port, s.thumbprint}}};
        if (it != sites.end() && it->second.physical_path == want.physical_path && it->second.app_pool == want.app_pool &&
            it->second.bindings.size() == 1 && it->second.bindings[0].thumbprint == s.thumbprint &&
            it->second.bindings[0].host == s.hostname && it->second.bindings[0].port == s.port && it->second.state == "Started")
            return Change::Unchanged;
        mutate("ensure_site");
        const Change r = it == sites.end() ? Change::Created : Change::Updated;
        sites[s.name] = want;
        return r;
    }
    Change ensure_site_config_access(const std::string& n, const std::vector<std::string>& vars) override {
        if (config_access_done.count(n)) return Change::Unchanged;
        mutate("config_access");
        config_access_done.insert(n);
        vars_ = vars;
        return Change::Updated;
    }
    Change restrict_acl(const std::string& p, const std::string&) override {
        if (acl_done.count(p)) return Change::Unchanged;
        mutate("restrict_acl");
        acl_done.insert(p);
        return Change::Updated;
    }
    Change ensure_firewall_rule(const std::string& r, int) override {
        if (firewall.count(r)) return Change::Unchanged;
        mutate("firewall");
        firewall.insert(r);
        return Change::Created;
    }
    bool remove_firewall_rule(const std::string& r) override {
        mutate("remove_firewall");
        return firewall.erase(r) > 0;
    }
    bool remove_site(const std::string& n) override {
        mutate("remove_site");
        return sites.erase(n) > 0;
    }
    bool remove_app_pool(const std::string& n) override {
        mutate("remove_app_pool");
        return pools.erase(n) > 0;
    }
    bool remove_certificate(const std::string& t) override {
        mutate("remove_cert");
        return certs.erase(t) > 0;
    }

    std::vector<std::string> vars_;
};

struct Env {
    FakeIisHost host;
    fairyfly::cred::InMemoryCredentialStore store;
    int counter = 0;
    int64_t now = kNow;
    IisContext ctx{host, store,
                   [this] { return "SECRET" + std::to_string(++counter) + "deadbeefdeadbeefdeadbeefdeadbeef"; },
                   [this] { return now; }};
};

SetupOptions base_options() {
    SetupOptions o;
    o.hostname = "mcp.example.com";
    o.self_signed = true;
    o.allow_ips = {"10.0.0.0/8", "192.168.1.5"};
    o.yes = true;
    return o;
}

std::string dump(const Report& r) { return r.data.dump() + "|" + r.error_code + "|" + r.error_message; }

std::string step_status(const Report& r, const std::string& name) {
    for (const auto& s : r.data.at("steps")) {
        if (s["step"] == name) return s["status"];
    }
    return "<missing>";
}

std::string golden_path(const std::string& name) { return std::string(FAIRYFLY_IIS_GOLDEN_DIR) + "/" + name; }

void check_golden(const std::string& name, const std::string& actual) {
    const std::string path = golden_path(name);
    if (const char* upd = std::getenv("FAIRYFLY_UPDATE_GOLDEN"); upd && std::string(upd) == "1") {
        std::ofstream(path, std::ios::binary) << actual;
    }
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    REQUIRE(ss.str() == actual);
}

const std::string kSecretRun1 = "SECRET1deadbeefdeadbeefdeadbeefdeadbeef";

} // namespace

// ---------------------------------------------------------------------------------------------
// validators

TEST_CASE("iis validators: hostname", "[iis]") {
    for (const char* ok : {"mcp.example.com", "a", "srv-01.corp.local", "A1.b2"}) CHECK_FALSE(hostname_error(ok));
    for (const char* bad : {"", "-a.com", "a..com", "a.com.", "*.example.com", "exa mple.com", "a_b.com", "10.1.2.3",
                            "a.com\"; calc", "a$(calc).com", "a`b.com", "a/b", "a:8443", "über.de"}) {
        INFO(bad);
        CHECK(hostname_error(bad));
    }
    CHECK(hostname_error(std::string(64, 'a') + ".com"));
    CHECK(hostname_error(std::string(254, 'a')));
}

TEST_CASE("iis validators: thumbprint", "[iis]") {
    CHECK_FALSE(thumbprint_error(std::string(40, 'a')));
    CHECK_FALSE(thumbprint_error("0123456789ABCDEF0123456789abcdef01234567"));
    CHECK(thumbprint_error(std::string(39, 'a')));
    CHECK(thumbprint_error(std::string(41, 'a')));
    CHECK(thumbprint_error(std::string(39, 'a') + "g"));
    CHECK(thumbprint_error(std::string(38, 'a') + "\"; calc"));
    CHECK(thumbprint_error(std::string(20, 'a') + " " + std::string(19, 'b')));
    CHECK(normalize_thumbprint("abcdef") == "ABCDEF");
}

TEST_CASE("iis validators: CIDR", "[iis]") {
    for (const char* ok : {"10.0.0.0/8", "192.168.1.5", "0.0.0.0/0", "255.255.255.255/32", "::1", "fe80::/10", "2001:db8::1/128"})
        CHECK_FALSE(cidr_error(ok));
    for (const char* bad : {"", "10.0.0.256", "10.0.0.0/33", "10.0.0/8", "10.0.0.0/", "10.0.0.0/8; calc", "a.b.c.d", "::g", ":::1",
                            "1:2:3:4:5:6:7:8:9", "10.0.0.0/-1", "10.0.0.0 /8", "$(calc)", "10.0.0.1,10.0.0.2"}) {
        INFO(bad);
        CHECK(cidr_error(bad));
    }
    const auto r = ipv4_range("10.1.2.3/8");
    REQUIRE(r);
    CHECK(r->address == "10.0.0.0");
    CHECK(r->mask == "255.0.0.0");
    CHECK(ipv4_range("192.168.1.5")->mask == "255.255.255.255");
    CHECK(ipv4_range("0.0.0.0/0")->mask == "0.0.0.0");
    CHECK_FALSE(ipv4_range("::1"));
}

TEST_CASE("iis validators: ports, names, paths, upstream", "[iis]") {
    CHECK_FALSE(port_error(1));
    CHECK_FALSE(port_error(8443));
    CHECK_FALSE(port_error(65535));
    CHECK(port_error(0));
    CHECK(port_error(65536));
    CHECK(port_error(-1));

    CHECK_FALSE(site_name_error("fairyfly-mcp"));
    CHECK(site_name_error(""));
    CHECK(site_name_error("-x"));
    CHECK(site_name_error("a b"));
    CHECK(site_name_error("a'; calc"));

    CHECK_FALSE(site_path_error("C:\\inetpub\\fairyfly-mcp"));
    CHECK_FALSE(site_path_error("D:\\web sites\\mcp.v2"));
    for (const char* bad : {"", "inetpub", "C:inetpub", "C:\\", "C:\\a\\..\\b", "C:\\a\\", "C:\\a;calc", "C:\\a\"b", "C:\\a$b", "\\\\srv\\share",
                            "C:\\a\\\\b", "C:\\a\\.", "C:\\a`b", "1:\\a"}) {
        INFO(bad);
        CHECK(site_path_error(bad));
    }

    CHECK_FALSE(upstream_error("http://127.0.0.1:8383"));
    CHECK_FALSE(upstream_error("http://localhost:1"));
    CHECK_FALSE(upstream_error("http://[::1]:65535"));
    for (const char* bad : {"", "https://127.0.0.1:8383", "http://10.0.0.1:8383", "http://127.0.0.1", "http://127.0.0.1:0",
                            "http://127.0.0.1:8383/mcp", "http://127.0.0.1:99999", "http://evil.com:80", "http://127.0.0.1:80\"><x"}) {
        INFO(bad);
        CHECK(upstream_error(bad));
    }
    CHECK(parse_upstream("http://127.0.0.1:8383")->port == 8383);
}

// ---------------------------------------------------------------------------------------------
// web.config generator

TEST_CASE("web.config goldens", "[iis][golden]") {
    WebConfigParams a;
    a.proxy_secret = "TESTSECRET";
    a.allow_ips = {"10.0.0.0/8", "192.168.1.5"};
    check_golden("web_default.config", generate_web_config(a));

    WebConfigParams b;
    b.upstream = "http://localhost:9000";
    b.proxy_secret = "TESTSECRET";
    b.allow_ips = {"203.0.113.0/24", "2001:db8::/32", "::1"};
    b.proxy_timeout_seconds = 600;
    b.max_content_bytes = 524288;
    b.hsts_max_age_seconds = 0;
    check_golden("web_ipv6_timeout.config", generate_web_config(b));

    WebConfigParams c;
    c.upstream = "http://[::1]:8383";
    c.proxy_secret = "a&b<c>\"d'";
    c.allow_any_ip = true;
    check_golden("web_any_ip_escaped.config", generate_web_config(c));
}

TEST_CASE("web.config invariants", "[iis]") {
    WebConfigParams p;
    p.proxy_secret = "S3CR3T";
    p.allow_ips = {"10.0.0.0/8"};
    const std::string x = generate_web_config(p);
    CHECK_THAT(x, ContainsSubstring("responseBufferLimit=\"0\""));
    CHECK_THAT(x, ContainsSubstring("timeout=\"00:05:00\""));
    CHECK_THAT(x, ContainsSubstring("maxAllowedContentLength=\"1048576\""));
    CHECK_THAT(x, ContainsSubstring("removeServerHeader=\"true\""));
    CHECK_THAT(x, ContainsSubstring("allowUnlisted=\"false\""));
    CHECK_THAT(x, ContainsSubstring("Strict-Transport-Security"));
    CHECK_THAT(x, ContainsSubstring("<directoryBrowse enabled=\"false\" />"));
    CHECK_THAT(x, ContainsSubstring("match url=\"^mcp/?$\""));
    CHECK_THAT(x, ContainsSubstring("statusCode=\"404\""));
    CHECK_THAT(x, ContainsSubstring("url=\"http://127.0.0.1:8383/mcp\""));
    CHECK_THAT(x, ContainsSubstring("HTTP_X_FAIRYFLY_PROXY_SECRET\" value=\"S3CR3T\""));
    CHECK_THAT(x, ContainsSubstring("HTTP_X_FORWARDED_FOR\" value=\"{REMOTE_ADDR}\""));
    CHECK_THAT(x, ContainsSubstring("HTTP_ACCEPT_ENCODING\" value=\"\""));
    // the hygiene rule (clears client-supplied values) comes before the proxy rule
    CHECK(x.find("fairyfly-strip-spoofed-headers") < x.find("name=\"fairyfly-mcp\""));
    // deterministic
    CHECK(generate_web_config(p) == x);
    // no auth modules configured
    CHECK_THAT(x, !ContainsSubstring("windowsAuthentication"));
    CHECK_THAT(x, !ContainsSubstring("basicAuthentication"));
    CHECK(format_timespan(300) == "00:05:00");
    CHECK(format_timespan(3725) == "01:02:05");

    for (const auto& v : required_server_variables()) CHECK_THAT(x, ContainsSubstring(v));
}

TEST_CASE("web.config generator rejects unsafe input", "[iis]") {
    WebConfigParams p;
    p.proxy_secret = "x";
    p.allow_ips = {"10.0.0.0/8"};
    WebConfigParams bad = p;
    bad.upstream = "http://evil.com:80";
    CHECK_THROWS(generate_web_config(bad));
    bad = p;
    bad.allow_ips = {"10.0.0.0/8\" /><x"};
    CHECK_THROWS(generate_web_config(bad));
    bad = p;
    bad.allow_ips.clear();
    CHECK_THROWS(generate_web_config(bad));
    bad = p;
    bad.proxy_secret.clear();
    CHECK_THROWS(generate_web_config(bad));
    bad = p;
    bad.proxy_secret = "a\nb";
    CHECK_THROWS(generate_web_config(bad));
    CHECK(mask_secret("aXbXc", "X") == std::string("a") + kSecretPlaceholder + "b" + kSecretPlaceholder + "c");
    CHECK(mask_secret("abc", "") == "abc");
    CHECK(content_hash("a") == content_hash("a"));
    CHECK(content_hash("a") != content_hash("b"));
}

// ---------------------------------------------------------------------------------------------
// prerequisites

TEST_CASE("prerequisite matrix", "[iis]") {
    struct Row {
        const char* id;
        void (*break_it)(HostFacts&);
    };
    const Row rows[] = {
        {"admin_rights", [](HostFacts& f) { f.elevated = false; }},
        {"iis_role", [](HostFacts& f) { f.iis_installed = false; }},
        {"iis_powershell_module", [](HostFacts& f) { f.admin_module.clear(); }},
        {"url_rewrite", [](HostFacts& f) { f.url_rewrite = false; }},
        {"arr_installed", [](HostFacts& f) { f.arr_installed = false; }},
        {"arr_proxy_enabled", [](HostFacts& f) { f.arr_proxy_enabled = false; }},
        {"ip_security", [](HostFacts& f) { f.ip_security = false; }},
    };
    CHECK(missing_ids(build_checklist(FakeIisHost::all_ok(), true)).empty());
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.id) {
            Env env;
            row.break_it(env.host.facts_);
            const auto list = build_checklist(env.host.facts_, true);
            REQUIRE(missing_ids(list) == std::vector<std::string>{row.id});
            for (const auto& item : list) {
                if (!item["ok"].get<bool>()) CHECK_FALSE(item["fix"].get<std::string>().empty());
            }
            const Report r = run_setup(env.ctx, base_options());
            REQUIRE_FALSE(r.ok);
            CHECK(r.error_code == "IIS_PREREQUISITE_MISSING");
            CHECK(r.data["missing"] == json::array({row.id}));
            CHECK_THAT(r.error_message, ContainsSubstring(row.id));
            CHECK(env.host.mutations.empty());
            CHECK(env.store.list().empty());
        }
    }
    SECTION("ip_security is not needed with --allow-any-ip") {
        HostFacts f = FakeIisHost::all_ok();
        f.ip_security = false;
        CHECK(missing_ids(build_checklist(f, false)).empty());
    }
    SECTION("several missing items are all listed") {
        Env env;
        env.host.facts_ = HostFacts{};
        const Report r = run_setup(env.ctx, base_options());
        REQUIRE_FALSE(r.ok);
        CHECK(r.data["missing"].size() == 7);
    }
    SECTION("IISAdministration alone satisfies the module check") {
        Env env;
        env.host.facts_.admin_module = "IISAdministration";
        CHECK(run_setup(env.ctx, base_options()).ok);
    }
    SECTION("dry run reports the missing items but still plans") {
        Env env;
        env.host.facts_.url_rewrite = false;
        SetupOptions o = base_options();
        o.dry_run = true;
        const Report r = run_setup(env.ctx, o);
        REQUIRE(r.ok);
        CHECK(r.data["prerequisites_ok"] == false);
        CHECK_THAT(r.data.dump(), ContainsSubstring("would stop with IIS_PREREQUISITE_MISSING"));
    }
}

// ---------------------------------------------------------------------------------------------
// option validation

TEST_CASE("setup option validation and injection rejection", "[iis]") {
    Env env;
    auto expect_invalid = [&](SetupOptions o, const std::string& field) {
        const Report r = run_setup(env.ctx, o);
        REQUIRE_FALSE(r.ok);
        CHECK(r.error_code == "INVALID_ARGUMENT");
        CHECK(r.data["field"] == field);
        CHECK(env.host.mutations.empty());
    };
    SetupOptions o = base_options();
    SECTION("hostname missing") { o.hostname.clear(); expect_invalid(o, "hostname"); }
    SECTION("hostname injection") { o.hostname = "a.com\"; calc"; expect_invalid(o, "hostname"); }
    SECTION("hostname subshell") { o.hostname = "$(calc).com"; expect_invalid(o, "hostname"); }
    SECTION("thumbprint injection") { o.self_signed = false; o.cert_thumbprint = std::string(30, 'A') + "\"; calc "; expect_invalid(o, "cert-thumbprint"); }
    SECTION("thumbprint and self-signed") { o.cert_thumbprint = std::string(40, 'A'); expect_invalid(o, "cert-thumbprint"); }
    SECTION("no certificate choice") { o.self_signed = false; expect_invalid(o, "cert-thumbprint"); }
    SECTION("cidr injection") { o.allow_ips = {"10.0.0.0/8; calc"}; expect_invalid(o, "allow-ip"); }
    SECTION("no allow-ip") { o.allow_ips.clear(); expect_invalid(o, "allow-ip"); }
    SECTION("allow-ip with allow-any-ip") { o.allow_any_ip = true; expect_invalid(o, "allow-ip"); }
    SECTION("port") { o.port = 70000; expect_invalid(o, "port"); }
    SECTION("port equals upstream port") { o.port = 8383; expect_invalid(o, "port"); }
    SECTION("upstream not loopback") { o.upstream = "http://10.0.0.1:8383"; expect_invalid(o, "upstream"); }
    SECTION("site name") { o.site_name = "x\"; calc"; expect_invalid(o, "site-name"); }
    SECTION("site path traversal") { o.site_path = "C:\\inetpub\\..\\Windows"; expect_invalid(o, "site-path"); }
    SECTION("status and remove validate site parameters too") {
        StatusOptions st;
        st.site_name = "a;calc";
        CHECK(run_status(env.ctx, st).error_code == "INVALID_ARGUMENT");
        RemoveOptions rm;
        rm.site_path = "C:\\x\\..\\y";
        CHECK(run_remove(env.ctx, rm).error_code == "INVALID_ARGUMENT");
    }
}

// ---------------------------------------------------------------------------------------------
// setup

TEST_CASE("setup dry run on a fresh machine only plans", "[iis]") {
    Env env;
    SetupOptions o = base_options();
    o.dry_run = true;
    o.yes = false;
    o.open_firewall = true;
    const Report r = run_setup(env.ctx, o);
    REQUIRE(r.ok);
    CHECK(r.data["dry_run"] == true);
    CHECK(env.host.mutations.empty());
    CHECK(env.store.list().empty());
    CHECK(env.host.files.empty());
    for (const char* step : {"certificate", "proxy_secret", "site_directory", "app_pool", "site", "web_config", "manifest", "firewall", "certificate_export"}) {
        INFO(step);
        CHECK(step_status(r, step) == "would_create");
    }
    CHECK(step_status(r, "site_config_access") == "would_apply");
    CHECK_FALSE(r.data.contains("proxy_secret"));
    CHECK(r.data["url"] == "https://mcp.example.com:8443/mcp");
}

TEST_CASE("setup dry run on an existing site reports unchanged/update", "[iis]") {
    Env env;
    REQUIRE(run_setup(env.ctx, base_options()).ok);
    env.host.mutations.clear();
    SetupOptions o = base_options();
    o.dry_run = true;
    o.port = 9443;  // the binding changes
    const Report r = run_setup(env.ctx, o);
    REQUIRE(r.ok);
    CHECK(step_status(r, "site") == "would_update");
    CHECK(step_status(r, "app_pool") == "unchanged");
    CHECK(step_status(r, "site_directory") == "unchanged");
    CHECK(step_status(r, "certificate") == "unchanged");
    CHECK(step_status(r, "proxy_secret") == "unchanged");
    CHECK(env.host.mutations.empty());
}

TEST_CASE("setup without --yes is refused", "[iis]") {
    Env env;
    SetupOptions o = base_options();
    o.yes = false;
    const Report r = run_setup(env.ctx, o);
    REQUIRE_FALSE(r.ok);
    CHECK(r.error_code == "CONFIRMATION_REQUIRED");
    CHECK(env.host.mutations.empty());
}

TEST_CASE("setup with a self-signed certificate creates everything", "[iis]") {
    Env env;
    SetupOptions o = base_options();
    o.open_firewall = true;
    const Report r = run_setup(env.ctx, o);
    REQUIRE(r.ok);
    for (const char* step : {"certificate", "proxy_secret", "site_directory", "app_pool", "site", "web_config", "manifest", "certificate_export", "firewall"}) {
        INFO(step);
        CHECK(step_status(r, step) == "created");
    }
    CHECK(step_status(r, "site_config_access") == "updated");
    CHECK(step_status(r, "web_config_acl") == "updated");

    const std::string wc = "C:\\inetpub\\fairyfly-mcp\\web.config";
    REQUIRE(env.host.files.count(wc));
    CHECK_THAT(env.host.files[wc], ContainsSubstring(kSecretRun1));
    REQUIRE(env.host.files.count("C:\\inetpub\\fairyfly-mcp-mcp.example.com.cer"));
    CHECK(env.host.pools["fairyfly-mcp"].runtime_version.empty());
    CHECK(env.host.pools["fairyfly-mcp"].identity == "ApplicationPoolIdentity");
    CHECK(env.host.sites["fairyfly-mcp"].bindings.size() == 1);
    CHECK(env.host.sites["fairyfly-mcp"].bindings[0].protocol == "https");
    CHECK(env.host.vars_ == required_server_variables());
    CHECK(env.host.firewall.count("fairyfly MCP HTTPS 8443") == 1);

    const auto stored = env.store.read(kProxySecretCredential);
    REQUIRE(stored);
    CHECK(stored->password.utf8() == kSecretRun1);

    // warnings: self-signed trust
    CHECK_THAT(r.data["warnings"].dump(), ContainsSubstring("self-signed"));
    // the secret is shown exactly once, in its own field
    CHECK(r.data["proxy_secret"] == kSecretRun1);
    json without = r.data;
    without.erase("proxy_secret");
    without.erase("proxy_secret_notice");
    CHECK_THAT(without.dump(), !ContainsSubstring(kSecretRun1));
    // the manifest never holds the secret
    CHECK_THAT(env.host.files["C:\\inetpub\\fairyfly-mcp\\fairyfly-iis.json"], !ContainsSubstring(kSecretRun1));
    // the firewall is only touched on request
    Env env2;
    const Report r2 = run_setup(env2.ctx, base_options());
    CHECK(step_status(r2, "firewall") == "skipped");
    CHECK(env2.host.firewall.empty());
}

TEST_CASE("setup is idempotent: the second run reports unchanged", "[iis]") {
    Env env;
    REQUIRE(run_setup(env.ctx, base_options()).ok);
    env.host.mutations.clear();
    const Report r2 = run_setup(env.ctx, base_options());
    REQUIRE(r2.ok);
    for (const auto& s : r2.data["steps"]) {
        INFO(s.dump());
        const std::string status = s["status"];
        CHECK((status == "unchanged" || status == "skipped"));
    }
    CHECK(env.host.mutations.empty());
    CHECK(env.host.self_signed_created == 1);
    CHECK_FALSE(r2.data.contains("proxy_secret"));
    CHECK(env.counter == 1);  // no second secret was generated
}

TEST_CASE("--rotate-secret replaces the secret and rewrites web.config", "[iis]") {
    Env env;
    REQUIRE(run_setup(env.ctx, base_options()).ok);
    SetupOptions o = base_options();
    o.rotate_secret = true;
    const Report r = run_setup(env.ctx, o);
    REQUIRE(r.ok);
    CHECK(step_status(r, "proxy_secret") == "updated");
    CHECK(step_status(r, "web_config") == "updated");
    const std::string second = "SECRET2deadbeefdeadbeefdeadbeefdeadbeef";
    CHECK(r.data["proxy_secret"] == second);
    CHECK_THAT(env.host.files["C:\\inetpub\\fairyfly-mcp\\web.config"], ContainsSubstring(second));
    CHECK_THAT(env.host.files["C:\\inetpub\\fairyfly-mcp\\web.config"], !ContainsSubstring(kSecretRun1));
    CHECK(env.store.read(kProxySecretCredential)->password.utf8() == second);
}

TEST_CASE("setup with an existing certificate", "[iis]") {
    Env env;
    CertInfo c;
    c.thumbprint = std::string(40, 'B');
    c.subject = "CN=*.example.com";
    c.dns_names = {"*.example.com"};
    c.not_after = kNow + 200 * kDay;
    c.has_private_key = true;
    env.host.certs[c.thumbprint] = c;
    SetupOptions o = base_options();
    o.self_signed = false;
    o.cert_thumbprint = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";  // lower case is normalised

    SECTION("valid wildcard certificate is used, no export") {
        const Report r = run_setup(env.ctx, o);
        REQUIRE(r.ok);
        CHECK(step_status(r, "certificate") == "unchanged");
        CHECK(step_status(r, "certificate_export") == "<missing>");
        CHECK(env.host.sites["fairyfly-mcp"].bindings[0].thumbprint == std::string(40, 'B'));
        CHECK(env.host.self_signed_created == 0);
        CHECK_THAT(r.data["warnings"].dump(), !ContainsSubstring("self-signed"));
    }
    SECTION("a wildcard does not cover a deeper name") {
        o.hostname = "a.b.example.com";
        const Report r = run_setup(env.ctx, o);
        REQUIRE_FALSE(r.ok);
        CHECK(r.error_code == "CERT_INVALID");
        CHECK(env.host.mutations.empty());
    }
    SECTION("unknown thumbprint") {
        o.cert_thumbprint = std::string(40, 'C');
        CHECK(run_setup(env.ctx, o).error_code == "CERT_NOT_FOUND");
        CHECK(env.host.mutations.empty());
    }
    SECTION("no private key") {
        env.host.certs[c.thumbprint].has_private_key = false;
        const Report r = run_setup(env.ctx, o);
        CHECK(r.error_code == "CERT_INVALID");
        CHECK_THAT(r.error_message, ContainsSubstring("private key"));
    }
    SECTION("expired") {
        env.host.certs[c.thumbprint].not_after = kNow - kDay;
        const Report r = run_setup(env.ctx, o);
        CHECK(r.error_code == "CERT_INVALID");
        CHECK_THAT(r.error_message, ContainsSubstring("expired"));
    }
    SECTION("wrong host") {
        o.hostname = "mcp.other.org";
        CHECK(run_setup(env.ctx, o).error_code == "CERT_INVALID");
    }
    SECTION("expiring soon warns") {
        env.host.certs[c.thumbprint].not_after = kNow + 10 * kDay;
        const Report r = run_setup(env.ctx, o);
        REQUIRE(r.ok);
        CHECK_THAT(r.data["warnings"].dump(), ContainsSubstring("expires in 10 days"));
    }
}

TEST_CASE("--allow-any-ip is allowed only explicitly and warns loudly", "[iis]") {
    Env env;
    SetupOptions o = base_options();
    o.allow_ips.clear();
    o.allow_any_ip = true;
    const Report r = run_setup(env.ctx, o);
    REQUIRE(r.ok);
    CHECK_THAT(r.data["warnings"].dump(), ContainsSubstring("WARNING: --allow-any-ip"));
    CHECK_THAT(env.host.files["C:\\inetpub\\fairyfly-mcp\\web.config"], !ContainsSubstring("<ipSecurity"));
}

TEST_CASE("an unreachable upstream is a warning, not an error", "[iis]") {
    Env env;
    env.host.upstream_up = false;
    const Report r = run_setup(env.ctx, base_options());
    REQUIRE(r.ok);
    CHECK_THAT(r.data["warnings"].dump(), ContainsSubstring("mcp --http"));
}

TEST_CASE("a partial failure reports the rollback path and never leaks the secret", "[iis]") {
    Env env;
    env.host.fail_on = "ensure_site";
    const Report r = run_setup(env.ctx, base_options());
    REQUIRE_FALSE(r.ok);
    CHECK(r.error_code == "IIS_SETUP_FAILED");
    CHECK(r.data["failed_step"] == "site");
    CHECK_THAT(r.data["rollback"].get<std::string>(), ContainsSubstring("mcp iis remove"));
    CHECK(r.data["applied_steps"].size() >= 3);
    CHECK_THAT(dump(r), !ContainsSubstring(kSecretRun1));
    // fixing the cause and re-running completes the setup
    env.host.fail_on.clear();
    const Report again = run_setup(env.ctx, base_options());
    REQUIRE(again.ok);
    CHECK(step_status(again, "certificate") == "unchanged");
    CHECK(step_status(again, "site") == "created");
}

// ---------------------------------------------------------------------------------------------
// status

TEST_CASE("status of a healthy installation", "[iis]") {
    Env env;
    REQUIRE(run_setup(env.ctx, base_options()).ok);
    const Report r = run_status(env.ctx, StatusOptions{});
    REQUIRE(r.ok);
    INFO(r.data.dump(2));
    CHECK(r.data["summary"] == "healthy");
    CHECK(r.data["url"] == "https://mcp.example.com:8443/mcp");
    CHECK(r.data["drift"]["drift"] == false);
    CHECK_THAT(r.data.dump(), !ContainsSubstring(kSecretRun1));
}

TEST_CASE("status detects web.config drift without printing content", "[iis]") {
    Env env;
    REQUIRE(run_setup(env.ctx, base_options()).ok);
    auto& wc = env.host.files["C:\\inetpub\\fairyfly-mcp\\web.config"];
    wc += "<!-- hand edit -->\n";
    const Report r = run_status(env.ctx, StatusOptions{});
    REQUIRE(r.ok);
    CHECK(r.data["summary"] == "degraded");
    CHECK(r.data["drift"]["drift"] == true);
    CHECK(r.data["drift"]["expected_hash"] != r.data["drift"]["actual_hash"]);
    CHECK_THAT(r.data.dump(), !ContainsSubstring("hand edit"));
    CHECK_THAT(r.data.dump(), !ContainsSubstring(kSecretRun1));
    // a rotated secret in the file that the store does not know is also drift
    wc = generate_web_config([] {
        WebConfigParams p;
        p.proxy_secret = "OTHER";
        p.allow_ips = {"10.0.0.0/8", "192.168.1.5"};
        return p;
    }());
    CHECK(run_status(env.ctx, StatusOptions{}).data["drift"]["drift"] == true);
}

TEST_CASE("status findings", "[iis]") {
    Env env;
    REQUIRE(run_setup(env.ctx, base_options()).ok);
    auto find = [](const Report& r, const std::string& check) {
        for (const auto& c : r.data["checks"]) {
            if (c["check"] == check) return c;
        }
        return json{};
    };
    SECTION("certificate expiry warning under 30 days") {
        env.now = kNow + 720 * kDay;  // ~10 days left of 730
        const Report r = run_status(env.ctx, StatusOptions{});
        CHECK(find(r, "certificate")["status"] == "warn");
        CHECK(r.data["summary"] == "degraded");
    }
    SECTION("expired certificate fails") {
        env.now = kNow + 800 * kDay;
        CHECK(find(run_status(env.ctx, StatusOptions{}), "certificate")["status"] == "fail");
    }
    SECTION("upstream down is a warning") {
        env.host.upstream_up = false;
        CHECK(find(run_status(env.ctx, StatusOptions{}), "upstream")["status"] == "warn");
    }
    SECTION("stopped site fails") {
        env.host.sites["fairyfly-mcp"].state = "Stopped";
        const Report r = run_status(env.ctx, StatusOptions{});
        CHECK(find(r, "site")["status"] == "fail");
        CHECK(r.data["summary"] == "unhealthy");
    }
    SECTION("missing secret fails") {
        env.store.remove(kProxySecretCredential);
        CHECK(find(run_status(env.ctx, StatusOptions{}), "proxy_secret")["status"] == "fail");
    }
    SECTION("missing prerequisites fail") {
        env.host.facts_.arr_proxy_enabled = false;
        CHECK(find(run_status(env.ctx, StatusOptions{}), "prerequisites")["status"] == "fail");
    }
    SECTION("no installation at all") {
        Env empty;
        const Report r = run_status(empty.ctx, StatusOptions{});
        REQUIRE(r.ok);
        CHECK(find(r, "site")["status"] == "fail");
        CHECK(r.data["summary"] == "unhealthy");
    }
    SECTION("status does not need elevation") {
        env.host.facts_.elevated = false;
        CHECK(find(run_status(env.ctx, StatusOptions{}), "prerequisites")["status"] == "ok");
    }
}

// ---------------------------------------------------------------------------------------------
// remove

TEST_CASE("remove", "[iis]") {
    Env env;
    SetupOptions so = base_options();
    so.open_firewall = true;
    REQUIRE(run_setup(env.ctx, so).ok);
    env.host.mutations.clear();

    SECTION("dry run changes nothing") {
        RemoveOptions o;
        o.dry_run = true;
        o.remove_cert = o.remove_firewall = o.delete_secret = true;
        const Report r = run_remove(env.ctx, o);
        REQUIRE(r.ok);
        CHECK(env.host.mutations.empty());
        CHECK(step_status(r, "site") == "would_remove");
        CHECK(step_status(r, "certificate") == "would_remove");
        CHECK(env.store.read(kProxySecretCredential).has_value());
    }
    SECTION("needs --yes and elevation") {
        RemoveOptions o;
        CHECK(run_remove(env.ctx, o).error_code == "CONFIRMATION_REQUIRED");
        env.host.facts_.elevated = false;
        o.yes = true;
        CHECK(run_remove(env.ctx, o).error_code == "IIS_PREREQUISITE_MISSING");
        CHECK(env.host.mutations.empty());
    }
    SECTION("default removal keeps certificate, firewall rule and secret") {
        RemoveOptions o;
        o.yes = true;
        const Report r = run_remove(env.ctx, o);
        REQUIRE(r.ok);
        CHECK(env.host.sites.empty());
        CHECK(env.host.pools.empty());
        CHECK_FALSE(env.host.files.count("C:\\inetpub\\fairyfly-mcp\\web.config"));
        CHECK_FALSE(env.host.files.count("C:\\inetpub\\fairyfly-mcp\\fairyfly-iis.json"));
        CHECK(env.host.certs.size() == 1);
        CHECK(env.host.firewall.size() == 1);
        CHECK(step_status(r, "proxy_secret") == "kept");
        CHECK(env.store.read(kProxySecretCredential).has_value());
        CHECK_THAT(dump(r), !ContainsSubstring(kSecretRun1));
        // second removal: everything already gone
        const Report again = run_remove(env.ctx, o);
        REQUIRE(again.ok);
        CHECK(step_status(again, "site") == "unchanged");
        CHECK(step_status(again, "web_config") == "unchanged");
    }
    SECTION("optional removals") {
        RemoveOptions o;
        o.yes = true;
        o.remove_cert = o.remove_firewall = o.delete_secret = true;
        const Report r = run_remove(env.ctx, o);
        REQUIRE(r.ok);
        CHECK(env.host.certs.empty());
        CHECK(env.host.firewall.empty());
        CHECK_FALSE(env.store.read(kProxySecretCredential).has_value());
        CHECK_FALSE(env.host.files.count("C:\\inetpub\\fairyfly-mcp-mcp.example.com.cer"));
    }
}

TEST_CASE("remove never deletes a certificate fairyfly did not create", "[iis]") {
    Env env;
    CertInfo c;
    c.thumbprint = std::string(40, 'D');
    c.dns_names = {"mcp.example.com"};
    c.not_after = kNow + 300 * kDay;
    c.has_private_key = true;
    env.host.certs[c.thumbprint] = c;
    SetupOptions so = base_options();
    so.self_signed = false;
    so.cert_thumbprint = c.thumbprint;
    REQUIRE(run_setup(env.ctx, so).ok);
    RemoveOptions o;
    o.yes = true;
    o.remove_cert = true;
    const Report r = run_remove(env.ctx, o);
    REQUIRE(r.ok);
    CHECK(step_status(r, "certificate") == "skipped");
    CHECK(env.host.certs.count(c.thumbprint) == 1);
}

// ---------------------------------------------------------------------------------------------
// the secret never leaks

TEST_CASE("the proxy secret appears in no output other than the one-time creation field", "[iis][secret]") {
    Env env;
    std::vector<Report> reports;
    SetupOptions dry = base_options();
    dry.dry_run = true;
    reports.push_back(run_setup(env.ctx, dry));
    const Report first = run_setup(env.ctx, base_options());
    reports.push_back(run_setup(env.ctx, base_options()));   // second run
    reports.push_back(run_status(env.ctx, StatusOptions{}));
    RemoveOptions rm;
    rm.dry_run = true;
    rm.delete_secret = true;
    reports.push_back(run_remove(env.ctx, rm));
    rm.dry_run = false;
    rm.yes = true;
    rm.delete_secret = true;
    reports.push_back(run_remove(env.ctx, rm));
    SetupOptions bad = base_options();
    bad.hostname = "x;calc";
    reports.push_back(run_setup(env.ctx, bad));

    REQUIRE(first.data.contains("proxy_secret"));
    const std::string secret = first.data["proxy_secret"];
    CHECK(secret.size() >= 32);
    json stripped = first.data;
    stripped.erase("proxy_secret");
    stripped.erase("proxy_secret_notice");
    CHECK_THAT(stripped.dump(), !ContainsSubstring(secret));
    CHECK_THAT(first.data["proxy_secret_notice"].get<std::string>(), !ContainsSubstring(secret));
    for (const auto& r : reports) CHECK_THAT(dump(r), !ContainsSubstring(secret));
}

// ---------------------------------------------------------------------------------------------
// PowerShell host with a fake runner: constant scripts, parameters only via JSON

namespace {

class FakeRunner final : public PowerShellRunner {
public:
    struct Call {
        std::string script;
        std::string params;
        int timeout_ms;
    };
    std::vector<Call> calls;
    PsResult next;

    PsResult run(const std::string& script, const std::string& params, int timeout_ms) override {
        calls.push_back({script, params, timeout_ms});
        return next;
    }
};

} // namespace

TEST_CASE("PowerShell scripts are constant and carry no interpolation", "[iis][ps]") {
    const auto& catalog = script_catalog();
    REQUIRE(catalog.size() >= 15);
    std::set<std::string> texts;
    for (const auto& [name, text] : catalog) {
        INFO(name);
        CHECK_THAT(text, ContainsSubstring("$env:FAIRYFLY_IIS_PARAMS"));
        CHECK_THAT(text, ContainsSubstring("$ErrorActionPreference = 'Stop'"));
        CHECK_THAT(text, !ContainsSubstring("Invoke-Expression"));
        CHECK_THAT(text, !ContainsSubstring("iex "));
        CHECK_THAT(text, !ContainsSubstring("{{"));
        CHECK_THAT(text, !ContainsSubstring("Read-Host"));
        CHECK_THAT(text, !ContainsSubstring("-Force -Confirm:$false"));  // no blanket force flags
        texts.insert(text);
    }
    CHECK(texts.size() == catalog.size());
}

TEST_CASE("PowerShell host passes user input only in the JSON params", "[iis][ps]") {
    FakeRunner runner;
    auto host = make_powershell_host(runner);
    runner.next.out = R"({"exists":false})";
    const std::string evil = "x\"; calc; $(calc) `whoami`";

    host->get_site(evil);
    host->get_site("plain");
    REQUIRE(runner.calls.size() == 2);
    CHECK(runner.calls[0].script == runner.calls[1].script);            // identical script text
    CHECK_THAT(runner.calls[0].script, !ContainsSubstring("calc"));
    CHECK_THAT(runner.calls[0].params, ContainsSubstring("calc"));      // ...the value travels as data
    CHECK(json::parse(runner.calls[0].params)["name"] == evil);         // and round-trips exactly
    CHECK(runner.calls[0].timeout_ms > 0);

    runner.next.out = R"({"change":"created"})";
    host->ensure_site(SiteSpec{evil, "C:\\p", "pool", "h.example.com", 8443, std::string(40, 'A')});
    host->restrict_acl("C:\\a\\web.config", evil);
    for (const auto& c : runner.calls) CHECK_THAT(c.script, !ContainsSubstring("calc"));
}

TEST_CASE("PowerShell host maps runner failures to structured errors", "[iis][ps]") {
    FakeRunner runner;
    auto host = make_powershell_host(runner);
    runner.next.exit_code = 1;
    runner.next.err = "Access denied\r\nat line 3";
    try {
        host->detect();
        FAIL("expected HostError");
    } catch (const HostError& e) {
        CHECK(e.code == "IIS_SCRIPT_FAILED");
        CHECK_THAT(e.message, ContainsSubstring("Access denied"));
        CHECK_THAT(e.message, !ContainsSubstring("line 3"));
    }
    runner.next = PsResult{};
    runner.next.timed_out = true;
    try {
        host->detect();
        FAIL("expected HostError");
    } catch (const HostError& e) {
        CHECK(e.code == "IIS_SCRIPT_TIMEOUT");
    }
    runner.next = PsResult{};
    runner.next.out = "not json";
    CHECK_THROWS_AS(host->detect(), HostError);

    runner.next.out = R"({"elevated":true,"iis_installed":true,"admin_module":"WebAdministration","url_rewrite":true,"arr_installed":false,"arr_proxy_enabled":false,"ip_security":true})";
    const HostFacts f = host->detect();
    CHECK(f.elevated);
    CHECK(f.admin_module == "WebAdministration");
    CHECK_FALSE(f.arr_installed);

    runner.next.out = R"({"found":true,"thumbprint":"AB","subject":"CN=x","dns_names":["x","y"],"not_after":123,"has_private_key":true})";
    const auto cert = host->find_certificate(std::string(40, 'A'));
    REQUIRE(cert);
    CHECK(cert->dns_names.size() == 2);
    CHECK(cert->not_after == 123);
}
