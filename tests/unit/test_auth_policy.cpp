#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <thread>

#include <CLI/CLI.hpp>

#include "include/audit_log.h"
#include "include/auth/authorize.h"
#include "include/auth/token_store.h"
#include "include/command_table.h"
#include "include/commands/cli_app.h"
#include "include/cli_handler.h"
#include "include/commands/global_options.h"
#include "include/mcp/dispatcher.h"
#include "include/mcp/mcp_audit.h"
#include "include/mcp/policy.h"
#include "include/mcp/session_leases.h"
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
            if (spec.family == granted && spec.def.name != "gui_session_lease") {
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

    // safe keys and menu listings act on the open transaction: allowed when it is allowlisted
    CHECK(decide(p, "gui_key_send", {{"key", "enter"}}, false, std::nullopt, "SE16").allowed);
    CHECK(decide(p, "gui_menu_list", {}, false, std::nullopt, "SM50").allowed);
    // menu selection is fail-closed (menu paths start transactions), see the navigation tests below
    CHECK(decide(p, "gui_menu_select", {{"path", "System > Services"}}, false, std::nullopt, "SM50").code == "TCODE_DENIED");

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
    std::vector<std::pair<std::string, std::string>> owner_guard_events;
    std::function<bool(const audit::SapFacts&, const std::string&)> attach_guard;
    std::function<bool(const audit::SapFacts&, const std::string&)> login_guard;
    std::function<bool(const audit::SapFacts&, const std::string&)> launch_guard;
    std::vector<int> launch_rollbacks;
    std::vector<bool> attach_finalizations;
    std::optional<audit::SapFacts> facts;
    std::function<std::optional<audit::SapFacts>(std::optional<int>)> facts_for;  // per-connection facts (wins over `facts`)
    std::function<Result(const Argv&)> handler = [](const Argv&) { return ok_result(); };

    std::unique_ptr<CommandDispatcher> make(Policy policy = Policy{}) {
        auto d = std::make_unique<CommandDispatcher>([this](const Argv& argv) { calls.push_back(argv); return handler(argv); },
                                                     policy, [this](const McpCallRecord& r) { records.push_back(r); });
        d->set_sap_facts_provider([this](std::optional<int> c) { return facts_for ? facts_for(c) : facts; });
        d->set_connection_snapshot_provider([this] {
            const Argv argv{"connection", "list"};
            calls.push_back(argv);
            return handler(argv);
        });
        d->set_read_only_override([this](bool ro) { read_only_events.push_back(ro); });
        d->set_owner_session_override([this](const std::string& session, const std::string& owner) {
            owner_guard_events.emplace_back(session, owner);
        });
        d->set_attach_target_guard_override([this](CommandDispatcher::AttachTargetGuard guard) {
            attach_guard = std::move(guard);
        });
        d->set_login_target_guard_override([this](CommandDispatcher::LoginTargetGuard guard) {
            login_guard = std::move(guard);
        });
        d->set_launch_target_guard_override([this](CommandDispatcher::LaunchTargetGuard guard) {
            launch_guard = std::move(guard);
        });
        d->set_launch_rollback_override([this](int id) {
            launch_rollbacks.push_back(id);
            return true;
        });
        d->set_attach_finalize_override([this](bool accepted) {
            attach_finalizations.push_back(accepted);
            return true;
        });
        d->set_control_probe_gate(std::make_shared<std::shared_timed_mutex>());
        return d;
    }
};

CallContext ctx_for(const Principal& p, bool http = true) {
    CallContext ctx;
    ctx.request_id = 1;
    ctx.principal = p;
    ctx.http = http;
    ctx.era = ProtocolEra::Stateless;
    ctx.reauthorize = [] { return true; };
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
    p.id = "issued-token-123";
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
        CHECK(row["token_id"] == "issued-token-123");
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
    CHECK(dry({"mcp", "token", "create", "ci", "--tcode", "SE16", "--allow-navigation"}).ok);
    CHECK(dry({"mcp", "token", "create", "ci", "--tcode", "SE16", "--allow-navigation"}).path == "mcp token create");
    CHECK(dry({"mcp", "token", "create", "ci", "--tcode", "SU01", "--read-only", "--allow-selection-input"}).ok);
    CHECK_FALSE(dry({"mcp", "token", "delete"}).ok);   // name is required
    CHECK_FALSE(dry({"mcp", "token", "create"}).ok);   // name is required
    CHECK_FALSE(dry({"mcp", "token"}).ok);             // a verb is required
    // the server command and its tools listing are unaffected
    CHECK(dry({"mcp"}).path == "mcp");
    CHECK(dry({"mcp", "tools"}).path == "mcp tools");
    CHECK(dry({"mcp", "--allow-write"}).path == "mcp");
}

// ---- session/connection targets (SAP-system and saved-connection allowlists) ---------------------
TEST_CASE("authorize: session target rules are pure and fail closed", "[auth][authz][target]") {
    using auth::SessionTarget;
    Principal open = token("open", {"session"});
    Principal sys = token("sys", {"session"});
    sys.sap_systems = {"A4H/001", "QAS"};
    Principal conn = token("conn", {"session", "screen"});
    conn.connections = {"DEV*", "Bigfox"};

    // tokens without lists are unchanged, whatever the target says
    CHECK(auth::authorize_session_target(open, "gui_session_launch", {{"name", "PRD"}}, SessionTarget{}).allowed);
    CHECK_FALSE(auth::needs_session_target(open, "gui_session_launch", {{"name", "PRD"}}));

    // system allowlist: launch/login/attach and disconnect --close-session; unknown target => SYSTEM_UNKNOWN
    CHECK(auth::needs_session_target(sys, "gui_session_launch", json::object()));
    for (const char* tool : {"gui_session_login", "gui_session_attach"}) {
        CHECK(auth::needs_session_target(sys, tool, json::object()));
        CHECK(auth::authorize_session_target(sys, tool, {{"name", "X"}}, SessionTarget{"A4H/001", ""}).allowed);
        CHECK(auth::authorize_session_target(sys, tool, {{"name", "X"}}, SessionTarget{"QAS/300", ""}).allowed);  // "QAS" = any client
        CHECK(auth::authorize_session_target(sys, tool, {{"name", "X"}}, SessionTarget{"PRD/100", ""}).code == "SYSTEM_DENIED");
        const auto unknown = auth::authorize_session_target(sys, tool, {{"name", "X"}}, SessionTarget{});
        CHECK_FALSE(unknown.allowed);
        CHECK(unknown.code == "SYSTEM_UNKNOWN");
        CHECK_FALSE(unknown.message.empty());
    }
    // launch with sap_systems: only for entry names vouched for by a --connections glob; other sessions' facts are no proof
    {
        Principal vouched = token("vouched", {"session"});
        vouched.sap_systems = {"A4H/001"};
        vouched.connections = {"DEV*"};
        const json dev = {{"name", "DEV1"}};
        const auto no_connections = auth::authorize_session_target(sys, "gui_session_launch", dev, SessionTarget{"A4H/001", "DEV1"});
        CHECK(no_connections.code == "SYSTEM_UNKNOWN");  // even when the entry looks like an allowed system
        CHECK(no_connections.message.find("--connections") != std::string::npos);
        CHECK(auth::authorize_session_target(sys, "gui_session_launch", dev, SessionTarget{}).code == "SYSTEM_UNKNOWN");
        // vouched entry name, no open session of that name: allowed
        CHECK(auth::authorize_session_target(vouched, "gui_session_launch", dev, SessionTarget{}).allowed);
        // entry name outside the glob: the system rule says unknown, never allowed
        CHECK(auth::authorize_session_target(vouched, "gui_session_launch", {{"name", "PRD"}}, SessionTarget{}).code == "SYSTEM_UNKNOWN");
        CHECK(auth::authorize_session_target(vouched, "gui_session_launch", json::object(), SessionTarget{}).code == "SYSTEM_UNKNOWN");
        // an open session of that name must be on an allowed system
        CHECK(auth::authorize_session_target(vouched, "gui_session_launch", dev, SessionTarget{"A4H/001", "DEV1"}).allowed);
        CHECK(auth::authorize_session_target(vouched, "gui_session_launch", dev, SessionTarget{"PRD/100", "DEV1"}).code == "SYSTEM_DENIED");
        SessionTarget ambiguous;
        ambiguous.ambiguous = true;
        CHECK(auth::authorize_session_target(vouched, "gui_session_launch", dev, ambiguous).code == "SYSTEM_UNKNOWN");
    }
    CHECK_FALSE(auth::needs_session_target(sys, "gui_session_disconnect", json::object()));
    CHECK_FALSE(auth::needs_session_target(sys, "gui_session_disconnect", {{"close_session", false}}));
    CHECK(auth::needs_session_target(sys, "gui_session_disconnect", {{"close_session", true}}));
    CHECK(auth::authorize_session_target(sys, "gui_session_disconnect", {{"close_session", true}}, SessionTarget{"PRD/100", ""}).code ==
          "SYSTEM_DENIED");
    CHECK_FALSE(auth::needs_session_target(sys, "gui_session_list", json::object()));

    // connection names: launch uses the requested SAP Logon entry itself
    CHECK(auth::authorize_session_target(conn, "gui_session_launch", {{"name", "dev-1"}}, SessionTarget{}).allowed);
    CHECK(auth::authorize_session_target(conn, "gui_session_launch", {{"name", "PRD"}}, SessionTarget{}).code == "CONNECTION_DENIED");
    CHECK(auth::authorize_session_target(conn, "gui_session_login", json::object(), SessionTarget{"", "Bigfox"}).allowed);
    CHECK(auth::authorize_session_target(conn, "gui_session_attach", json::object(), SessionTarget{"", "PRD"}).code == "CONNECTION_DENIED");
    CHECK(auth::authorize_session_target(conn, "gui_session_attach", json::object(), SessionTarget{}).code == "CONNECTION_DENIED");
    CHECK(auth::authorize_session_target(conn, "gui_screen_read", json::object(), SessionTarget{"", "DEV2"}).allowed);
    CHECK(auth::authorize_session_target(conn, "gui_screen_read", json::object(), SessionTarget{"", "PRD"}).code == "CONNECTION_DENIED");
    for (const char* tool : {"gui_session_list", "gui_connection_list", "gui_credentials_list", "gui_doctor", "gui_batch"})
        CHECK_FALSE(auth::needs_session_target(conn, tool, json::object()));
}

TEST_CASE("dispatcher: session tools enforce the SAP-system allowlist through the target resolver", "[auth][dispatch][target]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("sys", {"session"});
    p.sap_systems = {"A4H/001"};
    p.connections = {"DEV", "PRD", "NEW"};  // launch needs the entry names vouched for by --connections
    std::vector<CommandDispatcher::TargetQuery> queries;
    std::map<std::string, auth::SessionTarget> by_logon = {{"DEV", {"A4H/001", "DEV"}}, {"PRD", {"PRD/100", "PRD"}}};
    std::map<std::string, auth::SessionTarget> by_session = {{"/app/con[0]/ses[0]", {"A4H/001", "DEV"}},
                                                             {"/app/con[1]/ses[0]", {"PRD/100", "PRD"}}};
    std::map<int, auth::SessionTarget> by_connection = {{1, {"A4H/001", "DEV"}}, {2, {"PRD/100", "PRD"}}};
    d.set_session_target_resolver([&](const CommandDispatcher::TargetQuery& q) -> auth::SessionTarget {
        queries.push_back(q);
        if (!q.logon_name.empty()) return by_logon.count(q.logon_name) ? by_logon[q.logon_name] : auth::SessionTarget{};
        if (!q.session_id.empty()) return by_session.count(q.session_id) ? by_session[q.session_id] : auth::SessionTarget{};
        return q.connection && by_connection.count(*q.connection) ? by_connection[*q.connection] : auth::SessionTarget{};
    });

    // launch by SAP Logon name
    auto r = d.call_tool("gui_session_launch", {{"name", "PRD"}}, ctx_for(p));
    CHECK(text_of(r).find("SYSTEM_DENIED") != std::string::npos);
    r = d.call_tool("gui_session_launch", {{"name", "OTHER"}}, ctx_for(p));  // not vouched for by --connections
    CHECK(text_of(r).find("SYSTEM_UNKNOWN") != std::string::npos);
    CHECK(f.calls.empty());
    r = d.call_tool("gui_session_launch", {{"name", "DEV"}, {"login", true}}, ctx_for(p));
    CHECK_FALSE(r.is_error);
    REQUIRE(f.calls.size() == 1);
    f.calls.clear();
    r = d.call_tool("gui_session_launch", {{"name", "NEW"}}, ctx_for(p));  // vouched, no open session: allowed
    CHECK_FALSE(r.is_error);
    REQUIRE(f.calls.size() == 1);
    f.calls.clear();
    // a token WITHOUT connections cannot launch at all, even for an entry whose open session is on an allowed system
    Principal no_conn = token("sys2", {"session", "batch"});
    no_conn.sap_systems = {"A4H/001"};
    r = d.call_tool("gui_session_launch", {{"name", "DEV"}}, ctx_for(no_conn));
    CHECK(text_of(r).find("SYSTEM_UNKNOWN") != std::string::npos);
    CHECK(f.calls.empty());
    // inside gui_batch too
    r = d.call_tool("gui_batch", {{"items", json::array({{{"tool", "gui_session_launch"}, {"arguments", {{"name", "DEV"}, {"login", true}}}}})}},
                    ctx_for(no_conn));
    CHECK(r.is_error);
    CHECK(text_of(r).find("SYSTEM_UNKNOWN") != std::string::npos);
    CHECK(f.calls.empty());

    // attach by explicit session id
    f.calls.clear();
    r = d.call_tool("gui_session_attach", {{"session_id", "/app/con[1]/ses[0]"}}, ctx_for(p));
    CHECK(text_of(r).find("SYSTEM_DENIED") != std::string::npos);
    r = d.call_tool("gui_session_attach", {{"session_id", "/app/con[9]/ses[0]"}}, ctx_for(p));
    CHECK(text_of(r).find("SYSTEM_UNKNOWN") != std::string::npos);
    CHECK(f.calls.empty());
    r = d.call_tool("gui_session_attach", {{"session_id", "/app/con[0]/ses[0]"}}, ctx_for(p));
    CHECK_FALSE(r.is_error);

    // login and disconnect --close-session use the saved connection
    f.calls.clear();
    r = d.call_tool("gui_session_login", {{"connection", 2}}, ctx_for(p));
    CHECK(text_of(r).find("SYSTEM_DENIED") != std::string::npos);
    r = d.call_tool("gui_session_disconnect", {{"connection", 2}, {"close_session", true}}, ctx_for(p));
    CHECK(text_of(r).find("SYSTEM_DENIED") != std::string::npos);
    CHECK(f.calls.empty());
    r = d.call_tool("gui_session_login", {{"connection", 1}}, ctx_for(p));
    CHECK_FALSE(r.is_error);
    // a plain disconnect (no session ended) is not a system question
    r = d.call_tool("gui_session_disconnect", {{"connection", 2}}, ctx_for(p));
    CHECK_FALSE(r.is_error);

    // a token without any resolver answer at all is denied, never allowed by default
    Fixture g;
    auto dg_ptr = g.make(write_mode());
    CHECK(text_of(dg_ptr->call_tool("gui_session_launch", {{"name", "DEV"}}, ctx_for(p))).find("SYSTEM_UNKNOWN") != std::string::npos);
    CHECK(g.calls.empty());

    // tokens without sap_systems never trigger a lookup; stdio neither
    queries.clear();
    Principal open = token("open", {"session"});
    CHECK_FALSE(d.call_tool("gui_session_launch", {{"name", "PRD"}}, ctx_for(open)).is_error);
    CHECK_FALSE(d.call_tool("gui_session_launch", {{"name", "PRD"}}, ctx_for(Principal{}, false)).is_error);
    CHECK(queries.empty());
}

