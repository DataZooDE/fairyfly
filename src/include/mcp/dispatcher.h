#pragma once
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "include/cli_handler.h"
#include "include/mcp/policy.h"
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// ToolProvider that maps tool calls onto CLI argv via the catalog, checks policy, invokes the
/// command registry through `Invoker`, shapes the Result and emits an audit record.
///
/// Every call (including each item of sap_batch) runs the same pipeline: find spec -> check_call ->
/// rate limit -> build_argv -> invoke -> shape -> audit hook (exactly once) -> sticky connection.
class CommandDispatcher : public ToolProvider {
public:
    /// `specs` empty = all_tool_specs() (tests may inject a custom catalog).
    CommandDispatcher(Invoker invoker, Policy policy, AuditHook hook = nullptr,
                      std::vector<ToolSpec> specs = {});
    std::vector<ToolDef> list_tools() const override;
    /// Includes tools hidden by policy so a call to them gets the friendly policy refusal.
    bool has_tool(const std::string& name) const override { return find_spec(name) != nullptr; }
    ToolResult call_tool(const std::string& name, const json& args, const CallContext& ctx) override;
    void set_client_info(const json& client_info) override;

    /// Replaces the rate-limit decision (default: RateLimiter(policy.max_calls_per_minute)).
    /// Test seam; the gate returns false to refuse the call with RATE_LIMITED.
    void set_rate_gate(std::function<bool(std::chrono::steady_clock::time_point)> gate);
    /// Connection remembered from the last successful sap_attach / sap_launch.
    std::optional<int> sticky_connection() const { return sticky_connection_; }

private:
    ToolResult run_single(const std::string& name, const json& args, const CallContext& ctx,
                          std::string* code_out = nullptr);
    ToolResult execute_call(const std::string& name, const json& args, McpCallRecord& record, std::string& code);
    ToolResult run_batch(const json& args, const CallContext& ctx);
    ToolResult audited(const std::string& tool, const std::function<ToolResult(McpCallRecord&, std::string&)>& fn,
                       const CallContext& ctx, std::string* code_out);
    Result invoke(const std::vector<std::string>& argv);
    const ToolSpec* find_spec(const std::string& name) const;

    Invoker invoker_;
    Policy policy_;
    AuditHook hook_;
    std::vector<ToolSpec> specs_;
    json client_info_;
    std::string client_;
    RateLimiter limiter_;
    std::function<bool(std::chrono::steady_clock::time_point)> rate_gate_;
    std::optional<int> sticky_connection_;
};

/// Real Invoker: builds a private CLI::App, register_all_commands(), setup_all_commands, parses the
/// argv (prefixed with a program name, no app.exit), then execute_active_command(get_handler()).
/// Serialised by a mutex (the command registry is a singleton); never writes to stdout/stderr.
Invoker make_registry_invoker(const std::function<cli::CommandHandler&()>& get_handler);

} // namespace fairyfly::mcp
