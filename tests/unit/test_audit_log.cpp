#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "include/audit_log.h"
#include "include/cli_entry.h"

using namespace fairyfly::audit;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

using Clock = std::chrono::system_clock;

Clock::time_point utc(int y, int m, int d, int hh, int mm, int ss, int ms = 0) {
    // days_from_civil (Howard Hinnant)
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097LL + static_cast<long long>(doe) - 719468;
    return Clock::time_point(std::chrono::milliseconds(
        (((days * 24 + hh) * 60 + mm) * 60 + ss) * 1000LL + ms));
}

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("fairyfly_audit_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                            std::to_string(counter++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

std::vector<std::string> read_lines(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

struct EnvGuard {
    std::string name;
    std::string old;
    bool had = false;
    EnvGuard(const std::string& n, const std::string& value) : name(n) {
        if (const char* v = std::getenv(n.c_str())) { had = true; old = v; }
        _putenv_s(n.c_str(), value.c_str());
    }
    ~EnvGuard() { _putenv_s(name.c_str(), had ? old.c_str() : ""); }
};

GetEnvFn env_of(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](const char* name) {
        auto it = vars.find(name);
        return it == vars.end() ? std::string() : it->second;
    };
}

AuditRecord sample_record() {
    AuditRecord r;
    r.ts = utc(2026, 9, 29, 12, 34, 56, 789);
    r.pid = 1234;
    r.command = "element fill";
    r.argv = {"element", "fill", "wnd[0]/usr/txtX", "secret-value", "--connection", "0"};
    r.connection = 0;
    r.sap = SapFacts{"A4H", "001", "DEVELOPER", "SU01"};
    r.read_only = false;
    r.batch_line = 3;
    r.status = "error";
    r.error_code = "READ_ONLY_REFUSED";
    r.exit_code = 1;
    r.duration_ms = 123;
    return r;
}

std::string run_cli_capture(std::vector<std::string> args, int& exit_code) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    std::ostringstream out, err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    exit_code = fairyfly::cli::run_cli(static_cast<int>(argv.size()), argv.data());
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    return out.str() + err.str();
}

} // namespace

TEST_CASE("redact_argv masks the fill value but keeps flags", "[audit]") {
    const auto out = redact_argv({"element", "fill", "wnd[0]/usr/txtX", "hunter2", "--connection", "0", "--commit"});
    REQUIRE(out == std::vector<std::string>{"element", "fill", "wnd[0]/usr/txtX", "<redacted>", "--connection", "0", "--commit"});

    const auto grid = redact_argv({"--read-only", "element", "fill", "wnd[0]/usr/cntl/shell", "42", "--row", "3",
                                   "--column", "AMOUNT", "--checkbox"});
    REQUIRE(grid == std::vector<std::string>{"--read-only", "element", "fill", "wnd[0]/usr/cntl/shell", "<redacted>", "--row", "3",
                                             "--column", "AMOUNT", "--checkbox"});
}

TEST_CASE("redact_argv leaves fill --clear untouched", "[audit]") {
    const std::vector<std::string> argv{"element", "fill", "wnd[0]/usr/txtX", "--clear"};
    REQUIRE(redact_argv(argv) == argv);
}

TEST_CASE("redact_argv masks sensitive option values", "[audit]") {
    REQUIRE(redact_argv({"session", "login", "--password", "x", "--connection", "0"}) ==
            std::vector<std::string>{"session", "login", "--password", "<redacted>", "--connection", "0"});
    REQUIRE(redact_argv({"session", "login", "--token=abc"}) == std::vector<std::string>{"session", "login", "--token=<redacted>"});
    REQUIRE(redact_argv({"x", "--client-secret", "s", "--api-key=k"}) ==
            std::vector<std::string>{"x", "--client-secret", "<redacted>", "--api-key=<redacted>"});
    // A sensitive flag followed by another option must not swallow it.
    REQUIRE(redact_argv({"session", "login", "--password-stdin", "--connection", "1"}) ==
            std::vector<std::string>{"session", "login", "--password-stdin", "--connection", "1"});
}

