#include "include/link/link_runner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "include/link/link_console.h"
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
std::atomic<bool> g_pair_stop{false};
void on_pair_signal(int) { g_pair_stop = true; }
std::atomic<int> g_background{0};  // see background_threads()

/// Installs a signal handler and puts the previous one back.
class SignalScope {
public:
    SignalScope(int sig, void (*handler)(int)) : sig_(sig), previous_(std::signal(sig, handler)) {}
    ~SignalScope() { std::signal(sig_, previous_ == SIG_ERR ? SIG_DFL : previous_); }
    SignalScope(const SignalScope&) = delete;
    SignalScope& operator=(const SignalScope&) = delete;

private:
    int sig_;
    void (*previous_)(int);
};

/// Runs `fn` (a blocking libffl call: HTTP with 15 s timeouts) on its own thread and waits for it, looking at
/// `stop` every 50 ms. On stop it returns nullopt at once; the thread finishes on its own and owns all it uses.
template <class T>
std::optional<T> unless_stopped(std::function<T()> fn, const std::atomic<bool>& stop) {
    struct Shared {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        std::optional<T> value;
        std::exception_ptr error;
    };
    auto shared = std::make_shared<Shared>();
    g_background++;
    std::thread([shared, fn = std::move(fn)] {
        std::optional<T> value;
        std::exception_ptr error;
        try {
            value.emplace(fn());
        } catch (...) {
            error = std::current_exception();
        }
        {
            std::lock_guard<std::mutex> l(shared->mu);
            shared->value = std::move(value);
            shared->error = error;
            shared->done = true;
            shared->cv.notify_all();
        }
        g_background--;
    }).detach();
    std::unique_lock<std::mutex> l(shared->mu);
    while (!shared->done) {
        if (stop) return std::nullopt;
        shared->cv.wait_for(l, std::chrono::milliseconds(50));
    }
    if (shared->error) std::rethrow_exception(shared->error);
    return std::move(shared->value);
}

