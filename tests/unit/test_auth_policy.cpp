#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <fstream>
#include <sstream>

#include <CLI/CLI.hpp>

#include "include/audit_log.h"
#include "include/auth/authorize.h"
#include "include/auth/token_store.h"
#include "include/command_table.h"
#include "include/commands/cli_app.h"
#include "include/commands/global_options.h"
#include "include/mcp/dispatcher.h"
#include "include/mcp/mcp_audit.h"
#include "include/mcp/policy.h"
#include "include/mcp/tool_catalog.h"

using namespace fairyfly;
using namespace fairyfly::mcp;
using fairyfly::auth::authorize_call;
using fairyfly::auth::glob_match;
using fairyfly::auth::normalize_tcode;
using Argv = std::vector<std::string>;

namespace {

Result ok_result() {
    Result r;
    r.status = Result::Status::Success;
    r.data = json::object();
    return r;
}

const ToolSpec& spec_of(const std::string& tool) {
    static const std::vector<ToolSpec> catalog = all_tool_specs();
    for (const auto& s : catalog)
        if (s.def.name == tool) return s;
    FAIL("unknown tool " + tool);
    return catalog.front();
}

Principal token(const std::string& name, std::set<std::string> scopes) {
    Principal p;
    p.name = name;
    p.all_scopes = false;
    p.scopes = std::move(scopes);
    p.authenticated = true;
    p.read_only = false;
    return p;
}

PolicyDecision decide(const Principal& p, const std::string& tool, const json& args = json::object(), bool server_ro = false,
                      std::optional<std::string> system = std::nullopt, std::optional<std::string> tcode = std::nullopt) {
    Policy policy;
    policy.read_only = server_ro;
    const ToolSpec& spec = spec_of(tool);
    return authorize_call(p, spec, spec.family, args, policy, system, tcode);
}

/// Tool -> args that pass schema validation, for the scope matrix.
json sample_args(const std::string& tool) {
    if (tool == "gui_element_get" || tool == "gui_element_click" || tool == "gui_element_f4") return {{"element", "wnd[0]/usr/txtA"}};
    if (tool == "gui_element_fill") return {{"element", "wnd[0]/usr/txtA"}, {"value", "1"}};
    if (tool == "gui_transaction_start") return {{"code", "SE16"}};
    if (tool == "gui_key_send") return {{"key", "enter"}};
    return json::object();
}

} // namespace

TEST_CASE("authorize: glob and T-code normalisation", "[auth][authz]") {
    CHECK(glob_match("SE16", "se16"));
    CHECK(glob_match("SM*", "sm50"));
    CHECK(glob_match("S?16", "SE16"));
    CHECK_FALSE(glob_match("SE16", "SE16N"));
    CHECK(glob_match("*", "anything"));
    CHECK(glob_match("A4H/*", "a4h/001"));
    CHECK_FALSE(glob_match("A4H/001", "A4H/002"));
    CHECK(glob_match("", ""));
    CHECK_FALSE(glob_match("", "x"));

    CHECK(normalize_tcode("SE16") == "SE16");
    CHECK(normalize_tcode("  se16 ") == "SE16");
    CHECK(normalize_tcode("/nSE16") == "SE16");
    CHECK(normalize_tcode("/NSE16") == "SE16");
    CHECK(normalize_tcode("/oSE16") == "SE16");
    CHECK(normalize_tcode("/n SE16") == "SE16");
    CHECK(normalize_tcode("/*SE16") == "SE16");
    CHECK(normalize_tcode("SE16 param=1") == "SE16");
    CHECK(normalize_tcode("/n") == "");
    CHECK(normalize_tcode("/h") == "/H");   // other slash commands never look like a plain T-code
    CHECK(normalize_tcode("/nend") == "END");
    CHECK(auth::is_okcd_element("wnd[0]/tbar[0]/okcd"));
    CHECK(auth::is_okcd_element("WND[1]/TBAR[0]/OKCD"));
    CHECK_FALSE(auth::is_okcd_element("wnd[0]/usr/txtOKCD_FIELD"));
}

TEST_CASE("authorize: stdio default principal allows everything", "[auth][authz]") {
    const Principal stdio_principal;  // name "stdio", all scopes, no restrictions
    CHECK(stdio_principal.name == "stdio");
    CHECK(stdio_principal.all_scopes);
    for (const auto& spec : all_tool_specs()) {
        INFO(spec.def.name);
        Policy rw;
        rw.read_only = false;
        CHECK(authorize_call(stdio_principal, spec, spec.family, sample_args(spec.def.name), rw, std::nullopt, std::nullopt).allowed);
    }
}

