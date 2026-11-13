// Where the proxy comes from (include/link/proxy_config.h): the checks of what is typed or set by policy, the fairyfly
// policy and setting, the browser policies, the order of all sources, the trust each route gets, the route in plain
// words, the cached resolver and libffl's configuration. Portable: runs in unit_tests (Windows) and in the Linux
// proxy tests next to the real squid (tests/linux_proxy).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <map>
#include <thread>

#include "include/link/proxy_config.h"
#include "util.hpp"  // libffl

using namespace fairyfly::link::proxycfg;
using fairyfly::link::sysproxy::AutoProxyResult;
using fairyfly::link::sysproxy::AutoSource;
using ffy::proxy::Origin;
using ffy::proxy::Route;
using ffy::proxy::Routes;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string kUrl = "https://fairyfly.io/health";

Route via(const std::string& h, int p) { return Route::via({h, p}); }

Setting manual(std::string server, std::string bypass = {}) {
    Setting s;
    s.mode = Mode::manual;
    s.server = std::move(server);
    s.bypass = std::move(bypass);
    return s;
}
Setting pac(std::string url) {
    Setting s;
    s.mode = Mode::pac;
    s.pac_url = std::move(url);
    return s;
}
Setting direct() {
    Setting s;
    s.mode = Mode::direct;
    return s;
}
PolicySetting policy(Setting s) {
    PolicySetting p;
    p.present = true;
    p.setting = s;
    return p;
}
BrowserPolicy browser(std::string name, BrowserPolicy::Kind k, std::string value = {}, bool admin = true) {
    BrowserPolicy b;
    b.browser = std::move(name);
    b.where = b.browser == "Firefox" ? "C:\\Program Files\\Mozilla Firefox\\distribution\\policies.json" : "HKLM\\SOFTWARE\\Policies\\X";
    b.kind = k;
    b.admin = admin;
    if (k == BrowserPolicy::Kind::fixed) b.server = value;
    if (k == BrowserPolicy::Kind::pac) b.pac_url = value;
    return b;
}

// A proxy script evaluator that answers with a fixed proxy and records what it was asked (WinHTTP's part; the real one
// runs in the Windows tests).
struct Scripts {
    std::vector<std::string> asked;  // "pac <url> trusted" / "wpad"
    std::string answer = "script:3128";
    bool fail = false;
    AutoProxy fn() {
        return [this](const std::string&, AutoSource src, const std::string& url, bool trusted) {
            asked.push_back(src == AutoSource::wpad ? std::string("wpad") : "pac " + url + (trusted ? " trusted" : ""));
            AutoProxyResult r;
            if (fail) {
                r.error = "the proxy script (PAC) could not be downloaded";
                return r;
            }
            r.ok = true;
            r.proxy = answer;
            return r;
        };
    }
};

Origin first_origin(const RouteDecision& d) {
    REQUIRE_FALSE(d.routes.empty());
    return d.routes.front().proxy.origin;
}

}  // namespace

TEST_CASE("proxy config: what is typed or set by policy is checked before it is used") {
    CHECK_FALSE(check_server("proxy.corp.example:8080"));
    CHECK_FALSE(check_server("http://proxy.corp.example:8080/"));
    CHECK_FALSE(check_server("[fd00::1]:3128"));
    CHECK_FALSE(check_server("10.1.2.3:80"));
    CHECK_THAT(*check_server("https://proxy.corp.example:443"), ContainsSubstring("https:// proxies"));
    CHECK_THAT(*check_server("socks5://proxy:1080"), ContainsSubstring("SOCKS"));
    CHECK_THAT(*check_server("ftp://proxy:21"), ContainsSubstring("only HTTP"));
    CHECK_THAT(*check_server("proxy.corp.example"), ContainsSubstring("port"));
    CHECK_THAT(*check_server("alice:pw@proxy:8080"), ContainsSubstring("Credential Manager"));
    CHECK(check_server("proxy:8080\r\nX-Evil: 1"));
    CHECK(check_server("proxy:8080\n"));
    CHECK(check_server("proxy :8080"));
    CHECK(check_server("proxy:0"));
    CHECK(check_server("proxy:65536"));
    CHECK(check_server("proxy:80/path"));
    CHECK(check_server("https=a:1;http=b:2"));
    CHECK(check_server("-proxy:80"));
    CHECK(check_server(""));

    CHECK_FALSE(check_pac_url("https://pac.corp.example/proxy.pac"));
    CHECK_FALSE(check_pac_url("http://wpad.corp.example/wpad.dat?x=1"));
    CHECK_THAT(*check_pac_url("file:///C:/proxy.pac"), ContainsSubstring("http:// or https://"));
    CHECK(check_pac_url("ftp://pac/proxy.pac"));
    CHECK(check_pac_url("https://pac.corp.example/p.pac\r\nHost: x"));
    CHECK(check_pac_url("https://user@pac.corp.example/p.pac"));
    CHECK(check_pac_url("pac.corp.example/p.pac"));
    CHECK(check_pac_url("https:///p.pac"));

    CHECK_FALSE(check_bypass("<local>;*.corp.example, 10.*"));
    CHECK(check_bypass("a\r\nb"));
    CHECK(check_bypass("a\"b"));

    CHECK_FALSE(check(Setting{}));
    CHECK_FALSE(check(direct()));
    CHECK(check(manual("")));
    CHECK(check(pac("")));
    CHECK(normalised(manual("http://proxy:8080/", "<local> *.a, b;c")) == manual("proxy:8080", "<local>;*.a;b;c"));
    Setting extra = manual("proxy:8080");
    extra.pac_url = "https://x/p.pac";
    CHECK(normalised(extra).pac_url.empty());
}

