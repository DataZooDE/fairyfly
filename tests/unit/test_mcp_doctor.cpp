#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "include/auth/secret_backend.h"
#include "include/config/mcp_doctor.h"

using namespace fairyfly::config;

namespace {

struct FakeProbes : DoctorProbes {
    PortState port = PortState::Free;
    SapState sap_state{SapState::Tri::Yes, SapState::Tri::Yes, ""};
    DesktopState desktop_state{1u, true};
    LockState lock = LockState::Active;
    std::optional<int> tokens = 2;
    IisState iis_state{};
    std::optional<std::string> autostart;
    bool tray = false;

    PortState probe_port(const std::string&, int) override { return port; }
    SapState sap() override { return sap_state; }
    DesktopState desktop() override { return desktop_state; }
    LockState lock_state() override { return lock; }
    std::optional<int> token_count() override { return tokens; }
    IisState iis() override { return iis_state; }
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
    CHECK(checks.size() == 10);
    CHECK(get(checks, "config").status == CheckStatus::Pass);
    CHECK(get(checks, "port").status == CheckStatus::Pass);
    CHECK(get(checks, "sap_scripting").status == CheckStatus::Pass);
    CHECK(get(checks, "sap_session").status == CheckStatus::Pass);
    CHECK(get(checks, "desktop").status == CheckStatus::Pass);
    CHECK(get(checks, "session_lock").status == CheckStatus::Pass);
    CHECK(get(checks, "tokens").status == CheckStatus::Pass);
    CHECK(get(checks, "iis").status == CheckStatus::Skip);
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

TEST_CASE("mcp doctor: tokens, IIS, autostart and tray", "[mcp_doctor]") {
    FakeProbes probes;
    probes.tokens = std::nullopt;   // default probe: unknown
    CHECK(get(run_mcp_doctor(http_input(), probes), "tokens").status == CheckStatus::Skip);
    probes.tokens = 0;
    CHECK(get(run_mcp_doctor(http_input(), probes), "tokens").status == CheckStatus::Warn);
    auto stdio = http_input();
    stdio.transport = "stdio";
    CHECK(get(run_mcp_doctor(stdio, probes), "tokens").status == CheckStatus::Pass);   // no HTTP, no 401 problem

    probes.iis_state = {true, true, "site fairyfly on :8443"};
    CHECK(get(run_mcp_doctor(http_input(), probes), "iis").status == CheckStatus::Pass);
    probes.iis_state = {true, false, "URL Rewrite missing"};
    CHECK(get(run_mcp_doctor(http_input(), probes), "iis").status == CheckStatus::Warn);

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

namespace fi = fairyfly::iis;

struct ProbeIisHost final : fi::IisHost {
    fi::HostFacts facts;
    fi::SiteInfo site;
    bool throw_on_detect = false;
    int mutations = 0;

    fi::HostFacts detect() override {
        if (throw_on_detect) throw fi::HostError{"IIS_SCRIPT_FAILED", "boom"};
        return facts;
    }
    fi::SiteInfo get_site(const std::string&) override { return site; }
    fi::AppPoolInfo get_app_pool(const std::string&) override { return {}; }
    std::optional<fi::CertInfo> find_certificate(const std::string&) override { return std::nullopt; }
    std::optional<fi::CertInfo> find_self_signed(const std::string&) override { return std::nullopt; }
    bool firewall_rule_exists(const std::string&) override { return false; }
    bool tcp_reachable(const std::string&, int, int) override { return false; }
    bool dir_exists(const std::string&) override { return false; }
    std::optional<std::string> read_file(const std::string&) override { return std::nullopt; }
    fi::Change create_dir(const std::string&) override { ++mutations; return fi::Change::Unchanged; }
    fi::Change write_file(const std::string&, const std::string&) override { ++mutations; return fi::Change::Unchanged; }
    bool remove_file(const std::string&) override { ++mutations; return false; }
    bool remove_dir_if_empty(const std::string&) override { ++mutations; return false; }
    fi::CertInfo create_self_signed(const std::string&, int) override { ++mutations; return {}; }
    fi::Change export_certificate(const std::string&, const std::string&) override { ++mutations; return fi::Change::Unchanged; }
    fi::Change ensure_app_pool(const std::string&) override { ++mutations; return fi::Change::Unchanged; }
    fi::Change ensure_site(const fi::SiteSpec&) override { ++mutations; return fi::Change::Unchanged; }
    fi::Change ensure_site_config_access(const std::string&, const std::vector<std::string>&) override { ++mutations; return fi::Change::Unchanged; }
    fi::Change restrict_acl(const std::string&, const std::string&) override { ++mutations; return fi::Change::Unchanged; }
    fi::Change ensure_firewall_rule(const std::string&, int) override { ++mutations; return fi::Change::Unchanged; }
    bool remove_firewall_rule(const std::string&) override { ++mutations; return false; }
    bool remove_site(const std::string&) override { ++mutations; return false; }
    bool remove_app_pool(const std::string&) override { ++mutations; return false; }
    bool remove_certificate(const std::string&) override { ++mutations; return false; }
};

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

TEST_CASE("mcp doctor: probe_iis_state is read-only and reports absence as info", "[mcp_doctor]") {
    ProbeIisHost host;
    SECTION("IIS not installed -> info skip") {
        const IisState st = probe_iis_state(host);
        CHECK_FALSE(st.checked);
        CHECK(st.message.find("not installed") != std::string::npos);
        FakeProbes probes;
        probes.iis_state = st;
        const auto checks = run_mcp_doctor(http_input(), probes);
        CHECK(get(checks, "iis").status == CheckStatus::Skip);
        CHECK(get(checks, "iis").message.find("not installed") != std::string::npos);
    }
    SECTION("IIS installed, site absent -> not configured") {
        host.facts.iis_installed = true;
        host.facts.admin_module = "WebAdministration";
        const IisState st = probe_iis_state(host);
        CHECK_FALSE(st.checked);
        CHECK(st.message.find("not configured") != std::string::npos);
    }
    SECTION("site started -> ok") {
        host.facts.iis_installed = true;
        host.facts.admin_module = "WebAdministration";
        host.site.exists = true;
        host.site.state = "Started";
        const IisState st = probe_iis_state(host);
        CHECK(st.checked);
        CHECK(st.ok);
    }
    SECTION("site stopped -> problem") {
        host.facts.iis_installed = true;
        host.facts.admin_module = "WebAdministration";
        host.site.exists = true;
        host.site.state = "Stopped";
        const IisState st = probe_iis_state(host);
        CHECK(st.checked);
        CHECK_FALSE(st.ok);
        FakeProbes probes;
        probes.iis_state = st;
        CHECK(get(run_mcp_doctor(http_input(), probes), "iis").status == CheckStatus::Warn);
    }
    SECTION("host error -> info skip, no throw") {
        host.throw_on_detect = true;
        const IisState st = probe_iis_state(host);
        CHECK_FALSE(st.checked);
        CHECK(st.message.find("IIS_SCRIPT_FAILED") != std::string::npos);
    }
    CHECK(host.mutations == 0);
}