TEST_CASE("authorize: scope matrix per family", "[auth][authz]") {
    const auto families = command_table::families();
    REQUIRE_FALSE(families.empty());
    for (const auto& granted : families) {
        const Principal p = token("scoped", {granted});
        for (const auto& spec : all_tool_specs()) {
            INFO(granted << " -> " << spec.def.name);
            if (spec.def.name == "gui_batch") continue;  // batch is checked separately below
            const auto d = decide(p, spec.def.name, sample_args(spec.def.name));
            if (spec.family == granted) {
                CHECK(d.allowed);
            } else {
                CHECK_FALSE(d.allowed);
                CHECK(d.code == "SCOPE_DENIED");
            }
        }
    }
    // wildcard
    Principal star;
    star.name = "star";
    star.all_scopes = true;
    for (const auto& spec : all_tool_specs()) CHECK(decide(star, spec.def.name, sample_args(spec.def.name)).allowed);
    // no scopes at all: everything is denied
    Principal none = token("none", {});
    for (const auto& spec : all_tool_specs()) CHECK(decide(none, spec.def.name, sample_args(spec.def.name)).code == "SCOPE_DENIED");
}

TEST_CASE("authorize: read-only narrowing (effective = server || token)", "[auth][authz]") {
    Principal p = token("ro", {"element", "session", "screen"});
    p.read_only = true;
    // write tool
    auto d = decide(p, "gui_element_fill", sample_args("gui_element_fill"));
    CHECK_FALSE(d.allowed);
    CHECK(d.code == "READ_ONLY");
    // args-dependent refusals of the server read-only rule set
    d = decide(p, "gui_session_disconnect", {{"close_session", true}});
    CHECK(d.code == "READ_ONLY");
    d = decide(p, "gui_session_launch", {{"connection", "X"}, {"multiple_logon", "end"}});
    CHECK(d.code == "READ_ONLY");
    // reads pass
    CHECK(decide(p, "gui_screen_read").allowed);
    CHECK(decide(p, "gui_element_get", sample_args("gui_element_get")).allowed);

    // a token cannot widen the server mode
    Principal writer = token("rw", {"element"});
    writer.read_only = false;
    CHECK(decide(writer, "gui_element_fill", sample_args("gui_element_fill"), /*server_ro=*/false).allowed);
    const auto server_ro = decide(writer, "gui_element_fill", sample_args("gui_element_fill"), /*server_ro=*/true);
    CHECK_FALSE(server_ro.allowed);
    CHECK(server_ro.code == "READ_ONLY");
    // tool listing follows the same rule
    CHECK_FALSE(auth::tool_allowed_for(p, spec_of("gui_element_fill")));
    CHECK(auth::tool_allowed_for(writer, spec_of("gui_element_fill")));
    CHECK_FALSE(auth::tool_allowed_for(writer, spec_of("gui_screen_read")));  // scope
}

TEST_CASE("authorize: SAP system/client allowlist", "[auth][authz]") {
    Principal p = token("sys", {"screen", "element", "session", "transaction"});
    p.sap_systems = {"A4H/001", "QAS/*"};

    CHECK(decide(p, "gui_screen_read", {}, false, "A4H/001").allowed);
    CHECK(decide(p, "gui_screen_read", {}, false, "a4h/001").allowed);  // case-insensitive
    CHECK(decide(p, "gui_screen_read", {}, false, "QAS/300").allowed);  // glob on the client
    auto d = decide(p, "gui_screen_read", {}, false, "A4H/002");
    CHECK_FALSE(d.allowed);
    CHECK(d.code == "SYSTEM_DENIED");
    d = decide(p, "gui_screen_read", {}, false, "PRD/001");
    CHECK(d.code == "SYSTEM_DENIED");
    d = decide(p, "gui_screen_read", {}, false, std::nullopt);
    CHECK(d.code == "SYSTEM_UNKNOWN");
    d = decide(p, "gui_element_get", sample_args("gui_element_get"), false, "");
    CHECK(d.code == "SYSTEM_UNKNOWN");
    // session tools run before a system is attached
    CHECK(decide(p, "gui_session_list").allowed);
    CHECK(decide(p, "gui_session_attach", {{"session_id", "x"}}).allowed);

    // SID-only pattern = any client
    Principal sid = token("sid", {"screen"});
    sid.sap_systems = {"A4H"};
    CHECK(decide(sid, "gui_screen_read", {}, false, "A4H/999").allowed);
    CHECK(decide(sid, "gui_screen_read", {}, false, "A4X/999").code == "SYSTEM_DENIED");

    // no allowlist: unknown system is fine
    Principal free = token("free", {"screen"});
    CHECK(decide(free, "gui_screen_read", {}, false, std::nullopt).allowed);
}