TEST_CASE("proxy config: the fairyfly policy locks as soon as ProxyMode exists, also when its values are wrong") {
    CHECK_FALSE(policy_from_values(std::nullopt, std::string("p:1"), std::nullopt, std::nullopt).present);
    auto p = policy_from_values(std::string("Manual"), std::string("proxy.corp.example:8080"), std::nullopt, std::string("<local>"));
    CHECK(p.present);
    CHECK(p.error.empty());
    CHECK(p.setting == manual("proxy.corp.example:8080", "<local>"));
    auto bad = policy_from_values(std::string("sometimes"), std::nullopt, std::nullopt, std::nullopt);
    CHECK(bad.present);
    CHECK_THAT(bad.error, ContainsSubstring("ProxyMode"));
    auto socks = policy_from_values(std::string("manual"), std::string("socks://p:1080"), std::nullopt, std::nullopt);
    CHECK(socks.present);
    CHECK_THAT(socks.error, ContainsSubstring("SOCKS"));
    auto nopac = policy_from_values(std::string("pac"), std::nullopt, std::nullopt, std::nullopt);
    CHECK_THAT(nopac.error, ContainsSubstring("ProxyPacUrl"));
}

TEST_CASE("proxy config: the user's setting is saved and read back; what fails the checks is ignored") {
    std::map<std::string, std::string> store;
    SettingGet get = [&](const std::string& n) -> std::optional<std::string> {
        auto i = store.find(n);
        return i == store.end() ? std::nullopt : std::optional<std::string>(i->second);
    };
    SettingSet set = [&](const std::string& n, const std::string& v) {
        store[n] = v;
        return true;
    };
    CHECK_FALSE(load_user_setting(get));
    REQUIRE(save_user_setting(set, manual("proxy:8080", "<local>")));
    CHECK(load_user_setting(get) == manual("proxy:8080", "<local>"));
    REQUIRE(save_user_setting(set, pac("https://pac.corp.example/p.pac")));
    CHECK(load_user_setting(get) == pac("https://pac.corp.example/p.pac"));
    CHECK(store[kUserServerSetting].empty());  // the other mode's fields are cleared
    REQUIRE(save_user_setting(set, Setting{}));
    CHECK_FALSE(load_user_setting(get));
    CHECK(store[kUserModeSetting].empty());

    // tampered with (the registry is the user's): ignored, with the reason
    store[kUserModeSetting] = "manual";
    store[kUserServerSetting] = "evil:80\r\nX: y";
    std::string warning;
    CHECK_FALSE(load_user_setting(get, &warning));
    CHECK_THAT(warning, ContainsSubstring("not valid"));
    store[kUserModeSetting] = "weird";
    CHECK_FALSE(load_user_setting(get, &warning));
    CHECK_FALSE(save_user_setting([](const std::string&, const std::string&) { return false; }, direct()));
}

