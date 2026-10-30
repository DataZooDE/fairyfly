// The desktop link inside fairyfly (D10): calls from libffl's worker threads run on the main (COM) thread, one at
// a time, with the per-call facts (tenant, user, client, request id, write grant) the dispatcher needs.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "include/link/desktop_approval.h"
#include "include/link/main_thread_tools.h"
#include "include/mcp/protocol_session.h"
#include "include/mcp/tool_catalog.h"
#include "include/mcp/dispatcher.h"
#include "include/command_table.h"
#include "include/link/link_call.h"
#include "assertion.hpp"
#include "signer.hpp"

using fairyfly::link::MainThreadTools;
using nlohmann::json;

TEST_CASE("link bridge: reports queue and execution without exposing call arguments", "[link][latency]") {
    MainThreadTools tools;
    tools.set_handlers([] { return json::array(); }, [](const std::string&, const json&, const ffy::link::CallInfo&) {
        return json{{"content", json::array()}, {"isError", true}, {"_meta", {{"existing", true}}}};
    });
    json result;
    std::thread worker([&] {
        result = tools.rpc("tools/call", {{"name", "gui_screen_read"}, {"arguments", {{"secret", "do-not-copy"}}}},
            3000, {"tenant", "user", "client", "request-123", false});
    });
    tools.pump_once(std::chrono::milliseconds(2000));
    worker.join();
    CHECK(result["result"]["isError"] == true);
    CHECK(result["result"]["_meta"]["existing"] == true);
    const auto timing = result["result"]["_meta"]["fairyfly/desktop_timing"];
    REQUIRE(timing.is_object());
    CHECK(timing["request_id"] == "request-123");
    CHECK(timing["queue_ms"].get<int64_t>() >= 0);
    CHECK(timing["execute_ms"].get<int64_t>() >= 0);
    CHECK(timing.dump().find("do-not-copy") == std::string::npos);
    CHECK(timing.size() == 3);
}

TEST_CASE("link bridge: worker-thread calls run on the pumping thread, one at a time", "[link]") {
    MainThreadTools tools;
    const auto main_id = std::this_thread::get_id();
    std::atomic<int> running{0}, max_running{0}, on_main{0};
    std::vector<ffy::link::CallInfo> seen;
    tools.set_handlers([] { return json::array({{{"name", "gui_screen_read"}}}); },
                       [&](const std::string& name, const json& args, const ffy::link::CallInfo& info) {
                           int now = ++running;
                           max_running = std::max(max_running.load(), now);
                           if (std::this_thread::get_id() == main_id) on_main++;
                           seen.push_back(info);
                           std::this_thread::sleep_for(std::chrono::milliseconds(5));
                           --running;
                           return json{{"content", json::array({{{"type", "text"}, {"text", name + ":" + args.value("id", "")}}})}, {"isError", false}};
                       });
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    std::atomic<int> ok{0};
    for (int i = 0; i < 8; i++)
        workers.emplace_back([&, i] {
            ffy::link::CallInfo info{"acme", "alice@acme.example", "claude", "req-" + std::to_string(i), i == 3};
            auto r = tools.rpc("tools/call", {{"name", "gui_screen_read"}, {"arguments", {{"id", std::to_string(i)}}}}, 5000, info);
            if (r["result"]["content"][0]["text"] == "gui_screen_read:" + std::to_string(i)) ok++;
        });
    std::thread stopper([&] {
        for (auto& w : workers) w.join();
        stop = true;
    });
    tools.pump(stop);  // this thread plays fairyfly's main thread
    stopper.join();
    CHECK(ok == 8);
    CHECK(on_main == 8);
    CHECK(max_running == 1);
    REQUIRE(seen.size() == 8);
    int writes = 0;
    for (auto& s : seen) {
        CHECK(s.tenant == "acme");
        CHECK(s.client == "claude");
        writes += s.write_granted;
    }
    CHECK(writes == 1);
    CHECK(tools.per_call_read_only());
}

