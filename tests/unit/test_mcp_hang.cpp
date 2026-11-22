// Issue #155: the MCP dispatcher turns a GUI that ignores every navigation into SAP_GUI_UNRESPONSIVE,
// tries a soft recovery first and restarts SAP Logon only when automatic SAP startup is allowed.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "include/hang_detector.h"
#include "include/mcp/dispatcher.h"
#include "include/mcp/policy.h"
#include "include/session_startup.h"

using namespace fairyfly::mcp;
using fairyfly::Result;
using Argv = std::vector<std::string>;

namespace {

Result noop_key(const Argv& argv) {
    Result r;
    r.status = Result::Status::Success;
    int vkey = 0;
    if (argv.size() > 2 && (argv[2] == "F3" || argv[2] == "f3")) vkey = 3;
    if (argv.size() > 2 && (argv[2] == "F12" || argv[2] == "f12")) vkey = 12;
    r.data = {{"action", "send_key"}, {"vkey", vkey}, {"screen_changed", false}, {"busy_before", false}};
    r.duration = std::chrono::milliseconds(7);
    return r;
}

Result tcode_not_started() {
    auto r = Result::fail("TRANSACTION_NOT_STARTED", "Requested transaction did not become active");
    r.error["session_busy"] = false;
    r.error["screen_changed"] = false;
    return r;
}

/// A GUI that accepts every input and changes nothing (the state seen on 2026-11-21).
struct HungGui {
    std::vector<Argv> calls;
    bool hung = true;
    bool popup_open = false;    ///< a SAP modal (wnd[1]) that popup close removes
    bool n_works = false;       ///< "/n" through the command field leaves the screen although F3 does not
    std::function<Result(const Argv&)> on_other = [](const Argv&) { return Result::ok(json::object()); };

    Result operator()(const Argv& argv) {
        calls.push_back(argv);
        if (argv.size() > 1 && argv[0] == "key" && argv[1] == "send") {
            auto r = noop_key(argv);
            if (!hung) r.data["screen_changed"] = true;
            return r;
        }
        if (argv.size() > 1 && argv[0] == "transaction" && argv[1] == "start") {
            if (!hung || (n_works && argv.size() > 2 && argv[2] == "/n")) {
                hung = false;
                return Result::ok({{"actual_tcode", "SESSION_MANAGER"}, {"screen_changed", true}});
            }
            return tcode_not_started();
        }
        if (argv.size() > 1 && argv[0] == "popup" && argv[1] == "close") {
            if (!popup_open) return Result::fail("NO_POPUP", "No popup window is open");
            popup_open = false;
            hung = false;
            return Result::ok({{"closed", true}});
        }
        return on_other(argv);
    }
    int count(const std::string& noun, const std::string& verb) const {
        return static_cast<int>(std::count_if(calls.begin(), calls.end(), [&](const Argv& a) {
            return a.size() > 1 && a[0] == noun && a[1] == verb;
        }));
    }
};

struct Rig {
    HungGui gui;
    std::vector<McpCallRecord> records;
    CommandDispatcher dispatcher{[this](const Argv& argv) { return gui(argv); }, Policy{},
                                 [this](const McpCallRecord& r) { records.push_back(r); }};
    ToolResult f3() { return dispatcher.call_tool("gui_key_send", {{"key", "F3"}}, ctx()); }
    static CallContext ctx() {
        CallContext c;
        c.request_id = 1;
        return c;
    }
};

std::string text_of(const ToolResult& r) { return r.content.at(0).at("text").get<std::string>(); }
std::string all_text(const ToolResult& r) {
    std::string out;
    for (const auto& block : r.content)
        if (block.contains("text")) out += block["text"].get<std::string>() + "\n";
    return out;
}

}  // namespace

TEST_CASE("MCP hang: navigation keys that change nothing end in SAP_GUI_UNRESPONSIVE, not success", "[mcp][hang]") {
    Rig rig;
    CHECK_FALSE(rig.f3().is_error);
    CHECK_FALSE(rig.f3().is_error);
    const auto third = rig.f3();
    INFO(text_of(third));
    REQUIRE(third.is_error);
    CHECK(text_of(third).find("SAP_GUI_UNRESPONSIVE") != std::string::npos);
    REQUIRE(third.structured.has_value());
    const json& error = (*third.structured)["error"];
    CHECK(error["code"] == "SAP_GUI_UNRESPONSIVE");
    CHECK(error["evidence"].size() == 3);
    CHECK(error["original_operation_run"] == true);
    CHECK(rig.records.back().error_code == "SAP_GUI_UNRESPONSIVE");
}

