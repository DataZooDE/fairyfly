#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "include/mcp/dispatcher.h"
#include "include/mcp/json_rpc.h"
#include "include/mcp/policy.h"
#include "include/mcp/tool_catalog.h"
#include "include/mcp/transport.h"
#include "include/mcp/types.h"

using namespace fairyfly::mcp;

TEST_CASE("MCP shared type defaults", "[mcp]") {
    Policy p;
    CHECK(p.read_only);
    CHECK_FALSE(p.allow_write);
    CHECK_FALSE(p.default_connection.has_value());
    CHECK(p.default_format == "markdown");
    CHECK(p.max_result_chars == 60000);
    CHECK(p.max_image_bytes == 2u * 1024 * 1024);
    CHECK(p.max_calls_per_minute == 120);

    ServeOptions s;
    CHECK_FALSE(s.read_only);
    CHECK_FALSE(s.allow_write);
    CHECK(s.transport == "stdio");
    CHECK(s.format == "markdown");
    CHECK(s.max_image_bytes == 2097152);
    CHECK(s.call_timeout_ms == 120000);

    ServerOptions o;
    CHECK(o.name == "fairyfly");
    CHECK(o.max_queue == 16);
    CHECK(o.call_timeout_ms == 120000);
    REQUIRE(o.protocol_versions.size() == 4);
    CHECK(o.protocol_versions.front() == "2025-11-25");
    CHECK(o.protocol_versions.back() == "2024-11-05");

    ToolResult r;
    CHECK(r.content.is_array());
    CHECK_FALSE(r.is_error);
    CHECK_FALSE(r.structured.has_value());

    CHECK(kMethodNotFound == -32601);
    CHECK(kServerBusy == -32000);
    CHECK(kServerNotInitialized == -32002);
}

TEST_CASE("Dispatcher lists tools per policy", "[mcp]") {
    Invoker invoker = [](const std::vector<std::string>&) { return fairyfly::Result{}; };

    CommandDispatcher dispatcher(invoker, Policy{});
    bool found = false;
    for (const auto& def : dispatcher.list_tools()) found = found || def.name == "gui_doctor";
    CHECK(found);
    // tool_visible itself: test_mcp_policy.cpp
}

TEST_CASE("Catalog contains gui_doctor placeholder", "[mcp]") {
    auto specs = all_tool_specs();
    REQUIRE_FALSE(specs.empty());
    CHECK(specs.front().def.name == "gui_doctor");
    CHECK(specs.front().build_argv(json::object(), Policy{}) == std::vector<std::string>{"doctor"});
}
