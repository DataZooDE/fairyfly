#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "include/audit_log.h"
#include "include/mcp/mcp_audit.h"
#include "include/mcp/policy.h"
#include "include/mcp/tool_catalog.h"

using namespace fairyfly::mcp;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

ToolSpec spec_named(const std::string& name, bool write) {
    ToolSpec s;
    s.def.name = name;
    s.write_tool = write;
    return s;
}

Policy read_only_policy() { return Policy{}; }

Policy write_policy() {
    Policy p;
    p.read_only = false;
    p.allow_write = true;
    return p;
}

const EnvFn no_env = [](const char*) { return std::string(); };

ToolSpec fill_spec() {
    auto specs = write_tool_specs();
    REQUIRE(specs.size() == 1);
    return specs[0];
}

std::vector<std::string> fill_argv(const json& args, const Policy& p = Policy{}) {
    return fill_spec().build_argv(args, p);
}

struct TempFile {
    fs::path path;
    TempFile() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() /
               ("fairyfly_mcp_audit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "_" + std::to_string(counter++) + ".jsonl");
    }
    ~TempFile() { std::error_code ec; fs::remove(path, ec); }
    std::vector<json> records() const {
        std::ifstream in(path, std::ios::binary);
        std::vector<json> out;
        std::string line;
        while (std::getline(in, line)) out.push_back(json::parse(line));
        return out;
    }
};

McpCallRecord fill_call_record() {
    McpCallRecord r;
    r.tool = "gui_element_fill";
    r.command = "element fill";
    r.argv = {"element", "fill", "wnd[0]/usr/txtX", "hunter2-secret", "--connection", "0"};
    r.connection = 0;
    r.status = "success";
    r.client = "test-client/1.0";
    r.request_id = "17";
    r.duration_ms = 12;
    r.read_only = false;
    return r;
}

} // namespace

TEST_CASE("tool_visible hides write tools in read-only mode", "[mcp][policy]") {
    const auto read = spec_named("gui_screen_read", false);
    const auto fill = spec_named("gui_element_fill", true);
    REQUIRE(tool_visible(read, read_only_policy()));
    REQUIRE_FALSE(tool_visible(fill, read_only_policy()));
    REQUIRE(tool_visible(read, write_policy()));
    REQUIRE(tool_visible(fill, write_policy()));
}

TEST_CASE("check_call refuses write tools in read-only mode", "[mcp][policy]") {
    const auto d = check_call(fill_spec(), json{{"element", "x"}, {"value", "y"}}, read_only_policy(), no_env);
    REQUIRE_FALSE(d.allowed);
    REQUIRE(d.code == "TOOL_UNAVAILABLE_READ_ONLY");
    REQUIRE(d.message == "The fairyfly server runs read-only; ask the user to restart it with `mcp --allow-write`");
}

TEST_CASE("check_call refuses destructive launch/login/disconnect options in read-only mode", "[mcp][policy]") {
    for (const char* tool : {"gui_session_launch", "gui_session_login"}) {
        const auto spec = spec_named(tool, false);
        REQUIRE_FALSE(check_call(spec, json{{"multiple_logon", "end"}}, read_only_policy(), no_env).allowed);
        REQUIRE_FALSE(check_call(spec, json{{"multiple_logon", "END"}}, read_only_policy(), no_env).allowed);
        REQUIRE(check_call(spec, json{{"multiple_logon", "keep"}}, read_only_policy(), no_env).allowed);
        REQUIRE(check_call(spec, json{{"multiple_logon", "fail"}}, read_only_policy(), no_env).allowed);
        REQUIRE(check_call(spec, json::object(), read_only_policy(), no_env).allowed);
        const auto d = check_call(spec, json{{"multiple_logon", "end"}}, read_only_policy(), no_env);
        REQUIRE(d.code == "READ_ONLY_REFUSED");
        REQUIRE(check_call(spec, json{{"multiple_logon", "end"}}, write_policy(), no_env).allowed);
    }
    const auto disc = spec_named("gui_session_disconnect", false);
    REQUIRE_FALSE(check_call(disc, json{{"close_session", true}}, read_only_policy(), no_env).allowed);
    REQUIRE(check_call(disc, json{{"close_session", false}}, read_only_policy(), no_env).allowed);
    REQUIRE(check_call(disc, json::object(), read_only_policy(), no_env).allowed);
    REQUIRE(check_call(disc, json{{"close_session", true}}, write_policy(), no_env).allowed);
}

TEST_CASE("check_call allows everything in write mode and plain reads in read-only mode", "[mcp][policy]") {
    REQUIRE(check_call(fill_spec(), json{{"element", "x"}, {"value", "y"}}, write_policy(), no_env).allowed);
    REQUIRE(check_call(spec_named("gui_screen_read", false), json::object(), read_only_policy(), no_env).allowed);
    REQUIRE(check_call(spec_named("gui_transaction_start", false), json{{"transaction", "start", "SU01"}}, read_only_policy(), no_env).allowed);
    // Non-object arguments are not this layer's business.
    REQUIRE(check_call(spec_named("gui_session_launch", false), json("x"), read_only_policy(), no_env).allowed);
}