TEST_CASE("authorize: T-code allowlist", "[auth][authz]") {
    Principal p = token("tc", {"transaction", "element", "key", "menu"});
    p.tcodes = {"SE16", "SM*", "/SAPAPO/MC62"};

    for (const char* code : {"SE16", "se16", "/nSE16", "/oSE16", "/NSE16", " SE16 ", "SM50", "/nsm37", "/n"})
        CHECK(decide(p, "gui_transaction_start", {{"code", code}}).allowed);
    for (const char* code : {"SE38", "/nSE38", "/oSE38", "SE16N", "/h", "/nend", "/i", "PFCG"}) {
        INFO(code);
        const auto d = decide(p, "gui_transaction_start", {{"code", code}});
        CHECK_FALSE(d.allowed);
        CHECK(d.code == "TCODE_DENIED");
    }
    CHECK(decide(p, "gui_transaction_start", {{"code", "/SAPAPO/MC62"}}).allowed);

    // typing into the command field is blocked while an allowlist is set
    auto d = decide(p, "gui_element_fill", {{"element", "wnd[0]/tbar[0]/okcd"}, {"value", "/nSE38"}});
    CHECK_FALSE(d.allowed);
    CHECK(d.code == "TCODE_DENIED");
    d = decide(p, "gui_element_fill", {{"element", "wnd[1]/tbar[0]/OKCD"}, {"value", "x"}});
    CHECK(d.code == "TCODE_DENIED");
    // ordinary fields are fine
    CHECK(decide(p, "gui_element_fill", {{"element", "wnd[0]/usr/txtRSYST-BNAME"}, {"value", "x"}}, false, std::nullopt, "SE16").allowed);
    // ... but only inside an allowlisted (known) transaction
    CHECK(decide(p, "gui_element_fill", {{"element", "wnd[0]/usr/txtRSYST-BNAME"}, {"value", "x"}}, false, std::nullopt, "SE38").code == "TCODE_DENIED");
    CHECK(decide(p, "gui_element_fill", {{"element", "wnd[0]/usr/txtRSYST-BNAME"}, {"value", "x"}}).code == "TCODE_DENIED");

    // key send and menus act on the open transaction: allowed when it is allowlisted
    CHECK(decide(p, "gui_key_send", {{"key", "enter"}}, false, std::nullopt, "SE16").allowed);
    CHECK(decide(p, "gui_menu_select", {{"path", "System > Services"}}, false, std::nullopt, "SM50").allowed);

    // without an allowlist the command field is fillable
    Principal open = token("open", {"element"});
    CHECK(decide(open, "gui_element_fill", {{"element", "wnd[0]/tbar[0]/okcd"}, {"value", "/nSE38"}}).allowed);
}

TEST_CASE("authorize: gui_batch checks every item", "[auth][authz][batch]") {
    Principal p = token("batcher", {"batch", "transaction", "screen"});
    p.tcodes = {"SE16"};
    auto batch = [&](json items) { return decide(p, "gui_batch", {{"items", items}}); };

    CHECK(batch(json::array({{{"tool", "gui_transaction_start"}, {"arguments", {{"code", "SE16"}}}},
                             {{"tool", "gui_screen_read"}}})).allowed);

    auto d = batch(json::array({{{"tool", "gui_screen_read"}},
                                {{"tool", "gui_transaction_start"}, {"arguments", {{"code", "/nSE38"}}}}}));
    CHECK_FALSE(d.allowed);
    CHECK(d.code == "TCODE_DENIED");
    CHECK(d.message.find("batch item 2") != std::string::npos);

    d = batch(json::array({{{"tool", "gui_element_get"}, {"arguments", {{"element", "x"}}}}}));  // scope element missing
    CHECK(d.code == "SCOPE_DENIED");

    // the batch itself needs the "batch" scope
    Principal no_batch = token("nb", {"screen"});
    CHECK(decide(no_batch, "gui_batch", {{"items", json::array({{{"tool", "gui_screen_read"}}})}}).code == "SCOPE_DENIED");

    // read-only token: a write item refuses the whole batch
    Principal ro = token("ro", {"batch", "element"});
    ro.read_only = true;
    d = decide(ro, "gui_batch", {{"items", json::array({{{"tool", "gui_element_fill"}, {"arguments", {{"element", "a"}, {"value", "b"}}}}})}});
    CHECK(d.code == "READ_ONLY");
}

