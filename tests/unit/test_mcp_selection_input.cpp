#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

#include "include/audit_log.h"
#include "include/auth/authorize.h"
#include "include/mcp/dispatcher.h"
#include "include/mcp/mcp_audit.h"
#include "include/mcp/policy.h"
#include "include/mcp/tool_catalog.h"

// Selection input: a read-only token created with --allow-selection-input (and a T-code allowlist) may type into plain
// selection fields on the INITIAL screen of its transaction, and nowhere else. Pure fakes, no SAP.

using namespace fairyfly;
using namespace fairyfly::mcp;
using Argv = std::vector<std::string>;

namespace {

Result ok_res() {
    Result r;
    r.status = Result::Status::Success;
    r.data = json::object();
    return r;
}

audit::SapFacts facts_at(const std::string& tcode, const std::string& program, const std::string& screen) {
    audit::SapFacts f{"A4H", "001", "USER", tcode};
    f.program = program;
    f.screen_number = screen;
    f.connection_id = 1;
    f.session_identity = "ses-1";
    return f;
}

Principal ro_token(const std::string& name, bool option = true) {
    Principal p;
    p.name = name;
    p.id = name + "-id";
    p.all_scopes = false;
    p.scopes = {"transaction", "element", "screen", "key", "batch"};
    p.read_only = true;
    p.authenticated = true;
    p.tcodes = {"SU01", "SM21"};
    p.allow_selection_input = option;
    return p;
}

struct Env {
    std::vector<Argv> calls;
    std::vector<McpCallRecord> records;
    std::vector<bool> events;  // read-only override events
    std::vector<std::string> mode_events;  // selection-input mode events ("on:program/screen", "off:/")
    bool mode_hook = true;
    std::optional<audit::SapFacts> facts;
    std::function<Result(const Argv&)> on_call;
    int auto_connection = 1;  // what automatic single-connection resolution picks when no connection is given
    std::map<int, std::string> session_of;  // identity override per connection id (default "ses-<id>")

    /// Like CommandHandler::audit_facts_for_connection: the facts carry the connection id the lookup really resolved.
    std::optional<audit::SapFacts> facts_for(std::optional<int> c) {
        if (!facts) return std::nullopt;
        audit::SapFacts f = *facts;
        const int id = c ? *c : auto_connection;
        f.connection_id = id;
        f.session_identity = session_of.count(id) ? session_of[id] : "ses-" + std::to_string(id);
        return f;
    }

    std::unique_ptr<CommandDispatcher> make(bool server_read_only = true) {
        Policy policy;
        policy.read_only = server_read_only;
        policy.allow_write = !server_read_only;
        auto d = std::make_unique<CommandDispatcher>(
            [this](const Argv& argv) {
                calls.push_back(argv);
                // a transaction start lands on the initial screen of the requested transaction by default
                if (argv.size() >= 2 && argv[0] == "transaction" && !on_call) facts = facts_at("SU01", "SAPLSUU5", "100");
                return on_call ? on_call(argv) : ok_res();
            },
            policy, [this](const McpCallRecord& r) { records.push_back(r); });
        d->set_sap_facts_provider([this](std::optional<int> c) { return facts_for(c); });
        d->set_read_only_override([this](bool ro) { events.push_back(ro); });
        if (mode_hook)
            d->set_selection_input_override([this](bool on, const std::string& program, const std::string& screen) {
                mode_events.push_back(std::string(on ? "on:" : "off:") + program + "/" + screen);
            });
        return d;
    }
};

CallContext ctx_for(const Principal& p) {
    CallContext ctx;
    ctx.request_id = 1;
    ctx.principal = p;
    ctx.http = true;
    ctx.era = ProtocolEra::Stateless;
    return ctx;
}

std::string text_of(const ToolResult& r) { return r.content.at(0).at("text").get<std::string>(); }
bool has_code(const ToolResult& r, const std::string& code) { return r.is_error && text_of(r).find(code) != std::string::npos; }

json fill(const std::string& element = "wnd[0]/usr/ctxtUSR02-BNAME", const std::string& value = "JDOE") {
    return {{"element", element}, {"value", value}};
}

/// Starts SU01 (the fake lands on SAPLSUU5/100) and returns the result.
ToolResult start_su01(CommandDispatcher& d, const Principal& p) {
    return d.call_tool("gui_transaction_start", {{"code", "SU01"}}, ctx_for(p));
}

} // namespace