TEST_CASE("MCP hang: transaction starts that never become active count too", "[mcp][hang]") {
    Rig rig;
    rig.dispatcher.call_tool("gui_transaction_start", {{"code", "/nSE38"}}, Rig::ctx());
    rig.dispatcher.call_tool("gui_transaction_start", {{"code", "/nSU01"}}, Rig::ctx());
    const auto third = rig.dispatcher.call_tool("gui_transaction_start", {{"code", "/nSE16"}}, Rig::ctx());
    REQUIRE(third.is_error);
    CHECK(text_of(third).find("SAP_GUI_UNRESPONSIVE") != std::string::npos);
}

TEST_CASE("MCP hang: a responsive GUI never trips", "[mcp][hang]") {
    Rig rig;
    rig.gui.hung = false;
    for (int i = 0; i < 6; ++i) CHECK_FALSE(rig.f3().is_error);
}

TEST_CASE("MCP hang: a screen change between no-ops prevents the trip", "[mcp][hang]") {
    Rig rig;
    rig.f3();
    rig.f3();
    rig.gui.hung = false;
    CHECK_FALSE(rig.f3().is_error);
    rig.gui.hung = true;
    CHECK_FALSE(rig.f3().is_error);
    CHECK_FALSE(rig.f3().is_error);
}

TEST_CASE("MCP hang: the hint tells the agent not to retry in a loop", "[mcp][hang]") {
    Rig rig;
    rig.f3();
    rig.f3();
    const auto third = rig.f3();
    REQUIRE(third.is_error);
    CHECK(text_of(third).find("hint:") != std::string::npos);
    CHECK(text_of(third).find("Do not retry") != std::string::npos);
}

// ---- Stage B: soft recovery before reporting ------------------------------------------------------------------

namespace {
ToolResult trip(Rig& rig) {
    rig.f3();
    rig.f3();
    return rig.f3();
}
json error_of(const ToolResult& r) { return r.structured ? (*r.structured)["error"] : json::object(); }
}  // namespace

TEST_CASE("MCP hang: a SAP modal that blocks the screen is closed and reported as SAP_GUI_RECOVERED", "[mcp][hang][recovery]") {
    Rig rig;
    rig.gui.popup_open = true;
    const auto third = trip(rig);
    INFO(text_of(third));
    REQUIRE(third.is_error);
    CHECK(error_of(third)["code"] == "SAP_GUI_RECOVERED");
    CHECK(error_of(third)["recovery"]["step"] == "popup_closed");
    CHECK(error_of(third)["original_operation_run"] == true);
    CHECK(text_of(third).find("read the screen") != std::string::npos);
    CHECK(rig.gui.count("popup", "close") == 1);
}

TEST_CASE("MCP hang: a native SAP dialog is cancelled first", "[mcp][hang][recovery]") {
    Rig rig;
    int dialogs = 1;
    int dismissed = 0;
    CommandDispatcher::HangRecovery hooks;
    hooks.probe = [&] {
        json list = json::array();
        for (int i = 0; i < dialogs; ++i) list.push_back({{"title", "Save As"}, {"class", "#32770"}, {"process", "saplogon.exe"}});
        return json{{"native_dialogs", list}, {"main_window_enabled", dialogs == 0}};
    };
    hooks.dismiss_native_dialogs = [&] {
        ++dismissed;
        const int sent = dialogs;
        dialogs = 0;
        rig.gui.hung = false;
        return sent;
    };
    rig.dispatcher.set_hang_recovery(hooks);
    const auto third = trip(rig);
    INFO(text_of(third));
    REQUIRE(third.is_error);
    CHECK(error_of(third)["code"] == "SAP_GUI_RECOVERED");
    CHECK(error_of(third)["recovery"]["step"] == "native_dialog_cancelled");
    CHECK(dismissed == 1);
    CHECK(rig.gui.count("popup", "close") == 0);
}

TEST_CASE("MCP hang: /n through the command field recovers a screen that F3 cannot leave", "[mcp][hang][recovery]") {
    Rig rig;
    rig.gui.n_works = true;
    const auto third = trip(rig);
    INFO(text_of(third));
    REQUIRE(third.is_error);
    CHECK(error_of(third)["code"] == "SAP_GUI_RECOVERED");
    CHECK(error_of(third)["recovery"]["step"] == "command_n");
    CHECK(error_of(third)["recovery"]["unsaved_input_lost"] == true);
}

