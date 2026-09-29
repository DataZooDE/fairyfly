#include <catch2/catch_test_macros.hpp>

#include "include/config/client_config.h"

using namespace fairyfly::config;

namespace {
const ClientSnippet* find(const std::vector<ClientSnippet>& v, const std::string& id) {
    for (const auto& s : v)
        if (s.id == id) return &s;
    return nullptr;
}
} // namespace

TEST_CASE("client-config: golden Claude Code output", "[client_config]") {
    const auto snippets = build_client_configs({});
    const auto* cmd = find(snippets, "claude-code");
    REQUIRE(cmd);
    CHECK(cmd->body ==
          "claude mcp add --transport http fairyfly https://vm:8443/mcp --header \"Authorization: Bearer ${FAIRYFLY_TOKEN}\"\n");

    const auto* json = find(snippets, "claude-code-json");
    REQUIRE(json);
    CHECK(json->body ==
          "{\n"
          "  \"mcpServers\": {\n"
          "    \"fairyfly\": {\n"
          "      \"type\": \"http\",\n"
          "      \"url\": \"https://vm:8443/mcp\",\n"
          "      \"headers\": {\n"
          "        \"Authorization\": \"Bearer ${FAIRYFLY_TOKEN}\"\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n");
}

TEST_CASE("client-config: golden Claude Desktop and mcp-remote output use env indirection", "[client_config]") {
    const auto snippets = build_client_configs({});
    const auto* desktop = find(snippets, "claude-desktop");
    REQUIRE(desktop);
    CHECK(desktop->body ==
          "{\n"
          "  \"mcpServers\": {\n"
          "    \"fairyfly\": {\n"
          "      \"command\": \"npx\",\n"
          "      \"args\": [\n"
          "        \"-y\",\n"
          "        \"mcp-remote\",\n"
          "        \"https://vm:8443/mcp\",\n"
          "        \"--header\",\n"
          "        \"Authorization:${FAIRYFLY_AUTH_HEADER}\"\n"
          "      ],\n"
          "      \"env\": {\n"
          "        \"FAIRYFLY_AUTH_HEADER\": \"Bearer <paste-your-token-here>\"\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n");
    CHECK(desktop->notes.find("spaces") != std::string::npos);   // the Windows argument bug is explained

    const auto* remote = find(snippets, "mcp-remote");
    REQUIRE(remote);
    CHECK(remote->body ==
          "export FAIRYFLY_AUTH_HEADER=\"Bearer ${FAIRYFLY_TOKEN}\"\n"
          "npx -y mcp-remote https://vm:8443/mcp --header \"Authorization:${FAIRYFLY_AUTH_HEADER}\"\n");
}

TEST_CASE("client-config: curl smoke and stdio variants", "[client_config]") {
    const auto snippets = build_client_configs({});
    const auto* curl = find(snippets, "curl");
    REQUIRE(curl);
    CHECK(curl->body.find("\"method\":\"initialize\"") != std::string::npos);
    CHECK(curl->body.find("\"method\":\"tools/list\"") != std::string::npos);
    CHECK(curl->body.find("Authorization: Bearer $FAIRYFLY_TOKEN") != std::string::npos);
    CHECK(curl->body.find("URL=https://vm:8443/mcp") != std::string::npos);
    CHECK(curl->body.find("<paste-your-token-here>") != std::string::npos);

    const auto* stdio = find(snippets, "stdio");
    REQUIRE(stdio);
    CHECK(stdio->body.find("claude mcp add fairyfly -- fairyfly mcp") != std::string::npos);
    CHECK(stdio->body.find("\"args\": [\n        \"mcp\"") != std::string::npos);
}

TEST_CASE("client-config: selectors, custom names and no real token", "[client_config]") {
    CHECK(build_client_configs({}).size() == 6);

    ClientConfigOptions only_curl;
    only_curl.curl = true;
    const auto one = build_client_configs(only_curl);
    REQUIRE(one.size() == 1);
    CHECK(one[0].id == "curl");

    ClientConfigOptions custom;
    custom.url = "https://sap-vm.corp:9443/mcp";
    custom.token_env = "SAP_MCP_TOKEN";
    custom.name = "sap-prod";
    custom.claude_code = true;
    const auto snippets = build_client_configs(custom);
    REQUIRE(snippets.size() == 2);
    CHECK(snippets[0].body.find("add --transport http sap-prod https://sap-vm.corp:9443/mcp") != std::string::npos);
    CHECK(snippets[0].body.find("${SAP_MCP_TOKEN}") != std::string::npos);
    for (const auto& s : build_client_configs({})) {
        INFO(s.id);
        CHECK(s.body.find("ffy_") == std::string::npos);
        CHECK(s.notes.find("ffy_") == std::string::npos);
    }
}

TEST_CASE("client-config: input validation refuses tokens and unsafe values", "[client_config]") {
    CHECK(validate_client_options({}).empty());
    ClientConfigOptions o;
    o.token_env = "ffy_abcd_0123456789abcdef";
    CHECK_FALSE(validate_client_options(o).empty());
    o = {};
    o.token_env = "BAD NAME";
    CHECK_FALSE(validate_client_options(o).empty());
    o = {};
    o.url = "https://user:pw@vm/mcp";
    CHECK_FALSE(validate_client_options(o).empty());
    o.url = "ftp://vm/mcp";
    CHECK_FALSE(validate_client_options(o).empty());
    o.url = "https://vm/mcp$(id)";
    CHECK_FALSE(validate_client_options(o).empty());
    o = {};
    o.url = "https://vm/mcp?t=ffy_abc_0123456789";
    CHECK_FALSE(validate_client_options(o).empty());
    o = {};
    o.name = "a b";
    CHECK_FALSE(validate_client_options(o).empty());
}

TEST_CASE("client-config: text and json rendering", "[client_config]") {
    const auto snippets = build_client_configs({});
    const auto text = render_client_configs_text(snippets);
    CHECK(text.find("=== Claude Code: claude mcp add ===") != std::string::npos);
    const auto json = render_client_configs_json(snippets, {});
    CHECK(json["url"] == "https://vm:8443/mcp");
    CHECK(json["configs"].size() == 6);
    CHECK(json["configs"][0]["id"] == "claude-code");
    CHECK(json["configs"][0]["config"].is_string());
}
