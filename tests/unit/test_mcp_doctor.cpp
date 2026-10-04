#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "include/auth/secret_backend.h"
#include "include/config/mcp_doctor.h"

using namespace fairyfly::config;

TEST_CASE("mcp doctor distinguishes SAP logon screens from authenticated sessions", "[mcp_doctor]") {
    using Tri = SapState::Tri;
    CHECK(classify_logged_in_users({}, true) == Tri::No);
    CHECK(classify_logged_in_users({std::string{}}, true) == Tri::No);
    CHECK(classify_logged_in_users({std::string{}, std::string{"DEVELOPER"}}, true) == Tri::Yes);
    CHECK(classify_logged_in_users({std::nullopt}, true) == Tri::Unknown);
    CHECK(classify_logged_in_users({std::string{}}, false) == Tri::Unknown);
    CHECK(classify_logged_in_users({std::string{"DEVELOPER"}}, false) == Tri::Yes);
    CHECK(owner_sap_identity_allowed("A4H", "001", "DEVELOPER", {}));
    CHECK(owner_sap_identity_allowed("A4H", "001", "DEVELOPER", {"A4H/001/DEVELOPER"}));
    CHECK_FALSE(owner_sap_identity_allowed("A4H", "001", "OTHER", {"A4H/001/DEVELOPER"}));
    CHECK_FALSE(owner_sap_identity_allowed("A4H", "000", "DEVELOPER", {"A4H/001/DEVELOPER"}));
}

namespace {

struct FakeProbes : DoctorProbes {
    PortState port = PortState::Free;
    SapState sap_state{SapState::Tri::Yes, SapState::Tri::Yes, ""};
    DesktopState desktop_state{1u, true};
    LockState lock = LockState::Active;
    std::optional<int> tokens = 2;
    std::optional<std::string> autostart;
    bool tray = false;
    SetupFacts setup;
    bool legacy_secret = false;

    SetupFacts setup_facts(const SetupQuery&) override { return setup; }
    bool legacy_proxy_secret() override { return legacy_secret; }
    std::string probed_host;
    PortState probe_port(const std::string& host, int) override {
        probed_host = host;
        return port;
    }
    SapState sap() override { return sap_state; }
    DesktopState desktop() override { return desktop_state; }
    LockState lock_state() override { return lock; }
    std::optional<int> token_count() override { return tokens; }
    std::optional<std::string> autostart_command() override { return autostart; }
    bool tray_running() override { return tray; }
};

DoctorInput http_input() {
    DoctorInput in;
    in.transport = "http";
    in.config.path = "C:\\cfg\\mcp.yaml";
    in.config.exists = true;
    in.exe_path = "C:\\fairyfly\\fairyfly.exe";
    return in;
}

const DoctorCheck& get(const std::vector<DoctorCheck>& checks, const std::string& id) {
    for (const auto& c : checks)
        if (c.id == id) return c;
    FAIL("missing check " << id);
    return checks.front();
}

} // namespace

TEST_CASE("mcp doctor: healthy http setup passes", "[mcp_doctor]") {
    FakeProbes probes;
    const auto checks = run_mcp_doctor(http_input(), probes);
    CHECK(checks.size() == 17);
    // the setup checks skip when nothing was collected
    for (const char* id : {"setup_manifest", "urlacl", "sslcert", "certificate", "firewall", "tls_handshake"}) CHECK(get(checks, id).status == CheckStatus::Skip);
    CHECK(get(checks, "legacy_proxy_secret").status == CheckStatus::Pass);
    CHECK(get(checks, "config").status == CheckStatus::Pass);
    CHECK(get(checks, "port").status == CheckStatus::Pass);
    CHECK(get(checks, "sap_scripting").status == CheckStatus::Pass);
    CHECK(get(checks, "sap_session").status == CheckStatus::Pass);
    CHECK(get(checks, "desktop").status == CheckStatus::Pass);
    CHECK(get(checks, "session_lock").status == CheckStatus::Pass);
    CHECK(get(checks, "tokens").status == CheckStatus::Pass);
    CHECK(get(checks, "autostart").status == CheckStatus::Skip);
    CHECK(get(checks, "tray").status == CheckStatus::Skip);
    CHECK(overall_status(checks) == "pass");
}