TEST_CASE("dispatcher: attach without session_id checks the session it resolves to", "[auth][dispatch][target]") {
    Fixture f;
    f.handler = [](const Argv& argv) {
        Result r = ok_result();
        if (argv.size() >= 2 && argv[0] == "session" && argv[1] == "list")
            r.data = {{"connections", json::array({{{"description", "PRD"},
                                                    {"sessions", json::array({{{"id", "/app/con[1]/ses[0]"}}})}}})}};
        return r;
    };
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("sys", {"session"});
    p.sap_systems = {"A4H/001"};
    d.set_session_target_resolver([](const CommandDispatcher::TargetQuery& q) -> auth::SessionTarget {
        return q.session_id == "/app/con[1]/ses[0]" ? auth::SessionTarget{"PRD/100", "PRD"} : auth::SessionTarget{};
    });
    const auto r = d.call_tool("gui_session_attach", json::object(), ctx_for(p));
    CHECK(text_of(r).find("SYSTEM_DENIED") != std::string::npos);
    for (const auto& call : f.calls) CHECK(call[1] == "list");  // only the read-only listing ran, never the attach
}

TEST_CASE("dispatcher: connections allowlist binds session and connection-targeting tools by name", "[auth][dispatch][target]") {
    Fixture f;
    f.handler = [](const Argv& argv) {  // the listings return well-formed (empty) data: they are filtered for this token
        Result r = ok_result();
        if (argv[0] == "connection") r.data = {{"connections", json::array()}, {"count", 0}};
        else if (argv[0] == "session" && argv.size() > 1 && argv[1] == "list") r.data = {{"connections", json::array()}};
        return r;
    };
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("conn", {"session", "screen", "connection", "batch"});
    p.connections = {"DEV*"};
    d.set_session_target_resolver([](const CommandDispatcher::TargetQuery& q) -> auth::SessionTarget {
        if (q.connection == 1) return {"", "DEV1"};
        if (q.connection == 2) return {"", "PRD"};
        return {};
    });
    CHECK_FALSE(d.call_tool("gui_session_launch", {{"name", "DEV1"}}, ctx_for(p)).is_error);
    CHECK(text_of(d.call_tool("gui_session_launch", {{"name", "PRD"}}, ctx_for(p))).find("CONNECTION_DENIED") != std::string::npos);
    CHECK_FALSE(d.call_tool("gui_session_login", {{"connection", 1}}, ctx_for(p)).is_error);
    CHECK(text_of(d.call_tool("gui_session_login", {{"connection", 2}}, ctx_for(p))).find("CONNECTION_DENIED") != std::string::npos);
    CHECK(text_of(d.call_tool("gui_session_disconnect", {{"connection", 2}}, ctx_for(p))).find("CONNECTION_DENIED") != std::string::npos);
    CHECK_FALSE(d.call_tool("gui_screen_read", {{"connection", 1}}, ctx_for(p)).is_error);
    CHECK(text_of(d.call_tool("gui_screen_read", {{"connection", 2}}, ctx_for(p))).find("CONNECTION_DENIED") != std::string::npos);
    CHECK(text_of(d.call_tool("gui_screen_read", json::object(), ctx_for(p))).find("CONNECTION_DENIED") != std::string::npos);  // unknown
    // listings have no target
    CHECK_FALSE(d.call_tool("gui_connection_list", json::object(), ctx_for(p)).is_error);
    CHECK_FALSE(d.call_tool("gui_session_list", json::object(), ctx_for(p)).is_error);
    // batch items are checked one by one
    const auto r = d.call_tool("gui_batch", {{"items", json::array({{{"tool", "gui_screen_read"}, {"arguments", {{"connection", 1}}}},
                                                                    {{"tool", "gui_screen_read"}, {"arguments", {{"connection", 2}}}}})},
                                              {"stop_on_error", false}}, ctx_for(p));
    CHECK(text_of(r).find("CONNECTION_DENIED") != std::string::npos);
}

TEST_CASE("dispatcher: connection list cleanup is refused for tokens limited to named connections", "[auth][dispatch][cleanup]") {
    Fixture f;
    f.handler = [](const Argv&) {
        Result r = ok_result();
        r.data = {{"connections", json::array()}, {"count", 0}};
        return r;
    };
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("conn", {"connection", "batch"});
    p.connections = {"DEV*"};
    p.read_only = false;

    auto r = d.call_tool("gui_connection_list", {{"cleanup", true}}, ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("CONNECTION_DENIED") != std::string::npos);
    CHECK(f.calls.empty());  // nothing was deleted: the invoker never ran

    // inside gui_batch (refused up front, no item runs)
    r = d.call_tool("gui_batch", {{"items", json::array({{{"tool", "gui_connection_list"}, {"arguments", {{"cleanup", true}}}}})}}, ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("CONNECTION_DENIED") != std::string::npos);
    CHECK(f.calls.empty());

    // a plain listing and cleanup=false are still fine
    CHECK_FALSE(d.call_tool("gui_connection_list", {{"cleanup", false}}, ctx_for(p)).is_error);
    CHECK_FALSE(d.call_tool("gui_connection_list", json::object(), ctx_for(p)).is_error);

    // tokens without a connections restriction keep cleanup
    f.calls.clear();
    Principal open = token("open", {"connection"});
    open.read_only = false;
    CHECK_FALSE(d.call_tool("gui_connection_list", {{"cleanup", true}}, ctx_for(open)).is_error);
    REQUIRE(f.calls.size() == 1);
}

TEST_CASE("dispatcher: gui_doctor is refused for tokens limited to named connections", "[auth][dispatch][doctor]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("conn", {"system", "batch"});
    p.connections = {"DEV*"};

    auto r = d.call_tool("gui_doctor", json::object(), ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("CONNECTION_DENIED") != std::string::npos);
    r = d.call_tool("gui_batch", {{"items", json::array({{{"tool", "gui_doctor"}}})}}, ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("CONNECTION_DENIED") != std::string::npos);
    CHECK(f.calls.empty());  // the invoker never ran

    Principal open = token("open", {"system"});
    CHECK_FALSE(d.call_tool("gui_doctor", json::object(), ctx_for(open)).is_error);
    CHECK(f.calls.size() == 1);
}

// ---- listings are filtered to the token's connections --------------------------------------------
namespace {

std::string everything_of(const ToolResult& r) {
    std::string all;
    for (const auto& block : r.content) all += block.dump() + "\n";
    if (r.structured) all += r.structured->dump();
    return all;
}

json session_list_data() {
    return {{"connections",
             json::array({{{"index", 0}, {"id", "/app/con[0]"}, {"description", "DEV1"}, {"connection_string", "dev.example"},
                           {"backend_scripting_disabled", false}, {"reported_session_count", 2}, {"session_errors", json::array()},
                           {"sessions", json::array({{{"id", "/app/con[0]/ses[0]"}}, {{"id", "/app/con[0]/ses[1]"}}})},
                           {"session_count", 2}},
                          {{"index", 1}, {"id", "/app/con[1]"}, {"description", "SECRETPRD"}, {"connection_string", "prd.example"},
                           {"backend_scripting_disabled", true}, {"reported_session_count", 3},
                           {"session_errors", json::array({{{"index", 2}, {"message", "SECRETPRD boom"}}})},
                           {"sessions", json::array({{{"id", "/app/con[1]/ses[0]"}}, {{"id", "/app/con[1]/ses[1]"}}})},
                           {"session_count", 2}},
                          {{"index", 2}, {"error", "SECRETNONAME failed"}, {"sessions", json::array()}, {"session_count", 0}},
                          {{"index", 3}, {"id", "/app/con[3]"}, {"description", "DEV2"}, {"backend_scripting_disabled", nullptr},
                           {"session_errors", json::array()}, {"sessions", json::array({{{"id", "/app/con[3]/ses[0]"}}})},
                           {"session_count", 1}}})},
            {"total_connections", 4}, {"total_sessions", 5}, {"backend_scripting_disabled", true},
            {"connection_enumeration_errors", 1}, {"session_enumeration_errors", 1}};
}

json connection_list_data() {
    return {{"connections", json::array({{{"id", 1}, {"file", "C:/x/1.json"}, {"description", "DEV1"}, {"valid", true}},
                                         {{"id", 2}, {"file", "C:/x/SECRETPRD.json"}, {"description", "SECRETPRD"}, {"valid", true}},
                                         {{"id", 3}, {"file", "C:/x/3.json"}, {"description", ""}, {"valid", false}},
                                         {{"id", 4}, {"file", "C:/x/4.json"}, {"description", "dev-2"}, {"valid", false}}})},
            {"count", 4}};
}

json credentials_list_data() {
    return {{"credentials", json::array({{{"connection", "DEV1"}, {"username", "alice"}, {"client", "100"}},
                                         {{"connection", "SECRETPRD"}, {"username", "SECRETUSER"}, {"client", "999"}},
                                         {{"connection", "DEV2"}, {"username", "bob"}, {"client", "200"}}})},
            {"count", 3}};
}

Principal listing_token() {
    Principal p = token("lister", {"session", "connection", "credentials", "batch"});
    p.connections = {"DEV*"};
    return p;
}

} // namespace

TEST_CASE("authorize: session list is filtered to the token's connections with recomputed totals", "[auth][filter]") {
    Principal p = listing_token();
    Result r = ok_result();
    r.data = session_list_data();
    REQUIRE(auth::filter_listing_for_connections(p, "gui_session_list", r));
    REQUIRE(r.data["connections"].size() == 2);
    CHECK(r.data["connections"][0]["description"] == "DEV1");
    CHECK(r.data["connections"][1]["description"] == "DEV2");
    CHECK(r.data["total_connections"] == 2);
    CHECK(r.data["total_sessions"] == 3);
    CHECK(r.data["connection_enumeration_errors"] == 0);  // the nameless failed entry is gone
    CHECK(r.data["session_enumeration_errors"] == 0);     // the dropped connection's session error is gone
    CHECK(r.data["backend_scripting_disabled"] == false);  // came only from the dropped connection
    const std::string all = r.data.dump();
    for (const char* leak : {"SECRETPRD", "SECRETNONAME", "prd.example", "/app/con[1]", "con[2]"})
        CHECK(all.find(leak) == std::string::npos);

    // nothing allowed: empty lists, zero totals
    Principal none = listing_token();
    none.connections = {"NOPE"};
    Result empty = ok_result();
    empty.data = session_list_data();
    REQUIRE(auth::filter_listing_for_connections(none, "gui_session_list", empty));
    CHECK(empty.data["connections"].empty());
    CHECK(empty.data["total_connections"] == 0);
    CHECK(empty.data["total_sessions"] == 0);
}

TEST_CASE("authorize: connection and credentials lists are filtered by name with recomputed counts", "[auth][filter]") {
    Principal p = listing_token();
    Result c = ok_result();
    c.data = connection_list_data();
    REQUIRE(auth::filter_listing_for_connections(p, "gui_connection_list", c));
    REQUIRE(c.data["connections"].size() == 2);  // DEV1, dev-2 (case-insensitive glob); the unnamed row is dropped
    CHECK(c.data["count"] == 2);
    CHECK(c.data.dump().find("SECRETPRD") == std::string::npos);

    Result k = ok_result();
    k.data = credentials_list_data();
    REQUIRE(auth::filter_listing_for_connections(p, "gui_credentials_list", k));
    REQUIRE(k.data["credentials"].size() == 2);
    CHECK(k.data["count"] == 2);
    const std::string all = k.data.dump();
    for (const char* leak : {"SECRETPRD", "SECRETUSER", "999"}) CHECK(all.find(leak) == std::string::npos);

    // unrestricted tokens and other tools are untouched
    Principal open = token("open", {"session"});
    Result untouched = ok_result();
    untouched.data = credentials_list_data();
    CHECK(auth::filter_listing_for_connections(open, "gui_credentials_list", untouched));
    CHECK(untouched.data == credentials_list_data());
    CHECK_FALSE(auth::listing_needs_filter(p, "gui_screen_read"));
}

TEST_CASE("authorize: a listing of unexpected shape is never returned unfiltered", "[auth][filter]") {
    Principal p = listing_token();
    for (const char* tool : {"gui_session_list", "gui_connection_list", "gui_credentials_list"}) {
        for (const json& bad : {json::array(), json("text"), json::object(), json{{"connections", "x"}, {"credentials", "x"}},
                                json{{"connections", json::array({"not-an-object"})}, {"credentials", json::array({5})}},
                                json{{"connections", json::array()}, {"credentials", json::array()}, {"extra", "SECRETPRD"}}}) {
            Result r = ok_result();
            r.data = bad;
            INFO(tool << " " << bad.dump());
            CHECK_FALSE(auth::filter_listing_for_connections(p, tool, r));
            CHECK(r.data.is_null());
        }
    }
    // an error keeps its code only
    Result e;
    e.status = Result::Status::Error;
    e.error = {{"code", "ENUMERATION_FAILED"}, {"message", "SECRETPRD unreachable"}, {"connections", json::array({"SECRETPRD"})}};
    e.diagnostics = {{"trace", "SECRETPRD"}};
    REQUIRE(auth::filter_listing_for_connections(p, "gui_connection_list", e));
    CHECK(e.error["code"] == "ENUMERATION_FAILED");
    CHECK(e.error.dump().find("SECRETPRD") == std::string::npos);
    CHECK(e.diagnostics.is_null());
}

TEST_CASE("dispatcher: the three listings only show the token's connections (no leak in text or structure)", "[auth][dispatch][filter]") {
    Fixture f;
    f.handler = [](const Argv& argv) {
        Result r = ok_result();
        if (argv[0] == "session" && argv[1] == "list") r.data = session_list_data();
        else if (argv[0] == "connection" && argv[1] == "list") r.data = connection_list_data();
        else if (argv[0] == "credentials" && argv[1] == "list") r.data = credentials_list_data();
        return r;
    };
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    const Principal p = listing_token();

    for (const char* tool : {"gui_session_list", "gui_connection_list", "gui_credentials_list"}) {
        INFO(tool);
        const auto r = d.call_tool(tool, json::object(), ctx_for(p));
        CHECK_FALSE(r.is_error);
        const std::string all = everything_of(r);
        for (const char* leak : {"SECRETPRD", "SECRETNONAME", "SECRETUSER", "prd.example"}) CHECK(all.find(leak) == std::string::npos);
        CHECK(all.find("DEV1") != std::string::npos);
        CHECK(all.find(std::string(tool) == "gui_connection_list" ? "dev-2" : "DEV2") != std::string::npos);
    }
    // the same inside gui_batch (each item is filtered before it is rendered)
    const auto batch = d.call_tool("gui_batch", {{"items", json::array({{{"tool", "gui_session_list"}}, {{"tool", "gui_connection_list"}},
                                                                        {{"tool", "gui_credentials_list"}}})}}, ctx_for(p));
    CHECK_FALSE(batch.is_error);
    const std::string all = everything_of(batch);
    for (const char* leak : {"SECRETPRD", "SECRETNONAME", "SECRETUSER", "prd.example"}) CHECK(all.find(leak) == std::string::npos);
    CHECK(all.find("DEV1") != std::string::npos);

    // an unrestricted token still sees everything
    Principal open = token("open", {"session", "connection", "credentials"});
    CHECK(everything_of(d.call_tool("gui_credentials_list", json::object(), ctx_for(open))).find("SECRETPRD") != std::string::npos);
    CHECK(everything_of(d.call_tool("gui_session_list", json::object(), ctx_for(open))).find("SECRETPRD") != std::string::npos);
}

TEST_CASE("dispatcher: an unfilterable listing is an error, and errors do not name other connections", "[auth][dispatch][filter]") {
    Fixture f;
    f.handler = [](const Argv& argv) {
        Result r = ok_result();
        if (argv[0] == "credentials") r.data = {{"credentials", "SECRETPRD"}, {"count", 1}};      // wrong shape
        else if (argv[0] == "connection") {
            r.status = Result::Status::Error;
            r.error = {{"code", "ENUMERATION_FAILED"}, {"message", "SECRETPRD exploded"}};
        } else r.data = {{"connections", json::array()}, {"total_connections", 0}, {"surprise", "SECRETPRD"}};
        return r;
    };
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    const Principal p = listing_token();

    auto r = d.call_tool("gui_credentials_list", json::object(), ctx_for(p));
    CHECK(r.is_error);
    CHECK(everything_of(r).find("RESULT_FILTER_FAILED") != std::string::npos);
    CHECK(everything_of(r).find("SECRETPRD") == std::string::npos);

    r = d.call_tool("gui_session_list", json::object(), ctx_for(p));
    CHECK(r.is_error);
    CHECK(everything_of(r).find("SECRETPRD") == std::string::npos);

    r = d.call_tool("gui_connection_list", json::object(), ctx_for(p));
    CHECK(r.is_error);
    CHECK(everything_of(r).find("ENUMERATION_FAILED") != std::string::npos);
    CHECK(everything_of(r).find("SECRETPRD") == std::string::npos);
}