TEST_CASE("link bridge: a call with the write grant keeps it only with the desktop's permit", "[link]") {
    // the desktop's own check that this exact call was approved here (approval::take_permit in the link)
    MainThreadTools tools;
    std::vector<bool> seen;
    tools.set_handlers([] { return json::array(); },
                       [&](const std::string&, const json&, const ffy::link::CallInfo& info) {
                           seen.push_back(info.write_granted);
                           return json{{"content", json::array()}, {"isError", false}};
                       });
    std::vector<std::string> asked;
    tools.set_write_permit([&](const std::string& tool, const json& args) {
        asked.push_back(tool);
        return args.value("ok", false);
    });
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        ffy::link::CallInfo granted{"acme", "alice", "claude", "r1", true};
        ffy::link::CallInfo read_only{"acme", "alice", "claude", "r2", false};
        tools.rpc("tools/call", {{"name", "gui_key_send"}, {"arguments", {{"ok", true}}}}, 5000, granted);
        tools.rpc("tools/call", {{"name", "gui_key_send"}, {"arguments", {{"ok", false}}}}, 5000, granted);
        tools.rpc("tools/call", {{"name", "gui_screen_read"}, {"arguments", json::object()}}, 5000, read_only);
        stop = true;
    });
    tools.pump(stop);
    worker.join();
    CHECK(seen == std::vector<bool>{true, false, false});
    CHECK(asked == std::vector<std::string>{"gui_key_send", "gui_key_send"});  // a read-only grant needs no permit
}

TEST_CASE("link bridge: tools/list, timeouts when nobody pumps, shutdown", "[link]") {
    MainThreadTools tools;
    tools.set_handlers([] { return json::array({{{"name", "gui_screen_read"}}}); }, nullptr);
    std::atomic<bool> stop{false};
    std::thread main([&] { tools.pump_once(std::chrono::milliseconds(2000)); });
    auto list = tools.rpc("tools/list", json::object(), 3000, {});
    main.join();
    CHECK(list["result"]["tools"][0]["name"] == "gui_screen_read");
    // nobody pumps: the caller gets an error instead of hanging, and the call never runs later
    CHECK_THROWS(tools.rpc("tools/call", {{"name", "gui_screen_read"}}, 100, {}));
    CHECK_FALSE(tools.pump_once(std::chrono::milliseconds(10)));
    tools.shutdown();
    CHECK_THROWS(tools.rpc("tools/list", json::object(), 1000, {}));
}

namespace {
struct TestSigner : ffy::Signer {  // stands in for the gateway's per-tenant KMS key
    ffy::PKey k = ffy::ec_generate();
    std::string sign(const std::string& m) override { return ffy::es256_sign(k, m); }
    std::string kid() override { return ffy::jwk_from_ec(k).kid; }
    std::vector<ffy::Jwk> public_keys() override { return {ffy::jwk_from_ec(k)}; }
};
}  // namespace

TEST_CASE("link bridge: a signed call from the gateway reaches the dispatcher on the main thread", "[link]") {
    TestSigner gw;
    auto tools = std::make_shared<MainThreadTools>();
    const auto main_id = std::this_thread::get_id();
    bool ran_on_main = false;
    ffy::link::CallInfo got;
    tools->set_handlers([] { return json::array({{{"name", "gui_screen_read"}, {"annotations", {{"readOnlyHint", true}}}}}); },
                        [&](const std::string&, const json&, const ffy::link::CallInfo& info) {
                            ran_on_main = std::this_thread::get_id() == main_id;
                            got = info;
                            return json{{"content", json::array({{{"type", "text"}, {"text", "SAP screen"}}})}, {"isError", false}};
                        });
    ffy::link::State st;
    st.device_id = "dev_1"; st.issuer = "ffy-gateway"; st.tenant = "acme"; st.gateway_keys = gw.public_keys(); st.key = ffy::ec_generate();
    ffy::link::AgentConfig cfg;
    cfg.local = tools;
    ffy::link::Agent agent(st, cfg);
    ffy::assertion::Claims c;
    c.iss = "ffy-gateway"; c.aud = "dev_1"; c.tenant = "acme"; c.sub = "lee@acme.example"; c.jti = ffy::rand_hex(8); c.client = "claude";
    c.iat = ffy::now_unix(); c.exp = c.iat + 60; c.req = "inst.1";
    c.grants.tools = {"screen"};
    ffy::proto::Frame f;
    f.t = ffy::proto::TReq; f.id = c.req; f.assertion = ffy::assertion::sign(gw, c);
    f.rpc = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "gui_screen_read"}, {"arguments", json::object()}}}};
    std::atomic<bool> stop{false};
    ffy::proto::Frame res;
    std::thread worker([&] {  // libffl delivers each request on its own thread
        res = agent.process(f);
        stop = true;
    });
    tools->pump(stop);
    worker.join();
    REQUIRE_FALSE(res.error);
    CHECK(res.rpc["result"]["content"][0]["text"] == "SAP screen");
    CHECK(ran_on_main);
    CHECK(got.tenant == "acme");
    CHECK(got.user == "lee@acme.example");
    CHECK(got.request_id == "inst.1");
    CHECK_FALSE(got.write_granted);
}