TEST_CASE("mcp doctor: config states", "[mcp_doctor]") {
    FakeProbes probes;
    auto in = http_input();

    in.config.exists = false;
    CHECK(get(run_mcp_doctor(in, probes), "config").status == CheckStatus::Pass);

    in = http_input();
    in.config.issues.push_back({Severity::Warning, "CONFIG_UNKNOWN_KEY", "unknown key 'x'", 4, "x"});
    CHECK(get(run_mcp_doctor(in, probes), "config").status == CheckStatus::Warn);

    in = http_input();
    in.config.issues.push_back({Severity::Error, "CONFIG_INVALID_VALUE", "'server.port': must be between 1 and 65535", 7, "server.port"});
    auto checks = run_mcp_doctor(in, probes);
    CHECK(get(checks, "config").status == CheckStatus::Fail);
    CHECK(get(checks, "config").message.find("C:\\cfg\\mcp.yaml:7") != std::string::npos);
    CHECK(overall_status(checks) == "fail");

    in = http_input();
    in.config.read_error = true;
    CHECK(get(run_mcp_doctor(in, probes), "config").status == CheckStatus::Fail);
}

TEST_CASE("mcp doctor: port matrix", "[mcp_doctor]") {
    FakeProbes probes;
    auto in = http_input();

    probes.port = PortState::InUse;
    CHECK(get(run_mcp_doctor(in, probes), "port").status == CheckStatus::Warn);
    probes.tray = true;
    CHECK(get(run_mcp_doctor(in, probes), "port").status == CheckStatus::Pass);   // our own tray server
    probes.tray = false;
    probes.port = PortState::Unknown;
    CHECK(get(run_mcp_doctor(in, probes), "port").status == CheckStatus::Skip);
    in.transport = "stdio";
    probes.port = PortState::InUse;
    CHECK(get(run_mcp_doctor(in, probes), "port").status == CheckStatus::Skip);
}

TEST_CASE("mcp doctor: the '+' wildcard host is probed on loopback", "[mcp_doctor]") {
    FakeProbes probes;
    auto in = http_input();
    in.host = "+";
    in.port = 8443;
    probes.port = PortState::Free;
    const auto checks = run_mcp_doctor(in, probes);
    CHECK(probes.probed_host == "127.0.0.1");
    CHECK(get(checks, "port").status == CheckStatus::Pass);
    CHECK(get(checks, "port").message == "127.0.0.1:8443 is free");
    in.host = "10.1.2.3";
    run_mcp_doctor(in, probes);
    CHECK(probes.probed_host == "10.1.2.3");
}

TEST_CASE("mcp doctor: SAP GUI checks", "[mcp_doctor]") {
    FakeProbes probes;
    probes.sap_state.scripting_available = SapState::Tri::No;
    probes.sap_state.detail = "engine not initialized";
    auto checks = run_mcp_doctor(http_input(), probes);
    CHECK(get(checks, "sap_scripting").status == CheckStatus::Fail);
    CHECK(get(checks, "sap_scripting").message.find("engine not initialized") != std::string::npos);
    CHECK(overall_status(checks) == "fail");

    probes.sap_state = {SapState::Tri::Yes, SapState::Tri::No, ""};
    checks = run_mcp_doctor(http_input(), probes);
    CHECK(get(checks, "sap_session").status == CheckStatus::Warn);
    CHECK(overall_status(checks) == "warn");

    probes.sap_state = {};
    checks = run_mcp_doctor(http_input(), probes);
    CHECK(get(checks, "sap_scripting").status == CheckStatus::Skip);
    CHECK(get(checks, "sap_session").status == CheckStatus::Skip);
}

TEST_CASE("mcp doctor: desktop and lock state", "[mcp_doctor]") {
    FakeProbes probes;
    probes.desktop_state = {0u, true};
    auto checks = run_mcp_doctor(http_input(), probes);
    CHECK(get(checks, "desktop").status == CheckStatus::Fail);   // Session 0
    CHECK(get(checks, "desktop").message.find("Session 0") != std::string::npos);

    probes.desktop_state = {2u, false};
    CHECK(get(run_mcp_doctor(http_input(), probes), "desktop").status == CheckStatus::Warn);
    probes.desktop_state = {std::nullopt, true};
    CHECK(get(run_mcp_doctor(http_input(), probes), "desktop").status == CheckStatus::Skip);

    probes.lock = LockState::Locked;
    CHECK(get(run_mcp_doctor(http_input(), probes), "session_lock").status == CheckStatus::Warn);
    probes.lock = LockState::RdpDisconnected;
    checks = run_mcp_doctor(http_input(), probes);
    CHECK(get(checks, "session_lock").status == CheckStatus::Warn);
    CHECK(get(checks, "session_lock").message.find("black") != std::string::npos);
    probes.lock = LockState::Unknown;
    CHECK(get(run_mcp_doctor(http_input(), probes), "session_lock").status == CheckStatus::Skip);
}