TEST_CASE("FAIRYFLY_READ_ONLY is a hard cap in check_call", "[mcp][policy]") {
    const EnvFn on = [](const char* n) { return std::string(n) == "FAIRYFLY_READ_ONLY" ? "1" : ""; };
    const EnvFn on_word = [](const char*) { return std::string("TRUE"); };
    const EnvFn off = [](const char*) { return std::string("0"); };
    const auto d = check_call(fill_spec(), json{{"element", "x"}, {"value", "y"}}, write_policy(), on);
    REQUIRE_FALSE(d.allowed);
    REQUIRE(d.code == "TOOL_UNAVAILABLE_READ_ONLY");
    REQUIRE_FALSE(check_call(fill_spec(), json::object(), write_policy(), on_word).allowed);
    REQUIRE_FALSE(check_call(spec_named("gui_session_disconnect", false), json{{"close_session", true}}, write_policy(), on).allowed);
    REQUIRE(check_call(fill_spec(), json::object(), write_policy(), off).allowed);
    REQUIRE(check_call(fill_spec(), json::object(), write_policy(), no_env).allowed);
}

TEST_CASE("RateLimiter sliding window", "[mcp][policy]") {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    RateLimiter limiter(3);
    REQUIRE(limiter.allow(t0));
    REQUIRE(limiter.allow(t0 + std::chrono::seconds(1)));
    REQUIRE(limiter.allow(t0 + std::chrono::seconds(2)));
    REQUIRE_FALSE(limiter.allow(t0 + std::chrono::seconds(3)));
    REQUIRE_FALSE(limiter.allow(t0 + std::chrono::seconds(59)));
    // Rejected calls are not recorded; the oldest (t0) leaves the window at exactly 60 s.
    REQUIRE(limiter.allow(t0 + std::chrono::seconds(60)));
    REQUIRE_FALSE(limiter.allow(t0 + std::chrono::seconds(60)));
    REQUIRE(limiter.allow(t0 + std::chrono::seconds(61)));
    REQUIRE_FALSE(limiter.allow(t0 + std::chrono::seconds(61)));
}

TEST_CASE("RateLimiter edge cases", "[mcp][policy]") {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    RateLimiter unlimited(0);
    for (int i = 0; i < 1000; ++i) REQUIRE(unlimited.allow(t0));
    RateLimiter negative(-5);
    REQUIRE(negative.allow(t0));
    RateLimiter one(1);
    REQUIRE(one.allow(t0));
    REQUIRE_FALSE(one.allow(t0 + std::chrono::milliseconds(59999)));
    REQUIRE(one.allow(t0 + std::chrono::seconds(60)));
}

TEST_CASE("RateLimiter is thread safe", "[mcp][policy]") {
    RateLimiter limiter(100);
    const auto now = std::chrono::steady_clock::now();
    std::atomic<int> allowed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 100; ++i)
                if (limiter.allow(now)) ++allowed;
        });
    for (auto& t : threads) t.join();
    REQUIRE(allowed == 100);
}

TEST_CASE("write catalog exposes gui_element_fill with correct annotations", "[mcp][policy][catalog]") {
    const auto fill = fill_spec();
    REQUIRE(fill.def.name == "gui_element_fill");
    REQUIRE(fill.write_tool);
    REQUIRE(fill.output == ToolOutput::Json);
    REQUIRE(fill.def.annotations["destructiveHint"] == true);
    REQUIRE(fill.def.annotations["readOnlyHint"] == false);
    REQUIRE(fill.def.annotations["idempotentHint"] == true);
    REQUIRE(fill.def.description.find("Confirm with the user") != std::string::npos);
    REQUIRE(fill.def.description.find("gui_session_login") != std::string::npos);
    REQUIRE(fill.def.input_schema["required"] == json::array({"element"}));
    for (auto it = fill.def.input_schema["properties"].begin(); it != fill.def.input_schema["properties"].end(); ++it) {
        const std::string key = it.key();
        REQUIRE(key.find("pass") == std::string::npos);
        REQUIRE(key.find("secret") == std::string::npos);
        REQUIRE(key.find("credential") == std::string::npos);
    }
    for (const auto& s : all_tool_specs())
        if (s.def.name == "gui_element_fill") REQUIRE(s.write_tool);
}

