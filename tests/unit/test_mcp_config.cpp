#include <catch2/catch_test_macros.hpp>

#include <map>

#include "include/config/mcp_config.h"

using namespace fairyfly;
using namespace fairyfly::config;

namespace {

EnvLookup fake_env(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](const char* name) {
        const auto it = vars.find(name);
        return it == vars.end() ? std::string() : it->second;
    };
}

const ConfigIssue* find_issue(const ParseResult& r, const std::string& code) {
    for (const auto& i : r.issues)
        if (i.code == code) return &i;
    return nullptr;
}

} // namespace

TEST_CASE("config: a valid file is parsed into typed values", "[config]") {
    const auto r = parse_yaml(
        "server:\n"
        "  host: 127.0.0.1\n"
        "  port: 9000\n"
        "  transport: http\n"
        "  sse: true\n"
        "  allowed_hosts: [vm, vm.corp]\n"
        "mode:\n"
        "  read_only: true\n"
        "limits:\n"
        "  call_timeout_ms: 5000\n"
        "tools:\n"
        "  families: [session, screen]\n"
        "default_connection: 2\n"
        "format: json\n"
        "auth:\n"
        "  token_prefix: ffy\n");
    REQUIRE(r.ok());
    CHECK(r.issues.empty());
    CHECK(r.config.get_int("server.port") == 9000);
    CHECK(r.config.get_string("server.transport") == "http");
    CHECK(r.config.get_bool("server.sse") == true);
    CHECK(r.config.get_list("server.allowed_hosts") == std::vector<std::string>{"vm", "vm.corp"});
    CHECK(r.config.get_int("default_connection") == 2);
    CHECK(r.config.lines.at("server.port") == 3);

    mcp::ServeOptions options;
    apply_config(r.config, options);
    CHECK(options.port == 9000);
    CHECK(options.transport == "http");
    CHECK(options.read_only);
    CHECK(options.call_timeout_ms == 5000);
    CHECK(options.families == std::vector<std::string>{"session", "screen"});
    CHECK(options.default_connection == 2);
    CHECK(options.format == "json");
}

TEST_CASE("config: apply_config only touches keys that are present", "[config]") {
    mcp::ServeOptions options;
    options.max_calls_per_minute = 7;
    options.format = "json";
    McpConfig layer;
    layer.set("limits.call_timeout_ms", 4242LL);
    apply_config(layer, options);
    CHECK(options.call_timeout_ms == 4242);
    CHECK(options.max_calls_per_minute == 7);
    CHECK(options.format == "json");
    CHECK_FALSE(options.default_connection.has_value());
}

TEST_CASE("config: validation errors carry line numbers", "[config]") {
    struct Case { const char* yaml; const char* code; int line; };
    const Case cases[] = {
        {"server:\n  port: 99999\n", "CONFIG_INVALID_VALUE", 2},
        {"server:\n  port: abc\n", "CONFIG_INVALID_VALUE", 2},
        {"\n\nserver:\n  transport: smtp\n", "CONFIG_INVALID_VALUE", 4},
        {"mode:\n  read_only: maybe\n", "CONFIG_INVALID_VALUE", 2},
        {"limits:\n  call_timeout_ms: 5\n", "CONFIG_INVALID_VALUE", 2},
        {"tools:\n  families: [session, nonsense]\n", "CONFIG_INVALID_VALUE", 2},
        {"server:\n  allowed_hosts: notalist\n", "CONFIG_INVALID_VALUE", 2},
        {"server: 5\n", "CONFIG_INVALID_STRUCTURE", 1},
        {"auth:\n  token_prefix: BAD!\n", "CONFIG_INVALID_VALUE", 2},
        {"auth:\n  proxy_secret_source: file\n", "CONFIG_INVALID_VALUE", 2},
        {"format: xml\n", "CONFIG_INVALID_VALUE", 1},
        {"- a\n- b\n", "CONFIG_INVALID_STRUCTURE", 1},
        {"server:\n  host: [\n", "CONFIG_PARSE_ERROR", 0},
        {"mode:\n  read_only: true\n  allow_write: true\n", "CONFIG_CONFLICT", 3},
    };
    for (const auto& c : cases) {
        INFO(c.yaml);
        const auto r = parse_yaml(c.yaml);
        CHECK_FALSE(r.ok());
        const auto* issue = find_issue(r, c.code);
        REQUIRE(issue != nullptr);
        if (c.line > 0) CHECK(issue->line == c.line);
        else CHECK(issue->line >= 1);
        CHECK(r.config.values.empty());   // an invalid file yields no partial settings
    }
}