TEST_CASE("MCP hang: when no soft step helps, the error lists what was tried", "[mcp][hang][recovery]") {
    Rig rig;
    const auto third = trip(rig);
    REQUIRE(third.is_error);
    const json error = error_of(third);
    CHECK(error["code"] == "SAP_GUI_UNRESPONSIVE");
    REQUIRE(error.contains("recovery"));
    CHECK(error["recovery"]["recovered"] == false);
    CHECK(error["recovery"]["attempted"] == json::array({"popup_close", "command_n", "cancel"}));
}

TEST_CASE("MCP hang: after a recovery the count starts again", "[mcp][hang][recovery]") {
    Rig rig;
    rig.gui.popup_open = true;
    REQUIRE(error_of(trip(rig))["code"] == "SAP_GUI_RECOVERED");
    rig.gui.hung = true;
    CHECK_FALSE(rig.f3().is_error);
    CHECK_FALSE(rig.f3().is_error);
}

// ---- Stage C: restart only when automatic SAP startup is allowed ------------------------------------------------

namespace {

/// A logged-on session on connection 7 whose GUI hangs; ending SAP GUI and preparing again gives a responsive one
/// unless `hangs_after_restart`.
struct RestartRig {
    HungGui gui;
    bool logged_on = true;
    bool local_enabled = true;
    bool approval_waiting = false;
    bool hangs_after_restart = false;
    json open_connections = json::array();   ///< what session list reports (description = SAP Logon entry)
    bool end_fails = false;
    std::string saved_connection_name = "Bigfox";   ///< SAP Logon entry of the saved connection 7
    int ended = 0;
    int launches = 0;
    CommandDispatcher dispatcher;

    static Policy policy_for(bool write) {
        Policy p;
        p.read_only = !write;
        p.allow_write = write;
        return p;
    }
    explicit RestartRig(bool write = true)
        : dispatcher([this](const Argv& argv) { return gui(argv); }, policy_for(write)) {
        gui.on_other = [this](const Argv& argv) {
            if (argv.size() > 1 && argv[0] == "session" && argv[1] == "list")
                return Result::ok({{"connections", logged_on ? open_connections : json::array()},
                                   {"total_connections", logged_on ? open_connections.size() : 0}});
            if (argv.size() > 1 && argv[0] == "session" && argv[1] == "attach")
                return Result::ok({{"connection_file_id", 7}, {"session_id", "/app/con[0]/ses[0]"}});
            if (argv.size() > 1 && argv[0] == "session" && argv[1] == "launch") {
                ++launches;
                logged_on = true;
                gui.hung = hangs_after_restart;
                return Result::ok({{"connection_file_id", 7}});
            }
            return Result::ok(json::object());
        };
        dispatcher.set_session_startup([this] { return fairyfly::startup::Policy{local_enabled, false, false, "Bigfox"}; },
            [](const fairyfly::startup::Policy&, const std::function<bool()>&) { return Result::ok(); });
        dispatcher.set_sap_facts_provider([this](std::optional<int>) -> std::optional<fairyfly::audit::SapFacts> {
            if (!logged_on) return std::nullopt;
            fairyfly::audit::SapFacts facts;
            facts.connection_id = 7;
            facts.system = "A4H"; facts.client = "001"; facts.user = "DEVELOPER";
            facts.session_identity = "session-7";
            return facts;
        });
        CommandDispatcher::HangRecovery hooks;
        hooks.end_sap_gui = [this] {
            ++ended;
            if (end_fails) return Result::fail("SAP_GUI_RESTART_FAILED", "saplogon.exe (pid 1) did not end in time");
            logged_on = false;
            return Result::ok({{"ended", 1}});
        };
        dispatcher.set_session_target_resolver([this](const CommandDispatcher::TargetQuery&) {
            fairyfly::auth::SessionTarget target;
            target.connection_name = saved_connection_name;
            return target;
        });
        hooks.approval_pending = [this] { return approval_waiting; };
        dispatcher.set_hang_recovery(hooks);
    }
    ToolResult tcode(const std::string& code, CallContext c = Rig::ctx(), json extra = json::object()) {
        json args = {{"code", code}};
        for (auto it = extra.begin(); it != extra.end(); ++it) args[it.key()] = it.value();
        return dispatcher.call_tool("gui_transaction_start", args, c);
    }
    ToolResult trip_tcode(CallContext c = Rig::ctx(), json extra = json::object()) {
        tcode("/nSE38", c, extra);
        tcode("/nSE38", c, extra);
        return tcode("/nSE38", c, extra);
    }
};

json restart_of(const ToolResult& r) {
    const json error = error_of(r);
    return error.contains("restart") ? error["restart"] : json::object();
}

}  // namespace