/// False when `stop` became true before `total` passed.
bool sleep_unless_stopped(std::chrono::milliseconds total, const std::atomic<bool>& stop) {
    const auto end = std::chrono::steady_clock::now() + total;
    while (std::chrono::steady_clock::now() < end) {
        if (stop) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !stop;
}

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

int background_threads() { return g_background.load(); }

std::string default_state_path() {
#ifdef _WIN32
    std::string base = ffy::env_or("LOCALAPPDATA", ".");
#else
    std::string base = ffy::env_or("XDG_CONFIG_HOME");
    if (base.empty()) base = ffy::env_or("HOME", ".") + "/.config";
#endif
    return (std::filesystem::path(base) / "fairyfly" / "link" / "state.json").string();
}

Result link_pair(const LinkOptions& options, const PairIo& io) {
    // stderr (the console): stdout carries the command's JSON result
    std::unique_ptr<ConsoleStream> console;
    if (!io.out) console = std::make_unique<ConsoleStream>();
    std::ostream& out = io.out ? *io.out : *console;
    std::istream& in = io.in ? *io.in : std::cin;
    // Ctrl+C sets the flag (the CRT then restores the default, so a second Ctrl+C ends the process at once)
    g_pair_stop = false;
    std::optional<SignalScope> ctrl_c;
    if (!io.stop) ctrl_c.emplace(SIGINT, on_pair_signal);
    const std::atomic<bool>& stop = io.stop ? *io.stop : g_pair_stop;
    const auto cancelled = [] { return error("PAIRING_CANCELLED", "pairing cancelled - nothing was saved"); };

    LinkOptions o = options;
    if (o.gateway.empty()) o.gateway = ffy::env_or("FAIRYFLY_LINK_GATEWAY");
    if (o.gateway.empty()) {  // started from the tray: ask in the console
        if (stop) return cancelled();
        out <<"Web address of the fairyfly service (from your administrator, e.g. https://app.fairyfly.cloud): " << std::flush;
        std::getline(in, o.gateway);
        if (stop) return cancelled();
        while (!o.gateway.empty() && (o.gateway.back() == ' ' || o.gateway.back() == '\r')) o.gateway.pop_back();
    }
    if (o.gateway.rfind("https://", 0) != 0 && o.gateway.rfind("http://127.0.0.1", 0) != 0 && o.gateway.rfind("http://localhost", 0) != 0)
        return error("INVALID_ARGUMENT", "the gateway address must start with https://, e.g. https://app.fairyfly.cloud");
    while (o.gateway.size() > 8 && o.gateway.back() == '/') o.gateway.pop_back();
    try {
        out << "Contacting " << o.gateway << "\xE2\x80\xA6" << std::endl;
        auto pc = std::make_shared<ffy::link::PairingClient>(o.gateway, host_name(), "Windows", fairyfly::FAIRYFLY_VERSION);
        const auto t0 = std::chrono::steady_clock::now();
        auto shown = unless_stopped<ffy::link::PairingDisplay>([pc] { return pc->start(); }, stop);
        if (!shown) return cancelled();
        spdlog::debug("link pair: {}/pair/start answered after {} ms", o.gateway,
                      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        auto m = ffy::qr::encode(shown->link);
        out << "\nScan this code with your phone, sign in, and type the number " << shown->match << ":\n\n"
            << render_qr(m, choose_qr_style(o.qr_style)) << "\nOr open " << o.gateway << "/pair and type the code " << shown->code << "\n"
            << "Number to type on the phone: " << shown->match << "\n"
            << "Only approve on your own phone. Expires in " << shown->expires_in / 60 << " minutes. Ctrl+C cancels." << std::endl;
        if (!o.qr_png.empty()) std::ofstream(o.qr_png, std::ios::binary) << ffy::qr::to_png(m);

        // PairingClient::wait without the blocking: poll every 2 s, look at Ctrl+C every 50 ms
        std::optional<ffy::link::State> st;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(o.pair_timeout_s);
        while (std::chrono::steady_clock::now() < end) {
            auto polled = unless_stopped<std::optional<ffy::link::State>>([pc] { return pc->poll(); }, stop);
            if (!polled) return cancelled();
            if (*polled) {
                st = std::move(*polled);
                break;
            }
            if (pc->status() != "pending") break;
            if (!sleep_unless_stopped(std::chrono::milliseconds(2000), stop)) return cancelled();
        }
        if (!st) return error(pc->status() == "expired" ? "PAIRING_EXPIRED" : "PAIRING_FAILED", "pairing " + pc->status() + " - start again with: fairyfly link pair --gateway " + o.gateway);
        if (stop) return cancelled();  // Ctrl+C right after the phone approved: do not start reading the answer
        const bool confirmed = ffy::link::confirm_pairing(*st, in, out);
        if (stop) return cancelled();  // Ctrl+C at the prompt ends the input as well
        if (!confirmed) return error("PAIRING_NOT_CONFIRMED", "pairing not confirmed on this desktop - nothing was saved");
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

void request_stop() { g_stop = true; }

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
    // The person at the desktop sees one line per change of the connection. libffl's agent reports it only through
    // spdlog, so its messages go to the status printer as well as, filtered by --log-level, to stderr.
    const std::string sp = state_path(o);
    auto printer = std::make_shared<LinkStatusPrinter>(url_host(st.link_url), st.email, st.tenant_name.empty() ? st.tenant : st.tenant_name,
                                                       [tray = o.tray](const std::string& line) {
                                                           if (!tray) write_utf8(line + "\n");  // the tray shows the state itself
                                                       });
    {
        const auto user_level = spdlog::get_level();  // --log-level / FFLYLOG_LEVEL, applied by the CLI
        std::vector<spdlog::sink_ptr> sinks;
        if (o.tray) {
            sinks = spdlog::default_logger()->sinks();  // link.log (run_link_tray), no console
        } else {
            auto to_stderr = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            to_stderr->set_level(user_level);
            sinks.push_back(to_stderr);
        }
        auto status = make_status_sink(printer);
        status->set_level(spdlog::level::info);
        sinks.push_back(status);
        auto logger = std::make_shared<spdlog::logger>("fairyfly_link", sinks.begin(), sinks.end());
        logger->set_level(std::min(user_level, spdlog::level::info));
        spdlog::set_default_logger(logger);
    }

    // Everything the network thread uses lives here: when it does not end in time after Ctrl+C (a connect in
    // progress cannot be interrupted), the process ends without waiting for it.
    struct Net {
        explicit Net(ffy::link::State s, ffy::link::AgentConfig c) : agent(std::move(s), std::move(c)) {}
        ffy::link::Agent agent;
        std::string state_path;
        std::atomic<bool> done{false};
    };
    auto tools = std::make_shared<MainThreadTools>();
    ffy::link::AgentConfig cfg;
    cfg.local = tools;
    cfg.version = fairyfly::FAIRYFLY_VERSION;
    cfg.approver = o.approve == "no" ? ffy::link::fixed_approver(false) : ffy::link::desktop_approver();
    auto net = std::make_shared<Net>(st, cfg);
    net->state_path = sp;
    g_stop = false;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    printer->starting(is_paused(sp));
    // the link (outbound WebSocket), closed while paused; tool calls come back via `tools`
    SupervisorOptions supervision;
    supervision.on_change = [printer, last = std::string()](const std::string& state) mutable {
        if (!last.empty()) {
            if (state == "paused") printer->paused();
            else if (last == "paused") printer->resumed();
        }
        last = state;
    };
    g_background++;
    std::thread net_thread([net, supervision] {
        supervise(net->state_path, [net](const std::atomic<bool>& net_stop) { net->agent.run(net_stop); }, g_stop, supervision);
        net->done = true;
        g_background--;
    });

    mcp::ServeOptions so;
    so.transport = "link";
    so.allow_write = o.allow_write;
    so.read_only = o.read_only;
    so.families = o.families;
    mcp::HttpRunHooks hooks;
    hooks.keep_logging = true;  // the logger above; run_mcp would replace it
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
    printer->stopping();
    tools->shutdown();
    // the agent notices the stop within 200 ms; only a TCP/TLS connect in progress takes longer (up to 15 s)
    for (int i = 0; i < 60 && !net->done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (net->done) {
        net_thread.join();
    } else {
        net_thread.detach();  // owns `net`; the process ends without it
        write_status(sp, "stopped");
    }
    printer->stopped();
    return rc;
}

}  // namespace fairyfly::link
