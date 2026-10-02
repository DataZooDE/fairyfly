#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <future>
#include <set>
#include <thread>
#include <string>
#include <vector>

#include "include/audit_log.h"
#include "include/auth/authorize.h"
#include "include/mcp/call_executor.h"
#include "include/mcp/dispatcher.h"
#include "include/mcp/mcp_audit.h"
#include "include/mcp/result_shaper.h"
#include "include/mcp/tool_catalog.h"
#include "include/session_facts.h"
#include "include/string_utils.h"
#include "include/vkey.h"

using namespace fairyfly::mcp;
using fairyfly::Result;
namespace auth = fairyfly::auth;
using Argv = std::vector<std::string>;

namespace {

Result ok_result(json data = json::object()) {
    Result r;
    r.status = Result::Status::Success;
    r.data = std::move(data);
    return r;
}

std::string first_text(const ToolResult& r) { return r.content.at(0).at("text").get<std::string>(); }

const ToolDef* find_def(const std::vector<ToolDef>& defs, const std::string& name) {
    for (const auto& d : defs)
        if (d.name == name) return &d;
    return nullptr;
}

} // namespace

// ---- item 1: policy-aware texts ------------------------------------------------------------------
TEST_CASE("describe_for drops references to hidden tools", "[mcp][usability]") {
    ToolSpec spec;
    spec.def.description = "ids for gui_element_click / gui_element_get{?gui_element_fill: / gui_element_fill}. Cheap.";
    CHECK(describe_for(spec, {"gui_element_get"}) == "ids for gui_element_click / gui_element_get. Cheap.");
    CHECK(describe_for(spec, {"gui_element_fill"}) ==
          "ids for gui_element_click / gui_element_get / gui_element_fill. Cheap.");
    spec.def.description = "no markers here";
    CHECK(describe_for(spec, {}) == "no markers here");
    spec.def.description = "broken {?tool without end";
    CHECK(describe_for(spec, {}) == "broken {?tool without end");
}

TEST_CASE("tools/list texts never mention a hidden tool", "[mcp][usability]") {
    Policy read_only;  // read-only by default: gui_element_fill is hidden
    CommandDispatcher ro([](const Argv&) { return ok_result(); }, read_only);
    for (const auto& def : ro.list_tools()) {
        INFO(def.name);
        CHECK(def.description.find("gui_element_fill") == std::string::npos);
        CHECK(def.description.find("{?") == std::string::npos);
    }
    Policy writable;
    writable.read_only = false;
    writable.allow_write = true;
    CommandDispatcher rw([](const Argv&) { return ok_result(); }, writable);
    const auto defs = rw.list_tools();
    REQUIRE(find_def(defs, "gui_screen_find"));
    REQUIRE(find_def(defs, "gui_element_fill"));
    INFO(find_def(defs, "gui_screen_find")->description);
    CHECK(find_def(defs, "gui_screen_find")->description.find("gui_element_fill") != std::string::npos);
    CHECK(find_def(defs, "gui_element_f4")->description.find("gui_element_fill") != std::string::npos);

    // a token that is read-only on a writable server also loses the references
    Principal token;
    token.name = "t";
    token.read_only = true;
    const auto for_token = rw.list_tools_for(token);
    REQUIRE(find_def(for_token, "gui_screen_find"));
    CHECK(find_def(for_token, "gui_screen_find")->description.find("gui_element_fill") == std::string::npos);
}

