// SAP Logon is looked up again before every call, and fairyfly never starts a scripting engine of its own.
// Live 2026-10-28 (Windows 10, MSIX 2026.10.2800.0): the link tray started at Windows logon, before SAP Logon. Its
// first call created an embedded SapGui.ScriptingCtrl inside the tray (doctor: com_engine "initialized
// successfully") and kept it: after SAP Logon was started and logged on, the tray still saw 0 connections, and every
// other process's GetObject("SAPGUI") hung until the tray was killed.
#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <filesystem>
#include <cctype>
#include <fstream>
#include <sstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "include/cli_handler.h"
#include "include/com/wrapper.h"
#include "include/com_automation_engine.h"
#include "include/credential_store.h"
#include "include/doctor_checks.h"
#include "include/mcp/dispatcher.h"
#include "support/fake_dispatch.h"
#include "support/fake_engine.h"
#include "support/temp_dir.h"

using namespace fairyfly;
using namespace fairyfly::sap;
using namespace fairyfly::test;
using json = nlohmann::json;

namespace {

/// A running SAP Logon as the running object table shows it: SapGuiAuto (the SAPGUI entry) -> GetScriptingEngine ->
/// GuiApplication -> Connections.
struct FakeSapLogon {
    std::vector<FakeNode*> nodes;
    FakeNode *rot_entry, *app, *connections;

    FakeNode* make(const wchar_t* type, const std::wstring& id) {
        auto* node = new FakeNode();
        node->strings[L"Type"] = type;
        node->strings[L"Id"] = id;
        nodes.push_back(node);
        return node;
    }
    FakeSapLogon() {
        rot_entry = make(L"GuiSapGuiAuto", L"");
        app = make(L"GuiApplication", L"/app");
        connections = make(L"GuiComponentCollection", L"");
        rot_entry->dispatches[L"GetScriptingEngine"] = app;
        app->dispatches[L"Connections"] = connections;
    }
    ~FakeSapLogon() {
        for (auto* node : nodes) node->Release();
    }
    /// A logged-on connection with one session.
    FakeNode* add_connection(const wchar_t* description) {
        const std::wstring id = L"/app/con[" + std::to_wstring(connections->items.size()) + L"]";
        FakeNode* connection = make(L"GuiConnection", id);
        connection->strings[L"Description"] = description;
        connection->strings[L"ConnectionString"] = L"/H/a4h/S/3200";
        connection->ints[L"TypeAsNumber"] = 11;
        connection->bools[L"DisabledByServer"] = false;
        FakeNode* sessions = make(L"GuiComponentCollection", L"");
        FakeNode* session = make(L"GuiSession", id + L"/ses[0]");
        session->ints[L"TypeAsNumber"] = 12;
        sessions->items = {session};
        connection->dispatches[L"Children"] = sessions;
        connections->items.push_back(connection);
        return connection;
    }
    /// GuiApplication.OpenConnection opens `description` (a connection with one session).
    void script_open_connection(const wchar_t* description) {
        FakeNode* connection = add_connection(description);
        app->methods.insert(L"OpenConnection");
        app->dispatches[L"OpenConnection"] = connection;
    }
    /// What SapROTWr / GetObject("SAPGUI") answer while it runs (AddRef'd).
    IDispatchPtr entry() const { return IDispatchPtr(static_cast<IDispatch*>(rot_entry)); }
};

/// Runs `body` on a new thread (a fresh apartment: ComGuiApplication::create initializes an STA) and rethrows what
/// it threw. Catch2 assertions stay on the test thread: `body` only computes values.
template <class Body>
void on_new_thread(Body body) {
    std::exception_ptr error;
    std::thread worker([&] {
        try {
            body();
        } catch (...) {
            error = std::current_exception();
        }
    });
    worker.join();
    if (error) std::rethrow_exception(error);
}

/// The engine on a SAP Logon that `current` points at (null: not running), looked up through
/// ComGuiApplication::create like the real one.
ComAutomationEngine::ApplicationSource running(const FakeSapLogon* const& current) {
    return [&current] {
        return ComGuiApplication::create([&current] { return current ? current->entry() : IDispatchPtr(); });
    };
}

}  // namespace

