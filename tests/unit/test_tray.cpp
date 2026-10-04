// Tray tests: pure model, builders, controller and entry decisions with FAKES only. No tray icon,
// window, registry key, mutex or process is ever created here.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <map>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "include/tray/tray.h"

using namespace fairyfly;
using namespace fairyfly::tray;

namespace {

mcp::ServerStatus make_status(bool running, bool read_only, std::vector<std::string> warnings = {}) {
    mcp::ServerStatus s;
    s.running = running;
    s.read_only = read_only;
    s.endpoint = "http://127.0.0.1:8383/mcp";
    s.warnings = std::move(warnings);
    return s;
}

const MenuItem* find_item(const TrayMenuModel& m, TrayAction action) {
    for (const auto& i : m.items) {
        if (i.action == action && !i.separator) return &i;
        for (const auto& c : i.children)
            if (c.action == action) return &c;
    }
    return nullptr;
}

std::vector<std::string> top_labels(const TrayMenuModel& m) {
    std::vector<std::string> out;
    for (const auto& i : m.items) out.push_back(i.separator ? "---" : i.label);
    return out;
}

// ---- fakes ----
struct FakeControl : mcp::IServerControl {
    mutable std::mutex m;
    mcp::ServerStatus current = make_status(true, true);
    std::vector<std::string> calls;
    mcp::ServerStatus status() const override { std::lock_guard<std::mutex> l(m); return current; }
    void set_read_only(bool ro) override { std::lock_guard<std::mutex> l(m); calls.push_back(ro ? "ro=1" : "ro=0"); current.read_only = ro; }
    void request_stop() override { std::lock_guard<std::mutex> l(m); calls.push_back("stop"); current.running = false; }
    void request_restart() override { std::lock_guard<std::mutex> l(m); calls.push_back("restart"); current.running = true; }
    void request_start() override { std::lock_guard<std::mutex> l(m); calls.push_back("start"); current.running = true; }
    std::vector<std::string> snapshot() { std::lock_guard<std::mutex> l(m); return calls; }
};

struct FakeRunner : IServerRunner {
    FakeControl ctl;
    std::mutex m;
    std::condition_variable cv;
    bool quit = false;
    std::atomic<bool> ran{false};
    int run_blocking() override {
        ran = true;
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return quit; });
        return 0;
    }
    mcp::IServerControl& control() override { return ctl; }
    void request_quit() override { { std::lock_guard<std::mutex> l(m); quit = true; } cv.notify_all(); }
};

struct FakeHost : TrayHost {
    std::mutex m;
    std::condition_variable cv;
    bool quit = false;
    bool run_ok = true;
    bool confirm_answer = true;
    std::atomic<TrayListener*> listener{nullptr};
    std::vector<std::string> balloons, boxes, confirms;
    std::atomic<int> refreshes{0};
    bool run(TrayListener& l, const std::function<void(bool)>& ready) override {
        if (!run_ok) { ready(false); return false; }
        listener = &l;
        ready(true);
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return quit; });
        return true;
    }
    void post_quit() override { { std::lock_guard<std::mutex> l(m); quit = true; } cv.notify_all(); }
    void refresh() override { ++refreshes; }
    void balloon(const std::string& title, const std::string& text) override { std::lock_guard<std::mutex> l(m); balloons.push_back(title + "|" + text); }
    bool confirm(const std::string& title, const std::string& text) override { std::lock_guard<std::mutex> l(m); confirms.push_back(title + "|" + text); return confirm_answer; }
    void message_box(const std::string& title, const std::string& text) override { std::lock_guard<std::mutex> l(m); boxes.push_back(title + "|" + text); }
};

struct FakeOpener : FileOpener {
    std::vector<std::filesystem::path> opened;
    bool ok = true;
    bool open(const std::filesystem::path& p) override { opened.push_back(p); return ok; }
};

struct FakeLauncher : ProcessLauncher {
    std::vector<LaunchRequest> requests;
    std::optional<std::uint32_t> pid = 4242;
    std::optional<std::uint32_t> launch(const LaunchRequest& r) override { requests.push_back(r); return pid; }
};