TEST_CASE("redact_argv handles JSON-array style values", "[audit]") {
    const auto out = redact_argv({"x", "--secret", "[\"a\",\"b\"]", "--items", "[\"a\",\"b\"]"});
    REQUIRE(out == std::vector<std::string>{"x", "--secret", "<redacted>", "--items", "[\"a\",\"b\"]"});
    const auto fill = redact_argv({"element", "fill", "wnd[0]/usr/txtX", "[\"pw\"]"});
    REQUIRE(fill[3] == "<redacted>");
}

TEST_CASE("redact_argv keeps the login credentials file path and search terms", "[audit]") {
    const std::vector<std::string> argv{"session", "login", "--credentials-file", "C:\\keys\\trial.cfg", "screen", "find",
                                        "--text-contains", "Invoice"};
    REQUIRE(redact_argv(argv) == argv);
}

TEST_CASE("redact_argv enforces element and array limits", "[audit]") {
    const auto long_out = redact_argv({"element", "get", std::string(2000, 'a')});
    REQUIRE(long_out[2].size() == 512);

    std::vector<std::string> many(200, "x");
    const auto out = redact_argv(many);
    REQUIRE(out.size() == 64);
    REQUIRE(out.back() == "<truncated>");
}

TEST_CASE("format_record emits one parseable line with fixed key order", "[audit]") {
    const auto line = format_record(sample_record());
    REQUIRE(line.back() == '\n');
    REQUIRE(line.find('\n') == line.size() - 1);
    const auto j = nlohmann::ordered_json::parse(line);
    std::vector<std::string> keys;
    for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.key());
    REQUIRE(keys == std::vector<std::string>{"ts", "v", "fairyfly", "pid", "cmd", "argv", "connection", "sap",
                                             "read_only", "batch_line", "status", "error_code", "exit",
                                             "duration_ms", "audit_source"});
    REQUIRE(j["ts"] == "2026-09-29T12:34:56.789Z");
    REQUIRE(j["v"] == 1);
    REQUIRE(j["argv"][3] == "<redacted>");
    REQUIRE(j["sap"]["system"] == "A4H");
    REQUIRE(j["batch_line"] == 3);
    REQUIRE(j["audit_source"] == "cli");
    REQUIRE(line.find("secret-value") == std::string::npos);
    REQUIRE(line.find("\"message\"") == std::string::npos);
}

TEST_CASE("format_record omits unavailable optional keys", "[audit]") {
    AuditRecord r = sample_record();
    r.connection.reset();
    r.sap.reset();
    r.batch_line.reset();
    r.status = "success";
    r.error_code.clear();
    r.exit_code = 0;
    const auto j = json::parse(format_record(r));
    REQUIRE_FALSE(j.contains("connection"));
    REQUIRE_FALSE(j.contains("sap"));
    REQUIRE_FALSE(j.contains("batch_line"));
    REQUIRE_FALSE(j.contains("error_code"));

    r.sap = SapFacts{};
    REQUIRE_FALSE(json::parse(format_record(r)).contains("sap"));
}

TEST_CASE("format_record stays within 16 KiB", "[audit]") {
    AuditRecord r = sample_record();
    r.argv.assign(64, std::string(512, 'z'));
    const auto line = format_record(r);
    REQUIRE(line.size() <= 16 * 1024);
    REQUIRE(json::parse(line).contains("argv"));
}

TEST_CASE("monthly_file_name uses the UTC month", "[audit]") {
    REQUIRE(monthly_file_name(utc(2026, 9, 29, 12, 0, 0)) == "2026-09.jsonl");
    REQUIRE(monthly_file_name(utc(2026, 9, 30, 23, 59, 59, 999)) == "2026-09.jsonl");
    REQUIRE(monthly_file_name(utc(2026, 10, 1, 0, 0, 0)) == "2026-10.jsonl");
    REQUIRE(monthly_file_name(utc(2027, 1, 1, 0, 0, 0)) == "2027-01.jsonl");
    REQUIRE(utc_timestamp_ms(utc(2026, 1, 2, 3, 4, 5, 7)) == "2026-01-02T03:04:05.007Z");
}

