// CommandHandler with an injected engine and connection store (review 2026-10-25: it built both itself, so none of
// its ~30 handlers could be tested without SAP GUI).
#include <catch2/catch_test_macros.hpp>

#include "include/cli_handler.h"
#include "include/connection_manager.h"
#include "include/fill_fields.h"
#include "include/credential_store.h"
#include "support/fake_engine.h"
#include "support/temp_dir.h"

#include <memory>

using namespace fairyfly;
using fairyfly::test::FakeEngine;
using fairyfly::test::TempDir;

namespace {
constexpr const char* kSession = "/app/con[0]/ses[0]";

struct Rig {
    TempDir dir{"handler"};
    FakeEngine* engine = nullptr;  // owned by the handler
    std::unique_ptr<cli::CommandHandler> handler;

    explicit Rig(bool with_connection = true, const std::string& server_key = "") {
        auto fake = std::make_unique<FakeEngine>();
        engine = fake.get();
        auto connections = std::make_unique<cli::ConnectionManager>(dir.path().string());
        if (with_connection) connections->create_or_update_connection(kSession, "/app/con[0]", "BIG", "route", "SAP", server_key);
        handler = std::make_unique<cli::CommandHandler>(std::make_unique<cred::InMemoryCredentialStore>(), std::move(fake),
                                                        std::move(connections));
    }
};

std::string code_of(const Result& r) { return r.error.is_object() ? r.error.value("code", std::string()) : std::string(); }
}  // namespace

TEST_CASE("command handler: a fill under --read-only is refused before anything reaches SAP", "[handler]") {
    Rig rig;
    rig.engine->live_sessions[kSession] = true;
    rig.handler->set_read_only(true);
    const Result r = rig.handler->handle_fill("wnd[0]/usr/txtA", "secret", std::nullopt);
    CHECK(r.status == Result::Status::Error);
    CHECK(code_of(r) == "READ_ONLY_REFUSED");
    CHECK(r.to_json().dump().find("secret") == std::string::npos);  // the value is never echoed
    CHECK(rig.engine->calls.empty());
}

TEST_CASE("SAP Logon startup requires opt in before process creation", "[handler][startup]") {
    Rig rig(false);
    rig.engine->status = {false, false, "not running"};
    const auto result = rig.handler->ensure_sap_logon({false, false, false, "Bigfox"});
    CHECK(code_of(result) == "SAP_AUTOSTART_DISABLED");
    CHECK(rig.engine->count("start_sap_logon") == 0);
}

TEST_CASE("CLI task reports disabled startup before touching SAP", "[handler][startup]") {
    Rig rig(false);
    rig.engine->status = {false, false, "not running"};
    rig.handler->set_automatic_startup([] { return startup::Policy{false, false, false, "Bigfox"}; });
    const auto result = rig.handler->handle_transaction("SE11", std::nullopt);
    CHECK(code_of(result) == "SAP_AUTOSTART_DISABLED");
    CHECK(rig.engine->count("start_sap_logon") == 0);
    CHECK(rig.engine->count("execute_transaction") == 0);
}

TEST_CASE("CLI stale and logged-out saved sessions respect startup permission", "[handler][startup][startup-recovery]") {
    Rig rig;
    fairyfly::test::FakeScripting scripting;
    rig.engine->scripting_fake = &scripting;
    rig.engine->application_info = {{"total_connections", 0}, {"total_sessions", 0}, {"connections", json::array()}};
    SECTION("stale saved connection") {}
    SECTION("existing logged-out session") {
        rig.engine->live_sessions[kSession] = true;
        scripting.facts.system = "A4H"; scripting.facts.client = "001";
        rig.engine->application_info = {{"total_connections", 1}, {"total_sessions", 1},
            {"connections", json::array({{{"description", "BIG"}, {"sessions", json::array({{{"id", kSession}, {"user", ""}}})}}})}};
    }
    rig.handler->set_automatic_startup([] { return startup::Policy{false, false, false, "BIG"}; });
    const auto result = rig.handler->handle_transaction("SE11", std::nullopt);
    CHECK(code_of(result) == "SAP_AUTOSTART_DISABLED");
    CHECK(rig.engine->count("execute_transaction") == 0);
    CHECK(rig.engine->count("launch_connection") == 0);
}