TEST_CASE("config: unknown keys warn, secret-like keys and values are handled", "[config][secret]") {
    SECTION("unknown key and unknown section only warn") {
        const auto r = parse_yaml("server:\n  port: 8383\n  colour: red\nextra: 1\n");
        CHECK(r.ok());
        const auto* issue = find_issue(r, "CONFIG_UNKNOWN_KEY");
        REQUIRE(issue != nullptr);
        CHECK(issue->line == 3);
        CHECK(r.config.get_int("server.port") == 8383);
    }
    SECTION("an empty secret-like key only warns") {
        const auto r = parse_yaml("auth:\n  password:\n");
        CHECK(r.ok());
        CHECK(find_issue(r, "CONFIG_SECRET_KEY") != nullptr);
    }
    SECTION("a secret-like key with a value is refused") {
        for (const char* key : {"password", "secret", "token", "token_value", "proxy_secret", "api_key"}) {
            const auto r = parse_yaml(std::string("auth:\n  ") + key + ": hunter2hunter2\n");
            INFO(key);
            CHECK_FALSE(r.ok());
            CHECK(r.has_secret());
            CHECK(r.first_error_code() == "CONFIG_CONTAINS_SECRET");
        }
    }
    SECTION("token-shaped values are refused wherever they appear and never echoed") {
        const auto r = parse_yaml("server:\n  host: ffy_abc123_0123456789abcdefSECRETPART\n");
        CHECK_FALSE(r.ok());
        CHECK(r.has_secret());
        for (const auto& i : r.issues) CHECK(i.message.find("SECRETPART") == std::string::npos);
        const auto bearer = parse_yaml("server:\n  allowed_hosts: [\"Bearer abcdef\"]\n");
        CHECK(bearer.has_secret());
    }
    SECTION("the known auth keys are not mistaken for secrets") {
        const auto r = parse_yaml("auth:\n  token_prefix: ffy\n  proxy_secret_source: credential-manager\n");
        CHECK(r.ok());
        CHECK(r.issues.empty());
    }
}

TEST_CASE("config: precedence flag > env > yaml > default with sources", "[config][precedence]") {
    McpConfig yaml;
    yaml.set("server.port", 1111LL);
    yaml.set("limits.max_calls_per_minute", 10LL);
    yaml.set("format", std::string("json"));
    McpConfig flags;
    flags.set("server.port", 3333LL);
    const auto env = fake_env({{"FAIRYFLY_MCP_SERVER_PORT", "2222"}, {"FAIRYFLY_MCP_LIMITS_MAX_CALLS_PER_MINUTE", "20"}});

    std::vector<ConfigIssue> issues;
    const auto effective = resolve(yaml, flags, env, &issues);
    CHECK(issues.empty());
    CHECK(std::get<long long>(effective.at("server.port").value) == 3333);
    CHECK(effective.at("server.port").source == Source::Flag);
    CHECK(std::get<long long>(effective.at("limits.max_calls_per_minute").value) == 20);
    CHECK(effective.at("limits.max_calls_per_minute").source == Source::Env);
    CHECK(std::get<std::string>(effective.at("format").value) == "json");
    CHECK(effective.at("format").source == Source::Yaml);
    CHECK(std::get<long long>(effective.at("limits.call_timeout_ms").value) == 120000);
    CHECK(effective.at("limits.call_timeout_ms").source == Source::Default);
    CHECK_FALSE(effective.at("default_connection").set);
    CHECK(format_effective(effective).find("flag") != std::string::npos);
    CHECK(effective_to_json(effective)["server.port"]["source"] == "flag");

    mcp::ServeOptions options;
    apply_config(merged_layers(yaml, flags, env), options);
    CHECK(options.port == 3333);
    CHECK(options.max_calls_per_minute == 20);
    CHECK(options.format == "json");
    CHECK(options.call_timeout_ms == 120000);
}

