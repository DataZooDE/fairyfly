#include "include/link/link_state_names.h"
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
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#endif

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "include/link/desktop_approval.h"
#include "include/link/link_console.h"
#include "include/link/link_call.h"
#include "include/link/link_control.h"
#include "include/config/mcp_config.h"
#include "include/link/main_thread_tools.h"
#include "include/link/trail_recorder.h"
#include "include/link/trail_screen_source.h"
#include "include/mcp/run_mcp.h"
#ifdef _WIN32
#include "include/com/wrapper.h"
#include "include/link/desktop_approval_win32.h"
#include "include/tray/tray_win32.h"
#endif
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

void on_signal(int) { LinkRuntime::process().stop = true; }
void on_pair_signal(int) { LinkRuntime::process().pair_stop = true; }

/// "Allow changes in SAP" (approval model v2): the standing approval at this desktop, saved for the user in the tray
/// settings (HKCU), shared by the link tray's switch and the approval window's "Don't ask again".
std::shared_ptr<approval::Standing> saved_standing() {
#ifdef _WIN32
    std::shared_ptr<tray::TraySettings> settings = tray::make_win32_tray_settings();
    return std::make_shared<approval::Standing>([settings] { return settings->get("link_allow_changes"); },
                                                [settings](const std::string& v) { return settings->set("link_allow_changes", v); });
#else
    return std::make_shared<approval::Standing>();
#endif
}

class StandingSwitch : public tray::LinkWriteMode {
public:
    StandingSwitch(bool read_only, std::shared_ptr<approval::Standing> standing)
        : read_only_(read_only), standing_(std::move(standing)) {}
    bool allowed() override { return standing_->saved(); }
    bool locked() override { return read_only_ || config::read_only_cap(); }
    bool enforced() override { return standing_->enforced(); }
    bool set_allowed(bool allowed) override {
        if (allowed && locked()) return false;
        if (!standing_->set(allowed)) return false;
        spdlog::info("fairyfly link: \"Allow changes in SAP\" {}", allowed ? "on: changes run without asking" : "off: every change asks");
        return true;
    }

private:
    bool read_only_;
    std::shared_ptr<approval::Standing> standing_;
};

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
    std::thread([shared, fn = std::move(fn), alive = LinkRuntime::process().background_thread()] {
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

/// FAIRYFLY_LINK_FORBID_DEFAULT_STATE=1 (tests, tests/integration/link_trail_live.ps1): refuse the pairing state the
/// person at this desktop relies on, so a test copy can never pause, unpair or overwrite it.
bool guard_on() {
    return config::Environment::process().flag(config::env::kLinkForbidDefaultState);
}

/// The pairing directories to protect: from LOCALAPPDATA (what default_state_path uses) and, on Windows, from the
/// Local AppData known folder (a test may have redirected LOCALAPPDATA; the real pairing stays protected).
std::vector<std::string> protected_dirs() {
    std::vector<std::string> dirs{std::filesystem::path(default_state_path()).parent_path().string()};
#ifdef _WIN32
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known)
        dirs.push_back((std::filesystem::path(known) / "fairyfly" / "link").string());
    if (known) CoTaskMemFree(known);
#endif
    return dirs;
}

std::optional<Result> state_guard_for(const std::string& path) {
    if (!guard_on()) return std::nullopt;
    std::string hit;
    for (const auto& dir : protected_dirs())
        if (in_default_state_dir(path, dir)) hit = dir;
    if (hit.empty()) return std::nullopt;
    const std::string dir = hit;
    Result r;
    r.status = Result::Status::Error;
    r.error["code"] = "STATE_GUARD";
    r.error["message"] = "FAIRYFLY_LINK_FORBID_DEFAULT_STATE is set and the pairing state " + path + " lies in " + dir +
                         "; pass --state <file> outside it";
    return r;
}

std::optional<Result> state_guard(const LinkOptions& o) { return state_guard_for(state_path(o)); }

std::string host_name() {
    const auto& env = config::Environment::process();
    std::string h = env.get(config::env::kComputerName);
    if (h.empty()) h = env.get(config::env::kHostName);
    return h.empty() ? "unknown" : h;
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

/// Puts `text` into the console's input line, as if typed: the person can accept it with Enter or edit it. False
/// without a console on stdin (a pipe or file): the prompt then shows the prefill in brackets.
bool prefill_console_line(const std::string& text) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !h || !GetConsoleMode(h, &mode) || !(mode & ENABLE_LINE_INPUT) ||
        !(mode & ENABLE_ECHO_INPUT))
        return false;
    FlushConsoleInputBuffer(h);   // typeahead would end up in front of the address
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return false;
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), n);
    std::vector<INPUT_RECORD> keys;
    for (wchar_t c : wide)
        for (BOOL down : {TRUE, FALSE}) {
            INPUT_RECORD r{};
            r.EventType = KEY_EVENT;
            r.Event.KeyEvent.bKeyDown = down;
            r.Event.KeyEvent.wRepeatCount = 1;
            r.Event.KeyEvent.uChar.UnicodeChar = c;
            const SHORT vk = VkKeyScanW(c);   // some console hosts drop key events without a key code
            if (vk != -1) {
                r.Event.KeyEvent.wVirtualKeyCode = LOBYTE(vk);
                r.Event.KeyEvent.wVirtualScanCode = static_cast<WORD>(MapVirtualKeyW(LOBYTE(vk), MAPVK_VK_TO_VSC));
            }
            keys.push_back(r);
        }
    DWORD written = 0;
    return WriteConsoleInputW(h, keys.data(), static_cast<DWORD>(keys.size()), &written) && written == static_cast<DWORD>(keys.size());