struct FakeRegistry : RegistryRunKey {
    std::map<std::string, std::string> values;
    bool ok = true;
    bool set(const std::string& n, const std::string& c) override { if (!ok) return false; values[n] = c; return true; }
    bool remove(const std::string& n) override { if (!ok) return false; values.erase(n); return true; }
    std::optional<std::string> get(const std::string& n) override { auto it = values.find(n); if (it == values.end()) return std::nullopt; return it->second; }
};

struct FakeGuard : SingleInstance {
    bool other_running = false;
    bool held = false;
    bool acquire() override { if (other_running || held) return false; held = true; return true; }
    bool exists() override { return other_running; }
};

struct Fixture {
    FakeControl control;
    FakeHost host;
    FakeOpener opener;
    FakeLauncher launcher;
    FakeRunner runner;
    TrayPaths paths;
    Fixture() {
        paths.log_file = "C:\\logs\\mcp.log";
        paths.audit_dir = "C:\\audit";
        paths.config_file = "C:\\cfg\\mcp.yaml";
        paths.exe = "C:\\Program Files\\fairyfly\\fairyfly.exe";
    }
    TrayController make(TrayState state = {}) {
        TrayServices services;
        services.host = &host;
        services.opener = &opener;
        services.launcher = &launcher;
        return TrayController(control, &runner, services, paths, state);
    }
};

} // namespace

// ================= menu model =================
TEST_CASE("tray menu: running read-only server with warnings", "[tray][menu]") {
    const auto m = build_menu(make_status(true, true, {"no tokens", "insecure"}), {});
    CHECK(top_labels(m) == std::vector<std::string>{
        "Running: http://127.0.0.1:8383/mcp (read-only)", "Warnings (2)", "---",
        "Start", "Stop", "Restart", "Read-only mode", "---",
        "Open log", "Open audit trail folder", "Open config file", "Tokens", "---", "Quit"});
    CHECK(m.icon == IconState::Warning);
    CHECK_FALSE(m.items[0].enabled);
    REQUIRE(m.items[1].children.size() == 2);
    CHECK(m.items[1].children[0].label == "no tokens");
    CHECK_FALSE(find_item(m, TrayAction::Start)->enabled);
    CHECK(find_item(m, TrayAction::Stop)->enabled);
    CHECK(find_item(m, TrayAction::Restart)->enabled);
    const auto* ro = find_item(m, TrayAction::ToggleReadOnly);
    REQUIRE(ro);
    CHECK(ro->checkable);
    CHECK(ro->checked);
    CHECK(ro->enabled);
    REQUIRE(m.items[11].children.size() == 2);
    CHECK(find_item(m, TrayAction::TokenList));
    CHECK(find_item(m, TrayAction::TokenCreate));
    CHECK(m.tooltip == "fairyfly MCP: running (read-only), 2 warning(s)");
}

TEST_CASE("tray menu: write mode, stopped and error states", "[tray][menu]") {
    auto m = build_menu(make_status(true, false), {});
    CHECK(m.icon == IconState::Running);
    CHECK(m.items[0].label == "Running: http://127.0.0.1:8383/mcp (write mode)");
    CHECK(m.items[1].label == "No warnings");
    CHECK_FALSE(find_item(m, TrayAction::ToggleReadOnly)->checked);
    CHECK(m.tooltip == "fairyfly MCP: running (write mode)");

    m = build_menu(make_status(false, true), {});
    CHECK(m.icon == IconState::Stopped);
    CHECK(m.items[0].label.rfind("Stopped:", 0) == 0);
    CHECK(find_item(m, TrayAction::Start)->enabled);
    CHECK_FALSE(find_item(m, TrayAction::Stop)->enabled);
    CHECK_FALSE(find_item(m, TrayAction::Restart)->enabled);
    CHECK(m.tooltip == "fairyfly MCP: stopped");

    TrayState failed;
    failed.last_error = "port 8383 in use";
    m = build_menu(make_status(false, true), failed);
    CHECK(m.icon == IconState::Error);
    CHECK(m.items[0].label == "Error: port 8383 in use");
    CHECK(icon_for(make_status(true, true), failed) == IconState::Error);   // error wins
}