TEST_CASE("dispatcher: attach without session_id neither offers nor lists other connections' sessions", "[auth][dispatch][filter]") {
    Fixture f;
    f.handler = [](const Argv& argv) {
        Result r = ok_result();
        if (argv.size() >= 2 && argv[0] == "session" && argv[1] == "list") r.data = session_list_data();
        return r;
    };
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    d.set_session_target_resolver([](const CommandDispatcher::TargetQuery& q) -> auth::SessionTarget {
        return q.session_id == "/app/con[0]/ses[0]" ? auth::SessionTarget{"", "DEV1"} : auth::SessionTarget{};
    });
    Principal p = listing_token();
    p.read_only = false;
    const auto r = d.call_tool("gui_session_attach", json::object(), ctx_for(p));
    CHECK(r.is_error);
    CHECK(everything_of(r).find("MULTIPLE_SESSIONS") != std::string::npos);  // DEV1 has 2 sessions, DEV2 one
    for (const char* leak : {"SECRETPRD", "con[1]", "SECRETNONAME"}) CHECK(everything_of(r).find(leak) == std::string::npos);
}

// ---- per-principal sticky connection and budgets -------------------------------------------------
namespace {
bool argv_targets(const Argv& argv, const std::string& id) {
    for (std::size_t i = 0; i + 1 < argv.size(); ++i)
        if (argv[i] == "--connection" && argv[i + 1] == id) return true;
    return false;
}
bool argv_has_connection(const Argv& argv) {
    return std::find(argv.begin(), argv.end(), "--connection") != argv.end();
}
} // namespace

TEST_CASE("dispatcher: the sticky connection belongs to one principal", "[auth][dispatch][sticky]") {
    Fixture f;
    f.handler = [](const Argv& argv) {
        Result r = ok_result();
        if (argv.size() >= 3 && argv[0] == "session" && argv[1] == "attach") {
            // the attach result names the connection file; encode it from the session id (con[N] -> N)
            const std::string& sid = argv.back();
            const auto open = sid.find("con[");
            r.data = {{"connection_file_id", std::stoi(sid.substr(open + 4))}};
        }
        return r;
    };
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal a = token("token-a", {"session", "screen"});
    Principal b = token("token-b", {"session", "screen"});

    // A attaches connection 5; B has no default yet
    CHECK_FALSE(d.call_tool("gui_session_attach", {{"session_id", "/app/con[5]/ses[0]"}}, ctx_for(a)).is_error);
    CHECK(d.sticky_connection("token-a") == 5);
    CHECK_FALSE(d.sticky_connection("token-b").has_value());
    f.calls.clear();
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(b)).is_error);
    REQUIRE(f.calls.size() == 1);
    CHECK_FALSE(argv_has_connection(f.calls[0]));           // A's attach did not retarget B
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(a)).is_error);
    CHECK(argv_targets(f.calls.back(), "5"));

    // B attaches 7: interleaved calls keep their own targets
    CHECK_FALSE(d.call_tool("gui_session_attach", {{"session_id", "/app/con[7]/ses[0]"}}, ctx_for(b)).is_error);
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(a)).is_error);
    CHECK(argv_targets(f.calls.back(), "5"));
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(b)).is_error);
    CHECK(argv_targets(f.calls.back(), "7"));
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(a)).is_error);
    CHECK(argv_targets(f.calls.back(), "5"));
    CHECK(f.records.back().connection == 5);

    // an explicit connection argument still wins and does not change any default
    CHECK_FALSE(d.call_tool("gui_screen_read", {{"connection", 9}}, ctx_for(b)).is_error);
    CHECK(argv_targets(f.calls.back(), "9"));
    CHECK(d.sticky_connection("token-b") == 7);

    // the local stdio principal keeps the old single-default behaviour
    Fixture g;
    g.handler = f.handler;
    auto dg_ptr = g.make(write_mode());
    auto& dg = *dg_ptr;
    CHECK_FALSE(dg.call_tool("gui_session_attach", {{"session_id", "/app/con[3]/ses[0]"}}, ctx_for(Principal{}, false)).is_error);
    CHECK(dg.sticky_connection() == 3);
    CHECK_FALSE(dg.call_tool("gui_screen_read", json::object(), ctx_for(Principal{}, false)).is_error);
    CHECK(argv_targets(g.calls.back(), "3"));
}

TEST_CASE("dispatcher: the server default rate budget is per principal", "[auth][dispatch][sticky]") {
    Fixture f;
    Policy policy = write_mode();
    policy.max_calls_per_minute = 2;
    auto d_ptr = f.make(policy);
    auto& d = *d_ptr;
    Principal a = token("token-a", {"screen"});
    Principal b = token("token-b", {"screen"});
    Principal own = token("token-own", {"screen"});
    own.rate_per_minute = 5;
    for (int i = 0; i < 2; ++i) CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(a)).is_error);
    CHECK(text_of(d.call_tool("gui_screen_read", json::object(), ctx_for(a))).find("RATE_LIMITED") != std::string::npos);
    // B and a token with its own rate are not affected by A's exhausted budget
    for (int i = 0; i < 2; ++i) CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(b)).is_error);
    CHECK(text_of(d.call_tool("gui_screen_read", json::object(), ctx_for(b))).find("RATE_LIMITED") != std::string::npos);
    for (int i = 0; i < 5; ++i) CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(own)).is_error);
    CHECK(text_of(d.call_tool("gui_screen_read", json::object(), ctx_for(own))).find("RATE_LIMITED") != std::string::npos);
    // the stdio principal has its own server-wide gate
    for (int i = 0; i < 2; ++i) CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(Principal{}, false)).is_error);
    CHECK(text_of(d.call_tool("gui_screen_read", json::object(), ctx_for(Principal{}, false))).find("RATE_LIMITED") != std::string::npos);
}

TEST_CASE("dispatcher: per-family rate limits name the family and are independent", "[auth][dispatch][ratefamily]") {
    Fixture f;
    Policy policy = write_mode();
    policy.max_calls_per_minute = 100;
    auto d_ptr = f.make(policy);
    auto& d = *d_ptr;
    Principal p = token("fam", {"screen", "element", "key"});
    p.rate_families = {{"element", 2}, {"key", 1}};
    const json click = {{"element", "wnd[0]/usr/btnX"}};

    CHECK_FALSE(d.call_tool("gui_element_click", click, ctx_for(p)).is_error);
    CHECK_FALSE(d.call_tool("gui_element_click", click, ctx_for(p)).is_error);
    auto r = d.call_tool("gui_element_click", click, ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("RATE_LIMITED") != std::string::npos);
    CHECK(text_of(r).find("'element'") != std::string::npos);
    const std::size_t ran = f.calls.size();
    CHECK(ran == 2);  // the refused call never reached the CLI

    // other families are unaffected; key has its own 1 per minute
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(p)).is_error);
    CHECK_FALSE(d.call_tool("gui_key_send", {{"key", "enter"}}, ctx_for(p)).is_error);
    r = d.call_tool("gui_key_send", {{"key", "enter"}}, ctx_for(p));
    CHECK(text_of(r).find("'key'") != std::string::npos);

    // another principal, and a token without family limits, keep their own budgets
    Principal q = token("fam2", {"element"});
    q.rate_families = {{"element", 1}};
    CHECK_FALSE(d.call_tool("gui_element_click", click, ctx_for(q)).is_error);
    CHECK(d.call_tool("gui_element_click", click, ctx_for(q)).is_error);
    Principal free_token = token("free", {"element"});
    for (int i = 0; i < 5; ++i) CHECK_FALSE(d.call_tool("gui_element_click", click, ctx_for(free_token)).is_error);

    // every gui_batch item counts against its family
    Principal b = token("batcher", {"element", "batch"});
    b.rate_families = {{"element", 1}};
    const auto batch = d.call_tool("gui_batch", {{"items", json::array({{{"tool", "gui_element_click"}, {"arguments", click}},
                                                                        {{"tool", "gui_element_click"}, {"arguments", click}}})},
                                                  {"stop_on_error", false}}, ctx_for(b));
    CHECK(text_of(batch).find("RATE_LIMITED") != std::string::npos);
    // the stdio principal has no family limits
    for (int i = 0; i < 5; ++i) CHECK_FALSE(d.call_tool("gui_element_click", click, ctx_for(Principal{}, false)).is_error);
}

// ---- navigation hardening ----------------------------------------------------------------------
TEST_CASE("authorize: menu selection and navigating keys are denied with a T-code allowlist", "[auth][authz][navigation]") {
    Principal p = token("nav", {"menu", "key", "popup", "element"});
    p.tcodes = {"SE16"};

    auto d = decide(p, "gui_menu_select", {{"path", "System > Services"}}, false, std::nullopt, "SE16");
    CHECK_FALSE(d.allowed);
    CHECK(d.code == "TCODE_DENIED");
    CHECK(d.message.find("--allow-navigation") != std::string::npos);
    CHECK(decide(p, "gui_menu_list", {}, false, std::nullopt, "SE16").allowed);

    for (const char* key : {"enter", "ENTER", "f4", "F8", "0", "4", "8", "80", "81", "82", "83"}) {
        INFO(key);
        CHECK(decide(p, "gui_key_send", {{"key", key}}, false, std::nullopt, "SE16").allowed);
    }
    for (const char* key : {"f1", "f3", "F12", "f5", "f7", "shift+f3", "shift+f4", "3", "12", "15", "1"}) {
        INFO(key);
        d = decide(p, "gui_key_send", {{"key", key}}, false, std::nullopt, "SE16");
        CHECK_FALSE(d.allowed);
        CHECK(d.code == "TCODE_DENIED");
    }
    // an unknown key name is a bad argument, not a policy denial, and lists the supported names
    d = decide(p, "gui_key_send", {{"key", "bogus"}}, false, std::nullopt, "SE16");
    CHECK_FALSE(d.allowed);
    CHECK(d.code == "INVALID_ARGUMENT");
    CHECK(d.message.find("pagedown") != std::string::npos);
    CHECK(auth::tcode_safe_key(" f8 "));
    CHECK_FALSE(auth::tcode_safe_key("ctrl+/"));

    // popup close: the default (F12) and safe keys stay allowed, navigation VKeys are refused
    CHECK(decide(p, "gui_popup_close", {}, false, std::nullopt, "SE16").allowed);
    for (int v : {12, 0, 4, 8, 80, 83})
        CHECK(decide(p, "gui_popup_close", {{"vkey", v}}, false, std::nullopt, "SE16").allowed);
    for (int v : {15, 3, 1, 5, 99}) {
        d = decide(p, "gui_popup_close", {{"vkey", v}}, false, std::nullopt, "SE16");
        CHECK_FALSE(d.allowed);
        CHECK(d.code == "TCODE_DENIED");
    }
    {
        Principal b = token("pb", {"batch", "popup"});
        b.tcodes = {"SE16"};
        auto bd = decide(b, "gui_batch", {{"items", json::array({{{"tool", "gui_popup_close"}, {"arguments", {{"vkey", 15}}}}})}});
        CHECK_FALSE(bd.allowed);
        CHECK(bd.code == "TCODE_DENIED");
        CHECK(decide(b, "gui_batch", {{"items", json::array({{{"tool", "gui_popup_close"}}})}}).allowed);
        b.allow_navigation = true;
        CHECK(decide(b, "gui_popup_close", {{"vkey", 15}}, false, std::nullopt, "SE16").allowed);
    }
    // element clicks and F4 stay allowed (mitigated by the post-call re-check)
    CHECK(decide(p, "gui_element_click", {{"element", "wnd[0]/usr/btnX"}}, false, std::nullopt, "SE16").allowed);
    CHECK(decide(p, "gui_element_f4", {{"element", "wnd[0]/usr/txtA"}}, false, std::nullopt, "SE16").allowed);

    // --allow-navigation restores menus and every key
    p.allow_navigation = true;
    CHECK(decide(p, "gui_menu_select", {{"path", "System > Services"}}, false, std::nullopt, "SE16").allowed);
    CHECK(decide(p, "gui_key_send", {{"key", "f3"}}, false, std::nullopt, "SE16").allowed);
    CHECK(decide(p, "gui_key_send", {{"key", "shift+f3"}}, false, std::nullopt, "SE16").allowed);
    // ... but the open transaction must still be allowlisted
    CHECK(decide(p, "gui_menu_select", {{"path", "x"}}, false, std::nullopt, "SE38").code == "TCODE_DENIED");

    // no allowlist: completely unchanged
    Principal free = token("free", {"menu", "key"});
    CHECK(decide(free, "gui_menu_select", {{"path", "System > Services"}}).allowed);
    CHECK(decide(free, "gui_key_send", {{"key", "f3"}}).allowed);
}

TEST_CASE("authorize: batch items follow the navigation rules", "[auth][authz][batch][navigation]") {
    Principal p = token("nb", {"batch", "menu", "key"});
    p.tcodes = {"SE16"};
    auto batch = [&](const Principal& who, json items) { return decide(who, "gui_batch", {{"items", items}}); };

    auto d = batch(p, json::array({{{"tool", "gui_key_send"}, {"arguments", {{"key", "enter"}}}},
                                   {{"tool", "gui_key_send"}, {"arguments", {{"key", "f3"}}}}}));
    CHECK_FALSE(d.allowed);
    CHECK(d.code == "TCODE_DENIED");
    CHECK(d.message.find("batch item 2") != std::string::npos);
    d = batch(p, json::array({{{"tool", "gui_menu_select"}, {"arguments", {{"path", "System > Services"}}}}}));
    CHECK(d.code == "TCODE_DENIED");
    CHECK(batch(p, json::array({{{"tool", "gui_key_send"}, {"arguments", {{"key", "f8"}}}}})).allowed);

    p.allow_navigation = true;
    CHECK(batch(p, json::array({{{"tool", "gui_key_send"}, {"arguments", {{"key", "f3"}}}},
                                {{"tool", "gui_menu_select"}, {"arguments", {{"path", "a > b"}}}}})).allowed);
}

TEST_CASE("dispatcher: navigation rules reach the CLI only for allowed calls", "[auth][dispatch][navigation]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("nav", {"menu", "key", "batch"});
    p.tcodes = {"SE16"};
    f.facts = audit::SapFacts{"A4H", "001", "U", "SE16"};

    auto r = d.call_tool("gui_menu_select", {{"path", "System > Services"}}, ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("TCODE_DENIED") != std::string::npos);
    r = d.call_tool("gui_key_send", {{"key", "f3"}}, ctx_for(p));
    CHECK(r.is_error);
    CHECK(f.calls.empty());
    CHECK_FALSE(d.call_tool("gui_key_send", {{"key", "enter"}}, ctx_for(p)).is_error);
    CHECK(f.calls.size() == 1);

    // batch: the whole batch is refused up front, nothing runs
    f.calls.clear();
    r = d.call_tool("gui_batch", {{"items", json::array({{{"tool", "gui_key_send"}, {"arguments", {{"key", "enter"}}}},
                                                        {{"tool", "gui_key_send"}, {"arguments", {{"key", "f12"}}}}})}}, ctx_for(p));
    CHECK(r.is_error);
    CHECK(f.calls.empty());

    Principal nav = p;
    nav.name = "nav2";
    nav.allow_navigation = true;
    CHECK_FALSE(d.call_tool("gui_menu_select", {{"path", "System > Services"}}, ctx_for(nav)).is_error);
    CHECK_FALSE(d.call_tool("gui_key_send", {{"key", "f3"}}, ctx_for(nav)).is_error);
}

