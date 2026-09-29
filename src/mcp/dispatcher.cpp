#include "include/mcp/dispatcher.h"

#include <stdexcept>

#include "include/mcp/policy.h"
#include "include/mcp/tool_catalog.h"

namespace fairyfly::mcp {

CommandDispatcher::CommandDispatcher(Invoker invoker, Policy policy, AuditHook hook)
    : invoker_(std::move(invoker)), policy_(std::move(policy)), hook_(std::move(hook)) {}

std::vector<ToolDef> CommandDispatcher::list_tools() const {
    std::vector<ToolDef> defs;
    for (auto& spec : all_tool_specs())
        if (tool_visible(spec, policy_)) defs.push_back(spec.def);
    return defs;
}

// PHASE 2: real dispatch (owner: phase 2 worker). Stub reports NOT_IMPLEMENTED.
ToolResult CommandDispatcher::call_tool(const std::string& name, const json& args, const CallContext& ctx) {
    (void)args; (void)ctx;
    ToolResult r;
    r.is_error = true;
    r.content.push_back(json{{"type", "text"}, {"text", "NOT_IMPLEMENTED: tool call for '" + name + "'"}});
    return r;
}

void CommandDispatcher::set_client_info(const json& client_info) { client_info_ = client_info; }

// PHASE 2: real registry invoker (owner: phase 2 worker). Stub throws.
Invoker make_registry_invoker(const std::function<cli::CommandHandler&()>& get_handler) {
    (void)get_handler;
    return [](const std::vector<std::string>&) -> Result {
        throw std::runtime_error("NOT_IMPLEMENTED: registry invoker");
    };
}

} // namespace fairyfly::mcp