TEST_CASE("mcp doctor: tokens, autostart and tray", "[mcp_doctor]") {
    FakeProbes probes;
    probes.tokens = std::nullopt;   // default probe: unknown
    CHECK(get(run_mcp_doctor(http_input(), probes), "tokens").status == CheckStatus::Skip);
    probes.tokens = 0;
    CHECK(get(run_mcp_doctor(http_input(), probes), "tokens").status == CheckStatus::Warn);
    auto stdio = http_input();
    stdio.transport = "stdio";
    CHECK(get(run_mcp_doctor(stdio, probes), "tokens").status == CheckStatus::Pass);   // no HTTP, no 401 problem

    probes.autostart = "\"C:\\fairyfly\\fairyfly.exe\" mcp --tray";
    CHECK(get(run_mcp_doctor(http_input(), probes), "autostart").status == CheckStatus::Pass);
    probes.autostart = "\"D:\\old\\fairyfly.exe\" mcp --tray";
    CHECK(get(run_mcp_doctor(http_input(), probes), "autostart").status == CheckStatus::Warn);

    probes.tray = true;
    CHECK(get(run_mcp_doctor(http_input(), probes), "tray").status == CheckStatus::Pass);
}

TEST_CASE("mcp doctor: output formats", "[mcp_doctor]") {
    FakeProbes probes;
    probes.lock = LockState::Locked;
    const auto checks = run_mcp_doctor(http_input(), probes);
    const auto text = format_doctor_text(checks);
    CHECK(text.find("[PASS] config:") != std::string::npos);
    CHECK(text.find("[WARN] session_lock:") != std::string::npos);
    CHECK(text.find("-> ") != std::string::npos);   // remediation shown for warnings
    CHECK(text.find("overall: warn") != std::string::npos);
    const auto json = doctor_to_json(checks);
    CHECK(json["overall"] == "warn");
    CHECK(json["checks"].size() == checks.size());
    CHECK(json["checks"][0]["id"] == "config");
}

// ---- real probe functions with fake backends ---------------------------------------------------

namespace {

struct ThrowingBackend final : fairyfly::auth::SecretBackend {
    std::optional<std::string> get(const std::string&) override { throw std::runtime_error("denied"); }
    void put(const std::string&, const std::string&) override { throw std::runtime_error("denied"); }
    bool remove(const std::string&) override { throw std::runtime_error("denied"); }
    std::vector<std::string> list_names() override { throw std::runtime_error("denied"); }
};

} // namespace

TEST_CASE("mcp doctor: count_active_tokens skips revoked and expired tokens", "[mcp_doctor]") {
    using namespace fairyfly::auth;
    auto backend = std::make_shared<InMemorySecretBackend>();
    TimePoint now = std::chrono::system_clock::now();
    TokenStore store(backend, [&now] { return now; });
    CHECK(count_active_tokens(store) == 0);

    NewToken a;
    a.name = "alpha";
    a.scopes = {"*"};
    store.create(a);
    NewToken b = a;
    b.name = "beta";
    store.create(b);
    NewToken c = a;
    c.name = "gamma";
    c.expires = now + std::chrono::hours(1);
    store.create(c);
    CHECK(count_active_tokens(store) == 3);

    store.revoke("beta");
    CHECK(count_active_tokens(store) == 2);
    now += std::chrono::hours(2);  // gamma expired
    store.invalidate();
    CHECK(count_active_tokens(store) == 1);
}

TEST_CASE("mcp doctor: count_active_tokens is nullopt when the backend fails", "[mcp_doctor]") {
    fairyfly::auth::TokenStore store(std::make_shared<ThrowingBackend>());
    CHECK_FALSE(count_active_tokens(store).has_value());
}

