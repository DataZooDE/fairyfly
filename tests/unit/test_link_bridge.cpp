// The desktop link inside fairyfly (D10): calls from libffl's worker threads run on the main (COM) thread, one at
// a time, with the per-call facts (tenant, user, client, request id, write grant) the dispatcher needs.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "include/link/main_thread_tools.h"
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
