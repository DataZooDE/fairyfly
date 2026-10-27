#pragma once
// The MCP catalog entry of one tool, for tests (fails the test when the tool does not exist).
#include <catch2/catch_test_macros.hpp>

#include "include/mcp/tool_catalog.h"

#include <string>
#include <vector>

namespace fairyfly::test {

inline const mcp::ToolSpec& spec_of(const std::vector<mcp::ToolSpec>& specs, const std::string& tool) {
    for (const auto& s : specs)
        if (s.def.name == tool) return s;
    FAIL("tool not in catalog: " << tool);
    return specs.front();
}

inline const mcp::ToolSpec& spec_of(const std::string& tool) {
    static const std::vector<mcp::ToolSpec> catalog = mcp::all_tool_specs();
    return spec_of(catalog, tool);
}

}  // namespace fairyfly::test