TEST_CASE("MCP hang: without the startup checkbox nothing is ended and the error says how to allow it", "[mcp][hang][restart]") {
    RestartRig rig;
    rig.local_enabled = false;
    const auto third = rig.trip_tcode();
    INFO(text_of(third));
    REQUIRE(third.is_error);
    CHECK(error_of(third)["code"] == "SAP_GUI_UNRESPONSIVE");
    CHECK(restart_of(third)["allowed"] == false);
    CHECK(restart_of(third)["reason"] == "SAP_AUTOSTART_DISABLED");
    CHECK(text_of(third).find("Allow automatic SAP startup") != std::string::npos);
    CHECK(rig.ended == 0);
}

TEST_CASE("MCP hang: with the startup checkbox SAP GUI restarts once and the transaction runs", "[mcp][hang][restart]") {
    RestartRig rig;
    const auto third = rig.trip_tcode();
    INFO(text_of(third));
    CHECK(rig.ended == 1);
    CHECK(rig.launches == 1);
    REQUIRE_FALSE(third.is_error);
    REQUIRE(third.structured);
    CHECK(third.structured->dump().find("\"sap_gui_restarted\":true") != std::string::npos);
    CHECK(third.structured->dump().find("\"unsaved_input_lost\":true") != std::string::npos);
    CHECK(all_text(third).find("restarted") != std::string::npos);
}

TEST_CASE("MCP hang: a restarted session asks to observe the screen before a key runs again", "[mcp][hang][restart]") {
    RestartRig rig;
    rig.dispatcher.call_tool("gui_key_send", {{"key", "F3"}}, Rig::ctx());
    rig.dispatcher.call_tool("gui_key_send", {{"key", "F3"}}, Rig::ctx());
    const auto third = rig.dispatcher.call_tool("gui_key_send", {{"key", "F3"}}, Rig::ctx());
    INFO(text_of(third));
    CHECK(rig.ended == 1);
    REQUIRE(third.is_error);
    CHECK(text_of(third).find("SESSION_PREPARED") != std::string::npos);
    CHECK(third.structured->dump().find("\"sap_gui_restarted\":true") != std::string::npos);
}

TEST_CASE("MCP hang: an explicitly selected connection is never restarted", "[mcp][hang][restart]") {
    RestartRig rig;
    const auto third = rig.trip_tcode(Rig::ctx(), {{"connection", 7}});
    REQUIRE(third.is_error);
    CHECK(error_of(third)["code"] == "SAP_GUI_UNRESPONSIVE");
    CHECK(restart_of(third)["reason"] == "explicit_connection");
    CHECK(rig.ended == 0);
}

TEST_CASE("MCP hang: no restart while a desktop approval is waiting", "[mcp][hang][restart]") {
    RestartRig rig;
    rig.approval_waiting = true;
    const auto third = rig.trip_tcode();
    REQUIRE(third.is_error);
    CHECK(restart_of(third)["reason"] == "approval_pending");
    CHECK(rig.ended == 0);
}

TEST_CASE("MCP hang: one restart per episode, no loop when the new GUI hangs too", "[mcp][hang][restart]") {
    RestartRig rig;
    rig.hangs_after_restart = true;
    rig.trip_tcode();
    CHECK(rig.ended == 1);
    const auto again = rig.trip_tcode();
    INFO(text_of(again));
    REQUIRE(again.is_error);
    CHECK(error_of(again)["code"] == "SAP_GUI_UNRESPONSIVE");
    CHECK(restart_of(again)["reason"] == "already_restarted");
    CHECK(rig.ended == 1);
}

TEST_CASE("MCP hang: a gateway call without the startup grant is never restarted", "[mcp][hang][restart]") {
    RestartRig rig;
    auto c = Rig::ctx();
    c.link_request = true;
    c.sap_startup_granted = false;
    const auto third = rig.trip_tcode(c);
    REQUIRE(third.is_error);
    CHECK(restart_of(third)["reason"] == "SAP_AUTOSTART_ADMIN_DISABLED");
    CHECK(rig.ended == 0);
}

// ---- Stage D: the explicit gui_session_restart tool -------------------------------------------------------------

TEST_CASE("MCP restart tool: listed in write mode, hidden from a read-only server", "[mcp][hang][restart-tool]") {
    RestartRig write(true);
    const auto tools = write.dispatcher.list_tools();
    CHECK(std::any_of(tools.begin(), tools.end(), [](const ToolDef& t) { return t.name == "gui_session_restart"; }));
    RestartRig read_only(false);
    const auto hidden = read_only.dispatcher.list_tools();
    CHECK(std::none_of(hidden.begin(), hidden.end(), [](const ToolDef& t) { return t.name == "gui_session_restart"; }));
    const auto refused = read_only.dispatcher.call_tool("gui_session_restart", json::object(), Rig::ctx());
    CHECK(refused.is_error);
    CHECK(read_only.ended == 0);
}

