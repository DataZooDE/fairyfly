// `fairyfly link pause|resume` and the link supervisor (D10): a paused desktop is disconnected from the cloud until
// resumed; `link status` and the tray read the same marker and the running link's status file.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <thread>

#include "include/link/link_control.h"

#include "support/temp_dir.h"
#include "support/eventually.h"
namespace fs = std::filesystem;
using namespace fairyfly::link;

using fairyfly::test::eventually;

namespace {
std::string state_of(const fs::path& st) {
    auto s = read_status(st.string());
    return s.is_object() ? s.value("state", "") : "";
}
/// Stops and joins a supervisor thread when the test ends, also when a REQUIRE fails (a joinable thread would terminate).
struct StopAndJoin {
    std::atomic<bool>& stop;
    std::thread& thread;
    ~StopAndJoin() {
        stop = true;
        if (thread.joinable()) thread.join();
    }
};
}  // namespace

TEST_CASE("link control: pause marker next to the pairing state", "[link]") {
    const fairyfly::test::TempDir dir("link-ctl");
    const auto st = dir / "state.json";
    CHECK_FALSE(is_paused(st.string()));
    REQUIRE(set_paused(st.string(), true));
    CHECK(is_paused(st.string()));
    CHECK(fs::exists(pause_marker(st.string())));
    CHECK(pause_marker(st.string()).parent_path() == st.parent_path());
    REQUIRE(set_paused(st.string(), true));  // idempotent
    REQUIRE(set_paused(st.string(), false));
    CHECK_FALSE(is_paused(st.string()));
    REQUIRE(set_paused(st.string(), false));  // idempotent
}

TEST_CASE("link control: the supervisor disconnects while paused and reconnects on resume", "[link]") {
    const fairyfly::test::TempDir dir("link-ctl");
    const auto st = dir / "state.json";
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
    const StopAndJoin join_sup{stop, sup};  // also when a REQUIRE below fails

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
}

