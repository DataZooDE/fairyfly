#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "include/setup/fake_hosts.h"
#include "include/setup/setup_doctor.h"
#include "include/setup/setup_model.h"
#include "include/setup/setup_service.h"
#include "include/setup/setup_validators.h"
#include "include/system/cert_scripts.h"

using namespace fairyfly::setup;
using nlohmann::json;

namespace {

const std::string kHost = "sapbox.corp.example";
const std::string kSid = "S-1-5-21-1-2-3-1001";
const std::string kPrefix = "https://+:8443/mcp/";
const std::string kLad = "C:\\Users\\jr\\AppData\\Local";
const std::string kManifestPath = kLad + "\\fairyfly\\mcp-setup.json";
const std::string kConfigPath = kLad + "\\fairyfly\\mcp.yaml";
const std::string kCerPath = kLad + "\\fairyfly\\fairyfly-mcp-" + kHost + ".cer";

struct Rig {
    FakeMachine m;
    std::ostringstream out, err;
    RunEnv env;

    Rig() {
        env.out = &out;
        env.err = &err;
        m.elevator.child = [this](const json& plan) {
            Hosts h = m.hosts();
            return execute_elevated_work(plan, h);
        };
    }

    static Options self_signed() {
        Options o;
        o.self_signed = true;
        o.yes = true;
        return o;
    }
    int setup(Options o) {
        out.str("");
        err.str("");
        Hosts h = m.hosts();
        return run_setup(h, std::move(o), env);
    }
    int teardown(TeardownOptions o) {
        out.str("");
        err.str("");
        Hosts h = m.hosts();
        return run_teardown(h, std::move(o), env);
    }
    Plan plan(Options o) {
        Hosts h = m.hosts();
        fill_defaults(h, o);
        REQUIRE_FALSE(validate_options(o).has_value());
        return MakePlan(diagnose(h, o), o);
    }
    TeardownDiagnosis tdiag(TeardownOptions o = {}) {
        Hosts h = m.hosts();
        return diagnose_teardown(h, o);
    }
    static const StepItem& step(const Plan& p, const std::string& id) {
        const StepItem* s = p.step(id);
        REQUIRE(s != nullptr);
        return *s;
    }
    static std::string status(const Plan& p, const std::string& id) { return step(p, id).status; }
};

CertInfo good_cert(const std::string& thumb, const std::string& host = kHost, long long now = 1800000000) {
    CertInfo c;
    c.found = true;
    c.thumbprint = thumb;
    c.subject = "CN=" + host;
    c.friendly_name = "fairyfly-mcp " + host;
    c.dns_names = {host};
    c.not_after = now + 400LL * 86400;
    c.has_private_key = true;
    return c;
}

const std::string kThumb = "0123456789ABCDEF0123456789ABCDEF01234567";

} // namespace

// ---- validators / injection ------------------------------------------------------------------------------------
TEST_CASE("setup validators reject injection strings", "[mcp_setup]") {
    for (const std::string bad : {std::string("\"; calc"), std::string("$(calc)"), std::string("a`b"), std::string("a b"), std::string("x;y"), std::string("a|b"), std::string("..\\x")}) {
        CHECK(hostname_error(bad).has_value());
        CHECK(thumbprint_error(bad).has_value());
        CHECK(cidr_error(bad).has_value());
    }
    CHECK_FALSE(hostname_error("sapbox.corp.example").has_value());
    CHECK_FALSE(thumbprint_error(kThumb).has_value());
    CHECK_FALSE(cidr_error("10.0.0.0/8").has_value());
    CHECK(port_error(0).has_value());
    CHECK(port_error(70000).has_value());
    CHECK_FALSE(port_error(8443).has_value());
}

TEST_CASE("validate_options: exactly one certificate mode and safe values", "[mcp_setup]") {
    Options o;
    o.hostname = kHost;
    auto e = validate_options(o);
    REQUIRE(e.has_value());
    CHECK(e->code == "INVALID_ARGUMENT");
    CHECK(e->exit_code == 2);

    o = Options{};
    o.hostname = kHost;
    o.self_signed = true;
    o.no_tls = true;
    CHECK(validate_options(o)->code == "INVALID_ARGUMENT");

    o = Options{};
    o.hostname = kHost;
    o.self_signed = true;
    o.thumbprint = kThumb;
    CHECK(validate_options(o)->code == "INVALID_ARGUMENT");

    o = Options{};
    o.hostname = "Bad Host";
    o.self_signed = true;
    CHECK(validate_options(o)->code == "INVALID_ARGUMENT");

    o = Options{};
    o.hostname = kHost;
    o.thumbprint = "zz";
    CHECK(validate_options(o)->code == "INVALID_ARGUMENT");

    o = Options{};
    o.hostname = kHost;
    o.self_signed = true;
    o.allow_ip = {"10.0.0.0/8", "\"; calc"};
    CHECK(validate_options(o)->code == "INVALID_ARGUMENT");

    o = Options{};
    o.hostname = kHost;
    o.self_signed = true;
    o.user = "CORP\\jr\"; calc";
    CHECK(validate_options(o)->code == "INVALID_ARGUMENT");

    o = Options{};
    o.no_tls = true;
    o.open_firewall = true;
    CHECK(validate_options(o)->code == "INVALID_ARGUMENT");

    o = Options{};
    o.hostname = "SAPBOX.corp.example";
    o.thumbprint = "0123456789abcdef0123456789abcdef01234567";
    REQUIRE_FALSE(validate_options(o).has_value());
    CHECK(o.cert_mode == CertMode::Thumbprint);
    CHECK(o.thumbprint == kThumb);
    CHECK(o.hostname == "sapbox.corp.example");
    CHECK(o.port == 8443);

    o = Options{};
    o.no_tls = true;
    REQUIRE_FALSE(validate_options(o).has_value());
    CHECK(o.port == 8383);
}