TEST_CASE("resolve_config applies env and flag precedence", "[audit]") {
    const auto now = utc(2026, 9, 29, 12, 0, 0);

    auto def = resolve_config(env_of({{"LOCALAPPDATA", "C:\\Users\\x\\AppData\\Local"}}), false, false, now);
    REQUIRE(def.mode == Mode::Enabled);
    REQUIRE(def.file == fs::path("C:\\Users\\x\\AppData\\Local") / "fairyfly" / "audit" / "2026-09.jsonl");

    auto file = resolve_config(env_of({{"LOCALAPPDATA", "C:\\L"}, {"FAIRYFLY_AUDIT_FILE", "D:\\a\\b.jsonl"}}),
                               false, false, now);
    REQUIRE(file.file == fs::path("D:\\a\\b.jsonl"));

    REQUIRE(resolve_config(env_of({{"FAIRYFLY_AUDIT", "0"}}), false, false, now).mode == Mode::Disabled);
    REQUIRE(resolve_config(env_of({{"FAIRYFLY_AUDIT", "OFF"}}), false, false, now).mode == Mode::Disabled);
    REQUIRE(resolve_config(env_of({{"FAIRYFLY_AUDIT", "required"}}), false, false, now).mode == Mode::Required);
    REQUIRE(resolve_config(env_of({}), true, false, now).mode == Mode::Disabled);
    REQUIRE(resolve_config(env_of({}), false, true, now).mode == Mode::Required);
    REQUIRE(resolve_config(env_of({{"FAIRYFLY_AUDIT", "0"}}), false, true, now).mode == Mode::Required);
    REQUIRE(resolve_config(env_of({{"FAIRYFLY_AUDIT", "required"}}), true, false, now).mode == Mode::Disabled);
}

TEST_CASE("AuditSink appends exactly one terminated line per record", "[audit]") {
    TempDir dir;
    const auto file = dir.path / "nested" / "audit.jsonl";
    AuditSink sink(AuditConfig{Mode::Enabled, file});
    REQUIRE(sink.probe());
    REQUIRE(sink.append(sample_record()));
    REQUIRE(sink.append(sample_record()));

    std::ifstream in(file, std::ios::binary);
    std::stringstream content;
    content << in.rdbuf();
    const auto text = content.str();
    REQUIRE(std::count(text.begin(), text.end(), '\n') == 2);
    REQUIRE(text.back() == '\n');
    for (const auto& line : read_lines(file)) REQUIRE_NOTHROW(json::parse(line));
}

TEST_CASE("AuditSink concurrent appends never interleave", "[audit]") {
    TempDir dir;
    const auto file = dir.path / "audit.jsonl";
    AuditSink sink(AuditConfig{Mode::Enabled, file});
    constexpr int kThreads = 8;
    constexpr int kPerThread = 25;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                AuditRecord r = sample_record();
                r.argv = {"element", "get", "wnd[0]/usr/txt" + std::to_string(t) + "-" + std::to_string(i)};
                sink.append(r);
            }
        });
    }
    for (auto& th : threads) th.join();
    const auto lines = read_lines(file);
    REQUIRE(lines.size() == static_cast<size_t>(kThreads * kPerThread));
    for (const auto& line : lines) REQUIRE_NOTHROW(json::parse(line));
}

TEST_CASE("AuditSink append to an unwritable path fails quietly", "[audit]") {
    TempDir dir;
    // A directory where the file should be: opening it for append must fail.
    const auto path = dir.path / "blocked";
    fs::create_directories(path);
    AuditSink sink(AuditConfig{Mode::Enabled, path});
    bool result = true;
    REQUIRE_NOTHROW(result = sink.append(sample_record()));
    REQUIRE_FALSE(result);
    REQUIRE_FALSE(sink.probe());
}

TEST_CASE("Required mode turns a failed probe into AUDIT_UNAVAILABLE", "[audit]") {
    REQUIRE(preflight_error(Mode::Required, false) == std::optional<std::string>("AUDIT_UNAVAILABLE"));
    REQUIRE_FALSE(preflight_error(Mode::Required, true).has_value());
    REQUIRE_FALSE(preflight_error(Mode::Enabled, false).has_value());
    REQUIRE_FALSE(preflight_error(Mode::Disabled, false).has_value());
}