TEST_CASE("CLI verified manual login clears the matching retry block", "[handler][startup][startup-recovery]") {
    Rig rig;
    fairyfly::test::FakeScripting scripting;
    scripting.facts.system = "A4H"; scripting.facts.client = "001"; scripting.facts.user = "DEVELOPER";
    scripting.connection_description = "BIG";
    rig.engine->scripting_fake = &scripting;
    rig.engine->live_sessions[kSession] = true;
    auto attempts = std::make_shared<startup::LoginAttempts>(rig.dir.path());
    REQUIRE(attempts->begin("BIG").status == Result::Status::Success);
    REQUIRE(attempts->begin("Other").status == Result::Status::Success);
    rig.handler->set_automatic_startup([] { return startup::Policy{false, false, false, "BIG"}; }, attempts);
    REQUIRE(rig.handler->handle_transaction("SE11", std::nullopt).status == Result::Status::Success);
    CHECK(attempts->begin("BIG").status == Result::Status::Success);
    CHECK(attempts->begin("Other").status == Result::Status::Error);
}

TEST_CASE("CLI implicit startup cannot replace a saved target with another default", "[handler][startup][startup-recovery]") {
    Rig rig;
    rig.engine->application_info = {{"total_connections", 0}, {"total_sessions", 0}, {"connections", json::array()}};
    rig.handler->set_automatic_startup([] { return startup::Policy{true, false, false, "Other"}; });
    const auto result = rig.handler->handle_transaction("SE11", std::nullopt);
    CHECK(code_of(result) == "SESSION_TARGET_CHANGED");
    CHECK(rig.engine->count("execute_transaction") == 0);
    CHECK(rig.engine->count("launch_connection") == 0);
}

TEST_CASE("CLI session preparation reuses logged-on default without launching", "[handler][startup]") {
    Rig rig(false);
    fairyfly::test::FakeScripting scripting;
    scripting.facts.system = "A4H";
    scripting.facts.client = "001";
    scripting.facts.user = "DEVELOPER";
    rig.engine->scripting_fake = &scripting;
    rig.engine->live_sessions[kSession] = true;
    rig.engine->application_info = {{"total_connections", 1}, {"total_sessions", 1},
        {"connections", json::array({{{"id", "/app/con[0]"}, {"description", "Bigfox"},
            {"sessions", json::array({{{"id", kSession}, {"user", "DEVELOPER"}, {"system", "A4H"}, {"client", "001"}}})}}})}};
    rig.engine->next = Result::ok({{"session_id", kSession}, {"connection_id", "/app/con[0]"}});
    const auto result = rig.handler->prepare_default_session({false, false, false, "Bigfox"});
    CHECK(result.status == Result::Status::Success);
    CHECK(rig.engine->count("attach_by_session_id") == 1);
    CHECK(rig.engine->count("start_sap_logon") == 0);
    CHECK(rig.engine->count("launch_connection") == 0);
}

TEST_CASE("CLI does not click an unobserved screen after automatic startup", "[handler][startup][startup-observation]") {
    Rig rig(false);
    fairyfly::test::FakeScripting scripting;
    scripting.facts.system = "A4H"; scripting.facts.client = "001"; scripting.facts.user = "DEVELOPER";
    scripting.connection_description = "Bigfox";
    rig.engine->scripting_fake = &scripting;
    rig.engine->live_sessions[kSession] = true;
    rig.engine->application_info = {{"connections", json::array()}, {"total_connections", 0}, {"total_sessions", 0}};
    rig.engine->next = Result::ok({{"session_id", kSession}, {"connection_id", "/app/con[0]"}, {"description", "Bigfox"}});
    rig.handler->set_automatic_startup([] { return startup::Policy{true, false, false, "Bigfox"}; });
    const auto result = rig.handler->handle_click("@active/tbar[0]/btn[0]", std::nullopt);
    CHECK(code_of(result) == "SESSION_PREPARED");
    CHECK(result.error.value("connection_open", false));
    CHECK(rig.engine->count("click_element") == 0);
}