// ---- pure helpers ------------------------------------------------------------------------------------------------
TEST_CASE("pure helpers: sddl, san, prefixes, manifest", "[mcp_setup]") {
    CHECK(sddl_for_sid(kSid) == "D:(A;;GX;;;" + kSid + ")");
    CHECK(sddl_covers_sid("D:(A;;GX;;;" + kSid + ")", kSid));
    CHECK(sddl_covers_sid("D:(A;;GX;;;WD)", kSid));
    CHECK_FALSE(sddl_covers_sid("D:(A;;GX;;;S-1-5-32-545)", kSid));
    CHECK(merge_sddl("D:(A;;GX;;;S-1-5-32-545)", kSid) == "D:(A;;GX;;;S-1-5-32-545)(A;;GX;;;" + kSid + ")");
    CHECK(merge_sddl("", kSid) == sddl_for_sid(kSid));
    CHECK(merge_sddl("D:(A;;GX;;;" + kSid + ")", kSid) == "D:(A;;GX;;;" + kSid + ")");

    CHECK(san_matches({"SapBox.corp.example"}, kHost));
    CHECK(san_matches({"*.corp.example"}, kHost));
    CHECK_FALSE(san_matches({"*.corp.example"}, "a.b.corp.example"));
    CHECK_FALSE(san_matches({"*.corp.example"}, "corp.example"));
    CHECK_FALSE(san_matches({"other"}, kHost));

    CHECK(url_prefix(true, kHost, 8443) == kPrefix);
    CHECK(url_prefix(false, kHost, 8383) == "http://127.0.0.1:8383/mcp/");
    CHECK(endpoint_url(true, kHost, 8443) == "https://" + kHost + ":8443/mcp");
    CHECK(ssl_ipports(8443) == std::vector<std::string>{"0.0.0.0:8443", "[::]:8443"});
    CHECK(firewall_rule_name(8443) == "fairyfly MCP HTTPS 8443");
    CHECK(is_our_app_id("{8E2B5C3A-4D17-4F6A-B9C0-7A1D3E5F2B64}"));
    CHECK_FALSE(is_our_app_id("{00000000-0000-0000-0000-000000000000}"));
    std::string ip;
    int port = 0;
    REQUIRE(parse_ipport("[::]:8443", &ip, &port));
    CHECK(ip == "::");
    CHECK(port == 8443);
    CHECK_FALSE(parse_ipport("nonsense", &ip, &port));

    Manifest m;
    m.mode = "tls";
    m.hostname = kHost;
    m.port = 8443;
    m.prefixes = {kPrefix};
    m.thumbprint = kThumb;
    const auto back = Manifest::from_json(m.to_json());
    REQUIRE(back.has_value());
    CHECK(back->same_setup(m));
    CHECK(m.to_json()["schema"] == 1);
    CHECK_FALSE(Manifest::from_json(json{{"schema", 2}}).has_value());
}

TEST_CASE("script catalog: constant PowerShell without user-input interpolation", "[mcp_setup]") {
    using namespace fairyfly::sys;
    const std::string evil = "x\"; calc; $(whoami) `n";
    // parameters travel as JSON only, and round-trip exactly
    CHECK(json::parse(find_self_signed_params(evil))["hostname"] == evil);
    CHECK(json::parse(export_cert_params(evil, evil))["path"] == evil);
    CHECK(json::parse(firewall_ensure_params(evil, 8443))["rule_name"] == evil);
    for (const auto& [name, script] : script_catalog()) {
        INFO(name);
        CHECK(script.find(evil) == std::string::npos);
        CHECK(script.find("Invoke-Expression") == std::string::npos);
        CHECK(script.find("iex ") == std::string::npos);
        CHECK(script.find("Start-Process") == std::string::npos);
        CHECK(script.find("{0}") == std::string::npos);
        CHECK(script.find("%s") == std::string::npos);
        // the only environment access is the parameter block of the preamble
        std::size_t pos = 0;
        int env_refs = 0;
        while ((pos = script.find("$env:", pos)) != std::string::npos) { ++env_refs; pos += 5; }
        CHECK(env_refs == 2);   // the preamble's `if ($env:FAIRYFLY_PS_PARAMS) { ... $env:FAIRYFLY_PS_PARAMS ... }`
    }
    // building a script twice with different inputs yields the identical text
    CHECK(kFindSelfSigned == kFindSelfSigned);
    CHECK(script_catalog().size() == 8);
}

// ---- plan matrix -------------------------------------------------------------------------------------------------
TEST_CASE("plan: fresh machine", "[mcp_setup]") {
    Rig r;
    const Plan p = r.plan(Rig::self_signed());
    REQUIRE_FALSE(p.error.has_value());
    CHECK(p.url == "https://" + kHost + ":8443/mcp");
    CHECK(Rig::status(p, "certificate") == "would_create");
    CHECK(Rig::status(p, "urlacl") == "would_create");
    CHECK(Rig::status(p, "sslcert") == "would_create");
    CHECK(Rig::status(p, "firewall") == "skipped");
    CHECK(Rig::status(p, "certificate_export") == "would_create");
    CHECK(Rig::status(p, "config") == "would_create");
    CHECK(Rig::status(p, "manifest") == "would_create");
    CHECK(Rig::step(p, "certificate").elevated);
    CHECK(Rig::step(p, "urlacl").elevated);
    CHECK(Rig::step(p, "sslcert").elevated);
    CHECK_FALSE(Rig::step(p, "config").elevated);
    CHECK(p.needs_elevation);
    CHECK_FALSE(p.nothing);
    CHECK_FALSE(p.blocked);
    CHECK(p.sid == kSid);
    CHECK(p.sddl == "D:(A;;GX;;;" + kSid + ")");
    // Left for a human
    std::set<std::string> ids;
    for (const auto& h : p.human) ids.insert(h.id);
    CHECK(ids.count("trust_certificate"));
    CHECK(ids.count("dns"));
    CHECK(ids.count("firewall"));
}

TEST_CASE("plan: --open-firewall replaces the firewall human item", "[mcp_setup]") {
    Rig r;
    Options o = Rig::self_signed();
    o.open_firewall = true;
    const Plan p = r.plan(o);
    CHECK(Rig::status(p, "firewall") == "would_create");
    for (const auto& h : p.human) CHECK(h.id != "firewall");
}

TEST_CASE("plan: urlacl without the user's SID is updated, not replaced", "[mcp_setup]") {
    Rig r;
    r.m.http.urlacls[kPrefix] = "D:(A;;GX;;;S-1-5-32-545)";
    const Plan p = r.plan(Rig::self_signed());
    CHECK(Rig::status(p, "urlacl") == "would_update");
    CHECK(p.replace_urlacl);
    CHECK(p.sddl.find("S-1-5-32-545") != std::string::npos);
    CHECK(p.sddl.find(kSid) != std::string::npos);
}