TEST_CASE("adapt_cli_hints rewrites click hints and drops hints of hidden tools", "[mcp][usability][shaper]") {
    const std::string grid =
        "**Usage Examples:**\n```python\n# Read cell value\nfairyfly element get 'wnd[0]/usr/cntl/shellcont/shell' --row 0 --column 'COLUMN_NAME'\n\n"
        "# Set cell value\nfairyfly element fill 'wnd[0]/usr/cntl/shell' 'new_value' --row 0 --column 'COLUMN_NAME'\n\n"
        "# Click cell to select row\nfairyfly element click 'wnd[0]/usr/cntl/shell' --row 0 --column 'COLUMN_NAME'\n```\n\nafter\n";
    const std::set<std::string> read_only = {"gui_element_click", "gui_element_get"};
    const std::string adapted = adapt_cli_hints(grid, read_only);
    CHECK(adapted.find("element fill") == std::string::npos);
    CHECK(adapted.find("Set cell value") == std::string::npos);
    CHECK(adapted.find("fairyfly element") == std::string::npos);
    CHECK(adapted.find("# Click cell to select row\ngui_element_click(element=\"wnd[0]/usr/cntl/shell\", row=0, column=\"COLUMN_NAME\")") !=
          std::string::npos);
    CHECK(adapted.find("after") != std::string::npos);

    const std::set<std::string> writable = {"gui_element_click", "gui_element_get", "gui_element_fill"};
    const std::string with_fill = adapt_cli_hints(grid, writable);
    CHECK(with_fill.find("gui_element_fill(element=\"wnd[0]/usr/cntl/shell\", value=\"new_value\", row=0, column=\"COLUMN_NAME\")") !=
          std::string::npos);

    // inline hints: rewritten when click is visible, whole line dropped when not
    const std::string inline_hint = "x\n_Click with_: `fairyfly element click '@active/<button_path>'`\ny\n";
    CHECK(adapt_cli_hints(inline_hint, read_only) ==
          "x\n_Click with_: `gui_element_click(element=\"@active/<button_path>\")`\ny\n");
    CHECK(adapt_cli_hints(inline_hint, {}) == "x\ny\n");

    // a block whose every line is dropped disappears with its header
    const std::string only_fill = "a\n\n**Usage Examples:**\n```bash\n# Set cell value\nfairyfly element fill 'i' 'v' --row 0 --column 'C'\n```\n\nb\n";
    const std::string gone = adapt_cli_hints(only_fill, read_only);
    CHECK(gone.find("Usage Examples") == std::string::npos);
    CHECK(gone.find("```") == std::string::npos);
    CHECK(gone.find("b") != std::string::npos);

    // text without hints is untouched
    CHECK(adapt_cli_hints("nothing to see", {}) == "nothing to see");
}

TEST_CASE("shape_result adapts hints for Markdown screen results", "[mcp][usability][shaper]") {
    ToolSpec spec;
    spec.def.name = "gui_screen_find";
    spec.output = ToolOutput::Markdown;
    Policy policy;
    json elements = json::array();
    elements.push_back({{"id", "wnd[0]/usr/btnX"}, {"type", "GuiButton"}, {"text", "X"}});
    const Result res = ok_result({{"scanned_count", 1}, {"elements", elements}});
    const std::set<std::string> visible = {"gui_element_click"};
    const auto shaped = shape_result(res, spec, policy, "", &visible);
    CHECK(first_text(shaped).find("element fill") == std::string::npos);
}

