// The portable part of the tray's "Network / proxy..." window (include/tray/proxy_window.h): its form and the
// process's services behind it.
#include "include/tray/proxy_window.h"

#include "include/link/proxy_cli.h"

namespace fairyfly::tray {

using link::proxycfg::Mode;
using link::proxycfg::Setting;

Setting form_setting(const ProxyForm& f) {
    Setting s;
    s.mode = f.mode;
    if (f.mode == Mode::manual) {
        s.server = f.server;
        s.bypass = f.bypass;
    }
    if (f.mode == Mode::pac) s.pac_url = f.pac_url;
    return link::proxycfg::normalised(s);
}

std::optional<std::string> form_error(const ProxyForm& f) {
    Setting s;
    s.mode = f.mode;
    s.server = f.server;
    s.bypass = f.bypass;
    s.pac_url = f.pac_url;
    return link::proxycfg::check(s);
}

ProxyWindowServices make_proxy_window_services(std::function<void(const std::string&)> notify) {
    namespace pc = link::proxycfg;
    ProxyWindowServices w;
    w.load = [] {
        auto s = pc::process_services();
        const pc::Inputs in = s.inputs ? s.inputs() : pc::Inputs{};
        ProxyWindowState st;
        st.locked = in.policy.present;
        if (st.locked) {
            st.locked_text = pc::kLockedText;
            if (in.policy.error.empty()) st.setting = in.policy.setting;
        } else if (in.user) {
            st.setting = *in.user;
        }
        for (const auto& x : in.warnings) st.warnings += (st.warnings.empty() ? "" : " ") + x;
        return st;
    };
    w.route = [](const Setting& candidate) {
        auto s = pc::process_services();
        pc::Inputs in = s.inputs ? s.inputs() : pc::Inputs{};
        in.user = candidate.mode == Mode::automatic ? std::nullopt : std::optional<Setting>(candidate);
        const auto t = s.targets ? s.targets() : pc::probe_targets("", "https://fairyfly.io");
        return pc::route_text(pc::decide(in, t.https_url, s.autoproxy), in.discovered_login_policy);
    };
    w.save = [](const Setting& candidate) -> std::optional<std::string> {
        auto s = pc::process_services();
        const pc::Inputs in = s.inputs ? s.inputs() : pc::Inputs{};
        if (in.policy.present) return std::string("The proxy is set by your administrator and cannot be changed here.");
        if (auto e = pc::check(candidate)) return *e;
        if (!s.save || !s.save(candidate)) return std::string("The proxy setting could not be saved.");
        if (s.changed) s.changed();
        return std::nullopt;
    };
    w.test = [](const Setting& candidate) {
        auto s = pc::process_services();
        const pc::Inputs in = s.inputs ? s.inputs() : pc::Inputs{};
        const auto t = s.targets ? s.targets() : pc::probe_targets("", "https://fairyfly.io");
        if (!s.probe) return pc::ProbeResult{};
        return s.probe(t, in.policy.present ? std::nullopt : std::optional<Setting>(candidate));
    };
    w.saved = [notify] {
        if (notify) notify("Proxy setting saved. The cloud link reconnects along the new route.");
    };
    return w;
}

}  // namespace fairyfly::tray
