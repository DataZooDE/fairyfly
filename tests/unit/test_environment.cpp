// The one reader of fairyfly's environment variables (config/environment.h): every switch such as
// FAIRYFLY_READ_ONLY is parsed in one place, so no two parts of fairyfly can disagree about it.
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <sstream>

#include "include/config/environment.h"
#include "include/mcp/policy.h"
#include "support/env_guard.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <vector>
#include <map>
#include <string>

using namespace fairyfly;

namespace {
config::EnvLookup env_of(std::map<std::string, std::string> vars) {
    return [vars](const char* name) {
        auto i = vars.find(name);
        return i == vars.end() ? std::string() : i->second;
    };
}

}  // namespace

TEST_CASE("environment: a flag is on for 1/true/yes/on in any case, with surrounding spaces", "[config][env]") {
    for (const char* on : {"1", "true", "TRUE", "True", "yes", "YES", "Yes", "on", "ON", "On", " yes ", "\ttrue\n"})
        CHECK(config::parse_flag(on));
    for (const char* off : {"", "0", "false", "FALSE", "no", "off", "2", "y", "enabled", "tru", "yes please"})
        CHECK_FALSE(config::parse_flag(off));
    // explicitly off: 0/false/no/off, same rules; anything else (also "") is not an explicit off
    for (const char* off : {"0", "false", "FALSE", "No", "off", " Off "}) CHECK(config::parse_off(off));
    for (const char* other : {"", "1", "yes", "2", "required"}) CHECK_FALSE(config::parse_off(other));
    // ASCII case folding only: no locale may turn a non-ASCII letter into a match
    CHECK_FALSE(config::parse_flag("\xC4N"));
    CHECK(config::normalized(" TrUe\t") == "true");
}

TEST_CASE("environment: a switch that is on by default is off only for 0/false/no/off", "[config][env]") {
    // FAIRYFLY_UPDATE_CHECK: "OFF" or " no " must turn the check off, an unknown value keeps it on
    for (const char* off : {"0", "OFF", " no ", "False"})
        CHECK_FALSE(config::Environment(env_of({{config::env::kUpdateCheck, off}})).enabled(config::env::kUpdateCheck));
    for (const char* on : {"", "1", "yes", "sometimes"})
        CHECK(config::Environment(env_of({{config::env::kUpdateCheck, on}})).enabled(config::env::kUpdateCheck));
}

TEST_CASE("environment: an injected lookup answers get, flag and the read-only cap", "[config][env]") {
    const config::Environment env(env_of({{config::env::kReadOnly, "Yes"}, {"FAIRYFLY_X", " value "}}));
    CHECK(env.get("FAIRYFLY_X") == " value ");
    CHECK(env.get("FAIRYFLY_UNSET").empty());
    CHECK(env.flag(config::env::kReadOnly));
    CHECK(env.read_only_cap());
    CHECK_FALSE(config::Environment(env_of({})).read_only_cap());
    CHECK_FALSE(config::Environment(env_of({{config::env::kReadOnly, "0"}})).read_only_cap());
}

TEST_CASE("environment: the process environment is one shared instance and reads the live value", "[config][env]") {
    const config::Environment& a = config::Environment::process();
    const config::Environment& b = config::Environment::process();
    CHECK(&a == &b);
    {
        test::EnvGuard on("FAIRYFLY_ENV_TEST_FLAG", "On");
        CHECK(a.get("FAIRYFLY_ENV_TEST_FLAG") == "On");
        CHECK(a.flag("FAIRYFLY_ENV_TEST_FLAG"));
    }
    CHECK_FALSE(a.flag("FAIRYFLY_ENV_TEST_FLAG"));
    CHECK(config::process_env()("FAIRYFLY_ENV_TEST_FLAG").empty());
}

TEST_CASE("environment: FAIRYFLY_READ_ONLY=Yes is the hard cap for the MCP policy too", "[config][env][mcp]") {
    // review 2026-10-25: policy.cpp lower-cased the value, other readers matched exact spellings only
    for (const char* on : {"Yes", "ON", "True", "1"}) {
        const auto env = env_of({{config::env::kReadOnly, on}});
        CHECK(mcp::env_read_only(env));
        CHECK(config::Environment(env).read_only_cap());
    }
    {
        test::EnvGuard on(config::env::kReadOnly, " On ");
        CHECK(config::read_only_cap());
        CHECK(mcp::env_read_only({}));  // no lookup: the process environment
    }
    test::EnvGuard off(config::env::kReadOnly, "");
    CHECK_FALSE(config::read_only_cap());
}

TEST_CASE("environment: LOCALAPPDATA, or the temp directory when it is unset", "[config][env]") {
    CHECK(config::Environment(env_of({{config::env::kLocalAppData, R"(C:\Users\x\AppData\Local)"}})).local_app_data() ==
          std::filesystem::path(R"(C:\Users\x\AppData\Local)"));
    CHECK(config::Environment(env_of({})).local_app_data() == std::filesystem::temp_directory_path());
}

