// CommandHandler::handle_login under an owner guard, through the real handler: the order of the guard checks, the
// password clearing on a refusal and the Result that is returned (review 2026-10-25, Codex on #85). The session is a
// FakeNode scene on the SAP logon screen; the engine's actions are recorded by FakeEngine.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "include/cli_handler.h"
#include "include/com/wrapper.h"
#include "include/connection_manager.h"
#include "include/credential_store.h"
#include "support/fake_dispatch.h"
#include "support/fake_engine.h"
#include "support/temp_dir.h"

using namespace fairyfly;
using namespace fairyfly::test;

namespace {

constexpr const wchar_t* kSession = L"/app/con[0]/ses[0]";
const std::string kPassword = "@active/usr/pwdRSYST-BCODE";

/// A pre-logon session of the saved connection "S4H" (server key key-1) and a handler with an owner guard that admits
/// it until `revoked`.
struct LoginRig {
    ScopedDispatchCacheReset cache_reset;
    TempDir dir{"login-handler"};
    std::vector<FakeNode*> nodes;
    FakeNode* session_node = nullptr;
    FakeNode* info = nullptr;
    FakeScripting scripting;
    FakeEngine* engine = nullptr;
    std::unique_ptr<cli::CommandHandler> handler;
    bool revoked = false;
    int guard_calls = 0;

    FakeNode* make(const wchar_t* type, const std::wstring& id) {
        auto* node = new FakeNode();
        node->strings[L"Type"] = type;
        node->strings[L"Id"] = id;
        nodes.push_back(node);
        return node;
    }
    void add_element(const std::wstring& suffix) {
        const std::wstring id = std::wstring(kSession) + L"/" + suffix;
        session_node->find_by_id[id] = make(L"GuiTextField", id);
    }

    LoginRig() {
        session_node = make(L"GuiSession", kSession);
        info = make(L"GuiSessionInfo", L"");
        info->strings[L"Transaction"] = L"S000";
        info->strings[L"User"] = L"";
        session_node->dispatches[L"Info"] = info;
        add_element(L"wnd[0]/usr/pwdRSYST-BCODE");
        scripting.session = sap::ComGuiSession::create(IDispatchPtr(session_node, true));
        scripting.connection_description = "S4H";
        scripting.facts.system = "S4H";
        scripting.facts.transaction = "S000";

        const auto saved = cli::ConnectionManager(dir.path().string())
                               .create_or_update_connection("/app/con[0]/ses[0]", "/app/con[0]", "S4H", "route", "SAP", "key-1");
        (void)saved;
        auto fake = std::make_unique<FakeEngine>();
        engine = fake.get();
        engine->scripting_fake = &scripting;
        engine->live_sessions["/app/con[0]/ses[0]"] = true;
        engine->server_key = "key-1";
        auto store = std::make_unique<cred::InMemoryCredentialStore>();
        store->write("S4H", cred::CredentialSummary{"", "ALICE", "100", "EN", ""}, cred::SecretBuffer(std::string("pw-1")));
        handler = std::make_unique<cli::CommandHandler>(std::move(store), std::move(fake),
                                                        std::make_unique<cli::ConnectionManager>(dir.path().string()));
        handler->set_login_target_guard([this](const audit::SapFacts&, const std::string& name) {
            ++guard_calls;
            return !revoked && name == "S4H";
        });
    }
    ~LoginRig() {
        scripting.session.reset();
        for (auto* node : nodes) node->Release();
    }

    /// SAP accepts the logon when the logon screen's Enter is pressed: the session shows ALICE on client 100.
    void authenticate_on_enter() {
        engine->on_call = [this](const std::string& call) {
            if (call != "click_element @active/tbar[0]/btn[0]") return;
            info->strings[L"User"] = L"ALICE";
            info->strings[L"Transaction"] = L"SESSION_MANAGER";
            scripting.facts.user = "ALICE";
            scripting.facts.client = "100";
        };
    }
    /// The engine's fill and click calls, in order.
    std::vector<std::string> actions() const {
        std::vector<std::string> out;
        std::copy_if(engine->calls.begin(), engine->calls.end(), std::back_inserter(out), [](const std::string& c) {
            return c.rfind("fill_field ", 0) == 0 || c.rfind("click_element ", 0) == 0;
        });
        return out;
    }
    Result login(const std::string& multiple_logon = "fail") {
        return handler->handle_login("", std::nullopt, false, "", multiple_logon);
    }
};

std::string code(const Result& r) { return r.error.value("code", std::string()); }

bool clicked_after(const std::vector<std::string>& actions, const std::string& marker) {
    const auto at = std::find_if(actions.begin(), actions.end(),
                                 [&](const std::string& a) { return a.find(marker) != std::string::npos; });
    return at != actions.end() && std::any_of(std::next(at), actions.end(), [](const std::string& a) {
        return a.rfind("click_element ", 0) == 0;
    });
}

}  // namespace

TEST_CASE("Automatic login guard runs immediately before submission and can prevent it", "[login][startup][startup-submit]") {
    LoginRig rig;
    rig.authenticate_on_enter();
    int attempts = 0;
    rig.handler->set_login_submit_guard([&] {
        ++attempts;
        const auto actions = rig.actions();
        REQUIRE_FALSE(actions.empty());
        CHECK(actions.back().rfind("fill_field @active/usr/pwdRSYST-BCODE=", 0) == 0);
        return Result::fail("SAP_LOGIN_REQUIRED", "Complete login manually.");
    });
    const auto result = rig.login();
    CHECK(result.status == Result::Status::Error);
    CHECK(attempts == 1);
    CHECK(rig.engine->count("click_element") == 0);
    REQUIRE_FALSE(rig.actions().empty());
    CHECK(rig.actions().back() == "fill_field @active/usr/pwdRSYST-BCODE=");
}

