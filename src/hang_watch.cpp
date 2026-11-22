#include "include/hang_watch.h"

namespace fairyfly::hang {

void BlockedCallWatch::set_probe(Probe probe) {
    std::lock_guard<std::mutex> l(mu_);
    probe_ = std::move(probe);
}

void BlockedCallWatch::set_end_sap_gui(EndSapGui end) {
    std::lock_guard<std::mutex> l(mu_);
    end_ = std::move(end);
}

void BlockedCallWatch::arm(const std::string& tool, RestartPermit permit) {
    std::lock_guard<std::mutex> l(mu_);
    armed_ = true;
    tool_ = tool;
    permit_ = std::move(permit);
    ended_during_call_ = false;
}

bool BlockedCallWatch::disarm() {
    std::lock_guard<std::mutex> l(mu_);
    armed_ = false;
    const bool ended = ended_during_call_;
    ended_during_call_ = false;
    return ended;
}

void BlockedCallWatch::note_responsive() {
    std::lock_guard<std::mutex> l(mu_);
    restart_used_ = false;
}

std::optional<json> BlockedCallWatch::on_deadline(long long elapsed_ms) {
    // The probe and the end are Win32 only and may take seconds: they run without the lock, the decision under it.
    Probe probe;
    EndSapGui end;
    std::string tool;
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!armed_) return std::nullopt;
        probe = probe_;
        end = end_;
        tool = tool_;
    }
    const std::string seconds = std::to_string(elapsed_ms / 1000);
    const std::string call = tool.empty() ? std::string("the SAP call") : tool;
    auto restarted = [&] {
        json out = {{"code", kGuiRestartedCode},
                    {"message", "SAP GUI was frozen (" + call + " did not return for " + seconds +
                                " s) and was restarted; unsaved input in its sessions is lost."},
                    {"hint", "the effect of " + call + " is unknown: read the screen first (the next call prepares the "
                             "SAP session again), then decide whether to repeat it."},
                    {"running_tool", tool}, {"elapsed_ms", elapsed_ms}, {"sap_gui_restarted", true},
                    {"unsaved_input_lost", true}, {"original_operation_run", "unknown"}};
        return out;
    };
    {
        std::lock_guard<std::mutex> l(mu_);
        if (armed_ && ended_during_call_) return restarted();
    }
    FrontendState state;
    if (probe) {
        try { state = probe(); } catch (...) { return std::nullopt; }
    }
    if (!state.sap_gui_running) return std::nullopt;
    if (!state.frontend_hung) {
        return json{{"code", kServerNotRespondingCode},
                    {"message", call + " has not returned for " + seconds +
                                " s: SAP GUI still reacts and waits for the SAP server (a long-running or stuck step there)."},
                    {"hint", "do not repeat the call: wait, then read the screen; if it stays like this, ask the person at "
                             "the desktop or the SAP administrator (the SAP system may be overloaded). SAP GUI is not restarted "
                             "for this: it would not help."},
                    {"running_tool", tool}, {"elapsed_ms", elapsed_ms}, {"frontend", state.evidence}};
    }
    json restart = {{"allowed", false}};
    bool try_restart = false;
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!armed_) return std::nullopt;
        if (!permit_.allowed) {
            restart["reason"] = permit_.reason;
            if (!permit_.corrective.empty()) restart["corrective_action"] = permit_.corrective;
        } else if (restart_used_) {
            restart["reason"] = "already_restarted";
        } else if (!end) {
            restart["reason"] = "restart_unavailable";
        } else {
            restart_used_ = true;   // an attempt that fails half-way uses the episode's restart too
            try_restart = true;
        }
    }
    if (try_restart) {
        Result ended;
        try { ended = end(); } catch (const std::exception& e) { ended = Result::fail("SAP_GUI_RESTART_FAILED", e.what()); }
        if (ended.status == Result::Status::Success) {
            std::lock_guard<std::mutex> l(mu_);
            ended_during_call_ = true;
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
                {"running_tool", tool}, {"elapsed_ms", elapsed_ms}, {"frontend", state.evidence}, {"restart", restart}};
}

}  // namespace fairyfly::hang