#else
    (void)text;
    return false;
#endif
}

/// pair_next_step() for this desktop: is the link tray running, does a link already serve this pairing?
std::string pair_next_step_now(const std::string& path) {
#ifdef _WIN32
    // the link tray serves the default pairing (its `--state` aside): it only connects a pairing made there
    const bool tray_running = link_run_lock_name(path) == link_run_lock_name(default_state_path()) &&
                              tray::make_win32_single_instance(tray::kLinkTrayMutexName)->exists();
    const bool link_running = tray::make_win32_single_instance(link_run_lock_name(path))->exists();
    return pair_next_step(tray_running, link_running);
#else
    (void)path;
    return pair_next_step(false, false);
#endif
}

}  // namespace

LinkRuntime::LinkRuntime(std::shared_ptr<approval::Standing> standing) : standing(std::move(standing)) {}

LinkRuntime& LinkRuntime::process() {
    static LinkRuntime runtime(saved_standing());
    return runtime;
}

int background_threads() { return LinkRuntime::process().background_threads(); }

std::optional<Result> link_state_guard(const LinkOptions& options) { return state_guard(options); }

std::optional<Result> claim_link_run(const std::string& path, std::unique_ptr<tray::SingleInstance>& held) {
#ifdef _WIN32
    auto lock = tray::make_win32_single_instance(link_run_lock_name(path));
    if (!lock->acquire())
        return error("LINK_ALREADY_RUNNING", "a link for this pairing (" + path +
                                                 ") is already running, in the link tray or another console; quit it first "
                                                 "(tray: Quit, console: Ctrl+C)");
    held = std::move(lock);
#else
    (void)path;
    (void)held;
#endif
    return std::nullopt;
}

std::string default_state_path() {
#ifdef _WIN32
    std::string base = config::Environment::process().get(config::env::kLocalAppData);
    if (base.empty()) base = ".";
#else
    std::string base = config::Environment::process().get(config::env::kXdgConfigHome);
    if (base.empty()) {
        const std::string home = config::Environment::process().get(config::env::kHome);
        base = (home.empty() ? std::string(".") : home) + "/.config";
    }
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
    LinkRuntime& runtime = LinkRuntime::process();
    runtime.pair_stop = false;
    std::optional<SignalScope> ctrl_c;
    if (!io.stop) ctrl_c.emplace(SIGINT, on_pair_signal);
    const std::atomic<bool>& stop = io.stop ? *io.stop : runtime.pair_stop;
    const auto cancelled = [] { return error("PAIRING_CANCELLED", "pairing cancelled - nothing was saved"); };

    if (auto refused = state_guard(options)) return *refused;
    out << "state: " << state_path(options) << std::endl;
    LinkOptions o = options;
    const PairGateway gateway = pair_gateway(o.gateway, config::Environment::process().get(config::env::kLinkGateway));
    o.gateway = gateway.address;
    if (gateway.ask) {  // e.g. from the tray's "Pair with the fairyfly cloud": https://fairyfly.io, Enter accepts it
        if (stop) return cancelled();
        const bool in_line = !io.in && prefill_console_line(gateway.address);   // editable in the console's input line
        out << pair_gateway_prompt(gateway.address, in_line) << std::flush;
        std::string typed;
        std::getline(in, typed);
        if (stop) return cancelled();
        o.gateway = pair_gateway_answer(typed, gateway.address);
    }
    if (auto problem = gateway_url_problem(o.gateway)) return error("INVALID_ARGUMENT", *problem + ", e.g. " + kDefaultGateway);
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
        // the gateway decides where `link run` connects: never in plain text to another computer
        if (auto problem = gateway_url_problem(st->link_url, GatewayUrlKind::WebSocket))
            return error("PAIRING_FAILED", "the gateway handed out an insecure link address - nothing was saved (" + *problem + ")");
        if (stop) return cancelled();  // Ctrl+C right after the phone approved: do not start reading the answer
        const bool confirmed = ffy::link::confirm_pairing(*st, in, out);
        if (stop) return cancelled();  // Ctrl+C at the prompt ends the input as well
        if (!confirmed) return error("PAIRING_NOT_CONFIRMED", "pairing not confirmed on this desktop - nothing was saved");
        ffy::link::save_state(*st, state_path(o));
        Result r;
        r.status = Result::Status::Success;
        r.data = {{"device", st->device_id}, {"account", st->email}, {"organisation", st->tenant_name.empty() ? st->tenant : st->tenant_name},
                  {"organisation_id", st->tenant}, {"gateway", st->link_url}, {"state", state_path(o)}, {"next", pair_next_step_now(state_path(o))}};
        return r;
    } catch (const std::exception& e) {
        return error("PAIRING_FAILED", e.what());
    }
}