TEST_CASE("gui_element_fill builds exact argv", "[mcp][policy][catalog]") {
    using V = std::vector<std::string>;
    REQUIRE(fill_argv({{"element", "wnd[0]/usr/txtA"}, {"value", "abc"}}) == V{"element", "fill", "wnd[0]/usr/txtA", "abc"});
    REQUIRE(fill_argv({{"element", "e"}, {"value", ""}}) == V{"element", "fill", "e", ""});
    REQUIRE(fill_argv({{"element", "e"}, {"clear", true}}) == V{"element", "fill", "e", "--clear"});
    REQUIRE(fill_argv({{"element", "e"}, {"value", "v"}, {"clear", false}}) == V{"element", "fill", "e", "v"});
    REQUIRE(fill_argv({{"element", "e"}, {"value", "v"}, {"connection", 2}}) ==
            V{"element", "fill", "e", "v", "--connection", "2"});
    REQUIRE(fill_argv({{"element", "e"}, {"value", "v"}, {"row", 3}, {"column", "MATNR"}}) ==
            V{"element", "fill", "e", "v", "--row", "3", "--column", "MATNR"});
    REQUIRE(fill_argv({{"element", "e"}, {"value", "v"}, {"row", 0}, {"column", "C"}, {"commit", true}}) ==
            V{"element", "fill", "e", "v", "--row", "0", "--column", "C", "--commit"});
    REQUIRE(fill_argv({{"element", "e"}, {"value", "X"}, {"row", 1}, {"column", "SEL"}, {"checkbox", true},
                       {"commit", true}, {"connection", 0}}) ==
            V{"element", "fill", "e", "X", "--row", "1", "--column", "SEL", "--checkbox", "--commit", "--connection", "0"});
    REQUIRE(fill_argv({{"element", "e"}, {"clear", true}, {"row", 1}, {"column", "SEL"}, {"checkbox", true}}) ==
            V{"element", "fill", "e", "--clear", "--row", "1", "--column", "SEL", "--checkbox"});
}

TEST_CASE("gui_element_fill uses the policy default connection only when absent", "[mcp][policy][catalog]") {
    Policy p;
    p.default_connection = 4;
    using V = std::vector<std::string>;
    REQUIRE(fill_argv({{"element", "e"}, {"value", "v"}}, p) == V{"element", "fill", "e", "v", "--connection", "4"});
    REQUIRE(fill_argv({{"element", "e"}, {"value", "v"}, {"connection", 1}}, p) == V{"element", "fill", "e", "v", "--connection", "1"});
}

TEST_CASE("gui_element_fill protects values that look like options", "[mcp][policy][catalog]") {
    using V = std::vector<std::string>;
    REQUIRE(fill_argv({{"element", "e"}, {"value", "-5"}, {"connection", 0}}) ==
            V{"element", "fill", "--connection", "0", "--", "e", "-5"});
    REQUIRE(fill_argv({{"element", "e"}, {"value", "--clear"}}) == V{"element", "fill", "--", "e", "--clear"});
}

TEST_CASE("gui_element_fill rejects invalid combinations", "[mcp][policy][catalog]") {
    const std::vector<json> bad = {
        json::object(),
        json{{"value", "v"}},
        json{{"element", ""}, {"value", "v"}},
        json{{"element", 5}, {"value", "v"}},
        json{{"element", "e"}},
        json{{"element", "e"}, {"clear", false}},
        json{{"element", "e"}, {"value", "v"}, {"clear", true}},
        json{{"element", "e"}, {"value", 5}},
        json{{"element", "e"}, {"value", "v"}, {"clear", "yes"}},
        json{{"element", "e"}, {"value", "v"}, {"row", 1}},
        json{{"element", "e"}, {"value", "v"}, {"column", "C"}},
        json{{"element", "e"}, {"value", "v"}, {"checkbox", true}},
        json{{"element", "e"}, {"value", "v"}, {"commit", true}},
        json{{"element", "e"}, {"value", "v"}, {"row", -1}, {"column", "C"}},
        json{{"element", "e"}, {"value", "v"}, {"row", "1"}, {"column", "C"}},
        json{{"element", "e"}, {"value", "v"}, {"row", 1.5}, {"column", "C"}},
        json{{"element", "e"}, {"value", "v"}, {"row", 1}, {"column", ""}},
        json{{"element", "e"}, {"value", "maybe"}, {"row", 1}, {"column", "C"}, {"checkbox", true}},
        json{{"element", "e"}, {"value", "v"}, {"connection", -1}},
        json{{"element", "e"}, {"value", "v"}, {"connection", "0"}},
        json("not an object"),
    };
    for (const auto& args : bad) {
        INFO(args.dump());
        REQUIRE_THROWS_AS(fill_argv(args), std::invalid_argument);
    }
}