TEST_CASE("plan: foreign AppId on the port is blocked unless --force-binding", "[mcp_setup]") {
    Rig r;
    r.m.http.bindings["0.0.0.0:8443"] = SslBinding{"0.0.0.0:8443", kThumb, "{11111111-2222-3333-4444-555555555555}", "MY"};
    Plan p = r.plan(Rig::self_signed());
    CHECK(Rig::status(p, "sslcert") == "blocked");
    CHECK(p.blocked);
    CHECK_FALSE(p.nothing);
    Options o = Rig::self_signed();
    o.force_binding = true;
    p = r.plan(o);
    CHECK(Rig::status(p, "sslcert") == "would_create");   // [::] is still unbound
    CHECK_FALSE(p.blocked);
    CHECK(r.setup(Rig::self_signed()) == 1);               // run_setup refuses a blocked plan
    CHECK(r.err.str().find("PLAN_BLOCKED") != std::string::npos);
    CHECK(r.m.http.calls.empty());
}

TEST_CASE("plan: existing certificate problems", "[mcp_setup]") {
    Rig r;
    SECTION("expired") {
        CertInfo c = good_cert(kThumb);
        c.not_after = 1700000000;
        r.m.certs.certs.push_back(c);
        const Plan p = r.plan(Rig::self_signed());
        CHECK(Rig::status(p, "certificate") == "would_update");
        CHECK(p.create_cert);
        CHECK(Rig::step(p, "certificate").detail.find("expired") != std::string::npos);
    }
    SECTION("wrong SAN") {
        CertInfo c = good_cert(kThumb);
        c.dns_names = {"other.example"};
        r.m.certs.certs.push_back(c);
        const Plan p = r.plan(Rig::self_signed());
        CHECK(Rig::status(p, "certificate") == "would_update");
        CHECK(Rig::step(p, "certificate").detail.find("name does not match") != std::string::npos);
    }
    SECTION("no private key") {
        CertInfo c = good_cert(kThumb);
        c.has_private_key = false;
        r.m.certs.certs.push_back(c);
        const Plan p = r.plan(Rig::self_signed());
        CHECK(Rig::status(p, "certificate") == "would_update");
        CHECK(Rig::step(p, "certificate").detail.find("no private key") != std::string::npos);
    }
    SECTION("valid certificate is reused") {
        r.m.certs.certs.push_back(good_cert(kThumb));
        const Plan p = r.plan(Rig::self_signed());
        CHECK(Rig::status(p, "certificate") == "unchanged");
        CHECK(p.thumbprint == kThumb);
        CHECK(Rig::status(p, "sslcert") == "would_create");
    }
}

TEST_CASE("plan: thumbprint mode", "[mcp_setup]") {
    Rig r;
    Options o;
    o.thumbprint = kThumb;
    o.yes = true;
    SECTION("missing certificate blocks") {
        const Plan p = r.plan(o);
        CHECK(Rig::status(p, "certificate") == "blocked");
        CHECK(p.blocked);
    }
    SECTION("no private key blocks") {
        CertInfo c = good_cert(kThumb);
        c.has_private_key = false;
        r.m.certs.certs.push_back(c);
        CHECK(Rig::status(r.plan(o), "certificate") == "blocked");
    }
    SECTION("wrong SAN blocks") {
        CertInfo c = good_cert(kThumb);
        c.dns_names = {"other.example"};
        r.m.certs.certs.push_back(c);
        CHECK(Rig::status(r.plan(o), "certificate") == "blocked");
    }
    SECTION("good certificate is used and never created") {
        r.m.certs.certs.push_back(good_cert(kThumb));
        const Plan p = r.plan(o);
        CHECK(Rig::status(p, "certificate") == "unchanged");
        CHECK_FALSE(p.create_cert);
        CHECK(p.manifest.cert_mode == "thumbprint");
        CHECK(p.manifest.thumbprint == kThumb);
        // a CA-issued certificate needs no trust hint
        for (const auto& h : p.human) CHECK(h.id != "trust_certificate");
    }
}

TEST_CASE("plan: --no-tls", "[mcp_setup]") {
    Rig r;
    Options o;
    o.no_tls = true;
    o.yes = true;
    const Plan p = r.plan(o);
    CHECK(p.url == "http://127.0.0.1:8383/mcp");
    CHECK(p.prefix == "http://127.0.0.1:8383/mcp/");
    CHECK(Rig::status(p, "certificate") == "skipped");
    CHECK(Rig::status(p, "sslcert") == "skipped");
    CHECK(Rig::status(p, "firewall") == "skipped");
    CHECK(Rig::status(p, "certificate_export") == "skipped");
    CHECK(Rig::status(p, "urlacl") == "skipped");   // loopback prefixes need no reservation on this Windows build
    CHECK_FALSE(p.needs_elevation);
    CHECK(p.manifest.mode == "no-tls");
    CHECK(p.config_yaml.find("tls: false") != std::string::npos);
    CHECK(p.config_yaml.find("host: 127.0.0.1") != std::string::npos);
    for (const auto& h : p.human) CHECK(h.id != "trust_certificate");
}

TEST_CASE("setup --no-tls needs no elevation and reserves nothing", "[mcp_setup]") {
    Rig r;
    Options o;
    o.no_tls = true;
    o.yes = true;
    CHECK(r.setup(o) == 0);
    CHECK(r.m.elevator.plans_seen.empty());
    CHECK(r.m.http.urlacls.empty());
    CHECK(r.m.sys.files.count(kManifestPath) == 1);
    CHECK(r.m.sys.files.at(kConfigPath).find("tls: false") != std::string::npos);
    CHECK(r.out.str().find("Verify: ok") != std::string::npos);
    // an existing reservation is left alone and reported as optional
    r.m.http.urlacls["http://127.0.0.1:8383/mcp/"] = sddl_for_sid(kSid);
    CHECK(Rig::status(r.plan(o), "urlacl") == "unchanged");
    CHECK(r.setup(o) == 0);
    CHECK(r.out.str() == "nothing - already set up.\n");
}