TEST_CASE("CLI writes no audit file with --no-audit", "[audit][cli]") {
    TempDir dir;
    const auto file = dir.path / "audit.jsonl";
    EnvGuard f("FAIRYFLY_AUDIT_FILE", file.string());
    EnvGuard a("FAIRYFLY_AUDIT", "");
    int code = -1;
    run_cli_capture({"fairyfly", "--no-audit", "--help"}, code);
    REQUIRE(code == 0);
    run_cli_capture({"fairyfly", "--no-audit", "definitely-not-a-command"}, code);
    REQUIRE(code != 0);
    REQUIRE_FALSE(fs::exists(file));
}

TEST_CASE("CLI honours FAIRYFLY_AUDIT=0", "[audit][cli]") {
    TempDir dir;
    const auto file = dir.path / "audit.jsonl";
    EnvGuard f("FAIRYFLY_AUDIT_FILE", file.string());
    EnvGuard a("FAIRYFLY_AUDIT", "0");
    int code = -1;
    run_cli_capture({"fairyfly", "definitely-not-a-command"}, code);
    REQUIRE(code != 0);
    REQUIRE_FALSE(fs::exists(file));
}

TEST_CASE("CLI parse error writes exactly one audit record", "[audit][cli]") {
    TempDir dir;
    const auto file = dir.path / "audit.jsonl";
    EnvGuard f("FAIRYFLY_AUDIT_FILE", file.string());
    EnvGuard a("FAIRYFLY_AUDIT", "");
    int code = -1;
    run_cli_capture({"fairyfly", "definitely-not-a-command"}, code);
    REQUIRE(code != 0);
    const auto lines = read_lines(file);
    REQUIRE(lines.size() == 1);
    const auto j = json::parse(lines[0]);
    REQUIRE(j["v"] == 1);
    REQUIRE(j["status"] == "error");
    REQUIRE(j["error_code"] == "PARSE_ERROR");
    REQUIRE(j["exit"] == code);
    REQUIRE(j["argv"][0] == "definitely-not-a-command");
    REQUIRE_FALSE(j.contains("sap"));
    REQUIRE_FALSE(j.contains("message"));
}

TEST_CASE("CLI --audit-required fails before running when the file is unwritable", "[audit][cli]") {
    TempDir dir;
    const auto blocked = dir.path / "blocked";
    fs::create_directories(blocked);
    EnvGuard f("FAIRYFLY_AUDIT_FILE", blocked.string());
    EnvGuard a("FAIRYFLY_AUDIT", "");
    int code = -1;
    const auto output = run_cli_capture({"fairyfly", "--audit-required", "--help"}, code);
    REQUIRE(code == 1);
    REQUIRE(output.find("AUDIT_UNAVAILABLE") != std::string::npos);
}

TEST_CASE("AuditRecord source defaults to cli and MCP fields are omitted when empty", "[audit]") {
    AuditRecord r = sample_record();
    REQUIRE(r.source == "cli");
    const auto j = json::parse(format_record(r));
    REQUIRE(j["audit_source"] == "cli");
    REQUIRE_FALSE(j.contains("tool"));
    REQUIRE_FALSE(j.contains("client"));
    REQUIRE_FALSE(j.contains("request_id"));
}

TEST_CASE("AuditRecord MCP fields are emitted after audit_source in stable order", "[audit]") {
    AuditRecord r = sample_record();
    r.source = "mcp";
    r.tool = "gui_element_fill";
    r.client = "claude-code/1.2.3";
    r.request_id = "42";
    const auto j = nlohmann::ordered_json::parse(format_record(r));
    std::vector<std::string> keys;
    for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.key());
    REQUIRE(keys.size() == 18);
    REQUIRE(keys[14] == "audit_source");
    REQUIRE(keys[15] == "tool");
    REQUIRE(keys[16] == "client");
    REQUIRE(keys[17] == "request_id");
    REQUIRE(j["audit_source"] == "mcp");
}

TEST_CASE("AuditRecord with huge MCP fields stays within the record cap", "[audit]") {
    AuditRecord r = sample_record();
    r.source = "mcp";
    r.tool = std::string(5000, 't');
    r.client = std::string(5000, 'c');
    r.request_id = std::string(5000, 'r');
    const auto line = format_record(r);
    REQUIRE(line.size() <= 16 * 1024);
    const auto j = json::parse(line);
    REQUIRE(j["tool"].get<std::string>().size() <= 64);
    REQUIRE(j["request_id"].get<std::string>().size() <= 64);
}
