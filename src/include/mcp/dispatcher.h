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
/// Every call (including each item of gui_batch) runs the same pipeline: find spec -> check_call ->
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
    /// SAP system/client/transaction the call will run against (for token allowlists); nullopt = unknown.
    /// Without a provider every call of a token that has a system allowlist is denied SYSTEM_UNKNOWN.
    using SapFactsProvider = std::function<std::optional<audit::SapFacts>(std::optional<int> connection)>;
    void set_sap_facts_provider(SapFactsProvider provider) { facts_provider_ = std::move(provider); }
    /// Per-call read-only override: called with true before a call of a read-only token while the server
    /// is in write mode, and with the server value afterwards. Wire it to CommandHandler::set_read_only.
    using ReadOnlyOverride = std::function<void(bool read_only)>;
    void set_read_only_override(ReadOnlyOverride hook) { read_only_override_ = std::move(hook); }
    /// tools/list for one principal: hides tools outside its scopes and, for read-only tokens, write tools.
    std::vector<ToolDef> list_tools_for(const Principal& principal) const;
    /// Connection remembered from the last successful gui_session_attach / gui_session_launch.
    std::optional<int> sticky_connection() const { return sticky_connection_; }

private:
    ToolResult run_single(const std::string& name, const json& args, const CallContext& ctx,
                          std::string* code_out = nullptr);
    ToolResult execute_call(const std::string& name, const json& args, McpCallRecord& record, std::string& code,
                            const CallContext& ctx);
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
    KeyedRateLimiter keyed_limiter_;
    SapFactsProvider facts_provider_;
    ReadOnlyOverride read_only_override_;
};

/// Real Invoker: builds a private CLI::App, register_all_commands(), setup_all_commands, parses the
/// argv (prefixed with a program name, no app.exit), then execute_active_command(get_handler()).
/// Serialised by a mutex (the command registry is a singleton); never writes to stdout/stderr.
Invoker make_registry_invoker(const std::function<cli::CommandHandler&()>& get_handler);

} // namespace fairyfly::mcp