TEST_CASE("MCP restart tool: with the startup checkbox it ends SAP GUI and prepares the default session", "[mcp][hang][restart-tool]") {
    RestartRig rig(true);
    const auto result = rig.dispatcher.call_tool("gui_session_restart", json::object(), Rig::ctx());
    INFO(text_of(result));
    REQUIRE_FALSE(result.is_error);
    CHECK(rig.ended == 1);
    CHECK(rig.launches == 1);
    REQUIRE(result.structured);
    CHECK((*result.structured)["data"]["sap_gui_restarted"] == true);
    CHECK((*result.structured)["data"]["unsaved_input_lost"] == true);
    CHECK((*result.structured)["data"]["connection_file_id"] == 7);
}

TEST_CASE("MCP restart tool: refused without the checkbox, the grant or while an approval waits", "[mcp][hang][restart-tool]") {
    RestartRig rig(true);
    auto c = Rig::ctx();
    std::string expected;
    SECTION("checkbox off") {
        rig.local_enabled = false;
        expected = "SAP_AUTOSTART_DISABLED";
    }
    SECTION("gateway without grant") {
        c.link_request = true;
        expected = "SAP_AUTOSTART_ADMIN_DISABLED";
    }
    SECTION("approval waiting") {
        rig.approval_waiting = true;
        expected = "SAP_APPROVAL_PENDING";
    }
    const auto result = rig.dispatcher.call_tool("gui_session_restart", json::object(), c);
    INFO(text_of(result));
    REQUIRE(result.is_error);
    CHECK(text_of(result).find(expected) != std::string::npos);
    CHECK(rig.ended == 0);
}

// ---- Review follow-up: never end SAP GUI sessions of another SAP Logon entry ------------------------------------

namespace {
json connection_of(const std::string& description) {
    return {{"description", description}, {"id", "/app/con[0]"},
            {"sessions", json::array({{{"id", "/app/con[0]/ses[0]"}, {"busy", false}, {"alive", true}}})}};
}
}  // namespace

TEST_CASE("MCP hang: SAP GUI is not ended while a session of another SAP Logon entry is open", "[mcp][hang][restart]") {
    RestartRig rig;
    rig.open_connections = json::array({connection_of("Bigfox"), connection_of("PRD Production")});
    const auto third = rig.trip_tcode();
    INFO(text_of(third));
    REQUIRE(third.is_error);
    CHECK(error_of(third)["code"] == "SAP_GUI_UNRESPONSIVE");
    CHECK(restart_of(third)["reason"] == "other_sessions_open");
    CHECK(rig.ended == 0);
}

TEST_CASE("MCP hang: sessions of the default entry alone do not block the restart", "[mcp][hang][restart]") {
    RestartRig rig;
    rig.open_connections = json::array({connection_of("Bigfox"), connection_of(" bigfox ")});
    const auto third = rig.trip_tcode();
    INFO(text_of(third));
    CHECK(rig.ended == 1);
    CHECK_FALSE(third.is_error);
}

TEST_CASE("MCP restart tool: refused while a session of another SAP Logon entry is open", "[mcp][hang][restart-tool]") {
    RestartRig rig(true);
    rig.open_connections = json::array({connection_of("Bigfox"), connection_of("QAS")});
    const auto result = rig.dispatcher.call_tool("gui_session_restart", json::object(), Rig::ctx());
    INFO(text_of(result));
    REQUIRE(result.is_error);
    CHECK(text_of(result).find("SAP_RESTART_OTHER_SESSIONS") != std::string::npos);
    CHECK(rig.ended == 0);
}

// ---- Codex review 2026-11-22 ------------------------------------------------------------------------------------

TEST_CASE("MCP hang: a read-only call never ends SAP GUI", "[mcp][hang][restart][review]") {
    RestartRig rig;
    auto c = Rig::ctx();
    c.principal.read_only = true;
    SECTION("read-only token") {}
    SECTION("read-only server") {
        c.principal.read_only = false;
        rig.dispatcher.set_read_only(true);
    }
    const auto third = rig.trip_tcode(c);
    INFO(text_of(third));
    REQUIRE(third.is_error);
    CHECK(error_of(third)["code"] == "SAP_GUI_UNRESPONSIVE");
    CHECK(restart_of(third)["reason"] == "read_only");
    CHECK(rig.ended == 0);
}

