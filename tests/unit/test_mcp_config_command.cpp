// `fairyfly mcp config path|init|validate|show` through the CLI entry (review 2026-10-25: 1 of 18 functions of
// mcp_config_command was tested). The config file is FAIRYFLY_MCP_CONFIG in a temp directory.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/cli_entry.h"
#include "include/config/environment.h"
#include "include/config/mcp_config.h"
#include "include/tray/mcp_tray.h"
#include "include/text/utf8.h"
#include "support/env_guard.h"
#include "support/temp_dir.h"

using json = nlohmann::json;

namespace {

struct Run {
    int exit_code = -1;
    std::string out;
};

Run cli(std::vector<std::string> args) {
    args.insert(args.begin(), "fairyfly");
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    std::ostringstream out, err;
    auto* previous_out = std::cout.rdbuf(out.rdbuf());
    auto* previous_err = std::cerr.rdbuf(err.rdbuf());
    Run r;
    try {
        r.exit_code = fairyfly::cli::run_cli(static_cast<int>(argv.size()), argv.data());
    } catch (...) {
        r.exit_code = -2;
    }
    std::cout.rdbuf(previous_out);
    std::cerr.rdbuf(previous_err);
    r.out = out.str() + err.str();
    return r;
}

json parsed(const Run& r) {
    const auto start = r.out.find('{');
    return start == std::string::npos ? json() : json::parse(r.out.substr(start), nullptr, false);
}

struct ConfigFile {
    fairyfly::test::TempDir dir{"mcp-config-command"};
    std::filesystem::path file = dir.path() / "mcp.yaml";
    fairyfly::test::EnvGuard config{fairyfly::config::env::kMcpConfig, fairyfly::text::path_utf8(file)};
    fairyfly::test::EnvGuard no_audit{fairyfly::config::env::kAudit, "0"};
    void write(const std::string& text) const { std::ofstream(file, std::ios::binary) << text; }
    std::string read() const {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
};

}  // namespace

TEST_CASE("mcp config path names the file FAIRYFLY_MCP_CONFIG points to and whether it exists", "[mcp][config][command]") {
    ConfigFile cfg;
    const Run r = cli({"mcp", "config", "path", "--output", "json"});
    REQUIRE(r.exit_code == 0);
    const json j = parsed(r);
    CHECK(fairyfly::text::utf8_path(j["data"]["path"].get<std::string>()) == cfg.file);
    CHECK(j["data"]["source"] == "env");
    CHECK(j["data"]["exists"] == false);
}

TEST_CASE("mcp config init writes the template once; --force overwrites", "[mcp][config][command]") {
    ConfigFile cfg;
    REQUIRE(cli({"mcp", "config", "init", "--output", "json"}).exit_code == 0);
    CHECK(cfg.read() == fairyfly::config::config_template());   // exactly the template
    cfg.write("server:\n  port: 9555\n");
    const Run again = cli({"mcp", "config", "init", "--output", "json"});
    CHECK(again.exit_code != 0);
    CHECK(parsed(again)["error"]["code"] == "CONFIG_EXISTS");
    CHECK(cfg.read() == "server:\n  port: 9555\n");   // the refusal leaves the file as it was
    REQUIRE(cli({"mcp", "config", "init", "--force", "--output", "json"}).exit_code == 0);
    CHECK(cfg.read() == fairyfly::config::config_template());
}

TEST_CASE("mcp config validate: a missing file, a valid file and a file that holds a secret", "[mcp][config][command]") {
    ConfigFile cfg;
    const Run missing = cli({"mcp", "config", "validate", "--output", "json"});
    CHECK(missing.exit_code != 0);
    CHECK(parsed(missing)["error"]["code"] == "CONFIG_NOT_FOUND");
    cfg.write("server:\n  port: 9555\n");
    const Run valid = cli({"mcp", "config", "validate", "--output", "json"});
    CHECK(valid.exit_code == 0);
    CHECK(parsed(valid)["data"]["valid"] == true);
    cfg.write("server:\n  password: hunter2\n");
    const Run secret = cli({"mcp", "config", "validate", "--output", "json"});
    CHECK(secret.exit_code == 1);
    CHECK(parsed(secret)["data"]["valid"] == false);
    CHECK(secret.out.find("CONFIG_CONTAINS_SECRET") != std::string::npos);
}

TEST_CASE("mcp config show reports the effective values: a flag over the file", "[mcp][config][command]") {
    ConfigFile cfg;
    cfg.write("server:\n  port: 9555\n");
    const Run file_run = cli({"mcp", "config", "show", "--output", "json"});
    REQUIRE(file_run.exit_code == 0);
    const json port = parsed(file_run)["data"]["values"]["server.port"];
    CHECK(port["value"] == 9555);
    CHECK(port["source"] == "yaml");
    const Run flag_run = cli({"mcp", "--port", "9666", "config", "show", "--output", "json"});
    REQUIRE(flag_run.exit_code == 0);
    const json flagged = parsed(flag_run)["data"]["values"]["server.port"];
    CHECK(flagged["value"] == 9666);
    CHECK(flagged["source"] == "flag");
}

TEST_CASE("mcp config show refuses a flag value the key table does not allow", "[mcp][config][command]") {
    // the flag was dropped silently and the raw value stayed in the server options (1 ms despite the 1000 ms minimum)
    ConfigFile cfg;
    cfg.write("server:\n  port: 9555\n");
    const Run r = cli({"mcp", "--call-timeout-ms", "1", "config", "show", "--output", "json"});
    CHECK(r.exit_code != 0);
    CHECK(r.out.find("CONFIG_FLAG_INVALID") != std::string::npos);
    CHECK(r.out.find("--call-timeout-ms") != std::string::npos);
}

TEST_CASE("an mcp command leaves no tray runner factory behind", "[mcp][config][command]") {
    // the factory captured run_cli's locals by reference: after run_cli returned it dangled, and a later test saw it
    ConfigFile cfg;
    REQUIRE(cli({"mcp", "config", "path", "--output", "json"}).exit_code == 0);
    CHECK_FALSE(static_cast<bool>(fairyfly::tray::runner_factory()));
}