TEST_CASE("selection input: fill is allowed on the initial screen and the guard is lifted for that call only", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");

    CHECK_FALSE(start_su01(*d, p).is_error);
    env.events.clear();
    const auto r = d->call_tool("gui_element_fill", fill(), ctx_for(p));
    CHECK_FALSE(r.is_error);
    REQUIRE(env.calls.back().size() >= 3);
    CHECK(env.calls.back()[0] == "element");
    CHECK(env.calls.back()[1] == "fill");
    REQUIRE(env.events.size() == 2);
    CHECK(env.events[0] == false);   // lifted for this call
    CHECK(env.events[1] == true);    // server value restored
    CHECK(env.records.back().input_allowed);

    // a read in between never lifts the guard, and the start itself did not record input_allowed
    env.events.clear();
    CHECK_FALSE(d->call_tool("gui_screen_read", json::object(), ctx_for(p)).is_error);
    CHECK(env.events.empty());
    CHECK_FALSE(env.records.back().input_allowed);
}

TEST_CASE("selection input: other tools stay under the read-only guard", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    env.events.clear();
    env.calls.clear();
    // click runs under the handler's read-only guard (commit buttons are refused there): the guard is never lifted for it
    CHECK_FALSE(d->call_tool("gui_element_click", {{"element", "wnd[0]/tbar[1]/btn[8]"}}, ctx_for(p)).is_error);
    CHECK(env.calls.size() == 1);
    CHECK(env.events.empty());
    CHECK_FALSE(env.records.back().input_allowed);
    // Enter / F8 stay allowed (execute is not a commit)
    CHECK_FALSE(d->call_tool("gui_key_send", {{"key", "enter"}}, ctx_for(p)).is_error);
    CHECK_FALSE(d->call_tool("gui_key_send", {{"key", "f8"}}, ctx_for(p)).is_error);
}

TEST_CASE("selection input: principals without the option keep the read-only refusal", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal plain = ro_token("plain", false);
    CHECK_FALSE(start_su01(*d, plain).is_error);
    env.calls.clear();
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(plain)), "TOOL_UNAVAILABLE_READ_ONLY"));
    CHECK(env.calls.empty());

    // write-mode server, read-only token without the option
    Env env2;
    auto d2 = env2.make(false);
    CHECK(has_code(d2->call_tool("gui_element_fill", fill(), ctx_for(plain)), "READ_ONLY"));
    CHECK(env2.calls.empty());

    // the stdio principal on a read-only server
    Env env3;
    auto d3 = env3.make(true);
    CHECK(has_code(d3->call_tool("gui_element_fill", fill(), ctx_for(Principal{})), "TOOL_UNAVAILABLE_READ_ONLY"));
}

TEST_CASE("selection input: the option works the same on a write-mode server with a read-only token", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(false);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    env.events.clear();
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);
    REQUIRE(env.events.size() == 2);
    CHECK(env.events[0] == false);
    CHECK(env.events[1] == false);  // the server value (write mode) is restored
    // a click of the same token runs with the handler narrowed to read-only (true first), never lifted
    env.events.clear();
    CHECK_FALSE(d->call_tool("gui_element_click", {{"element", "wnd[0]/tbar[1]/btn[8]"}}, ctx_for(p)).is_error);
    REQUIRE(env.events.size() == 2);
    CHECK(env.events[0] == true);
    CHECK(env.events[1] == false);
}

TEST_CASE("selection input: typing is refused after navigating away from the initial screen", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);

    env.facts = facts_at("SU01", "SAPLSUU5", "200");  // Enter moved to a sub screen
    env.calls.clear();
    env.events.clear();
    const auto r = d->call_tool("gui_element_fill", fill(), ctx_for(p));
    CHECK(has_code(r, "INPUT_SCREEN_DENIED"));
    CHECK(text_of(r).find("initial screen of the transaction") != std::string::npos);
    CHECK(text_of(r).find("gui_transaction_start") != std::string::npos);
    CHECK(env.calls.empty());
    CHECK(env.events.empty());
    CHECK_FALSE(env.records.back().input_allowed);
    CHECK(env.records.back().error_code == "INPUT_SCREEN_DENIED");

    // same screen number in another program is another screen
    env.facts = facts_at("SU01", "SAPMSUU5", "100");
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
    // back on the initial screen: still denied, typing needs a new gui_transaction_start
    env.facts = facts_at("SU01", "SAPLSUU5", "100");
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
    CHECK_FALSE(start_su01(*d, p).is_error);
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);
}