TEST_CASE("Running SAP Logon never needs startup permission", "[handler][startup]") {
    Rig rig(false);
    const auto result = rig.handler->ensure_sap_logon({false, true, false, ""});
    CHECK(result.status == Result::Status::Success);
    CHECK(rig.engine->count("start_sap_logon") == 0);
}

TEST_CASE("SAP Logon already starting is awaited even when automatic startup is disabled", "[handler][startup]") {
    Rig rig(false);
    rig.engine->status = {false, true, "starting"};
    rig.engine->on_call = [&](const std::string& call) {
        if (call == "start_sap_logon") rig.engine->status = {true, true, "ready"};
    };
    const auto result = rig.handler->ensure_sap_logon({false, true, false, ""});
    CHECK(result.status == Result::Status::Success);
    CHECK(rig.engine->count("start_sap_logon") == 1);
    CHECK_FALSE(rig.engine->startup_may_create_process);
}

TEST_CASE("SAP Logon startup reports engine failures", "[handler][startup]") {
    Rig rig(false);
    rig.engine->status = {false, false, "not running"};
    rig.engine->next = Result::fail("SAP_LOGON_NOT_INSTALLED", "Install SAP GUI.");
    const auto result = rig.handler->ensure_sap_logon({true, false, false, "Bigfox"});
    CHECK(code_of(result) == "SAP_LOGON_NOT_INSTALLED");
    CHECK(rig.engine->count("start_sap_logon") == 1);
}

TEST_CASE("SAP Logon startup requires verified scripting readiness", "[handler][startup]") {
    Rig rig(false);
    rig.engine->status = {false, false, "not running"};
    const auto result = rig.handler->ensure_sap_logon({true, false, false, "Bigfox"});
    CHECK(code_of(result) == "SAP_STARTUP_NOT_READY");
    CHECK(rig.engine->count("start_sap_logon") == 1);
}

TEST_CASE("command handler: without a saved connection nothing is sent to SAP", "[handler]") {
    Rig rig(false);
    const Result r = rig.handler->handle_fill("wnd[0]/usr/txtA", "x", std::nullopt);
    CHECK(code_of(r) == "NO_CONNECTIONS");
    CHECK(rig.engine->count("fill_field") == 0);
}

TEST_CASE("command handler: a connection whose session is gone is reported and forgotten", "[handler]") {
    Rig rig;  // the session is not alive in the fake
    const Result r = rig.handler->handle_fill("wnd[0]/usr/txtA", "x", 0);
    CHECK(code_of(r) == "INVALID_CONNECTION");
    CHECK(rig.engine->count("fill_field") == 0);
    // forgotten: the next call has no connection at all
    CHECK(code_of(rig.handler->handle_fill("wnd[0]/usr/txtA", "x", std::nullopt)) == "NO_CONNECTIONS");
}

TEST_CASE("command handler: a fill reaches the engine on the resolved session", "[handler]") {
    Rig rig;
    rig.engine->live_sessions[kSession] = true;
    const Result r = rig.handler->handle_fill("wnd[0]/usr/txtA", "hello", 0);
    CHECK(r.status == Result::Status::Success);
    CHECK(rig.engine->count("select_session /app/con[0]/ses[0]") >= 1);
    CHECK(rig.engine->count("fill_field wnd[0]/usr/txtA=hello") == 1);
}