TEST_CASE("MCP hang: a session attached from another SAP Logon entry is never restarted", "[mcp][hang][restart][review]") {
    RestartRig rig(true);
    rig.saved_connection_name = "PRD Production";
    const auto attached = rig.dispatcher.call_tool("gui_session_attach", {{"session_id", "/app/con[0]/ses[0]"}}, Rig::ctx());
    INFO(text_of(attached));
    REQUIRE_FALSE(attached.is_error);
    const auto third = rig.trip_tcode(Rig::ctx());
    INFO(text_of(third));
    REQUIRE(third.is_error);
    CHECK(restart_of(third)["reason"] == "explicit_connection");
    CHECK(rig.ended == 0);
}

TEST_CASE("MCP hang: a session attached from the default entry may be restarted", "[mcp][hang][restart][review]") {
    RestartRig rig(true);
    const auto attached = rig.dispatcher.call_tool("gui_session_attach", {{"session_id", "/app/con[0]/ses[0]"}}, Rig::ctx());
    INFO(text_of(attached));
    REQUIRE_FALSE(attached.is_error);
    const auto third = rig.trip_tcode(Rig::ctx());
    INFO(text_of(third));
    CHECK(rig.ended == 1);
}

TEST_CASE("MCP hang: reading the screen after SAP_GUI_UNRESPONSIVE runs no recovery", "[mcp][hang][review]") {
    Rig rig;
    REQUIRE(error_of(trip(rig))["code"] == "SAP_GUI_UNRESPONSIVE");
    const int closes = rig.gui.count("popup", "close");
    const auto read = rig.dispatcher.call_tool("gui_screen_read", json::object(), Rig::ctx());
    INFO(text_of(read));
    CHECK_FALSE(read.is_error);
    CHECK(rig.gui.count("popup", "close") == closes);
    CHECK(rig.gui.count("key", "send") == 3 + 1);   // the three F3 and the one F12 of the first recovery
}

TEST_CASE("MCP hang: a transaction SAP keeps rejecting is not a hung GUI", "[mcp][hang][review]") {
    Rig rig;
    rig.gui.on_other = [](const Argv&) { return Result::ok(json::object()); };
    CommandDispatcher& d = rig.dispatcher;
    Result rejected = Result::fail("TRANSACTION_NOT_STARTED", "Requested transaction did not become active");
    rejected.error["session_busy"] = false;
    rejected.error["screen_changed"] = false;
    rejected.error["statusbar_message"] = "Transaction ZNOPE does not exist";
    rejected.error["message_type"] = "E";
    rejected.error["statusbar_unchanged"] = true;
    CHECK(fairyfly::hang::classify("gui_transaction_start", {{"code", "ZNOPE"}}, rejected).signal ==
          fairyfly::hang::Signal::Neutral);
    (void)d;
}

TEST_CASE("MCP hang: a failed attempt to end SAP GUI uses the episode's restart", "[mcp][hang][restart][review]") {
    RestartRig rig;
    rig.end_fails = true;
    const auto first = rig.trip_tcode();
    INFO(text_of(first));
    REQUIRE(first.is_error);
    CHECK(rig.ended == 1);
    const auto second = rig.trip_tcode();
    INFO(text_of(second));
    CHECK(restart_of(second)["reason"] == "already_restarted");
    CHECK(rig.ended == 1);
}

// ---- Second variant: a call that never returns (the watch decides at the deadline) -----------------------------

namespace {

/// A RestartRig whose SAP GUI blocks inside one call: the test plays the transport's watchdog from inside the call.
struct BlockingRig : RestartRig {
    std::optional<json> deadline_answer;
    std::function<void()> during_block;
    std::string deadline_tool = "gui_screen_read";   ///< the tools/call name the transport reports
    bool frozen = true;
    int forgotten = 0;
    std::vector<McpCallRecord> audited;
    explicit BlockingRig(bool write = true) : RestartRig(write) {
        CommandDispatcher::HangRecovery hooks;
        hooks.end_sap_gui = [this] {
            ++ended;
            logged_on = false;
            frozen = false;
            return Result::ok({{"ended", 1}});
        };
        hooks.approval_pending = [this] { return approval_waiting; };
        hooks.frontend = [this] {
            fairyfly::hang::FrontendState s;
            s.sap_gui_running = logged_on || frozen;
            s.frontend_hung = frozen;
            return s;
        };
        hooks.after_external_end = [this] { ++forgotten; };
        dispatcher.set_hang_recovery(hooks);
        dispatcher.set_blocked_call_confirm_delay(std::chrono::milliseconds(0));
        dispatcher.set_audit_hook([this](const McpCallRecord& r) { audited.push_back(r); });
        gui.on_other = [this, previous = gui.on_other](const Argv& argv) {
            if (argv.size() > 1 && argv[0] == "screen" && argv[1] == "read" && frozen) {
                if (during_block) during_block();
                deadline_answer = dispatcher.on_call_deadline(deadline_tool, 0);   // the watchdog thread
                return Result::fail("COM_ERROR", "GetObjectTree failed: 0x800706BA");          // SAP GUI was ended
            }
            return previous(argv);
        };
    }
};

}  // namespace