TEST_CASE("link bridge: posted tasks run on the pumping thread between calls", "[link]") {
    MainThreadTools tools;
    std::thread::id ran_on, pumping;
    REQUIRE(tools.post([&] { ran_on = std::this_thread::get_id(); }));
    std::thread main([&] {
        pumping = std::this_thread::get_id();
        tools.pump_once(std::chrono::milliseconds(2000));
    });
    main.join();
    CHECK(ran_on == pumping);
    tools.shutdown();
    CHECK_FALSE(tools.post([] {}));
}

namespace {
// fairyfly's own tool catalog as libffl's agent reads it; every call succeeds (nothing reaches SAP)
struct CatalogTools : ffy::link::LocalTools {
    std::vector<std::string> called;
    json rpc(const std::string& method, const json& params, int, const ffy::link::CallInfo&) override {
        if (method == "tools/list") {
            json tools = json::array();
            for (const auto& spec : fairyfly::mcp::all_tool_specs()) tools.push_back(fairyfly::mcp::tool_to_json(spec.def));
            return {{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"tools", tools}}}};
        }
        called.push_back(params.value("name", ""));
        return {{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"content", json::array({{{"type", "text"}, {"text", "closed"}}})}, {"isError", false}}}};
    }
    bool per_call_read_only() const override { return true; }
};
struct CountingApprover : ffy::link::Approver {
    std::atomic<int> asked{0};
    bool approve(const std::string&) override { return false; }
    ffy::link::ApprovalResult decide(const ffy::link::ApprovalPrompt&) override {
        ++asked;
        return {false, "denied"};
    }
};
}  // namespace

TEST_CASE("link bridge: gui_popup_close asks for approval only when it confirms the popup", "[link]") {
    // review of #34: annotated destructive, gui_popup_close asked on every close, F12 (Cancel) too. With fairyfly's
    // catalog it is no change by its annotation; libffl asks for the keys that confirm a popup (read-only guard rule).
    const auto specs = fairyfly::mcp::all_tool_specs();
    for (const auto& spec : specs)
        if (spec.def.name == "gui_popup_close") CHECK(spec.def.annotations["destructiveHint"] == false);
    TestSigner gw;
    auto tools = std::make_shared<CatalogTools>();
    auto who = std::make_shared<CountingApprover>();
    ffy::link::State st;
    st.device_id = "dev_1"; st.issuer = "ffy-gateway"; st.tenant = "acme"; st.gateway_keys = gw.public_keys(); st.key = ffy::ec_generate();
    ffy::link::AgentConfig cfg;
    cfg.local = tools;
    cfg.approver = who;
    cfg.approve_window_s = 0;  // as fairyfly's link runs it: every change asks
    ffy::link::Agent agent(st, cfg);
    REQUIRE(agent.refresh_local());
    auto call = [&](const std::string& tool, const json& args) {
        ffy::assertion::Claims c;
        c.iss = "ffy-gateway"; c.aud = "dev_1"; c.tenant = "acme"; c.sub = "lee@acme.example"; c.jti = ffy::rand_hex(8); c.client = "claude";
        c.iat = ffy::now_unix(); c.exp = c.iat + 60; c.req = "inst." + c.jti;
        c.grants.tools = {"gui_batch", "gui_popup_close"};
        c.grants.allow_write = true;
        c.grants.approval = "device";
        ffy::proto::Frame f;
        f.t = ffy::proto::TReq; f.id = c.req; f.assertion = ffy::assertion::sign(gw, c);
        f.rpc = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
        auto res = agent.process(f);
        return res.error ? res.error->code : std::string();
    };
    auto batch = [](const json& args) { return json{{"items", json::array({{{"tool", "gui_popup_close"}, {"arguments", args}}})}}; };
    // Enter confirms the popup: asked, single and in a batch
    CHECK(call("gui_popup_close", {{"vkey", 0}}) == "APPROVAL_DENIED");
    CHECK(who->asked == 1);
    CHECK(call("gui_batch", batch({{"vkey", 0}})) == "APPROVAL_DENIED");
    CHECK(who->asked == 2);
    // F12 (also the default) and Shift+F3 cancel it: not asked, the call runs
    for (const json& args : {json{{"vkey", 12}}, json{{"vkey", 15}}, json::object()}) {
        INFO(args.dump());
        CHECK(call("gui_popup_close", args) == "");
        CHECK(call("gui_batch", batch(args)) == "");
    }
    CHECK(who->asked == 2);
    CHECK(tools->called.size() == 6);
}