TEST_CASE("command handler: an engine error is passed on with its code", "[handler]") {
    Rig rig;
    rig.engine->live_sessions[kSession] = true;
    rig.engine->next = Result::fail("ELEMENT_NOT_FOUND", "no such element");
    const Result r = rig.handler->handle_fill("wnd[0]/usr/txtNOPE", "x", 0);
    CHECK(code_of(r) == "ELEMENT_NOT_FOUND");
}

TEST_CASE("command handler: key send needs SAP GUI scripting", "[handler]") {
    Rig rig;
    rig.engine->live_sessions[kSession] = true;
    const Result r = rig.handler->handle_send_key("enter", "", 0);
    CHECK(code_of(r) == "ENGINE_TYPE_MISMATCH");
}

TEST_CASE("command handler: a transaction is started on the resolved session", "[handler]") {
    Rig rig;
    rig.engine->live_sessions[kSession] = true;
    const Result r = rig.handler->handle_transaction("SU01", 0);
    CHECK(r.status == Result::Status::Success);
    CHECK(rig.engine->count("execute_transaction SU01") == 1);
}

TEST_CASE("command handler: keys under --read-only: an allowlist, judged before SAP is asked", "[handler][readonly]") {
    Rig rig;
    rig.engine->live_sessions[kSession] = true;
    test::FakeScripting scripting;
    rig.engine->scripting_fake = &scripting;
    rig.handler->set_read_only(true);
    // F11 saves: refused without touching SAP
    CHECK(code_of(rig.handler->handle_send_key("f11", "", 0)) == "READ_ONLY_REFUSED");
    CHECK(scripting.calls.empty());
    // Enter on the main window is allowed
    CHECK(rig.handler->handle_send_key("enter", "", 0).status == Result::Status::Success);
    CHECK(scripting.calls == std::vector<std::string>{"send_key 0 "});
    // Enter on a popup could confirm a save or delete dialog
    rig.engine->active_window = "wnd[1]";
    CHECK(code_of(rig.handler->handle_send_key("enter", "", 0)) == "READ_ONLY_REFUSED");
    CHECK(scripting.calls.size() == 1);
}

TEST_CASE("command handler: a page key SAP rejects in a grid is explained, not a raw COM error", "[handler]") {
    Rig rig;
    rig.engine->live_sessions[kSession] = true;
    test::FakeScripting scripting;
    scripting.next = Result::fail("COM_ERROR", "0x80020009");
    rig.engine->scripting_fake = &scripting;
    const Result r = rig.handler->handle_send_key("pagedown", "", 0);
    CHECK(code_of(r) == "KEY_NOT_APPLICABLE");
    CHECK(r.error.contains("hint"));
}

TEST_CASE("command handler: popup close under --read-only refuses Enter (it would confirm), allows F12", "[handler][readonly]") {
    Rig rig;
    rig.engine->live_sessions[kSession] = true;
    test::FakeScripting scripting;
    rig.engine->scripting_fake = &scripting;
    rig.handler->set_read_only(true);
    CHECK(code_of(rig.handler->handle_close(0, 0)) == "READ_ONLY_REFUSED");
    CHECK(scripting.calls.empty());
    CHECK(rig.handler->handle_close(12, 0).status == Result::Status::Success);
    CHECK(scripting.calls == std::vector<std::string>{"close_popup 12"});
}

// ---- the SAP session behind a saved connection replaced or closed (live 2026-10-28) ----------------------------------
// Connection 1 was saved for /app/con[0]/ses[0]; SAP was ended and logged on again, so a new session sat at the same
// path. Key send and the okcd fill reported success, and only the next screen read noticed that the saved connection
// no longer named that session. An action must never report success for a session other than the one it checked.

TEST_CASE("command handler: a session replaced at the same path before the action is refused, nothing is sent", "[handler][session]") {
    Rig rig(true, "SID-OLD:1");
    rig.engine->live_sessions[kSession] = true;
    rig.engine->session_keys[kSession] = "SID-NEW:1";   // SAP logged on again: same path, another server session
    test::FakeScripting scripting;
    rig.engine->scripting_fake = &scripting;
    CHECK(code_of(rig.handler->handle_send_key("enter", "", 0)) == "INVALID_CONNECTION");
    CHECK(scripting.calls.empty());
}

