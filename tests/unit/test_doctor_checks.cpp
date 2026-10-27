// `fairyfly doctor`: what each check concludes from what was probed (doctor_checks.h). The Win32 probes only read
// the desktop, the process list and the registry; every decision is here and testable without them.
#include <catch2/catch_test_macros.hpp>

#include "include/doctor_checks.h"

using namespace fairyfly::cli::doctor;
using nlohmann::json;

TEST_CASE("doctor: the interactive desktop passes, any other warns", "[doctor]") {
    CHECK(judge_desktop("WinSta0", "Default").status == "pass");
    CHECK(judge_desktop("winsta0", "default").status == "pass");  // case does not matter
    const Check service = judge_desktop("Service-0x0-3e7$", "Default");
    CHECK(service.status == "warn");
    CHECK(service.message.find("Service-0x0-3e7$\\Default") != std::string::npos);
    CHECK_FALSE(service.remediation.empty());
}

TEST_CASE("doctor: SAP GUI processes", "[doctor]") {
    ProcessScan both;
    both.logon = both.gui = true;
    both.matched = 2;
    both.toolhelp_count = 120;
    const Check found = judge_processes(both);
    CHECK(found.status == "pass");
    CHECK(found.message.find("saplogon.exe and sapgui.exe") != std::string::npos);
    CHECK(found.details.find("PSAPI not run") != std::string::npos);

    ProcessScan pad;
    pad.pad = true;
    pad.matched = 1;
    pad.fallback_attempted = true;
    pad.fallback_count = 80;
    CHECK(judge_processes(pad).message.find("saplgpad.exe") != std::string::npos);
    CHECK(judge_processes(pad).details.find("PSAPI saw 80 entries") != std::string::npos);

    ProcessScan none;
    none.toolhelp_count = 3;
    none.toolhelp_error = 5;
    CHECK(judge_processes(none).status == "warn");
    CHECK(judge_processes(none).details.find("error 5") != std::string::npos);
}

TEST_CASE("doctor: SAP GUI client security settings", "[doctor]") {
    ClientSecurity defaults;  // no key: SAP GUI's defaults
    CHECK(judge_client_security(defaults).status == "pass");

    ClientSecurity disabled;
    disabled.key_found = true;
    disabled.user_scripting = 0;
    CHECK(judge_client_security(disabled).status == "fail");

    ClientSecurity warnings;
    warnings.key_found = true;
    warnings.user_scripting = 1;
    warnings.warn_on_attach = 1;
    const Check w = judge_client_security(warnings);
    CHECK(w.status == "warn");
    CHECK(w.details == "WarnOnAttach=1, WarnOnConnection=0");

    ClientSecurity optimal;
    optimal.key_found = true;
    optimal.user_scripting = 1;
    optimal.warn_on_attach = 0;
    optimal.warn_on_connection = 0;
    CHECK(judge_client_security(optimal).status == "pass");
}

TEST_CASE("doctor: sessions, from the application info", "[doctor]") {
    CHECK(judge_sessions(json{{"error", "RPC failed"}}).status == "fail");
    CHECK(judge_sessions(json{{"backend_scripting_disabled", true}, {"total_connections", 1}, {"total_sessions", 0}}).status == "fail");
    CHECK(judge_sessions(json{{"total_connections", 0}}).status == "warn");
    CHECK(judge_sessions(json{{"total_connections", 2}, {"total_sessions", 1}, {"connection_enumeration_errors", 1}}).message
              .find("connections could not be inspected") != std::string::npos);
    CHECK(judge_sessions(json{{"total_connections", 2}, {"total_sessions", 1}, {"session_enumeration_errors", 2}}).status == "warn");
    CHECK(judge_sessions(json{{"total_connections", 2}, {"total_sessions", 1}, {"backend_scripting_disabled", true}}).status == "warn");
    CHECK(judge_sessions(json{{"total_connections", 1}, {"total_sessions", 0}}).status == "warn");
    const Check ok = judge_sessions(json{{"total_connections", 1}, {"total_sessions", 2}});
    CHECK(ok.status == "pass");
    CHECK(ok.message.find("1 connections, 2 sessions") != std::string::npos);
    // an exception while asking: "disabled" in it means the backend switched scripting off
    CHECK(judge_sessions_error("Scripting is disabled").status == "fail");
    CHECK(judge_sessions_error("timeout").status == "warn");
}

TEST_CASE("doctor: overall health and the JSON of a check", "[doctor]") {
    CHECK(overall_health({{"a", "pass", "x"}, {"b", "info", "y"}}) == "ok");
    CHECK(overall_health({{"a", "pass", "x"}, {"b", "warn", "y"}}) == "warning");
    CHECK(overall_health({{"a", "warn", "x"}, {"b", "fail", "y"}}) == "error");
    const json j = to_json(Check{"com_engine", "pass", "fine"});
    CHECK(j == json{{"name", "com_engine"}, {"status", "pass"}, {"message", "fine"}});  // no empty details/remediation
    CHECK(judge_engine(true).status == "pass");
    CHECK(judge_engine(false).status == "fail");
}