TEST_CASE("MCP blocked call: a frozen SAP GUI is ended at the deadline when the restart is allowed", "[mcp][hang][watch]") {
    BlockingRig rig;
    rig.open_connections = json::array({connection_of("Bigfox")});
    REQUIRE_FALSE(rig.dispatcher.call_tool("gui_session_list", json::object(), Rig::ctx()).is_error);   // known sessions
    const auto blocked = rig.dispatcher.call_tool("gui_screen_read", json::object(), Rig::ctx());
    REQUIRE(rig.deadline_answer.has_value());
    CHECK((*rig.deadline_answer)["code"] == "SAP_GUI_RESTARTED");
    CHECK(rig.ended == 1);
    CHECK(rig.forgotten == 1);              // the handler dropped its COM objects on the COM thread
    // the next implicit call prepares the default session again (startup path)
    const auto next = rig.dispatcher.call_tool("gui_screen_read", json::object(), Rig::ctx());
    INFO(rig.dispatcher.call_tool("gui_session_list", json::object(), Rig::ctx()).content.dump());
    CHECK(rig.launches == 1);
    CHECK_FALSE(next.is_error);
}

TEST_CASE("MCP blocked call: no restart at the deadline without permission", "[mcp][hang][watch]") {
    BlockingRig rig;
    rig.open_connections = json::array({connection_of("Bigfox")});
    std::string expected;
    auto c = Rig::ctx();
    SECTION("checkbox off") { rig.local_enabled = false; expected = "SAP_AUTOSTART_DISABLED"; }
    SECTION("read-only call") { c.principal.read_only = true; expected = "read_only"; }
    SECTION("approval waiting") { rig.approval_waiting = true; expected = "approval_pending"; }
    SECTION("explicit connection") { expected = "explicit_connection"; }
    SECTION("other SAP Logon entry open") {
        rig.open_connections = json::array({connection_of("Bigfox"), connection_of("PRD")});
        expected = "other_sessions_open";
    }
    SECTION("sessions never listed") { rig.open_connections = json::array(); expected = "other_sessions_unknown"; }
    if (expected != "other_sessions_unknown")
        REQUIRE_FALSE(rig.dispatcher.call_tool("gui_session_list", json::object(), c).is_error);
    json args = json::object();
    if (expected == "explicit_connection") args["connection"] = 7;
    rig.dispatcher.call_tool("gui_screen_read", args, c);
    REQUIRE(rig.deadline_answer.has_value());
    INFO(rig.deadline_answer->dump());
    CHECK((*rig.deadline_answer)["code"] == "SAP_GUI_NOT_RESPONDING");
    CHECK((*rig.deadline_answer)["restart"]["reason"] == expected);
    CHECK(rig.ended == 0);
}

TEST_CASE("MCP blocked call: SAP GUI waiting for the server is never ended", "[mcp][hang][watch]") {
    BlockingRig rig;
    rig.open_connections = json::array({connection_of("Bigfox")});
    REQUIRE_FALSE(rig.dispatcher.call_tool("gui_session_list", json::object(), Rig::ctx()).is_error);
    CommandDispatcher::HangRecovery hooks;
    hooks.end_sap_gui = [&rig] { ++rig.ended; return Result::ok(); };
    hooks.frontend = [] { return fairyfly::hang::FrontendState{true, false, json::object()}; };
    rig.dispatcher.set_hang_recovery(hooks);
    rig.dispatcher.call_tool("gui_screen_read", json::object(), Rig::ctx());
    REQUIRE(rig.deadline_answer.has_value());
    CHECK((*rig.deadline_answer)["code"] == "SAP_SERVER_NOT_RESPONDING");
    CHECK(rig.ended == 0);
}

TEST_CASE("MCP blocked call: outside a call the deadline has nothing to say", "[mcp][hang][watch]") {
    BlockingRig rig;
    CHECK_FALSE(rig.dispatcher.on_call_deadline("gui_screen_read", 0).has_value());
}

// ---- Codex review 2026-11-22 (second variant) -------------------------------------------------------------------