TEST_CASE("MCP audit hook writes redacted source=mcp records without content", "[mcp][audit]") {
    TempFile file;
    fairyfly::audit::AuditSink sink(fairyfly::audit::AuditConfig{fairyfly::audit::Mode::Enabled, file.path});
    auto hook = make_mcp_audit_hook(&sink, [] { return nullptr; }, false);
    hook(fill_call_record());

    McpCallRecord refused;
    refused.tool = "gui_element_fill";
    refused.status = "refused";
    refused.error_code = "TOOL_UNAVAILABLE_READ_ONLY";
    refused.read_only = true;
    hook(refused);

    const auto recs = file.records();
    REQUIRE(recs.size() == 2);
    const auto& r = recs[0];
    REQUIRE(r["audit_source"] == "mcp");
    REQUIRE(r["tool"] == "gui_element_fill");
    REQUIRE(r["cmd"] == "element fill");
    REQUIRE(r["argv"][3] == "<redacted>");
    REQUIRE(r["client"] == "test-client/1.0");
    REQUIRE(r["request_id"] == "17");
    REQUIRE(r["status"] == "success");
    REQUIRE(r["exit"] == 0);
    REQUIRE(r["duration_ms"] == 12);
    REQUIRE(r["read_only"] == false);
    REQUIRE(r["connection"] == 0);
    REQUIRE_FALSE(r.contains("message"));
    REQUIRE_FALSE(r.contains("result"));

    REQUIRE(recs[1]["status"] == "refused");
    REQUIRE(recs[1]["exit"] == 1);
    REQUIRE(recs[1]["error_code"] == "TOOL_UNAVAILABLE_READ_ONLY");
    REQUIRE(recs[1]["read_only"] == true);

    std::ifstream in(file.path, std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(all.find("hunter2-secret") == std::string::npos);
}

TEST_CASE("MCP audit hook caps oversized identifiers", "[mcp][audit]") {
    TempFile file;
    fairyfly::audit::AuditSink sink(fairyfly::audit::AuditConfig{fairyfly::audit::Mode::Enabled, file.path});
    auto hook = make_mcp_audit_hook(&sink, {}, true);
    auto rec = fill_call_record();
    rec.tool = std::string(4000, 't');
    rec.client = std::string(4000, 'c');
    rec.request_id = std::string(4000, 'r');
    rec.argv.assign(500, std::string(2000, 'a'));
    hook(rec);
    std::ifstream in(file.path, std::ios::binary);
    std::string line;
    std::getline(in, line);
    REQUIRE(line.size() + 1 <= 16 * 1024);
    const auto j = json::parse(line);
    REQUIRE(j["audit_source"] == "mcp");
}

TEST_CASE("MCP audit hook is a no-op for null or disabled sinks", "[mcp][audit]") {
    TempFile file;
    fairyfly::audit::AuditSink disabled(fairyfly::audit::AuditConfig{fairyfly::audit::Mode::Disabled, file.path});
    auto hook1 = make_mcp_audit_hook(&disabled, {}, true);
    auto hook2 = make_mcp_audit_hook(nullptr, {}, true);
    REQUIRE(hook1);
    REQUIRE(hook2);
    hook1(fill_call_record());
    hook2(fill_call_record());
    REQUIRE_FALSE(fs::exists(file.path));
    REQUIRE(append_serve_event(&disabled, "started", true));
    REQUIRE(append_serve_event(nullptr, "started", true));
    REQUIRE_FALSE(fs::exists(file.path));
}

TEST_CASE("serve start/stop records", "[mcp][audit]") {
    TempFile file;
    fairyfly::audit::AuditSink sink(fairyfly::audit::AuditConfig{fairyfly::audit::Mode::Enabled, file.path});
    REQUIRE(append_serve_event(&sink, "started", true));
    REQUIRE(append_serve_event(&sink, "stopped", true));
    const auto recs = file.records();
    REQUIRE(recs.size() == 2);
    for (const auto& r : recs) {
        REQUIRE(r["cmd"] == "mcp");
        REQUIRE(r["audit_source"] == "mcp");
        REQUIRE(r["read_only"] == true);
    }
    REQUIRE(recs[0]["status"] == "started");
    REQUIRE(recs[1]["status"] == "stopped");
}

TEST_CASE("MCP audit failures are reported through mcp_audit_failed", "[mcp][audit]") {
    mcp_audit_reset_failure();
    REQUIRE_FALSE(mcp_audit_failed());
    TempFile blocker;
    { std::ofstream(blocker.path) << "x"; }
    // The "directory" of the audit file is a regular file, so appends cannot succeed.
    fairyfly::audit::AuditSink sink(
        fairyfly::audit::AuditConfig{fairyfly::audit::Mode::Required, blocker.path / "audit.jsonl"});
    auto hook = make_mcp_audit_hook(&sink, {}, false);
    hook(fill_call_record());
    REQUIRE(mcp_audit_failed());
    mcp_audit_reset_failure();
    REQUIRE_FALSE(mcp_audit_failed());
}
