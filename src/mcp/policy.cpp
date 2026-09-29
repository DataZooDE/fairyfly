#include "include/mcp/policy.h"

namespace fairyfly::mcp {

// PHASE 3: real policy (owner: phase 3 worker). Stubs allow every call; tool_visible already
// implements the read-only visibility rule needed by list_tools.
PolicyDecision check_call(const ToolSpec& spec, const json& args, const Policy& policy) {
    (void)spec; (void)args; (void)policy;
    return PolicyDecision{};
}

bool tool_visible(const ToolSpec& spec, const Policy& policy) {
    return !(spec.write_tool && policy.read_only);
}

bool RateLimiter::allow(std::chrono::steady_clock::time_point now) {
    (void)now;
    return true;
}

} // namespace fairyfly::mcp