// ---- dispatcher integration ------------------------------------------------------------------
namespace {

struct Fixture {
    std::vector<Argv> calls;
    std::vector<McpCallRecord> records;
    std::vector<bool> read_only_events;
    std::optional<audit::SapFacts> facts;
    std::function<std::optional<audit::SapFacts>(std::optional<int>)> facts_for;  // per-connection facts (wins over `facts`)
    std::function<Result(const Argv&)> handler = [](const Argv&) { return ok_result(); };

    std::unique_ptr<CommandDispatcher> make(Policy policy = Policy{}) {
        auto d = std::make_unique<CommandDispatcher>([this](const Argv& argv) { calls.push_back(argv); return handler(argv); },
                                                     policy, [this](const McpCallRecord& r) { records.push_back(r); });
        d->set_sap_facts_provider([this](std::optional<int> c) { return facts_for ? facts_for(c) : facts; });
        d->set_read_only_override([this](bool ro) { read_only_events.push_back(ro); });
        return d;
    }
};

CallContext ctx_for(const Principal& p, bool http = true) {
    CallContext ctx;
    ctx.request_id = 1;
    ctx.principal = p;
    ctx.http = http;
    ctx.era = ProtocolEra::Stateless;
    return ctx;
}

std::string text_of(const ToolResult& r) { return r.content.at(0).at("text").get<std::string>(); }

Policy write_mode() {
    Policy p;
    p.read_only = false;
    p.allow_write = true;
    return p;
}

} // namespace

TEST_CASE("dispatcher: scope denial is a tool error and never reaches the CLI", "[auth][dispatch]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("screen-only", {"screen"});
    auto r = d.call_tool("gui_element_click", {{"element", "wnd[0]/usr/btnX"}}, ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("SCOPE_DENIED") != std::string::npos);
    CHECK(f.calls.empty());
    REQUIRE(f.records.size() == 1);
    CHECK(f.records[0].status == "error");
    CHECK(f.records[0].error_code == "SCOPE_DENIED");
    CHECK(f.records[0].principal == "screen-only");

    r = d.call_tool("gui_screen_read", json::object(), ctx_for(p));
    CHECK_FALSE(r.is_error);
    CHECK(f.calls.size() == 1);
}

TEST_CASE("dispatcher: read-only token narrows a write-mode server per call", "[auth][dispatch]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal ro = token("ro", {"element", "screen"});
    ro.read_only = true;
    Principal rw = token("rw", {"element", "screen"});

    auto r = d.call_tool("gui_element_fill", {{"element", "wnd[0]/usr/txtA"}, {"value", "x"}}, ctx_for(ro));
    CHECK(r.is_error);
    CHECK(text_of(r).find("READ_ONLY") != std::string::npos);
    CHECK(f.calls.empty());
    CHECK(f.read_only_events.empty());

    // an allowed read by the read-only token runs with the handler switched to read-only, then restored
    r = d.call_tool("gui_screen_read", json::object(), ctx_for(ro));
    CHECK_FALSE(r.is_error);
    REQUIRE(f.read_only_events.size() == 2);
    CHECK(f.read_only_events[0] == true);
    CHECK(f.read_only_events[1] == false);
    CHECK(f.records.back().read_only);

    // the writable token neither switches the handler nor is marked read-only
    f.read_only_events.clear();
    r = d.call_tool("gui_element_fill", {{"element", "wnd[0]/usr/txtA"}, {"value", "x"}}, ctx_for(rw));
    CHECK_FALSE(r.is_error);
    CHECK(f.read_only_events.empty());
    CHECK_FALSE(f.records.back().read_only);

    // read-only server: a writable token is still refused (server mode is the ceiling), with the server's code
    Fixture g;
    Policy server_ro;
    server_ro.read_only = true;
    auto ds_ptr = g.make(server_ro);
    auto& ds = *ds_ptr;
    r = ds.call_tool("gui_element_fill", {{"element", "wnd[0]/usr/txtA"}, {"value", "x"}}, ctx_for(rw));
    CHECK(r.is_error);
    CHECK(text_of(r).find("TOOL_UNAVAILABLE_READ_ONLY") != std::string::npos);
    CHECK(g.calls.empty());
    CHECK(g.read_only_events.empty());
}