TEST_CASE("MCP blocked call: a desktop approval that appears during the call prevents the end", "[mcp][hang][watch][review2]") {
    BlockingRig rig;
    rig.open_connections = json::array({connection_of("Bigfox")});
    REQUIRE_FALSE(rig.dispatcher.call_tool("gui_session_list", json::object(), Rig::ctx()).is_error);
    rig.during_block = [&rig] { rig.approval_waiting = true; };
    rig.dispatcher.call_tool("gui_screen_read", json::object(), Rig::ctx());
    REQUIRE(rig.deadline_answer.has_value());
    CHECK((*rig.deadline_answer)["restart"]["reason"] == "approval_pending");
    CHECK(rig.ended == 0);
}

TEST_CASE("MCP blocked call: a listing older than a lifecycle change is refreshed", "[mcp][hang][watch][review2]") {
    BlockingRig rig;
    rig.open_connections = json::array({connection_of("Bigfox")});
    REQUIRE_FALSE(rig.dispatcher.call_tool("gui_session_list", json::object(), Rig::ctx()).is_error);
    // another SAP Logon entry is opened through fairyfly: its lifecycle call refreshes what is known
    rig.open_connections = json::array({connection_of("Bigfox"), connection_of("PRD")});
    REQUIRE_FALSE(rig.dispatcher.call_tool("gui_session_attach", {{"session_id", "/app/con[1]/ses[0]"}}, Rig::ctx()).is_error);
    rig.dispatcher.call_tool("gui_screen_read", json::object(), Rig::ctx());
    REQUIRE(rig.deadline_answer.has_value());
    CHECK((*rig.deadline_answer)["restart"]["reason"] != nullptr);
    CHECK(rig.ended == 0);
}

TEST_CASE("MCP blocked call: the blocked call itself is audited and answered as SAP_GUI_RESTARTED", "[mcp][hang][watch][review2]") {
    BlockingRig rig;
    rig.open_connections = json::array({connection_of("Bigfox")});
    REQUIRE_FALSE(rig.dispatcher.call_tool("gui_session_list", json::object(), Rig::ctx()).is_error);
    const auto late = rig.dispatcher.call_tool("gui_screen_read", json::object(), Rig::ctx());
    REQUIRE(rig.ended == 1);
    INFO(late.content.dump());
    CHECK(late.is_error);
    CHECK(late.content.dump().find("SAP_GUI_RESTARTED") != std::string::npos);
    REQUIRE_FALSE(rig.audited.empty());
    CHECK(rig.audited.back().error_code == "SAP_GUI_RESTARTED");
}

TEST_CASE("MCP blocked call: a batch stops after SAP GUI was ended", "[mcp][hang][watch][review2]") {
    BlockingRig rig(true);
    rig.open_connections = json::array({connection_of("Bigfox")});
    REQUIRE_FALSE(rig.dispatcher.call_tool("gui_session_list", json::object(), Rig::ctx()).is_error);
    rig.deadline_tool = "gui_batch";
    const json batch = {{"stop_on_error", false},
                        {"items", json::array({{{"tool", "gui_screen_read"}, {"arguments", json::object()}},
                                               {{"tool", "gui_key_send"}, {"arguments", {{"key", "F3"}}}}})}};
    const auto out = rig.dispatcher.call_tool("gui_batch", batch, Rig::ctx());
    INFO(out.content.dump());
    INFO((rig.deadline_answer ? rig.deadline_answer->dump() : std::string("no deadline answer")));
    REQUIRE(rig.ended == 1);
    CHECK(rig.gui.count("key", "send") == 0);
}

TEST_CASE("MCP blocked call: only a working SAP screen call renews the restart", "[mcp][hang][watch][review2]") {
    BlockingRig rig;
    rig.open_connections = json::array({connection_of("Bigfox")});
    REQUIRE_FALSE(rig.dispatcher.call_tool("gui_session_list", json::object(), Rig::ctx()).is_error);
    rig.dispatcher.call_tool("gui_screen_read", json::object(), Rig::ctx());
    REQUIRE(rig.ended == 1);
    // a call that does not touch SAP succeeds; then the prepared GUI freezes again
    REQUIRE_FALSE(rig.dispatcher.call_tool("gui_credentials_list", json::object(), Rig::ctx()).is_error);
    rig.frozen = true;
    rig.logged_on = true;
    rig.deadline_answer.reset();
    rig.dispatcher.call_tool("gui_screen_read", json::object(), Rig::ctx());
    REQUIRE(rig.deadline_answer.has_value());
    CHECK((*rig.deadline_answer)["restart"]["reason"] == "already_restarted");
    CHECK(rig.ended == 1);
}