TEST_CASE("selection input: navigating away and back does not re-enable typing", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    // a key call moves to another screen; the post-call facts clear the record
    env.on_call = [&](const Argv&) { env.facts = facts_at("SU01", "SAPLSUU5", "200"); return ok_res(); };
    CHECK_FALSE(d->call_tool("gui_key_send", {{"key", "enter"}}, ctx_for(p)).is_error);
    // F3 returns to the initial screen: the record is gone anyway
    env.on_call = [&](const Argv&) { env.facts = facts_at("SU01", "SAPLSUU5", "100"); return ok_res(); };
    CHECK_FALSE(d->call_tool("gui_key_send", {{"key", "f8"}}, ctx_for(p)).is_error);
    env.calls.clear();
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
    CHECK(env.calls.empty());
}

TEST_CASE("selection input: empty post-call facts clear the record", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    env.on_call = [&](const Argv&) { env.facts = std::nullopt; return ok_res(); };
    CHECK_FALSE(d->call_tool("gui_screen_read", json::object(), ctx_for(p)).is_error);
    env.on_call = nullptr;
    env.facts = facts_at("SU01", "SAPLSUU5", "100");
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
}

TEST_CASE("selection input: the record is bound to the connection of the start", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(d->call_tool("gui_transaction_start", {{"code", "SU01"}, {"connection", 1}}, ctx_for(p)).is_error);
    json other = fill();
    other["connection"] = 2;
    env.calls.clear();
    CHECK(has_code(d->call_tool("gui_element_fill", other, ctx_for(p)), "INPUT_SCREEN_DENIED"));
    CHECK(env.calls.empty());
    // an implicit connection that resolves to another connection than the explicit 1 is another target as well
    env.auto_connection = 2;
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
    env.auto_connection = 1;
    // the same connection works (a denied call does not clear a record whose screen still matches)
    CHECK_FALSE(d->call_tool("gui_transaction_start", {{"code", "SU01"}, {"connection", 1}}, ctx_for(p)).is_error);
    json same = fill();
    same["connection"] = 1;
    CHECK_FALSE(d->call_tool("gui_element_fill", same, ctx_for(p)).is_error);
}

TEST_CASE("selection input: no recorded initial screen or unknown screen facts deny", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");

    // never started a transaction through this server
    env.facts = facts_at("SU01", "SAPLSUU5", "100");
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));

    // start, then the program/screen facts become unknown
    CHECK_FALSE(start_su01(*d, p).is_error);
    env.facts = facts_at("SU01", "", "");
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
    env.facts = facts_at("SU01", "SAPLSUU5", "");
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
    // no facts at all: the open transaction is unknown (fail closed)
    env.facts = std::nullopt;
    CHECK(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);

    // a start whose post-call facts carry no program/screen records nothing
    Env env2;
    auto d2 = env2.make(true);
    env2.on_call = [&](const Argv&) { env2.facts = facts_at("SU01", "", ""); return ok_res(); };
    CHECK_FALSE(start_su01(*d2, p).is_error);
    env2.facts = facts_at("SU01", "SAPLSUU5", "100");
    CHECK(has_code(d2->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
}

TEST_CASE("selection input: cells, the command field and credential fields are refused", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    env.calls.clear();

    json cell = fill("wnd[0]/usr/cntlGRID/shellcont/shell");
    cell["row"] = 0;
    cell["column"] = "BNAME";
    CHECK(has_code(d->call_tool("gui_element_fill", cell, ctx_for(p)), "INPUT_TARGET_DENIED"));
    json checkbox = fill("wnd[0]/usr/chkX", "X");
    checkbox["checkbox"] = true;
    CHECK(has_code(d->call_tool("gui_element_fill", checkbox, ctx_for(p)), "INPUT_TARGET_DENIED"));

    CHECK(has_code(d->call_tool("gui_element_fill", fill("wnd[0]/tbar[0]/okcd", "/nSE38"), ctx_for(p)), "INPUT_TARGET_DENIED"));
    CHECK(has_code(d->call_tool("gui_element_fill", {{"id", "wnd[0]/tbar[0]/OKCD"}, {"value", "/nSE38"}}, ctx_for(p)), "INPUT_TARGET_DENIED"));

    for (const char* id : {"wnd[0]/usr/pwdRSYST-BCODE", "wnd[0]/usr/txtUSR02-PASSWORD", "wnd[0]/usr/txtRSYST-BCODE",
                           "wnd[0]/usr/txtSUID_ST_NODE_PASSWORD-PASSWORD", "wnd[0]/usr/txtMY_API_TOKEN"}) {
        const auto r = d->call_tool("gui_element_fill", fill(id, "x"), ctx_for(p));
        INFO(id);
        CHECK(has_code(r, "INPUT_TARGET_DENIED"));
    }
    CHECK(env.calls.empty());  // nothing reached the CLI
}

TEST_CASE("selection input: an option without a T-code allowlist is refused", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    Principal p = ro_token("basis");
    p.tcodes.clear();  // not producible by the CLI; a hand-built principal must still fail closed
    env.facts = facts_at("SU01", "SAPLSUU5", "100");
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_NOT_ALLOWED"));
    CHECK(env.calls.empty());
}

TEST_CASE("selection input: the open transaction must be allowlisted", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    env.facts = facts_at("SE38", "SAPLSUU5", "100");
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "TCODE_DENIED"));
}