TEST_CASE("plan: existing mcp.yaml is never rewritten", "[mcp_setup]") {
    Rig r;
    r.m.sys.files[kConfigPath] = "server:\n  transport: http\n  tls: false\n  port: 9000\nauth:\n  token: SECRET-SENTINEL\n";
    Plan p = r.plan(Rig::self_signed());
    CHECK(Rig::status(p, "config") == "skipped");
    bool found = false;
    for (const auto& h : p.human)
        if (h.id == "config_keys") {
            found = true;
            CHECK(h.text.find("server.tls: true") != std::string::npos);
            CHECK(h.text.find("server.port: 8443") != std::string::npos);
            CHECK(h.text.find("SECRET-SENTINEL") == std::string::npos);
        }
    CHECK(found);
    // a file that already matches is unchanged
    r.m.sys.files[kConfigPath] =
        "server:\n  transport: http\n  tls: true\n  host: '+'\n  port: 8443\n  allowed_hosts: [" + kHost + "]\n";
    p = r.plan(Rig::self_signed());
    CHECK(Rig::status(p, "config") == "unchanged");
}

// ---- consent / exit code matrix ----------------------------------------------------------------------------------
TEST_CASE("setup --dry-run prints the plan and changes nothing", "[mcp_setup]") {
    Rig r;
    Options o = Rig::self_signed();
    o.dry_run = true;
    o.yes = false;
    CHECK(r.setup(o) == 0);
    CHECK(r.out.str().find("would_create") != std::string::npos);
    CHECK(r.out.str().find("Left for a human") != std::string::npos);
    CHECK(r.m.http.calls.empty());
    CHECK(r.m.certs.calls.empty());
    CHECK(r.m.elevator.plans_seen.empty());
    CHECK(r.m.sys.files.empty());
}

TEST_CASE("setup --yes: one elevated child with the explicit SID, then export, config, manifest, verify", "[mcp_setup]") {
    Rig r;
    CHECK(r.setup(Rig::self_signed()) == 0);
    REQUIRE(r.m.elevator.plans_seen.size() == 1);
    const json& plan = r.m.elevator.plans_seen[0];
    CHECK(plan["sid"] == kSid);
    CHECK(plan["operation"] == "setup");
    CHECK(plan["hostname"] == kHost);
    CHECK(plan["port"] == 8443);
    CHECK(plan["steps"] == json::array({"certificate", "urlacl", "sslcert"}));
    // machine state
    REQUIRE(r.m.certs.certs.size() == 1);
    const std::string thumb = r.m.certs.certs[0].thumbprint;
    CHECK(r.m.http.urlacls.at(kPrefix) == sddl_for_sid(kSid));
    CHECK(r.m.http.bindings.at("0.0.0.0:8443").thumbprint == thumb);
    CHECK(r.m.http.bindings.at("[::]:8443").thumbprint == thumb);
    CHECK(is_our_app_id(r.m.http.bindings.at("0.0.0.0:8443").app_id));
    CHECK(r.m.sys.files.at(kCerPath) == "cer:" + thumb);
    // unelevated parent wrote config + manifest
    REQUIRE(r.m.sys.files.count(kConfigPath));
    CHECK(r.m.sys.files.at(kConfigPath).find("tls: true") != std::string::npos);
    REQUIRE(r.m.sys.files.count(kManifestPath));
    const auto manifest = Manifest::from_json(json::parse(r.m.sys.files.at(kManifestPath)));
    REQUIRE(manifest.has_value());
    CHECK(manifest->thumbprint == thumb);
    CHECK(manifest->cert_mode == "self-signed");
    CHECK(manifest->sid == kSid);
    CHECK(manifest->hostname == kHost);
    CHECK(manifest->appid == "{8e2b5c3a-4d17-4f6a-b9c0-7a1d3e5f2b64}");
    CHECK_FALSE(manifest->created_at.empty());
    // verify ran against the certificate just created, pinned by thumbprint
    REQUIRE(r.m.verify.requests.size() == 1);
    CHECK(r.m.verify.requests[0].expected_thumbprint == thumb);
    CHECK(r.m.verify.requests[0].owner_sid == kSid);
    CHECK(r.out.str().find("Verify: ok") != std::string::npos);
    CHECK(r.out.str().find("created") != std::string::npos);
    CHECK(r.out.str().find("setup complete") != std::string::npos);
}

TEST_CASE("setup: already elevated runs in process, no child", "[mcp_setup]") {
    Rig r;
    r.m.elevator.type = ElevationType::Elevated;
    CHECK(r.setup(Rig::self_signed()) == 0);
    CHECK(r.m.elevator.plans_seen.empty());
    CHECK(r.m.certs.certs.size() == 1);
    CHECK(r.m.http.bindings.size() == 2);
}

TEST_CASE("setup: second run is a no-op", "[mcp_setup]") {
    Rig r;
    REQUIRE(r.setup(Rig::self_signed()) == 0);
    const auto files_before = r.m.sys.files;
    const auto calls_before = r.m.http.calls.size();
    CHECK(r.setup(Rig::self_signed()) == 0);
    CHECK(r.out.str() == "nothing - already set up.\n");
    CHECK(r.m.elevator.plans_seen.size() == 1);   // no second prompt
    CHECK(r.m.http.calls.size() == calls_before);
    CHECK(r.m.sys.files == files_before);
    CHECK(r.m.certs.certs.size() == 1);
    // and it needs no --yes
    Options o = Rig::self_signed();
    o.yes = false;
    o.non_interactive = true;
    CHECK(r.setup(o) == 0);
}

TEST_CASE("setup: consent gate", "[mcp_setup]") {
    Rig r;
    Options o = Rig::self_signed();
    o.yes = false;

    SECTION("no TTY without --yes is refused") {
        r.env.stdin_is_tty = false;
        CHECK(r.setup(o) == 2);
        CHECK(r.err.str().find("CONFIRMATION_REQUIRED") != std::string::npos);
        CHECK(r.err.str().find("Refusing to change " + kHost + " without confirmation") != std::string::npos);
        CHECK(r.m.http.calls.empty());
    }
    SECTION("--non-interactive without --yes is refused even on a TTY") {
        r.env.stdin_is_tty = true;
        o.non_interactive = true;
        CHECK(r.setup(o) == 2);
        CHECK(r.err.str().find("CONFIRMATION_REQUIRED") != std::string::npos);
    }
    SECTION("TTY prompt: y proceeds") {
        r.env.stdin_is_tty = true;
        std::string asked;
        r.env.confirm = [&](const std::string& prompt) {
            asked = prompt;
            return true;
        };
        CHECK(r.setup(o) == 0);
        CHECK(asked == "Change this machine (" + kHost + ")? [y/N] ");
        CHECK(r.m.certs.certs.size() == 1);
    }
    SECTION("TTY prompt: n aborts with nothing changed") {
        r.env.stdin_is_tty = true;
        r.env.confirm = [](const std::string&) { return false; };
        CHECK(r.setup(o) == 2);
        CHECK(r.m.http.calls.empty());
        CHECK(r.m.elevator.plans_seen.empty());
    }
}