TEST_CASE("proxy config: Chrome and Edge proxy policies (single policies, ProxySettings JSON, legacy ProxyServerMode)") {
    using K = BrowserPolicy::Kind;
    std::string w;
    ChromiumValues v;
    CHECK_FALSE(chromium_policy("Edge", "HKLM\\x", v, &w));  // nothing set
    v.mode = "fixed_servers";
    v.server = "proxy.corp.example:8080";
    v.bypass = "<local>;*.corp.example";
    auto p = chromium_policy("Edge", "HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge", v, &w);
    REQUIRE(p);
    CHECK(p->kind == K::fixed);
    CHECK(p->server == "proxy.corp.example:8080");
    CHECK(p->label() == "Edge policy (HKLM)");

    v = {};
    v.mode = "pac_script";
    v.pac_url = "https://pac.corp.example/p.pac";
    REQUIRE(chromium_policy("Chrome", "HKCU\\x", v, &w));
    CHECK(chromium_policy("Chrome", "HKCU\\x", v, &w)->label() == "Chrome policy (HKCU)");
    v.pac_url = "file:///C:/p.pac";  // Chrome may read it; fairyfly does not fetch files
    w.clear();
    CHECK_FALSE(chromium_policy("Chrome", "HKCU\\x", v, &w));
    CHECK_THAT(w, ContainsSubstring("ProxyPacUrl"));

    // ProxySettings overrides the single policies
    v = {};
    v.mode = "direct";
    v.settings_json = R"({"ProxyMode":"fixed_servers","ProxyServer":"https=secure:8443;http=web:80","ProxyBypassList":"*.x"})";
    p = chromium_policy("Chrome", "HKLM\\x", v, &w);
    REQUIRE(p);
    CHECK(p->kind == K::fixed);
    CHECK(p->bypass == "*.x");
    v.settings_json = "not json";
    p = chromium_policy("Chrome", "HKLM\\x", v, &w);
    REQUIRE(p);
    CHECK(p->kind == K::direct);  // the single policy, with a warning
    CHECK_THAT(w, ContainsSubstring("ProxySettings"));

    v = {};
    v.server_mode = 3;
    CHECK(chromium_policy("Edge", "HKLM\\x", v)->kind == K::system);
    v.server_mode = 1;
    CHECK(chromium_policy("Edge", "HKLM\\x", v)->kind == K::auto_detect);
    v.server_mode = 9;
    CHECK_FALSE(chromium_policy("Edge", "HKLM\\x", v));

    v = {};
    v.server = "p:1";  // no mode: Chrome ignores it too
    w.clear();
    CHECK_FALSE(chromium_policy("Edge", "HKLM\\x", v, &w));
    CHECK_THAT(w, ContainsSubstring("without ProxyMode"));
    v.mode = "fixed_servers";
    v.server = "socks5://p:1080";
    CHECK_FALSE(chromium_policy("Edge", "HKLM\\x", v, &w));
    v.server = "p:1\r\nX: y";
    CHECK_FALSE(chromium_policy("Edge", "HKLM\\x", v, &w));
    v.mode = "bogus";
    CHECK_FALSE(chromium_policy("Edge", "HKLM\\x", v, &w));
}

TEST_CASE("proxy config: Firefox's Proxy policy (policies.json and registry values)") {
    using K = BrowserPolicy::Kind;
    std::string err;
    auto v = firefox_values_from_json(R"({"policies":{"Proxy":{"Mode":"manual","HTTPProxy":"web:3128","UseHTTPProxyForAllProtocols":true,"Passthrough":"<local>, .corp.example"}}})", &err);
    CHECK(err.empty());
    auto p = firefox_policy("C:\\Program Files\\Mozilla Firefox\\distribution\\policies.json", true, v);
    REQUIRE(p);
    CHECK(p->kind == K::fixed);
    CHECK(p->server == "web:3128");
    CHECK(p->label() == "Firefox policies.json");
    auto writable = firefox_policy("C:\\Firefox\\distribution\\policies.json", false, v);
    writable->untrusted = true;  // set by the reader when non-administrators can change the file
    CHECK(writable->label() == "Firefox policies.json (user-writable)");
    CHECK(firefox_policy("HKCU\\Software\\Policies\\Mozilla\\Firefox\\Proxy", false, v)->label() == "Firefox policy (HKCU)");

    v.http_for_all = false;  // HTTPS goes direct in Firefox then
    CHECK(firefox_policy("x", true, v)->kind == K::direct);
    v.ssl_proxy = "http://secure:8443";
    p = firefox_policy("x", true, v);
    CHECK(p->kind == K::fixed);
    CHECK(p->server == "secure:8443");

    FirefoxValues r;
    r.mode = "autoConfig";
    r.autoconfig_url = "http://pac.corp.example/p.pac";
    p = firefox_policy("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy", true, r);
    REQUIRE(p);
    CHECK(p->kind == K::pac);
    CHECK(p->label() == "Firefox policy (HKLM)");
    r.mode = "none";
    CHECK(firefox_policy("x", true, r)->kind == K::direct);
    r.mode = "system";
    CHECK(firefox_policy("x", true, r)->kind == K::system);
    r.mode = "autoDetect";
    CHECK(firefox_policy("x", true, r)->kind == K::auto_detect);
    r.mode = "manual";
    r.ssl_proxy = "socks://x:1";
    CHECK_FALSE(firefox_policy("x", true, r));
    r = {};
    r.http_proxy = "p:1";
    CHECK_FALSE(firefox_policy("x", true, r));  // no Mode

    CHECK_FALSE(firefox_values_from_json("{", &err).any());
    CHECK_THAT(err, ContainsSubstring("not valid JSON"));
    err.clear();
    CHECK_FALSE(firefox_values_from_json(R"({"policies":{"DisableAppUpdate":true}})", &err).any());
    CHECK(err.empty());
    CHECK_FALSE(firefox_values_from_json(R"({"policies":{"Proxy":"manual"}})", &err).any());
    CHECK_THAT(err, ContainsSubstring("not an object"));
}

