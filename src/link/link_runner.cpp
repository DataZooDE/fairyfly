#include "include/link/link_runner.h"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include <spdlog/spdlog.h>

#include "include/link/link_control.h"
#include "include/link/main_thread_tools.h"
#include "include/mcp/run_mcp.h"
#include "include/version.h"

// libffl
#include "link_agent.hpp"
#include "link_pair.hpp"
#include "link_state.hpp"
#include "qr.hpp"
#include "util.hpp"

namespace fairyfly::link {

using json = nlohmann::json;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

Result error(const std::string& code, const std::string& message) {
    Result r;
    r.status = Result::Status::Error;
    r.error["code"] = code;
    r.error["message"] = message;
    return r;
}

std::string state_path(const LinkOptions& o) { return o.state_path.empty() ? default_state_path() : o.state_path; }

std::string host_name() {
    std::string h = ffy::env_or("COMPUTERNAME");
    return h.empty() ? ffy::env_or("HOSTNAME", "unknown") : h;
}

json tools_json(const mcp::ToolProvider& provider) {
    json out = json::array();
    for (const auto& t : provider.list_tools()) {
        json d = {{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}};
        if (!t.title.empty()) d["title"] = t.title;
        if (!t.annotations.is_null()) d["annotations"] = t.annotations;
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace

std::string default_state_path() {
#ifdef _WIN32
    std::string base = ffy::env_or("LOCALAPPDATA", ".");
#else
    std::string base = ffy::env_or("XDG_CONFIG_HOME");
    if (base.empty()) base = ffy::env_or("HOME", ".") + "/.config";
#endif
    return (std::filesystem::path(base) / "fairyfly" / "link" / "state.json").string();
}

Result link_pair(const LinkOptions& options) {
    LinkOptions o = options;
    if (o.gateway.empty()) o.gateway = ffy::env_or("FAIRYFLY_LINK_GATEWAY");
    if (o.gateway.empty()) {  // started from the tray: ask in the console
        std::cerr << "Web address of the fairyfly service (from your administrator, e.g. https://app.fairyfly.cloud): " << std::flush;
        std::getline(std::cin, o.gateway);
        while (!o.gateway.empty() && (o.gateway.back() == ' ' || o.gateway.back() == '\r')) o.gateway.pop_back();
    }
    if (o.gateway.rfind("https://", 0) != 0 && o.gateway.rfind("http://127.0.0.1", 0) != 0 && o.gateway.rfind("http://localhost", 0) != 0)
        return error("INVALID_ARGUMENT", "the gateway address must start with https://, e.g. https://app.fairyfly.cloud");
    try {
        ffy::link::PairingClient pc(o.gateway, host_name(), "Windows", fairyfly::FAIRYFLY_VERSION);
        auto shown = pc.start();
        auto m = ffy::qr::encode(shown.link);
        // stderr: stdout carries the command's JSON result
        std::cerr << "\nScan this code with your phone, sign in, and type the number " << shown.match << ":\n\n"
                  << ffy::qr::to_terminal(m) << "\nOr open " << o.gateway << "/pair and type the code " << shown.code << "\n"
                  << "Number to type on the phone: " << shown.match << "\n"
                  << "Only approve on your own phone. Expires in " << shown.expires_in / 60 << " minutes.\n" << std::flush;
        if (!o.qr_png.empty()) std::ofstream(o.qr_png, std::ios::binary) << ffy::qr::to_png(m);
        auto st = pc.wait(o.pair_timeout_s);
        if (!st) return error(pc.status() == "expired" ? "PAIRING_EXPIRED" : "PAIRING_FAILED", "pairing " + pc.status() + " - start again with: fairyfly link pair --gateway " + o.gateway);
        if (!ffy::link::confirm_pairing(*st, std::cin, std::cerr)) return error("PAIRING_NOT_CONFIRMED", "pairing not confirmed on this desktop - nothing was saved");
        ffy::link::save_state(*st, state_path(o));
        Result r;
        r.status = Result::Status::Success;
        r.data = {{"device", st->device_id}, {"account", st->email}, {"organisation", st->tenant_name.empty() ? st->tenant : st->tenant_name},
                  {"organisation_id", st->tenant}, {"gateway", st->link_url}, {"state", state_path(o)}, {"next", "fairyfly link run"}};
        return r;
    } catch (const std::exception& e) {
        return error("PAIRING_FAILED", e.what());
    }
}

Result link_status(const LinkOptions& o) {
    try {
        auto st = ffy::link::load_state(state_path(o));
        Result r;
        r.status = Result::Status::Success;
        r.data = {{"paired", true}, {"device", st.device_id}, {"account", st.email}, {"organisation_id", st.tenant},
                  {"organisation", st.tenant_name}, {"gateway", st.link_url}, {"pinned_gateway_keys", st.gateway_keys.size()}, {"state", state_path(o)},
                  {"paused", is_paused(state_path(o))}, {"link", describe(read_status(state_path(o)), ffy::now_unix())}};
        return r;
    } catch (const std::exception&) {
        Result r;
        r.status = Result::Status::Success;
        r.data = {{"paired", false}, {"state", state_path(o)}, {"next", "fairyfly link pair --gateway <address>"}};
        return r;
    }
}

Result link_unpair(const LinkOptions& o) {
    std::error_code ec;
    bool removed = std::filesystem::remove(state_path(o), ec);
    if (ec) return error("UNPAIR_FAILED", ec.message());
    Result r;
    r.status = Result::Status::Success;
    r.data = {{"removed", removed}, {"note", "also remove the desktop in the web app (Overview -> Desktops) so the gateway forgets it"}};
    return r;
}

Result link_pause(const LinkOptions& o) {
    if (!set_paused(state_path(o), true)) return error("PAUSE_FAILED", "could not write " + pause_marker(state_path(o)).string());
    Result r;
    r.status = Result::Status::Success;
    r.data = {{"paused", true}, {"link", describe(read_status(state_path(o)), ffy::now_unix())},
              {"note", "a running link disconnects within a second; the AI assistant cannot reach this desktop until: fairyfly link resume"}};
    return r;
}

Result link_resume(const LinkOptions& o) {
    if (!set_paused(state_path(o), false)) return error("RESUME_FAILED", "could not remove " + pause_marker(state_path(o)).string());
    Result r;
    r.status = Result::Status::Success;
    r.data = {{"paused", false}, {"link", describe(read_status(state_path(o)), ffy::now_unix())}};
    return r;
}

namespace {
class StateLinkControl : public tray::LinkControl {
public:
    explicit StateLinkControl(std::string path) : path_(std::move(path)) {}
    tray::LinkView view() override {
        tray::LinkView v;
        v.paused = is_paused(path_);
        try {
            auto st = ffy::link::load_state(path_);
            v.paired = true;
            v.organisation = st.tenant_name.empty() ? st.tenant : st.tenant_name;
            v.account = st.email;
        } catch (const std::exception&) {
            return v;  // not paired (or the state is unreadable for this Windows user)
        }
        v.state = describe(read_status(path_), ffy::now_unix());
        return v;
    }
    bool set_paused(bool paused) override { return fairyfly::link::set_paused(path_, paused); }

private:
    std::string path_;
};
}  // namespace

std::unique_ptr<tray::LinkControl> make_link_control(const std::string& path) {
    return std::make_unique<StateLinkControl>(path.empty() ? default_state_path() : path);
}

int run_link(const LinkOptions& o, const std::function<cli::CommandHandler&()>& get_handler, const commands::GlobalOptions& global,
             audit::AuditSink* sink, const std::function<cli::CommandHandler*()>& peek) {
    ffy::link::State st;
    try {
        st = ffy::link::load_state(state_path(o));
    } catch (const std::exception& e) {
        Result r = error("LINK_NOT_PAIRED", std::string("this desktop is not paired (") + e.what() + "); run: fairyfly link pair --gateway <address>");
        std::cerr << r.to_json().dump() << std::endl;
        return 2;
    }
    auto tools = std::make_shared<MainThreadTools>();
    ffy::link::AgentConfig cfg;
    cfg.local = tools;
    cfg.version = fairyfly::FAIRYFLY_VERSION;
    cfg.approver = o.approve == "no" ? ffy::link::fixed_approver(false) : ffy::link::desktop_approver();
    ffy::link::Agent agent(st, cfg);
    g_stop = false;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    // the link (outbound WebSocket), closed while paused; tool calls come back via `tools`
    const std::string sp = state_path(o);
    std::thread net([&] { supervise(sp, [&agent](const std::atomic<bool>& net_stop) { agent.run(net_stop); }, g_stop); });
    if (is_paused(sp)) std::cerr << "fairyfly link: paused - it connects after: fairyfly link resume" << std::endl;

    mcp::ServeOptions so;
    so.transport = "link";
    so.allow_write = o.allow_write;
    so.read_only = o.read_only;
    so.families = o.families;
    mcp::HttpRunHooks hooks;
    hooks.link_loop = [&](mcp::ToolProvider& provider) -> int {
        tools->set_handlers([&provider] { return tools_json(provider); },
                            [&provider](const std::string& name, const json& args, const ffy::link::CallInfo& info) -> json {
                                mcp::CallContext ctx;
                                ctx.request_id = info.request_id;
                                ctx.cancelled = [] { return g_stop.load(); };
                                // the AI assistant acting for this user of this organisation; read-only unless the
                                // gateway granted a write for exactly this call
                                ctx.principal.name = "link:" + info.tenant + "/" + info.user;
                                ctx.principal.read_only = !info.write_granted;
                                ctx.principal.authenticated = true;
                                provider.set_client_info({{"name", info.client}, {"version", "fairyfly-link"}});
                                mcp::ToolResult r = provider.call_tool(name, args, ctx);
                                json out = {{"content", r.content}, {"isError", r.is_error}};
                                if (r.structured) out["structuredContent"] = *r.structured;
                                return out;
                            });
        spdlog::info("fairyfly link: serving {} for {} ({})", st.tenant_name.empty() ? st.tenant : st.tenant_name, st.email, st.device_id);
        tools->pump(g_stop);  // this is the main (COM) thread
        tools->shutdown();
        return 0;
    };
    const int rc = mcp::run_mcp(so, get_handler, global, sink, peek, &hooks);
    g_stop = true;
    tools->shutdown();
    net.join();
    return rc;
}

}  // namespace fairyfly::link