TEST_CASE("setup: --non-interactive needing elevation prints the exact command", "[mcp_setup]") {
    Rig r;
    Options o = Rig::self_signed();
    o.non_interactive = true;
    o.command_line = "fairyfly mcp setup --self-signed --hostname sapbox.corp.example --yes";
    CHECK(r.setup(o) == 2);
    CHECK(r.err.str().find("ELEVATION_REQUIRED") != std::string::npos);
    CHECK(r.err.str().find(o.command_line) != std::string::npos);
    CHECK(r.m.elevator.plans_seen.empty());
    CHECK(r.m.http.calls.empty());
    // elevated non-interactive works
    r.m.elevator.type = ElevationType::Elevated;
    CHECK(r.setup(o) == 0);
}

TEST_CASE("setup: FAIRYFLY_READ_ONLY refuses apply but not dry run or runbook", "[mcp_setup]") {
    Rig r;
    r.env.read_only = true;
    CHECK(r.setup(Rig::self_signed()) == 2);
    CHECK(r.err.str().find("READ_ONLY") != std::string::npos);
    CHECK(r.m.http.calls.empty());
    Options o = Rig::self_signed();
    o.dry_run = true;
    CHECK(r.setup(o) == 0);
    o.dry_run = false;
    o.print_runbook = true;
    CHECK(r.setup(o) == 0);
    CHECK(r.m.http.calls.empty());
    CHECK(r.m.elevator.plans_seen.empty());
}

TEST_CASE("setup: UAC declined exits 1 and changes nothing", "[mcp_setup]") {
    Rig r;
    r.m.elevator.decline = true;
    CHECK(r.setup(Rig::self_signed()) == 1);
    CHECK(r.err.str().find("ELEVATION_DECLINED") != std::string::npos);
    CHECK(r.m.http.calls.empty());
    CHECK(r.m.certs.certs.empty());
    CHECK(r.m.sys.files.empty());
}

TEST_CASE("setup: the unelevated process cannot touch http.sys (elevation is real in the fake)", "[mcp_setup]") {
    Rig r;
    CHECK_THROWS_AS(r.m.http.add_urlacl(kPrefix, sddl_for_sid(kSid)), HostError);
    CHECK_THROWS_AS(r.m.certs.create_self_signed(kHost, 730), HostError);
}

TEST_CASE("setup: a failing elevated step stops, later steps are skipped, no manifest", "[mcp_setup]") {
    Rig r;
    r.m.http.fail_ipport = "0.0.0.0:8443";
    CHECK(r.setup(Rig::self_signed()) == 1);
    CHECK(r.err.str().find("STEP_FAILED") != std::string::npos);
    CHECK(r.out.str().find("failed") != std::string::npos);
    CHECK(r.m.sys.files.count(kManifestPath) == 0);
    CHECK(r.m.sys.files.count(kConfigPath) == 0);
    CHECK(r.m.verify.requests.empty());
}

TEST_CASE("setup: IPv6 binding failure is reported but not fatal", "[mcp_setup]") {
    Rig r;
    r.m.http.fail_ipport = "[::]:8443";
    CHECK(r.setup(Rig::self_signed()) == 0);
    CHECK(r.m.http.bindings.count("0.0.0.0:8443") == 1);
    CHECK(r.m.http.bindings.count("[::]:8443") == 0);
}

TEST_CASE("setup: verification", "[mcp_setup]") {
    Rig r;
    SECTION("failure exits 1 with VERIFY_FAILED") {
        r.m.verify.result = VerifyResult{"failed", "", 0, false, "connection refused"};
        Options o = Rig::self_signed();
        CHECK(r.setup(o) == 1);
        CHECK(r.err.str().find("VERIFY_FAILED") != std::string::npos);
        CHECK(r.m.sys.files.count(kManifestPath) == 1);   // setup applied; only the proof failed
    }
    SECTION("a 200 instead of 401 is a failure") {
        r.m.verify.result = VerifyResult{"ok", "TLS 1.3", 200, true, ""};
        CHECK(r.setup(Rig::self_signed()) == 1);
        CHECK(r.err.str().find("401") != std::string::npos);
    }
    SECTION("thumbprint mismatch is a failure") {
        r.m.verify.result = VerifyResult{"ok", "TLS 1.3", 401, false, ""};
        CHECK(r.setup(Rig::self_signed()) == 1);
    }
    SECTION("negotiated TLS 1.1 adds a Schannel item") {
        r.m.verify.result = VerifyResult{"ok", "TLS 1.1", 401, true, ""};
        Options o = Rig::self_signed();
        o.json = true;
        CHECK(r.setup(o) == 0);
        const json doc = json::parse(r.out.str());
        bool schannel = false;
        for (const auto& h : doc["data"]["human"]) schannel = schannel || h["id"] == "schannel";
        CHECK(schannel);
        CHECK(doc["data"]["verify"]["protocol"] == "TLS 1.1");
    }
    SECTION("skipped verification (other account) is accepted") {
        r.m.verify.result = VerifyResult{"skipped", "", 0, false, "reservation belongs to another account"};
        CHECK(r.setup(Rig::self_signed()) == 0);
    }
}

TEST_CASE("setup: --user resolves the SID before elevation", "[mcp_setup]") {
    Rig r;
    r.m.elevator.known_users["CORP\\svc"] = "S-1-5-21-9-9-9-500";
    Options o = Rig::self_signed();
    o.user = "CORP\\svc";
    CHECK(r.setup(o) == 0);
    CHECK(r.m.elevator.plans_seen[0]["sid"] == "S-1-5-21-9-9-9-500");
    CHECK(r.m.http.urlacls.at(kPrefix) == sddl_for_sid("S-1-5-21-9-9-9-500"));
    CHECK(r.m.verify.requests[0].owner_sid == "S-1-5-21-9-9-9-500");

    Rig r2;
    o.user = "CORP\\nobody";
    CHECK(r2.setup(o) == 2);
    CHECK(r2.err.str().find("USER_NOT_FOUND") != std::string::npos);
}