TEST_CASE("proxy config: a file counts as policy only when nobody but administrators can change it") {
    SecurityInfo admin_only;
    admin_only.readable = admin_only.has_dacl = admin_only.owner_admin_like = true;
    admin_only.aces = {{true, false, true, 0x1F01FF}, {true, false, false, 0x1200A9}};  // admins full, users read+execute
    CHECK(only_admins_can_change(admin_only, false));
    CHECK(only_admins_can_change(admin_only, true));

    auto with = [&](Ace a) {
        SecurityInfo s = admin_only;
        s.aces.push_back(a);
        return s;
    };
    CHECK_FALSE(only_admins_can_change(with({true, false, false, 0x2}), false));        // users may write
    CHECK_FALSE(only_admins_can_change(with({true, false, false, 0x10000}), false));    // or delete
    CHECK_FALSE(only_admins_can_change(with({true, false, false, 0x40000}), false));    // or re-permission
    CHECK_FALSE(only_admins_can_change(with({true, false, false, 0x10000000}), false)); // GENERIC_ALL
    CHECK(only_admins_can_change(with({true, false, false, 0x40}), false));             // delete-child means nothing on a file
    CHECK_FALSE(only_admins_can_change(with({true, false, false, 0x40}), true));        // on a folder it does
    CHECK(only_admins_can_change(with({true, true, false, 0x1F01FF}), true));           // inherit-only: not this folder
    CHECK(only_admins_can_change(with({false, false, false, 0x1F01FF}), true));         // a deny entry grants nothing

    SecurityInfo s = admin_only;
    s.owner_admin_like = false;  // the owner can always change the permissions
    CHECK_FALSE(only_admins_can_change(s, false));
    s = admin_only;
    s.has_dacl = false;  // no DACL: everyone
    CHECK_FALSE(only_admins_can_change(s, false));
    CHECK_FALSE(only_admins_can_change(SecurityInfo{}, false));
}

TEST_CASE("proxy config: the order of the sources - fairyfly policy, your setting, HTTPS_PROXY, browser policy, Windows") {
    using K = BrowserPolicy::Kind;
    Scripts scripts;
    Inputs in;
    in.windows.proxy = "windows:8080";
    in.browsers = {browser("Edge", K::fixed, "edge:8080"), browser("Chrome", K::fixed, "chrome:8080")};
    in.env.https_proxy = "http://env:3128";
    in.user = manual("user:8080");
    in.policy = policy(manual("policy:8080"));

    auto d = decide(in, kUrl, scripts.fn());
    CHECK(d.routes == Routes{via("policy", 8080)});
    CHECK(d.tier == Tier::fairyfly_policy);
    CHECK(d.locked);
    CHECK(d.setting_from == "policy");

    in.policy = {};
    d = decide(in, kUrl, scripts.fn());
    CHECK(d.routes == Routes{via("user", 8080)});
    CHECK(d.tier == Tier::user_setting);
    CHECK_FALSE(d.locked);
    CHECK(d.setting_from == "user");

    in.user.reset();
    d = decide(in, kUrl, scripts.fn());
    CHECK(d.routes == Routes{via("env", 3128)});
    CHECK(d.tier == Tier::environment);

    in.env = {};
    d = decide(in, kUrl, scripts.fn());
    CHECK(d.routes == Routes{via("edge", 8080)});  // Edge before Chrome
    CHECK(d.source == "Edge policy (HKLM)");
    CHECK(first_origin(d) == Origin::configured);  // set by policy: explicit

    in.browsers.erase(in.browsers.begin());
    CHECK(decide(in, kUrl, scripts.fn()).routes == Routes{via("chrome", 8080)});

    in.browsers.clear();
    d = decide(in, kUrl, scripts.fn());
    CHECK(d.routes == Routes{via("windows", 8080)});
    CHECK(d.tier == Tier::windows);
    CHECK(d.setting_from == "default");
    CHECK(scripts.asked.empty());  // nothing evaluated a script on the way
}

