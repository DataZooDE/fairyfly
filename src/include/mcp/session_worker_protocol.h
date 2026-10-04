#pragma once

#include <string>
#include <vector>
#include <functional>
#include <iosfwd>

#include <nlohmann/json.hpp>
#include "include/core.h"

namespace fairyfly::mcp {

using json = nlohmann::json;

constexpr std::size_t kWorkerRequestMaxBytes = 65536;

struct WorkerCall {
    int id = 0;
    int connection = -1;
    bool probe = false; ///< read-only live facts lookup; no argv or session binding
    bool enumerate = false; ///< one-shot read-only SAP session discovery; no binding
    std::string session_identity;
    std::string owner_identity;
    std::string expected_screen_guard; ///< optional dynpro precondition, checked again inside the worker
    std::vector<std::string> argv;
    bool read_only = true;
    bool selection_input = false;
    std::string selection_program;
    std::string selection_screen;
};

/// Decode one versioned private-pipe request. Throws std::invalid_argument on
/// malformed, oversized, unbound or control-plane commands.
WorkerCall decode_worker_call(const std::string& line);
std::string encode_worker_call(const WorkerCall& call);
bool worker_command_allowed(const std::vector<std::string>& argv);

using WorkerInvoker = std::function<Result(const WorkerCall&)>;
int run_worker_loop(std::istream& input, std::ostream& output, const WorkerInvoker& invoke);

} // namespace fairyfly::mcp