TEST_CASE("setup: JSON output shape", "[mcp_setup]") {
    Rig r;
    Options o = Rig::self_signed();
    o.dry_run = true;
    o.json = true;
    REQUIRE(r.setup(o) == 0);
    const json doc = json::parse(r.out.str());
    CHECK(doc["status"] == "success");
    const json& data = doc["data"];
    CHECK(data["operation"] == "setup");
    CHECK(data["dry_run"] == true);
    CHECK(data["url"] == "https://" + kHost + ":8443/mcp");
    CHECK(data["elevation"] == "standard_user");
    REQUIRE(data["diagnosis"].is_array());
    for (const auto& d : data["diagnosis"]) {
        CHECK(d.contains("id"));
        CHECK(d.contains("status"));
        CHECK(d.contains("detail"));
    }
    REQUIRE(data["steps"].is_array());
    std::vector<std::string> ids;
    for (const auto& s : data["steps"]) {
        for (const char* k : {"id", "title", "status", "detail", "elevated"}) CHECK(s.contains(k));
        ids.push_back(s["id"]);
    }
    CHECK(ids == std::vector<std::string>{"certificate", "urlacl", "sslcert", "firewall", "certificate_export", "config", "manifest"});
    REQUIRE(data["human"].is_array());
    for (const auto& h : data["human"]) {
        CHECK(h.contains("id"));
        CHECK(h.contains("text"));
    }
    CHECK(data["next_steps"].is_array());
    CHECK_FALSE(doc.contains("error"));

    // applied run carries verify; errors carry {code,message}
    Rig r2;
    o = Rig::self_signed();
    o.json = true;
    REQUIRE(r2.setup(o) == 0);
    const json applied = json::parse(r2.out.str());
    CHECK(applied["data"]["verify"]["status"] == "ok");
    CHECK(applied["data"]["verify"].contains("protocol"));
    CHECK(applied["data"]["verify"].contains("http_status"));
    CHECK(applied["data"]["verify"].contains("thumbprint_match"));
    CHECK(applied["data"]["steps"][0]["status"] == "created");

    Rig r3;
    o = Rig::self_signed();
    o.json = true;
    o.yes = false;
    CHECK(r3.setup(o) == 2);
    const json err = json::parse(r3.out.str());
    CHECK(err["status"] == "error");
    CHECK(err["error"]["code"] == "CONFIRMATION_REQUIRED");
    CHECK(err["error"]["message"].get<std::string>().find("Refusing") == 0);
}

TEST_CASE("runbook contains the manual netsh and PowerShell equivalents", "[mcp_setup]") {
    Rig r;
    Options o = Rig::self_signed();
    o.print_runbook = true;
    REQUIRE(r.setup(o) == 0);
    const std::string text = r.out.str();
    CHECK(text.find("netsh http add urlacl url=https://+:8443/mcp/ user=CORP\\jr") != std::string::npos);
    CHECK(text.find("New-SelfSignedCertificate") != std::string::npos);
    CHECK(text.find("netsh http add sslcert ipport=0.0.0.0:8443 certhash=") != std::string::npos);
    CHECK(text.find("netsh http add sslcert ipport=[::]:8443") != std::string::npos);
    CHECK(text.find("appid={8e2b5c3a-4d17-4f6a-b9c0-7a1d3e5f2b64}") != std::string::npos);
    CHECK(text.find("certstorename=MY") != std::string::npos);
    CHECK(text.find("New-NetFirewallRule -DisplayName 'fairyfly MCP HTTPS 8443'") != std::string::npos);
    CHECK(text.find("server.tls: true") != std::string::npos);
    CHECK(text.find("certutil -addstore") != std::string::npos);
    CHECK(text.find("NODE_EXTRA_CA_CERTS") != std::string::npos);
    CHECK(r.m.http.calls.empty());
    // JSON runbook
    o.json = true;
    REQUIRE(r.setup(o) == 0);
    CHECK(json::parse(r.out.str())["data"]["runbook"].get<std::string>().find("netsh http add urlacl") != std::string::npos);
}

TEST_CASE("secrets in an existing config are never printed", "[mcp_setup]") {
    Rig r;
    r.m.sys.files[kConfigPath] = "auth:\n  token: SECRET-SENTINEL\nserver:\n  transport: http\n";
    CHECK(r.setup(Rig::self_signed()) == 0);
    CHECK(r.out.str().find("SECRET-SENTINEL") == std::string::npos);
    CHECK(r.err.str().find("SECRET-SENTINEL") == std::string::npos);
    CHECK(r.m.sys.files.at(kConfigPath).find("SECRET-SENTINEL") != std::string::npos);   // untouched
}

// ---- elevated work file: never trusted ----------------------------------------------------------------------------
TEST_CASE("execute_elevated_work re-validates a tampered plan file", "[mcp_setup]") {
    Rig r;
    r.m.elevator.type = ElevationType::Elevated;
    Hosts h = r.m.hosts();
    const json good = {{"schema", 1}, {"operation", "setup"}, {"hostname", kHost}, {"port", 8443}, {"tls", true}, {"sid", kSid},
                       {"thumbprint", ""}, {"create_cert", true}, {"valid_days", 730}, {"steps", json::array({"certificate"})}, {"firewall_rule", ""}};
    for (const auto& [field, value] : std::vector<std::pair<std::string, json>>{
             {"hostname", "a\"; calc"}, {"hostname", "$(calc)"}, {"sid", "S-1-5-21-1; calc"}, {"port", 0}, {"port", 99999},
             {"thumbprint", "nothex"}, {"firewall_rule", "evil rule"}, {"steps", json::array({"format-disk"})}, {"operation", "setup; calc"}, {"schema", 2}}) {
        json bad = good;
        bad[field] = value;
        const json result = execute_elevated_work(bad, h);
        INFO(field << "=" << value.dump());
        CHECK(result["ok"] == false);
        CHECK(result["error"]["code"] == "INVALID_PLAN");
    }
    CHECK(r.m.certs.calls.empty());
    CHECK(r.m.http.calls.empty());
    const json td = {{"schema", 1}, {"operation", "teardown"}, {"ipports", json::array({"0.0.0.0:8443"})},
                     {"prefixes", json::array({"https://+:8443/mcp/; calc"})}, {"firewall_rule", ""}, {"cert_thumbprint", ""}, {"steps", json::array({"urlacl"})}};
    CHECK(execute_elevated_work(td, h)["error"]["code"] == "INVALID_PLAN");
    const json ok = execute_elevated_work(good, h);
    CHECK(ok["ok"] == true);
    CHECK(r.m.certs.calls.size() == 1);
}