TEST_CASE("SAP Logon not running: create() reports it and starts no scripting engine of its own", "[com][sap-logon]") {
    int lookups = 0;
    bool not_running = false;
    std::string message;
    HRESULT apartment_after = E_FAIL;
    int entries = -1;
    on_new_thread([&] {
        try {
            ComGuiApplication::create([&] {
                ++lookups;
                return IDispatchPtr();
            });
        } catch (const SapLogonNotRunningException& e) {
            not_running = true;
            message = e.what();
        } catch (const ComException& e) {
            message = e.what();
        }
        APTTYPE type;
        APTTYPEQUALIFIER qualifier;
        apartment_after = CoGetApartmentType(&type, &qualifier);
        entries = thread_apartment_entries();
    });
    INFO(message);
    CHECK(lookups == 1);
    CHECK(not_running);
    CHECK(message.find("SAP Logon (saplogon.exe) is not running") != std::string::npos);
    CHECK(apartment_after == S_OK);   // the thread keeps its apartment (entered once, left when the thread ends)
    CHECK(entries == 1);
}

TEST_CASE("SAP Logon running: create() wraps its own GuiApplication", "[com][sap-logon]") {
    FakeSapLogon logon;
    bool same = false;
    on_new_thread([&] {
        auto app = ComGuiApplication::create([&] { return logon.entry(); });
        same = same_com_object(app->get_dispatch(), logon.app);
    });
    CHECK(same);
}

TEST_CASE("No source file creates a SAP GUI scripting engine or registers in the running object table",
          "[com][sap-logon]") {
    namespace fs = std::filesystem;
    std::vector<std::string> offenders;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(FAIRYFLY_SOURCE_DIR))) {
        const auto ext = e.path().extension();
        if (!e.is_regular_file() || (ext != ".cpp" && ext != ".h")) continue;
        std::ifstream in(e.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto rel = fs::relative(e.path(), fs::path(FAIRYFLY_SOURCE_DIR)).generic_string();
        std::string lower = text;
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        // the ProgID as a wide or narrow string literal, any case; registering anything in the ROT
        for (const char* forbidden : {"\"sapgui.scriptingctrl", "registeractiveobject", "irunningobjecttable",
                                      "getrunningobjecttable"}) {
            if (lower.find(forbidden) != std::string::npos) offenders.push_back(rel + ": " + forbidden);
        }
        // creation by CLSID (its GUID is registered per installation): every CoCreateInstance/CoGetClassObject/
        // CLSIDFromProgID in src/ must name one of the known, harmless classes on the same line
        std::istringstream lines(text);
        std::string line;
        for (int number = 1; std::getline(lines, line); ++number) {
            const bool creates = line.find("CoCreateInstance") != std::string::npos ||
                                 line.find("CoGetClassObject") != std::string::npos ||
                                 line.find("CLSIDFromProgID") != std::string::npos ||
                                 line.find("CoGetObject(") != std::string::npos;
            if (!creates || line.find("//") < line.find("Co")) continue;
            const bool allowed = line.find("SapROTWr.SapROTWrapper") != std::string::npos ||  // ROT lookup helper
                                 line.find("clsid_rot") != std::string::npos ||
                                 line.find("CLSID_CUIAutomation") != std::string::npos ||   // UI Automation
                                 line.find("L\"SAPGUI\"") != std::string::npos;            // ROT lookup only
            if (!allowed) offenders.push_back(rel + ":" + std::to_string(number) + ": " + line);
        }
    }
    CAPTURE(offenders);
    CHECK(offenders.empty());
}

TEST_CASE("engine: SAP Logon started after the engine is found at the next call", "[engine][sap-logon]") {
    FakeSapLogon logon;
    logon.add_connection(L"A4H");
    const FakeSapLogon* current = nullptr;   // the tray started at Windows logon, before SAP Logon
    EngineStatus before, after;
    json info_before, info_after;
    on_new_thread([&] {
        ComAutomationEngine engine(running(current));
        before = engine.engine_status();
        info_before = engine.get_application_info();
        current = &logon;   // the user starts SAP Logon and logs on
        engine.refresh_application();
        after = engine.engine_status();
        info_after = engine.get_application_info();
    });
    CHECK_FALSE(before.available);
    CHECK_FALSE(before.sap_logon_running);
    CHECK(before.detail.find("not running") != std::string::npos);
    CHECK(info_before.value("total_connections", -1) == 0);
    CHECK(after.available);
    CHECK(after.sap_logon_running);
    INFO(info_after.dump());
    CHECK(info_after.value("total_connections", -1) == 1);
}