// ---- post-call transaction re-check -------------------------------------------------------------
TEST_CASE("dispatcher: leaving the allowlist during a call is flagged and blocks the next screen call", "[auth][dispatch][recheck]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("leaver", {"element", "screen", "transaction", "key"});
    p.tcodes = {"VA03"};
    f.facts = audit::SapFacts{"A4H", "001", "U", "VA03"};

    // the click navigates to SE38 while it runs
    f.handler = [&](const Argv& argv) {
        if (!argv.empty() && argv[0] == "element" && argv.size() > 1 && argv[1] == "click")
            f.facts = audit::SapFacts{"A4H", "001", "U", "SE38"};
        return ok_result();
    };
    auto r = d.call_tool("gui_element_click", {{"element", "wnd[0]/usr/btnGo"}}, ctx_for(p));
    CHECK_FALSE(r.is_error);  // the tool result itself stays as is
    CHECK(text_of(r).find("tcode_left_allowlist") != std::string::npos);
    REQUIRE(r.structured);
    CHECK((*r.structured)["tcode_left_allowlist"] == true);
    REQUIRE(f.records.size() == 1);
    CHECK(f.records[0].tcode_left_allowlist);
    CHECK(f.records[0].error_code.empty());

    // the audit record carries the additive field, and nothing else new
    audit::AuditRecord rec;
    rec.tcode_left_allowlist = true;
    CHECK(nlohmann::json::parse(audit::format_record(rec))["tcode_left_allowlist"] == true);
    CHECK_FALSE(nlohmann::json::parse(audit::format_record(audit::AuditRecord{})).contains("tcode_left_allowlist"));

    // next screen-acting call is denied even if the user navigated back meanwhile
    f.calls.clear();
    f.facts = audit::SapFacts{"A4H", "001", "U", "VA03"};
    for (const char* tool : {"gui_screen_read", "gui_element_get", "gui_key_send"}) {
        INFO(tool);
        const json args = std::string(tool) == "gui_element_get" ? json{{"element", "wnd[0]/usr/txtA"}}
                          : std::string(tool) == "gui_key_send"   ? json{{"key", "enter"}} : json::object();
        r = d.call_tool(tool, args, ctx_for(p));
        CHECK(r.is_error);
        CHECK(text_of(r).find("TCODE_DENIED") != std::string::npos);
    }
    CHECK(f.calls.empty());

    // another principal is unaffected
    Principal other = token("other", {"screen"});
    other.tcodes = {"VA03"};
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(other)).is_error);

    // a start of a NOT allowed code does not unblock (and is refused up front)
    CHECK(d.call_tool("gui_transaction_start", {{"code", "SE38"}}, ctx_for(p)).is_error);
    CHECK(d.call_tool("gui_screen_read", json::object(), ctx_for(p)).is_error);

    // an allowed gui_transaction_start that ends in an allowed transaction unblocks
    f.handler = [](const Argv&) { return ok_result(); };
    CHECK_FALSE(d.call_tool("gui_transaction_start", {{"code", "VA03"}}, ctx_for(p)).is_error);
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(p)).is_error);
}

TEST_CASE("dispatcher: an allowed start that still ends outside the allowlist keeps the block", "[auth][dispatch][recheck]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("stuck", {"screen", "transaction", "key"});
    p.tcodes = {"VA03"};
    f.facts = audit::SapFacts{"A4H", "001", "U", "VA03"};
    f.handler = [&](const Argv& argv) {
        if (argv.size() > 1 && argv[0] == "key") f.facts = audit::SapFacts{"A4H", "001", "U", "S000"};
        return ok_result();
    };
    auto r = d.call_tool("gui_key_send", {{"key", "enter"}}, ctx_for(p));
    CHECK(text_of(r).find("tcode_left_allowlist") != std::string::npos);
    // the start "works" but SAP shows S000 afterwards (e.g. the code was rewritten): still blocked, and flagged again
    r = d.call_tool("gui_transaction_start", {{"code", "VA03"}}, ctx_for(p));
    CHECK(r.structured);
    CHECK(d.call_tool("gui_screen_read", json::object(), ctx_for(p)).is_error);
    f.handler = [](const Argv&) { return ok_result(); };
    f.facts = audit::SapFacts{"A4H", "001", "U", "VA03"};
    CHECK_FALSE(d.call_tool("gui_transaction_start", {{"code", "VA03"}}, ctx_for(p)).is_error);
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(p)).is_error);
}

TEST_CASE("dispatcher: T-code denial survives provider replacement", "[auth][dispatch][recheck][provider-reload]") {
    Fixture f;
    auto policy_state = std::make_shared<SessionPolicyState>();
    auto before = f.make(write_mode());
    before->set_session_policy_state(policy_state);
    Principal p = token("reload", {"screen", "key"});
    p.tcodes = {"VA03"};
    f.facts = audit::SapFacts{"A4H", "001", "U", "VA03"};
    f.handler = [&](const Argv& argv) {
        if (argv.size() > 1 && argv[0] == "key") f.facts->transaction = "S000";
        return ok_result();
    };
    REQUIRE_FALSE(before->call_tool("gui_key_send", {{"key", "enter"}}, ctx_for(p)).is_error);
    f.facts->transaction = "VA03";
    auto after = f.make(write_mode());
    after->set_session_policy_state(policy_state);
    const auto denied = after->call_tool("gui_screen_read", json::object(), ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("TCODE_DENIED") != std::string::npos);
}

TEST_CASE("dispatcher: tokens without a T-code allowlist never re-check", "[auth][dispatch][recheck]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal free = token("free", {"screen", "element"});
    int asked = 0;
    d.set_sap_facts_provider([&](std::optional<int>) { ++asked; return std::optional<audit::SapFacts>(audit::SapFacts{"A4H", "001", "U", "SE38"}); });
    auto r = d.call_tool("gui_screen_read", json::object(), ctx_for(free));
    CHECK_FALSE(r.is_error);
    CHECK(text_of(r).find("tcode_left_allowlist") == std::string::npos);
    CHECK_FALSE((r.structured.has_value() && r.structured->contains("tcode_left_allowlist")));
    CHECK(asked == 0);
    CHECK_FALSE(f.records.back().tcode_left_allowlist);

    // the stdio principal is unrestricted as well
    Principal stdio;
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(stdio, false)).is_error);
    CHECK(asked == 0);
}

TEST_CASE("dispatcher: batch items are re-checked one by one", "[auth][dispatch][recheck][batch]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal p = token("batcher", {"batch", "element", "screen"});
    p.tcodes = {"VA03"};
    f.facts = audit::SapFacts{"A4H", "001", "U", "VA03"};
    f.handler = [&](const Argv& argv) {
        if (argv.size() > 1 && argv[0] == "element" && argv[1] == "click") f.facts = audit::SapFacts{"A4H", "001", "U", "SE38"};
        return ok_result();
    };
    const auto r = d.call_tool("gui_batch",
                               {{"items", json::array({{{"tool", "gui_element_click"}, {"arguments", {{"element", "wnd[0]/usr/btnGo"}}}},
                                                       {{"tool", "gui_screen_read"}}})},
                                {"stop_on_error", false}},
                               ctx_for(p));
    CHECK(r.is_error);  // the second item was denied
    CHECK(r.structured);
    CHECK((*r.structured)["tcode_left_allowlist"] == true);
    const std::string text = text_of(r);
    CHECK(text.find("\"tcode_left_allowlist\":true") != std::string::npos);
    CHECK(text.find("TCODE_DENIED") != std::string::npos);
    CHECK(f.calls.size() == 1);
    REQUIRE(f.records.size() >= 2);
    CHECK(f.records[0].tcode_left_allowlist);
    CHECK_FALSE(f.records[1].tcode_left_allowlist);
}

TEST_CASE("dispatcher: per-principal state is keyed by token id, not by name", "[auth][dispatch][recheck][principal-id]") {
    Fixture f;
    auto d_ptr = f.make(write_mode());
    auto& d = *d_ptr;
    Principal a = token("same-name", {"screen", "key"});
    a.id = "idA";
    a.tcodes = {"VA03"};
    f.facts = audit::SapFacts{"A4H", "001", "U", "VA03"};
    f.handler = [&](const Argv& argv) {
        if (argv.size() > 1 && argv[0] == "key") f.facts = audit::SapFacts{"A4H", "001", "U", "S000"};
        return ok_result();
    };
    CHECK(text_of(d.call_tool("gui_key_send", {{"key", "enter"}}, ctx_for(a))).find("tcode_left_allowlist") != std::string::npos);
    f.facts = audit::SapFacts{"A4H", "001", "U", "VA03"};
    CHECK(d.call_tool("gui_screen_read", json::object(), ctx_for(a)).is_error);  // A is blocked

    // the token is deleted and recreated under the same name: new id, clean state
    Principal b = a;
    b.id = "idB";
    CHECK_FALSE(d.call_tool("gui_screen_read", json::object(), ctx_for(b)).is_error);
    CHECK(d.call_tool("gui_screen_read", json::object(), ctx_for(a)).is_error);  // A stays blocked
    CHECK_FALSE(d.sticky_connection("idB").has_value());
}

TEST_CASE("dispatcher: leaving a T-code allowlist blocks only the affected SAP session", "[auth][dispatch][recheck][session-state]") {
    Fixture f;
    auto d = f.make(write_mode());
    Principal p = token("operator", {"key", "screen", "transaction"});
    p.id = "token-1";
    p.tcodes = {"VA03"};
    std::map<int, std::string> transactions{{1, "VA03"}, {2, "VA03"}};
    f.facts_for = [&](std::optional<int> connection) -> std::optional<audit::SapFacts> {
        if (!connection || !transactions.count(*connection)) return std::nullopt;
        audit::SapFacts facts{"A4H", "001", "U", transactions.at(*connection)};
        facts.connection_id = *connection;
        facts.session_identity = "session-" + std::to_string(*connection);
        return facts;
    };
    f.handler = [&](const Argv& argv) {
        if (argv.size() >= 2 && argv[0] == "key" && argv[1] == "send") transactions[1] = "SE38";
        return ok_result();
    };

    CHECK_FALSE(d->call_tool("gui_key_send", {{"key", "enter"}, {"connection", 1}}, ctx_for(p)).is_error);
    transactions[1] = "VA03";  // another actor navigated back; the block persists for session 1
    CHECK_FALSE(d->call_tool("gui_screen_read", {{"connection", 2}}, ctx_for(p)).is_error);
    CHECK(d->call_tool("gui_screen_read", {{"connection", 1}}, ctx_for(p)).is_error);

    CHECK_FALSE(d->call_tool("gui_transaction_start", {{"code", "VA03"}, {"connection", 2}}, ctx_for(p)).is_error);
    CHECK(d->call_tool("gui_screen_read", {{"connection", 1}}, ctx_for(p)).is_error);
    CHECK_FALSE(d->call_tool("gui_transaction_start", {{"code", "VA03"}, {"connection", 1}}, ctx_for(p)).is_error);
    CHECK_FALSE(d->call_tool("gui_screen_read", {{"connection", 1}}, ctx_for(p)).is_error);
}

