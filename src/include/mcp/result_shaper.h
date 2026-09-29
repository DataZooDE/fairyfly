#pragma once
#include <string>
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Turns a CLI Result into an MCP ToolResult: renders per spec.output, applies the char/image caps
/// from `policy`, prepends `untrusted_header` (data-not-instructions notice) to screen-derived text,
/// maps error Results to is_error with the structured error code.
ToolResult shape_result(const Result& result, const ToolSpec& spec, const Policy& policy,
                        const std::string& untrusted_header);

} // namespace fairyfly::mcp