TEST_CASE("selection input: state is cleared when the transaction leaves the allowlist and replaced by a new start", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);

    // a key press ends in a transaction outside the allowlist
    env.on_call = [&](const Argv&) { env.facts = facts_at("SE38", "SAPMS38M", "100"); return ok_res(); };
    CHECK_FALSE(d->call_tool("gui_key_send", {{"key", "enter"}}, ctx_for(p)).is_error);
    env.on_call = nullptr;
    // back on the allowlisted transaction and screen (e.g. by hand on the desktop): the record is gone, and the block holds
    env.facts = facts_at("SU01", "SAPLSUU5", "100");
    CHECK(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);
    // start again: allowed
    CHECK_FALSE(start_su01(*d, p).is_error);
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);

    // a later successful start replaces the record (new transaction, new screen)
    env.on_call = [&](const Argv&) { env.facts = facts_at("SM21", "RSLG0100", "1000"); return ok_res(); };
    CHECK_FALSE(d->call_tool("gui_transaction_start", {{"code", "SM21"}}, ctx_for(p)).is_error);
    env.on_call = nullptr;
    CHECK_FALSE(d->call_tool("gui_element_fill", fill("wnd[0]/usr/ctxtFROM_DATE", "01.10.2026"), ctx_for(p)).is_error);
    env.facts = facts_at("SU01", "SAPLSUU5", "100");  // the old screen is not the initial screen any more
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
}

TEST_CASE("selection input: state is separate per principal", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal a = ro_token("alice");
    const Principal b = ro_token("bob");
    CHECK_FALSE(start_su01(*d, a).is_error);
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(a)).is_error);
    // bob never started a transaction, whatever the shared SAP screen shows
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(b)), "INPUT_SCREEN_DENIED"));
    CHECK_FALSE(start_su01(*d, b).is_error);
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(b)).is_error);

    // same name, different token id: starts clean
    Principal a2 = ro_token("alice");
    a2.id = "other-id";
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(a2)), "INPUT_SCREEN_DENIED"));
}

TEST_CASE("selection input: gui_batch items follow the same rules", "[mcp][selection-input][batch]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");

    // start + fill in one batch: the fill sees the screen the start recorded
    json items = json::array({{{"tool", "gui_transaction_start"}, {"arguments", {{"code", "SU01"}}}},
                              {{"tool", "gui_element_fill"}, {"arguments", fill()}}});
    auto r = d->call_tool("gui_batch", {{"items", items}}, ctx_for(p));
    CHECK_FALSE(r.is_error);
    REQUIRE(env.records.size() >= 2);
    CHECK(env.records.back().tool == "gui_element_fill");
    CHECK(env.records.back().input_allowed);

    // a fill item after the screen changed is refused at execution time
    env.facts = facts_at("SU01", "SAPLSUU5", "300");
    items = json::array({{{"tool", "gui_element_fill"}, {"arguments", fill()}}});
    r = d->call_tool("gui_batch", {{"items", items}}, ctx_for(p));
    CHECK(r.is_error);
    CHECK(text_of(r).find("INPUT_SCREEN_DENIED") != std::string::npos);

    // a cell item refuses the whole batch up front: nothing runs
    env.calls.clear();
    json cell = fill("wnd[0]/usr/cntlGRID/shellcont/shell");
    cell["row"] = 1;
    cell["column"] = "BNAME";
    items = json::array({{{"tool", "gui_screen_read"}}, {{"tool", "gui_element_fill"}, {"arguments", cell}}});
    r = d->call_tool("gui_batch", {{"items", items}}, ctx_for(p));
    CHECK(has_code(r, "INPUT_TARGET_DENIED"));
    CHECK(env.calls.empty());

    // a click item never lifts the guard
    env.events.clear();
    items = json::array({{{"tool", "gui_element_click"}, {"arguments", {{"element", "wnd[0]/tbar[1]/btn[8]"}}}}});
    r = d->call_tool("gui_batch", {{"items", items}}, ctx_for(p));
    CHECK_FALSE(r.is_error);
    CHECK(env.events.empty());

    // a batch of a token without the option is refused up front
    const Principal plain = ro_token("plain", false);
    items = json::array({{{"tool", "gui_element_fill"}, {"arguments", fill()}}});
    CHECK(has_code(d->call_tool("gui_batch", {{"items", items}}, ctx_for(plain)), "READ_ONLY"));
}