TEST_CASE("config: invalid environment values are ignored with a warning", "[config][precedence]") {
    std::vector<ConfigIssue> issues;
    McpConfig yaml;
    yaml.set("server.port", 1111LL);
    const auto effective = resolve(yaml, {}, fake_env({{"FAIRYFLY_MCP_SERVER_PORT", "notanumber"},
                                                        {"FAIRYFLY_MCP_MODE_READ_ONLY", "yes"},
                                                        {"FAIRYFLY_MCP_TOOLS_FAMILIES", "session, screen"}}), &issues);
    CHECK(std::get<long long>(effective.at("server.port").value) == 1111);
    CHECK(effective.at("server.port").source == Source::Yaml);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "CONFIG_ENV_INVALID");
    CHECK(std::get<bool>(effective.at("mode.read_only").value));
    CHECK(std::get<std::vector<std::string>>(effective.at("tools.families").value) ==
          std::vector<std::string>{"session", "screen"});
}

TEST_CASE("config: path resolution flag > env > default", "[config][precedence]") {
    auto p = resolve_config_path("C:\\x\\a.yaml", fake_env({{"FAIRYFLY_MCP_CONFIG", "C:\\y.yaml"}}));
    CHECK(p.source == Source::Flag);
    CHECK(p.explicit_request);
    CHECK(p.path == std::filesystem::path("C:\\x\\a.yaml"));
    p = resolve_config_path("", fake_env({{"FAIRYFLY_MCP_CONFIG", "C:\\y.yaml"}}));
    CHECK(p.source == Source::Env);
    p = resolve_config_path("", fake_env({{"LOCALAPPDATA", "C:\\Users\\u\\AppData\\Local"}}));
    CHECK(p.source == Source::Default);
    CHECK_FALSE(p.explicit_request);
    CHECK(p.path.filename() == "mcp.yaml");
    CHECK(p.path.parent_path().filename() == "fairyfly");
}

TEST_CASE("config: the template validates and equals the defaults", "[config][template]") {
    const std::string text = config_template();
    const auto r = parse_yaml(text);
    REQUIRE(r.ok());
    CHECK(r.issues.empty());
    CHECK_FALSE(r.has_secret());
    // Every active line equals the default, so resolving the template changes nothing.
    const auto effective = resolve(r.config, {}, fake_env({}));
    for (const auto& spec : key_specs()) {
        if (!spec.def) { CHECK_FALSE(effective.at(spec.key).set); continue; }
        CHECK(value_to_string(effective.at(spec.key).value) == value_to_string(*spec.def));
    }
    // Documented for every key.
    for (const auto& spec : key_specs()) CHECK(text.find(spec.doc) != std::string::npos);
    CHECK(text.find("NO SECRETS") != std::string::npos);
}

TEST_CASE("config: key table sanity", "[config]") {
    for (const auto& spec : key_specs()) {
        INFO(spec.key);
        CHECK_FALSE(spec.doc.empty());
        CHECK(spec.env.rfind("FAIRYFLY_MCP_", 0) == 0);
        if (spec.def) {
            std::string why;
            CHECK(parse_value(spec, value_to_string(*spec.def) == "[]" ? "" : value_to_string(*spec.def), &why).has_value());
        }
    }
    CHECK(find_key("server.nonexistent") == nullptr);
}

TEST_CASE("config: issue formatting", "[config]") {
    ConfigIssue issue{Severity::Error, "CONFIG_INVALID_VALUE", "bad", 12, "server.port"};
    CHECK(format_issue(issue, "mcp.yaml") == "mcp.yaml:12: error CONFIG_INVALID_VALUE: bad");
    issue.line = 0;
    CHECK(format_issue(issue, "mcp.yaml") == "mcp.yaml: error CONFIG_INVALID_VALUE: bad");
}
