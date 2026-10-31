#include "include/session_preparation.h"
#include "include/connection_launcher.h"
#include "include/startup_coordination.h"
#include <optional>

namespace fairyfly::startup {
Result prepare(const Policy& policy, const Operations& operations) {
    const auto cancelled = [&] { return operations.cancelled && operations.cancelled(); };
    std::optional<int> connection;
    const auto with_connection = [&](Result result) {
        if (connection) {
            if (!result.error.is_object()) result.error = json::object();
            result.error["connection_open"] = true;
            result.error["connection_file_id"] = *connection;
        }
        return result;
    };
    const auto cancel_result = [&] { return with_connection(Result::fail("CANCELLED", "SAP session preparation was cancelled; inspect the session before retrying.")); };
    if (cancelled()) return cancel_result();
    if (!operations.invoke || !operations.authenticated)
        return Result::fail("SAP_STARTUP_UNAVAILABLE", "This transport cannot prepare a SAP session.");
    const auto guard = acquire_preparation_guard(operations.cancelled);
    if (!guard.token) return guard.result;
    if (cancelled()) return cancel_result();
    const auto permission = [&]() -> std::optional<Result> {
        const auto current = operations.current_policy ? operations.current_policy() : policy;
        if (current.default_connection != policy.default_connection)
            return Result::fail("SAP_STARTUP_CONFIG_CHANGED", "The default SAP connection changed; retry the task to use the new default.");
        return check(current);
    };
    // Discovery must occur after taking the shared lock: another process may
    // have completed startup while this request was waiting.
    const Result listed = operations.invoke("gui_session_list", json::object());
    if (cancelled()) return cancel_result();
    if (listed.status != Result::Status::Success &&
        (!listed.error.is_object() || listed.error.value("code", "") != "SAP_LOGON_NOT_RUNNING")) return listed;
    json candidates = json::array();
    if (listed.status == Result::Status::Success) {
        if (!listed.data.is_object() || !listed.data.contains("connections") || !listed.data["connections"].is_array())
            return Result::fail("SAP_STARTUP_DISCOVERY_FAILED", "SAP session discovery returned an incomplete result; inspect SAP Logon and retry.");
        for (const auto* field : {"connection_enumeration_errors", "session_enumeration_errors"})
            if (listed.data.contains(field) &&
                (!listed.data[field].is_number_integer() || listed.data[field] != 0))
                return Result::fail("SAP_STARTUP_DISCOVERY_FAILED", "Some SAP sessions could not be inspected; wait for SAP and retry. No new session was opened.");
        for (const auto& connection : listed.data["connections"]) {
            if (!connection.is_object() || !connection.contains("sessions") || !connection["sessions"].is_array() ||
                !connection.contains("description") || !connection["description"].is_string() ||
                connection["description"].get<std::string>().empty())
                return Result::fail("SAP_STARTUP_DISCOVERY_FAILED", "SAP session discovery was incomplete; no new session was opened.");
            if (!policy.default_connection.empty() && !sap::ConnectionLauncher::matches_connection_name(
                    policy.default_connection, connection.value("description", ""))) continue;
            for (const auto& session : connection["sessions"]) candidates.push_back(session);
        }
    }
    // A matching authenticated session already satisfies the task. Unrelated
    // login windows must not force a new login or make that session ambiguous.
    json authenticated = json::array();
    bool readable = true;
    for (const auto& candidate : candidates) {
        if (!candidate.is_object() || !candidate.contains("user") || !candidate["user"].is_string() ||
            !candidate.contains("id") || !candidate["id"].is_string() || candidate["id"].get<std::string>().empty()) {
            readable = false;
            break;
        }
        if (!candidate["user"].get<std::string>().empty()) authenticated.push_back(candidate);
    }
    if (readable && !authenticated.empty()) candidates = std::move(authenticated);
    if (candidates.size() > 1)
        return Result::fail("MULTIPLE_SESSIONS", "Several matching SAP sessions are open.",
            json::array({"Select the intended session explicitly with gui_session_attach or fairyfly session attach, then retry."}));
    bool logged_on = false;
    std::string session_id;
    if (!candidates.empty()) {
        const auto& session = candidates[0];
        if (!session.is_object() || !session.contains("user") || !session["user"].is_string() ||
            !session.contains("id") || !session["id"].is_string() || session["id"].get<std::string>().empty())
            return Result::fail("SAP_SESSION_NOT_READY", "The matching SAP session is busy or its identity is unavailable; wait for SAP and retry.");
        logged_on = !session["user"].get<std::string>().empty();
        if (logged_on &&
            (!session.contains("system") || !session["system"].is_string() || session["system"].get<std::string>().empty() ||
             !session.contains("client") || !session["client"].is_string() || session["client"].get<std::string>().empty()))
            return Result::fail("SAP_SESSION_NOT_READY", "The matching SAP session's authenticated identity is incomplete; wait for SAP and retry.");
        session_id = session["id"].get<std::string>();
    }
    if (!logged_on) if (auto refused = permission()) return *refused;
    bool changed_screen = !logged_on;
    if (cancelled()) return cancel_result();
    Result selected = session_id.empty()
        ? operations.invoke("gui_session_launch", {{"name", policy.default_connection}, {"login", false}})
        : operations.invoke("gui_session_attach", {{"session_id", session_id}});
    if (selected.status != Result::Status::Success) return selected;
    if (!selected.data.is_object() || !selected.data.contains("connection_file_id") ||
        !selected.data["connection_file_id"].is_number_integer())
        return Result::fail("SAP_STARTUP_NOT_READY", "Session preparation did not return a saved connection; inspect SAP before retrying.");
    const int id = selected.data["connection_file_id"].get<int>();
    connection = id;
    if (cancelled()) return cancel_result();
    if (!operations.authenticated(id)) {
        changed_screen = true;
        // SSO may already have authenticated the newly opened connection. Only
        // resolve credentials when the live session still needs login.
        if (auto refused = permission()) return with_connection(*refused);
        if (cancelled()) return cancel_result();
        auto login = operations.invoke("gui_session_login", {{"connection", id}, {"multiple_logon", "fail"}});
        if (login.status != Result::Status::Success) {
            return with_connection(login);
        }
        if (cancelled()) return cancel_result();
        if (!operations.authenticated(id))
            return with_connection(Result::fail("SAP_LOGIN_REQUIRED", "SAP login needs attention on the desktop.",
                json::array({"Complete SAP login or SSO on the desktop, then retry the original task."})));
    }
    if (operations.login_ready) operations.login_ready();
    selected.data["session_ready"] = true;
    selected.data["startup_changed_screen"] = changed_screen;
    selected.data["startup_default"] = policy.default_connection;
    return selected;
}
} // namespace fairyfly::startup