TEST_CASE("dispatcher: mismatched live connection facts cannot authorize another session", "[auth][dispatch][session-state]") {
    Fixture f;
    auto d = f.make(write_mode());
    Principal p = token("agent", {"screen"});
    p.tcodes = {"VA03"};
    f.facts_for = [](std::optional<int>) -> std::optional<audit::SapFacts> {
        audit::SapFacts facts{"A4H", "001", "OWNER", "VA03"};
        facts.connection_id = 2;
        facts.session_identity = "session-two";
        return facts;
    };
    const auto denied = d->call_tool("gui_screen_read", {{"connection", 1}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("SESSION_TARGET_UNKNOWN") != std::string::npos);
    CHECK(f.calls.empty());
}

TEST_CASE("dispatcher: mismatched post-call facts cannot block a different SAP session", "[auth][dispatch][recheck][session-state]") {
    Fixture f;
    auto d = f.make(write_mode());
    Principal p = token("agent", {"key", "screen"});
    p.tcodes = {"VA03"};
    bool after_key = false;
    f.facts_for = [&](std::optional<int> connection) -> std::optional<audit::SapFacts> {
        const int reported = connection == 1 && after_key ? 2 : connection.value_or(0);
        audit::SapFacts facts{"A4H", "001", "OWNER", connection == 1 && after_key ? "SE38" : "VA03"};
        facts.connection_id = reported;
        facts.session_identity = "session-" + std::to_string(reported);
        return facts;
    };
    f.handler = [&](const Argv&) { after_key = true; return ok_result(); };
    CHECK_FALSE(d->call_tool("gui_key_send", {{"key", "enter"}, {"connection", 1}}, ctx_for(p)).is_error);
    CHECK_FALSE(d->call_tool("gui_screen_read", {{"connection", 2}}, ctx_for(p)).is_error);
}

TEST_CASE("dispatcher: T-code block survives a missing session identity", "[auth][dispatch][session-state]") {
    Fixture f;
    auto d = f.make(write_mode());
    Principal p = token("agent", {"key", "screen"});
    p.tcodes = {"VA03"};
    std::string transaction = "VA03";
    bool identity_missing = false;
    f.facts_for = [&](std::optional<int> connection) -> std::optional<audit::SapFacts> {
        audit::SapFacts facts{"A4H", "001", "OWNER", transaction};
        facts.connection_id = connection;
        facts.session_identity = identity_missing ? "" : "session-one|key|generation";
        return facts;
    };
    f.handler = [&](const Argv&) { transaction = "SE38"; return ok_result(); };
    CHECK_FALSE(d->call_tool("gui_key_send", {{"key", "enter"}, {"connection", 1}}, ctx_for(p)).is_error);
    transaction = "VA03";
    identity_missing = true;
    const auto denied = d->call_tool("gui_screen_read", {{"connection", 1}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("TCODE_DENIED") != std::string::npos);
    CHECK(f.calls.size() == 1);
}

TEST_CASE("dispatcher: owner SAP identity limits established session calls", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"screen"});
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "session-one";

    CHECK_FALSE(d->call_tool("gui_screen_read", {{"connection", 1}, {"no_tabs", true}}, ctx_for(p)).is_error);
    CHECK(f.calls.size() == 1);

    f.facts->user = "OTHER";
    auto denied = d->call_tool("gui_screen_read", {{"connection", 1}, {"no_tabs", true}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    CHECK(f.calls.size() == 1);
    REQUIRE_FALSE(f.records.empty());
    CHECK(f.records.back().error_code == "OWNER_IDENTITY_DENIED");

    f.facts->user.clear();
    denied = d->call_tool("gui_screen_read", {{"connection", 1}, {"no_tabs", true}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    CHECK(f.calls.size() == 1);
}

TEST_CASE("dispatcher sends an authorized bound call to its session worker", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("reader", {"screen"});
    p.read_only = true;
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 7;
    f.facts->session_identity = "/app/con[0]/ses[0]|server-key|generation";
    std::optional<WorkerCall> received;
    d->set_session_invoker([&](const WorkerCall& call) {
        received = call;
        Result result = ok_result();
        result.data = {{"connection_id", 7}};
        return result;
    });

    const auto result = d->call_tool("gui_screen_read", {{"connection", 7}, {"no_tabs", true}}, ctx_for(p));
    CHECK_FALSE(result.is_error);
    REQUIRE(received);
    CHECK(received->connection == 7);
    CHECK(received->session_identity == f.facts->session_identity);
    CHECK(received->owner_identity == "A4H/001/OWNER");
    CHECK(received->read_only);
    CHECK(std::find(received->argv.begin(), received->argv.end(), "--connection") != received->argv.end());
    CHECK(f.calls.empty());

    d->set_session_invoker([](const WorkerCall&) -> Result {
        throw std::runtime_error("worker pipe closed after submission");
    });
    const auto uncertain = d->call_tool("gui_screen_read", {{"connection", 7}, {"no_tabs", true}}, ctx_for(p));
    CHECK(uncertain.is_error);
    CHECK(text_of(uncertain).find("OUTCOME_UNKNOWN") != std::string::npos);
    CHECK(f.calls.empty());
}

TEST_CASE("required audit failure before worker send does not claim the SAP action ran", "[auth][dispatch][audit]") {
    mcp_audit_reset_failure();
    const auto blocker = std::filesystem::temp_directory_path() /
        ("ff_worker_audit_blocker_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    { std::ofstream(blocker) << "x"; }
    audit::AuditSink sink(audit::AuditConfig{audit::Mode::Required, blocker / "audit.jsonl"});
    Fixture f;
    Policy policy = write_mode();
    policy.audit_required = true;
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("reader", {"screen"});
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 7;
    f.facts->session_identity = "/app/con[0]/ses[0]|key|generation";
    bool worker_action = false;
    d->set_session_invoker_with_gate([&](const WorkerCall&, const std::function<void()>& gate) {
        CHECK_FALSE(append_serve_event(&sink, "probe", true));
        gate();
        worker_action = true;
        return ok_result();
    });
    const auto result = d->call_tool("gui_screen_read", {{"connection", 7}, {"no_tabs", true}}, ctx_for(p));
    CHECK(result.is_error);
    CHECK(text_of(result).find("AUDIT_UNAVAILABLE") != std::string::npos);
    CHECK(text_of(result).find("before the worker action") != std::string::npos);
    CHECK(text_of(result).find("carried out") == std::string::npos);
    CHECK_FALSE(worker_action);
    mcp_audit_reset_failure();
    std::error_code ec;
    std::filesystem::remove(blocker, ec);
}

TEST_CASE("dispatcher reauthenticates immediately before each worker action", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("reader", {"screen"});
    p.read_only = true;
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 7;
    f.facts->session_identity = "/app/con[0]/ses[0]|server-key|generation";
    int worker_calls = 0;
    d->set_session_invoker([&](const WorkerCall&) { ++worker_calls; return ok_result(); });
    auto ctx = ctx_for(p);
    ctx.reauthorize = [] { return false; };
    const auto revoked = d->call_tool("gui_screen_read", {{"connection", 7}, {"no_tabs", true}}, ctx);
    INFO(text_of(revoked));
    CHECK(revoked.is_error);
    CHECK(text_of(revoked).find("TOKEN_CHANGED") != std::string::npos);
    CHECK(worker_calls == 0);
    ctx.reauthorize = {};
    const auto missing = d->call_tool("gui_screen_read", {{"connection", 7}, {"no_tabs", true}}, ctx);
    INFO(text_of(missing));
    CHECK(missing.is_error);
    CHECK(text_of(missing).find("AUTH_UNAVAILABLE") != std::string::npos);
    CHECK(worker_calls == 0);
}

TEST_CASE("dispatcher screenshot retry stays in its bound worker", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    policy.max_image_bytes = 100;
    auto d = f.make(policy);
    Principal p = token("reader", {"screen"});
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 7;
    f.facts->session_identity = "/app/con[0]/ses[0]|server-key|generation";
    std::vector<WorkerCall> worker_calls;
    d->set_session_invoker([&](const WorkerCall& call) {
        worker_calls.push_back(call);
        Result result = ok_result();
        result.data = {{"screenshot", "data:image/png;base64," + std::string(4000, 'A')}};
        return result;
    });
    (void)d->call_tool("gui_screen_capture", {{"connection", 7}}, ctx_for(p));
    CHECK(worker_calls.size() == 2);
    CHECK(f.calls.empty());
    if (worker_calls.size() == 2)
        CHECK(worker_calls[0].session_identity == worker_calls[1].session_identity);
}

TEST_CASE("worker mode refuses an unrecognized session command without local fallback", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    ToolSpec spec;
    spec.def.name = "gui_screen_unknown";
    spec.def.input_schema = {{"type", "object"}, {"properties", {{"connection", {{"type", "integer"}}}}}};
    spec.family = "screen";
    spec.build_argv = [](const json&, const Policy&) {
        return Argv{"screen", "unknown", "--connection", "7"};
    };
    CommandDispatcher d([&](const Argv& argv) { f.calls.push_back(argv); return ok_result(); },
                        policy, nullptr, {spec});
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 7;
    f.facts->session_identity = "/app/con[0]/ses[0]|server-key|generation";
    d.set_sap_facts_provider([&](std::optional<int>) { return f.facts; });
    d.set_owner_session_override([](const std::string&, const std::string&) {});
    d.set_session_invoker([](const WorkerCall&) { return ok_result(); });
    Principal p = token("reader", {"screen"});
    const auto result = d.call_tool("gui_screen_unknown", {{"connection", 7}}, ctx_for(p));
    CHECK(result.is_error);
    CHECK(text_of(result).find("WORKER_UNSUPPORTED") != std::string::npos);
    CHECK(f.calls.empty());
}

TEST_CASE("dispatcher: owner check pins an implicitly resolved connection", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"screen"});
    f.facts_for = [](std::optional<int> connection) -> std::optional<audit::SapFacts> {
        if (connection && *connection != 2) return std::nullopt;
        audit::SapFacts facts{"A4H", "001", "OWNER", "VA03"};
        facts.connection_id = 2;
        facts.session_identity = "session-two|key|generation";
        return facts;
    };
    const auto read = d->call_tool("gui_screen_read", {{"no_tabs", true}}, ctx_for(p));
    CHECK_FALSE(read.is_error);
    REQUIRE(f.calls.size() == 1);
    CHECK(std::find(f.calls[0].begin(), f.calls[0].end(), "--connection") != f.calls[0].end());
    CHECK(std::find(f.calls[0].begin(), f.calls[0].end(), "2") != f.calls[0].end());
    REQUIRE(f.owner_guard_events.size() == 2);
    CHECK(f.owner_guard_events[0] == std::make_pair(std::string("session-two|key|generation"), std::string("A4H/001/OWNER")));
    CHECK(f.owner_guard_events[1] == std::make_pair(std::string(), std::string()));
}

TEST_CASE("dispatcher: unknown and foreign sessions have the same owner refusal", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"screen"});
    f.facts_for = [](std::optional<int> connection) -> std::optional<audit::SapFacts> {
        if (connection != 2) return std::nullopt;
        audit::SapFacts facts{"A4H", "001", "OTHER", "VA03"};
        facts.connection_id = 2;
        facts.session_identity = "session-two|key|generation";
        return facts;
    };
    const auto missing = d->call_tool("gui_screen_read", {{"connection", 1}, {"no_tabs", true}}, ctx_for(p));
    const auto foreign = d->call_tool("gui_screen_read", {{"connection", 2}, {"no_tabs", true}}, ctx_for(p));
    CHECK(missing.is_error);
    CHECK(foreign.is_error);
    CHECK(text_of(missing) == text_of(foreign));
    CHECK(f.calls.empty());
}

TEST_CASE("dispatcher: owner recheck withholds a result from a changed session", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"screen"});
    bool changed = false;
    f.facts_for = [&](std::optional<int>) -> std::optional<audit::SapFacts> {
        audit::SapFacts facts{"A4H", "001", changed ? "OTHER" : "OWNER", "VA03"};
        facts.connection_id = 1;
        facts.session_identity = changed ? "session-new|key|generation" : "session-old|key|generation";
        return facts;
    };
    f.handler = [&](const Argv&) {
        changed = true;
        Result result = ok_result();
        result.data = {{"secret", "PRIVATE SCREEN"}};
        return result;
    };
    const auto read = d->call_tool("gui_screen_read", {{"connection", 1}, {"no_tabs", true}}, ctx_for(p));
    CHECK(read.is_error);
    CHECK(text_of(read).find("PRIVATE SCREEN") == std::string::npos);
    CHECK(text_of(read).find("OUTCOME_UNKNOWN") != std::string::npos);
}

TEST_CASE("dispatcher: owner recheck covers screenshot retry", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    policy.max_image_bytes = 100;
    auto d = f.make(policy);
    Principal p = token("agent", {"screen"});
    int captures = 0;
    f.facts_for = [&](std::optional<int>) -> std::optional<audit::SapFacts> {
        audit::SapFacts facts{"A4H", "001", captures == 2 ? "OTHER" : "OWNER", "VA03"};
        facts.connection_id = 1;
        facts.session_identity = captures == 2 ? "new|key|generation" : "old|key|generation";
        return facts;
    };
    f.handler = [&](const Argv&) {
        ++captures;
        Result result = ok_result();
        result.data = {{"screenshot", "data:image/png;base64," + std::string(captures == 1 ? 400 : 40, 'A')},
                       {"format", "base64"}};
        return result;
    };
    const auto image = d->call_tool("gui_screen_capture", {{"connection", 1}}, ctx_for(p));
    CHECK(captures == 2);
    CHECK(image.is_error);
    CHECK(text_of(image).find("OUTCOME_UNKNOWN") != std::string::npos);
}

TEST_CASE("dispatcher: scope refusal does not inspect a SAP session", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    int lookups = 0;
    f.facts_for = [&](std::optional<int>) -> std::optional<audit::SapFacts> {
        ++lookups;
        return std::nullopt;
    };
    const auto denied = d->call_tool("gui_screen_read", {{"connection", 1}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("SCOPE_DENIED") != std::string::npos);
    CHECK(lookups == 0);
}

TEST_CASE("dispatcher: rate refusal does not inspect a SAP session", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"screen"});
    p.rate_per_minute = 1;
    int lookups = 0;
    f.facts_for = [&](std::optional<int>) -> std::optional<audit::SapFacts> {
        ++lookups;
        audit::SapFacts facts{"A4H", "001", "OWNER", "VA03"};
        facts.connection_id = 1;
        facts.session_identity = "session-one|key|generation";
        return facts;
    };
    CHECK_FALSE(d->call_tool("gui_screen_read", {{"connection", 1}, {"no_tabs", true}}, ctx_for(p)).is_error);
    const int before = lookups;
    const auto denied = d->call_tool("gui_screen_read", {{"connection", 1}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("RATE_LIMITED") != std::string::npos);
    CHECK(lookups == before);
}

TEST_CASE("dispatcher: attaching a session checks the requested SAP user", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "session-one|key|generation";
    f.handler = [](const Argv&) {
        Result result = ok_result();
        result.data = {{"connection_file_id", 1}, {"session_id", "session-one"}};
        return result;
    };
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery& query) {
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.user = query.session_id == "session-two" ? "OTHER" : "OWNER";
        return target;
    });

    auto denied = d->call_tool("gui_session_attach", {{"session_id", "session-two"}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    CHECK(f.calls.empty());
    auto revoked = ctx_for(p);
    revoked.reauthorize = [] { return false; };
    const auto changed = d->call_tool("gui_session_attach", {{"session_id", "session-one"}}, revoked);
    CHECK(changed.is_error);
    CHECK(text_of(changed).find("TOKEN_CHANGED") != std::string::npos);
    CHECK(f.calls.empty());
    CHECK_FALSE(d->call_tool("gui_session_attach", {{"session_id", "session-one"}}, ctx_for(p)).is_error);
    CHECK(f.calls.size() == 1);
    CHECK(f.attach_finalizations == std::vector<bool>{true});
    f.calls.clear();
    d->set_attach_finalize_override([&](bool accepted) {
        f.attach_finalizations.push_back(accepted);
        return false;  // saved generation was replaced by another process before commit
    });
    const auto replaced = d->call_tool("gui_session_attach", {{"session_id", "session-one"}}, ctx_for(p));
    CHECK(replaced.is_error);
    CHECK(text_of(replaced).find("OUTCOME_UNKNOWN") != std::string::npos);
    CHECK(f.attach_finalizations == std::vector<bool>{true, true});
    f.calls.clear();
    d->set_attach_finalize_override({});
    const auto no_rollback = d->call_tool("gui_session_attach", {{"session_id", "session-one"}}, ctx_for(p));
    CHECK(no_rollback.is_error);
    CHECK(f.calls.empty());
}

TEST_CASE("attach checks token limits again before writing a changed live target", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER", "B4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    p.sap_systems = {"A4H/001"};
    p.connections = {"A Logon"};
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.user = "OWNER";
        target.connection_name = "A Logon";
        return target;
    });
    bool cache_written = false;
    f.handler = [&](const Argv&) {
        audit::SapFacts changed{"B4H", "001", "OWNER", ""};
        changed.session_identity = "session-one|key|generation";
        if (!f.attach_guard || !f.attach_guard(changed, "B Logon")) {
            Result denied;
            denied.status = Result::Status::Error;
            denied.error = {{"code", "OWNER_SESSION_UNAVAILABLE"}, {"message", "session unavailable"}};
            return denied;
        }
        cache_written = true;
        Result result = ok_result();
        result.data = {{"connection_file_id", 1}, {"session_id", "session-one"}};
        return result;
    };
    const auto denied = d->call_tool("gui_session_attach", {{"session_id", "session-one"}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK_FALSE(cache_written);
    CHECK_FALSE(f.attach_guard);
}

TEST_CASE("dispatcher: owner identity gate withholds unfiltered session discovery", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session", "connection", "credentials"});
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "session-one";

    for (const char* tool : {"gui_session_list", "gui_connection_list", "gui_credentials_list"}) {
        const auto denied = d->call_tool(tool, json::object(), ctx_for(p));
        CHECK(denied.is_error);
        CHECK(text_of(denied).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    }
    const auto implicit = d->call_tool("gui_session_attach", json::object(), ctx_for(p));
    CHECK(implicit.is_error);
    CHECK(text_of(implicit).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    CHECK(f.calls.size() == 2);  // malformed session and connection listings were withheld
}

TEST_CASE("owner session listing withholds results after cancellation or verification timeout", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    f.handler = [](const Argv&) {
        Result r = ok_result();
        r.data = session_list_data();
        return r;
    };
    bool cancelled = false;
    int probes = 0;
    d->set_session_target_resolver([&](const CommandDispatcher::TargetQuery&) {
        ++probes;
        cancelled = true;
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.connection_name = "DEV1";
        target.user = "OWNER";
        return target;
    });
    auto ctx = ctx_for(p);
    ctx.cancelled = [&] { return cancelled; };
    auto result = d->call_tool("gui_session_list", json::object(), ctx);
    CHECK(result.is_error);
    CHECK(text_of(result).find("SECRETPRD") == std::string::npos);
    CHECK(probes == 1);

    cancelled = false;
    probes = 0;
    d->set_owner_listing_budget(std::chrono::milliseconds(1));
    d->set_session_target_resolver([&](const CommandDispatcher::TargetQuery&) {
        ++probes;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.connection_name = "DEV1";
        target.user = "OWNER";
        return target;
    });
    result = d->call_tool("gui_session_list", json::object(), ctx_for(p));
    CHECK(result.is_error);
    CHECK(text_of(result).find("SECRETPRD") == std::string::npos);
    CHECK(probes == 1);

    // The initial SAP enumeration is synchronous. If it returns after the budget,
    // its unverified data must still be withheld before any owner probes run.
    probes = 0;
    f.handler = [](const Argv&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Result r = ok_result();
        r.data = session_list_data();
        return r;
    };
    result = d->call_tool("gui_session_list", json::object(), ctx_for(p));
    CHECK(result.is_error);
    CHECK(text_of(result).find("SECRETPRD") == std::string::npos);
    CHECK(probes == 0);
}

TEST_CASE("owner discovery uses verified worker targets without tray COM enumeration", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    int tray_calls = 0;
    f.handler = [&](const Argv&) { ++tray_calls; return ok_result(); };
    int tray_probes = 0;
    d->set_session_target_resolver([&](const CommandDispatcher::TargetQuery&) {
        ++tray_probes;
        return auth::SessionTarget{};
    });
    d->set_owner_session_listing_provider([](std::chrono::milliseconds, const std::function<bool()>&) {
        Result r = ok_result();
        r.data = session_list_data();
        r.data["connections"][0]["sessions"][0]["verified_target"] =
            {{"system", "A4H/001"}, {"user", "OWNER"}, {"connection_name", "DEV1"}};
        r.data["connections"][0]["sessions"][1]["verified_target"] =
            {{"system", "A4H/001"}, {"user", "OTHER"}, {"connection_name", "DEV1"}};
        return r;
    });
    const auto result = d->call_tool("gui_session_list", json::object(), ctx_for(p));
    CHECK_FALSE(result.is_error);
    CHECK(everything_of(result).find("/app/con[0]/ses[0]") != std::string::npos);
    CHECK(everything_of(result).find("/app/con[0]/ses[1]") == std::string::npos);
    CHECK(everything_of(result).find("SECRETPRD") == std::string::npos);
    CHECK(tray_calls == 0);
    CHECK(tray_probes == 0);
}

TEST_CASE("owner connection listing exposes only verified live owner rows", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"connection", "system"});
    p.sap_systems = {"A4H/001"};
    p.connections = {"Allowed*"};
    f.handler = [](const Argv&) {
        Result r = ok_result();
        r.data = {{"connections", json::array({
            {{"id", 1}, {"session_id", "session-one"}, {"server_session_key", "key-one"},
             {"cache_generation", "gen-one"}, {"description", "SAVED_SECRET"}, {"file", "SECRET_FILE"}, {"valid", false}},
            {{"id", 2}, {"session_id", "session-two"}, {"server_session_key", "key-two"},
             {"cache_generation", "gen-two"}, {"description", "FOREIGN_SECRET"}, {"valid", true}},
            {{"id", 3}, {"session_id", "session-three"}, {"server_session_key", "key-three"},
             {"cache_generation", "gen-three"}, {"description", "STALE_SECRET"}, {"valid", true}},
            {{"id", 4}, {"session_id", "session-four"}, {"server_session_key", "key-four"},
             {"cache_generation", "gen-four"}, {"description", "NARROW_SECRET"}, {"valid", true}}
        })}, {"count", 4}};
        return r;
    };
    f.facts_for = [](std::optional<int> id) -> std::optional<audit::SapFacts> {
        if (!id) return std::nullopt;
        audit::SapFacts facts{"A4H", "001", *id == 2 ? "OTHER" : "OWNER", "VA03"};
        facts.connection_id = *id;
        facts.session_identity = "session-" + std::string(*id == 1 ? "one|key-one|gen-one" :
            *id == 2 ? "two|key-two|gen-two" : *id == 3 ? "three|key-three|old-gen" :
            "four|key-four|gen-four");
        return facts;
    };
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery& q) {
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.user = q.connection == 2 ? "OTHER" : "OWNER";
        target.connection_name = q.connection == 4 ? "Other Logon" : "Allowed Logon";
        return target;
    });
    const auto result = d->call_tool("gui_connection_list", json::object(), ctx_for(p));
    INFO(text_of(result));
    REQUIRE_FALSE(result.is_error);
    const auto visible = text_of(result);
    CHECK(visible.find("Allowed Logon") != std::string::npos);
    CHECK(visible.find("session-one") != std::string::npos);
    for (const char* secret : {"SAVED_SECRET", "SECRET_FILE", "FOREIGN_SECRET", "STALE_SECRET",
                               "NARROW_SECRET", "session-two", "session-three", "session-four", "key-one", "gen-one"})
        CHECK(visible.find(secret) == std::string::npos);
    CHECK(visible.find("\"count\":1") != std::string::npos);
    CHECK(f.calls.size() == 1);
    const auto doctor = d->call_tool("gui_doctor", json::object(), ctx_for(p));
    REQUIRE_FALSE(doctor.is_error);
    const auto diagnostics = text_of(doctor);
    CHECK(diagnostics.find("owner_connections") != std::string::npos);
    CHECK(diagnostics.find("Allowed Logon") == std::string::npos); // count-only diagnostic
    for (const char* secret : {"SAVED_SECRET", "SECRET_FILE", "FOREIGN_SECRET", "STALE_SECRET",
                               "NARROW_SECRET", "session-two", "session-three", "session-four", "key-one", "gen-one"})
        CHECK(diagnostics.find(secret) == std::string::npos);
    CHECK(f.calls.size() == 2);
    const auto local_doctor = d->call_tool("gui_doctor", json::object(), ctx_for(p, false));
    CHECK(local_doctor.is_error);  // local doctor still reports global desktop state
    CHECK(text_of(local_doctor).find("CONNECTION_DENIED") != std::string::npos);
    CHECK(f.calls.size() == 2);
    const auto cleanup = d->call_tool("gui_connection_list", {{"cleanup", true}}, ctx_for(p));
    CHECK(cleanup.is_error);
    CHECK(f.calls.size() == 2);
    f.handler = [](const Argv&) {
        Result r = ok_result();
        r.data = {{"connections", json::array()}, {"count", 0}};
        return r;
    };
    const auto empty = d->call_tool("gui_doctor", json::object(), ctx_for(p));
    REQUIRE_FALSE(empty.is_error);
    CHECK(text_of(empty).find("No allowed SAP connection is ready") != std::string::npos);
    CHECK(text_of(empty).find("\"overall_health\":\"warning\"") != std::string::npos);
    f.handler = [](const Argv&) {
        Result r = ok_result();
        r.data = {{"connections", "FOREIGN_SECRET"}, {"count", 1}};
        return r;
    };
    const auto malformed = d->call_tool("gui_doctor", json::object(), ctx_for(p));
    CHECK(malformed.is_error);
    CHECK(text_of(malformed).find("FOREIGN_SECRET") == std::string::npos);
    f.handler = [](const Argv&) {
        Result r;
        r.status = Result::Status::Error;
        r.error = {{"code", "ENUMERATION_FAILED"}, {"message", "FOREIGN_SECRET"}};
        return r;
    };
    const auto failed = d->call_tool("gui_doctor", json::object(), ctx_for(p));
    CHECK(failed.is_error);
    CHECK(text_of(failed).find("FOREIGN_SECRET") == std::string::npos);
}