namespace {
// the person at the desktop, scripted: the window approves and ticks "Don't ask again"; no notification
struct WindowUi : fairyfly::link::approval::Ui {
    std::vector<fairyfly::link::approval::Request> windows;
    bool notify(const fairyfly::link::approval::Request&) override { return false; }
    void withdraw(const fairyfly::link::approval::Request&) override {}
    fairyfly::link::approval::Answer ask(const fairyfly::link::approval::Request& r, const std::function<bool()>&, bool& remember) override {
        windows.push_back(r);
        remember = true;
        return fairyfly::link::approval::Answer::Approve;
    }
};
}  // namespace

TEST_CASE("link bridge: the administrator's \"Always ask at the desktop\" asks every change, \"Allow changes in SAP\" on or not", "[link]") {
    // approval model v2: the gateway pushes approval_enforced (ready.policy, policy frames) and signs "always" into
    // each call; the tray's switch then counts as off and the window offers no "Don't ask again"
    namespace approval = fairyfly::link::approval;
    TestSigner gw;
    auto tools = std::make_shared<CatalogTools>();
    auto ui = std::make_shared<WindowUi>();
    auto standing = std::make_shared<approval::Standing>();
    REQUIRE(standing->set(true));
    approval::Desk desk;
    desk.standing = standing;
    ffy::link::State st;
    st.device_id = "dev_1"; st.issuer = "ffy-gateway"; st.tenant = "acme"; st.gateway_keys = gw.public_keys(); st.key = ffy::ec_generate();
    ffy::link::AgentConfig cfg;
    cfg.local = tools;
    cfg.approver = approval::make_approver(std::make_shared<approval::Broker>(ui, desk));
    cfg.approve_window_s = 0;
    ffy::link::Agent agent(st, cfg);
    REQUIRE(agent.refresh_local());
    auto call = [&](const std::string& approval_setting) {
        ffy::assertion::Claims c;
        c.iss = "ffy-gateway"; c.aud = "dev_1"; c.tenant = "acme"; c.sub = "lee@acme.example"; c.jti = ffy::rand_hex(8); c.client = "claude";
        c.iat = ffy::now_unix(); c.exp = c.iat + 60; c.req = "inst." + c.jti;
        c.grants.tools = {"gui_key_send"};
        c.grants.allow_write = true;
        c.grants.approval = approval_setting;
        ffy::proto::Frame f;
        f.t = ffy::proto::TReq; f.id = c.req; f.assertion = ffy::assertion::sign(gw, c);
        f.rpc = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "gui_key_send"}, {"arguments", {{"vkey", 11}}}}}};
        auto res = agent.process(f);
        return res.error ? res.error->code : std::string();
    };
    // the user decides: the switch is on, nobody is asked
    CHECK(call("device") == "");
    CHECK(ui->windows.empty());
    // the admin's push greys out the switch: the change asks, a yes runs it, the tick turns nothing on
    agent.set_link_policy(ffy::proto::LinkPolicy{true});
    CHECK(standing->enforced());
    CHECK_FALSE(standing->on());
    CHECK(call("device") == "");
    REQUIRE(ui->windows.size() == 1);
    CHECK_FALSE(ui->windows[0].offer_standing);
    // the admin lets the user decide again: the saved switch is back
    agent.set_link_policy(ffy::proto::LinkPolicy{false});
    CHECK_FALSE(standing->enforced());
    CHECK(standing->on());
    CHECK(call("device") == "");
    CHECK(ui->windows.size() == 1);
    // a call signed "always" asks even before the push reached this desktop
    CHECK(call("always") == "");
    REQUIRE(ui->windows.size() == 2);
    CHECK_FALSE(ui->windows[1].offer_standing);
    CHECK(standing->on());
    CHECK(tools->called.size() == 4);
}