Result link_status(const LinkOptions& o) {
    if (auto refused = state_guard(o)) return *refused;
    try {
        auto st = ffy::link::load_state(state_path(o));
        if (auto problem = gateway_url_problem(st.link_url, GatewayUrlKind::WebSocket))
            return error("LINK_STATE_INSECURE", "the pairing state " + state_path(o) + " holds an insecure link address (" + *problem +
                                                    "); pair again: fairyfly link pair");
        Result r;
        r.status = Result::Status::Success;
        r.data = {{"paired", true}, {"device", st.device_id}, {"account", st.email}, {"organisation_id", st.tenant},
                  {"organisation", st.tenant_name}, {"gateway", st.link_url}, {"pinned_gateway_keys", st.gateway_keys.size()}, {"state", state_path(o)},
                  {"paused", is_paused(state_path(o))}, {"link", describe(read_status(state_path(o)), ffy::now_unix())}};
        // the audit trail this desktop runs under, as the running link last wrote it
        const json status = read_status(state_path(o));
        const bool running = r.data["link"] != state::kNotRunning;
        r.data["audit_trail"] = running && status.contains("trail") ? trail::status_text(status["trail"]) : std::string("off");
        return r;
    } catch (const std::exception&) {
        Result r;
        r.status = Result::Status::Success;
        r.data = {{"paired", false}, {"state", state_path(o)}, {"next", "fairyfly link pair"}};
        return r;
    }
}

Result link_unpair(const LinkOptions& o) {
    if (auto refused = state_guard(o)) return *refused;
    std::error_code ec;
    bool removed = std::filesystem::remove(state_path(o), ec);
    if (ec) return error("UNPAIR_FAILED", ec.message());
    Result r;
    r.status = Result::Status::Success;
    r.data = {{"removed", removed}, {"note", "also remove the desktop in the web app (Overview -> Desktops) so the gateway forgets it"}};
    return r;
}

Result link_pause(const LinkOptions& o) {
    if (auto refused = state_guard(o)) return *refused;
    if (!set_paused(state_path(o), true)) return error("PAUSE_FAILED", "could not write " + pause_marker(state_path(o)).string());
    Result r;
    r.status = Result::Status::Success;
    r.data = {{"paused", true}, {"link", describe(read_status(state_path(o)), ffy::now_unix())},
              {"note", "a running link disconnects within a second; the AI assistant cannot reach this desktop until: fairyfly link resume"}};
    return r;
}

Result link_resume(const LinkOptions& o) {
    if (auto refused = state_guard(o)) return *refused;
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
        const json status = read_status(path_);
        v.state = describe(status, ffy::now_unix());
        if (v.state != state::kNotRunning && status.contains("trail")) {
            const auto p = ffy::proto::TrailPolicy::from_json(status["trail"]);
            v.trail_on = p.on;
            v.trail_mode = p.mode;
            v.trail_retention_days = p.retention_days;
            v.trail_blur = p.blur;
        }
        return v;
    }
    bool set_paused(bool paused) override {
        if (state_guard_for(path_)) return false;  // a test copy never pauses the real pairing
        return fairyfly::link::set_paused(path_, paused);
    }

private:
    std::string path_;
};
}  // namespace

