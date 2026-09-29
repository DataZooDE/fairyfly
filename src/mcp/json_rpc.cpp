#include "include/mcp/json_rpc.h"

namespace fairyfly::mcp {

// PHASE 1: real JSON-RPC parsing/validation (owner: phase 1 worker). Stub parse returns Invalid.
Message parse_message(std::string_view line, json& error_out) {
    (void)line;
    Message m;
    m.kind = Message::Kind::Invalid;
    error_out = make_error(nullptr, kInternalError, "parse_message not implemented");
    return m;
}

json make_result(const json& id, const json& result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

json make_error(const json& id, int code, const std::string& message, const json& data) {
    json err{{"code", code}, {"message", message}};
    if (!data.is_null()) err["data"] = data;
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", err}};
}

} // namespace fairyfly::mcp