TEST_CASE("engine: the same SAP Logon keeps the bound session, a restarted one replaces it", "[engine][sap-logon]") {
    FakeSapLogon first, second;
    first.add_connection(L"A4H");
    second.add_connection(L"A4H");
    second.add_connection(L"BIGFOX");
    const FakeSapLogon* current = &first;
    bool kept = false, replaced = false, dropped = false, gone_status = true;
    int connections_after_restart = -1;
    on_new_thread([&] {
        ComAutomationEngine engine(running(current));
        const auto bound = engine.get_session();   // the first session, bound at start-up
        engine.refresh_application();   // nothing changed
        kept = bound && engine.get_session() == bound && engine.get_app() &&
               same_com_object(engine.get_app()->get_dispatch(), first.app);

        current = &second;   // SAP Logon was restarted: a new GuiApplication
        engine.refresh_application();
        replaced = engine.get_app() && same_com_object(engine.get_app()->get_dispatch(), second.app) &&
                   engine.get_session() != bound;
        connections_after_restart = engine.get_application_info().value("total_connections", -1);

        current = nullptr;   // SAP Logon closed
        engine.refresh_application();
        dropped = !engine.get_app() && !engine.get_session() && !engine.get_connection();
        gone_status = engine.engine_status().sap_logon_running;
    });
    CHECK(kept);
    CHECK(replaced);
    CHECK(connections_after_restart == 2);
    CHECK(dropped);
    CHECK_FALSE(gone_status);
}

TEST_CASE("engine: an engine that held no SAP Logon looks it up when a call needs one", "[engine][sap-logon]") {
    FakeSapLogon logon;
    const FakeSapLogon* current = nullptr;
    std::string first_error, second_session;
    bool second_ok = false;
    on_new_thread([&] {
        ComAutomationEngine engine(running(current));
        const Result missing = engine.launch_connection("A4H");
        first_error = missing.error.value("code", "");
        current = &logon;   // SAP Logon started; no refresh_application in between
        logon.script_open_connection(L"A4H");
        const Result launched = engine.launch_connection("A4H");
        second_ok = launched.status == Result::Status::Success;
        second_session = launched.data.value("session_id", "");
    });
    CHECK(first_error == "SAP_LOGON_NOT_RUNNING");
    CHECK(second_ok);
    CHECK(second_session == "/app/con[0]/ses[0]");
}

TEST_CASE("engine: one COM apartment for the thread however often SAP Logon comes and goes", "[engine][sap-logon]") {
    FakeSapLogon logon;
    logon.add_connection(L"A4H");
    const FakeSapLogon* current = nullptr;
    int entries = -1;
    HRESULT apartment = E_FAIL;
    APTTYPE type = APTTYPE_MTA;
    on_new_thread([&] {
        ComAutomationEngine engine(running(current));
        for (int i = 0; i < 4; ++i) {
            current = (i % 2) ? nullptr : &logon;   // present, absent, present, absent
            engine.refresh_application();
        }
        APTTYPEQUALIFIER qualifier;
        apartment = CoGetApartmentType(&type, &qualifier);   // still in the apartment after SAP Logon was dropped
        entries = thread_apartment_entries();
    });
    CHECK(apartment == S_OK);
    CHECK(type == APTTYPE_STA);
    CHECK(entries == 1);
}

namespace {
/// Current COM reference count of a fake object.
ULONG references(FakeNode* node) {
    node->AddRef();
    return node->Release();
}
}  // namespace

