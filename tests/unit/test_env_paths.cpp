// Environment values are UTF-8 (config::Environment reads the wide environment); every path made from one must
// interpret it as UTF-8, not in the ANSI code page (codex 2026-10-25: a user profile "Jörg" became "JÃ¶rg").
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#include "include/audit_log.h"
#include "include/config/environment.h"
#include "include/config/mcp_config.h"
#include "include/connection_manager.h"
#include "include/link/link_runner.h"
#include "include/text/utf8.h"
#include "support/env_guard.h"
#include "support/temp_dir.h"

using namespace fairyfly;
namespace fs = std::filesystem;

namespace {
// "C:\Users\Jörg\AppData\Local" in UTF-8 and as the native wide path
const std::string kUtf8Dir = "C:\\Users\\J\xC3\xB6rg\\AppData\\Local";
const fs::path kWideDir = fs::path(L"C:\\Users\\J\u00F6rg\\AppData\\Local");
// "C:\Users\Jörg", the profile the data directory lives in
const std::string kUtf8Profile = "C:\\Users\\J\xC3\xB6rg";
const fs::path kWideProfile = fs::path(L"C:\\Users\\J\u00F6rg");

config::EnvLookup only(const char* key, const std::string& value) {
    return [key = std::string(key), value](const char* name) { return key == name ? value : std::string(); };
}
}  // namespace

TEST_CASE("env paths: UTF-8 text and native paths convert both ways", "[config][env][paths]") {
    CHECK(text::utf8_path(kUtf8Dir) == kWideDir);
    CHECK(text::path_utf8(kWideDir) == kUtf8Dir);
    CHECK(text::path_utf8(text::utf8_path("plain/ascii")) == "plain/ascii");
}

TEST_CASE("env paths: LOCALAPPDATA and the data directory keep a non-ASCII user name", "[config][env][paths]") {
    const config::Environment env(only(config::env::kLocalAppData, kUtf8Dir));
    CHECK(env.local_app_data() == kWideDir);
    CHECK(env.legacy_data_dir() == kWideDir / "fairyfly");
    CHECK(config::Environment(only(config::env::kUserProfile, kUtf8Profile)).data_dir() == kWideProfile / ".fairyfly");
}

TEST_CASE("env paths: FAIRYFLY_AUDIT_FILE and FAIRYFLY_MCP_CONFIG keep a non-ASCII path", "[config][env][paths]") {
    const auto audit = audit::resolve_config(only(config::env::kAuditFile, kUtf8Dir + "\\audit.jsonl"), false, false,
                                             std::chrono::system_clock::now());
    CHECK(audit.file == kWideDir / "audit.jsonl");
    const auto mcp = config::resolve_config_path("", only(config::env::kMcpConfig, kUtf8Dir + "\\mcp.yaml"));
    CHECK(mcp.path == kWideDir / "mcp.yaml");
    const auto by_default = config::resolve_config_path("", only(config::env::kUserProfile, kUtf8Profile));
    CHECK(by_default.path == kWideProfile / ".fairyfly" / "mcp.yaml");
}

TEST_CASE("env paths: the session cache and the link state under a non-ASCII USERPROFILE", "[config][env][paths]") {
    test::TempDir root("fairyfly-env-paths");
    const fs::path profile = root.path() / L"J\u00F6rg";
    const test::EnvGuard user(config::env::kUserProfile, text::path_utf8(profile));
    const test::EnvGuard no_override(config::env::kDataDir, "");
    const test::EnvGuard no_cache(config::env::kCacheDir, "");
    CHECK(fs::path(cli::ConnectionManager::get_default_cache_directory()) == profile / ".fairyfly" / "sessions");
    CHECK(fs::exists(profile / ".fairyfly" / "sessions"));
    CHECK(fs::path(link::default_state_path()) == profile / ".fairyfly" / "link" / "state.json");
}

TEST_CASE("env paths: FAIRYFLY_DATA_DIR moves the session cache and the link state", "[config][env][paths]") {
    test::TempDir root("fairyfly-env-data-dir");
    const fs::path data = root.path() / L"d\u00E4ta";
    const test::EnvGuard over(config::env::kDataDir, text::path_utf8(data));
    const test::EnvGuard no_cache(config::env::kCacheDir, "");
    CHECK(fs::path(cli::ConnectionManager::get_default_cache_directory()) == data / "sessions");
    CHECK(fs::path(link::default_state_path()) == data / "link" / "state.json");
}
