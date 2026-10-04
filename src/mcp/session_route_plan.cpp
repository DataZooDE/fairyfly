#include "include/mcp/session_route_plan.h"

#include <algorithm>

namespace fairyfly::mcp {
namespace {

SessionRoutePlan refused(const char* code) {
    SessionRoutePlan plan;
    plan.error_code = code;
    return plan;
}

bool global_control(const std::string& tool) {
    return tool == "gui_session_list" || tool == "gui_connection_list" ||
           tool == "gui_credentials_list" || tool == "gui_doctor" ||
           tool == "gui_session_lease" || tool == "gui_session_attach" ||
           tool == "gui_session_login" || tool == "gui_session_launch";
}

} // namespace

std::string window_lane_key(const std::string& session_identity) {
    const auto separator = session_identity.find('|');
    if (separator == std::string::npos || separator == 0) return {};
    return "window:" + session_identity.substr(0, separator);
}

SessionRoutePlan plan_session_route(const Principal& principal, const std::string& tool,
                                    const json& args, const Policy& policy,
                                    const SessionRoutingState& routing,
                                    const std::vector<ToolSpec>& specs) {
    const auto found = std::find_if(specs.begin(), specs.end(), [&](const ToolSpec& spec) {
        return spec.def.name == tool;
    });
    if (found == specs.end()) return refused("TOOL_NOT_FOUND");
    if (!args.is_object() && !args.is_null()) return refused("INVALID_ARGUMENT");
    if (tool == "gui_session_attach" &&
        (!args.is_object() || !args.contains("session_id") || !args["session_id"].is_string() ||
         args["session_id"].get<std::string>().empty()))
        return refused("SESSION_ID_REQUIRED");
    if (tool == "gui_session_disconnect" && policy.owner_sap_identities.size() > 1 &&
        args.is_object() && args.contains("close_session") && args["close_session"].is_boolean() &&
        args["close_session"].get<bool>() &&
        args.contains("connection") && args["connection"].is_number_integer() &&
        routing.has_prelogin_claim(principal.id, args["connection"].get<int>())) {
        SessionRoutePlan plan;
        plan.kind = SessionRoutePlan::Kind::GlobalControl;
        return plan;
    }
    if (global_control(tool)) {
        SessionRoutePlan plan;
        plan.kind = SessionRoutePlan::Kind::GlobalControl;
        return plan;
    }
    const bool batch = tool == "gui_batch";
    const bool accepts_connection = batch ||
        (found->def.input_schema.is_object() &&
         found->def.input_schema.value("properties", json::object()).contains("connection"));
    if (!accepts_connection) return refused("SESSION_ROUTE_REQUIRED");

    SessionRoutePlan plan;
    plan.kind = SessionRoutePlan::Kind::Session;
    if (args.is_object() && args.contains("connection")) {
        if (!args["connection"].is_number_integer()) return refused("INVALID_ARGUMENT");
        plan.connection = args["connection"].get<int>();
    } else if (batch) {
        return refused("BATCH_SESSION_REQUIRED");
    } else if (policy.default_connection) {
        plan.connection = policy.default_connection;
    } else if (const auto sticky = routing.sticky_target(principal_key(principal))) {
        plan.connection = sticky->connection;
        plan.sticky_identity = sticky->session_identity;
    }
    return plan;
}

} // namespace fairyfly::mcp
