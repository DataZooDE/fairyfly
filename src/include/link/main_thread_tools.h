#pragma once
// The desktop link (D10, `fairyfly link run`): libffl's agent receives tool calls on its worker threads, but
// fairyfly's ToolProvider may only run on the main (COM) thread, one call at a time. MainThreadTools queues every
// call and the main thread executes them in order (pump). Portable: depends only on libffl and nlohmann-json.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "link_agent.hpp"  // libffl

namespace fairyfly::link {

/// The call the main thread is running right now. SAP GUI can stop answering scripting inside one (live 2026-10-27:
/// gui_screen_read on SU01 never returned); every later call then waits behind it.
struct InFlightCall {
    std::string tool;          ///< MCP tool name; "" for a posted task (the audit trail reading the screen)
    std::string connection;    ///< the call's `connection` argument; "" when it named none (the attached session)
    std::chrono::milliseconds elapsed{0};
    std::int64_t started_unix = 0;  ///< wall clock, shown as local HH:MM:SS
};

/// A call running this long is stuck: the tray turns amber (stuck_call_status) and later calls fail at once.
inline constexpr std::chrono::seconds kStuckCallAfter{60};
/// How long gui_doctor and gui_session_list wait for the main thread before they answer SAP_GUI_NOT_RESPONDING.
inline constexpr std::chrono::seconds kDiagnosticWait{10};

/// "SAP GUI has not answered gui_screen_read on connection 0 for 183 s (started 20:27:21)".
std::string not_responding_text(const InFlightCall& call);
/// The tools/call result of a diagnostic tool (`tool`) that could not run behind `call`: isError, a text that starts
/// with SAP_GUI_NOT_RESPONDING and says what to do, structuredContent {status:"error", error:{code, message,
/// running_tool, connection, elapsed_ms, started, hint}}.
nlohmann::json not_responding_result(const std::string& tool, const InFlightCall& call);
/// For the link's status file (the tray): {tool, connection, elapsed_s, started} once `call` has been running for
/// `after` or longer; null otherwise.
nlohmann::json stuck_call_status(const std::optional<InFlightCall>& call, std::chrono::milliseconds after = kStuckCallAfter);

class MainThreadTools : public ffy::link::LocalTools {
public:
    /// MCP `tools` array (name, description, inputSchema, annotations).
    using ListFn = std::function<nlohmann::json()>;
    /// MCP tool result object ({"content": [...], "isError": bool, "structuredContent"?}).
    using CallFn = std::function<nlohmann::json(const std::string& name, const nlohmann::json& args, const ffy::link::CallInfo& info)>;

    MainThreadTools() = default;
    /// Main thread: what to run. Set before pumping (the dispatcher exists only inside the run loop).
    void set_handlers(ListFn list, CallFn call);
    /// Set: a tools/call with the write grant keeps it only when `permit(tool, args)` says this desktop approved that
    /// call (approval::take_permit); otherwise it runs read-only. Asked on the caller's thread. Unset: as granted.
    using PermitFn = std::function<bool(const std::string& tool, const nlohmann::json& args)>;
    void set_write_permit(PermitFn permit);

    /// Any thread (libffl's agent): queues the call and waits for the main thread. Throws ffy::Error on timeout
    /// (nothing ran: the message names the call that holds the main thread; or the call itself ran that long:
    /// SAP_GUI_NOT_RESPONDING) or after shutdown. gui_doctor and gui_session_list do not queue behind a call that SAP
    /// GUI has not answered for the diagnostic wait: they answer SAP_GUI_NOT_RESPONDING (not_responding_result) at
    /// once or after that wait, and never run later. Any other call behind one running for kStuckCallAfter fails at
    /// once, naming it. No second COM call goes into SAP GUI.
    nlohmann::json rpc(const std::string& method, const nlohmann::json& params, int timeout_ms, const ffy::link::CallInfo& info) override;
    bool per_call_read_only() const override { return true; }

    /// Any thread: queues `fn` to run on the main thread between calls; does not wait. False after shutdown.
    bool post(std::function<void()> fn);
    /// Any thread: queues `fn` like post() and waits until it has run on the main thread (the audit trail reads the
    /// screen there). False when it did not finish within `timeout_ms` (a task that has not started then never
    /// runs) or after shutdown. `fn` must own what it writes: after a timeout it may still be running.
    bool invoke(std::function<void()> fn, int timeout_ms);

    /// Main thread: runs queued calls until `stop` becomes true (checks it at least every 200 ms).
    void pump(const std::atomic<bool>& stop);
    /// Main thread: runs at most one queued call, waiting up to `wait`. True if one ran.
    bool pump_once(std::chrono::milliseconds wait);
    /// Refuses queued and future calls (pending callers get an error).
    void shutdown();

    /// Any thread: the call the main thread runs now; nullopt while it is idle.
    std::optional<InFlightCall> in_flight() const;
    /// The tools that change SAP (destructiveHint): a caller that gave up on one of them while it ran is told the
    /// change may still be applied. Set before pumping.
    void set_change_tools(std::set<std::string> tools);
    /// Tests: the clock for the age of the running call (default steady_clock::now). Set before pumping.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    void set_clock(Clock now);
    /// Tests: how long a diagnostic tool waits for the main thread (default kDiagnosticWait). Set before pumping.
    void set_diagnostic_wait(std::chrono::milliseconds wait);

private:
    struct Job {
        std::string method;
        nlohmann::json params;
        ffy::link::CallInfo info;
        std::function<void()> fn;  ///< set: a posted task instead of an rpc
        std::string tool, connection;  ///< for in_flight()
        std::chrono::steady_clock::time_point started;
        std::int64_t started_unix = 0;
        nlohmann::json result;
        std::string error;
        bool done = false, abandoned = false, running = false;
    };
    nlohmann::json run(Job& job);
    InFlightCall in_flight_locked(const Job& job) const;
    PermitFn permit_;
    Clock now_ = [] { return std::chrono::steady_clock::now(); };
    std::chrono::milliseconds diagnostic_wait_ = kDiagnosticWait;
    std::set<std::string> change_tools_;
    std::shared_ptr<Job> running_;  ///< the job on the main thread now
    mutable std::mutex mu_;
    std::condition_variable work_cv_, done_cv_;
    std::deque<std::shared_ptr<Job>> queue_;
    ListFn list_;
    CallFn call_;
    bool shutdown_ = false;
};

}  // namespace fairyfly::link