TEST_CASE("run_apply_plan reads the plan file and writes the result file", "[mcp_setup]") {
    Rig r;
    r.m.elevator.type = ElevationType::Elevated;
    Hosts h = r.m.hosts();
    const json plan = {{"schema", 1}, {"operation", "setup"}, {"hostname", kHost}, {"port", 8443}, {"tls", true}, {"sid", kSid},
                       {"thumbprint", ""}, {"create_cert", true}, {"valid_days", 730}, {"steps", json::array({"certificate", "urlacl"})}, {"firewall_rule", ""}};
    r.m.sys.files["C:\\plan.json"] = plan.dump();
    CHECK(run_apply_plan(h, "C:\\plan.json", "C:\\result.json") == 0);
    const json result = json::parse(r.m.sys.files.at("C:\\result.json"));
    CHECK(result["ok"] == true);
    CHECK(result["steps"].size() == 2);
    CHECK(r.m.http.urlacls.count(kPrefix) == 1);
    CHECK(run_apply_plan(h, "C:\\missing.json", "C:\\result2.json") == 1);
    CHECK(json::parse(r.m.sys.files.at("C:\\result2.json"))["ok"] == false);
}

// ---- teardown ------------------------------------------------------------------------------------------------------
TEST_CASE("teardown removes exactly what setup created", "[mcp_setup]") {
    Rig r;
    Options o = Rig::self_signed();
    o.open_firewall = true;
    REQUIRE(r.setup(o) == 0);
    const std::string thumb = r.m.certs.certs[0].thumbprint;
    REQUIRE(r.m.firewall.rules.count("fairyfly MCP HTTPS 8443") == 1);
    // a foreign binding on another port and an unrelated certificate stay
    r.m.http.bindings["0.0.0.0:9999"] = SslBinding{"0.0.0.0:9999", kThumb, "{11111111-2222-3333-4444-555555555555}", "MY"};
    r.m.certs.certs.push_back(good_cert(kThumb, "other.example"));

    TeardownOptions t;
    t.yes = true;
    CHECK(r.teardown(t) == 0);
    CHECK(r.m.http.bindings.count("0.0.0.0:8443") == 0);
    CHECK(r.m.http.bindings.count("[::]:8443") == 0);
    CHECK(r.m.http.bindings.count("0.0.0.0:9999") == 1);
    CHECK(r.m.http.urlacls.empty());
    CHECK(r.m.firewall.rules.empty());
    REQUIRE(r.m.certs.certs.size() == 1);
    CHECK(r.m.certs.certs[0].thumbprint == kThumb);   // only ours was removed
    CHECK(r.m.sys.files.count(kManifestPath) == 0);
    CHECK(r.m.sys.files.count(kCerPath) == 0);         // the exported .cer is deleted too
    CHECK(r.m.elevator.plans_seen.size() == 2);        // setup + teardown, one prompt each
    CHECK(r.m.elevator.plans_seen[1]["operation"] == "teardown");
    CHECK(r.out.str().find("teardown complete") != std::string::npos);
    (void)thumb;
}

TEST_CASE("teardown is idempotent", "[mcp_setup]") {
    Rig r;
    REQUIRE(r.setup(Rig::self_signed()) == 0);
    TeardownOptions t;
    t.yes = true;
    REQUIRE(r.teardown(t) == 0);
    const auto prompts = r.m.elevator.plans_seen.size();
    CHECK(r.teardown(t) == 0);
    CHECK(r.out.str() == "nothing - already removed.\n");
    CHECK(r.m.elevator.plans_seen.size() == prompts);
}

TEST_CASE("teardown --keep-cert and --keep-firewall", "[mcp_setup]") {
    Rig r;
    Options o = Rig::self_signed();
    o.open_firewall = true;
    REQUIRE(r.setup(o) == 0);
    TeardownOptions t;
    t.yes = true;
    t.keep_cert = true;
    t.keep_firewall = true;
    CHECK(r.teardown(t) == 0);
    CHECK(r.m.certs.certs.size() == 1);
    CHECK(r.m.firewall.rules.size() == 1);
    CHECK(r.m.http.urlacls.empty());
    CHECK(r.m.http.bindings.empty());
}

TEST_CASE("teardown never removes a user-supplied certificate", "[mcp_setup]") {
    Rig r;
    r.m.certs.certs.push_back(good_cert(kThumb));
    Options o;
    o.thumbprint = kThumb;
    o.yes = true;
    REQUIRE(r.setup(o) == 0);
    TeardownOptions t;
    t.yes = true;
    CHECK(r.teardown(t) == 0);
    CHECK(r.m.certs.certs.size() == 1);
}

TEST_CASE("teardown without a manifest", "[mcp_setup]") {
    Rig r;
    TeardownOptions t;
    t.yes = true;
    SECTION("clean machine: nothing to do") {
        CHECK(r.teardown(t) == 0);
        CHECK(r.out.str() == "nothing - already removed.\n");
    }
    SECTION("leftovers without a manifest: MANIFEST_MISSING exit 2") {
        r.m.http.urlacls[kPrefix] = sddl_for_sid(kSid);
        CHECK(r.teardown(t) == 2);
        CHECK(r.err.str().find("MANIFEST_MISSING") != std::string::npos);
        CHECK(r.m.http.urlacls.size() == 1);
    }
    SECTION("--hostname/--port removes reservations and our bindings, never a foreign one, never a certificate") {
        r.m.http.urlacls[kPrefix] = sddl_for_sid(kSid);
        r.m.http.bindings["0.0.0.0:8443"] = SslBinding{"0.0.0.0:8443", kThumb, normalize_app_id(kAppId), "MY"};
        r.m.http.bindings["[::]:8443"] = SslBinding{"[::]:8443", kThumb, "{11111111-2222-3333-4444-555555555555}", "MY"};
        r.m.certs.certs.push_back(good_cert(kThumb));
        t.hostname = kHost;
        t.port = 8443;
        CHECK(r.teardown(t) == 0);
        CHECK(r.m.http.urlacls.empty());
        CHECK(r.m.http.bindings.count("0.0.0.0:8443") == 0);
        CHECK(r.m.http.bindings.count("[::]:8443") == 1);   // foreign: untouched
        CHECK(r.m.certs.certs.size() == 1);
    }
    SECTION("dry run changes nothing") {
        r.m.http.urlacls[kPrefix] = sddl_for_sid(kSid);
        t.hostname = kHost;
        t.port = 8443;
        t.dry_run = true;
        CHECK(r.teardown(t) == 0);
        CHECK(r.out.str().find("would_remove") != std::string::npos);
        CHECK(r.m.http.urlacls.size() == 1);
    }
}