// ---- http.sys setup checks -----------------------------------------------------------------------------------------

namespace {

SetupFacts healthy_tls_facts() {
    SetupFacts f;
    f.available = true;
    f.elevation = "standard_user";
    f.manifest_present = true;
    f.manifest_mode = "tls";
    f.manifest_hostname = "sapbox";
    f.manifest_port = 8443;
    f.manifest_thumbprint = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    f.manifest_firewall_rule = "fairyfly MCP HTTPS 8443";
    f.prefix = "https://+:8443/mcp/";
    f.urlacl_known = f.urlacl_reserved = f.urlacl_covers_user = true;
    f.ssl_known = f.ssl_v4 = f.ssl_v6 = true;
    f.ssl_thumbprint = f.manifest_thumbprint;
    f.cert_known = f.cert_found = f.cert_has_key = f.cert_san_ok = true;
    f.cert_thumbprint = f.manifest_thumbprint;
    f.now = 1800000000;
    f.cert_not_after = f.now + 300LL * 86400;
    f.firewall_known = f.firewall_exists = true;
    f.port_listening = true;
    f.tls_status = "ok";
    f.tls_protocol = "TLS 1.3";
    f.tls_thumbprint_match = true;
    return f;
}

DoctorInput tls_input() {
    auto in = http_input();
    in.tls = true;
    in.hostname = "sapbox";
    in.port = 8443;
    return in;
}

} // namespace

TEST_CASE("mcp doctor: healthy https setup passes every http.sys check", "[mcp_doctor]") {
    FakeProbes probes;
    probes.setup = healthy_tls_facts();
    const auto checks = run_mcp_doctor(tls_input(), probes);
    for (const char* id : {"elevation", "setup_manifest", "urlacl", "sslcert", "certificate", "firewall", "tls_handshake", "legacy_proxy_secret"}) {
        INFO(id);
        CHECK(get(checks, id).status == CheckStatus::Pass);
    }
    // order: the new checks come right after the tokens check
    size_t tokens = 0, elevation = 0;
    for (size_t i = 0; i < checks.size(); ++i) {
        if (checks[i].id == "tokens") tokens = i;
        if (checks[i].id == "elevation") elevation = i;
    }
    CHECK(elevation == tokens + 1);
}

TEST_CASE("mcp doctor: fresh machine fails urlacl/sslcert/certificate with the setup remedy", "[mcp_doctor]") {
    FakeProbes probes;
    SetupFacts f;
    f.available = true;
    f.elevation = "standard_user";
    f.manifest_path = "C:\\x\\mcp-setup.json";
    f.prefix = "https://+:8443/mcp/";
    f.urlacl_known = true;
    f.ssl_known = true;
    f.cert_known = true;
    f.now = 1800000000;
    probes.setup = f;
    const auto checks = run_mcp_doctor(tls_input(), probes);
    for (const char* id : {"urlacl", "sslcert", "certificate"}) {
        INFO(id);
        CHECK(get(checks, id).status == CheckStatus::Fail);
        CHECK(get(checks, id).remediation.find("fairyfly mcp setup") != std::string::npos);
    }
    CHECK(get(checks, "setup_manifest").status == CheckStatus::Warn);
    CHECK(get(checks, "firewall").status == CheckStatus::Skip);
    CHECK(get(checks, "tls_handshake").status == CheckStatus::Skip);
    CHECK(overall_status(checks) == "fail");
}

TEST_CASE("mcp doctor: loopback prefix needs no urlacl", "[mcp_doctor]") {
    FakeProbes probes;
    SetupFacts f;
    f.available = true;
    f.elevation = "standard_user";
    f.prefix = "http://127.0.0.1:8383/mcp/";
    f.urlacl_known = true;
    probes.setup = f;
    const auto checks = run_mcp_doctor(http_input(), probes);
    CHECK(get(checks, "urlacl").status == CheckStatus::Pass);
    CHECK(get(checks, "sslcert").status == CheckStatus::Skip);
    CHECK(get(checks, "certificate").status == CheckStatus::Skip);
}