TEST_CASE("selection input: the handler mode is set for the fill call only and restored on every path", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    CHECK(env.mode_events.empty());  // the start does not switch the mode
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);
    REQUIRE(env.mode_events.size() == 2);
    CHECK(env.mode_events[0] == "on:SAPLSUU5/100");  // the recorded initial screen is what the handler enforces
    CHECK(env.mode_events[1] == "off:/");

    // a refusal before the call never switches it
    env.mode_events.clear();
    env.facts = facts_at("SU01", "SAPLSUU5", "200");
    CHECK(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);
    CHECK(env.mode_events.empty());

    // reads never switch it
    env.facts = facts_at("SU01", "SAPLSUU5", "100");
    CHECK_FALSE(start_su01(*d, p).is_error);
    CHECK_FALSE(d->call_tool("gui_screen_read", json::object(), ctx_for(p)).is_error);
    CHECK(env.mode_events.empty());

    // an exception inside the invocation (also what a CALL_TIMEOUT unwinding looks like) restores it
    env.on_call = [](const Argv&) -> Result { throw std::runtime_error("boom"); };
    CHECK(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);
    REQUIRE(env.mode_events.size() == 2);
    CHECK(env.mode_events[1] == "off:/");

    // an error result restores it too
    env.mode_events.clear();
    env.facts = facts_at("SU01", "SAPLSUU5", "100");
    env.on_call = [](const Argv&) {
        Result r;
        r.status = Result::Status::Error;
        r.error = {{"code", "INPUT_SCREEN_DENIED"}, {"message", "screen changed"}};
        return r;
    };
    // the earlier exception call changed nothing in the record: the screen still matches
    const auto r = d->call_tool("gui_element_fill", fill(), ctx_for(p));
    CHECK(r.is_error);
    REQUIRE(env.mode_events.size() == 2);
    CHECK(env.mode_events[1] == "off:/");
}

TEST_CASE("selection input: without a handler mode hook the fill is refused (fail closed)", "[mcp][selection-input]") {
    Env env;
    env.mode_hook = false;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    env.calls.clear();
    env.events.clear();
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_TARGET_DENIED"));
    CHECK(env.calls.empty());
    // the lifted guard (if it was lifted at all) is restored
    if (!env.events.empty()) CHECK(env.events.back() == true);
}

TEST_CASE("selection input: the read-only override is restored when the invocation throws", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    env.events.clear();
    env.on_call = [](const Argv&) -> Result { throw std::runtime_error("boom"); };
    const auto r = d->call_tool("gui_element_fill", fill(), ctx_for(p));
    CHECK(r.is_error);
    REQUIRE(env.events.size() == 2);
    CHECK(env.events[0] == false);
    CHECK(env.events[1] == true);

    // an override hook that throws on restore does not leak out of the call
    Env env2;
    auto d2 = env2.make(true);
    CHECK_FALSE(start_su01(*d2, p).is_error);
    d2->set_read_only_override([&](bool ro) {
        env2.events.push_back(ro);
        if (ro) throw std::runtime_error("restore failed");
    });
    CHECK_NOTHROW(d2->call_tool("gui_element_fill", fill(), ctx_for(p)));
}

