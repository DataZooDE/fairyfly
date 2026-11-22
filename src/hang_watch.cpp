#include "include/hang_watch.h"

#include <thread>

namespace fairyfly::hang {

namespace {

using Clock = std::chrono::steady_clock;

/// A call that has run this much shorter than the transport says is not the one the deadline is about.
constexpr long long kDeadlineToleranceMs = 1000;

}  // namespace

void BlockedCallWatch::set_probe(Probe probe) {
    std::lock_guard<std::mutex> l(mu_);
    probe_ = std::move(probe);
}

void BlockedCallWatch::set_end_sap_gui(EndSapGui end) {
    std::lock_guard<std::mutex> l(mu_);
    end_ = std::move(end);
}

void BlockedCallWatch::set_revalidate(Revalidate revalidate) {
    std::lock_guard<std::mutex> l(mu_);
    revalidate_ = std::move(revalidate);
}

void BlockedCallWatch::set_confirm_delay(std::chrono::milliseconds delay) {
    std::lock_guard<std::mutex> l(mu_);
    confirm_delay_ = delay;
}

void BlockedCallWatch::arm(const std::string& tool, RestartPermit permit) {
    std::lock_guard<std::mutex> l(mu_);
    armed_ = true;
    ++generation_;
    armed_at_ = Clock::now();
    tool_ = tool;
    permit_ = std::move(permit);
    ended_during_call_ = false;
}

bool BlockedCallWatch::disarm() {
    std::lock_guard<std::mutex> l(mu_);
    armed_ = false;
    // also true while the end is still running: the call came back because SAP GUI is going away
    const bool ended = ending_generation_ == generation_ && generation_ != 0;
    ended_during_call_ = false;
    return ended;
}

void BlockedCallWatch::note_responsive() {
    std::lock_guard<std::mutex> l(mu_);
    restart_used_ = false;
}

void BlockedCallWatch::mark_restart_used() {
    std::lock_guard<std::mutex> l(mu_);
    restart_used_ = true;
}

std::optional<json> BlockedCallWatch::on_deadline(const std::string& requested_tool, long long elapsed_ms) {
    // The probes and the end are Win32 only and take seconds: they run without the lock, each decision under it.
    Probe probe;
    EndSapGui end;
    Revalidate revalidate;
    std::chrono::milliseconds confirm{0};
    std::uint64_t generation = 0;
    std::string tool;
    const auto same_call = [&] { return armed_ && generation_ == generation; };
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!armed_ || (!requested_tool.empty() && requested_tool != tool_)) return std::nullopt;
        const long long ran_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - armed_at_).count();
        if (ran_ms + kDeadlineToleranceMs < elapsed_ms) return std::nullopt;   // the deadline of an earlier call
        probe = probe_;
        end = end_;
        revalidate = revalidate_;
        confirm = confirm_delay_;
        generation = generation_;
        tool = tool_;
    }
    const std::string seconds = std::to_string(elapsed_ms / 1000);
    const std::string call = tool.empty() ? std::string("the SAP call") : tool;
    auto restarted = [&] {
        return json{{"code", kGuiRestartedCode},
                    {"message", "SAP GUI was frozen (" + call + " did not return for " + seconds +
                                " s) and was restarted; unsaved input in its sessions is lost."},
                    {"hint", "the effect of " + call + " is unknown: read the screen first (the next call prepares the "
                             "SAP session again), then decide whether to repeat it."},
                    {"running_tool", tool}, {"elapsed_ms", elapsed_ms}, {"sap_gui_restarted", true},
                    {"unsaved_input_lost", true}, {"original_operation_run", "unknown"}};
    };
    {
        std::lock_guard<std::mutex> l(mu_);
        if (same_call() && ended_during_call_) return restarted();
    }
    const auto look = [&]() -> std::optional<FrontendState> {
        if (!probe) return FrontendState{};
        try { return probe(); } catch (...) { return std::nullopt; }
    };
    auto state = look();
    if (!state) return std::nullopt;
    if (!state->sap_gui_running) return std::nullopt;
    if (state->frontend_hung && confirm.count() > 0) std::this_thread::sleep_for(confirm);
    if (state->frontend_hung) {
        // one window that was busy for a moment is no proof: a second look must agree
        const auto again = look();
        if (!again) return std::nullopt;
        if (!again->frontend_hung) state = again;
        else state->evidence = again->evidence;
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!same_call()) return std::nullopt;   // the call returned meanwhile
    }
    if (!state->frontend_hung) {
        return json{{"code", kServerNotRespondingCode},
                    {"message", call + " has not returned for " + seconds +
                                " s, and SAP GUI still processes window messages: it most likely waits for the SAP server "
                                "(a long-running or stuck step there) or for a dialog."},
                    {"hint", "do not repeat the call: wait, then read the screen; if it stays like this, ask the person at "
                             "the desktop or the SAP administrator. SAP GUI is not restarted for this."},
                    {"running_tool", tool}, {"elapsed_ms", elapsed_ms}, {"frontend", state->evidence}};
    }
    // still allowed? (startup permission and approvals may have changed while the call ran)
    std::string late_refusal;
    if (revalidate) {
        try { late_refusal = revalidate(); } catch (...) { late_refusal = "restart_unavailable"; }
    }
    json restart = {{"allowed", false}};
    bool try_restart = false;
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!same_call()) return std::nullopt;
        const int windows = state->evidence.value("session_windows", -1);
        if (!permit_.allowed) {
            restart["reason"] = permit_.reason;
            if (!permit_.corrective.empty()) restart["corrective_action"] = permit_.corrective;
        } else if (restart_used_) {
            restart["reason"] = "already_restarted";
        } else if (!end) {
            restart["reason"] = "restart_unavailable";
        } else if (permit_.expected_session_windows >= 0 && windows > permit_.expected_session_windows) {
            restart["reason"] = "other_sessions_unknown";   // more SAP sessions than the last listing knew
        } else if (!late_refusal.empty()) {
            restart["reason"] = late_refusal;
        } else {
            restart_used_ = true;              // an attempt that fails half-way uses the episode's restart too
            ending_generation_ = generation;   // set before the end: the released call may disarm before it returns
            try_restart = true;
        }
    }
    if (try_restart) {
        Result ended;
        try { ended = end(); } catch (const std::exception& e) { ended = Result::fail("SAP_GUI_RESTART_FAILED", e.what()); }
        if (ended.status == Result::Status::Success) {
            std::lock_guard<std::mutex> l(mu_);
            if (generation_ == generation) ended_during_call_ = true;
            return restarted();
        }
        restart["allowed"] = true;
        restart["failed"] = ended.error.value("message", std::string("SAP GUI could not be ended"));
    }
    return json{{"code", kGuiNotRespondingCode},
                {"message", "SAP GUI is frozen: " + call + " has not returned for " + seconds +
                            " s and its windows do not react."},
                {"hint", "do not repeat the call: ask the person at the desktop to end SAP Logon (Task Manager) or allow "
                         "automatic SAP startup so fairyfly can restart it; then read the screen again."},
                {"running_tool", tool}, {"elapsed_ms", elapsed_ms}, {"frontend", state->evidence}, {"restart", restart}};
}

bool BlockedCallWatch::ended_current_call() const {
    std::lock_guard<std::mutex> l(mu_);
    return armed_ && ending_generation_ == generation_ && generation_ != 0;
}

}  // namespace fairyfly::hang