TEST_CASE("tray status identifies owner-routed parallel session mode", "[tray][menu]") {
    auto status = make_status(true, true);
    status.parallel_sessions = true;
    const auto menu = build_menu(status, {});
    CHECK(menu.items[0].label.find("parallel SAP windows") != std::string::npos);
    CHECK(menu.tooltip.find("parallel sessions") != std::string::npos);
    CHECK(status_balloon_text(status).find("same-window calls ordered") != std::string::npos);
}

TEST_CASE("tray menu: hard read-only cap locks the toggle", "[tray][menu]") {
    TrayState locked;
    locked.hard_read_only = true;
    const auto m = build_menu(make_status(true, false), locked);
    const auto* ro = find_item(m, TrayAction::ToggleReadOnly);
    REQUIRE(ro);
    CHECK_FALSE(ro->enabled);
    CHECK(ro->checked);
    CHECK(ro->label.find("FAIRYFLY_READ_ONLY") != std::string::npos);
}

TEST_CASE("tray menu: tooltip never exceeds the shell limit and no secrets are shown", "[tray][menu]") {
    auto s = make_status(true, true, std::vector<std::string>(50, std::string(200, 'x')));
    CHECK(build_menu(s, {}).tooltip.size() <= 127);
    CHECK(status_balloon_text(make_status(true, true, {"w1"})).find("! w1") != std::string::npos);
}

