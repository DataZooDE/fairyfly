#pragma once
// Issue #155, second variant: a SAP GUI scripting call that never returns (the COM thread blocks in it). The
// transports notice that at their per-call deadline on another thread (CallExecutor's watchdog, the link caller's
// wait) and ask this watch what to answer instead of a bare CALL_TIMEOUT:
//   - SAP_SERVER_NOT_RESPONDING: SAP GUI itself still reacts (it waits for the SAP server: a long or stuck server
//     step). Restarting SAP GUI would not help and is never done.
//   - SAP_GUI_NOT_RESPONDING: the SAP GUI frontend is frozen (its windows do not process messages) and may not be
//     restarted (the reason comes from the permit computed on the COM thread before the call).
//   - SAP_GUI_RESTARTED: the frontend is frozen, the restart is allowed and this logon session's SAP GUI was ended;
//     the blocked call then fails and frees the thread, and the next call prepares the session again.
// One restart per stuck episode: note_responsive() (a call that returned) renews it. Thread-safe; no COM here.
#include "core.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <cstdint>
#include <string>

namespace fairyfly::hang {

inline constexpr const char* kServerNotRespondingCode = "SAP_SERVER_NOT_RESPONDING";
inline constexpr const char* kGuiNotRespondingCode = "SAP_GUI_NOT_RESPONDING";
inline constexpr const char* kGuiRestartedCode = "SAP_GUI_RESTARTED";

/// May the watch end SAP GUI if THIS call blocks? Computed on the COM thread before the call, without COM.
struct RestartPermit {
    bool allowed = false;
    std::string reason;                  ///< why not ("" when allowed)
    json corrective = json::array();     ///< what the person can do (from startup::check)
    int expected_session_windows = -1;   ///< SAP sessions of the last listing (-1 = not checked)
};

/// What Win32 says about the SAP GUI frontend right now (any thread).
struct FrontendState {
    bool sap_gui_running = false;  ///< a SAP GUI process of this logon session runs
    bool frontend_hung = false;    ///< one of its windows does not process messages
    json evidence = json::object();
};

class BlockedCallWatch {
public:
    using Probe = std::function<FrontendState()>;
    using EndSapGui = std::function<Result()>;

    void set_probe(Probe probe);
    void set_end_sap_gui(EndSapGui end);
    /// Asked (any thread, no COM) right before SAP GUI is ended: "" = still allowed, else the reason (the startup
    /// permission or a desktop approval may have changed while the call ran).
    using Revalidate = std::function<std::string()>;
    void set_revalidate(Revalidate revalidate);
    /// A frozen frontend is believed only when a second probe after this delay says so too (default 5 s).
    void set_confirm_delay(std::chrono::milliseconds delay);

    /// COM thread, before a call: what to do if it blocks.
    void arm(const std::string& tool, RestartPermit permit);
    /// COM thread, after the call: true when SAP GUI was ended while it ran (the caller drops its COM state then).
    bool disarm();
    /// A call returned on its own: SAP GUI reacts again, a later freeze may restart again.
    void note_responsive();
    /// The other recovery (SAP_GUI_UNRESPONSIVE) restarted SAP GUI: both share one restart per episode.
    void mark_restart_used();
    /// COM thread, right after the blocked invocation returned: was SAP GUI ended for the armed call?
    bool ended_current_call() const;

    /// Any thread, at a call's deadline. nullopt: nothing armed or no SAP GUI runs (the generic timeout applies).
    /// Otherwise the error object {code, message, hint, ...} to answer with.
    /// `tool` and `elapsed_ms` identify the call the transport means: a deadline of an earlier call (another tool, or
    /// longer than the armed call has run) is ignored.
    std::optional<json> on_deadline(const std::string& tool, long long elapsed_ms);

private:
    mutable std::mutex mu_;
    Probe probe_;
    EndSapGui end_;
    Revalidate revalidate_;
    std::chrono::milliseconds confirm_delay_{5000};
    std::chrono::steady_clock::time_point armed_at_{};
    std::uint64_t generation_ = 0;
    std::uint64_t ending_generation_ = 0;   ///< the call whose SAP GUI is being or was ended
    bool armed_ = false;
    std::string tool_;
    RestartPermit permit_;
    bool ended_during_call_ = false;
    bool restart_used_ = false;
};

}  // namespace fairyfly::hang
