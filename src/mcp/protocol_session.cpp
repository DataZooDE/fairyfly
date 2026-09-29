#include "include/mcp/protocol_session.h"

#include <algorithm>
#include <exception>

#include <spdlog/spdlog.h>

#include "include/mcp/json_rpc.h"

namespace fairyfly::mcp {

json text_result(const std::string& text, bool is_error) {
    json result{{"content", json::array({json{{"type", "text"}, {"text", text}}})}};
    if (is_error) result["isError"] = true;
    return result;
}

json tool_to_json(const ToolDef& tool) {
    json out{{"name", tool.name},
             {"title", tool.title},
             {"description", tool.description},
             {"inputSchema", tool.input_schema.is_null() ? json{{"type", "object"}} : tool.input_schema}};
    if (!tool.annotations.is_null()) out["annotations"] = tool.annotations;
    if (tool.meta.is_object() && !tool.meta.empty()) out["_meta"] = tool.meta;
    return out;
}

std::string negotiate_version(const std::vector<std::string>& supported, const std::string& requested) {
    std::string version = supported.empty() ? std::string("2025-11-25") : supported.front();
    for (const auto& s : supported)
        if (s == requested) version = requested;
    return version;
}

json make_initialize_result(const ServerOptions& options, const std::string& version) {
    return json{
        {"protocolVersion", version},
        {"capabilities", json{{"tools", json{{"listChanged", false}}}, {"logging", json::object()}}},
        {"serverInfo", json{{"name", options.name},
                            {"title", "fairyfly SAP GUI"},
                            {"version", options.version},
                            {"description", "SAP GUI automation through the SAP GUI Scripting API"}}},
        {"instructions", options.instructions}};
}

json tools_list_message(ToolProvider& provider, const Pending& p, bool sort_by_name) {
    if (p.params.is_object() && p.params.contains("cursor"))
        return make_error(p.id, kInvalidParams, "Invalid cursor: pagination is not supported");
    std::vector<ToolDef> defs = provider.list_tools();
    if (sort_by_name)
        std::stable_sort(defs.begin(), defs.end(), [](const ToolDef& a, const ToolDef& b) { return a.name < b.name; });
    json tools = json::array();
    for (const auto& tool : defs) tools.push_back(tool_to_json(tool));
    return make_result(p.id, json{{"tools", tools}});
}

json set_level_message(const Pending& p) {
    if (!p.params.is_object() || !p.params.contains("level") || !p.params["level"].is_string())
        return make_error(p.id, kInvalidParams, "logging/setLevel requires params.level");
    const std::string level = p.params["level"].get<std::string>();
    spdlog::level::level_enum mapped;
    if (level == "debug") mapped = spdlog::level::debug;
    else if (level == "info" || level == "notice") mapped = spdlog::level::info;
    else if (level == "warning") mapped = spdlog::level::warn;
    else if (level == "error") mapped = spdlog::level::err;
    else if (level == "critical" || level == "alert" || level == "emergency") mapped = spdlog::level::critical;
    else return make_error(p.id, kInvalidParams, "Invalid log level: " + level);
    spdlog::set_level(mapped);
    return make_result(p.id, json::object());
}

json call_tool_message(ToolProvider& provider, const Pending& p, CallContext ctx) {
    if (!p.params.is_object() || !p.params.contains("name") || !p.params["name"].is_string())
        return make_error(p.id, kInvalidParams, "tools/call requires a string params.name");
    json args = json::object();
    if (p.params.contains("arguments") && !p.params["arguments"].is_null()) {
        if (!p.params["arguments"].is_object())
            return make_error(p.id, kInvalidParams, "params.arguments must be an object");
        args = p.params["arguments"];
    }
    const std::string name = p.params["name"].get<std::string>();
    if (!provider.has_tool(name)) return make_error(p.id, kInvalidParams, "Unknown tool: " + name);

    ctx.request_id = p.id;
    if (p.params.contains("_meta") && p.params["_meta"].is_object() && p.params["_meta"].contains("progressToken"))
        ctx.progress_token = p.params["_meta"]["progressToken"];

    ToolResult result;
    try {
        result = provider.call_tool(name, args, ctx);
    } catch (const std::exception& e) {
        result = ToolResult{};
        result.content = json::array({json{{"type", "text"}, {"text", std::string("INTERNAL_ERROR: ") + e.what()}}});
        result.is_error = true;
    } catch (...) {
        result = ToolResult{};
        result.content = json::array({json{{"type", "text"}, {"text", "INTERNAL_ERROR: unknown exception"}}});
        result.is_error = true;
    }

    json out{{"content", result.content.is_array() ? result.content : json::array()}};
    if (result.structured) out["structuredContent"] = *result.structured;
    if (result.is_error) out["isError"] = true;
    return make_result(p.id, out);
}

json ProtocolSession::handle_initialize(const Pending& p) {
    if (initialized_) return make_error(p.id, kInvalidRequest, "Server already initialized");
    if (!p.params.is_object() || !p.params.contains("protocolVersion") || !p.params["protocolVersion"].is_string())
        return make_error(p.id, kInvalidParams, "initialize requires params.protocolVersion");
    version_ = negotiate_version(options_.protocol_versions, p.params["protocolVersion"].get<std::string>());

    if (p.params.contains("clientInfo") && p.params["clientInfo"].is_object()) {
        client_info_ = p.params["clientInfo"];
        try {
            provider_.set_client_info(client_info_);
        } catch (const std::exception& e) {
            spdlog::warn("set_client_info failed: {}", e.what());
        }
    }
    initialized_ = true;
    return make_result(p.id, make_initialize_result(options_, version_));
}

json ProtocolSession::process(const Pending& p, std::function<bool()> cancelled, const CallContext& ctx_base) {
    const bool is_request = !p.id.is_null();
    if (!is_request) {
        if (p.method == "notifications/initialized") ready_ = true;
        return json();  // unknown notifications are ignored
    }

    if (p.method == "initialize") return handle_initialize(p);
    if (p.method == "tools/list" || p.method == "tools/call" || p.method == "logging/setLevel") {
        if (!initialized_) return make_error(p.id, kServerNotInitialized, "Server not initialized");
        if (p.method == "tools/list") return tools_list_message(provider_, p, false);
        if (p.method == "logging/setLevel") return set_level_message(p);
        CallContext ctx = ctx_base;
        ctx.cancelled = std::move(cancelled);
        return call_tool_message(provider_, p, std::move(ctx));
    }
    // server/discover and everything else: lets dual-era clients fall back to initialize.
    return make_error(p.id, kMethodNotFound, "Method not found: " + p.method);
}

} // namespace fairyfly::mcp
