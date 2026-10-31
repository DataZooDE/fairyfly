#pragma once
#include "session_startup.h"
#include <functional>

namespace fairyfly::startup {
// The transport supplies its authorized lifecycle operations. MCP operations go
// through dispatcher admission; CLI operations use the same handler methods.
struct Operations {
    std::function<Result(const std::string&, const json&)> invoke;
    std::function<bool(int)> authenticated;
    std::function<bool()> cancelled;
    std::function<Policy()> current_policy;
    std::function<Result()> begin_login;
    std::function<void()> login_ready;
};
Result prepare(const Policy& policy, const Operations& operations);
} // namespace fairyfly::startup
