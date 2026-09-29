#pragma once
#include <vector>
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Read-only tools (sap_screen_read, ...). Implemented in tool_catalog.cpp.
std::vector<ToolSpec> read_tool_specs();
/// State-changing tools (sap_click, sap_fill, ...), all with write_tool = true. tool_catalog_write.cpp.
std::vector<ToolSpec> write_tool_specs();
/// read_tool_specs() followed by write_tool_specs().
std::vector<ToolSpec> all_tool_specs();

} // namespace fairyfly::mcp
