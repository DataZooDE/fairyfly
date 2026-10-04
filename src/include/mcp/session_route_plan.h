#pragma once

#include <optional>
#include <string>
#include <vector>

#include "include/mcp/session_routing_state.h"
#include "include/mcp/types.h"

namespace fairyfly::mcp {

struct SessionRoutePlan {
    enum class Kind { Session, GlobalControl, Refused } kind = Kind::Refused;
    std::optional<int> connection;
    std::string sticky_identity;
    std::string error_code;
};

/// Stable FIFO key for one live GUI window across server-key/cache-generation changes.
std::string window_lane_key(const std::string& session_identity);

/// Selects the saved connection using the dispatcher's exact precedence. It
/// performs no COM work; a caller must validate live facts and token policy
/// before allocating the selected session lane.
SessionRoutePlan plan_session_route(const Principal& principal, const std::string& tool,
                                    const json& args, const Policy& policy,
                                    const SessionRoutingState& routing,
                                    const std::vector<ToolSpec>& specs);

} // namespace fairyfly::mcp