TEST_CASE("command handler: a session replaced while the action ran is SESSION_CHANGED, not success", "[handler][session]") {
    Rig rig(true, "SID-OLD:1");
    rig.engine->live_sessions[kSession] = true;
    rig.engine->session_keys[kSession] = "SID-OLD:1";
    test::FakeScripting scripting;
    rig.engine->scripting_fake = &scripting;
    const auto replace = [&](const std::string&) { rig.engine->session_keys[kSession] = "SID-NEW:1"; };
    rig.engine->on_call = replace;
    scripting.on_call = replace;
    const auto check_changed = [&](const Result& r) {
        INFO(r.to_json().dump());
        CHECK(r.status == Result::Status::Error);
        CHECK(code_of(r) == "SESSION_CHANGED");
        CHECK(r.error.value("session_id", std::string()) == kSession);
        CHECK(r.error.value("message", std::string()).find("replaced") != std::string::npos);
        CHECK(r.error.contains("hint"));
        // what the action itself answered stays as evidence (a status bar like "Document 4711 posted")
        REQUIRE(r.error.contains("action_result"));
        CHECK(r.error.at("action_result").value("status", std::string()) == "success");
        rig.engine->session_keys[kSession] = "SID-OLD:1";   // the next action starts on the checked session again
    };
    SECTION("key send") { check_changed(rig.handler->handle_send_key("enter", "", 0)); }
    SECTION("transaction") { check_changed(rig.handler->handle_transaction("SU01", 0)); }
    SECTION("fill") { check_changed(rig.handler->handle_fill("wnd[0]/tbar[0]/okcd", "/nSU01", 0)); }
    SECTION("click") { check_changed(rig.handler->handle_click("wnd[0]/tbar[0]/btn[0]", 0)); }
    SECTION("popup close") {
        rig.engine->active_window = "wnd[1]";
        check_changed(rig.handler->handle_close(12, 0));
    }
}

TEST_CASE("command handler: an action that closes its own session still reports its outcome", "[handler][session]") {
    // e.g. /nex: the session is gone afterwards, which the action caused; say so instead of failing
    Rig rig(true, "SID-OLD:1");
    rig.engine->live_sessions[kSession] = true;
    rig.engine->session_keys[kSession] = "SID-OLD:1";
    rig.engine->on_call = [&](const std::string&) { rig.engine->live_sessions[kSession] = false; };
    const Result r = rig.handler->handle_transaction("/nex", 0);
    INFO(r.to_json().dump());
    CHECK(r.status == Result::Status::Success);
    CHECK(r.data.value("session_closed", false));
}

TEST_CASE("command handler: an unchanged session keeps the action's success", "[handler][session]") {
    Rig rig(true, "SID-OLD:1");
    rig.engine->live_sessions[kSession] = true;
    rig.engine->session_keys[kSession] = "SID-OLD:1";
    test::FakeScripting scripting;
    rig.engine->scripting_fake = &scripting;
    const Result r = rig.handler->handle_send_key("enter", "", 0);
    CHECK(r.status == Result::Status::Success);
    CHECK_FALSE(r.data.contains("session_closed"));
    // a connection saved without a key (older file) is learned on first use and then checked by it
    Rig keyless;
    keyless.engine->live_sessions[kSession] = true;
    keyless.engine->server_key = "SID-OLD:1";
    keyless.engine->session_keys[kSession] = "SID-OLD:1";
    keyless.engine->on_call = [&](const std::string&) { keyless.engine->session_keys[kSession] = "SID-NEW:1"; };
    CHECK(code_of(keyless.handler->handle_transaction("SU01", 0)) == "SESSION_CHANGED");
}