TEST_CASE("link control: a link started while paused does not connect", "[link]") {
    const fairyfly::test::TempDir dir("link-ctl");
    const auto st = dir / "state.json";
    REQUIRE(set_paused(st.string(), true));
    std::atomic<int> starts{0};
    std::atomic<bool> stop{false};
    SupervisorOptions so;
    so.poll = std::chrono::milliseconds(10);
    std::thread sup([&] { supervise(st.string(), [&](const std::atomic<bool>&) { starts++; }, stop, so); });
    const StopAndJoin join_sup{stop, sup};  // also when a REQUIRE below fails
    CHECK(eventually([&] { return state_of(st) == "paused"; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(starts == 0);
    stop = true;
    sup.join();
}

TEST_CASE("link control: an agent that returns by itself is restarted with a back-off", "[link]") {
    const fairyfly::test::TempDir dir("link-ctl");
    const auto st = dir / "state.json";
    std::atomic<int> starts{0};
    std::atomic<bool> stop{false};
    SupervisorOptions so;
    so.poll = std::chrono::milliseconds(5);
    so.restart_delay = std::chrono::milliseconds(20);
    std::thread sup([&] { supervise(st.string(), [&](const std::atomic<bool>&) { starts++; }, stop, so); });
    const StopAndJoin join_sup{stop, sup};  // also when a REQUIRE below fails
    CHECK(eventually([&] { return starts >= 3; }));
    stop = true;
    sup.join();
}

TEST_CASE("link control: status of a link that is not running", "[link]") {
    const fairyfly::test::TempDir dir("link-ctl");
    const auto st = dir / "state.json";
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
}

TEST_CASE("link control: a reconnect request closes the link and opens it again at once", "[link]") {
    const fairyfly::test::TempDir dir("link-ctl");
    const auto st = dir / "state.json";
    std::atomic<int> starts{0}, running{0};
    std::atomic<bool> stop{false}, reconnect{false};
    auto agent = [&](const std::atomic<bool>& net_stop) {
        starts++;
        running++;
        while (!net_stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        running--;
    };
    SupervisorOptions so;
    so.poll = std::chrono::milliseconds(10);
    so.restart_delay = std::chrono::seconds(30);   // a reconnect must not wait for the back-off
    so.reconnect = &reconnect;
    std::thread sup([&] { supervise(st.string(), agent, stop, so); });
    const StopAndJoin join_sup{stop, sup};  // also when a REQUIRE below fails

    CHECK(eventually([&] { return running == 1 && starts == 1; }));
    reconnect = true;
    CHECK(eventually([&] { return starts == 2 && running == 1; }));
    CHECK_FALSE(reconnect);   // consumed

    stop = true;
    sup.join();
}

// ---- liveness: the status file names the process that wrote it; a dead or replaced process is "not running" ----
namespace {
/// The processes a status reader sees: pid -> identity; a pid not listed has ended.
struct FakeProbe : fairyfly::system::ProcessProbe {
    std::map<std::uint32_t, fairyfly::system::ProcessIdentity> running;
    std::optional<fairyfly::system::ProcessIdentity> find(std::uint32_t pid) override {
        const auto it = running.find(pid);
        if (it == running.end()) return std::nullopt;
        return it->second;
    }
};
constexpr const char* kLinkExe = "C:\\Users\\ada\\AppData\\Local\\Programs\\fairyfly\\fairyfly.exe";
nlohmann::json written_status(const std::string& state, std::int64_t updated, std::int64_t pid = 2892,
                              const std::string& image = kLinkExe, std::int64_t started = 1761700000) {
    nlohmann::json s{{"state", state}, {"pid", pid}, {"updated", updated}};
    if (!image.empty()) s["image"] = image;
    if (started) s["started"] = started;
    return s;
}
}  // namespace

TEST_CASE("link control: a status whose process has ended reads \"not running\" at once, not after the heartbeat window", "[link][liveness]") {
    // live 2026-10-29: the link was killed seconds before `fairyfly link status`, which still said "connected"
    FakeProbe probe;   // no process 2892
    const std::int64_t now = 1761750000;
    CHECK(describe(written_status("connected", now - 2), now, probe) == "not running");
    CHECK(describe(written_status("paused", now - 2), now, probe) == "not running");
    // a status of an older link (no image, no start time): its pid alone decides
    CHECK(describe(written_status("connected", now - 2, 2892, "", 0), now, probe) == "not running");
}

TEST_CASE("link control: a pid that now belongs to another program is not the link", "[link][liveness]") {
    const std::int64_t now = 1761750000;
    FakeProbe probe;
    probe.running[2892] = {"C:\\Windows\\System32\\notepad.exe", 1761740000};
    CHECK(describe(written_status("connected", now - 2), now, probe) == "not running");
    // the same program started again under the reused pid (another start time) is not the link that wrote it either
    probe.running[2892] = {kLinkExe, 1761749990};
    CHECK(describe(written_status("connected", now - 2), now, probe) == "not running");
}

TEST_CASE("link control: the live link with a fresh heartbeat is connected; a stale heartbeat keeps the hung-link rule", "[link][liveness]") {
    const std::int64_t now = 1761750000;
    FakeProbe probe;
    // the probe may spell the path differently (case, separators, \\?\ prefix) and round the start time
    probe.running[2892] = {"\\\\?\\c:/users/ada/appdata/local/programs/fairyfly/FAIRYFLY.EXE", 1761700001};
    CHECK(describe(written_status("connected", now - 5), now, probe) == "connected");
    CHECK(describe(written_status("paused", now - 5), now, probe) == "paused");
    // alive but no status for longer than the heartbeat window: hung, as before
    CHECK(describe(written_status("connected", now - kStatusStaleSeconds - 30), now, probe) == "not running");
    // a process the reader may not inspect (empty identity) still counts as running
    probe.running[2892] = {};
    CHECK(describe(written_status("connected", now - 5), now, probe) == "connected");
    CHECK(describe(written_status("stopped", now - 5), now, probe) == "not running");
}

TEST_CASE("link control: link status keeps what the ended link last wrote as last_seen", "[link][liveness]") {
    const std::int64_t now = 1761750000;
    FakeProbe probe;
    const auto status = written_status("connected", now - 3);
    const auto gone = link_liveness(status, now, probe);
    CHECK(gone.link == "not running");
    REQUIRE(gone.last_seen.is_object());
    CHECK(gone.last_seen["state"] == "connected");
    CHECK(gone.last_seen["updated"] == now - 3);
    CHECK(gone.last_seen["pid"] == 2892);
    probe.running[2892] = {kLinkExe, 1761700000};
    const auto alive = link_liveness(status, now, probe);
    CHECK(alive.link == "connected");
    CHECK(alive.last_seen.is_null());
    // no status file at all: nothing was seen
    CHECK(link_liveness(nlohmann::json(), now, probe).last_seen.is_null());
}

TEST_CASE("link control: the status names the process that wrote it", "[link][liveness]") {
    const fairyfly::test::TempDir dir("link-ctl-who");
    const auto st = dir / "state.json";
    write_status(st.string(), "connected");
    const auto s = read_status(st.string());
    REQUIRE(s.contains("image"));
    CHECK(fairyfly::system::same_executable(s["image"].get<std::string>(), fairyfly::system::current_process_identity().image));
    // this test process runs, so the status it wrote reads as written
    CHECK(describe(s, s["updated"].get<std::int64_t>() + 1, fairyfly::system::os_process_probe()) == "connected");
}
