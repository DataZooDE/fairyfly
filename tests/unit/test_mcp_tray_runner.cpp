// HttpTrayRunner tests with a FAKE run function (no HTTP server, no sockets, no COM, no tray UI).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "include/mcp/http_tray_runner.h"

using namespace fairyfly;
using namespace fairyfly::mcp;

namespace {

using namespace std::chrono_literals;

// Simulates one running server: blocks until request_stop/request_restart.
class FakeServer final : public IServerControl {
public:
    ServerStatus status() const override {
        ServerStatus st;
        st.running = true;
        st.endpoint = "fake";
        st.read_only = read_only.load();
        return st;
    }
    void set_read_only(bool ro) override { read_only = ro; }
    void request_stop() override {
        std::lock_guard<std::mutex> g(m);
        stop = true;
        cv.notify_all();
    }
    void request_restart() override {
        std::lock_guard<std::mutex> g(m);
        restart = true;
        cv.notify_all();
    }
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    bool restart = false;
    std::atomic<bool> read_only{true};
};

struct FakeRunLog {
    std::mutex m;
    std::atomic<int> runs{0};
    std::atomic<bool> up{false};
    std::deque<int> exit_codes;  // scripted early exits (server never comes up)
    std::vector<bool> read_only_at_start;
    std::vector<bool> allow_write_at_start;
    std::atomic<bool> released{false};
    std::atomic<FakeServer*> current{nullptr};
};

template <class Pred>
bool wait_until(Pred p, std::chrono::milliseconds limit = 5000ms) {
    auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (p()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return p();
}

struct Harness {
    std::shared_ptr<FakeRunLog> log = std::make_shared<FakeRunLog>();
    std::unique_ptr<tray::IServerRunner> runner;
    std::future<int> result;
    std::thread thread;

    explicit Harness(bool allow_write = false) {
        ServeOptions opts;
        opts.allow_write = allow_write;
        auto lg = log;
        RunMcpFunction fn = [lg](const ServeOptions& o, const std::function<cli::CommandHandler&()>&,
                                 const commands::GlobalOptions&, audit::AuditSink*,
                                 const std::function<cli::CommandHandler*()>&, const HttpRunHooks* hooks) -> int {
            lg->runs++;
            {
                std::lock_guard<std::mutex> g(lg->m);
                lg->read_only_at_start.push_back(o.read_only);
                lg->allow_write_at_start.push_back(o.allow_write);
                if (!lg->exit_codes.empty()) {
                    int code = lg->exit_codes.front();
                    lg->exit_codes.pop_front();
                    return code;
                }
            }
            FakeServer server;
            server.read_only = o.read_only;
            lg->current = &server;
            lg->up = true;
            if (hooks && hooks->on_control) hooks->on_control(server);
            {
                std::unique_lock<std::mutex> lk(server.m);
                server.cv.wait(lk, [&] { return server.stop || server.restart || lg->released; });
                if (server.restart && hooks && hooks->restart_requested) *hooks->restart_requested = true;
            }
            lg->up = false;
            lg->current = nullptr;
            return 0;
        };
        runner = make_http_tray_runner(opts, {}, commands::GlobalOptions{}, nullptr, {}, std::move(fn));
    }

    void start_loop() {
        auto* r = runner.get();
        std::packaged_task<int()> task([r] { return r->run_blocking(); });
        result = task.get_future();
        thread = std::thread(std::move(task));
    }

    // Hard guard: a bug must fail the test, never hang CI.
    int join(std::chrono::milliseconds limit = 5000ms) {
        if (result.wait_for(limit) != std::future_status::ready) {
            log->released = true;
            if (auto* s = log->current.load()) s->request_stop();
            runner->request_quit();
            if (result.wait_for(2000ms) != std::future_status::ready) {
                std::cerr << "test_mcp_tray_runner: run_blocking hung; aborting\n";
                std::_Exit(3);
            }
            thread.join();
            FAIL("run_blocking did not return in time");
        }
        int code = result.get();
        thread.join();
        return code;
    }

    ~Harness() {
        if (thread.joinable()) {
            log->released = true;
            runner->request_quit();
            if (result.valid() && result.wait_for(3000ms) != std::future_status::ready) std::_Exit(3);
            thread.join();
        }
    }
};

} // namespace

TEST_CASE("tray runner: start -> running, quit ends run_blocking", "[mcp][tray_runner]") {
    Harness h;
    h.start_loop();
    REQUIRE(wait_until([&] { return h.log->up.load(); }));
    REQUIRE(wait_until([&] { return h.runner->control().status().running; }));
    h.runner->request_quit();
    CHECK(h.join() == 0);
    CHECK(h.log->runs == 1);
}

TEST_CASE("tray runner: stop keeps run_blocking alive, proxy valid, start runs again", "[mcp][tray_runner]") {
    Harness h;
    h.start_loop();
    REQUIRE(wait_until([&] { return h.runner->control().status().running; }));

    h.runner->control().request_stop();
    REQUIRE(wait_until([&] { return !h.log->up.load(); }));
    REQUIRE(wait_until([&] { return !h.runner->control().status().running; }));
    CHECK(h.result.wait_for(150ms) == std::future_status::timeout);  // still alive, idle
    CHECK(h.log->runs == 1);
    auto st = h.runner->control().status();  // proxy valid while stopped
    CHECK_FALSE(st.running);
    CHECK_FALSE(st.endpoint.empty());

    h.runner->control().request_start();
    REQUIRE(wait_until([&] { return h.log->runs == 2 && h.runner->control().status().running; }));
    h.runner->request_quit();
    CHECK(h.join() == 0);
}

TEST_CASE("tray runner: restart loops without leaving run_blocking", "[mcp][tray_runner]") {
    Harness h;
    h.start_loop();
    REQUIRE(wait_until([&] { return h.log->runs == 1 && h.runner->control().status().running; }));
    h.runner->control().request_restart();
    REQUIRE(wait_until([&] { return h.log->runs == 2 && h.runner->control().status().running; }));
    CHECK(h.result.wait_for(50ms) == std::future_status::timeout);
    h.runner->request_quit();
    CHECK(h.join() == 0);
}

TEST_CASE("tray runner: restart while stopped starts the server", "[mcp][tray_runner]") {
    Harness h;
    h.start_loop();
    REQUIRE(wait_until([&] { return h.runner->control().status().running; }));
    h.runner->control().request_stop();
    REQUIRE(wait_until([&] { return !h.log->up.load(); }));
    h.runner->control().request_restart();
    REQUIRE(wait_until([&] { return h.log->runs == 2 && h.log->up.load(); }));
    h.runner->request_quit();
    CHECK(h.join() == 0);
}

TEST_CASE("tray runner: quit while idle", "[mcp][tray_runner]") {
    Harness h;
    h.start_loop();
    REQUIRE(wait_until([&] { return h.runner->control().status().running; }));
    h.runner->control().request_stop();
    REQUIRE(wait_until([&] { return !h.log->up.load(); }));
    h.runner->request_quit();
    CHECK(h.join() == 0);
    CHECK(h.log->runs == 1);
}

TEST_CASE("tray runner: quit latched before run_blocking", "[mcp][tray_runner]") {
    Harness h;
    h.runner->request_quit();
    h.start_loop();
    CHECK(h.join() == 0);
    CHECK(h.log->runs == 0);
}

TEST_CASE("tray runner: failing start goes idle with a warning and does not busy-loop", "[mcp][tray_runner]") {
    Harness h;
    h.log->exit_codes.push_back(2);
    h.start_loop();
    REQUIRE(wait_until([&] { return h.log->runs == 1; }));
    REQUIRE(wait_until([&] {
        auto st = h.runner->control().status();
        return !st.running && !st.warnings.empty();
    }));
    std::this_thread::sleep_for(200ms);
    CHECK(h.log->runs == 1);  // no retry storm
    CHECK(h.result.wait_for(0ms) == std::future_status::timeout);

    h.runner->control().request_start();  // user retries; the fake now succeeds
    REQUIRE(wait_until([&] { return h.log->runs == 2 && h.runner->control().status().running; }));
    CHECK(h.runner->control().status().warnings.empty());
    h.runner->request_quit();
    CHECK(h.join() == 0);
}

TEST_CASE("tray runner: set_read_only while stopped applies at the next run", "[mcp][tray_runner]") {
    Harness h(/*allow_write=*/true);
    h.start_loop();
    REQUIRE(wait_until([&] { return h.runner->control().status().running; }));
    CHECK_FALSE(h.runner->control().status().read_only);

    h.runner->control().request_stop();
    REQUIRE(wait_until([&] { return !h.log->up.load(); }));
    h.runner->control().set_read_only(true);
    CHECK(h.runner->control().status().read_only);  // visible while stopped

    h.runner->control().request_start();
    REQUIRE(wait_until([&] { return h.log->runs == 2 && h.log->up.load(); }));
    {
        std::lock_guard<std::mutex> g(h.log->m);
        REQUIRE(h.log->read_only_at_start.size() == 2);
        CHECK_FALSE(h.log->read_only_at_start[0]);
        CHECK(h.log->read_only_at_start[1]);
        CHECK_FALSE(h.log->allow_write_at_start[1]);
    }
    h.runner->request_quit();
    CHECK(h.join() == 0);
}

TEST_CASE("tray runner: set_read_only while running reaches the server", "[mcp][tray_runner]") {
    Harness h(/*allow_write=*/true);
    h.start_loop();
    REQUIRE(wait_until([&] { return h.runner->control().status().running; }));
    h.runner->control().set_read_only(true);
    CHECK(h.runner->control().status().read_only);
    h.runner->request_quit();
    CHECK(h.join() == 0);
}