TEST_CASE("engine: dropping SAP Logon releases every COM reference into it", "[engine][sap-logon]") {
    FakeSapLogon logon;
    logon.add_connection(L"A4H");
    std::vector<ULONG> baseline, held, after;
    for (auto* node : logon.nodes) baseline.push_back(references(node));
    const FakeSapLogon* current = &logon;
    on_new_thread([&] {
        ComAutomationEngine engine(running(current));
        (void)engine.get_application_info();
        for (auto* node : logon.nodes) held.push_back(references(node));
        current = nullptr;   // SAP Logon closed
        engine.refresh_application();
        for (auto* node : logon.nodes) after.push_back(references(node));
    });
    CHECK(held != baseline);   // the engine held SAP Logon (application, bound session)
    CHECK(after == baseline);
}

TEST_CASE("handler: without SAP Logon every SAP command answers SAP_LOGON_NOT_RUNNING and keeps saved connections",
          "[cli][sap-logon]") {
    TempDir dir{"handler_logon"};
    auto fake = std::make_unique<FakeEngine>();
    FakeEngine* engine = fake.get();
    engine->status = EngineStatus{false, false, "SAP Logon (saplogon.exe) is not running on this desktop"};
    auto connections = std::make_unique<cli::ConnectionManager>(dir.path().string());
    cli::ConnectionManager* saved = connections.get();
    saved->create_or_update_connection("/app/con[0]/ses[0]", "/app/con[0]", "A4H", "route", "SAP");
    cli::CommandHandler handler(std::make_unique<cred::InMemoryCredentialStore>(), std::move(fake), std::move(connections));
    const auto code = [](const Result& r) { return r.error.is_object() ? r.error.value("code", std::string()) : ""; };

    const Result listed = handler.handle_list_all();                              // session list
    const Result attached = handler.handle_attach(5, std::string("/app/con[0]/ses[0]"));  // session attach
    const Result screen = handler.handle_screen_read(false, std::nullopt);        // screen read (saved connection)
    const Result filled = handler.handle_fill("wnd[0]/usr/txtA", "x", 0);         // element fill
    CHECK(code(listed) == "SAP_LOGON_NOT_RUNNING");
    CHECK(code(attached) == "SAP_LOGON_NOT_RUNNING");
    CHECK(code(screen) == "SAP_LOGON_NOT_RUNNING");
    CHECK(code(filled) == "SAP_LOGON_NOT_RUNNING");
    CHECK(filled.error.value("hint", "").find("Start SAP Logon") != std::string::npos);
    CHECK(engine->count("attach_by_session_id") == 0);
    CHECK(engine->count("fill_field") == 0);
    CHECK(saved->list_connections().size() == 1);   // not forgotten: the session may come back with SAP Logon
}

TEST_CASE("handler: a keyless saved connection is not re-bound by position after SAP Logon was replaced",
          "[cli][sap-logon]") {
    constexpr const char* kPath = "/app/con[0]/ses[0]";
    TempDir dir{"handler_keyless"};
    auto fake = std::make_unique<FakeEngine>();
    FakeEngine* engine = fake.get();
    FakeScripting scripting;
    engine->scripting_fake = &scripting;
    engine->live_sessions[kPath] = true;
    auto connections = std::make_unique<cli::ConnectionManager>(dir.path().string());
    cli::ConnectionManager* saved = connections.get();
    SECTION("unknown identity: refused, nothing runs, the entry stays") {
        saved->create_or_update_connection(kPath, "/app/con[0]", "A4H", "route", "SAP");   // no key, no identity
        cli::CommandHandler handler(std::make_unique<cred::InMemoryCredentialStore>(), std::move(fake),
                                    std::move(connections));
        REQUIRE(handler.handle_fill("wnd[0]/usr/txtA", "one", 0).status == Result::Status::Success);
        engine->generation = 2;   // SAP Logon restarted inside this long-lived process
        const Result r = handler.handle_fill("wnd[0]/usr/txtA", "two", 0);
        CHECK(r.error.value("code", "") == "SESSION_TARGET_CHANGED");
        CHECK(r.error.value("hint", "").find("attach the SAP session again") != std::string::npos);
        CHECK(engine->count("fill_field wnd[0]/usr/txtA=two") == 0);
        CHECK(saved->list_connections().size() == 1);
    }
    SECTION("same system, client and user: accepted") {
        saved->create_or_update_connection(kPath, "/app/con[0]", "A4H", "route", "SAP", "",
                                           cli::ConnectionIdentity{"A4H", "001", "DEVELOPER"});
        scripting.facts.system = "A4H";
        scripting.facts.client = "001";
        scripting.facts.user = "DEVELOPER";
        cli::CommandHandler handler(std::make_unique<cred::InMemoryCredentialStore>(), std::move(fake),
                                    std::move(connections));
        REQUIRE(handler.handle_fill("wnd[0]/usr/txtA", "one", 0).status == Result::Status::Success);
        engine->generation = 2;
        CHECK(handler.handle_fill("wnd[0]/usr/txtA", "two", 0).status == Result::Status::Success);
        CHECK(engine->count("fill_field wnd[0]/usr/txtA=two") == 1);
    }
    SECTION("another user at that position: refused") {
        saved->create_or_update_connection(kPath, "/app/con[0]", "A4H", "route", "SAP", "",
                                           cli::ConnectionIdentity{"A4H", "001", "DEVELOPER"});
        scripting.facts.system = "A4H";
        scripting.facts.client = "001";
        scripting.facts.user = "ALICE";
        cli::CommandHandler handler(std::make_unique<cred::InMemoryCredentialStore>(), std::move(fake),
                                    std::move(connections));
        REQUIRE(handler.handle_fill("wnd[0]/usr/txtA", "one", 0).status == Result::Status::Success);
        engine->generation = 2;
        CHECK(handler.handle_fill("wnd[0]/usr/txtA", "two", 0).error.value("code", "") == "SESSION_TARGET_CHANGED");
    }
}

