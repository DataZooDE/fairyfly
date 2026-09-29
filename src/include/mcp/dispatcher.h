#pragma once
#include <functional>
#include "include/cli_handler.h"
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// ToolProvider that maps tool calls onto CLI argv via the catalog, checks policy, invokes the
/// command registry through `Invoker`, shapes the Result and emits an audit record.
class CommandDispatcher : public ToolProvider {
public:
    CommandDispatcher(Invoker invoker, Policy policy, AuditHook hook = nullptr);
    std::vector<ToolDef> list_tools() const override;
    ToolResult call_tool(const std::string& name, const json& args, const CallContext& ctx) override;
    void set_client_info(const json& client_info) override;
private:
    Invoker invoker_;
    Policy policy_;
    AuditHook hook_;
    json client_info_;
};

/// Real Invoker: builds a private CLI::App, register_all_commands(), setup_all_commands, parses the
/// argv (prefixed with a program name, no app.exit), then execute_active_command(get_handler()).
Invoker make_registry_invoker(const std::function<cli::CommandHandler&()>& get_handler);

} // namespace fairyfly::mcp