TEST_CASE("selection input: FAIRYFLY_READ_ONLY hard cap still allows it (typing cannot commit)", "[mcp][selection-input]") {
    _putenv_s("FAIRYFLY_READ_ONLY", "1");
    Env env;
    auto d = env.make(true);  // run_mcp forces the server read-only under the cap
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    const auto r = d->call_tool("gui_element_fill", fill(), ctx_for(p));
    const bool fill_ok = !r.is_error;
    // without the option the cap refuses fill
    const Principal plain = ro_token("plain", false);
    CHECK_FALSE(start_su01(*d, plain).is_error);
    const auto plain_fill = d->call_tool("gui_element_fill", fill(), ctx_for(plain));
    _putenv_s("FAIRYFLY_READ_ONLY", "");
    CHECK(fill_ok);
    CHECK(has_code(plain_fill, "TOOL_UNAVAILABLE_READ_ONLY"));
}

TEST_CASE("selection input: tools/list shows the narrowed fill tool only to such principals", "[mcp][selection-input][tools]") {
    Env env;
    auto d = env.make(true);
    const auto names = [](const std::vector<ToolDef>& defs) {
        std::set<std::string> out;
        for (const auto& t : defs) out.insert(t.name);
        return out;
    };
    const auto find = [](const std::vector<ToolDef>& defs, const std::string& n) -> const ToolDef* {
        for (const auto& t : defs)
            if (t.name == n) return &t;
        return nullptr;
    };

    const auto with_option = d->list_tools_for(ro_token("basis"));
    const auto plain = d->list_tools_for(ro_token("plain", false));
    const auto local = d->list_tools();
    CHECK(names(with_option).count("gui_element_fill") == 1);
    CHECK(names(plain).count("gui_element_fill") == 0);
    CHECK(names(local).count("gui_element_fill") == 0);
    // the narrowed fill tool is the ONLY difference to the plain read-only view
    auto extra = names(with_option);
    for (const auto& n : names(plain)) extra.erase(n);
    CHECK(extra == std::set<std::string>{"gui_element_fill"});

    const ToolDef* fill_def = find(with_option, "gui_element_fill");
    REQUIRE(fill_def);
    CHECK(fill_def->description.find("initial screen") != std::string::npos);
    CHECK(fill_def->description.find("selection field") != std::string::npos);
    CHECK(fill_def->input_schema["properties"].contains("element"));
    CHECK_FALSE(fill_def->input_schema["properties"].contains("row"));
    CHECK_FALSE(fill_def->input_schema["properties"].contains("column"));

    // descriptions that mention fill follow the visible set
    const ToolDef* get_with = find(with_option, "gui_screen_find");
    const ToolDef* get_plain = find(plain, "gui_screen_find");
    REQUIRE(get_with);
    REQUIRE(get_plain);
    CHECK(get_with->description.find("gui_element_fill") != std::string::npos);
    CHECK(get_plain->description.find("gui_element_fill") == std::string::npos);

    // scope matters: no element scope, no fill
    Principal no_element = ro_token("noel");
    no_element.scopes.erase("element");
    CHECK(names(d->list_tools_for(no_element)).count("gui_element_fill") == 0);
    // a write-mode server shows it to the read-only option holder too (the tool is narrowed to the same description)
    Env env2;
    auto d2 = env2.make(false);
    const auto wm = d2->list_tools_for(ro_token("basis"));
    const ToolDef* wm_fill = find(wm, "gui_element_fill");
    REQUIRE(wm_fill);
    CHECK(wm_fill->description.find("initial screen") != std::string::npos);
    CHECK(wm_fill->description.find("echoed nowhere") == std::string::npos);
    CHECK(wm_fill->description.find("value read back from the control, except for credential fields") != std::string::npos);
    CHECK(wm_fill->description.find("EXECUTE") != std::string::npos);
}

