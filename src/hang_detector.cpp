#include "include/hang_detector.h"

#include "include/vkey.h"

namespace fairyfly::hang {

namespace {

std::string error_code_of(const Result& result) {
    if (result.status == Result::Status::Success || !result.error.is_object()) return {};
    const auto it = result.error.find("code");
    return it != result.error.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string text_arg(const json& args, const char* name) {
    if (!args.is_object()) return {};
    const auto it = args.find(name);
    return it != args.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

bool is_navigation_vkey(int vkey) { return vkey == 3 || vkey == 12 || vkey == 15; }

Observation classify(const std::string& tool, const json& args, const Result& result) {
    Observation o;
    o.tool = tool;
    o.error_code = error_code_of(result);
    o.sap_action_ms = result.duration.count();
    if (tool == "gui_key_send") {
        o.detail = text_arg(args, "key");
        if (result.status != Result::Status::Success || !result.data.is_object()) return o;
        const auto changed = result.data.find("screen_changed");
        if (changed == result.data.end() || !changed->is_boolean()) return o;   // no screen check: no verdict
        if (changed->get<bool>()) {
            o.signal = Signal::Change;
            return o;
        }
        int vkey = -1;
        if (const auto v = result.data.find("vkey"); v != result.data.end() && v->is_number_integer()) vkey = v->get<int>();
        else if (const auto parsed = sap::parse_vkey(o.detail)) vkey = *parsed;
        if (is_navigation_vkey(vkey)) o.signal = Signal::NoOp;
        return o;
    }
    if (tool == "gui_transaction_start") {
        o.detail = text_arg(args, "code");
        if (result.status == Result::Status::Success) {
            // bare /n is reported as success without a transaction check: only a changed screen proves a reaction
            const bool changed = !result.data.is_object() || result.data.value("screen_changed", true);
            o.signal = changed ? Signal::Change : Signal::Neutral;
            return o;
        }
        if (o.error_code == "TRANSACTION_NOT_STARTED" && result.error.value("session_busy", true) == false &&
            result.error.value("screen_changed", true) == false)
            o.signal = Signal::NoOp;
        return o;
    }
    return o;
}

Verdict Detector::observe(const std::string& session_key, const Observation& observation) {
    Verdict verdict;
    auto& state = sessions_[session_key];
    switch (observation.signal) {
    case Signal::Change:
        state = SessionState{};
        restart_used_ = false;   // the GUI reacts again: the episode is over
        break;
    case Signal::NoOp: {
        ++state.noops;
        json entry = {{"tool", observation.tool}, {"sap_action_ms", observation.sap_action_ms}};
        if (!observation.detail.empty()) entry["input"] = observation.detail;
        if (!observation.error_code.empty()) entry["error_code"] = observation.error_code;
        state.evidence.push_back(std::move(entry));
        while (state.evidence.size() > static_cast<std::size_t>(threshold_)) state.evidence.erase(state.evidence.begin());
        break;
    }
    case Signal::Neutral:
        break;
    }
    verdict.consecutive_noops = state.noops;
    verdict.tripped = state.noops >= threshold_;
    if (verdict.tripped) verdict.evidence = state.evidence;
    return verdict;
}

void Detector::reset(const std::string& session_key) { sessions_.erase(session_key); }

int Detector::consecutive_noops(const std::string& session_key) const {
    const auto it = sessions_.find(session_key);
    return it == sessions_.end() ? 0 : it->second.noops;
}

}  // namespace fairyfly::hang
