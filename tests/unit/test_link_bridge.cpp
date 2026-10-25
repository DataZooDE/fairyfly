// The desktop link inside fairyfly (D10): calls from libffl's worker threads run on the main (COM) thread, one at
// a time, with the per-call facts (tenant, user, client, request id, write grant) the dispatcher needs.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "include/link/main_thread_tools.h"
#include "include/mcp/protocol_session.h"
#include "include/mcp/tool_catalog.h"
#include "assertion.hpp"
#include "signer.hpp"

using fairyfly::link::MainThreadTools;
using nlohmann::json;

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
