// `fairyfly proxy show|set|test|reset` (include/link/proxy_cli.h), the link tray's "Network / proxy..." item and form,
// and the doctor's proxy check: with the inputs as tests set them (the registry readers and the real window are
// tested in test_proxy_sources_win32.cpp, the routes end to end against squid in tests/linux_proxy).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "include/doctor_checks.h"
#include "include/link/proxy_cli.h"
#include "include/tray/proxy_window.h"
#include "include/tray/tray.h"

using namespace fairyfly;
using namespace fairyfly::link::proxycfg;
using Catch::Matchers::ContainsSubstring;

namespace {

struct Fake {
    Inputs in;
    std::optional<Setting> saved;
    int changed = 0;
    std::vector<std::optional<Setting>> probed;
    bool probe_ok = true;
    Services services() {
        Services s;
        s.inputs = [this] {
            Inputs x = in;
            if (!x.policy.present) x.user = saved;
            return x;
        };
        s.save = [this](const Setting& v) {
            saved = v.mode == Mode::automatic ? std::nullopt : std::optional<Setting>(v);
            return true;
        };
        s.changed = [this] { ++changed; };
        s.targets = [] { return probe_targets("wss://link.fairyfly.io/ffl", ""); };
        s.probe = [this](const ProbeTargets&, const std::optional<Setting>& c) {
            probed.push_back(c);
            ProbeResult r;
            r.ok = probe_ok;
            r.summary = probe_ok ? "Connected" : "Could not connect: cannot reach proxy p:1";
            return r;
        };
        return s;
    }
};

CliArgs args(std::string verb, std::optional<std::string> mode = {}, std::optional<std::string> server = {},
             std::optional<std::string> pac = {}, std::optional<std::string> bypass = {}) {
    CliArgs a;
    a.verb = std::move(verb);
    a.mode = std::move(mode);
    a.server = std::move(server);
    a.pac = std::move(pac);
    a.bypass = std::move(bypass);
    return a;
}

std::string code(const Result& r) { return r.error.value("code", std::string()); }

}  // namespace

TEST_CASE("proxy cli: show names the setting, the route, its trust and the sources - never HTTPS_PROXY's value") {
    Fake f;
    f.in.env.https_proxy = "http://alice:s3cret@env:3128";
    auto s = f.services();
    auto r = run_cli(args("show"), s);
    REQUIRE(r.status == Result::Status::Success);
    CHECK(r.data["setting"]["mode"] == "auto");
    CHECK(r.data["setting"]["from"] == "default");
    CHECK(r.data["locked"] == false);
    CHECK(r.data["route"]["route"] == "via env:3128 from HTTPS_PROXY, Windows login: yes");
    CHECK(r.data["sources"]["https_proxy_set"] == true);
    CHECK(r.data["for"] == "https://link.fairyfly.io/health");
    CHECK(r.data.dump().find("s3cret") == std::string::npos);
}

TEST_CASE("proxy cli: set saves a checked setting, which decides at once; reset forgets it") {
    Fake f;
    auto s = f.services();
    auto r = run_cli(args("set", "manual", "http://proxy.corp.example:8080", {}, "<local>;*.corp.example"), s);
    REQUIRE(r.status == Result::Status::Success);
    REQUIRE(f.saved);
    CHECK(f.saved->server == "proxy.corp.example:8080");
    CHECK(f.changed == 1);
    CHECK(r.data["route"]["tier"] == "your fairyfly setting");
    CHECK(r.data["setting"]["from"] == "user");
    CHECK_THAT(r.data["next"].get<std::string>(), ContainsSubstring("reconnects"));

    r = run_cli(args("set", "pac", {}, "https://pac.corp.example/p.pac"), s);
    REQUIRE(r.status == Result::Status::Success);
    CHECK(f.saved->mode == Mode::pac);
    r = run_cli(args("set", "direct"), s);
    CHECK(r.data["route"]["route"] == "direct (your fairyfly setting: Direct)");
    r = run_cli(args("reset"), s);
    REQUIRE(r.status == Result::Status::Success);
    CHECK_FALSE(f.saved);
    CHECK(r.data["setting"]["from"] == "default");
}

TEST_CASE("proxy cli: set refuses what cannot be used, with the reason and without saving") {
    Fake f;
    auto s = f.services();
    for (auto a : {args("set"), args("set", "sometimes"), args("set", "manual"), args("set", "pac"),
                   args("set", "manual", "https://proxy:443"), args("set", "manual", "socks5://proxy:1080"),
                   args("set", "manual", "proxy:8080\r\nX-Injected: 1"), args("set", "manual", "alice:pw@proxy:8080"),
                   args("set", "pac", {}, "file:///C:/p.pac"), args("set", "direct", "proxy:8080"),
                   args("set", "manual", "proxy:8080", "https://x/p.pac")}) {
        INFO(a.verb << " " << a.mode.value_or("-") << " " << a.server.value_or("-") << " " << a.pac.value_or("-"));
        auto r = run_cli(a, s);
        CHECK(code(r) == "INVALID_ARGUMENT");
        CHECK_FALSE(f.saved);
    }
    auto r = run_cli(args("set", "manual", "https://proxy:443"), s);
    CHECK_THAT(r.error["message"].get<std::string>(), ContainsSubstring("https:// proxies"));
    CHECK(r.error["suggestions"][0] == kUnsupportedText);
}