TEST_CASE("proxy config: the fairyfly policy in any mode (Auto, invalid) skips the user's setting and HTTPS_PROXY/NO_PROXY") {
    Inputs in;
    in.env.https_proxy = "http://env:3128";
    in.env.no_proxy = "fairyfly.io";
    in.user = manual("user:8080");
    in.windows.proxy = "windows:8080";
    Setting automatic;
    in.policy = policy(automatic);
    auto d = decide(in, kUrl, {});
    CHECK(d.routes == Routes{via("windows", 8080)});  // a variable of the user's process does not beat the policy
    CHECK(d.tier == Tier::windows);
    CHECK(d.locked);
    CHECK(d.effective.mode == Mode::automatic);

    in.policy = PolicySetting{true, {}, "ProxyMode must be auto, manual, pac or direct"};
    d = decide(in, kUrl, {});
    CHECK(d.routes == Routes{via("windows", 8080)});
    CHECK(d.locked);
    CHECK_FALSE(d.policy_error.empty());

    in.policy = {};  // without the policy, the user's setting, then the environment
    CHECK(decide(in, kUrl, {}).routes == Routes{via("user", 8080)});
    in.user.reset();
    CHECK(decide(in, kUrl, {}).source == "NO_PROXY");
}

TEST_CASE("proxy config: Direct wins over everything below it; the bypass list of a manual proxy") {
    Inputs in;
    in.env.https_proxy = "http://env:3128";
    in.windows.proxy = "windows:8080";
    in.user = direct();
    auto d = decide(in, kUrl, {});
    CHECK(d.routes.empty());
    CHECK(route_text(d, false) == "direct (your fairyfly setting: Direct)");

    in.policy = policy(direct());
    CHECK(route_text(decide(in, kUrl, {}), false) == "direct (fairyfly policy: Direct)");

    in.policy = {};
    in.user = manual("user:8080", "*.fairyfly.io;fairyfly.io");
    CHECK(decide(in, kUrl, {}).routes.empty());
    CHECK(decide(in, "https://github.com/", {}).routes == Routes{via("user", 8080)});
}

TEST_CASE("proxy config: NO_PROXY and HTTPS_PROXY rank below the fairyfly setting") {
    Inputs in;
    in.windows.proxy = "windows:8080";
    in.env.no_proxy = ".fairyfly.io,fairyfly.io";
    in.env.https_proxy = "http://env:3128";
    auto d = decide(in, kUrl, {});
    CHECK(d.routes.empty());
    CHECK(d.source == "NO_PROXY");
    CHECK(route_text(d, false) == "direct (NO_PROXY)");
    CHECK(decide(in, "https://github.com/", {}).routes == Routes{via("env", 3128)});

    in.user = manual("user:8080");  // the user's setting is above NO_PROXY: it has its own exceptions
    CHECK(decide(in, kUrl, {}).routes == Routes{via("user", 8080)});

    in.user.reset();
    in.env.no_proxy.clear();
    in.env.https_proxy = "socks5://env:1080";
    d = decide(in, kUrl, {});
    CHECK_THAT(d.error, ContainsSubstring("HTTPS_PROXY"));
    CHECK_THAT(route_text(d, false), ContainsSubstring("unusable"));
    in.env.https_proxy = "http://alice:s3cret@env:3128";
    d = decide(in, kUrl, {});
    CHECK(d.routes == Routes{via("env", 3128)});
    CHECK(to_json(d, false).dump().find("s3cret") == std::string::npos);
}

TEST_CASE("proxy config: browser policies - the first browser decides, \"system\" hands over to Windows") {
    using K = BrowserPolicy::Kind;
    Scripts scripts;
    Inputs in;
    in.windows.proxy = "windows:8080";
    in.browsers = {browser("Edge", K::system), browser("Chrome", K::fixed, "chrome:8080")};
    auto d = decide(in, kUrl, scripts.fn());
    CHECK(d.routes == Routes{via("windows", 8080)});  // Edge says "use the system's": Chrome is not asked
    CHECK(d.tier == Tier::windows);

    in.browsers = {browser("Chrome", K::direct), browser("Firefox", K::fixed, "ff:3128")};
    d = decide(in, kUrl, scripts.fn());
    CHECK(d.routes.empty());
    CHECK(d.source == "Chrome policy (HKLM)");

    in.browsers = {browser("Firefox", K::auto_detect)};
    d = decide(in, kUrl, scripts.fn());
    CHECK(scripts.asked.back() == "wpad");
    CHECK(first_origin(d) == Origin::discovered);  // set by policy, still found by WPAD: no login
    CHECK(route_text(d, false) == "via script:3128 from automatic detection (WPAD, Firefox policies.json), Windows login: no");
    CHECK(route_text(d, true).find("Windows login: yes") != std::string::npos);

    in.browsers = {browser("Edge", K::fixed, "edge:8080")};
    in.browsers[0].bypass = "*.fairyfly.io";
    CHECK(decide(in, "https://link.fairyfly.io/ffl", {}).routes.empty());
}

