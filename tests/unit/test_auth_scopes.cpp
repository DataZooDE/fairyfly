// Verb-level token scopes: "<family>" (all tools of the family), "<family>.<verb>" (one tool), "*".
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>

#include "include/auth/authenticator.h"
#include "include/auth/authorize.h"
#include "include/auth/crypto.h"
#include "include/auth/scopes.h"
#include "include/auth/secret_backend.h"
#include "include/auth/token_cli.h"
#include "include/auth/token_store.h"
#include "include/command_table.h"
#include "include/mcp/dispatcher.h"
#include "include/mcp/tool_catalog.h"

using namespace fairyfly;
using namespace fairyfly::auth;
using namespace fairyfly::mcp;

namespace {

const ToolSpec& spec_of(const std::string& tool) {
    static const std::vector<ToolSpec> catalog = all_tool_specs();
    for (const auto& s : catalog)
        if (s.def.name == tool) return s;
    FAIL("unknown tool " + tool);
    return catalog.front();
}

Principal token(std::set<std::string> scopes) {
    Principal p;
    p.name = "x";
    p.all_scopes = false;
    p.scopes = std::move(scopes);
    p.authenticated = true;
    p.read_only = false;
    return p;
}

PolicyDecision decide(const Principal& p, const std::string& tool, const json& args = json::object()) {
    Policy policy;
    policy.read_only = false;
    const ToolSpec& spec = spec_of(tool);
    return authorize_call(p, spec, spec.family, args, policy, std::nullopt, std::nullopt);
}

json sample_args(const std::string& tool) {
    if (tool == "gui_element_get" || tool == "gui_element_click" || tool == "gui_element_f4") return {{"element", "wnd[0]/usr/txtA"}};
    if (tool == "gui_element_fill") return {{"element", "wnd[0]/usr/txtA"}, {"value", "1"}};
    if (tool == "gui_transaction_start") return {{"code", "SE16"}};
    if (tool == "gui_key_send") return {{"key", "enter"}};
    return json::object();
}

std::set<std::string> listed(const Principal& p) {
    Policy policy;
    policy.read_only = false;
    policy.allow_write = true;
    CommandDispatcher d([](const std::vector<std::string>&) { return Result{}; }, policy);
    std::set<std::string> names;
    for (const auto& def : d.list_tools_for(p)) names.insert(def.name);
    return names;
}

struct Env {
    std::shared_ptr<InMemorySecretBackend> backend = std::make_shared<InMemorySecretBackend>();
    TimePoint now = std::chrono::sys_seconds(std::chrono::seconds(1'800'000'000));
    std::shared_ptr<unsigned> counter = std::make_shared<unsigned>(1);
    TokenStore store() {
        auto c = counter;
        return TokenStore(backend, [this] { return now; },
                          [c](unsigned char* buf, std::size_t n) {
                              for (std::size_t i = 0; i < n; ++i) buf[i] = static_cast<unsigned char>((*c * 37 + i * 11 + 5) & 0xff);
                              ++*c;
                          },
                          std::chrono::milliseconds(0));
    }
    AuthOutcome authenticate(const std::string& token) {
        AuthConfig config;
        config.token_backend = backend;
        config.clock = [this] { return now; };
        config.cache_ttl = std::chrono::milliseconds(0);
        TokenAuthenticator auth(config);
        AuthRequest request;
        request.authorization = "Bearer " + token;
        request.peer_addr = "127.0.0.1";
        return auth.authenticate(request);
    }
};

} // namespace

TEST_CASE("scopes: the verb list is derived from the command table", "[auth][scopes]") {
    const auto verbs = verb_scopes();
    const std::vector<std::string> expected = {
        "session.list",   "session.attach", "session.launch", "session.login", "session.disconnect", "connection.list",
        "screen.read",    "screen.find",    "screen.capture", "menu.list",     "menu.select",        "element.get",
        "element.click",  "element.fill",   "element.f4",     "key.send",      "popup.close",        "transaction.start",
        "credentials.list"};
    CHECK(verbs == expected);
    CHECK(verbs_of_family("session") == std::vector<std::string>{"list", "attach", "launch", "login", "disconnect"});
    CHECK(verbs_of_family("system").empty());
    CHECK(verbs_of_family("batch").empty());
    CHECK(verb_scope_of_tool("gui_session_disconnect") == "session.disconnect");
    CHECK(verb_scope_of_tool("gui_doctor").empty());
    CHECK(verb_scope_of_tool("gui_batch").empty());
}

TEST_CASE("scopes: validation codes and messages", "[auth][scopes]") {
    CHECK_FALSE(validate_scope("*"));
    CHECK_FALSE(validate_scope("session"));
    CHECK_FALSE(validate_scope("session.disconnect"));
    CHECK_FALSE(validate_scope("system"));
    CHECK(validate_scope("nonsense")->code == "UNKNOWN_FAMILY");
    CHECK(validate_scope("nonsense.list")->code == "UNKNOWN_FAMILY");
    const auto bad = validate_scope("session.nuke");
    REQUIRE(bad);
    CHECK(bad->code == "UNKNOWN_SCOPE");
    CHECK(bad->message.find("list, attach, launch, login, disconnect") != std::string::npos);
    CHECK(validate_scope("system.doctor")->code == "UNKNOWN_SCOPE");  // verbless roots are family-only
    CHECK(validate_scope("session.")->code == "UNKNOWN_SCOPE");
    CHECK(normalize_scope("Session.List") == "session.list");
    for (const auto& scope : verb_scopes()) CHECK_FALSE(validate_scope(scope));
}

TEST_CASE("scopes: authorize matrix (family, verb, both, star, unknown)", "[auth][scopes][authz]") {
    // family scope: all tools of the family, exactly as before
    for (const char* tool : {"gui_session_list", "gui_session_attach", "gui_session_launch", "gui_session_login", "gui_session_disconnect"})
        CHECK(decide(token({"session"}), tool).allowed);

    // verb scope: only that tool
    const Principal list_only = token({"session.list"});
    CHECK(decide(list_only, "gui_session_list").allowed);
    for (const char* tool : {"gui_session_attach", "gui_session_launch", "gui_session_login", "gui_session_disconnect"}) {
        const auto d = decide(list_only, tool);
        CHECK_FALSE(d.allowed);
        CHECK(d.code == "SCOPE_DENIED");
    }
    CHECK(decide(list_only, "gui_screen_read").code == "SCOPE_DENIED");

    // the message names the missing scope precisely
    CHECK(decide(list_only, "gui_session_disconnect").message == "token 'x' lacks scope 'session.disconnect' (or 'session')");
    CHECK(decide(token({}), "gui_doctor").message == "token 'x' lacks scope 'system'");

    // both (redundant) is fine
    CHECK(decide(token({"session", "session.list"}), "gui_session_disconnect").allowed);
    // a verb of the same family does not grant another tool
    CHECK(decide(token({"screen.read"}), "gui_screen_find").code == "SCOPE_DENIED");
    CHECK(decide(token({"screen.read"}), "gui_screen_read").allowed);
    // unknown or malformed stored scopes never match anything
    CHECK(decide(token({"session.nuke", "SESSION", "session."}), "gui_session_list").code == "SCOPE_DENIED");
    // star
    Principal star;
    star.all_scopes = true;
    for (const auto& spec : all_tool_specs()) CHECK(decide(star, spec.def.name, sample_args(spec.def.name)).code != "SCOPE_DENIED");

    // doctor and batch stay family-only
    CHECK(decide(token({"system"}), "gui_doctor").allowed);
    CHECK(decide(token({"batch"}), "gui_batch", {{"items", json::array()}}).code != "SCOPE_DENIED");
}

TEST_CASE("scopes: every tool resolves to its own valid verb scope", "[auth][scopes][authz]") {
    for (const auto& spec : all_tool_specs()) {
        INFO(spec.def.name);
        const std::string verb = verb_scope_of_tool(spec.def.name);
        if (spec.def.name == "gui_doctor" || spec.def.name == "gui_batch") {
            CHECK(verb.empty());
            continue;
        }
        REQUIRE_FALSE(verb.empty());
        CHECK_FALSE(validate_scope(verb));
        CHECK(verb.rfind(spec.family + ".", 0) == 0);
        CHECK(decide(token({verb}), spec.def.name, sample_args(spec.def.name)).code != "SCOPE_DENIED");
        // the verb scope grants exactly this tool
        for (const auto& other : all_tool_specs()) {
            if (other.def.name == spec.def.name) continue;
            CHECK_FALSE(tool_allowed_for(token({verb}), other));
        }
    }
}

TEST_CASE("scopes: tools/list shows exactly what the scopes allow", "[auth][scopes][dispatch]") {
    CHECK(listed(token({"session.list", "session.attach", "screen"})) ==
          std::set<std::string>{"gui_session_list", "gui_session_attach", "gui_screen_read", "gui_screen_find", "gui_screen_capture"});
    CHECK(listed(token({"session.disconnect"})) == std::set<std::string>{"gui_session_disconnect"});
    CHECK(listed(token({"session", "session.list"})).size() == 5);
    CHECK(listed(token({"system", "batch"})) == std::set<std::string>{"gui_doctor", "gui_batch"});
    CHECK(listed(token({})).empty());
    Principal star;
    star.all_scopes = true;
    CHECK(listed(star).size() == all_tool_specs().size());

    // read-only still hides write tools behind a verb scope
    Principal ro = token({"element.get", "element.fill"});
    ro.read_only = true;
    CHECK(listed(ro) == std::set<std::string>{"gui_element_get"});
}

TEST_CASE("scopes: gui_batch items are authorized per item by the same rule", "[auth][scopes][batch]") {
    const Principal p = token({"batch", "session.list", "screen.read"});
    auto batch = [&](json items) { return decide(p, "gui_batch", {{"items", items}}); };
    CHECK(batch(json::array({{{"tool", "gui_session_list"}}, {{"tool", "gui_screen_read"}}})).allowed);
    const auto d = batch(json::array({{{"tool", "gui_session_list"}}, {{"tool", "gui_session_disconnect"}}}));
    CHECK_FALSE(d.allowed);
    CHECK(d.code == "SCOPE_DENIED");
    CHECK(d.message.find("session.disconnect") != std::string::npos);
    CHECK(batch(json::array({{{"tool", "gui_screen_find"}}})).code == "SCOPE_DENIED");

    // through the dispatcher: refused up front, nothing runs
    Policy policy;
    policy.read_only = false;
    policy.allow_write = true;
    std::vector<std::vector<std::string>> calls;
    CommandDispatcher disp([&](const std::vector<std::string>& argv) { calls.push_back(argv); return Result{}; }, policy);
    CallContext ctx;
    ctx.principal = p;
    ctx.http = true;
    const auto r = disp.call_tool("gui_batch", {{"items", json::array({{{"tool", "gui_session_disconnect"}}})}}, ctx);
    CHECK(r.is_error);
    CHECK(calls.empty());
}

TEST_CASE("scopes: token create validates, normalizes and keeps entries as given", "[auth][scopes][token]") {
    Env env;
    auto store = env.store();
    NewToken t;
    t.name = "v";
    t.scopes = {"Session.List", "SCREEN", "session", "session.list"};
    const auto created = store.create(t);
    CHECK(created.meta.scopes == std::vector<std::string>{"session.list", "screen", "session", "session.list"});

    NewToken bad;
    bad.name = "b1";
    bad.scopes = {"session.nuke"};
    try {
        store.create(bad);
        FAIL("expected UNKNOWN_SCOPE");
    } catch (const AuthError& e) {
        CHECK(e.code() == "UNKNOWN_SCOPE");
        CHECK(std::string(e.what()).find("list, attach, launch, login, disconnect") != std::string::npos);
    }
    bad.scopes = {"nonsense.list"};
    try {
        store.create(bad);
        FAIL("expected UNKNOWN_FAMILY");
    } catch (const AuthError& e) {
        CHECK(e.code() == "UNKNOWN_FAMILY");
    }

    const auto rotated = store.rotate("v");  // rotate keeps the scopes untouched
    CHECK(rotated.meta.scopes == created.meta.scopes);
}

TEST_CASE("scopes: CLI default is verb-level and read-only", "[auth][scopes][cli]") {
    Env env;
    auto store = env.store();
    TokenCliArgs args;
    args.action = "create";
    args.name = "dflt";
    auto result = run_token_action(args, store);
    REQUIRE(result.status == Result::Status::Success);
    CHECK(result.data["scopes"] == nlohmann::json::array({"session.list", "session.attach", "connection.list", "screen"}));
    CHECK(result.data["read_only"] == true);

    // the default token cannot call launch/login/disconnect and does not see them
    const auto out = env.authenticate(result.data["token"].get<std::string>());
    REQUIRE(out.ok);
    CHECK(out.principal.scopes == std::set<std::string>{"session.list", "session.attach", "connection.list", "screen"});
    const auto names = listed(out.principal);
    CHECK(names.count("gui_session_list") == 1);
    CHECK(names.count("gui_session_attach") == 1);
    CHECK(names.count("gui_connection_list") == 1);
    CHECK(names.count("gui_screen_read") == 1);
    CHECK(names.count("gui_session_disconnect") == 0);
    CHECK(names.count("gui_session_launch") == 0);
    CHECK(names.count("gui_session_login") == 0);

    TokenCliArgs verb;
    verb.action = "create";
    verb.name = "verbs";
    verb.scopes = {"session.list,Session.Attach,screen"};
    result = run_token_action(verb, store);
    REQUIRE(result.status == Result::Status::Success);
    CHECK(result.data["scopes"] == nlohmann::json::array({"session.list", "session.attach", "screen"}));
    verb.name = "verbs2";
    verb.scopes = {"session.bogus"};
    result = run_token_action(verb, store);
    CHECK(result.status == Result::Status::Error);
    CHECK(result.error["code"] == "UNKNOWN_SCOPE");
}

TEST_CASE("scopes: old family-only records keep their meaning", "[auth][scopes][token]") {
    Env env;
    auto store = env.store();
    NewToken t;
    t.name = "old";
    t.scopes = {"session", "connection", "screen"};  // the former default
    const auto created = store.create(t);
    const auto out = env.authenticate(created.token);
    REQUIRE(out.ok);
    CHECK(out.principal.scopes == std::set<std::string>{"session", "connection", "screen"});
    const auto names = listed(out.principal);
    CHECK(names.count("gui_session_disconnect") == 1);
    CHECK(names.count("gui_session_launch") == 1);
    CHECK(decide(out.principal, "gui_session_login").allowed);
}

TEST_CASE("scopes: help text lists families and the generated verbs", "[auth][scopes][cli]") {
    const auto text = scope_help_text();
    for (const auto& family : command_table::families()) CHECK(text.find(family) != std::string::npos);
    for (const auto& scope : verb_scopes()) CHECK(text.find(scope) != std::string::npos);
    CHECK(text.find("session.list,session.attach,connection.list,screen") != std::string::npos);
}