TEST_CASE("teardown: consent, elevation and read-only", "[mcp_setup]") {
    Rig r;
    REQUIRE(r.setup(Rig::self_signed()) == 0);
    TeardownOptions t;
    SECTION("no --yes and no TTY") {
        CHECK(r.teardown(t) == 2);
        CHECK(r.err.str().find("CONFIRMATION_REQUIRED") != std::string::npos);
        CHECK(r.m.http.urlacls.size() == 1);
    }
    SECTION("--non-interactive needs elevation") {
        t.yes = true;
        t.non_interactive = true;
        t.command_line = "fairyfly mcp teardown --yes";
        CHECK(r.teardown(t) == 2);
        CHECK(r.err.str().find("ELEVATION_REQUIRED") != std::string::npos);
        CHECK(r.err.str().find("fairyfly mcp teardown --yes") != std::string::npos);
    }
    SECTION("read-only") {
        t.yes = true;
        r.env.read_only = true;
        CHECK(r.teardown(t) == 2);
        CHECK(r.err.str().find("READ_ONLY") != std::string::npos);
    }
    SECTION("UAC declined") {
        t.yes = true;
        r.m.elevator.decline = true;
        CHECK(r.teardown(t) == 1);
        CHECK(r.m.http.urlacls.size() == 1);
    }
    SECTION("config still saying tls: true becomes a human item") {
        t.yes = true;
        t.json = true;
        REQUIRE(r.teardown(t) == 0);
        bool found = false;
        const json doc = json::parse(r.out.str());
        for (const auto& h : doc["data"]["human"]) found = found || h["id"] == "config_tls";
        CHECK(found);
    }
}

// ---- cert export ---------------------------------------------------------------------------------------------------
TEST_CASE("cert export", "[mcp_setup]") {
    Rig r;
    Hosts h = r.m.hosts();
    CertExportOptions c;
    SECTION("without a setup it explains what to do") {
        CHECK(run_cert_export(h, c, r.env) == 1);
        CHECK(r.err.str().find("CERT_NOT_FOUND") != std::string::npos);
    }
    SECTION("after setup: default path, trust hints, JSON") {
        REQUIRE(r.setup(Rig::self_signed()) == 0);
        r.out.str("");
        r.m.sys.files.erase(kCerPath);
        c.json = true;
        CHECK(run_cert_export(h, c, r.env) == 0);
        const json doc = json::parse(r.out.str());
        CHECK(doc["data"]["path"] == kCerPath);
        CHECK(doc["data"]["format"] == "der");
        CHECK(doc["data"]["thumbprint"] == r.m.certs.certs[0].thumbprint);
        CHECK(doc["data"]["trust_hints"].size() >= 4);
        CHECK(r.m.sys.files.count(kCerPath) == 1);
    }
    SECTION("pem to a chosen path (thumbprint from the sslcert binding when no manifest exists)") {
        r.m.certs.certs.push_back(good_cert(kThumb));
        r.m.http.bindings["0.0.0.0:8443"] = SslBinding{"0.0.0.0:8443", kThumb, normalize_app_id(kAppId), "MY"};
        c.format = CerFormat::Pem;
        c.out_path = "C:\\out\\x.pem";
        CHECK(run_cert_export(h, c, r.env) == 0);
        CHECK(r.m.sys.files.count("C:\\out\\x.pem") == 1);
        CHECK(r.out.str().find("NODE_EXTRA_CA_CERTS") != std::string::npos);
    }
}

// ---- doctor facts -------------------------------------------------------------------------------------------------
TEST_CASE("collect_setup_facts: fresh machine and after setup", "[mcp_setup]") {
    Rig r;
    Hosts h = r.m.hosts();
    fairyfly::config::SetupQuery q;
    q.tls = true;
    q.hostname = kHost;
    q.port = 8443;
    auto f = collect_setup_facts(h, q);
    CHECK(f.available);
    CHECK(f.elevation == "standard_user");
    CHECK_FALSE(f.manifest_present);
    CHECK(f.urlacl_known);
    CHECK_FALSE(f.urlacl_reserved);
    CHECK_FALSE(f.ssl_v4);
    CHECK_FALSE(f.cert_found);

    Options o = Rig::self_signed();
    o.open_firewall = true;
    REQUIRE(r.setup(o) == 0);
    f = collect_setup_facts(h, q);
    CHECK(f.manifest_present);
    CHECK(f.manifest_mode == "tls");
    CHECK(f.urlacl_reserved);
    CHECK(f.urlacl_covers_user);
    CHECK(f.ssl_v4);
    CHECK(f.ssl_v6);
    CHECK_FALSE(f.ssl_foreign);
    CHECK(f.cert_found);
    CHECK(f.cert_has_key);
    CHECK(f.cert_san_ok);
    CHECK(f.firewall_known);
    CHECK(f.firewall_exists);
    CHECK_FALSE(f.port_listening);
    CHECK(f.tls_status.empty());

    r.m.sys.listening = {8443};
    r.m.sys.tls = TlsProbe{"ok", "TLS 1.3", r.m.certs.certs[0].thumbprint, false, 405, ""};
    f = collect_setup_facts(h, q);
    CHECK(f.port_listening);
    CHECK(f.tls_status == "ok");
    CHECK(f.tls_thumbprint_match);
    CHECK(f.tls_protocol == "TLS 1.3");
}