TEST_CASE("proxy config: a PAC script's proxies get the Windows login only when the script is trusted, by tier") {
    using K = BrowserPolicy::Kind;
    Scripts scripts;
    auto origin_for = [&](Inputs in) { return first_origin(decide(in, kUrl, scripts.fn())); };

    Inputs in;
    in.user = pac("https://pac.corp.example/p.pac");
    CHECK(origin_for(in) == Origin::pac);
    CHECK(scripts.asked.back() == "pac https://pac.corp.example/p.pac trusted");
    in.user = pac("http://pac.corp.example/p.pac");  // the user's http:// script: anyone on the path could answer it
    CHECK(origin_for(in) == Origin::discovered);
    CHECK(scripts.asked.back() == "pac http://pac.corp.example/p.pac");

    in.user.reset();
    in.policy = policy(pac("http://pac.corp.example/p.pac"));  // the administrator's, dotted host: trusted
    CHECK(origin_for(in) == Origin::pac);
    in.policy = policy(pac("http://wpad/p.pac"));  // single label: never
    CHECK(origin_for(in) == Origin::discovered);
    in.policy = policy(pac("http://10.0.0.5/p.pac"));  // an address says nothing about who set it up
    CHECK(origin_for(in) == Origin::discovered);

    in.policy = {};
    in.browsers = {browser("Edge", K::pac, "http://pac.corp.example/p.pac")};
    CHECK(origin_for(in) == Origin::pac);
    in.browsers = {browser("Firefox", K::pac, "http://pac.corp.example/p.pac", false)};  // the user's own (HKCU) policy
    CHECK(origin_for(in) == Origin::discovered);
    in.browsers = {browser("Firefox", K::pac, "https://pac.corp.example/p.pac", false)};
    CHECK(origin_for(in) == Origin::pac);
    in.browsers.back().untrusted = true;  // a policies.json other users can change: not even an https:// script
    CHECK(origin_for(in) == Origin::discovered);
    CHECK(scripts.asked.back() == "pac https://pac.corp.example/p.pac");  // and no login while downloading it

    // the route says where the script came from
    in.browsers = {browser("Edge", K::pac, "https://pac.corp.example/p.pac?token=abc")};
    auto d = decide(in, kUrl, scripts.fn());
    CHECK(route_text(d, false) == "via script:3128 from PAC (https://pac.corp.example/p.pac, Edge policy (HKLM)), Windows login: yes");

    // owner decision: a script that fails hands over to the next source, which keeps its own trust, and says so
    scripts.fail = true;
    in.windows.proxy = "windows:8080";
    d = decide(in, kUrl, scripts.fn());
    CHECK(d.routes == Routes{via("windows", 8080)});
    CHECK(d.tier == Tier::windows);
    CHECK(d.warning ==
          "PAC https://pac.corp.example/p.pac failed (the proxy script (PAC) could not be downloaded), using next source: Windows proxy settings");
    CHECK(to_json(d, false)["warning"] == d.warning);

    // the fairyfly policy's script fails: the browser policy is next (the user's setting and HTTPS_PROXY are not)
    in.policy = policy(pac("https://pac.corp.example/admin.pac"));
    in.user = manual("user:8080");
    in.env.https_proxy = "http://env:3128";
    in.browsers = {browser("Edge", K::pac, "https://pac.corp.example/edge.pac"), browser("Chrome", K::fixed, "chrome:8080")};
    d = decide(in, kUrl, scripts.fn());
    CHECK(d.routes == Routes{via("chrome", 8080)});
    CHECK(first_origin(d) == Origin::configured);
    CHECK(d.warning == "PAC https://pac.corp.example/admin.pac failed (the proxy script (PAC) could not be downloaded); PAC "
                       "https://pac.corp.example/edge.pac failed (the proxy script (PAC) could not be downloaded), using next source: "
                       "Chrome policy (HKLM)");
    // detection by policy that finds nothing hands over too, and WPAD's proxies still get nothing when it does
    in.policy = {};
    in.user = pac("https://pac.corp.example/mine.pac");
    in.env = {};
    in.browsers = {browser("Firefox", K::auto_detect)};
    d = decide(in, kUrl, scripts.fn());
    CHECK(d.tier == Tier::windows);
    CHECK_THAT(d.warning, ContainsSubstring("automatic detection (WPAD) failed"));
    CHECK_THAT(d.warning, ContainsSubstring("using next source: Windows proxy settings"));
}