TEST_CASE("dispatcher: owner identity gate does not trust an existing session to authorize launch or login", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "session-one";
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.user = "OWNER";
        return target;
    });
    for (const char* tool : {"gui_session_launch", "gui_session_login"}) {
        const json args = std::string(tool) == "gui_session_launch" ? json{{"name", "DEV"}} : json{{"connection", 1}};
        const auto denied = d->call_tool(tool, args, ctx_for(p));
        CHECK(denied.is_error);
        CHECK(text_of(denied).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    }
    CHECK(f.calls.empty());
}

TEST_CASE("owner launch verifies a new prelogin window before exposing its saved ID", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    p.sap_systems = {"A4H/001"};
    p.connections = {"A4H Logon"};
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.connection_name = "A4H Logon";
        return target;
    });
    bool wrong_system = true;
    bool saved = false;
    f.handler = [&](const Argv&) {
        audit::SapFacts live{wrong_system ? "PRD" : "A4H", "000", "", "S000"};
        if (!f.launch_guard || !f.launch_guard(live, "A4H Logon")) {
            Result denied;
            denied.status = Result::Status::Error;
            denied.error = {{"code", "OWNER_SESSION_UNAVAILABLE"}, {"message", "launch unavailable"}};
            return denied;
        }
        saved = true;
        f.facts = audit::SapFacts{"A4H", "000", "", "S000"};
        f.facts->connection_id = 7;
        f.facts->session_identity = "session-seven||generation";
        Result result = ok_result();
        result.data = {{"connection_file_id", 7}, {"session_id", "session-seven"}};
        return result;
    };
    auto denied = d->call_tool("gui_session_launch", {{"name", "A4H Logon"}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK_FALSE(saved);
    wrong_system = false;
    auto allowed = d->call_tool("gui_session_launch", {{"name", "A4H Logon"}}, ctx_for(p));
    INFO(text_of(allowed));
    CHECK_FALSE(allowed.is_error);
    CHECK(saved);
    CHECK_FALSE(f.launch_guard);
}

TEST_CASE("multi-user launch does not inherit an existing logon entry's SAP user", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/DEVELOPER", "A4H/001/SECOND"};
    auto d = f.make(policy);
    Principal second = token("second", {"session"});
    second.id = "second-token";
    second.sap_identities = {"A4H/001/SECOND"};
    second.sap_systems = {"A4H/001"};
    second.connections = {"Bigfox"};
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget existing;
        existing.system = "A4H/001";
        existing.user = "DEVELOPER";
        existing.connection_name = "Bigfox";
        return existing;
    });
    f.handler = [&](const Argv&) {
        audit::SapFacts pre{"A4H", "000", "", "S000"};
        REQUIRE(f.launch_guard);
        REQUIRE(f.launch_guard(pre, "Bigfox"));
        f.facts = pre;
        f.facts->connection_id = 7;
        f.facts->session_identity = "session-seven|key|generation";
        Result result = ok_result();
        result.data = {{"connection_file_id", 7}, {"session_id", "session-seven"}};
        return result;
    };
    const auto opened = d->call_tool("gui_session_launch", {{"name", "Bigfox"}}, ctx_for(second));
    INFO(text_of(opened));
    CHECK_FALSE(opened.is_error);
    CHECK(f.calls.size() == 1);
}

TEST_CASE("owner launch with login checks credential identity before SAP submission", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    p.sap_systems = {"A4H/001"};
    p.connections = {"A4H Logon"};
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.connection_name = "A4H Logon";
        return target;
    });
    bool wrong_credential = true;
    bool typed = false;
    f.handler = [&](const Argv&) {
        const auto denied = [] {
            Result result;
            result.status = Result::Status::Error;
            result.error = {{"code", "OWNER_SESSION_UNAVAILABLE"}, {"message", "login denied"}};
            return result;
        };
        audit::SapFacts pre{"A4H", "000", "", "S000"};
        if (!f.launch_guard || !f.launch_guard(pre, "A4H Logon")) return denied();
        audit::SapFacts credential = pre;
        credential.client = "001";
        credential.user = wrong_credential ? "OTHER" : "OWNER";
        credential.connection_id = 7;
        credential.session_identity = "session-seven||generation";
        if (!f.login_guard || !f.login_guard(credential, "A4H Logon"))
            return denied();
        typed = true;
        f.facts = credential;
        f.facts->connection_id = 7;
        f.facts->session_identity = "session-seven|key|generation";
        Result result = ok_result();
        result.data = {{"connection_file_id", 7}, {"session_id", "session-seven"}};
        return result;
    };
    const json args = {{"name", "A4H Logon"}, {"login", true}};
    CHECK(d->call_tool("gui_session_launch", args, ctx_for(p)).is_error);
    CHECK_FALSE(typed);
    wrong_credential = false;
    const auto allowed = d->call_tool("gui_session_launch", args, ctx_for(p));
    INFO(text_of(allowed));
    CHECK_FALSE(allowed.is_error);
    CHECK(typed);
    CHECK_FALSE(f.launch_guard);
    CHECK_FALSE(f.login_guard);
}

TEST_CASE("owner launch rolls back its saved generation when postcheck fails", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    p.connections = {"A4H Logon"};
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.connection_name = "A4H Logon";
        return target;
    });
    f.handler = [&](const Argv&) {
        audit::SapFacts pre{"A4H", "", "", ""};
        REQUIRE(f.launch_guard);
        REQUIRE(f.launch_guard(pre, "A4H Logon"));
        f.facts = audit::SapFacts{"PRD", "100", "OTHER", ""};
        f.facts->connection_id = 7;
        f.facts->session_identity = "session-seven|key|generation";
        Result result = ok_result();
        result.data = {{"connection_file_id", 7}, {"session_id", "session-seven"}};
        return result;
    };
    const auto outcome = d->call_tool("gui_session_launch", {{"name", "A4H Logon"}}, ctx_for(p));
    CHECK(outcome.is_error);
    CHECK(text_of(outcome).find("OUTCOME_UNKNOWN") != std::string::npos);
    CHECK(f.launch_rollbacks == std::vector<int>{7});
}

TEST_CASE("owner launch keeps a verified prelogin connection after credential refusal", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    p.connections = {"A4H Logon"};
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.connection_name = "A4H Logon";
        return target;
    });
    f.handler = [&](const Argv&) {
        audit::SapFacts pre{"A4H", "", "", ""};
        REQUIRE(f.launch_guard);
        REQUIRE(f.launch_guard(pre, "A4H Logon"));
        f.facts = pre;
        f.facts->connection_id = 7;
        f.facts->session_identity = "session-seven||generation";
        Result result;
        result.status = Result::Status::Error;
        result.error = {{"code", "OWNER_SESSION_UNAVAILABLE"}, {"message", "credential denied"},
                        {"launch", {{"connection_file_id", 7}, {"session_id", "session-seven"}}}};
        return result;
    };
    const auto outcome = d->call_tool("gui_session_launch",
        {{"name", "A4H Logon"}, {"login", true}}, ctx_for(p));
    CHECK(outcome.is_error);
    CHECK(text_of(outcome).find("LOGIN_NOT_COMPLETED") != std::string::npos);
    CHECK(text_of(outcome).find("connection_file_id") != std::string::npos);
    CHECK(f.launch_rollbacks.empty());
}

