#pragma once
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Environment lookup used by check_call for the FAIRYFLY_READ_ONLY hard cap (empty = process env).
using EnvFn = std::function<std::string(const char*)>;

/// Decides whether a call may run (read-only refusals). Pure and cheap; it checks policy only, not
/// argument shapes (the dispatcher / argv builders do that). FAIRYFLY_READ_ONLY=1 is treated as
/// read-only even when `policy` says otherwise (defensive; run_mcp already enforces the cap).
PolicyDecision check_call(const ToolSpec& spec, const json& args, const Policy& policy,
                          const EnvFn& getenv_fn = {});

/// Whether tools/list shows the tool (write tools are hidden in read-only mode).
bool tool_visible(const ToolSpec& spec, const Policy& policy);

/// Sliding-window limiter, `per_minute` calls per 60 s (<= 0 = unlimited). Thread-safe.
class RateLimiter {
public:
    explicit RateLimiter(int per_minute) : per_minute_(per_minute) {}
    /// Records and permits the call, or returns false (nothing recorded) when the budget is used up.
    bool allow(std::chrono::steady_clock::time_point now);
private:
    int per_minute_;
    std::mutex mutex_;
    std::deque<std::chrono::steady_clock::time_point> calls_;
};

} // namespace fairyfly::mcp