TEST_CASE("proxy config: a policies.json other users can change gives its proxy no credentials, and says so") {
    using K = BrowserPolicy::Kind;
    Inputs in;
    in.browsers = {browser("Firefox", K::fixed, "ff.corp.example:3128", false)};
    in.browsers[0].untrusted = true;
    auto d = decide(in, kUrl, {});
    CHECK(d.routes == Routes{via("ff.corp.example", 3128)});
    CHECK(first_origin(d) == Origin::discovered);
    CHECK_FALSE(windows_login(d, false));
    CHECK_FALSE(stored_basic_allowed(d));
    CHECK_THAT(d.warning, ContainsSubstring("can be changed by users other than administrators"));
    CHECK(d.source == "Firefox policies.json (user-writable)");
    CHECK(route_text(d, false) == "via ff.corp.example:3128 from Firefox policies.json (user-writable), Windows login: no");
    // the user's own HKCU policy (owner decision): explicit, like the user's fairyfly setting
    in.browsers[0].untrusted = false;
    in.browsers[0].where = "HKCU\\Software\\Policies\\Mozilla\\Firefox\\Proxy";
    d = decide(in, kUrl, {});
    CHECK(first_origin(d) == Origin::configured);
    CHECK(d.warning.empty());
}

TEST_CASE("proxy config: the Windows settings decide last, as before, and a trusted Windows script is said so") {
    Scripts scripts;
    Inputs in;
    in.windows.auto_config_url = "https://pac.corp.example/p.pac";
    in.windows.proxy = "manual:8080";
    auto d = decide(in, kUrl, scripts.fn());
    CHECK(d.routes == Routes{via("script", 3128)});
    CHECK(first_origin(d) == Origin::pac);
    CHECK(scripts.asked.back() == "pac https://pac.corp.example/p.pac trusted");
    CHECK(d.pac_url == "https://pac.corp.example/p.pac");
    CHECK(route_text(d, false) == "via script:3128 from PAC (https://pac.corp.example/p.pac, Windows proxy settings), Windows login: yes");

    // the script fails and detection decides: the route does not claim the script
    in.windows.auto_detect = true;
    in.windows.proxy.clear();
    int calls = 0;
    d = decide(in, kUrl, [&](const std::string&, AutoSource src, const std::string&, bool) {
        ++calls;
        AutoProxyResult r;
        if (src == AutoSource::pac) {
            r.error = "timeout";
            return r;
        }
        r.ok = true;
        r.proxy = "wpad-proxy:8080";
        return r;
    });
    CHECK(calls == 2);
    CHECK(d.pac_url.empty());
    CHECK(first_origin(d) == Origin::discovered);
    CHECK(route_text(d, false) ==
          "via wpad-proxy:8080 from Windows proxy settings (PAC https://pac.corp.example/p.pac failed (timeout), automatic detection (WPAD)), Windows login: no");

    Inputs m;
    m.machine = fairyfly::link::sysproxy::ProxySettings{};
    m.machine->proxy = "machine:3128";
    d = decide(m, kUrl, {});
    CHECK(route_text(d, false) == "via machine:3128 from Windows proxy settings (WinHTTP machine setting (netsh winhttp)), Windows login: yes");
    CHECK(stored_basic_allowed(d));
    CHECK(route_text(decide(Inputs{}, kUrl, {}), false) == "direct (Windows proxy settings: no proxy configured)");
}

TEST_CASE("proxy config: the JSON of a decision names routes, origins and trust, never HTTPS_PROXY's credentials") {
    Inputs in;
    in.user = manual("proxy:8080");
    auto j = to_json(decide(in, kUrl, {}), false);
    CHECK(j["routes"][0]["proxy"] == "proxy:8080");
    CHECK(j["routes"][0]["origin"] == "configured");
    CHECK(j["windows_login"] == true);
    CHECK(j["stored_basic"] == true);
    CHECK(j["tier"] == "your fairyfly setting");
}

