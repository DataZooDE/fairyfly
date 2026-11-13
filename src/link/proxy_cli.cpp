// `fairyfly proxy show|set|test|reset` (include/link/proxy_cli.h).
#include "include/link/proxy_cli.h"

#include <cstdlib>

#include "include/config/environment.h"
#include "include/link/link_runner.h"
#ifdef _WIN32
#include "include/link/proxy_sources.h"
#endif

#include "link_state.hpp"  // libffl
#include "util.hpp"        // libffl

namespace fairyfly::link::proxycfg {

using nlohmann::json;

std::optional<Setting> setting_from_args(const CliArgs& a, std::string& error) {
    if (!a.mode) {
        error = "--mode is required (auto, manual, pac or direct)";
        return std::nullopt;
    }
    auto m = parse_mode(*a.mode);
    if (!m) {
        error = "--mode must be auto, manual, pac or direct";
        return std::nullopt;
    }
    if ((a.server || a.bypass) && *m != Mode::manual) {
        error = "--server and --bypass belong to --mode manual";
        return std::nullopt;
    }
    if (a.pac && *m != Mode::pac) {
        error = "--pac belongs to --mode pac";
        return std::nullopt;
    }
    Setting s;
    s.mode = *m;
    s.server = a.server.value_or("");
    s.bypass = a.bypass.value_or("");
    s.pac_url = a.pac.value_or("");
    if (*m == Mode::manual && !a.server) {
        error = "--mode manual needs --server host:port";
        return std::nullopt;
    }
    if (*m == Mode::pac && !a.pac) {
        error = "--mode pac needs --pac http(s)://host/proxy.pac";
        return std::nullopt;
    }
    if (auto e = check(s)) {
        error = *e;
        return std::nullopt;
    }
    return normalised(s);
}

namespace {

json setting_json(const Setting& s) {
    json j{{"mode", mode_name(s.mode)}};
    if (s.mode == Mode::manual) {
        j["server"] = s.server;
        j["bypass"] = s.bypass;
    }
    if (s.mode == Mode::pac) j["pac_url"] = s.pac_url;
    return j;
}

std::string kind_name(BrowserPolicy::Kind k) {
    switch (k) {
    case BrowserPolicy::Kind::direct: return "direct";
    case BrowserPolicy::Kind::system: return "system";
    case BrowserPolicy::Kind::auto_detect: return "auto_detect";
    case BrowserPolicy::Kind::pac: return "pac";
    case BrowserPolicy::Kind::fixed: return "fixed";
    }
    return "";
}

}  // namespace

json show_json(const Inputs& in, const RouteDecision& d, const ProbeTargets& t) {
    json setting = setting_json(d.effective);
    setting["from"] = d.setting_from;
    json j{{"setting", setting}, {"locked", d.locked}, {"route", to_json(d, in.discovered_login_policy)}, {"for", t.https_url}};
    if (d.locked) j["locked_by"] = "HKLM\\SOFTWARE\\Policies\\DataZoo\\fairyfly";
    if (!d.policy_error.empty()) j["policy_error"] = d.policy_error;
    json browsers = json::array();
    for (const auto& b : in.browsers) {
        json bj{{"policy", b.label()}, {"mode", kind_name(b.kind)}, {"where", b.where}, {"admin", b.admin}};
        if (!b.server.empty()) bj["server"] = b.server;
        if (!b.pac_url.empty()) bj["pac_url"] = b.pac_url;
        browsers.push_back(bj);
    }
    j["sources"] = json{{"https_proxy_set", !ffy::trim(in.env.https_proxy).empty()},  // never the value (user:password@)
                        {"no_proxy", in.env.no_proxy},
                        {"browser_policies", browsers},
                        {"windows_pac", in.windows.auto_config_url},
                        {"windows_auto_detect", in.windows.auto_detect},
                        {"windows_manual", in.windows.proxy},
                        {"winhttp_machine", in.machine ? in.machine->proxy : std::string()},
                        {"login_to_discovered_proxies", in.discovered_login_policy}};
    if (!in.warnings.empty()) j["warnings"] = in.warnings;
    return j;
}

Result run_cli(const CliArgs& a, Services& s) {
    const ProbeTargets t = s.targets ? s.targets() : probe_targets("", "https://fairyfly.io");
    auto current = [&](const std::optional<Setting>& candidate = std::nullopt) {
        Inputs in = s.inputs ? s.inputs() : Inputs{};
        if (candidate) in.user = candidate->mode == Mode::automatic ? std::nullopt : std::optional<Setting>(*candidate);
        RouteDecision d = decide(in, t.https_url, s.autoproxy);
        return std::make_pair(in, d);
    };
    if (a.verb == "show") {
        auto [in, d] = current();
        return Result::ok(show_json(in, d, t));
    }
    if (a.verb == "set" || a.verb == "reset" || (a.verb == "test" && a.mode)) {
        Inputs in = s.inputs ? s.inputs() : Inputs{};
        if (in.policy.present)
            return Result::fail("PROXY_SETTING_LOCKED",
                                "the proxy setting is set by your administrator (HKLM\\SOFTWARE\\Policies\\DataZoo\\fairyfly) and cannot be changed here",
                                json::array({"fairyfly proxy show", "ask IT to change the fairyfly policy"}));
    }
    if (a.verb == "set" || a.verb == "reset") {
        Setting next;
        if (a.verb == "set") {
            std::string error;
            auto parsed = setting_from_args(a, error);
            if (!parsed) return Result::fail("INVALID_ARGUMENT", error, json::array({kUnsupportedText}));
            next = *parsed;
        }
        if (!s.save || !s.save(next)) return Result::fail("PROXY_SETTING_NOT_SAVED", "the proxy setting could not be saved");
        if (s.changed) s.changed();
        auto [in, d] = current();
        json data = show_json(in, d, t);
        data["saved"] = true;
        data["next"] = "a running link reconnects along the new route within a second; check it with: fairyfly proxy test";
        return Result::ok(data);
    }
    if (a.verb == "test") {
        std::optional<Setting> candidate;
        if (a.mode) {
            std::string error;
            candidate = setting_from_args(a, error);
            if (!candidate) return Result::fail("INVALID_ARGUMENT", error, json::array({kUnsupportedText}));
        }
        auto [in, d] = current(candidate);
        json data = show_json(in, d, t);
        if (candidate) data["tested_setting"] = setting_json(*candidate);
        if (!s.probe) return Result::fail("PROXY_TEST_UNAVAILABLE", "the connection test is not available here");
        const ProbeResult p = s.probe(t, candidate);
        data["test"] = p.to_json();
        if (!t.note.empty()) data["test"]["note"] = t.note;
        if (p.ok) return Result::ok(data);
        Result r = Result::fail("PROXY_TEST_FAILED", p.summary,
                                json::array({"fairyfly proxy show", "docs/LINK.md \"Corporate proxies\" (what IT must allow)"}));
        r.data = data;
        return r;
    }
    return Result::fail("NO_SUBCOMMAND", "No proxy subcommand specified (show, set, test or reset)");
}

Services process_services(const std::string& state_path_in) {
    Services s;
#ifdef _WIN32
    sysproxy::install_system_proxy();
#endif
    auto resolver = process_resolver();
    if (resolver) {
        s.inputs = resolver->sources().inputs;
        s.autoproxy = resolver->sources().autoproxy;
        s.changed = [resolver] { resolver->invalidate(); };
    }
#ifdef _WIN32
    s.save = [](const Setting& x) { return win::write_user_setting(x); };
#endif
    const std::string state_path = state_path_in.empty() ? default_state_path() : state_path_in;
    s.targets = [state_path] {
        std::string link_url;
        try {
            link_url = ffy::link::load_state(state_path).link_url;
        } catch (const std::exception&) {
        }
        std::string gateway = config::process_env()(config::env::kLinkGateway);
        if (gateway.empty()) gateway = "https://fairyfly.io";  // link_console.h kDefaultGateway
        try {
            return probe_targets(link_url, gateway);
        } catch (const std::exception&) {
            return probe_targets("", "https://fairyfly.io");
        }
    };
    s.probe = [resolver](const ProbeTargets& t, const std::optional<Setting>& candidate) {
        std::optional<ScopedCandidate> scoped;
        if (candidate) scoped.emplace(*candidate);
        bool policy = false;
        std::function<RouteDecision(const std::string&)> decision;
        if (resolver) {
            decision = [resolver, candidate](const std::string& url) { return resolver->decision(url, candidate); };
            policy = resolver->sources().inputs ? resolver->sources().inputs().discovered_login_policy : false;
        }
        return probe(t, 8000, decision, policy);
    };
    return s;
}

}  // namespace fairyfly::link::proxycfg