TEST_CASE("selection input: audit records input_allowed, the INPUT_* codes, and never the typed value", "[mcp][selection-input][audit]") {
    const auto file = std::filesystem::temp_directory_path() /
                      ("ff_selin_audit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    audit::AuditConfig config;
    config.mode = audit::Mode::Enabled;
    config.file = file;
    audit::AuditSink sink(config);

    Env env;
    Policy policy;
    policy.read_only = true;
    CommandDispatcher d(
        [&](const Argv& argv) {
            env.calls.push_back(argv);
            if (argv[0] == "transaction") env.facts = facts_at("SU01", "SAPLSUU5", "100");
            return ok_res();
        },
        policy, make_mcp_audit_hook(&sink, nullptr, true));
    d.set_sap_facts_provider([&](std::optional<int>) { return env.facts; });
    d.set_read_only_override([](bool) {});
    d.set_selection_input_override([](bool, const std::string&, const std::string&) {});
    const Principal p = ro_token("basis");
    const std::string typed = "zz-typed-value-4711";

    CHECK_FALSE(start_su01(d, p).is_error);
    CHECK_FALSE(d.call_tool("gui_element_fill", fill("wnd[0]/usr/ctxtUSR02-BNAME", typed), ctx_for(p)).is_error);
    env.facts = facts_at("SU01", "SAPLSUU5", "200");
    CHECK(d.call_tool("gui_element_fill", fill("wnd[0]/usr/ctxtUSR02-BNAME", typed), ctx_for(p)).is_error);
    json cell = fill("wnd[0]/usr/cntlGRID/shellcont/shell", typed);
    cell["row"] = 0;
    cell["column"] = "BNAME";
    env.facts = facts_at("SU01", "SAPLSUU5", "100");
    CHECK(d.call_tool("gui_element_fill", cell, ctx_for(p)).is_error);

    std::ifstream in(file);
    std::string line, all;
    std::vector<nlohmann::json> rows;
    while (std::getline(in, line)) {
        all += line + "\n";
        rows.push_back(nlohmann::json::parse(line));
    }
    in.close();
    std::filesystem::remove(file);

    REQUIRE(rows.size() == 4);
    CHECK_FALSE(rows[0].contains("input_allowed"));
    CHECK(rows[1]["input_allowed"] == true);
    CHECK(rows[1]["principal"] == "basis");
    CHECK(rows[1]["transport"] == "http");
    CHECK(rows[1]["status"] == "success");
    CHECK_FALSE(rows[2].contains("input_allowed"));
    CHECK(rows[2]["error_code"] == "INPUT_SCREEN_DENIED");
    CHECK_FALSE(rows[3].contains("input_allowed"));
    CHECK(rows[3]["error_code"] == "INPUT_TARGET_DENIED");
    CHECK(all.find(typed) == std::string::npos);
}

TEST_CASE("selection input: an automatically resolved start binds typing to the connection it really used", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    // no connection argument, no sticky default: the lookup resolves the only cached connection (1)
    CHECK_FALSE(start_su01(*d, p).is_error);
    env.calls.clear();
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);

    // the only cached connection becomes another one (B) that shows the same transaction/program/screen
    env.auto_connection = 2;
    env.calls.clear();
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
    CHECK(env.calls.empty());
    env.auto_connection = 1;

    // same connection id, but re-attached to another SAP session: denied as well
    env.session_of[1] = "ses-other";
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
    env.session_of.clear();
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);
}

TEST_CASE("selection input: the connection id of the start result wins over the pre-call guess", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    env.auto_connection = 3;
    env.on_call = [&](const Argv& argv) {
        if (argv[0] == "transaction") {
            env.facts = facts_at("SU01", "SAPLSUU5", "100");
            Result r = ok_res();
            r.data = {{"connection_id", 3}};
            return r;
        }
        return ok_res();
    };
    CHECK_FALSE(start_su01(*d, p).is_error);
    CHECK_FALSE(d->call_tool("gui_element_fill", fill(), ctx_for(p)).is_error);
    json explicit_same = fill();
    explicit_same["connection"] = 3;
    CHECK_FALSE(d->call_tool("gui_element_fill", explicit_same, ctx_for(p)).is_error);
    json explicit_other = fill();
    explicit_other["connection"] = 4;
    CHECK(has_code(d->call_tool("gui_element_fill", explicit_other, ctx_for(p)), "INPUT_SCREEN_DENIED"));
}

TEST_CASE("selection input: unknown connection or session identity on either side denies", "[mcp][selection-input]") {
    Env env;
    auto d = env.make(true);
    const Principal p = ro_token("basis");
    CHECK_FALSE(start_su01(*d, p).is_error);
    // the lookup cannot tell the session (identity empty) -> unknown at fill time
    env.session_of[1] = "";
    CHECK(has_code(d->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));

    // unknown identity at start time records nothing
    Env env2;
    auto d2 = env2.make(true);
    env2.session_of[1] = "";
    CHECK_FALSE(start_su01(*d2, p).is_error);
    env2.session_of.clear();
    CHECK(has_code(d2->call_tool("gui_element_fill", fill(), ctx_for(p)), "INPUT_SCREEN_DENIED"));
}
