#pragma once
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include "include/config/environment.h"
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Environment lookup used by check_call for the FAIRYFLY_READ_ONLY hard cap (empty = process env).
using EnvFn = config::EnvLookup;

/// FAIRYFLY_READ_ONLY through `getenv_fn` (empty: the process environment), parsed by config::parse_flag.
bool env_read_only(const EnvFn& getenv_fn = {});

/// Decides whether a call may run (read-only refusals). Pure and cheap; it checks policy only, not
/// argument shapes (the dispatcher / argv builders do that). FAIRYFLY_READ_ONLY=1 is treated as
/// read-only even when `policy` says otherwise (defensive; run_mcp already enforces the cap).
PolicyDecision check_call(const ToolSpec& spec, const json& args, const Policy& policy,
                          const EnvFn& getenv_fn = {});

/// Whether tools/list shows the tool (write tools are hidden in read-only mode).
bool tool_visible(const ToolSpec& spec, const Policy& policy);

/// One sliding-window budget per principal name (remote MCP tokens). `per_minute` <= 0 = unlimited.
/// Thread-safe. Idle principals are dropped lazily.
class KeyedRateLimiter {
public:
    bool allow(const std::string& key, int per_minute, std::chrono::steady_clock::time_point now);
private:
    std::mutex mutex_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> calls_;
};

/// Sliding-window limiter, `per_minute` calls per 60 s (<= 0 = unlimited): one budget of a KeyedRateLimiter.
class RateLimiter {
public:
    explicit RateLimiter(int per_minute) : per_minute_(per_minute) {}
    /// Records and permits the call, or returns false (nothing recorded) when the budget is used up. Thread-safe.
    bool allow(std::chrono::steady_clock::time_point now) { return budget_.allow({}, per_minute_, now); }

private:
    int per_minute_;
    KeyedRateLimiter budget_;
};

} // namespace fairyfly::mcp