namespace {
struct ApprovingApprover : ffy::link::Approver {
    std::atomic<int> asked{0};
    bool approve(const std::string&) override { return true; }
    ffy::link::ApprovalResult decide(const ffy::link::ApprovalPrompt&) override {
        ++asked;
        return {true, "approved"};
    }
};
}  // namespace

TEST_CASE("link bridge: a change approved at the desktop runs after a stale default connection is re-attached, asked once", "[link][reattach]") {
    // seen live 2026-10-24/25: after the link tray or SAP Logon restarted, the approved change failed with
    // INVALID_CONNECTION and had to be approved again. libffl asks before the call reaches fairyfly, so the re-attach
    // happens inside the same call and uses the approval it already has.
    using fairyfly::Result;
    using Argv = std::vector<std::string>;
    TestSigner gw;
    std::vector<Argv> calls;
    bool attached = false;
    auto invoker = [&](const Argv& argv) {
        calls.push_back(argv);
        Result r;
        const std::string command = fairyfly::command_table::command_of_argv(argv);
        if (command == "session list") {
            r.status = Result::Status::Success;
            r.data = {{"connections", json::array({{{"description", "Bigfox"},
                                                    {"sessions", json::array({{{"id", "/app/con[1]/ses[0]"}, {"system", "BFX"},
                                                                               {"client", "100"}, {"user", "LEE"}}})}}})}};
        } else if (command == "session attach") {
            attached = true;
            r.status = Result::Status::Success;
            r.data = {{"connection_file_id", 1}};
        } else if (attached) {
            r.status = Result::Status::Success;
            r.data = {{"message", "key sent"}};
        } else {
            r.status = Result::Status::Error;
            r.error = {{"code", "INVALID_CONNECTION"}, {"message", "Connection 0 session no longer exists"},
                       {"stale_connection", {{"id", 0}, {"system", "BFX"}, {"client", "100"}, {"user", "LEE"}}}};
        }
        return r;
    };
    fairyfly::mcp::Policy policy;
    policy.auto_attach = true;
    policy.read_only = false;
    policy.allow_write = true;
    fairyfly::mcp::CommandDispatcher dispatcher(invoker, policy, nullptr);
    std::atomic<bool> stop{false};
    auto tools = std::make_shared<MainThreadTools>();
    tools->set_handlers([&] {
                            json list = json::array();
                            for (const auto& t : dispatcher.list_tools()) list.push_back(fairyfly::mcp::tool_to_json(t));
                            return list;
                        },
                        [&](const std::string& name, const json& args, const ffy::link::CallInfo& info) {
                            auto r = dispatcher.call_tool(name, args, fairyfly::link::link_call_context(info, stop));
                            return json{{"content", r.content}, {"isError", r.is_error}};
                        });
    auto who = std::make_shared<ApprovingApprover>();
    ffy::link::State st;
    st.device_id = "dev_1"; st.issuer = "ffy-gateway"; st.tenant = "acme"; st.gateway_keys = gw.public_keys(); st.key = ffy::ec_generate();
    ffy::link::AgentConfig cfg;
    cfg.local = tools;
    cfg.approver = who;
    cfg.approve_window_s = 0;  // every change asks
    ffy::link::Agent agent(st, cfg);
    ffy::assertion::Claims c;
    c.iss = "ffy-gateway"; c.aud = "dev_1"; c.tenant = "acme"; c.sub = "lee@acme.example"; c.jti = ffy::rand_hex(8); c.client = "claude";
    c.iat = ffy::now_unix(); c.exp = c.iat + 60; c.req = "inst.1";
    c.grants.tools = {"gui_key_send"};
    c.grants.allow_write = true;
    c.grants.approval = "device";
    ffy::proto::Frame f;
    f.t = ffy::proto::TReq; f.id = c.req; f.assertion = ffy::assertion::sign(gw, c);
    f.rpc = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "gui_key_send"}, {"arguments", {{"key", "f11"}}}}}};
    ffy::proto::Frame res;
    bool refreshed = false;
    std::thread worker([&] {  // libffl's thread: reads the catalog, asks the approver, then the call runs on the main thread
        refreshed = agent.refresh_local();
        if (refreshed) res = agent.process(f);
        stop = true;
    });
    tools->pump(stop);
    worker.join();
    REQUIRE(refreshed);
    INFO(res.rpc.dump());
    REQUIRE_FALSE(res.error);
    CHECK_FALSE(res.rpc["result"].value("isError", true));
    CHECK(res.rpc["result"].dump().find("re-attached to the only open SAP session") != std::string::npos);
    CHECK(who->asked == 1);
    REQUIRE(calls.size() == 4);
    CHECK(fairyfly::command_table::command_of_argv(calls[0]) == "key send");
    CHECK(calls[1] == Argv{"session", "list"});
    CHECK(calls[2] == Argv{"session", "attach", "--session-id", "/app/con[1]/ses[0]"});
    CHECK(fairyfly::command_table::command_of_argv(calls[3]) == "key send");
}