TEST_CASE("environment: fairyfly's data directory is LOCALAPPDATA\fairyfly (temp\fairyfly when unset)", "[config][env]") {
    CHECK(config::Environment(env_of({{config::env::kLocalAppData, R"(C:\L)"}})).data_dir() == std::filesystem::path(R"(C:\L\fairyfly)"));
    CHECK(config::Environment(env_of({})).data_dir() == std::filesystem::temp_directory_path() / "fairyfly");
}

namespace {
/// The matches of `re` (group `group`) on the lines of `text` that contain one of `markers` (std::regex over whole
/// source files took 20 s; the markers keep it to the few lines that can match).
std::vector<std::string> matching_lines(const std::string& text, std::initializer_list<const char*> markers, const std::regex& re,
                                        int group) {
    std::vector<std::string> found;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (std::none_of(markers.begin(), markers.end(), [&](const char* m) { return line.find(m) != std::string::npos; })) continue;
        for (std::sregex_iterator m(line.begin(), line.end(), re), end; m != end; ++m) found.push_back((*m)[group].str());
    }
    return found;
}
}  // namespace

TEST_CASE("environment: no file under src/ reads the environment except config/environment.cpp", "[config][env]") {
    // keeps the one reader the only one: a new getenv/_dupenv_s elsewhere would bring back diverging parsers
    namespace fs = std::filesystem;
    const fs::path src = FAIRYFLY_SOURCE_DIR;
    REQUIRE(fs::exists(src / "config" / "environment.cpp"));
    std::vector<std::string> offenders;
    for (const auto& e : fs::recursive_directory_iterator(src)) {
        const auto ext = e.path().extension();
        if (!e.is_regular_file() || (ext != ".cpp" && ext != ".h" && ext != ".hpp")) continue;
        const fs::path rel = fs::relative(e.path(), src);
        if (rel == fs::path("config") / "environment.cpp") continue;
        std::ifstream in(e.path(), std::ios::binary);
        REQUIRE(in.good());
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        static const std::regex call(R"(\b(std::)?(getenv|_dupenv_s|_wgetenv|_wdupenv_s|GetEnvironmentVariable[AW]?|ffy::env_or)\s*\()");
        for (const auto& hit : matching_lines(text, {"getenv", "dupenv", "GetEnvironmentVariable", "env_or"}, call, 2))
            offenders.push_back(rel.string() + ": " + hit);
    }
    CAPTURE(offenders);
    CHECK(offenders.empty());
}

TEST_CASE("environment: a process variable is set and removed through config", "[config][env]") {
    // review 2026-10-25: the tray and the PowerShell runner wrote the environment with their own spellings
    const char* name = "FAIRYFLY_TEST_SET_PROCESS_ENV";
    config::set_process_env(name, "tr\xC3\xA4y");  // UTF-8 survives the wide API
    CHECK(config::Environment::process().get(name) == "tr\xC3\xA4y");
    config::unset_process_env(name);
    CHECK(config::Environment::process().get(name).empty());
}

TEST_CASE("environment: names and writers of the environment live in config only", "[config][env]") {
    namespace fs = std::filesystem;
    const fs::path src = FAIRYFLY_SOURCE_DIR;
    std::vector<std::string> offenders;
    for (const auto& e : fs::recursive_directory_iterator(src)) {
        const auto ext = e.path().extension();
        if (!e.is_regular_file() || (ext != ".cpp" && ext != ".h" && ext != ".hpp")) continue;
        const fs::path rel = fs::relative(e.path(), src);
        if (rel.parent_path() == fs::path("config") || rel.parent_path() == fs::path("include") / "config") continue;
        std::ifstream in(e.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        static const std::regex name(R"re(L?"FAIRYFLY_[A-Z_]+")re");
        static const std::regex writer(R"(\b(SetEnvironmentVariable[AW]?|_w?putenv(_s)?|(un)?setenv)\s*\()");
        for (const auto& hit : matching_lines(text, {"FAIRYFLY_"}, name, 0)) offenders.push_back(rel.string() + ": " + hit);
        // the windowless starter is its own small exe without fairyfly_core: it writes with the Win32 call itself
        if (rel == fs::path("tray_launcher") / "fairyfly_tray.cpp") continue;
        for (const auto& hit : matching_lines(text, {"putenv", "setenv", "SetEnvironmentVariable"}, writer, 1))
            offenders.push_back(rel.string() + ": " + hit);
    }
    CAPTURE(offenders);
    CHECK(offenders.empty());
}

TEST_CASE("environment: the writer guard recognises every writer spelling", "[config][env]") {
    // codex 2026-10-25: the guard missed _wputenv_s (config's own writer) and unsetenv
    const std::regex writer(R"(\b(SetEnvironmentVariable[AW]?|_w?putenv(_s)?|(un)?setenv)\s*\()");
    for (const char* call : {"SetEnvironmentVariableW(", "SetEnvironmentVariable (", "_putenv(", "_putenv_s(", "_wputenv(",
                             "_wputenv_s(", "setenv(", "unsetenv("})
        CHECK(std::regex_search(std::string(call), writer));
    CHECK_FALSE(std::regex_search(std::string("set_process_env("), writer));
}