TEST_CASE("Missing credentials do not record an automatic login attempt", "[login][startup][startup-submit]") {
    LoginRig rig;
    int attempts = 0;
    rig.handler->set_login_submit_guard([&] { ++attempts; return Result::ok(); });
    const auto result = rig.handler->handle_login("", 0, false, "missing-credentials", "fail");
    CHECK(result.status == Result::Status::Error);
    CHECK(attempts == 0);
    CHECK(rig.engine->count("click_element") == 0);
}

TEST_CASE("A failed credential submission retains its automatic retry block", "[login][startup][startup-submit]") {
    LoginRig rig;
    startup::LoginAttempts attempts(rig.dir.path());
    rig.handler->set_login_submit_guard([&] { return attempts.begin("S4H"); });
    rig.engine->on_call = [&](const std::string& call) {
        if (call == "click_element @active/tbar[0]/btn[0]")
            rig.engine->next = Result::fail("SUBMIT_FAILED", "Submission outcome is unknown.");
    };
    CHECK(rig.login().status == Result::Status::Error);
    CHECK(rig.engine->count("click_element") == 1);
    CHECK(attempts.begin("S4H").status == Result::Status::Error);
}

TEST_CASE("login: an owner logon fills the screen, presses Enter and confirms the authenticated owner", "[login][owner][handler]") {
    LoginRig rig;
    rig.authenticate_on_enter();
    const Result r = rig.login();
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Success);
    CHECK(r.data["session_id"] == "/app/con[0]/ses[0]");
    CHECK(r.data["transaction"] == "SESSION_MANAGER");
    CHECK(r.data["credential_source"] == "credential-manager");
    CHECK(rig.actions() == std::vector<std::string>{
        "fill_field @active/usr/txtRSYST-MANDT=100", "fill_field @active/usr/txtRSYST-BNAME=ALICE",
        "fill_field @active/usr/txtRSYST-LANGU=EN", "fill_field " + kPassword + "=pw-1",
        "click_element @active/tbar[0]/btn[0]"});
    CHECK(rig.guard_calls >= 3);   // before the screen is touched, before Enter, after the logon
}

TEST_CASE("login: a revocation after the password was typed clears it and presses nothing", "[login][owner][handler]") {
    LoginRig rig;
    rig.engine->on_call = [&](const std::string& call) {
        if (call == "fill_field " + kPassword + "=pw-1") rig.revoked = true;
    };
    const Result r = rig.login();
    INFO(r.to_json().dump());
    CHECK(code(r) == "OWNER_SESSION_UNAVAILABLE");
    const auto actions = rig.actions();
    REQUIRE_FALSE(actions.empty());
    CHECK(actions.back() == "fill_field " + kPassword + "=");   // the typed password is cleared last
    CHECK(std::none_of(actions.begin(), actions.end(), [](const std::string& a) { return a.rfind("click_element ", 0) == 0; }));
}

TEST_CASE("login: a revocation before anything is typed refuses without touching the screen", "[login][owner][handler]") {
    LoginRig rig;
    rig.revoked = true;
    const Result r = rig.login();
    CHECK(code(r) == "OWNER_SESSION_UNAVAILABLE");
    CHECK(rig.actions().empty());
}

TEST_CASE("login: the multiple-logon dialog under an owner guard", "[login][owner][handler][multiple-logon]") {
    LoginRig rig;
    rig.add_element(L"wnd[1]/usr/radMULTI_LOGON_OPT2");

    SECTION("a revocation between the radio and its confirmation clears the password and confirms nothing") {
        rig.engine->on_call = [&](const std::string& call) {
            if (call.find("MULTI_LOGON") != std::string::npos) rig.revoked = true;
        };
        const Result r = rig.login("keep");
        INFO(r.to_json().dump());
        CHECK(code(r) == "OWNER_SESSION_UNAVAILABLE");
        const auto actions = rig.actions();
        CHECK_FALSE(clicked_after(actions, "MULTI_LOGON"));
        CHECK(actions.back() == "fill_field " + kPassword + "=");
    }
    SECTION("a failed radio click is the answer, and nothing is confirmed") {
        rig.engine->on_call = [&](const std::string& call) {
            if (call.find("MULTI_LOGON") == std::string::npos) return;
            Result failed;
            failed.set_error("ELEMENT_NOT_FOUND", "gone");
            rig.engine->next = failed;
        };
        const Result r = rig.login("keep");
        CHECK(code(r) == "ELEMENT_NOT_FOUND");
        CHECK_FALSE(clicked_after(rig.actions(), "MULTI_LOGON"));
    }
    SECTION("terminate answers a fresh MULTIPLE_LOGON_TERMINATED without the clicks' data") {
        rig.engine->next = Result::ok(json{{"clicked", true}});
        const Result r = rig.login("terminate");
        INFO(r.to_json().dump());
        CHECK(code(r) == "MULTIPLE_LOGON_TERMINATED");
        CHECK(r.error["connection_open"] == false);
        CHECK_FALSE(r.data.contains("clicked"));
    }
    SECTION("fail reports the dialog and presses nothing after Enter") {
        const Result r = rig.login("fail");
        CHECK(r.status == Result::Status::Error);
        CHECK(r.error["password_change_detected"] == false);
        CHECK_FALSE(clicked_after(rig.actions(), "tbar[0]/btn[0]"));
    }
}