TEST_CASE("dispatcher: SAP system allowlist uses the facts provider", "[auth][dispatch]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("sys", {"screen", "session"});
    p.sap_systems = {"A4H/001"};

    auto r = d.call_tool("gui_screen_read", json::object(), ctx_for(p));
    CHECK(text_of(r).find("SYSTEM_UNKNOWN") != std::string::npos);  // no session facts yet

    f.facts = audit::SapFacts{"A4H", "002", "USER", "SE16"};
    r = d.call_tool("gui_screen_read", json::object(), ctx_for(p));
    CHECK(text_of(r).find("SYSTEM_DENIED") != std::string::npos);

    f.facts = audit::SapFacts{"A4H", "001", "USER", "SE16"};
    r = d.call_tool("gui_screen_read", json::object(), ctx_for(p));
    CHECK_FALSE(r.is_error);
    CHECK(f.calls.size() == 1);

    // the local stdio principal is never asked for facts
    Fixture g;
    auto dg_ptr = g.make(write_mode());
    auto& dg = *dg_ptr;
    bool asked = false;
    dg.set_sap_facts_provider([&](std::optional<int>) { asked = true; return std::optional<audit::SapFacts>(); });
    CHECK_FALSE(dg.call_tool("gui_screen_read", json::object(), ctx_for(Principal{}, false)).is_error);
    CHECK_FALSE(asked);
}

TEST_CASE("dispatcher: T-code allowlist blocks transaction and okcd fill", "[auth][dispatch]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("tc", {"transaction", "element"});
    p.tcodes = {"SE16"};
    CHECK_FALSE(d.call_tool("gui_transaction_start", {{"code", "/nSE16"}}, ctx_for(p)).is_error);
    auto r = d.call_tool("gui_transaction_start", {{"code", "/nSE38"}}, ctx_for(p));
    CHECK(text_of(r).find("TCODE_DENIED") != std::string::npos);
    r = d.call_tool("gui_element_fill", {{"element", "wnd[0]/tbar[0]/okcd"}, {"value", "/nSE38"}}, ctx_for(p));
    CHECK(text_of(r).find("TCODE_DENIED") != std::string::npos);
    CHECK(f.calls.size() == 1);
}

TEST_CASE("dispatcher: batch items are authorized one by one", "[auth][dispatch][batch]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("b", {"batch", "transaction", "screen"});
    p.tcodes = {"SE16"};

    // whole batch refused up front when an item violates the T-code rule: nothing runs, one audit record
    auto r = d.call_tool("gui_batch",
                         {{"items", json::array({{{"tool", "gui_screen_read"}},
                                                 {{"tool", "gui_transaction_start"}, {"arguments", {{"code", "/nPFCG"}}}}})}},
                         ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("TCODE_DENIED") != std::string::npos);
    CHECK(f.calls.empty());
    CHECK(f.records.size() == 1);
    CHECK(f.records[0].tool == "gui_batch");
    CHECK(f.records[0].principal == "b");

    // allowed batch: each item audited with the principal
    f.facts = audit::SapFacts{"A4H", "001", "U", "SE16"};
    r = d.call_tool("gui_batch",
                    {{"items", json::array({{{"tool", "gui_transaction_start"}, {"arguments", {{"code", "SE16"}}}},
                                            {{"tool", "gui_screen_read"}}})}},
                    ctx_for(p));
    CHECK_FALSE(r.is_error);
    CHECK(f.calls.size() == 2);
    REQUIRE(f.records.size() == 3);
    CHECK(f.records[1].principal == "b");
    CHECK(f.records[2].principal == "b");

    // system rule is applied per item at execution time
    Fixture g;
    auto dg_ptr = g.make(write_mode());
    auto& dg = *dg_ptr;
    Principal sys = token("s", {"batch", "screen"});
    sys.sap_systems = {"A4H/001"};
    g.facts = audit::SapFacts{"PRD", "100", "U", ""};
    r = dg.call_tool("gui_batch", {{"items", json::array({{{"tool", "gui_screen_read"}}})}}, ctx_for(sys));
    CHECK(r.is_error);
    CHECK(text_of(r).find("SYSTEM_DENIED") != std::string::npos);
    CHECK(g.calls.empty());
}