TEST_CASE("proxy config: the resolver caches per host, drops the cache when the setting changes, and honours a candidate") {
    std::atomic<int> reads{0};
    std::string fingerprint = "a";
    std::optional<Setting> user = manual("one:8080");
    RouteResolver::Sources src;
    src.inputs = [&] {
        ++reads;
        Inputs in;
        in.user = user;
        return in;
    };
    src.fingerprint = [&] { return fingerprint; };
    RouteResolver r(src);
    CHECK_FALSE(r.setting_changed());  // the first call records
    CHECK(r.routes(kUrl) == Routes{via("one", 8080)});
    CHECK(r.routes("wss://fairyfly.io/ffl") == Routes{via("one", 8080)});  // its own key (wss://)
    const int after = reads;
    CHECK(r.routes(kUrl) == Routes{via("one", 8080)});
    CHECK(reads == after);  // cached

    user = manual("two:8080");
    fingerprint = "b";  // e.g. `fairyfly proxy set` in another process
    CHECK(r.routes(kUrl) == Routes{via("two", 8080)});  // the fingerprint is part of the cache key
    CHECK(r.setting_changed());
    CHECK_FALSE(r.setting_changed());

    {
        ScopedCandidate c(manual("candidate:3128"));
        CHECK(r.routes(kUrl) == Routes{via("candidate", 3128)});
        Routes elsewhere;
        std::thread([&] { elsewhere = r.routes(kUrl); }).join();
        CHECK(elsewhere == Routes{via("two", 8080)});  // other threads (the running link): the saved one
        ScopedCandidate d(direct());
        CHECK(r.routes(kUrl).empty());
    }
    CHECK(r.routes(kUrl) == Routes{via("two", 8080)});
    CHECK(ScopedCandidate::current() == nullptr);
    CHECK(r.decision(kUrl, direct()).routes.empty());

    user.reset();
    src.inputs = [] {
        Inputs in;
        in.env.https_proxy = "socks://x:1";
        return in;
    };
    RouteResolver bad(src);
    CHECK_THROWS_WITH(bad.routes(kUrl), ContainsSubstring("HTTPS_PROXY"));
}

TEST_CASE("proxy config: libffl's configuration - HTTPS_PROXY's user:password only for that proxy, logins by origin") {
    EnvOverrides env;
    env.https_proxy = "http://alice:pw@env:3128";
    auto inputs = std::make_shared<Inputs>();
    inputs->env = env;
    RouteResolver::Sources src;
    src.inputs = [inputs] { return *inputs; };
    auto resolver = std::make_shared<RouteResolver>(src);
    int sspi_made = 0;
    AuthSources auth;
    auth.sspi = [&](const ffy::proxy::Endpoint&) -> std::unique_ptr<ffy::proxy::Authenticator> {
        ++sspi_made;
        return nullptr;
    };
    auto c = make_libffl_config(resolver, env, auth);
    CHECK(c.source == kConfigSource);
    REQUIRE(c.authenticators.size() == 2);  // login, HTTPS_PROXY's Basic (no stored-login reader given)
    ffy::proxy::Endpoint env_proxy{"env", 3128, Origin::configured};
    ffy::proxy::Endpoint other{"other", 3128, Origin::configured};
    auto basic = c.authenticators[1](env_proxy);
    REQUIRE(basic);
    CHECK(basic->respond({"Basic realm=x"}).value_or("") == "Basic " + ffy::b64std_encode("alice:pw"));
    CHECK_FALSE(c.authenticators[1](other));
    CHECK_FALSE(c.authenticators[1](ffy::proxy::Endpoint{"env", 3128, Origin::pac}));  // same address from a PAC script
    // only while HTTPS_PROXY decides: not when your setting (also one under test) or the policy names that proxy
    inputs->user = manual("env:3128");
    CHECK_FALSE(c.authenticators[1](env_proxy));
    inputs->user.reset();
    {
        ScopedCandidate cand(manual("env:3128"));
        CHECK_FALSE(c.authenticators[1](env_proxy));
    }
    inputs->policy = policy(Setting{});
    CHECK_FALSE(c.authenticators[1](env_proxy));
    inputs->policy = {};
    CHECK(c.authenticators[1](env_proxy));
    c.authenticators[0](env_proxy);
    CHECK(sspi_made == 1);
    auto refused = c.authenticators[0](ffy::proxy::Endpoint{"wpad-proxy", 8080, Origin::discovered});
    REQUIRE(refused);  // the NoLogin authenticator: it says why
    CHECK_FALSE(refused->respond({"NTLM"}));
    CHECK_THAT(refused->declined(), ContainsSubstring("discovered automatically"));
    CHECK(sspi_made == 1);
}

TEST_CASE("proxy config: Test connection targets - the pairing's link address, else the gateway's") {
    auto t = probe_targets("wss://link.fairyfly.io/ffl", "https://ignored.example");
    CHECK(t.https_url == "https://link.fairyfly.io/health");
    CHECK(t.wss_url == "wss://link.fairyfly.io/ffl");
    CHECK(t.note.empty());
    t = probe_targets("", "https://gw.example:8443");
    CHECK(t.https_url == "https://gw.example:8443/health");
    CHECK(t.wss_url == "wss://gw.example:8443/ffl");
    CHECK_FALSE(t.note.empty());
}
