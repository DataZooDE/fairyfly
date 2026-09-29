#pragma once
#include <chrono>
#include <deque>
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Decides whether a call may run (read-only refusal, argument sanity). Pure function.
PolicyDecision check_call(const ToolSpec& spec, const json& args, const Policy& policy);

/// Whether tools/list shows the tool (write tools are hidden in read-only mode).
bool tool_visible(const ToolSpec& spec, const Policy& policy);

/// Sliding-window limiter, `per_minute` calls per 60 s. Not thread-safe (main thread only).
class RateLimiter {
public:
    explicit RateLimiter(int per_minute) : per_minute_(per_minute) {}
    bool allow(std::chrono::steady_clock::time_point now);
private:
    int per_minute_;
    std::deque<std::chrono::steady_clock::time_point> calls_;
};

} // namespace fairyfly::mcp
