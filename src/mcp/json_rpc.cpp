#include "include/mcp/json_rpc.h"

namespace fairyfly::mcp {

namespace {

bool valid_id(const json& id) {
    return id.is_string() || id.is_number_integer() || id.is_number_unsigned();
}

Message invalid(json& error_out, const json& id, int code, const std::string& text) {
    Message m;
    m.kind = Message::Kind::Invalid;
    error_out = make_error(id, code, text);
    return m;
}

} // namespace

Message parse_message(std::string_view line, json& error_out) {
    error_out = nullptr;

    // Strip a UTF-8 BOM and surrounding whitespace (CR/LF included).
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.remove_prefix(3);
    }
    auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    while (!line.empty() && is_space(line.front())) line.remove_prefix(1);
    while (!line.empty() && is_space(line.back())) line.remove_suffix(1);

    Message m;
    if (line.empty()) return m;  // Invalid without error: caller skips blank lines

    json value = json::parse(line.begin(), line.end(), nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded()) return invalid(error_out, nullptr, kParseError, "Parse error: invalid JSON");
    if (value.is_array())
        return invalid(error_out, nullptr, kInvalidRequest, "Invalid Request: JSON-RPC batching is not supported");
    if (!value.is_object())
        return invalid(error_out, nullptr, kInvalidRequest, "Invalid Request: expected a JSON object");

    const bool has_id = value.contains("id");
    const bool has_method = value.contains("method");
    const json known_id = has_id && valid_id(value["id"]) ? value["id"] : json(nullptr);

    if (has_method) {
        if (!value["method"].is_string())
            return invalid(error_out, known_id, kInvalidRequest, "Invalid Request: method must be a string");
        m.method = value["method"].get<std::string>();
        if (value.contains("params")) {
            const json& params = value["params"];
            if (!params.is_object() && !params.is_array() && !params.is_null())
                return invalid(error_out, known_id, kInvalidRequest,
                               "Invalid Request: params must be an object or array");
            m.params = params;
        }
        if (has_id) {
            if (!valid_id(value["id"]))
                return invalid(error_out, nullptr, kInvalidRequest,
                               "Invalid Request: id must be a string or an integer");
            m.id = value["id"];
            m.kind = Message::Kind::Request;
        } else {
            m.kind = Message::Kind::Notification;
        }
        m.raw = std::move(value);
        return m;
    }

    if (has_id && (value.contains("result") || value.contains("error"))) {
        m.kind = Message::Kind::Response;
        m.id = value["id"];
        m.raw = std::move(value);
        return m;
    }

    return invalid(error_out, known_id, kInvalidRequest, "Invalid Request: missing method");
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
