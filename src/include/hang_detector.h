#pragma once
// Issue #155: a SAP GUI that is alive and not busy but ignores every input. Each navigation still reports
// success (or TRANSACTION_NOT_STARTED) after a few milliseconds and the screen never changes. The detector
// counts such no-ops per session and trips after kNoOpThreshold of them in a row; any observed screen change
// ends the episode. Pure logic: no COM, no clock, unit-tested in tests/unit/test_hang_detector.cpp.
#include "core.h"

#include <map>
#include <string>
#include <vector>

namespace fairyfly::hang {

inline constexpr int kNoOpThreshold = 3;
inline constexpr const char* kUnresponsiveCode = "SAP_GUI_UNRESPONSIVE";
inline constexpr const char* kRecoveredCode = "SAP_GUI_RECOVERED";

/// What one call tells about the GUI's responsiveness.
enum class Signal {
    Neutral,  ///< says nothing (reads, writes without a screen check, other errors)
    NoOp,     ///< a navigation that should have changed the screen and did not
    Change,   ///< the screen demonstrably changed: the GUI reacts
};

struct Observation {
    std::string tool;
    Signal signal = Signal::Neutral;
    std::string detail;        ///< key name or transaction, for the evidence
    std::string error_code;    ///< the call's error code, "" on success
    long long sap_action_ms = 0;
};

/// Classifies a finished call. Navigation keys are F3 (Back, vkey 3), F12 (Cancel, 12) and Shift+F3 (Exit, 15):
/// they leave the current screen on any responsive GUI. Enter, F8 and other keys may legitimately change nothing.
/// A transaction start that did not become active on an idle, unchanged session is a no-op as well.
Observation classify(const std::string& tool, const json& args, const Result& result);

/// True for the vkeys that must leave the current screen (F3, F12, Shift+F3).
bool is_navigation_vkey(int vkey);

struct Verdict {
    bool tripped = false;
    int consecutive_noops = 0;
    json evidence = json::array();  ///< the no-op observations of this episode, oldest first
};

class Detector {
public:
    explicit Detector(int threshold = kNoOpThreshold) : threshold_(threshold) {}

    /// Feeds one call for a session key (the saved connection id or "implicit"). A Change resets that session
    /// and ends the restart budget's episode; a NoOp counts; Neutral leaves the count unchanged.
    Verdict observe(const std::string& session_key, const Observation& observation);

    /// Forgets a session (after recovery, restart, launch or login).
    void reset(const std::string& session_key);

    /// One automatic restart per stuck episode: true once a restart happened and no screen change has been
    /// observed since, in any session. Guards against a restart loop when the new GUI hangs again.
    bool restart_used() const { return restart_used_; }
    void note_restart() { restart_used_ = true; }

    int consecutive_noops(const std::string& session_key) const;

private:
    struct SessionState {
        int noops = 0;
        json evidence = json::array();
    };
    int threshold_;
    std::map<std::string, SessionState> sessions_;
    bool restart_used_ = false;
};

}  // namespace fairyfly::hang
