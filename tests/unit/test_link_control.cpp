// `fairyfly link pause|resume` and the link supervisor (D10): a paused desktop is disconnected from the cloud until
// resumed; `link status` and the tray read the same marker and the running link's status file.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "include/link/link_control.h"

namespace fs = std::filesystem;
using namespace fairyfly::link;

namespace {
fs::path temp_state() {
    auto d = fs::temp_directory_path() / ("ffy-link-ctl-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(d);
    return d / "state.json";
}
std::string state_of(const fs::path& st) {
    auto s = read_status(st.string());
    return s.is_object() ? s.value("state", "") : "";
}
template <class F>
bool eventually(F f, int ms = 3000) {
    for (int i = 0; i < ms / 10; i++) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}
}  // namespace

TEST_CASE("link control: pause marker next to the pairing state", "[link]") {
    auto st = temp_state();
    CHECK_FALSE(is_paused(st.string()));
    REQUIRE(set_paused(st.string(), true));
    CHECK(is_paused(st.string()));
    CHECK(fs::exists(pause_marker(st.string())));
    CHECK(pause_marker(st.string()).parent_path() == st.parent_path());
    REQUIRE(set_paused(st.string(), true));  // idempotent
    REQUIRE(set_paused(st.string(), false));
    CHECK_FALSE(is_paused(st.string()));
    REQUIRE(set_paused(st.string(), false));  // idempotent
    fs::remove_all(st.parent_path());
}

TEST_CASE("link control: the supervisor disconnects while paused and reconnects on resume", "[link]") {
    auto st = temp_state();
    std::atomic<int> starts{0}, running{0};
    std::atomic<bool> stop{false};
    // stands in for libffl's Agent::run(net_stop): connected until net_stop
    auto agent = [&](const std::atomic<bool>& net_stop) {
        starts++;
        running++;
        while (!net_stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        running--;
    };
    SupervisorOptions so;
    so.poll = std::chrono::milliseconds(10);
    std::thread sup([&] { supervise(st.string(), agent, stop, so); });

    CHECK(eventually([&] { return running == 1; }));
    CHECK(eventually([&] { return state_of(st) == "connected"; }));
    REQUIRE(set_paused(st.string(), true));
    CHECK(eventually([&] { return running == 0; }));  // paused: the link is closed, the cloud cannot reach this desktop
    CHECK(eventually([&] { return state_of(st) == "paused"; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(starts == 1);  // stays closed while paused
    REQUIRE(set_paused(st.string(), false));
    CHECK(eventually([&] { return running == 1 && starts == 2; }));

    stop = true;
    sup.join();
    CHECK(running == 0);
    CHECK(state_of(st) == "stopped");
    fs::remove_all(st.parent_path());
}

TEST_CASE("link control: a link started while paused does not connect", "[link]") {
    auto st = temp_state();
    REQUIRE(set_paused(st.string(), true));
    std::atomic<int> starts{0};
    std::atomic<bool> stop{false};
    SupervisorOptions so;
    so.poll = std::chrono::milliseconds(10);
    std::thread sup([&] { supervise(st.string(), [&](const std::atomic<bool>&) { starts++; }, stop, so); });
    CHECK(eventually([&] { return state_of(st) == "paused"; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(starts == 0);
    stop = true;
    sup.join();
    fs::remove_all(st.parent_path());
}

TEST_CASE("link control: an agent that returns by itself is restarted with a back-off", "[link]") {
    auto st = temp_state();
    std::atomic<int> starts{0};
    std::atomic<bool> stop{false};
    SupervisorOptions so;
    so.poll = std::chrono::milliseconds(5);
    so.restart_delay = std::chrono::milliseconds(20);
    std::thread sup([&] { supervise(st.string(), [&](const std::atomic<bool>&) { starts++; }, stop, so); });
    CHECK(eventually([&] { return starts >= 3; }));
    stop = true;
    sup.join();
    fs::remove_all(st.parent_path());
}

TEST_CASE("link control: status of a link that is not running", "[link]") {
    auto st = temp_state();
    CHECK(read_status(st.string()).is_null());
    write_status(st.string(), "connected");
    auto s = read_status(st.string());
    CHECK(s["state"] == "connected");
    CHECK(s["pid"].is_number_integer());
    CHECK(s["updated"].is_number_integer());
    // a status older than the staleness window means the process is gone
    CHECK(describe(s, s["updated"].get<int64_t>() + 5) == "connected");
    CHECK(describe(s, s["updated"].get<int64_t>() + 120) == "not running");
    CHECK(describe(nlohmann::json(), 0) == "not running");
    fs::remove_all(st.parent_path());
}