TEST_CASE("owner login checks the credential identity before touching SAP", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    auto probe_gate = std::make_shared<std::shared_timed_mutex>();
    d->set_control_probe_gate(probe_gate);
    std::string reserved_lane;
    bool unrelated_probe_can_run_while_login_waits = false;
    d->set_login_lane_reserver([&](const std::string& lane, const std::function<bool()>&) -> std::shared_ptr<void> {
        reserved_lane = lane;
        std::thread probe([&] {
            unrelated_probe_can_run_while_login_waits =
                probe_gate->try_lock_shared_for(std::chrono::milliseconds(30));
            if (unrelated_probe_can_run_while_login_waits) probe_gate->unlock_shared();
        });
        probe.join();
        return std::make_shared<int>(1);
    });
    Principal p = token("agent", {"session"});
    p.sap_systems = {"A4H/001"};
    p.connections = {"A Logon"};
    f.facts = audit::SapFacts{"A4H", "", "", ""};
    f.facts->connection_id = 1;
    f.facts->session_identity = "session-one||generation";
    d->set_session_target_resolver([&](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.system = f.facts->system + (f.facts->client.empty() ? "" : "/" + f.facts->client);
        target.user = f.facts->user;
        target.connection_name = "A Logon";
        return target;
    });
    bool typed = false;
    bool wrong_credential = true;
    f.handler = [&](const Argv&) {
        audit::SapFacts anticipated = *f.facts;
        anticipated.client = "001";
        anticipated.user = wrong_credential ? "OTHER" : "OWNER";
        if (!f.login_guard || !f.login_guard(anticipated, "A Logon")) {
            Result denied;
            denied.status = Result::Status::Error;
            denied.error = {{"code", "OWNER_SESSION_UNAVAILABLE"}, {"message", "login unavailable"}};
            return denied;
        }
        typed = true;
        f.facts->client = "001";
        f.facts->user = "OWNER";
        f.facts->session_identity = "session-one|newkey|newgeneration";
        Result result = ok_result();
        result.data = {{"connection_id", 1}, {"session_id", "session-one"}};
        return result;
    };
    const auto denied = d->call_tool("gui_session_login", {{"connection", 1}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK_FALSE(typed);
    wrong_credential = false;
    const auto success = d->call_tool("gui_session_login", {{"connection", 1}}, ctx_for(p));
    INFO(text_of(success));
    CHECK_FALSE(success.is_error);
    CHECK(typed);
    CHECK(reserved_lane == "window:session-one");
    CHECK(unrelated_probe_can_run_while_login_waits);
    CHECK_FALSE(f.login_guard);
}

TEST_CASE("owner login refuses before invoking SAP when its lane cannot be reserved", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    f.facts = audit::SapFacts{"A4H", "", "", ""};
    f.facts->connection_id = 1;
    f.facts->session_identity = "session-one||generation";
    auto denied = d->call_tool("gui_session_login", {{"connection", 1}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    CHECK(f.calls.empty());
    d->set_login_lane_reserver([](const std::string&, const std::function<bool()>&) -> std::shared_ptr<void> { return {}; });
    denied = d->call_tool("gui_session_login", {{"connection", 1}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("SESSION_ROUTE_UNAVAILABLE") != std::string::npos);
    CHECK(f.calls.empty());
}

TEST_CASE("owner login reports an unknown outcome when post-login facts cannot be verified", "[auth][dispatch][owner-identity]") {
    for (const bool missing_facts : {true, false}) {
        Fixture f;
        Policy policy = write_mode();
        policy.owner_sap_identities = {"A4H/001/OWNER"};
        auto d = f.make(policy);
        d->set_login_lane_reserver([](const std::string&, const std::function<bool()>&) -> std::shared_ptr<void> {
            return std::make_shared<int>(1);
        });
        Principal p = token("agent", {"session"});
        f.facts = audit::SapFacts{"A4H", "", "", ""};
        f.facts->connection_id = 1;
        f.facts->session_identity = "session-one||generation";
        d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
            auth::SessionTarget target;
            target.system = "A4H/001";
            target.user = "OWNER";
            target.connection_name = "A Logon";
            return target;
        });
        bool submitted = false;
        f.handler = [&](const Argv&) {
            audit::SapFacts anticipated = *f.facts;
            anticipated.client = "001";
            anticipated.user = "OWNER";
            REQUIRE(f.login_guard);
            REQUIRE(f.login_guard(anticipated, "A Logon"));
            submitted = true;
            if (missing_facts) {
                f.facts.reset();
            } else {
                f.facts->client = "001";
                f.facts->user = "OTHER";
                f.facts->session_identity = "session-one|newkey|newgeneration";
            }
            Result result = ok_result();
            result.data = {{"connection_id", 1}, {"session_id", "session-one"}};
            return result;
        };
        const auto outcome = d->call_tool("gui_session_login", {{"connection", 1}}, ctx_for(p));
        INFO("missing_facts=" << missing_facts << ": " << text_of(outcome));
        CHECK(submitted);
        CHECK(outcome.is_error);
        CHECK(text_of(outcome).find("OUTCOME_UNKNOWN") != std::string::npos);
        CHECK(text_of(outcome).find("before retrying") != std::string::npos);
        CHECK_FALSE(f.login_guard);
    }
}

TEST_CASE("dispatcher: attach result is withheld if the selected SAP user changes during attach", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.user = "OWNER";
        return target;
    });
    f.handler = [](const Argv&) {
        Result result = ok_result();
        result.data = {{"connection_file_id", 2}, {"session_id", "session-two"}, {"user", "OTHER"}};
        return result;
    };
    f.facts_for = [](std::optional<int> connection) -> std::optional<audit::SapFacts> {
        if (connection != 2) return std::nullopt;
        audit::SapFacts facts{"A4H", "001", "OTHER", "VA03"};
        facts.connection_id = 2;
        facts.session_identity = "session-two|key|generation";
        return facts;
    };
    const auto denied = d->call_tool("gui_session_attach", {{"session_id", "session-two"}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    CHECK(text_of(denied).find("OTHER") == std::string::npos);
    CHECK_FALSE(d->sticky_connection(p.id).has_value());
    CHECK(f.attach_finalizations == std::vector<bool>{false});
}

TEST_CASE("dispatcher: session discovery shows only sessions of the configured SAP owner", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    f.handler = [](const Argv&) {
        Result result = ok_result();
        result.data = {{"connections", json::array({{{"id", "/app/con[0]"}, {"description", "DEV"},
                      {"connection_string", "secret.example"},
                      {"sessions", json::array({{{"id", "allowed"}, {"active_window_title", "Allowed"}},
                                                {{"id", "denied"}, {"active_window_title", "SECRET"}}})},
                      {"session_count", 2}}})}, {"total_connections", 1}, {"total_sessions", 2}};
        return result;
    };
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery& query) {
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.user = query.session_id == "allowed" ? "OWNER" : "OTHER";
        return target;
    });
    const auto listed = d->call_tool("gui_session_list", json::object(), ctx_for(p));
    CHECK_FALSE(listed.is_error);
    const auto content = text_of(listed);
    CHECK(content.find("allowed") != std::string::npos);
    CHECK(content.find("denied") == std::string::npos);
    CHECK(content.find("SECRET") == std::string::npos);
    CHECK(content.find("secret.example") == std::string::npos);
}

TEST_CASE("dispatcher: owner session discovery also honors the token SAP system", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER", "QAS/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    p.sap_systems = {"A4H/001"};
    p.sap_identities = {"A4H/001/OWNER"};
    f.handler = [](const Argv&) {
        Result result = ok_result();
        result.data = {{"connections", json::array({
            {{"id", "con-a"}, {"description", "A4H"}, {"sessions", json::array({{{"id", "allowed"}, {"active_window_title", "Allowed"}}})}},
            {{"id", "con-q"}, {"description", "QAS"}, {"sessions", json::array({{{"id", "hidden"}, {"active_window_title", "SECRET"}}})}}
        })}, {"total_connections", 2}, {"total_sessions", 2}};
        return result;
    };
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery& query) {
        auth::SessionTarget target;
        target.system = query.session_id == "allowed" ? "A4H/001" : "QAS/001";
        target.user = "OWNER";
        return target;
    });
    const auto listed = d->call_tool("gui_session_list", json::object(), ctx_for(p));
    CHECK_FALSE(listed.is_error);
    const auto content = text_of(listed);
    CHECK(content.find("allowed") != std::string::npos);
    CHECK(content.find("hidden") == std::string::npos);
    CHECK(content.find("SECRET") == std::string::npos);
    CHECK(content.find("QAS") == std::string::npos);
}

TEST_CASE("dispatcher: two SAP users on one tray are separated by token identity", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/ALICE", "A4H/001/BOB"};
    auto d = f.make(policy);
    f.handler = [](const Argv&) {
        Result result = ok_result();
        result.data = {{"connections", json::array({{{"description", "Bigfox"},
            {"sessions", json::array({{{"id", "alice-window"}}, {{"id", "bob-window"}}})}}})},
            {"total_connections", 1}, {"total_sessions", 2}};
        return result;
    };
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery& query) {
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.user = query.session_id == "alice-window" ? "ALICE" : "BOB";
        target.connection_name = "Bigfox";
        return target;
    });
    Principal caller = token("alice-client", {"session"});
    const auto unbound = d->call_tool("gui_session_list", json::object(), ctx_for(caller));
    CHECK(unbound.is_error);
    CHECK(text_of(unbound).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    caller.sap_identities = {"A4H/001/ALICE"};
    const auto alice = d->call_tool("gui_session_list", json::object(), ctx_for(caller));
    REQUIRE_FALSE(alice.is_error);
    CHECK(text_of(alice).find("alice-window") != std::string::npos);
    CHECK(text_of(alice).find("bob-window") == std::string::npos);
    caller.sap_identities = {"A4H/001/BOB"};
    const auto bob = d->call_tool("gui_session_list", json::object(), ctx_for(caller));
    REQUIRE_FALSE(bob.is_error);
    CHECK(text_of(bob).find("alice-window") == std::string::npos);
    CHECK(text_of(bob).find("bob-window") != std::string::npos);
}

TEST_CASE("a client token cannot read, lease, or attach another SAP user's window", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/ALICE", "A4H/001/BOB"};
    auto d = f.make(policy);
    Principal alice = token("alice-client", {"session", "screen"});
    alice.all_scopes = true;
    alice.id = "alice-token";
    alice.sap_identities = {"A4H/001/ALICE"};
    f.facts = audit::SapFacts{"A4H", "001", "BOB", "S000"};
    f.facts->connection_id = 7;
    f.facts->session_identity = "bob-window|key|generation";
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.user = "BOB";
        target.connection_name = "Bigfox";
        return target;
    });
    for (const auto& [tool, args] : std::vector<std::pair<std::string, json>>{
             {"gui_screen_read", {{"connection", 7}, {"no_tabs", true}}},
             {"gui_session_lease", {{"connection", 7}, {"action", "acquire"}}},
             {"gui_session_attach", {{"session_id", "bob-window"}}}}) {
        const auto result = d->call_tool(tool, args, ctx_for(alice));
        INFO(tool << ": " << text_of(result));
        CHECK(result.is_error);
        CHECK(text_of(result).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    }
    CHECK(f.calls.empty());
}

TEST_CASE("multi-user prelogin window is bound to its launching token", "[auth][dispatch][owner-identity][prelogin]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/ALICE", "A4H/001/BOB"};
    auto d = f.make(policy);
    Principal alice = token("alice-client", {"session"});
    alice.id = "alice-token";
    alice.sap_identities = {"A4H/001/ALICE"};
    alice.connections = {"Bigfox"};
    alice.sap_systems = {"A4H/001"};
    d->set_session_target_resolver([&](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        if (f.facts && !f.facts->user.empty()) {
            target.system = f.facts->system + "/" + f.facts->client;
            target.user = f.facts->user;
        }
        target.connection_name = "Bigfox";
        return target;
    });
    d->set_login_lane_reserver([](const std::string&, const std::function<bool()>&) -> std::shared_ptr<void> {
        return std::make_shared<int>(1);
    });
    f.handler = [&](const Argv& argv) {
        if (fairyfly::command_table::command_of_argv(argv) == "session login") {
            audit::SapFacts anticipated = *f.facts;
            anticipated.client = "001";
            anticipated.user = "ALICE";
            REQUIRE(f.login_guard);
            REQUIRE(f.login_guard(anticipated, "Bigfox"));
            f.facts->client = "001";
            f.facts->user = "ALICE";
            f.facts->session_identity = "session-seven|key|new-generation";
            return ok_result();
        }
        audit::SapFacts pre{"A4H", "000", "", "S000"};
        REQUIRE(f.launch_guard);
        REQUIRE(f.launch_guard(pre, "Bigfox"));
        f.facts = pre;
        f.facts->connection_id = 7;
        f.facts->session_identity = "session-seven||generation";
        Result result = ok_result();
        result.data = {{"connection_file_id", 7}, {"session_id", "session-seven"}};
        return result;
    };
    const auto launch = d->call_tool("gui_session_launch", {{"name", "Bigfox"}}, ctx_for(alice));
    CHECK_FALSE(launch.is_error);
    CHECK(f.launch_rollbacks.empty());
    const auto before_login = f.calls.size();
    Principal bob = token("bob-client", {"session"});
    bob.id = "bob-token";
    bob.sap_identities = {"A4H/001/BOB"};
    bob.connections = {"Bigfox"};
    const auto login = d->call_tool("gui_session_login", {{"connection", 7}, {"credential", "Bigfox-Bob"}}, ctx_for(bob));
    CHECK(login.is_error);
    CHECK(text_of(login).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    CHECK(f.calls.size() == before_login);
    const auto owner_login = d->call_tool("gui_session_login",
        {{"connection", 7}, {"credential", "Bigfox-Alice"}}, ctx_for(alice));
    INFO(text_of(owner_login));
    CHECK_FALSE(owner_login.is_error);
    CHECK(f.calls.size() == before_login + 1);
}

TEST_CASE("multi-user launch refuses a full prelogin claim table before opening SAP", "[auth][dispatch][owner-identity][prelogin]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/ALICE", "A4H/001/BOB"};
    auto d = f.make(policy);
    auto routing = std::make_shared<SessionRoutingState>();
    for (int i = 0; i < 64; ++i)
        REQUIRE(routing->bind_prelogin("old-token-" + std::to_string(i / 8), i,
                                       "window-" + std::to_string(i) + "|key|generation"));
    d->set_routing_state(routing);
    Principal alice = token("alice-client", {"session"});
    alice.id = "alice-token";
    alice.sap_identities = {"A4H/001/ALICE"};
    alice.connections = {"Bigfox"};
    const auto denied = d->call_tool("gui_session_launch", {{"name", "Bigfox"}}, ctx_for(alice));
    CHECK(denied.is_error);
    CHECK(text_of(denied).find("OWNER_SESSION_UNAVAILABLE") != std::string::npos);
    CHECK(f.calls.empty());
}

TEST_CASE("only the launching token can close its exact prelogin window", "[auth][dispatch][owner-identity][prelogin]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/ALICE", "A4H/001/BOB"};
    auto d = f.make(policy);
    auto routing = std::make_shared<SessionRoutingState>();
    REQUIRE(routing->bind_prelogin("alice-token", 7, "session-seven|key|generation"));
    d->set_routing_state(routing);
    audit::SapFacts pre{"A4H", "000", "", "S000"};
    pre.connection_id = 7;
    pre.session_identity = "session-seven|key|generation";
    f.facts = pre;
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.connection_name = "Bigfox";
        return target;
    });
    Principal alice = token("alice-client", {"session"});
    alice.id = "alice-token";
    alice.sap_identities = {"A4H/001/ALICE"};
    alice.connections = {"Bigfox"};
    alice.sap_systems = {"A4H/001"};
    Principal bob = alice;
    bob.id = "bob-token";
    bob.sap_identities = {"A4H/001/BOB"};
    const json close = {{"connection", 7}, {"close_session", true}};
    const auto other = d->call_tool("gui_session_disconnect", close, ctx_for(bob));
    CHECK(other.is_error);
    CHECK(f.calls.empty());
    f.facts->session_identity = "session-seven|key|replacement";
    const auto reused = d->call_tool("gui_session_disconnect", close, ctx_for(alice));
    CHECK(reused.is_error);
    CHECK(f.calls.empty());
    f.facts->session_identity = pre.session_identity;
    f.handler = [&](const Argv&) {
        REQUIRE_FALSE(f.owner_guard_events.empty());
        CHECK(f.owner_guard_events.back().first == pre.session_identity);
        Result result = ok_result();
        result.data = {{"file_deleted", true}};
        return result;
    };
    const auto closed = d->call_tool("gui_session_disconnect", close, ctx_for(alice));
    INFO(text_of(closed));
    CHECK_FALSE(closed.is_error);
    CHECK(f.calls.size() == 1);
    CHECK_FALSE(routing->owns_prelogin(alice.id, 7, pre.session_identity));
}

TEST_CASE("keyless owner guard requires the same live prelogin window", "[auth][prelogin]") {
    cli::Connection saved{};
    saved.id = 7;
    saved.session_id = "/app/con[0]/ses[0]";
    saved.cache_generation = "generation";
    audit::SapFacts live{"A4H", "000", "", "S000"};
    live.connection_id = 7;
    live.session_identity = saved.session_id + "||generation|hwnd:12345";
    CHECK(cli::owner_bound_session_matches(saved, live, live.session_identity, "A4H/000/"));
    CHECK_FALSE(cli::owner_bound_session_matches(saved, live, saved.session_id + "||generation|hwnd:999", "A4H/000/"));
    live.user = "ALICE";
    CHECK_FALSE(cli::owner_bound_session_matches(saved, live, live.session_identity, "A4H/000/ALICE"));
    live.user.clear();
    live.transaction = "SE80";
    CHECK_FALSE(cli::owner_bound_session_matches(saved, live, live.session_identity, "A4H/000/"));
    live.transaction = "S000";
    live.session_identity = saved.session_id + "||generation";
    CHECK_FALSE(cli::owner_bound_session_matches(saved, live, live.session_identity, "A4H/000/"));
    saved.server_session_key = "server-key";
    live.user = "ALICE";
    live.client = "001";
    live.session_identity = saved.session_id + "|server-key|generation";
    CHECK(cli::owner_bound_session_matches(saved, live, live.session_identity, "A4H/001/ALICE"));
}

TEST_CASE("dispatcher: owner session discovery does not forward stale window metadata", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    f.handler = [](const Argv&) {
        Result result = ok_result();
        result.data = {{"connections", json::array({{
            {"id", "stale-connection"}, {"description", "stale-name"},
            {"sessions", json::array({{{"id", "reused-id"}, {"active_window_title", "PRIVATE PRIOR WINDOW"}}})}
        }})}, {"total_connections", 1}, {"total_sessions", 1}};
        return result;
    };
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.user = "OWNER";
        target.connection_name = "current-name";
        return target;
    });
    const auto listed = d->call_tool("gui_session_list", json::object(), ctx_for(p));
    CHECK_FALSE(listed.is_error);
    const auto content = text_of(listed);
    CHECK(content.find("reused-id") != std::string::npos);
    CHECK(content.find("PRIVATE PRIOR WINDOW") == std::string::npos);
    CHECK(content.find("stale-name") == std::string::npos);
    CHECK(content.find("stale-connection") == std::string::npos);
    CHECK(content.find("current-name") != std::string::npos);
}