// Live 2026-10-27: the Windows VM woke up with its clock 5 min 46 s behind and every call came back as a bare
// "ASSERTION_REJECTED: the desktop rejected the gateway's authorization". The gateway mints iat = its now.
TEST_CASE("link bridge: a desktop clock 346 s behind is refused as clock skew, with the offset and the fix", "[link][clock]") {
    TestSigner gw;
    ffy::link::State st;
    st.device_id = "dev_1"; st.issuer = "ffy-gateway"; st.tenant = "acme"; st.gateway_keys = gw.public_keys(); st.key = ffy::ec_generate();
    ffy::link::AgentConfig cfg;
    cfg.local = std::make_shared<MainThreadTools>();
    ffy::link::Agent agent(st, cfg);
    ffy::assertion::Claims c;
    c.iss = "ffy-gateway"; c.aud = "dev_1"; c.tenant = "acme"; c.sub = "lee@acme.example"; c.jti = ffy::rand_hex(8); c.client = "claude";
    c.iat = ffy::now_unix() + 346; c.exp = c.iat + 60; c.req = "inst.skew";   // the gateway's clock, 346 s ahead of ours
    c.grants.tools = {"screen"};
    ffy::proto::Frame f;
    f.t = ffy::proto::TReq; f.id = c.req; f.assertion = ffy::assertion::sign(gw, c);
    f.rpc = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "gui_screen_read"}, {"arguments", json::object()}}}};
    const auto res = agent.process(f);
    REQUIRE(res.error);
    CHECK(res.error->code == "ASSERTION_CLOCK_SKEW");
    CHECK(res.error->message.find("346 s behind") != std::string::npos);
    CHECK(res.error->message.find("Sync now") != std::string::npos);
}

// A stale assertion (retry, queued frame, replay) with iat not in the future proves nothing about the clock: neutral.
TEST_CASE("link bridge: an assertion that arrives after it expired is ASSERTION_EXPIRED, not clock skew", "[link][clock]") {
    TestSigner gw;
    ffy::link::State st;
    st.device_id = "dev_1"; st.issuer = "ffy-gateway"; st.tenant = "acme"; st.gateway_keys = gw.public_keys(); st.key = ffy::ec_generate();
    ffy::link::AgentConfig cfg;
    cfg.local = std::make_shared<MainThreadTools>();
    int heard = 0;
    cfg.on_clock_skew = [&](std::int64_t) { ++heard; };
    ffy::link::Agent agent(st, cfg);
    ffy::assertion::Claims c;
    c.iss = "ffy-gateway"; c.aud = "dev_1"; c.tenant = "acme"; c.sub = "lee@acme.example"; c.jti = ffy::rand_hex(8); c.client = "claude";
    c.iat = ffy::now_unix() - 300; c.exp = c.iat + 60; c.req = "inst.late";
    c.grants.tools = {"screen"};
    ffy::proto::Frame f;
    f.t = ffy::proto::TReq; f.id = c.req; f.assertion = ffy::assertion::sign(gw, c);
    f.rpc = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "gui_screen_read"}, {"arguments", json::object()}}}};
    const auto res = agent.process(f);
    REQUIRE(res.error);
    CHECK(res.error->code == "ASSERTION_EXPIRED");
    CHECK(res.error->message.find("too late") != std::string::npos);
    CHECK(heard == 0);   // once is no reason to turn the tray yellow
}