// ================= builders =================
TEST_CASE("tray: command-line quoting and relaunch plan", "[tray][args]") {
    CHECK(quote_arg("abc") == "abc");
    CHECK(quote_arg("a b") == "\"a b\"");
    CHECK(quote_arg("") == "\"\"");
    CHECK(quote_arg("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(quote_arg("C:\\dir with space\\") == "\"C:\\dir with space\\\\\"");
    CHECK(join_command_line("C:\\Program Files\\f.exe", {"mcp", "--tray", "-c", "C:\\my dir\\mcp.yaml"}) ==
          "\"C:\\Program Files\\f.exe\" mcp --tray -c \"C:\\my dir\\mcp.yaml\"");

    const std::vector<std::string> args{"mcp", "--tray", "--http"};
    auto plan = plan_relaunch("C:\\f.exe", args, true, false, true);
    CHECK(plan.relaunch);
    CHECK(plan.command_line == "C:\\f.exe mcp --tray --http");
    REQUIRE(plan.extra_env.size() == 1);
    CHECK(plan.extra_env[0] == std::make_pair(std::string("FAIRYFLY_TRAY_CHILD"), std::string("1")));
    CHECK(plan.no_window);

    CHECK_FALSE(plan_relaunch("C:\\f.exe", args, false, false, true).relaunch);   // no --tray
    CHECK_FALSE(plan_relaunch("C:\\f.exe", args, true, true, true).relaunch);     // already the child
    CHECK_FALSE(plan_relaunch("C:\\f.exe", args, true, false, false).relaunch);   // no console attached
}

TEST_CASE("tray: autostart, console and log path builders", "[tray][args]") {
    CHECK(autostart_command("C:\\Program Files\\fairyfly\\fairyfly.exe", "") ==
          "\"C:\\Program Files\\fairyfly\\fairyfly.exe\" mcp --tray");
    CHECK(autostart_command("C:\\f.exe", "C:\\cfg dir\\mcp.yaml") == "\"C:\\f.exe\" mcp --tray -c \"C:\\cfg dir\\mcp.yaml\"");
    CHECK(console_command_line("C:\\Program Files\\f.exe", {"mcp", "token", "list"}) ==
          "cmd.exe /k \"\"C:\\Program Files\\f.exe\" mcp token list\"");
    CHECK(console_command_line("C:\\f.exe", {"mcp", "token", "create"}) == "cmd.exe /k \"C:\\f.exe mcp token create\"");
    const auto dir = log_directory([](const char* n) { return std::string(n) == "LOCALAPPDATA" ? "C:\\Users\\u\\AppData\\Local" : ""; });
    CHECK(dir == std::filesystem::path("C:\\Users\\u\\AppData\\Local") / "fairyfly" / "logs");
}

// ================= controller =================
TEST_CASE("tray controller: read-only toggle confirms before enabling writes", "[tray][controller]") {
    Fixture f;
    auto controller = f.make();
    f.control.current = make_status(true, true);

    f.host.confirm_answer = false;
    controller.on_action(TrayAction::ToggleReadOnly);
    REQUIRE(f.host.confirms.size() == 1);
    CHECK(f.host.confirms[0] == "fairyfly MCP|Allow the MCP server to change SAP data?");
    CHECK(f.control.snapshot().empty());   // denied: nothing changed

    f.host.confirm_answer = true;
    controller.on_action(TrayAction::ToggleReadOnly);
    CHECK(f.control.snapshot() == std::vector<std::string>{"ro=0"});

    controller.on_action(TrayAction::ToggleReadOnly);   // back to read-only: no confirmation needed
    CHECK(f.host.confirms.size() == 2);
    CHECK(f.control.snapshot() == std::vector<std::string>{"ro=0", "ro=1"});

    TrayState locked;
    locked.hard_read_only = true;
    Fixture g;
    g.control.current = make_status(true, true);
    auto hard = g.make(locked);
    hard.on_action(TrayAction::ToggleReadOnly);
    CHECK(g.control.snapshot().empty());
    CHECK(g.host.confirms.empty());
}

TEST_CASE("tray controller: start, stop, restart", "[tray][controller]") {
    Fixture f;
    auto controller = f.make();
    controller.on_action(TrayAction::Stop);
    controller.on_action(TrayAction::Start);
    controller.on_action(TrayAction::Restart);
    CHECK(f.control.snapshot() == std::vector<std::string>{"stop", "start", "restart"});
    CHECK(f.host.refreshes >= 3);
}

TEST_CASE("tray controller: open shortcuts use the file opener", "[tray][controller]") {
    Fixture f;
    auto controller = f.make();
    controller.on_action(TrayAction::OpenLog);
    controller.on_action(TrayAction::OpenAuditFolder);
    controller.on_action(TrayAction::OpenConfig);
    REQUIRE(f.opener.opened.size() == 3);
    CHECK(f.opener.opened[0] == f.paths.log_file);
    CHECK(f.opener.opened[1] == f.paths.audit_dir);
    CHECK(f.opener.opened[2] == f.paths.config_file);
    CHECK(f.host.balloons.empty());

    f.opener.ok = false;
    controller.on_action(TrayAction::OpenLog);
    REQUIRE(f.host.balloons.size() == 1);
    CHECK(f.host.balloons[0].find("Could not open log") != std::string::npos);
}

TEST_CASE("tray controller: token shortcuts open a console and never see a secret", "[tray][controller]") {
    Fixture f;
    auto controller = f.make();
    controller.on_action(TrayAction::TokenList);
    controller.on_action(TrayAction::TokenCreate);
    REQUIRE(f.launcher.requests.size() == 2);
    CHECK(f.launcher.requests[0].command_line == "cmd.exe /k \"\"C:\\Program Files\\fairyfly\\fairyfly.exe\" mcp token list\"");
    CHECK(f.launcher.requests[1].command_line.find("mcp token create") != std::string::npos);
    for (const auto& r : f.launcher.requests) {
        CHECK(r.new_console);
        CHECK_FALSE(r.detached);
    }
}

TEST_CASE("tray controller: quit stops the server, ends the runner and the UI", "[tray][controller]") {
    Fixture f;
    auto controller = f.make();
    controller.on_action(TrayAction::Quit);
    CHECK(f.control.snapshot() == std::vector<std::string>{"stop"});
    CHECK(f.runner.quit);
    CHECK(f.host.quit);
}

TEST_CASE("tray controller: startup balloon shows warnings once", "[tray][controller]") {
    Fixture f;
    f.control.current = make_status(false, true);
    auto controller = f.make();
    controller.on_tick();
    CHECK(f.host.balloons.empty());   // server not up yet

    f.control.current = make_status(true, true, {"no tokens configured"});
    controller.on_tick();
    controller.on_tick();
    REQUIRE(f.host.balloons.size() == 1);
    CHECK(f.host.balloons[0].find("no tokens configured") != std::string::npos);

    Fixture g;
    g.control.current = make_status(true, true);
    auto quiet = g.make();
    quiet.on_tick();
    REQUIRE(g.host.balloons.size() == 1);
    CHECK(g.host.balloons[0].find("Started:") != std::string::npos);

    TrayState no_notice;
    no_notice.start_notice = false;
    Fixture h;
    auto silent = h.make(no_notice);
    silent.on_tick();
    CHECK(h.host.balloons.empty());
}

TEST_CASE("tray controller: left click text and view", "[tray][controller]") {
    Fixture f;
    f.control.current = make_status(true, false, {"w"});
    auto controller = f.make();
    CHECK(controller.left_click_text().find("write mode") != std::string::npos);
    CHECK(controller.view().icon == IconState::Warning);
    CHECK(controller.menu().items.size() == 14);
}

// ================= entry decisions =================
TEST_CASE("tray entry: autostart install and remove", "[tray][autostart]") {
    FakeRegistry registry;
    TrayEntryServices services;
    services.registry = &registry;
    std::vector<std::string> printed;
    services.print = [&](const std::string& s) { printed.push_back(s); };

    TrayEntry entry;
    entry.tray_requested = true;
    entry.install_autostart = true;
    entry.exe = "C:\\Program Files\\fairyfly\\fairyfly.exe";
    entry.config_path = "C:\\cfg\\mcp.yaml";
    auto r = decide_tray_entry(entry, services);
    CHECK(r.outcome == TrayEntryOutcome::AutostartDone);
    CHECK(r.exit_code == 0);
    CHECK(registry.values.at("fairyfly-mcp") == "\"C:\\Program Files\\fairyfly\\fairyfly.exe\" mcp --tray -c \"C:\\cfg\\mcp.yaml\"");
    REQUIRE(printed.size() == 1);
    CHECK(printed[0].find("logged on") != std::string::npos);

    entry.install_autostart = false;
    entry.remove_autostart = true;
    r = decide_tray_entry(entry, services);
    CHECK(r.outcome == TrayEntryOutcome::AutostartDone);
    CHECK(registry.values.empty());

    entry.install_autostart = true;   // both flags
    r = decide_tray_entry(entry, services);
    CHECK(r.outcome == TrayEntryOutcome::Failed);
    CHECK(r.exit_code != 0);

    registry.ok = false;
    entry.remove_autostart = false;
    r = decide_tray_entry(entry, services);
    CHECK(r.outcome == TrayEntryOutcome::Failed);
    CHECK(r.message.find("TRAY_AUTOSTART_FAILED") != std::string::npos);
}

TEST_CASE("tray entry: relaunch, child and single-instance decisions", "[tray][entry]") {
    FakeLauncher launcher;
    FakeGuard guard;
    TrayEntryServices services;
    services.launcher = &launcher;
    services.guard = &guard;
    std::vector<std::string> printed;
    services.print = [&](const std::string& s) { printed.push_back(s); };

    TrayEntry entry;
    entry.exe = "C:\\f.exe";
    entry.args = {"mcp", "--tray"};
    entry.has_console = true;

    CHECK(decide_tray_entry(entry, services).outcome == TrayEntryOutcome::NotTray);

    entry.tray_requested = true;
    auto r = decide_tray_entry(entry, services);
    CHECK(r.outcome == TrayEntryOutcome::Relaunched);
    CHECK(r.exit_code == 0);
    REQUIRE(launcher.requests.size() == 1);
    CHECK(launcher.requests[0].command_line == "C:\\f.exe mcp --tray");
    CHECK(launcher.requests[0].detached);
    REQUIRE(launcher.requests[0].extra_env.size() == 1);
    CHECK(launcher.requests[0].extra_env[0].first == "FAIRYFLY_TRAY_CHILD");
    REQUIRE(printed.size() == 1);
    CHECK(printed[0].find("4242") != std::string::npos);   // the child pid is printed

    launcher.pid = std::nullopt;
    r = decide_tray_entry(entry, services);
    CHECK(r.outcome == TrayEntryOutcome::Failed);
    CHECK(r.message.find("TRAY_RELAUNCH_FAILED") != std::string::npos);
    launcher.pid = 4242;

    guard.other_running = true;
    launcher.requests.clear();
    r = decide_tray_entry(entry, services);
    CHECK(r.outcome == TrayEntryOutcome::AlreadyRunning);
    CHECK(r.exit_code == 0);
    CHECK(launcher.requests.empty());
    guard.other_running = false;

    entry.is_child = true;   // the marked child never relaunches
    r = decide_tray_entry(entry, services);
    CHECK(r.outcome == TrayEntryOutcome::RunHere);
    CHECK(launcher.requests.empty());

    entry.is_child = false;
    entry.has_console = false;   // no console (e.g. started by a GUI launcher): become the tray directly
    CHECK(decide_tray_entry(entry, services).outcome == TrayEntryOutcome::RunHere);
}

// ================= run_with_tray =================
namespace {
/// Drives the fake UI from a second thread once the host is up.
template <typename Fn>
std::thread drive(FakeHost& host, Fn fn) {
    return std::thread([&host, fn] {
        for (int i = 0; i < 500 && !host.listener; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (host.listener) fn(*host.listener.load());
    });
}
} // namespace

TEST_CASE("run_with_tray: server runs on the calling thread, tray Quit ends it", "[tray][run]") {
    Fixture f;
    FakeGuard guard;
    TrayServices services;
    services.host = &f.host;
    services.opener = &f.opener;
    services.launcher = &f.launcher;
    const auto main_thread = std::this_thread::get_id();
    struct Probe : FakeRunner {
        std::thread::id ran_on;
        int run_blocking() override { ran_on = std::this_thread::get_id(); return FakeRunner::run_blocking(); }
    } runner;

    auto driver = drive(f.host, [&](TrayListener& l) {
        l.on_action(TrayAction::Restart);
        l.on_action(TrayAction::Quit);
    });
    const int code = run_with_tray(runner, services, guard, f.paths);
    driver.join();
    CHECK(code == 0);
    CHECK(runner.ran_on == main_thread);   // COM/STA executor stays on the main thread
    CHECK(runner.ctl.snapshot() == std::vector<std::string>{"restart", "stop"});
    CHECK(guard.held);
}

TEST_CASE("run_with_tray: a second instance shows a message and exits 0", "[tray][run]") {
    Fixture f;
    FakeGuard guard;
    guard.other_running = true;
    TrayServices services;
    services.host = &f.host;
    const int code = run_with_tray(f.runner, services, guard, f.paths);
    CHECK(code == 0);
    CHECK_FALSE(f.runner.ran);
    REQUIRE(f.host.boxes.size() == 1);
    CHECK(f.host.boxes[0].find("already running") != std::string::npos);
}

TEST_CASE("run_with_tray: UI creation failure does not start the server", "[tray][run]") {
    Fixture f;
    FakeGuard guard;
    f.host.run_ok = false;
    TrayServices services;
    services.host = &f.host;
    const int code = run_with_tray(f.runner, services, guard, f.paths);
    CHECK(code == 1);
    CHECK_FALSE(f.runner.ran);
}

TEST_CASE("tray: runner factory registration point", "[tray]") {
    CHECK_FALSE(static_cast<bool>(runner_factory()));   // nothing registered in the unit-test binary
}