TEST_CASE("dispatcher: system allowlist checks the connection the call targets", "[auth][dispatch]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    std::vector<std::optional<int>> asked;
    // connection 1 = allowed system A4H/001 (the "active" session), connection 2 = PRD/100, others unknown
    f.facts_for = [&](std::optional<int> c) -> std::optional<audit::SapFacts> {
        asked.push_back(c);
        if (c && *c == 1) return audit::SapFacts{"A4H", "001", "U", "SE16"};
        if (c && *c == 2) return audit::SapFacts{"PRD", "100", "U", "SE16"};
        return std::nullopt;
    };
    Principal p = token("sys", {"screen", "batch"});
    p.sap_systems = {"A4H/001"};

    CHECK_FALSE(d.call_tool("gui_screen_read", {{"connection", 1}}, ctx_for(p)).is_error);
    auto r = d.call_tool("gui_screen_read", {{"connection", 2}}, ctx_for(p));
    CHECK(text_of(r).find("SYSTEM_DENIED") != std::string::npos);
    CHECK(f.calls.size() == 1);  // the disallowed call never reached SAP
    r = d.call_tool("gui_screen_read", {{"connection", 3}}, ctx_for(p));  // facts cannot be established: fail closed
    CHECK(text_of(r).find("SYSTEM_UNKNOWN") != std::string::npos);
    CHECK(f.calls.size() == 1);
    REQUIRE(asked.size() == 3);
    CHECK(*asked[0] == 1);
    CHECK(*asked[1] == 2);
    CHECK(*asked[2] == 3);

    // policy default connection is what the facts are resolved for when the argument is omitted
    Policy with_default = write_mode();
    with_default.default_connection = 2;
    Fixture g;
    auto dg_ptr = g.make(with_default);
    g.facts_for = f.facts_for;
    r = dg_ptr->call_tool("gui_screen_read", json::object(), ctx_for(p));
    CHECK(text_of(r).find("SYSTEM_DENIED") != std::string::npos);

    // batch: every item is checked against ITS connection; stop_on_error false shows both verdicts
    f.calls.clear();
    r = d.call_tool("gui_batch",
                    {{"stop_on_error", false},
                     {"items", json::array({{{"tool", "gui_screen_read"}, {"arguments", {{"connection", 1}}}},
                                            {{"tool", "gui_screen_read"}, {"arguments", {{"connection", 2}}}}})}},
                    ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("SYSTEM_DENIED") != std::string::npos);
    CHECK(f.calls.size() == 1);  // only the connection-1 item ran
}

TEST_CASE("dispatcher: T-code allowlist follows the open transaction", "[auth][dispatch]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("va03", {"screen", "element", "key", "popup", "menu", "session", "batch"});
    p.tcodes = {"VA03"};

    // allowed transaction open: every screen-acting family passes
    f.facts = audit::SapFacts{"A4H", "001", "U", "VA03"};
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(p)).is_error);
    CHECK_FALSE(d.call_tool("gui_key_send", {{"key", "enter"}}, ctx_for(p)).is_error);
    CHECK_FALSE(d.call_tool("gui_popup_close", json::object(), ctx_for(p)).is_error);
    CHECK_FALSE(d.call_tool("gui_menu_list", json::object(), ctx_for(p)).is_error);
    CHECK_FALSE(d.call_tool("gui_element_get", {{"element", "wnd[0]/usr/txtA"}}, ctx_for(p)).is_error);
    const auto ran = f.calls.size();

    // another transaction open: denied and nothing reaches SAP
    f.facts = audit::SapFacts{"A4H", "001", "U", "SE38"};
    for (const char* tool : {"gui_screen_read", "gui_key_send", "gui_popup_close", "gui_menu_list"}) {
        INFO(tool);
        const json args = std::string(tool) == "gui_key_send" ? json{{"key", "enter"}} : json::object();
        const auto r = d.call_tool(tool, args, ctx_for(p));
        CHECK(r.is_error);
        CHECK(text_of(r).find("TCODE_DENIED") != std::string::npos);
    }
    CHECK(f.calls.size() == ran);

    // easy access screens are not allowlisted; an unknown transaction fails closed
    for (const char* open : {"S000", "SESSION_MANAGER", ""}) {
        f.facts = audit::SapFacts{"A4H", "001", "U", open};
        INFO(open);
        CHECK(text_of(d.call_tool("gui_screen_read", json::object(), ctx_for(p))).find("TCODE_DENIED") != std::string::npos);
    }
    f.facts = std::nullopt;
    CHECK(text_of(d.call_tool("gui_screen_read", json::object(), ctx_for(p))).find("TCODE_DENIED") != std::string::npos);

    // session tools keep their rules; gui_transaction_start is judged by its code, not the open transaction
    f.facts = audit::SapFacts{"A4H", "001", "U", "SE38"};
    CHECK_FALSE(d.call_tool("gui_session_list", json::object(), ctx_for(p)).is_error);
    Principal starter = token("st", {"transaction"});
    starter.tcodes = {"VA03"};
    CHECK_FALSE(d.call_tool("gui_transaction_start", {{"code", "VA03"}}, ctx_for(starter)).is_error);

    // no allowlist: unchanged
    Principal open_token = token("free", {"screen"});
    f.facts = std::nullopt;
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(open_token)).is_error);

    // batch items see the transaction that is open when THEY run
    f.calls.clear();
    f.facts = audit::SapFacts{"A4H", "001", "U", "VA03"};
    auto r = d.call_tool("gui_batch",
                         {{"items", json::array({{{"tool", "gui_screen_read"}},
                                                 {{"tool", "gui_key_send"}, {"arguments", {{"key", "enter"}}}}})}},
                         ctx_for(p));
    CHECK_FALSE(r.is_error);
    CHECK(f.calls.size() == 2);
    f.facts = audit::SapFacts{"A4H", "001", "U", "SE38"};
    f.calls.clear();
    r = d.call_tool("gui_batch", {{"items", json::array({{{"tool", "gui_screen_read"}}})}}, ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("TCODE_DENIED") != std::string::npos);
    CHECK(f.calls.empty());
}