TEST_CASE("doctor: SAP Logon not running is reported as such, not as an initialized engine", "[cli][doctor][sap-logon]") {
    CHECK(cli::doctor::judge_engine(false, false, "no SAPGUI entry").message == "SAP Logon is not running");
    CHECK(cli::doctor::judge_engine(false, false, "no SAPGUI entry").status == "fail");
    CHECK(cli::doctor::judge_engine(false, true, "scripting off").message.find("scripting engine is not available") !=
          std::string::npos);

    TempDir dir{"doctor_logon"};
    auto fake = std::make_unique<FakeEngine>();
    FakeEngine* engine = fake.get();
    engine->status = EngineStatus{false, false, "SAP Logon (saplogon.exe) is not running on this desktop"};
    cli::CommandHandler handler(std::make_unique<cred::InMemoryCredentialStore>(), std::move(fake),
                                std::make_unique<cli::ConnectionManager>(dir.path().string()));
    const Result result = handler.handle_doctor();
    REQUIRE(result.status == Result::Status::Success);
    CHECK(engine->refreshes >= 1);   // the doctor looks SAP Logon up now
    json com;
    for (const auto& check : result.data["checks"])
        if (check["name"] == "com_engine") com = check;
    INFO(result.data.dump());
    REQUIRE(com.is_object());
    CHECK(com["status"] == "fail");
    CHECK(com["message"] == "SAP Logon is not running");
    CHECK(com["code"] == "SAP_LOGON_NOT_RUNNING");
    CHECK(result.data["overall_health"] == "error");
}

TEST_CASE("Every command looks SAP Logon up again before it runs", "[cli][sap-logon]") {
    TempDir dir{"registry_logon"};
    auto fake = std::make_unique<FakeEngine>();
    FakeEngine* engine = fake.get();
    cli::CommandHandler handler(std::make_unique<cred::InMemoryCredentialStore>(), std::move(fake),
                                std::make_unique<cli::ConnectionManager>(dir.path().string()));
    auto invoke = mcp::make_registry_invoker([&]() -> cli::CommandHandler& { return handler; });
    invoke({"connection", "list"});
    CHECK(engine->refreshes == 1);
    invoke({"connection", "list"});
    CHECK(engine->refreshes == 2);

    // inside one MCP call (gui_batch included) the dispatcher already looked SAP Logon up: no second lookup
    {
        const auto held = handler.refresh_sap_gui_for_call();
        CHECK(engine->refreshes == 3);
        invoke({"connection", "list"});
        invoke({"connection", "list"});
        CHECK(engine->refreshes == 3);
    }
    invoke({"connection", "list"});
    CHECK(engine->refreshes == 4);
}