TEST_CASE("mcp doctor: certificate problems", "[mcp_doctor]") {
    FakeProbes probes;
    probes.setup = healthy_tls_facts();

    probes.setup.cert_not_after = probes.setup.now - 1;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "certificate").status == CheckStatus::Fail);
    probes.setup = healthy_tls_facts();
    probes.setup.cert_not_after = probes.setup.now + 10LL * 86400;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "certificate").status == CheckStatus::Warn);   // 30-day warning
    probes.setup = healthy_tls_facts();
    probes.setup.cert_has_key = false;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "certificate").status == CheckStatus::Fail);
    probes.setup = healthy_tls_facts();
    probes.setup.cert_san_ok = false;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "certificate").status == CheckStatus::Fail);
    probes.setup = healthy_tls_facts();
    probes.setup.cert_found = false;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "certificate").status == CheckStatus::Fail);
}

TEST_CASE("mcp doctor: sslcert, urlacl and firewall problems", "[mcp_doctor]") {
    FakeProbes probes;
    probes.setup = healthy_tls_facts();
    probes.setup.urlacl_covers_user = false;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "urlacl").status == CheckStatus::Fail);
    probes.setup = healthy_tls_facts();
    probes.setup.ssl_v6 = false;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "sslcert").status == CheckStatus::Warn);
    probes.setup = healthy_tls_facts();
    probes.setup.ssl_foreign = true;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "sslcert").status == CheckStatus::Warn);
    probes.setup = healthy_tls_facts();
    probes.setup.ssl_thumbprint = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    CHECK(get(run_mcp_doctor(tls_input(), probes), "sslcert").status == CheckStatus::Warn);
    probes.setup = healthy_tls_facts();
    probes.setup.ssl_v4 = false;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "sslcert").status == CheckStatus::Fail);
    probes.setup = healthy_tls_facts();
    probes.setup.firewall_exists = false;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "firewall").status == CheckStatus::Warn);
    probes.setup = healthy_tls_facts();
    probes.setup.manifest_firewall_rule.clear();
    CHECK(get(run_mcp_doctor(tls_input(), probes), "firewall").status == CheckStatus::Skip);
    probes.setup = healthy_tls_facts();
    probes.setup.urlacl_known = false;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "urlacl").status == CheckStatus::Skip);
}

TEST_CASE("mcp doctor: tls handshake", "[mcp_doctor]") {
    FakeProbes probes;
    probes.setup = healthy_tls_facts();
    probes.setup.port_listening = false;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "tls_handshake").status == CheckStatus::Skip);
    probes.setup = healthy_tls_facts();
    probes.setup.tls_thumbprint_match = false;
    CHECK(get(run_mcp_doctor(tls_input(), probes), "tls_handshake").status == CheckStatus::Warn);
    probes.setup = healthy_tls_facts();
    probes.setup.tls_protocol = "TLS 1.1";
    CHECK(get(run_mcp_doctor(tls_input(), probes), "tls_handshake").status == CheckStatus::Warn);
    probes.setup = healthy_tls_facts();
    probes.setup.tls_status = "handshake_failed";
    CHECK(get(run_mcp_doctor(tls_input(), probes), "tls_handshake").status == CheckStatus::Warn);
    probes.setup = healthy_tls_facts();
    probes.setup.tls_protocol = "TLS 1.2";
    const auto ok = get(run_mcp_doctor(tls_input(), probes), "tls_handshake");
    CHECK(ok.status == CheckStatus::Pass);
    CHECK(ok.message.find("TLS 1.2") != std::string::npos);
}

TEST_CASE("mcp doctor: legacy proxy secret", "[mcp_doctor]") {
    FakeProbes probes;
    probes.legacy_secret = true;
    const auto check = get(run_mcp_doctor(http_input(), probes), "legacy_proxy_secret");
    CHECK(check.status == CheckStatus::Warn);
    CHECK(check.remediation == "cmdkey /delete:fairyfly:fairyfly-mcp-proxy");
}

TEST_CASE("mcp doctor: stdio without a manifest skips the http.sys checks", "[mcp_doctor]") {
    FakeProbes probes;
    probes.setup = healthy_tls_facts();
    probes.setup.manifest_present = false;
    auto in = http_input();
    in.transport = "stdio";
    const auto checks = run_mcp_doctor(in, probes);
    CHECK(get(checks, "urlacl").status == CheckStatus::Skip);
    CHECK(get(checks, "certificate").status == CheckStatus::Skip);
}