TEST_CASE("dispatcher: per-principal rate limiting", "[auth][dispatch]") {
    Fixture f;
    Policy policy = write_mode();
    policy.max_calls_per_minute = 100;
    auto d_ptr = f.make(policy);
    auto& d = *d_ptr;
    Principal slow = token("slow", {"screen"});
    slow.rate_per_minute = 2;
    Principal other = token("other", {"screen"});
    other.rate_per_minute = 2;

    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(slow)).is_error);
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(slow)).is_error);
    auto r = d.call_tool("gui_screen_read", json::object(), ctx_for(slow));
    CHECK(r.is_error);
    CHECK(text_of(r).find("RATE_LIMITED") != std::string::npos);
    // another principal has its own budget
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(other)).is_error);

    // rate 0 = the server default, per principal
    Fixture g;
    Policy tight = write_mode();
    tight.max_calls_per_minute = 1;
    auto dg_ptr = g.make(tight);
    auto& dg = *dg_ptr;
    Principal a = token("a", {"screen"});
    Principal b = token("b", {"screen"});
    CHECK_FALSE(dg.call_tool("gui_screen_read", json::object(), ctx_for(a)).is_error);
    CHECK(dg.call_tool("gui_screen_read", json::object(), ctx_for(a)).is_error);
    CHECK_FALSE(dg.call_tool("gui_screen_read", json::object(), ctx_for(b)).is_error);
}

TEST_CASE("rate limiter: keyed windows are independent and expire", "[auth][dispatch]") {
    KeyedRateLimiter limiter;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(limiter.allow("a", 1, t0));
    CHECK_FALSE(limiter.allow("a", 1, t0 + std::chrono::seconds(30)));
    CHECK(limiter.allow("b", 1, t0 + std::chrono::seconds(30)));
    CHECK(limiter.allow("a", 1, t0 + std::chrono::seconds(61)));
    CHECK(limiter.allow("a", 0, t0));  // unlimited
}

TEST_CASE("dispatcher: listing hides tools a token cannot use", "[auth][dispatch]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("l", {"screen", "element"});
    p.read_only = true;
    std::set<std::string> names;
    for (const auto& def : d.list_tools_for(p)) names.insert(def.name);
    CHECK(names.count("gui_screen_read"));
    CHECK(names.count("gui_element_get"));
    CHECK_FALSE(names.count("gui_element_fill"));  // write tool, read-only token
    CHECK_FALSE(names.count("gui_session_list"));  // scope
    CHECK(d.list_tools_for(Principal{}).size() == d.list_tools().size());
}

TEST_CASE("dispatcher: stdio default principal keeps its audit shape", "[auth][dispatch]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    CallContext ctx;
    ctx.request_id = 3;
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx).is_error);
    REQUIRE(f.records.size() == 1);
    CHECK(f.records[0].principal == "stdio");
    CHECK(f.records[0].transport == "stdio");
    CHECK(f.records[0].remote_addr.empty());
    CHECK(f.records[0].era.empty());
    CHECK_FALSE(f.records[0].read_only);
}