TEST_CASE("dispatcher: owner discovery groups windows of one GUI connection", "[auth][dispatch][owner-identity]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    Principal p = token("agent", {"session"});
    f.handler = [](const Argv&) {
        Result result = ok_result();
        result.data = {{"connections", json::array({{{"id", "con-one"}, {"description", "DEV"},
            {"sessions", json::array({{{"id", "session-one"}}, {{"id", "session-two"}}})}}})},
            {"total_connections", 1}, {"total_sessions", 2}};
        return result;
    };
    d->set_session_target_resolver([](const CommandDispatcher::TargetQuery&) {
        auth::SessionTarget target;
        target.system = "A4H/001";
        target.user = "OWNER";
        target.connection_name = "DEV";
        return target;
    });
    const auto listed = d->call_tool("gui_session_list", json::object(), ctx_for(p));
    REQUIRE_FALSE(listed.is_error);
    REQUIRE(listed.structured.has_value());
    const auto& data = listed.structured->at("data");
    CHECK(data.at("total_connections") == 1);
    CHECK(data.at("total_sessions") == 2);
    CHECK(data.at("connections").size() == 1);
    CHECK(data.at("connections").at(0).at("sessions").size() == 2);
}

TEST_CASE("dispatcher: owner session navigation requires a live lease held by this token", "[auth][dispatch][lease]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    auto leases = std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "lease-secret"; });
    d->set_session_leases(leases);
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "ses-1|server-key|generation-1";
    Principal a = token("agent-a", {"session", "session.lease", "element"});
    a.id = "token-a";
    Principal b = token("agent-b", {"session", "session.lease", "element"});
    b.id = "token-b";
    const json click = {{"element", "wnd[0]/usr/btnX"}, {"connection", 1}};

    const auto missing = d->call_tool("gui_element_click", click, ctx_for(a));
    CHECK(missing.is_error);
    CHECK(text_of(missing).find("LEASE_REQUIRED") != std::string::npos);
    CHECK(f.calls.empty());

    const auto acquired = d->call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(a));
    REQUIRE_FALSE(acquired.is_error);
    REQUIRE(acquired.structured.has_value());
    const std::string id = acquired.structured->at("lease_id").get<std::string>();
    CHECK(id == "lease-secret");
    CHECK(f.calls.empty());
    CHECK(d->call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(b)).is_error);

    json allowed = click;
    allowed["lease_id"] = id;
    CHECK_FALSE(d->call_tool("gui_element_click", allowed, ctx_for(a)).is_error);
    CHECK(d->call_tool("gui_element_click", allowed, ctx_for(b)).is_error);
    REQUIRE(f.calls.size() == 1);
    CHECK(std::find(f.calls[0].begin(), f.calls[0].end(), id) == f.calls[0].end());
    CHECK(std::none_of(f.records.begin(), f.records.end(), [&](const McpCallRecord& record) {
        return std::find(record.argv.begin(), record.argv.end(), id) != record.argv.end();
    }));
    f.facts->session_identity = "ses-1|server-key|generation-2";
    CHECK(d->call_tool("gui_element_click", allowed, ctx_for(a)).is_error);
    f.facts->session_identity = "ses-1|server-key|generation-1";
    CHECK(d->call_tool("gui_session_lease", {{"action", "release"}, {"connection", 1}, {"lease_id", id}}, ctx_for(a)).is_error == false);
    CHECK(d->call_tool("gui_element_click", allowed, ctx_for(a)).is_error);
}

TEST_CASE("dispatcher: owner screen guard rejects navigation after the observed dynpro changes", "[auth][dispatch][screen-guard]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    d->set_session_leases(std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "lease-secret"; }));
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "ses-1|server-key|generation-1";
    f.facts->program = "SAPMV45A";
    f.facts->screen_number = "0100";
    Principal agent = token("agent", {"screen", "session.lease", "element"});
    agent.id = "token-a";

    const auto observed = d->call_tool("gui_screen_read", {{"connection", 1}, {"no_tabs", true}}, ctx_for(agent));
    REQUIRE_FALSE(observed.is_error);
    REQUIRE(observed.structured.has_value());
    REQUIRE(observed.structured->contains("screen_guard"));
    const std::string guard = observed.structured->at("screen_guard").get<std::string>();
    REQUIRE(guard.size() == 64);
    const auto lease = d->call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(agent));
    REQUIRE_FALSE(lease.is_error);
    json click = {{"element", "wnd[0]/usr/btnX"}, {"connection", 1},
                  {"lease_id", lease.structured->at("lease_id")}, {"expected_screen_guard", guard}};
    f.calls.clear();
    CHECK_FALSE(d->call_tool("gui_element_click", click, ctx_for(agent)).is_error);
    REQUIRE(f.calls.size() == 1);
    f.facts->screen_number = "0200";
    const auto stale = d->call_tool("gui_element_click", click, ctx_for(agent));
    CHECK(stale.is_error);
    CHECK(text_of(stale).find("SCREEN_CHANGED") != std::string::npos);
    CHECK(f.calls.size() == 1);
}

TEST_CASE("dispatcher: T-code block survives a saved connection generation change", "[auth][dispatch][session-state]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    auto state = std::make_shared<SessionPolicyState>();
    d->set_session_policy_state(state);
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "/app/con[0]/ses[0]|key|generation-1";
    Principal agent = token("agent", {"screen", "key"});
    agent.id = "token-A";
    agent.tcodes = {"VA03"};
    agent.allow_navigation = true;
    f.handler = [&](const Argv& argv) {
        if (argv.size() > 1 && argv[0] == "key") f.facts->transaction = "SE38";
        return ok_result();
    };
    d->set_session_leases(std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "lease-A"; }));
    // Seed through the call itself so both the identity and numeric fallback block are set.
    auto lease_principal = agent;
    lease_principal.scopes.insert("session.lease");
    const auto lease = d->call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(lease_principal));
    REQUIRE_FALSE(lease.is_error);
    const auto left = d->call_tool("gui_key_send", {{"key", "enter"}, {"connection", 1}, {"lease_id", "lease-A"}},
                                   ctx_for(lease_principal));
    REQUIRE_FALSE(left.is_error);
    state->retire_connection(1); // saved connection detached, window still alive
    f.facts->session_identity = "/app/con[0]/ses[0]|key|generation-2";
    f.facts->transaction = "VA03"; // another actor moved it back
    const auto blocked = d->call_tool("gui_screen_read", {{"connection", 1}, {"no_tabs", true}}, ctx_for(agent));
    CHECK(blocked.is_error);
    CHECK(text_of(blocked).find("TCODE_DENIED") != std::string::npos);
}

TEST_CASE("dispatcher: screen read with a changing dynpro does not issue a guard for unrelated content", "[auth][dispatch][screen-guard]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "ses-1|key|generation";
    f.facts->program = "SAPMV45A";
    f.facts->screen_number = "0100";
    f.handler = [&](const Argv&) {
        f.facts->screen_number = "0200";
        return ok_result();
    };
    auto d = f.make(policy);
    Principal agent = token("agent", {"screen"});
    agent.id = "token-A";
    const auto read = d->call_tool("gui_screen_read", {{"connection", 1}, {"no_tabs", true}}, ctx_for(agent));
    REQUIRE_FALSE(read.is_error);
    CHECK_FALSE((read.structured && read.structured->contains("screen_guard")));
}

TEST_CASE("dispatcher: read-only owner clients can acquire a navigation lease", "[auth][dispatch][lease]") {
    Fixture f;
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    d->set_session_leases(std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "navigation-lease"; }));
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "ses-1|key|generation";
    Principal p = token("reader", {"session", "session.lease"});
    p.id = "reader-id";
    p.read_only = true;
    const auto result = d->call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(p));
    REQUIRE_FALSE(result.is_error);
    CHECK(result.structured->at("lease_id") == "navigation-lease");
    CHECK(f.calls.empty());
}

TEST_CASE("dispatcher: successful disconnect retires its workflow lease", "[auth][dispatch][lease]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    auto leases = std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "lease-A"; });
    d->set_session_leases(leases);
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "ses-1|key|generation";
    Principal p = token("agent", {"session", "session.lease"});
    p.id = "token-A";
    REQUIRE_FALSE(d->call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(p)).is_error);
    REQUIRE(leases->permits_write(f.facts->session_identity, p.id, "lease-A", SessionLeases::Clock::now()));
    CHECK_FALSE(d->call_tool("gui_session_disconnect", {{"connection", 1}, {"lease_id", "lease-A"}}, ctx_for(p)).is_error);
    CHECK_FALSE(leases->permits_write(f.facts->session_identity, p.id, "lease-A", SessionLeases::Clock::now()));
}

TEST_CASE("dispatcher: stale disconnect preserves replacement connection policy", "[auth][dispatch][session-state]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    auto state = std::make_shared<SessionPolicyState>();
    d->set_session_policy_state(state);
    d->set_session_leases(std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "lease-A"; }));
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "ses-1|old-key|old-generation";
    Principal p = token("agent", {"session", "session.lease"});
    p.id = "token-A";
    REQUIRE_FALSE(d->call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(p)).is_error);
    const std::string replacement_key = std::string("token-B\x1f") + "connection:1";
    state->set_tcode_blocked(replacement_key, true);
    f.handler = [](const Argv&) {
        auto result = ok_result();
        result.data["file_deleted"] = false; // saved ID was replaced while disconnect ran
        result.data["session_closed"] = false;
        return result;
    };
    CHECK_FALSE(d->call_tool("gui_session_disconnect", {{"connection", 1}, {"lease_id", "lease-A"}}, ctx_for(p)).is_error);
    CHECK(state->tcode_blocked(replacement_key));
}

TEST_CASE("worker mode dispatches disconnect through its bound session lane", "[auth][dispatch][lease]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    auto leases = std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "lease-A"; });
    d->set_session_leases(leases);
    std::optional<WorkerCall> sent;
    d->set_session_invoker([&](const WorkerCall& call) { sent = call; return ok_result(); });
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "ses-1|key|generation";
    Principal p = token("agent", {"session", "session.lease"});
    p.id = "token-A";
    REQUIRE_FALSE(d->call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(p)).is_error);
    const auto result = d->call_tool("gui_session_disconnect", {{"connection", 1}, {"lease_id", "lease-A"}}, ctx_for(p));
    CHECK_FALSE(result.is_error);
    REQUIRE(sent.has_value());
    CHECK(sent->connection == 1);
    CHECK(sent->session_identity == "ses-1|key|generation");
    CHECK(sent->argv == std::vector<std::string>{"session", "disconnect", "--connection", "1"});
    CHECK_FALSE(leases->permits_write(f.facts->session_identity, p.id, "lease-A", SessionLeases::Clock::now()));
    CHECK(f.calls.empty());
}

TEST_CASE("dispatcher: tab-changing reads need a lease in owner mode", "[auth][dispatch][lease]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    d->set_session_leases(std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "lease-A"; }));
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "ses-1|key|generation";
    Principal p = token("reader", {"session.lease", "screen", "element"});
    p.id = "token-A";
    CHECK_FALSE(d->call_tool("gui_screen_read", {{"connection", 1}, {"no_tabs", true}}, ctx_for(p)).is_error);
    CHECK(text_of(d->call_tool("gui_screen_read", {{"connection", 1}}, ctx_for(p))).find("LEASE_REQUIRED") != std::string::npos);
    CHECK(text_of(d->call_tool("gui_element_get", {{"connection", 1}, {"element", "wnd[0]/usr/txtX"},
        {"activate_tab", true}}, ctx_for(p))).find("LEASE_REQUIRED") != std::string::npos);
}

TEST_CASE("dispatcher: owner batch binds one connection and one lease", "[auth][dispatch][lease][batch]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    d->set_session_leases(std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "lease-A"; }));
    f.facts = audit::SapFacts{"A4H", "001", "OWNER", "VA03"};
    f.facts->connection_id = 1;
    f.facts->session_identity = "ses-1|key|generation";
    Principal p = token("agent", {"session.lease", "batch", "element", "screen"});
    p.id = "token-A";
    REQUIRE_FALSE(d->call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(p)).is_error);
    const json items = json::array({{{"tool", "gui_element_click"}, {"arguments", {{"element", "wnd[0]/usr/btnX"}}}},
                                    {{"tool", "gui_screen_read"}, {"arguments", {{"no_tabs", true}}}}});
    const auto good = d->call_tool("gui_batch", {{"connection", 1}, {"lease_id", "lease-A"}, {"items", items}}, ctx_for(p));
    CHECK_FALSE(good.is_error);
    REQUIRE(f.calls.size() == 2);
    for (const auto& argv : f.calls) {
        CHECK(std::find(argv.begin(), argv.end(), "lease-A") == argv.end());
        CHECK(std::find(argv.begin(), argv.end(), "1") != argv.end());
    }
    f.calls.clear();
    json mixed = items;
    mixed[1]["arguments"]["connection"] = 2;
    const auto denied = d->call_tool("gui_batch", {{"connection", 1}, {"lease_id", "lease-A"}, {"items", mixed}}, ctx_for(p));
    CHECK(denied.is_error);
    CHECK(f.calls.empty());
}

TEST_CASE("dispatcher: owner batch pins its lease between items", "[auth][dispatch][lease][batch]") {
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto leases = std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "lease-A"; });
    Principal p = token("agent", {"session.lease", "batch", "element"});
    p.id = "token-A";
    int invoked = 0;
    bool released_between_items = true;
    CommandDispatcher d([&](const Argv&) { ++invoked; return ok_result(); }, policy,
        [&](const McpCallRecord& record) {
            if (record.tool == "gui_element_click" && invoked == 1)
                released_between_items = leases->release("ses-1|key|generation", p.id, "lease-A");
        });
    d.set_session_leases(leases);
    d.set_sap_facts_provider([](std::optional<int>) -> std::optional<audit::SapFacts> {
        audit::SapFacts facts{"A4H", "001", "OWNER", "VA03"};
        facts.connection_id = 1;
        facts.session_identity = "ses-1|key|generation";
        return facts;
    });
    d.set_owner_session_override([](const std::string&, const std::string&) {});
    REQUIRE_FALSE(d.call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(p)).is_error);
    const json items = json::array({{{"tool", "gui_element_click"}, {"arguments", {{"element", "wnd[0]/usr/btnA"}}}},
                                    {{"tool", "gui_element_click"}, {"arguments", {{"element", "wnd[0]/usr/btnB"}}}}});
    CHECK_FALSE(d.call_tool("gui_batch", {{"connection", 1}, {"lease_id", "lease-A"}, {"items", items}}, ctx_for(p)).is_error);
    CHECK_FALSE(released_between_items);
    CHECK(invoked == 2);
}

TEST_CASE("dispatcher: lease acquisition is withheld if its SAP session disappears", "[auth][dispatch][lease]") {
    Fixture f;
    Policy policy = write_mode();
    policy.owner_sap_identities = {"A4H/001/OWNER"};
    auto d = f.make(policy);
    auto leases = std::make_shared<SessionLeases>(std::chrono::seconds(60), [] { return "lease-A"; });
    d->set_session_leases(leases);
    int lookups = 0;
    f.facts_for = [&](std::optional<int>) -> std::optional<audit::SapFacts> {
        if (++lookups > 1) return std::nullopt;
        audit::SapFacts facts{"A4H", "001", "OWNER", "VA03"};
        facts.connection_id = 1;
        facts.session_identity = "ses-1|key|generation";
        return facts;
    };
    Principal p = token("agent", {"session.lease"});
    p.id = "token-A";
    const auto result = d->call_tool("gui_session_lease", {{"action", "acquire"}, {"connection", 1}}, ctx_for(p));
    CHECK(result.is_error);
    CHECK_FALSE(leases->permits_write("ses-1|key|generation", p.id, "lease-A", SessionLeases::Clock::now()));
}