// ---- item 2: benign status messages reach the model as success ---------------------------------
TEST_CASE("A click result with a benign warning is shaped as success with the message", "[mcp][usability][status]") {
    Policy policy;
    CommandDispatcher dispatcher(
        [](const Argv&) {
            return ok_result({{"action", "click"},
                              {"status_message", {{"type", "W"}, {"text", "No short dumps match the selection criteria"}}},
                              {"warning", true}});
        },
        policy);
    CallContext ctx;
    ctx.request_id = 1;
    const auto result = dispatcher.call_tool("gui_element_click", {{"element", "wnd[0]/usr/btnTODAY"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(first_text(result).find("No short dumps match") != std::string::npos);
    REQUIRE(result.structured.has_value());
    CHECK((*result.structured)["data"]["status_message"]["type"] == "W");
    CHECK((*result.structured)["data"]["warning"] == true);
}

// ---- item 3: key names ---------------------------------------------------------------------------
TEST_CASE("parse_vkey accepts named page keys and tolerant spellings", "[mcp][usability][vkey]") {
    using fairyfly::sap::parse_vkey;
    struct Row { const char* text; int vkey; };
    const Row rows[] = {
        {"enter", 0}, {"Enter", 0}, {" ENTER ", 0}, {"return", 0},
        {"pageup", 81}, {"page_up", 81}, {"Page-Up", 81}, {"page up", 81}, {"PgUp", 81},
        {"pagedown", 82}, {"page_down", 82}, {"PAGE DOWN", 82}, {"pgdn", 82}, {"pgdown", 82},
        {"pagetop", 80}, {"page_top", 80}, {"ctrl+pageup", 80}, {"Ctrl+PgUp", 80}, {"firstpage", 80},
        {"pagebottom", 83}, {"page_bottom", 83}, {"ctrl+pagedown", 83}, {"lastpage", 83},
        {"f1", 1}, {"F8", 8}, {"f12", 12}, {"shift+f1", 13}, {"Shift+F12", 24}, {"shift_f4", 16}, {"shift-f4", 16},
        {"0", 0}, {"15", 15}, {"82", 82}, {"99", 99},
    };
    for (const auto& row : rows) {
        INFO(row.text);
        REQUIRE(parse_vkey(row.text).has_value());
        CHECK(*parse_vkey(row.text) == row.vkey);
    }
    for (const char* bad : {"", "tab", "up", "down", "left", "arrow_down", "f13", "f22", "f0", "100", "-1", "shift+enter",
                            "pagedownx", "ctrl+s", "banana", "_", "--"}) {
        INFO(bad);
        CHECK_FALSE(parse_vkey(bad).has_value());
    }
}

TEST_CASE("T-code allowlist key policy: spellings, messages, unknown names", "[mcp][usability][vkey][auth]") {
    Principal p;
    p.name = "t";
    p.all_scopes = true;
    p.authenticated = true;
    p.tcodes = {"SM21"};
    Policy policy;
    policy.read_only = false;
    const auto spec = [] {
        for (const auto& s : all_tool_specs())
            if (s.def.name == "gui_key_send") return s;
        return ToolSpec{};
    }();
    auto decide = [&](const std::string& key) {
        return auth::authorize_call(p, spec, spec.family, json{{"key", key}}, policy, std::nullopt, std::string("SM21"));
    };

    for (const char* key : {"pagedown", "page_down", "PageDown", "pgdn", "pageup", "page_up", "pagetop", "pagebottom",
                            "ctrl+pagedown", "enter", "f4", "F8", "82"})
        CHECK(decide(key).allowed);

    // a known but navigating key is a policy denial that lists the exact accepted spellings
    const auto f3 = decide("f3");
    CHECK_FALSE(f3.allowed);
    CHECK(f3.code == "TCODE_DENIED");
    for (const char* spelling : {"enter", "f4", "f8", "pagedown", "page_down", "pgdn", "pageup", "pagetop", "pagebottom"})
        CHECK(f3.message.find(spelling) != std::string::npos);

    // every spelling printed in the denial message really is accepted
    const std::string spellings = auth::tcode_safe_key_spellings();
    for (const char* name : {"enter", "f4", "f8", "pageup", "page_up", "pgup", "pagedown", "page_down", "pgdn", "pagetop",
                             "ctrl+pageup", "pagebottom", "ctrl+pagedown"}) {
        INFO(name);
        CHECK(spellings.find(name) != std::string::npos);
        CHECK(auth::tcode_safe_key(name));
    }

    // an unknown name is INVALID_ARGUMENT with the supported names, not TCODE_DENIED
    for (const char* key : {"f22", "tab", "arrow_down", "bogus"}) {
        INFO(key);
        const auto d = decide(key);
        CHECK_FALSE(d.allowed);
        CHECK(d.code == "INVALID_ARGUMENT");
        CHECK(d.message.find("pagedown") != std::string::npos);
        CHECK(d.message.find(key) != std::string::npos);
    }
}

TEST_CASE("gui_key_send builds the key argv and rejects unknown names", "[mcp][usability][vkey]") {
    for (const auto& spec : all_tool_specs()) {
        if (spec.def.name != "gui_key_send") continue;
        Policy policy;
        CHECK(spec.build_argv(json{{"key", "page_down"}}, policy) == Argv{"key", "send", "page_down"});
        try {
            spec.build_argv(json{{"key", "f22"}}, policy);
            FAIL("expected invalid_argument");
        } catch (const std::invalid_argument& e) {
            CHECK(std::string(e.what()).find("pagedown") != std::string::npos);
        }
        CHECK(spec.def.description.find("pagedown") != std::string::npos);
        CHECK(spec.def.description.find("arrow") != std::string::npos);
    }
}

// ---- item 4: escaped tree keys, element aliases ----------------------------------------------------
TEST_CASE("unescape_html_entities decodes the five entities in one pass", "[mcp][usability][tree]") {
    using fairyfly::utils::unescape_html_entities;
    CHECK(unescape_html_entities("PROG&lt;SYST&gt;") == "PROG<SYST>");
    CHECK(unescape_html_entities("A &amp; B") == "A & B");
    CHECK(unescape_html_entities("&quot;x&quot; &#39;y&#39; &apos;z&apos; &#x27;w&#x27;") == "\"x\" 'y' 'z' 'w'");
    CHECK(unescape_html_entities("&amp;lt;") == "&lt;");           // never decoded twice
    CHECK(unescape_html_entities("PROG<SYST>") == "PROG<SYST>");   // already literal
    CHECK(unescape_html_entities("R&D & more &unknown; &") == "R&D & more &unknown; &");
    CHECK(unescape_html_entities("") == "");
    CHECK(unescape_html_entities("&lt") == "&lt");                 // no terminating ';'
}

namespace {

const ToolSpec& catalog_spec(const std::string& name) {
    static const std::vector<ToolSpec> specs = all_tool_specs();
    for (const auto& s : specs)
        if (s.def.name == name) return s;
    FAIL("no tool " << name);
    return specs.front();
}

Argv build(const std::string& tool, const json& args) { return catalog_spec(tool).build_argv(args, Policy{}); }

bool contains(const Argv& argv, const std::string& item) {
    for (const auto& a : argv)
        if (a == item) return true;
    return false;
}

} // namespace

TEST_CASE("gui_element_click decodes HTML-escaped node keys and menu items", "[mcp][usability][tree]") {
    const Argv argv = build("gui_element_click", {{"element", "wnd[0]/usr/cntlTREE/shellcont/shell"},
                                                  {"node_key", "PROG&lt;SYST&gt;"},
                                                  {"tree_action", "select"},
                                                  {"menu_item", "A &amp; B"}});
    CHECK(contains(argv, "PROG<SYST>"));
    CHECK(contains(argv, "A & B"));
    CHECK_FALSE(contains(argv, "PROG&lt;SYST&gt;"));
    // a literal key is untouched
    CHECK(contains(build("gui_element_click", {{"element", "wnd[0]/usr/t"}, {"node_key", "PROG<SYST>"}}), "PROG<SYST>"));
    // menu paths
    const Argv menu = build("gui_menu_select", {{"path", "Edit/Find &amp; Replace"}});
    CHECK(contains(menu, "Edit/Find & Replace"));
}

TEST_CASE("gui_element_click/get/f4/fill accept id and element_id as aliases of element", "[mcp][usability][alias]") {
    struct Case { const char* tool; json extra; const char* verb; };
    const Case cases[] = {{"gui_element_click", json::object(), "click"},
                          {"gui_element_get", json::object(), "get"},
                          {"gui_element_f4", json::object(), "f4"},
                          {"gui_element_fill", {{"value", "x"}}, "fill"}};
    for (const auto& c : cases) {
        INFO(c.tool);
        for (const char* key : {"element", "element_id", "id"}) {
            json args = c.extra;
            args[key] = "wnd[0]/usr/txtA";
            const Argv argv = build(c.tool, args);
            CHECK(argv.at(0) == "element");
            CHECK(argv.at(1) == c.verb);
            CHECK(contains(argv, "wnd[0]/usr/txtA"));
        }
        // the same value twice is fine, different values are rejected, none is rejected
        json same = c.extra;
        same["element"] = "wnd[0]/usr/txtA";
        same["id"] = "wnd[0]/usr/txtA";
        CHECK_NOTHROW(build(c.tool, same));
        json differ = c.extra;
        differ["element"] = "wnd[0]/usr/txtA";
        differ["element_id"] = "wnd[0]/usr/txtB";
        CHECK_THROWS_AS(build(c.tool, differ), std::invalid_argument);
        CHECK_THROWS_AS(build(c.tool, c.extra), std::invalid_argument);
    }
}

TEST_CASE("the command field stays blocked for a fill through an element alias", "[mcp][usability][alias][auth]") {
    Principal p;
    p.name = "t";
    p.all_scopes = true;
    p.authenticated = true;
    p.tcodes = {"SM21"};
    Policy policy;
    policy.read_only = false;
    const ToolSpec& spec = catalog_spec("gui_element_fill");
    for (const char* key : {"element", "id", "element_id"}) {
        INFO(key);
        json args = {{"value", "/nSE16"}};
        args[key] = "wnd[0]/tbar[0]/okcd";
        const auto d = auth::authorize_call(p, spec, spec.family, args, policy, std::nullopt, std::string("SM21"));
        CHECK_FALSE(d.allowed);
        CHECK(d.code == "TCODE_DENIED");
    }
}

TEST_CASE("gui_element_click description explains select versus doubleclick", "[mcp][usability][tree]") {
    const auto& d = catalog_spec("gui_element_click").def.description;
    CHECK(d.find("select") != std::string::npos);
    CHECK(d.find("does NOT refresh") != std::string::npos);
    CHECK(d.find("doubleclick") != std::string::npos);
}

// ---- item 5: busy answers name the running tool ---------------------------------------------------
TEST_CASE("busy_call_result names the tool, elapsed seconds and retry_after_ms", "[mcp][usability][busy]") {
    CallInfo info;
    info.tool = "gui_screen_read";
    info.elapsed_ms = 121500;
    info.timeout_ms = 120000;
    const json timeout = busy_call_result("CALL_TIMEOUT", info);
    CHECK(timeout["isError"] == true);
    const std::string text = timeout["content"][0]["text"].get<std::string>();
    CHECK(text.rfind("CALL_TIMEOUT:", 0) == 0);
    CHECK(text.find("gui_screen_read") != std::string::npos);
    CHECK(text.find("121 s") != std::string::npos);
    CHECK(text.find("5000") != std::string::npos);
    const json& error = timeout["structuredContent"]["error"];
    CHECK(error["code"] == "CALL_TIMEOUT");
    CHECK(error["running_tool"] == "gui_screen_read");
    CHECK(error["elapsed_ms"] == 121500);
    CHECK(error["retry_after_ms"] == 5000);

    const json busy = busy_call_result("SERVER_BUSY", info);
    CHECK(busy["content"][0]["text"].get<std::string>().rfind("SERVER_BUSY:", 0) == 0);
    CHECK(busy["content"][0]["text"].get<std::string>().find("gui_screen_read") != std::string::npos);
    CHECK(busy["structuredContent"]["error"]["code"] == "SERVER_BUSY");

    // unknown tool still produces a usable message
    CHECK(busy_call_result("SERVER_BUSY", CallInfo{})["content"][0]["text"].get<std::string>().find("a SAP call") != std::string::npos);
}

TEST_CASE("retry_after_ms_hint is min(5000, remaining soft timeout), at least 500", "[mcp][usability][busy]") {
    CHECK(retry_after_ms_hint({"t", 0, 120000}) == 5000);
    CHECK(retry_after_ms_hint({"t", 118000, 120000}) == 2000);
    CHECK(retry_after_ms_hint({"t", 119900, 120000}) == 500);
    CHECK(retry_after_ms_hint({"t", 130000, 120000}) == 5000);  // already past the soft timeout
}

TEST_CASE("Executor answers CALL_TIMEOUT and SERVER_BUSY with the running tool", "[mcp][usability][busy]") {
    CallExecutor executor(4, 80);
    std::thread main_thread([&] { executor.run(); });

    std::promise<void> release;
    auto released = release.get_future().share();
    std::promise<json> answer;
    auto answered = answer.get_future();
    ExecJob job;
    job.id = 1;
    job.timed = true;
    job.tool = "gui_transaction_start";
    job.deliver = [&](const json& message) {
        try { answer.set_value(message); } catch (...) {}
    };
    job.timeout_response = [](const CallInfo& info) { return json{{"result", busy_call_result("CALL_TIMEOUT", info)}}; };
    job.run = [released](CallState&) {
        released.wait();
        return json();
    };
    REQUIRE(executor.submit(std::move(job)) == SubmitResult::Queued);

    REQUIRE(answered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const json timeout = answered.get();
    CHECK(timeout["result"]["structuredContent"]["error"]["running_tool"] == "gui_transaction_start");
    CHECK(timeout["result"]["structuredContent"]["error"]["elapsed_ms"].get<long long>() >= 80);
    CHECK(timeout["result"]["structuredContent"]["error"]["timeout_ms"] == 80);

    // a second timed call is refused while the first is still running; the running tool is known
    ExecJob second;
    second.id = 2;
    second.timed = true;
    second.run = [](CallState&) { return json(); };
    CHECK(executor.submit(std::move(second)) == SubmitResult::Busy);
    const auto info = executor.running_info();
    REQUIRE(info.has_value());
    CHECK(info->tool == "gui_transaction_start");
    CHECK(info->elapsed_ms >= 80);
    const json busy = busy_call_result("SERVER_BUSY", *info);
    CHECK(busy["structuredContent"]["error"]["running_tool"] == "gui_transaction_start");
    CHECK(busy["structuredContent"]["error"]["retry_after_ms"] == 5000);

    release.set_value();
    for (int i = 0; i < 200 && executor.running_info(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_FALSE(executor.running_info().has_value());
    executor.request_stop();
    main_thread.join();
}

// ---- item 6: step timing and the bounded facts lookup ---------------------------------------------
namespace {

struct FakeClock {
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    int sleeps = 0;
    fairyfly::sap::FactsClock clock() {
        fairyfly::sap::FactsClock c;
        c.now = [this] { return t; };
        c.sleep = [this](std::chrono::milliseconds d) { t += d; ++sleeps; };
        return c;
    }
};

} // namespace

TEST_CASE("read_facts_bounded: reads at once when the session is idle", "[mcp][usability][facts]") {
    FakeClock fake;
    bool timed_out = true;
    int reads = 0;
    const auto facts = fairyfly::sap::read_facts_bounded(
        [] { return false; },
        [&] { ++reads; return fairyfly::audit::SapFacts{"A4H", "001", "U", "STRUST"}; }, {}, fake.clock(), &timed_out);
    CHECK(facts.system == "A4H");
    CHECK(facts.transaction == "STRUST");
    CHECK(reads == 1);
    CHECK_FALSE(timed_out);
    CHECK(fake.sleeps == 0);
}

TEST_CASE("read_facts_bounded: waits for a busy session, then reads", "[mcp][usability][facts]") {
    FakeClock fake;
    int polls = 0;
    const auto facts = fairyfly::sap::read_facts_bounded(
        [&] { return ++polls <= 3; }, [] { return fairyfly::audit::SapFacts{"A4H", "001", "U", "SM21"}; }, {}, fake.clock());
    CHECK(facts.transaction == "SM21");
    CHECK(fake.sleeps == 3);
}

TEST_CASE("read_facts_bounded: a session that stays busy gives empty facts after the budget", "[mcp][usability][facts]") {
    FakeClock fake;
    bool timed_out = false;
    int reads = 0;
    const auto start = fake.t;
    const auto facts = fairyfly::sap::read_facts_bounded(
        [] { return true; }, [&] { ++reads; return fairyfly::audit::SapFacts{"A4H", "001", "U", "SM21"}; },
        fairyfly::sap::FactsBudget{std::chrono::milliseconds(5000), std::chrono::milliseconds(100)}, fake.clock(), &timed_out);
    CHECK_FALSE(facts.any());
    CHECK(timed_out);
    CHECK(reads == 0);  // the properties are never touched while the GUI is busy
    CHECK(fake.t - start >= std::chrono::milliseconds(5000));
    CHECK(fake.t - start < std::chrono::milliseconds(5200));
}

TEST_CASE("read_facts_bounded: throwing seams never escape", "[mcp][usability][facts]") {
    FakeClock fake;
    // an unreadable Busy property counts as not busy
    auto facts = fairyfly::sap::read_facts_bounded(
        []() -> bool { throw std::runtime_error("busy"); }, [] { return fairyfly::audit::SapFacts{"A4H", "001", "U", "X"}; }, {},
        fake.clock());
    CHECK(facts.transaction == "X");
    // a failing read means no facts
    facts = fairyfly::sap::read_facts_bounded([] { return false; },
                                              []() -> fairyfly::audit::SapFacts { throw std::runtime_error("com"); }, {}, fake.clock());
    CHECK_FALSE(facts.any());
}

TEST_CASE("a lookup that gives up makes a token with an allowlist fail closed", "[mcp][usability][facts]") {
    Principal p;
    p.name = "strust";
    p.all_scopes = true;
    p.authenticated = true;
    p.tcodes = {"STRUST"};
    Policy policy;
    policy.read_only = false;
    CommandDispatcher dispatcher([](const Argv&) { return ok_result(); }, policy);
    dispatcher.set_sap_facts_provider([](std::optional<int>) { return std::optional<fairyfly::audit::SapFacts>(); });  // gave up
    CallContext ctx;
    ctx.request_id = 1;
    ctx.principal = p;
    ctx.http = true;
    const auto result = dispatcher.call_tool("gui_screen_read", json::object(), ctx);
    CHECK(result.is_error);
    CHECK(first_text(result).find("TCODE_DENIED") != std::string::npos);
}

TEST_CASE("slow steps are reported and timed in the audit record", "[mcp][usability][timing]") {
    const auto file = std::filesystem::temp_directory_path() /
                      ("ff_usab_audit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    fairyfly::audit::AuditConfig config;
    config.mode = fairyfly::audit::Mode::Enabled;
    config.file = file;
    fairyfly::audit::AuditSink sink(config);

    Principal p;
    p.name = "usertest-strust";
    p.all_scopes = true;
    p.authenticated = true;
    p.tcodes = {"STRUST"};
    Policy policy;
    policy.read_only = false;

    struct Slow { std::string step, tool, principal; long long ms; };
    std::vector<Slow> reports;
    CommandDispatcher dispatcher(
        [](const Argv&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            return ok_result();
        },
        policy, make_mcp_audit_hook(&sink, nullptr, false));
    dispatcher.set_slow_step_threshold(std::chrono::milliseconds(30));
    dispatcher.set_slow_step_reporter([&](const std::string& step, const std::string& tool, const std::string& principal, long long ms) {
        reports.push_back({step, tool, principal, ms});
    });
    int lookups = 0;
    dispatcher.set_sap_facts_provider([&](std::optional<int>) {
        ++lookups;
        std::this_thread::sleep_for(std::chrono::milliseconds(lookups == 1 ? 50 : 40));
        return std::optional<fairyfly::audit::SapFacts>(fairyfly::audit::SapFacts{"A4H", "001", "U", "STRUST"});
    });
    CallContext ctx;
    ctx.request_id = 1;
    ctx.principal = p;
    ctx.http = true;
    CHECK_FALSE(dispatcher.call_tool("gui_transaction_start", {{"code", "STRUST"}}, ctx).is_error);

    REQUIRE(reports.size() == 3);
    CHECK(reports[0].step == "facts_pre");
    CHECK(reports[1].step == "invoke");
    CHECK(reports[2].step == "facts_post");
    for (const auto& r : reports) {
        CHECK(r.tool == "gui_transaction_start");
        CHECK(r.principal == "usertest-strust");
        CHECK(r.ms >= 30);
    }

    std::ifstream in(file);
    std::string line;
    REQUIRE(std::getline(in, line));
    in.close();
    std::filesystem::remove(file);
    const auto row = json::parse(line);
    CHECK(row["facts_pre_ms"].get<long long>() >= 40);
    CHECK(row["invoke_ms"].get<long long>() >= 55);
    CHECK(row["facts_post_ms"].get<long long>() >= 35);
    CHECK(line.find("ffy_") == std::string::npos);

    // formatted warning: step, tool, principal name, elapsed
    const std::string text = format_slow_step_message("facts_post", "gui_transaction_start", "usertest-strust", 2100);
    CHECK(text == "slow MCP step: step=facts_post tool=gui_transaction_start principal=usertest-strust elapsed_ms=2100");
}

TEST_CASE("steps under the threshold are not reported and zero timings are not audited", "[mcp][usability][timing]") {
    fairyfly::audit::AuditRecord rec;
    rec.source = "mcp";
    rec.tool = "gui_screen_read";
    const auto quiet = json::parse(fairyfly::audit::format_record(rec));
    CHECK_FALSE(quiet.contains("facts_pre_ms"));
    CHECK_FALSE(quiet.contains("invoke_ms"));
    CHECK_FALSE(quiet.contains("facts_post_ms"));
    rec.facts_pre_ms = 5;
    rec.invoke_ms = 6;
    rec.facts_post_ms = 7;
    const auto timed = json::parse(fairyfly::audit::format_record(rec));
    CHECK(timed["facts_pre_ms"] == 5);
    CHECK(timed["invoke_ms"] == 6);
    CHECK(timed["facts_post_ms"] == 7);

    Policy policy;
    std::vector<std::string> steps;
    CommandDispatcher dispatcher([](const Argv&) { return ok_result(); }, policy);
    dispatcher.set_slow_step_reporter([&](const std::string& step, const std::string&, const std::string&, long long) { steps.push_back(step); });
    CallContext ctx;
    ctx.request_id = 1;
    CHECK_FALSE(dispatcher.call_tool("gui_doctor", json::object(), ctx).is_error);
    CHECK(steps.empty());
}