// ---- audit -----------------------------------------------------------------------------------
TEST_CASE("audit: principal, remote_addr, transport and era reach the JSONL; no secret does", "[auth][audit]") {
    const auto file = std::filesystem::temp_directory_path() /
                      ("ff_auth_audit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    audit::AuditConfig config;
    config.mode = audit::Mode::Enabled;
    config.file = file;
    audit::AuditSink sink(config);

    const std::string secret = "SECRETSECRETSECRETSECRETSECRETSECRETSECRET0";
    Fixture f;
    Policy policy = write_mode();
    CommandDispatcher d([&](const Argv& argv) { f.calls.push_back(argv); return ok_result(); }, policy,
                        make_mcp_audit_hook(&sink, nullptr, false));
    Principal p = token("ci-bot", {"screen", "element"});
    p.remote_addr = "10.0.0.5";
    p.read_only = true;
    d.set_client_info(json{{"name", "curl"}, {"version", "1"}});

    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(p)).is_error);
    CHECK(d.call_tool("gui_element_fill", {{"element", "wnd[0]/usr/txtA"}, {"value", "x"}}, ctx_for(p)).is_error);   // READ_ONLY
    CHECK(d.call_tool("gui_session_list", json::object(), ctx_for(p)).is_error);                                    // SCOPE_DENIED

    std::ifstream in(file);
    std::string line;
    std::vector<nlohmann::json> rows;
    std::string all;
    while (std::getline(in, line)) {
        all += line + "\n";
        rows.push_back(nlohmann::json::parse(line));
    }
    in.close();
    std::filesystem::remove(file);

    REQUIRE(rows.size() == 3);
    for (const auto& row : rows) {
        CHECK(row["principal"] == "ci-bot");
        CHECK(row["remote_addr"] == "10.0.0.5");
        CHECK(row["transport"] == "http");
        CHECK(row["era"] == "stateless");
        CHECK(row["audit_source"] == "mcp");
        CHECK(row["read_only"] == true);
    }
    CHECK(rows[0]["status"] == "success");
    CHECK(rows[1]["error_code"] == "READ_ONLY");
    CHECK(rows[2]["error_code"] == "SCOPE_DENIED");
    CHECK(all.find(secret) == std::string::npos);
    CHECK(all.find("ffy_") == std::string::npos);
    CHECK(all.find("\"value\"") == std::string::npos);
}

TEST_CASE("audit: stdio records omit remote_addr and era but carry principal and transport", "[auth][audit]") {
    audit::AuditRecord rec;
    rec.source = "mcp";
    rec.tool = "gui_screen_read";
    rec.principal = "stdio";
    rec.transport = "stdio";
    const auto j = nlohmann::json::parse(audit::format_record(rec));
    CHECK(j["principal"] == "stdio");
    CHECK(j["transport"] == "stdio");
    CHECK_FALSE(j.contains("remote_addr"));
    CHECK_FALSE(j.contains("era"));
    // CLI records are unchanged
    audit::AuditRecord cli;
    const auto c = nlohmann::json::parse(audit::format_record(cli));
    CHECK_FALSE(c.contains("principal"));
    CHECK_FALSE(c.contains("transport"));
}

// ---- CLI wiring ------------------------------------------------------------------------------
namespace {
struct Parsed {
    bool ok = false;
    std::string path, error;
};
Parsed dry(const Argv& args) {
    Parsed out;
    CLI::App app{"fairyfly"};
    commands::GlobalOptions global;
    commands::add_global_options(app, global);
    commands::build_command_tree(app);
    Argv full{"fairyfly"};
    full.insert(full.end(), args.begin(), args.end());
    std::vector<const char*> ptrs;
    for (const auto& a : full) ptrs.push_back(a.c_str());
    try {
        app.parse(static_cast<int>(ptrs.size()), ptrs.data());
        out.ok = true;
    } catch (const CLI::CallForHelp&) {
        out.ok = true;
    } catch (const CLI::ParseError& e) {
        out.error = e.what();
    }
    out.path = commands::invoked_command_path(app);
    return out;
}
} // namespace

TEST_CASE("cli: mcp token subcommands parse and do not start the server", "[auth][cli]") {
    auto p = dry({"mcp", "token", "create", "ci", "--scope", "screen,element", "--system", "A4H/001,QAS/*", "--tcode", "SE16,SM*",
                  "--rate", "30", "--ip", "10.0.0.0/8,192.168.1.5", "--expires", "30d", "--read-only", "--yes", "--output", "json"});
    CHECK(p.ok);
    CHECK(p.path == "mcp token create");
    CHECK(dry({"mcp", "token", "list", "--output", "toon"}).path == "mcp token list");
    CHECK(dry({"mcp", "token", "revoke", "ci"}).path == "mcp token revoke");
    CHECK(dry({"mcp", "token", "rotate", "ci", "--output", "markdown"}).path == "mcp token rotate");
    CHECK(dry({"mcp", "token", "delete", "ci", "--yes"}).path == "mcp token delete");
    CHECK_FALSE(dry({"mcp", "token", "delete"}).ok);   // name is required
    CHECK_FALSE(dry({"mcp", "token", "create"}).ok);   // name is required
    CHECK_FALSE(dry({"mcp", "token"}).ok);             // a verb is required
    // the server command and its tools listing are unaffected
    CHECK(dry({"mcp"}).path == "mcp");
    CHECK(dry({"mcp", "tools"}).path == "mcp tools");
    CHECK(dry({"mcp", "--allow-write"}).path == "mcp");
}
