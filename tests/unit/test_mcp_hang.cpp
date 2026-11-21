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
    std::function<Result(const Argv&)> on_other = [](const Argv&) { return Result::ok(json::object()); };

    Result operator()(const Argv& argv) {
        calls.push_back(argv);
        if (argv.size() > 1 && argv[0] == "key" && argv[1] == "send") {
            auto r = noop_key(argv);
            if (!hung) r.data["screen_changed"] = true;
            return r;
        }
        if (argv.size() > 1 && argv[0] == "transaction" && argv[1] == "start") {
            if (!hung) return Result::ok({{"actual_tcode", "SESSION_MANAGER"}});
            return tcode_not_started();
        }
        if (argv.size() > 1 && argv[0] == "popup" && argv[1] == "close")
            return Result::fail("NO_POPUP", "No popup window is open");
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
