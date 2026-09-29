#include "include/mcp/result_shaper.h"

namespace fairyfly::mcp {

// PHASE 2: real shaping (owner: phase 2 worker). Stub returns the compact JSON as a text block.
ToolResult shape_result(const Result& result, const ToolSpec& spec, const Policy& policy,
                        const std::string& untrusted_header) {
    (void)spec; (void)policy; (void)untrusted_header;
    ToolResult out;
    out.content.push_back(json{{"type", "text"}, {"text", result.to_json().dump()}});
    out.is_error = result.status != Result::Status::Success;
    return out;
}

} // namespace fairyfly::mcp