std::unique_ptr<tray::LinkControl> make_link_control(const std::string& path) {
    return std::make_unique<StateLinkControl>(path.empty() ? default_state_path() : path);
}

void request_stop() { LinkRuntime::process().request_stop(); }

std::unique_ptr<tray::LinkWriteMode> make_write_mode_control(bool read_only, LinkRuntime& runtime) {
    return std::make_unique<StandingSwitch>(read_only, runtime.standing);
}

int run_link(const LinkOptions& o, const std::function<cli::CommandHandler&()>& get_handler, const commands::GlobalOptions& global,
             audit::AuditSink* sink, const std::function<cli::CommandHandler*()>& peek) {
    LinkRuntime& runtime = LinkRuntime::process();
    if (auto refused = state_guard(o)) {
        std::cerr << refused->to_json().dump() << std::endl;
        return 2;
    }
    if (!o.tray) write_utf8("state: " + state_path(o) + "\n");
    // one link per pairing: a second `link run` (or the tray's) next to a running one would fight over the connection
    std::unique_ptr<tray::SingleInstance> run_lock;
    if (auto refused = claim_link_run(state_path(o), run_lock)) {
        spdlog::error("LINK_ALREADY_RUNNING: {}", refused->error.value("message", std::string()));   // the tray's link.log
        std::cerr << refused->to_json().dump() << std::endl;
        return tray::kLinkAlreadyRunningExitCode;   // the link tray waits on it (run_link_with_tray)
    }
    ffy::link::State st;
    try {
        st = ffy::link::load_state(state_path(o));
    } catch (const std::exception& e) {
        Result r = error("LINK_NOT_PAIRED", std::string("this desktop is not paired (") + e.what() + "); run: fairyfly link pair");
        std::cerr << r.to_json().dump() << std::endl;
        return 2;
    }
    if (auto problem = gateway_url_problem(st.link_url, GatewayUrlKind::WebSocket)) {
        Result r = error("LINK_STATE_INSECURE", "the pairing state " + state_path(o) + " holds an insecure link address (" + *problem +
                                                    "); pair again: fairyfly link pair");
        spdlog::error("LINK_STATE_INSECURE: {}", *problem);   // the tray's link.log
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
#ifdef _WIN32
    // a change is approved at this desktop: "Allow changes in SAP" (the standing approval), or a notification and the
    // approval window on Details (desktop_approval_win32). libffl's own window is off: a yes approves one change.
    cfg.approve_window_s = 0;
    approval::Desk desk;
    desk.standing = runtime.standing;
    desk.can_change = [read_only = o.read_only] { return !read_only && !config::read_only_cap(); };
    desk.sap = [weak = std::weak_ptr<MainThreadTools>(tools), peek] {
                                           // the SAP session the change would run in, read on the main (COM) thread
                                           auto out = std::make_shared<approval::SapContext>();
                                           auto t = weak.lock();
                                           if (!t) return *out;
                                           auto read = std::make_shared<approval::SapContext>();
                                           const bool ran = t->invoke(
                                               [read, peek] {
                                                   cli::CommandHandler* h = peek ? peek() : nullptr;
                                                   auto s = h ? h->attached_com_session() : nullptr;
                                                   if (!s) return;
                                                   try { read->system = s->get_system_name(); } catch (const std::exception&) {}
                                                   try { read->client = s->get_client(); } catch (const std::exception&) {}
                                                   try { read->transaction = s->get_transaction_code(); } catch (const std::exception&) {}
                                                   try { read->user = s->get_user(); } catch (const std::exception&) {}
                                               },
                                               2000);
                                           return ran ? *read : *out;
                                       };
    cfg.approver = o.approve == "no" ? ffy::link::fixed_approver(false) : approval::make_desktop_approver(std::move(desk));
    // the desktop's own check: write rights only for the call approved here, whatever the grant says about approval
    tools->set_write_permit(approval::take_permit);
#else
    cfg.approver = o.approve == "no" ? ffy::link::fixed_approver(false) : ffy::link::desktop_approver();
#endif
    // offer this desktop's tool list on every connect; the gateway keeps it only from a catalog_publishers account,
    // otherwise it uses its built-in catalog
    cfg.publish_catalog = true;
    // the dispatcher applies the grant's transaction list and the gateway's cancel to every call (link_call_context)
    cfg.local_enforces_tcodes = true;
    cfg.local_honours_cancel = true;
    // The audit trail (docs/LINK.md "Audit trail"): libffl announces it and asks for steps only while the gateway's
    // policy is on; the recorder reads SAP on the main thread through `tools`, shows the notice and puts the policy
    // into the status file for the tray.
    auto trail_changed = std::make_shared<std::atomic<bool>>(false);
    trail::RecorderOptions ro;
    ro.source = trail::make_com_screen_source(peek);
    ro.run_on_main = [weak = std::weak_ptr<MainThreadTools>(tools)](std::function<void()> fn) {
        auto t = weak.lock();
        return t && t->invoke(std::move(fn), 5000);  // never holds up the answer for long
    };
    ro.on_policy = [trail_changed, tray = o.tray](const ffy::proto::TrailPolicy& p) {
        const std::string line = trail::notice_line(p);
        if (!tray) write_utf8(line + "\n");
        spdlog::info("{}", line);
        *trail_changed = true;
    };
    auto recorder = std::make_shared<trail::TrailRecorder>(ro);
    cfg.recorder = recorder;
    auto net = std::make_shared<Net>(st, cfg);
    net->state_path = sp;
    // the tray's Quit may come before this point (request_stop): only a console run starts from a clean flag
    if (!o.tray) runtime.stop = false;
    // Ctrl+C and termination stop the link; the previous handlers come back when run_link returns
    const SignalScope stop_on_ctrl_c(SIGINT, on_signal);
    const SignalScope stop_on_term(SIGTERM, on_signal);
    printer->starting(is_paused(sp));
    // the link (outbound WebSocket), closed while paused; tool calls come back via `tools`
    SupervisorOptions supervision;
    supervision.status_changed = trail_changed.get();
    supervision.status_extra = [recorder] { return json{{"trail", recorder->current_policy().to_json()}}; };
    supervision.on_change = [printer, last = std::string()](const std::string& state) mutable {
        if (!last.empty()) {
            if (state == link::state::kPaused) printer->paused();
            else if (last == link::state::kPaused) printer->resumed();
        }
        last = state;
    };
    std::thread net_thread([net, supervision, trail_changed, &runtime, alive = runtime.background_thread()] {
        supervise(net->state_path, [net](const std::atomic<bool>& net_stop) { net->agent.run(net_stop); }, runtime.stop, supervision);
        net->done = true;
    });

    mcp::ServeOptions so;
    so.transport = "link";
    // changes are possible: each needs the organisation's grant (per call) and the approval at this desktop; only
    // --read-only / FAIRYFLY_READ_ONLY make fairyfly refuse them here
    so.allow_write = !o.read_only && !config::read_only_cap();
    so.read_only = o.read_only;
    so.families = o.families;
    mcp::HttpRunHooks hooks;
    hooks.keep_logging = true;  // the logger above; run_mcp would replace it
    hooks.link_loop = [&](mcp::ToolProvider& provider, const std::function<bool(bool)>&) -> int {
        {
            // the tools that change SAP (destructiveHint): their steps carry field changes
            std::set<std::string> change_tools;
            for (const auto& t : tools_json(provider))
                if (t.contains("annotations") && t["annotations"].is_object() && t["annotations"].value("destructiveHint", false))
                    change_tools.insert(t.value("name", std::string()));
            recorder->set_change_tools(std::move(change_tools));
        }
        tools->set_handlers([&provider] { return tools_json(provider); },
                            [&provider, &runtime](const std::string& name, const json& args, const ffy::link::CallInfo& info) -> json {
                                mcp::CallContext ctx = link_call_context(info, runtime.stop);
                                provider.set_client_info({{"name", info.client}, {"version", "fairyfly-link"}});
                                mcp::ToolResult r = provider.call_tool(name, args, ctx);
                                json out = {{"content", r.content}, {"isError", r.is_error}};
                                if (r.structured) out["structuredContent"] = *r.structured;
                                return out;
                            });
        spdlog::info("fairyfly link: serving {} for {} ({})", st.tenant_name.empty() ? st.tenant : st.tenant_name, st.email, st.device_id);
        tools->pump(runtime.stop);  // this is the main (COM) thread
        tools->shutdown();
        return 0;
    };
    const int rc = mcp::run_mcp(so, get_handler, global, sink, peek, &hooks);
    runtime.stop = true;
    printer->stopping();
    tools->shutdown();
    // the agent notices the stop within 200 ms; only a TCP/TLS connect in progress takes longer (up to 15 s)
    for (int i = 0; i < 60 && !net->done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (net->done) {
        net_thread.join();
    } else {
        net_thread.detach();  // owns `net`; the process ends without it
        write_status(sp, state::kStopped);
    }
    printer->stopped();
    return rc;
}

}  // namespace fairyfly::link