TEST_CASE("command handler: an unreadable session key after the action is inconclusive, never SESSION_CHANGED", "[handler][session]") {
    // SAP still in a round trip or tearing down (/nend, log off): Info.SystemSessionId reads empty. A save that
    // succeeded must not be reported as failed (the caller would repeat it).
    Rig rig(true, "SID-OLD:1");
    rig.engine->live_sessions[kSession] = true;
    rig.engine->session_keys[kSession] = "SID-OLD:1";
    rig.engine->on_call = [&](const std::string&) { rig.engine->session_keys[kSession] = ""; };
    const Result r = rig.handler->handle_transaction("SU01", 0);
    INFO(r.to_json().dump());
    CHECK(r.status == Result::Status::Success);
    CHECK(r.data.value("session_check", std::string()) == "inconclusive");
    REQUIRE(r.data.contains("warnings"));
    CHECK(r.data.at("warnings").dump().find("could not be confirmed") != std::string::npos);
    CHECK_FALSE(r.data.contains("session_closed"));
}

TEST_CASE("command handler: fill with fields checks the session once per call", "[handler][session]") {
    Rig rig(true, "SID-OLD:1");
    rig.engine->live_sessions[kSession] = true;
    rig.engine->session_keys[kSession] = "SID-OLD:1";
    test::FakeScripting scripting;
    rig.engine->scripting_fake = &scripting;
    std::vector<sap::FillField> fields(3);
    fields[0].element = "wnd[0]/usr/txtA"; fields[0].value = "1";
    fields[1].element = "wnd[0]/usr/txtB"; fields[1].value = "2";
    fields[2].element = "wnd[0]/usr/txtC"; fields[2].value = "3";
    const Result ok = rig.handler->handle_fill_fields(fields, "enter", true, 0);
    INFO(ok.to_json().dump());
    CHECK(ok.status == Result::Status::Success);
    CHECK(rig.engine->key_reads == 1);

    SECTION("a session replaced during the call still turns the whole call into SESSION_CHANGED") {
        rig.engine->key_reads = 0;
        scripting.on_call = [&](const std::string&) { rig.engine->session_keys[kSession] = "SID-NEW:1"; };
        const Result r = rig.handler->handle_fill_fields(fields, "enter", true, 0);
        INFO(r.to_json().dump());
        CHECK(code_of(r) == "SESSION_CHANGED");
        CHECK(r.error.contains("action_result"));
        CHECK(rig.engine->key_reads == 1);
    }
}

TEST_CASE("Native startup adoption never opens a new connection when its owner guard is unavailable", "[handler][startup][startup-prelogin]") {
    Rig rig(false);
    rig.handler->set_startup_adoption_session(kSession);
    SECTION("no owner identities") {}
    SECTION("no live target guard") { rig.handler->set_attach_owner_identities({"A4H/001/OWNER"}); }
    const auto result = rig.handler->handle_launch("Bigfox");
    CHECK(code_of(result) == "SAP_STARTUP_UNAVAILABLE");
    CHECK(rig.engine->count("launch_connection") == 0);
    CHECK(rig.engine->count("attach_by_session_id") == 0);
}

TEST_CASE("Native startup adoption rejects a different attached session before writing a receipt", "[handler][startup][startup-prelogin]") {
    Rig rig(false);
    rig.handler->set_startup_adoption_session(kSession);
    rig.handler->set_attach_owner_identities({"A4H/001/OWNER"});
    rig.handler->set_launch_target_guard([](const audit::SapFacts&, const std::string&) { return true; });
    rig.engine->next = Result::ok({{"session_id", "/app/con[1]/ses[0]"}, {"connection_id", "/app/con[1]"}});
    const auto result = rig.handler->handle_launch("Bigfox");
    CHECK(code_of(result) == "OWNER_SESSION_UNAVAILABLE");
    CHECK(rig.engine->count("attach_by_session_id") == 1);
    CHECK(rig.engine->count("launch_connection") == 0);
    CHECK(cli::ConnectionManager(rig.dir.path().string()).list_connections().empty());
}
