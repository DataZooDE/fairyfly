#pragma once
#include <string>
#include <string_view>
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// One parsed JSON-RPC 2.0 message.
struct Message {
    enum class Kind { Request, Notification, Response, Invalid } kind = Kind::Invalid;
    json id;            ///< request/response id (null for notifications)
    std::string method; ///< empty for responses
    json params;        ///< object/array or null
    json raw;           ///< the whole parsed JSON value
};

/// Parses one line. On failure returns Kind::Invalid and sets `error_out` to a ready-to-send
/// JSON-RPC error object (kParseError / kInvalidRequest, id null or the request id when known).
Message parse_message(std::string_view line, json& error_out);

/// {"jsonrpc":"2.0","id":id,"result":result}
json make_result(const json& id, const json& result);
/// {"jsonrpc":"2.0","id":id,"error":{"code","message"[,"data"]}}; `data` omitted when null.
json make_error(const json& id, int code, const std::string& message, const json& data = nullptr);

} // namespace fairyfly::mcp