TEST_CASE("proxy cli: the administrator's policy locks set, reset and testing another setting") {
    Fake f;
    f.in.policy.present = true;
    f.in.policy.setting.mode = Mode::direct;
    auto s = f.services();
    for (auto a : {args("set", "direct"), args("reset"), args("test", "manual", "p:1")}) {
        auto r = run_cli(a, s);
        CHECK(code(r) == "PROXY_SETTING_LOCKED");
        CHECK_THAT(r.error["message"].get<std::string>(), ContainsSubstring("administrator"));
    }
    CHECK_FALSE(f.saved);
    CHECK(f.changed == 0);
    auto r = run_cli(args("show"), s);
    CHECK(r.data["locked"] == true);
    CHECK(r.data["setting"]["from"] == "policy");
    CHECK(r.data["route"]["route"] == "direct (fairyfly policy: Direct)");
    r = run_cli(args("test"), s);  // testing the effective route is fine
    CHECK(r.status == Result::Status::Success);
}

TEST_CASE("proxy cli: test runs the connection test with the saved route, or with a setting given on the command line") {
    Fake f;
    auto s = f.services();
    auto r = run_cli(args("test"), s);
    REQUIRE(r.status == Result::Status::Success);
    REQUIRE(f.probed.size() == 1);
    CHECK_FALSE(f.probed[0]);
    CHECK(r.data["test"]["ok"] == true);

    r = run_cli(args("test", "manual", "proxy:3128"), s);
    REQUIRE(f.probed.size() == 2);
    REQUIRE(f.probed[1]);
    CHECK(f.probed[1]->server == "proxy:3128");
    CHECK(r.data["tested_setting"]["server"] == "proxy:3128");
    CHECK_FALSE(f.saved);  // not saved

    f.probe_ok = false;
    r = run_cli(args("test"), s);
    CHECK(code(r) == "PROXY_TEST_FAILED");
    CHECK_THAT(r.error["message"].get<std::string>(), ContainsSubstring("cannot reach proxy"));
    CHECK(r.data["test"]["ok"] == false);  // the details stay
    CHECK(code(run_cli(args("nothing"), s)) == "NO_SUBCOMMAND");
}

TEST_CASE("proxy tray: \"Network / proxy...\" is in the link tray's menu (paired or not) and opens the window") {
    using namespace fairyfly::tray;
    LinkView paired;
    paired.paired = true;
    paired.state = "connected";
    for (const LinkView& v : {paired, LinkView{}}) {
        auto m = build_link_menu(v, {}, {}, {}, true, true);
        bool found = false;
        for (const auto& i : m.items) found = found || (i.label == kProxyMenuItem && i.action == TrayAction::ProxySettings && i.enabled);
        CHECK(found);
        for (const auto& i : build_link_menu(v).items) CHECK(i.action != TrayAction::ProxySettings);  // only with a window
    }
    int opened = 0;
    LinkTrayServices services;
    services.open_proxy_window = [&] { ++opened; };
    LinkTrayController controller(services, TrayPaths{});
    bool in_menu = false;
    for (const auto& i : controller.menu().items) in_menu = in_menu || i.action == TrayAction::ProxySettings;
    CHECK(in_menu);
    controller.on_action(TrayAction::ProxySettings);
    CHECK(opened == 1);
}

TEST_CASE("proxy tray: the window's form is checked like the CLI's flags") {
    using namespace fairyfly::tray;
    ProxyForm f;
    CHECK_FALSE(form_error(f));
    f.mode = Mode::manual;
    CHECK(form_error(f));
    f.server = "https://proxy:443";
    CHECK_THAT(*form_error(f), ContainsSubstring("https://"));
    f.server = "proxy:8080";
    f.pac_url = "ignored for manual";
    CHECK_FALSE(form_error(f));
    CHECK(form_setting(f).pac_url.empty());
    f.mode = Mode::pac;
    CHECK(form_error(f));
    f.pac_url = "https://pac.corp.example/p.pac";
    CHECK_FALSE(form_error(f));
    CHECK(form_setting(f).server.empty());
}

TEST_CASE("doctor: over MCP the proxy check says only configured, direct or failing") {
    for (const char* s : {"configured", "direct"}) {
        auto c = cli::doctor::judge_proxy_summary(s);
        CHECK(c.name == "proxy");
        CHECK(c.status == "info");
        CHECK(c.message == std::string("proxy: ") + s);
        CHECK(c.details.empty());  // no proxy or PAC host names for a remote caller
    }
    auto c = cli::doctor::judge_proxy_summary("failing");
    CHECK(c.status == "warn");
    CHECK(c.message == "proxy: failing");
    CHECK_THAT(c.remediation, ContainsSubstring("at this desktop"));
}

TEST_CASE("doctor: the proxy check passes when the gateway answered, warns with the way forward when not") {
    cli::doctor::ProxyFacts f;
    f.route = "via proxy:8080 from your fairyfly setting, Windows login: yes";
    f.source = "your fairyfly setting (manual proxy)";
    f.setting = "manual";
    f.setting_from = "user";
    f.windows_login = true;
    f.test_ok = true;
    f.test = "Connected";
    auto c = cli::doctor::judge_proxy(f);
    CHECK(c.name == "proxy");
    CHECK(c.status == "pass");
    CHECK_THAT(c.message, ContainsSubstring("via proxy:8080"));
    CHECK_THAT(c.message, ContainsSubstring("(yours)"));
    CHECK_THAT(c.details, ContainsSubstring("Windows login: yes"));
    f.locked = true;
    CHECK_THAT(cli::doctor::judge_proxy(f).message, ContainsSubstring("set by your administrator"));
    f.test_ok = false;
    c = cli::doctor::judge_proxy(f);
    CHECK(c.status == "warn");
    CHECK(c.code == "PROXY_TEST_FAILED");
    CHECK_THAT(c.remediation, ContainsSubstring("fairyfly proxy test"));
    f.test_ok = true;
    f.warnings = "Edge policy ProxyMode \"x\" is unknown; ignored";
    CHECK(cli::doctor::judge_proxy(f).status == "warn");
}
