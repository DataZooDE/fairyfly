// Tray tests: pure model, builders, controller and entry decisions with FAKES only. No tray icon,
// window, registry key, mutex or process is ever created here.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "include/link/link_runner.h"
#include "include/tray/mcp_tray.h"
#include "include/tray/tray_contract.h"

#include <windows.h>
#include <objbase.h>
#include "include/version.h"

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

struct FakeLink : LinkControl {
    LinkView current;
    std::vector<bool> paused_calls;
    bool ok = true;
    LinkView view() override { return current; }
    bool set_paused(bool p) override {
        paused_calls.push_back(p);
        if (ok) current.paused = p;
        return ok;
    }
};

LinkView paired(const std::string& state, bool paused = false) {
    LinkView v;
    v.paired = true;
    v.organisation = "Acme Purchasing";
    v.account = "pat@acme.example";
    v.state = state;
    v.paused = paused;
    return v;
}

std::vector<std::string> link_labels(const TrayMenuModel& m) {
    for (const auto& i : m.items)
        if (i.label == "Cloud link") {
            std::vector<std::string> out;
            for (const auto& c : i.children) out.push_back(c.label + (c.enabled ? "" : " [off]"));
            return out;
        }
    return {};
}

struct Fixture {
    FakeLink link;
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
        services.link = &link;
        return TrayController(control, &runner, services, paths, state);
    }
};

} // namespace

// ================= menu model =================
// the running version, shown (disabled) above "Check for updates" in both trays
const std::string kVersionItem = std::string("fairyfly ") + FAIRYFLY_VERSION;

TEST_CASE("tray menu: running read-only server with warnings", "[tray][menu]") {
    const auto m = build_menu(make_status(true, true, {"no tokens", "insecure"}), {});
    CHECK(top_labels(m) == std::vector<std::string>{
        "Running: http://127.0.0.1:8383/mcp (read-only)", "Warnings (2)", "---",
        "Start", "Stop", "Restart", "Read-only mode", "---",
        "Open log", "Open audit trail folder", "Open config file", "Tokens", "Cloud link", kVersionItem, "Check for updates (console)", "---", "Quit"});
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
    const auto dir = log_directory([](const char* n) { return std::string(n) == "USERPROFILE" ? "C:\\Users\\u" : ""; });
    CHECK(dir == std::filesystem::path("C:\\Users\\u") / ".fairyfly" / "logs");
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
    CHECK(controller.menu().items.size() == 17);  // with the version line
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

// ================= cloud link (D10) =================
TEST_CASE("tray menu: cloud link submenu for each link state", "[tray][menu][link]") {
    TrayState st;
    CHECK(link_labels(build_menu(make_status(true, true), st)) ==
          std::vector<std::string>{"Not connected to the fairyfly cloud [off]", "Connect to cloud... (pair in a console)"});
    st.link = paired("connected");
    CHECK(link_labels(build_menu(make_status(true, true), st)) ==
          std::vector<std::string>{"Acme Purchasing (pat@acme.example): connected [off]", "Start link [off]", "Pause",
                                   "Health check (console)", "Pair again (console)"});
    st.link = paired("paused", true);
    CHECK(link_labels(build_menu(make_status(true, true), st)) ==
          std::vector<std::string>{"Acme Purchasing (pat@acme.example): paused [off]", "Start link [off]", "Resume",
                                   "Health check (console)", "Pair again (console)"});
    st.link = paired("not running");
    auto m = build_menu(make_status(true, true), st);
    CHECK(link_labels(m)[0] == "Acme Purchasing (pat@acme.example): not running [off]");
    CHECK(find_item(m, TrayAction::LinkStart)->enabled);
    // paused but not running: starting is allowed (it stays disconnected until resumed)
    st.link = paired("not running", true);
    CHECK(find_item(build_menu(make_status(true, true), st), TrayAction::LinkStart)->enabled);
    CHECK(find_item(build_menu(make_status(true, true), st), TrayAction::LinkResume));
}

TEST_CASE("tray controller: cloud link actions", "[tray][controller][link]") {
    Fixture f;
    auto controller = f.make();
    // the tick reads the link's view (pairing state + the running link's status file)
    f.link.current = paired("connected");
    controller.on_tick();
    CHECK(controller.state().link.paired);
    CHECK(link_labels(controller.menu())[0] == "Acme Purchasing (pat@acme.example): connected [off]");

    controller.on_action(TrayAction::LinkPause);
    REQUIRE(f.link.paused_calls == std::vector<bool>{true});
    CHECK(controller.state().link.paused);
    controller.on_action(TrayAction::LinkResume);
    CHECK(f.link.paused_calls == std::vector<bool>{true, false});

    f.link.ok = false;
    controller.on_action(TrayAction::LinkPause);
    CHECK(f.host.balloons.back().find("Could not pause") != std::string::npos);

    controller.on_action(TrayAction::LinkStart);  // the link tray, detached: `fairyfly link run --tray`, no console
    REQUIRE(f.launcher.requests.size() == 1);
    CHECK(f.launcher.requests[0].command_line == "\"C:\\Program Files\\fairyfly\\fairyfly.exe\" link run --tray");
    CHECK(f.launcher.requests[0].detached);
    CHECK_FALSE(f.launcher.requests[0].new_console);

    controller.on_action(TrayAction::LinkPair);  // pairing needs the person at the desktop: a console
    controller.on_action(TrayAction::LinkStatus);
    REQUIRE(f.launcher.requests.size() == 3);
    CHECK(f.launcher.requests[1].command_line.find("link pair") != std::string::npos);
    CHECK(f.launcher.requests[1].new_console);
    CHECK(f.launcher.requests[2].command_line.find("link status") != std::string::npos);

    f.launcher.pid = std::nullopt;
    controller.on_action(TrayAction::LinkStart);
    CHECK(f.host.balloons.back().find("Could not start the cloud link") != std::string::npos);
}

TEST_CASE("tray controller: no link control means the submenu only offers pairing", "[tray][controller][link]") {
    Fixture f;
    TrayServices services;
    services.host = &f.host;
    services.launcher = &f.launcher;
    TrayController controller(f.control, &f.runner, services, f.paths);
    controller.on_tick();
    controller.on_action(TrayAction::LinkPause);  // nothing to pause: no crash, a notice
    CHECK_FALSE(f.host.balloons.empty());
    CHECK(link_labels(controller.menu()).size() == 2);
}

TEST_CASE("tray controller: update check opens a console; an available update is announced once", "[tray][controller][update]") {
    Fixture f;
    auto controller = f.make();
    controller.on_action(TrayAction::UpdateCheck);
    REQUIRE(f.launcher.requests.size() == 1);
    CHECK(f.launcher.requests[0].command_line.find("update check") != std::string::npos);
    CHECK(f.launcher.requests[0].new_console);
    const auto before = f.host.balloons.size();
    controller.announce_update("2026.11.02");  // from the background check (any thread)
    controller.announce_update("2026.11.02");
    REQUIRE(f.host.balloons.size() == before + 1);
    CHECK(f.host.balloons.back().find("fairyfly 2026.11.02 is available") != std::string::npos);
    CHECK(f.host.balloons.back().find("fairyfly update apply") != std::string::npos);
}

TEST_CASE("run_with_tray: the background update check announces a newer release; quitting never waits for it", "[tray][run][update]") {
    Fixture f;
    FakeGuard guard;
    TrayServices services;
    services.host = &f.host;
    services.launcher = &f.launcher;
    services.latest_update = [] { return std::optional<std::string>("2026.11.02"); };
    services.update_check_delay = std::chrono::milliseconds(0);
    auto driver = drive(f.host, [&](TrayListener&) {
        for (int i = 0; i < 300; i++) {
            {
                std::lock_guard<std::mutex> l(f.host.m);
                for (const auto& b : f.host.balloons)
                    if (b.find("2026.11.02 is available") != std::string::npos) goto done;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    done:
        f.host.listener.load()->on_action(TrayAction::Quit);
    });
    FakeRunner runner;
    CHECK(run_with_tray(runner, services, guard, f.paths) == 0);
    driver.join();
    bool seen = false;
    for (const auto& b : f.host.balloons) seen = seen || b.find("2026.11.02 is available") != std::string::npos;
    CHECK(seen);

    // a check that hangs (no network) does not hold up Quit
    Fixture g;
    FakeGuard guard2;
    TrayServices slow = services;
    slow.host = &g.host;
    std::atomic<bool> release{false};
    slow.latest_update = [&release] {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return std::optional<std::string>("2026.11.03");
    };
    auto driver2 = drive(g.host, [&](TrayListener& l) { l.on_action(TrayAction::Quit); });
    FakeRunner runner2;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(run_with_tray(runner2, slow, guard2, g.paths) == 0);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    driver2.join();
    release = true;  // the late answer goes nowhere (the controller is gone)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ================= link tray (`link run --tray`) =================
TEST_CASE("link tray: autostart command keeps the link options, drops the autostart flags", "[tray][link_tray]") {
    CHECK(link_autostart_command("C:\\Program Files\\fairyfly\\fairyfly.exe",
                                 {"link", "run", "--tray", "--allow-write", "--install-autostart"}) ==
          "\"C:\\Program Files\\fairyfly\\fairyfly.exe\" link run --tray --allow-write");
    CHECK(link_autostart_command("C:\\f.exe", {"link", "run", "--install-autostart", "--tools", "screen,session"}) ==
          "C:\\f.exe link run --tools screen,session --tray");
}

TEST_CASE("link tray: entry registers its own Run value and names itself", "[tray][link_tray]") {
    FakeRegistry registry;
    FakeGuard guard;
    TrayEntryServices services;
    services.registry = &registry;
    services.guard = &guard;
    std::vector<std::string> printed;
    services.print = [&](const std::string& s) { printed.push_back(s); };

    TrayEntry entry;
    entry.tray_requested = true;
    entry.install_autostart = true;
    entry.exe = "C:\\f.exe";
    entry.autostart_name = kLinkAutostartValueName;
    entry.autostart_line = "C:\\f.exe link run --tray";
    entry.name = "fairyfly link tray";
    CHECK(decide_tray_entry(entry, services).outcome == TrayEntryOutcome::AutostartDone);
    CHECK(registry.values.at("fairyfly-link") == "C:\\f.exe link run --tray");
    CHECK(registry.values.count("fairyfly-mcp") == 0);   // the MCP tray's autostart is a separate value

    entry.install_autostart = false;
    entry.has_console = true;
    guard.other_running = true;
    CHECK(decide_tray_entry(entry, services).outcome == TrayEntryOutcome::AlreadyRunning);
    CHECK(printed.back() == "fairyfly link tray is already running.");
}

TEST_CASE("link tray menu: no MCP server items, state per link", "[tray][link_tray][menu]") {
    auto m = build_link_menu(paired("connected"));
    CHECK(m.icon == IconState::Running);
    CHECK(top_labels(m) == std::vector<std::string>{"Acme Purchasing (pat@acme.example): connected", "---", "Pause",
                                                    "Health check (console)", "Pair again (console)", "---", "Open log",
                                                    "Open audit trail folder", kVersionItem, "Check for updates (console)", "---", "Quit"});
    CHECK(m.tooltip == "fairyfly link: connected - Acme Purchasing (pat@acme.example)");
    CHECK(find_item(m, TrayAction::Start) == nullptr);
    CHECK(find_item(m, TrayAction::ToggleReadOnly) == nullptr);

    m = build_link_menu(paired("paused", true));
    CHECK(m.icon == IconState::Warning);
    CHECK(find_item(m, TrayAction::LinkResume) != nullptr);
    CHECK(find_item(m, TrayAction::LinkPause) == nullptr);

    CHECK(build_link_menu(paired("not running")).icon == IconState::Stopped);
    m = build_link_menu(LinkView{});
    CHECK(top_labels(m)[0] == "Not paired with the fairyfly cloud yet");
    CHECK(find_item(m, TrayAction::LinkPause) == nullptr);
}

TEST_CASE("link tray menu: an unpaired desktop is led to pairing", "[tray][link_tray][menu]") {
    const auto m = build_link_menu(LinkView{});
    const MenuItem* pair = find_item(m, TrayAction::LinkPair);
    REQUIRE(pair != nullptr);
    CHECK(pair->enabled);
    CHECK(pair->label == "Pair with the fairyfly cloud... (console)");
    CHECK(top_labels(m)[2] == pair->label);   // the first action under the status line
    CHECK(m.tooltip == "fairyfly link: not paired - click to pair");
    CHECK(m.icon == IconState::Stopped);
}

TEST_CASE("link tray menu: Start local MCP server, checked and disabled while it runs", "[tray][link_tray][menu]") {
    CHECK(find_item(build_link_menu(paired("connected")), TrayAction::StartMcpServer) == nullptr);   // nothing to start it with
    McpTrayView mcp;
    mcp.available = true;
    auto m = build_link_menu(paired("connected"), {}, mcp);
    const MenuItem* start = find_item(m, TrayAction::StartMcpServer);
    REQUIRE(start != nullptr);
    CHECK(start->label == "Start local MCP server");
    CHECK(start->enabled);
    CHECK_FALSE(start->checked);
    CHECK(top_labels(m) == std::vector<std::string>{"Acme Purchasing (pat@acme.example): connected", "---", "Pause",
                                                    "Health check (console)", "Pair again (console)", "---",
                                                    "Start local MCP server", "---", "Open log", "Open audit trail folder",
                                                    kVersionItem, "Check for updates (console)", "---", "Quit"});
    mcp.running = true;
    const auto running_menu = build_link_menu(paired("connected"), {}, mcp);   // find_item points into it
    start = find_item(running_menu, TrayAction::StartMcpServer);
    REQUIRE(start != nullptr);
    CHECK(start->label == "Local MCP server running");
    CHECK_FALSE(start->enabled);
    CHECK(start->checkable);
    CHECK(start->checked);
    CHECK(find_item(build_link_menu(LinkView{}, {}, mcp), TrayAction::StartMcpServer) != nullptr);   // also unpaired
    CHECK(mcp_tray_args() == std::vector<std::string>{"mcp", "--tray", "--http"});
}

TEST_CASE("link tray controller: Start local MCP server launches `mcp --tray --http` once", "[tray][link_tray][controller]") {
    Fixture f;
    FakeGuard mcp_guard;
    LinkTrayServices services;
    services.host = &f.host;
    services.launcher = &f.launcher;
    services.link = &f.link;
    services.mcp_tray = &mcp_guard;
    LinkTrayController controller(services, f.paths);

    const MenuItem* start = find_item(controller.menu(), TrayAction::StartMcpServer);
    REQUIRE(start != nullptr);
    CHECK(start->enabled);
    controller.on_action(TrayAction::StartMcpServer);
    REQUIRE(f.launcher.requests.size() == 1);
    CHECK(f.launcher.requests[0].command_line == "\"C:\\Program Files\\fairyfly\\fairyfly.exe\" mcp --tray --http");
    CHECK(f.launcher.requests[0].detached);   // its own tray, no console
    CHECK_FALSE(f.launcher.requests[0].new_console);
    CHECK(f.host.balloons.back().find("local MCP server") != std::string::npos);
    CHECK_FALSE(mcp_guard.held);   // the link tray only probes the MCP tray's guard

    mcp_guard.other_running = true;   // the MCP tray holds its guard now
    const auto menu = controller.menu();
    start = find_item(menu, TrayAction::StartMcpServer);
    REQUIRE(start != nullptr);
    CHECK_FALSE(start->enabled);
    CHECK(start->checked);
    controller.on_action(TrayAction::StartMcpServer);   // a stale menu click: not a second server
    CHECK(f.launcher.requests.size() == 1);
    CHECK(f.host.balloons.back().find("already running") != std::string::npos);

    mcp_guard.other_running = false;
    f.launcher.pid = std::nullopt;
    controller.on_action(TrayAction::StartMcpServer);
    CHECK(f.host.balloons.back() == "fairyfly link|Could not start the local MCP server");
}

TEST_CASE("tray startup errors reach the person: console only when it stays open", "[tray][startup_error]") {
    CHECK_FALSE(startup_error_needs_dialog(false, true, 2));   // a shell shares the console: it reads the error there
    CHECK(startup_error_needs_dialog(false, true, 1));         // Start menu / autostart: the console closes with us
    CHECK(startup_error_needs_dialog(false, false, 0));        // started detached (the link tray's menu)
    CHECK(startup_error_needs_dialog(true, false, 0));         // the detached tray child
    CHECK(startup_error_needs_dialog(true, true, 3));          // the child never owns a console a person reads

    FakeHost host;
    std::vector<std::string> printed;
    auto print = [&](const std::string& s) { printed.push_back(s); };
    CHECK(report_startup_error("fairyfly MCP", "TRAY_REQUIRES_HTTP", "The tray needs the HTTP transport", 2, true, &host,
                               print) == 2);
    REQUIRE(printed.size() == 1);
    CHECK(printed[0] == "error TRAY_REQUIRES_HTTP: The tray needs the HTTP transport");
    REQUIRE(host.boxes.size() == 1);
    CHECK(host.boxes[0] == "fairyfly MCP|TRAY_REQUIRES_HTTP: The tray needs the HTTP transport");

    CHECK(report_startup_error("fairyfly link", "X", "y", 1, false, &host, print) == 1);
    CHECK(printed.size() == 2);
    CHECK(host.boxes.size() == 1);   // in a shell's console: no box
    CHECK(report_startup_error("fairyfly link", "X", "y", 1, true, nullptr, print) == 1);   // no host: printed only
    CHECK(printed.size() == 3);
}

TEST_CASE("link tray controller: notices on state changes, pause, consoles, quit", "[tray][link_tray][controller]") {
    Fixture f;
    int stops = 0;
    LinkTrayServices services;
    services.host = &f.host;
    services.opener = &f.opener;
    services.launcher = &f.launcher;
    services.link = &f.link;
    services.stop_link = [&] { ++stops; };
    LinkTrayController controller(services, f.paths);

    controller.on_tick();   // no status yet: nothing to say
    CHECK(f.host.balloons.empty());
    f.link.current = paired("connected");
    controller.on_tick();
    controller.on_tick();
    REQUIRE(f.host.balloons.size() == 1);
    CHECK(f.host.balloons[0] == "fairyfly link|Cloud link running for Acme Purchasing (pat@acme.example)");

    controller.on_action(TrayAction::LinkPause);
    CHECK(f.link.paused_calls == std::vector<bool>{true});
    f.link.current = paired("paused", true);
    controller.on_tick();
    CHECK(f.host.balloons.back().find("Cloud link paused") != std::string::npos);
    f.link.current = paired("connected");
    controller.on_tick();
    CHECK(f.host.balloons.back() == "fairyfly link|Cloud link resumed");

    controller.on_action(TrayAction::LinkStatus);
    controller.on_action(TrayAction::UpdateCheck);
    REQUIRE(f.launcher.requests.size() == 2);
    CHECK(f.launcher.requests[0].command_line.find("link status") != std::string::npos);
    CHECK(f.launcher.requests[0].new_console);
    CHECK(f.launcher.requests[1].command_line.find("update check") != std::string::npos);

    controller.on_action(TrayAction::OpenLog);
    CHECK(f.opener.opened.back() == f.paths.log_file);

    controller.on_action(TrayAction::Quit);
    CHECK(stops == 1);
    CHECK(controller.quit_requested());
    const auto before = f.host.balloons.size();
    f.link.current = paired("not running");
    controller.on_tick();   // stopping after Quit is expected: no notice
    CHECK(f.host.balloons.size() == before);
}

TEST_CASE("run_link_with_tray: the link runs on the calling thread, Quit stops it", "[tray][link_tray][run]") {
    Fixture f;
    FakeGuard guard;
    f.link.current = paired("connected");
    std::atomic<bool> stop{false};
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    services.stop_link = [&] { stop = true; };
    const auto main_thread = std::this_thread::get_id();
    std::thread::id ran_on;
    std::atomic<bool> started{false};
    // Quit only once the link runs: a Quit posted before it starts ends the tray without running it (flaky 1 in 6)
    auto driver = drive(f.host, [&](TrayListener& l) {
        for (int i = 0; i < 500 && !started; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        l.on_action(TrayAction::Quit);
    });
    const int code = run_link_with_tray(
        [&] {
            ran_on = std::this_thread::get_id();
            started = true;
            for (int i = 0; i < 500 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return stop ? 0 : 9;
        },
        services, guard, f.paths);
    driver.join();
    CHECK(code == 0);
    CHECK(ran_on == main_thread);   // `link run` keeps the main (COM) thread
    CHECK(guard.held);
    CHECK(f.host.boxes.empty());
}

namespace {
/// A link whose pairing state another thread changes (the console's `link pair`).
struct SharedLink : LinkControl {
    std::mutex m;
    LinkView current;
    LinkView view() override { std::lock_guard<std::mutex> l(m); return current; }
    bool set_paused(bool) override { return true; }
    void set(const LinkView& v) { std::lock_guard<std::mutex> l(m); current = v; }
};
} // namespace

TEST_CASE("run_link_with_tray: unpaired, the tray stays up, leads to pairing and starts the link once paired", "[tray][link_tray][run]") {
    Fixture f;
    FakeGuard guard;
    SharedLink link;
    LinkTrayServices services;
    services.host = &f.host;
    services.launcher = &f.launcher;
    services.link = &link;
    services.pairing_poll = std::chrono::milliseconds(5);
    std::atomic<bool> stop{false};
    services.stop_link = [&] { stop = true; };
    std::atomic<int> runs{0};
    int runs_before_pairing = -1;
    bool pair_offered = false;   // asserted on the test thread (Catch2 assertions are not thread-safe)
    auto driver = drive(f.host, [&](TrayListener& l) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        runs_before_pairing = runs;   // not paired: `link run` would only fail
        const auto menu = l.menu();
        const MenuItem* pair = find_item(menu, TrayAction::LinkPair);
        pair_offered = pair != nullptr && pair->enabled;
        l.on_action(TrayAction::LinkPair);   // `link pair` in a console
        link.set(paired("not running"));     // ... and the person paired
        for (int i = 0; i < 500 && runs == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        l.on_action(TrayAction::Quit);
    });
    const int code = run_link_with_tray(
        [&] {
            ++runs;
            for (int i = 0; i < 500 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return 0;
        },
        services, guard, f.paths);
    driver.join();
    CHECK(code == 0);
    CHECK(runs_before_pairing == 0);
    CHECK(pair_offered);
    CHECK(runs == 1);
    REQUIRE_FALSE(f.host.balloons.empty());
    CHECK(f.host.balloons[0].find("not paired") != std::string::npos);
    CHECK(f.host.balloons[0].find("Pair with the fairyfly cloud") != std::string::npos);
    REQUIRE(f.launcher.requests.size() == 1);
    CHECK(f.launcher.requests[0].command_line.find("link pair") != std::string::npos);
    CHECK(f.launcher.requests[0].new_console);
    CHECK(f.host.boxes.empty());
}

TEST_CASE("run_link_with_tray: while a console link serves the pairing the tray waits, then starts its own", "[tray][link_tray][run]") {
    Fixture f;
    FakeGuard guard;
    f.link.current = paired("not running");
    std::atomic<bool> busy{true};
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    services.pairing_poll = std::chrono::milliseconds(5);
    services.link_busy = [&] { return busy.load(); };
    std::atomic<bool> stop{false};
    services.stop_link = [&] { stop = true; };
    std::atomic<int> runs{0};
    int runs_while_busy = -1;
    auto driver = drive(f.host, [&](TrayListener& l) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        runs_while_busy = runs;   // refused anyway (LINK_ALREADY_RUNNING): not started
        busy = false;             // the console link ended
        for (int i = 0; i < 500 && runs == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        l.on_action(TrayAction::Quit);
    });
    const int code = run_link_with_tray(
        [&] {
            ++runs;
            for (int i = 0; i < 500 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return 0;
        },
        services, guard, f.paths);
    driver.join();
    CHECK(code == 0);
    CHECK(runs_while_busy == 0);
    CHECK(runs == 1);
    REQUIRE_FALSE(f.host.balloons.empty());
    CHECK(f.host.balloons[0].find("already running in a console") != std::string::npos);
    CHECK(f.host.boxes.empty());
}

TEST_CASE("run_link_with_tray: a console link that claims the pairing after the busy check makes the tray wait, not fail",
          "[tray][link_tray][run]") {
    Fixture f;
    FakeGuard guard;
    f.link.current = paired("not running");
    std::atomic<bool> busy{false};   // free when the tray looks ...
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    services.pairing_poll = std::chrono::milliseconds(5);
    services.link_busy = [&] { return busy.load(); };
    std::atomic<int> runs{0};
    int runs_while_busy = -1;
    // the console link ends a little later; the tray itself is never touched (no Quit needed)
    std::thread console([&] {
        for (int i = 0; i < 500 && runs == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        runs_while_busy = runs;
        busy = false;
    });
    const int code = run_link_with_tray(
        [&] {
            if (++runs == 1) {   // ... but a console `link run` claims it first: claim_link_run refuses ours
                busy = true;
                return kLinkAlreadyRunningExitCode;
            }
            return 0;   // the tray's own link, after the console's ended
        },
        services, guard, f.paths);
    console.join();
    CHECK(code == 0);
    CHECK(runs_while_busy == 1);   // not retried while the console link ran
    CHECK(runs == 2);
    CHECK(f.host.boxes.empty());   // no "cloud link stopped (exit code ...)"
    REQUIRE_FALSE(f.host.balloons.empty());
    CHECK(f.host.balloons[0].find("already running in a console") != std::string::npos);
}

TEST_CASE("run_link_with_tray: Quit while unpaired ends the tray without starting the link", "[tray][link_tray][run]") {
    Fixture f;
    FakeGuard guard;
    SharedLink link;
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &link;
    services.pairing_poll = std::chrono::milliseconds(5);
    auto driver = drive(f.host, [&](TrayListener& l) { l.on_action(TrayAction::Quit); });
    bool ran = false;
    const int code = run_link_with_tray([&] { ran = true; return 2; }, services, guard, f.paths);
    driver.join();
    CHECK(code == 0);
    CHECK_FALSE(ran);
    CHECK(f.host.boxes.empty());
}

TEST_CASE("run_link_with_tray and run_with_tray: a tray that cannot start says so", "[tray][run][startup_error]") {
    {
        Fixture f;
        FakeGuard guard;
        f.host.run_ok = false;
        LinkTrayServices services;
        services.host = &f.host;
        bool ran = false;
        CHECK(run_link_with_tray([&] { ran = true; return 0; }, services, guard, f.paths) == 1);
        CHECK_FALSE(ran);
        REQUIRE(f.host.boxes.size() == 1);
        CHECK(f.host.boxes[0].find("tray") != std::string::npos);
    }
    {
        Fixture f;
        FakeGuard guard;
        f.host.run_ok = false;
        TrayServices services;
        services.host = &f.host;
        CHECK(run_with_tray(f.runner, services, guard, f.paths) == 1);
        REQUIRE(f.host.boxes.size() == 1);
        CHECK(f.host.boxes[0].find("tray") != std::string::npos);
    }
}

TEST_CASE("run_link_with_tray: a failing link shows a message; a second instance only a notice", "[tray][link_tray][run]") {
    {
        Fixture f;
        FakeGuard guard;
        LinkTrayServices services;
        services.host = &f.host;
        CHECK(run_link_with_tray([] { return 2; }, services, guard, f.paths) == 2);
        REQUIRE(f.host.boxes.size() == 1);
        CHECK(f.host.boxes[0].find("The cloud link stopped (exit code 2)") != std::string::npos);
        CHECK(f.host.boxes[0].find("mcp.log") != std::string::npos);   // the log path of the fixture
    }
    {
        Fixture f;
        FakeGuard guard;
        guard.other_running = true;
        LinkTrayServices services;
        services.host = &f.host;
        bool ran = false;
        CHECK(run_link_with_tray([&] { ran = true; return 0; }, services, guard, f.paths) == 0);
        CHECK_FALSE(ran);
        REQUIRE(f.host.boxes.size() == 1);
        CHECK(f.host.boxes[0].find("already running") != std::string::npos);
    }
}

namespace {
struct FakeWrites : LinkWriteMode {
    bool is_allowed = false, is_locked = false, is_enforced = false, ok = true;
    std::vector<bool> requests;
    bool allowed() override { return is_allowed; }
    bool locked() override { return is_locked; }
    bool enforced() override { return is_enforced; }
    bool set_allowed(bool a) override {
        requests.push_back(a);
        if (ok) is_allowed = a;
        return ok;
    }
};
} // namespace

TEST_CASE("link tray: Allow changes in SAP is the standing approval: asks first, greyed when enforced or read-only", "[tray][link_tray]") {
    LinkWritesView writes;
    writes.available = true;
    auto m = build_link_menu(paired("connected"), writes);
    const MenuItem* allow = find_item(m, TrayAction::LinkToggleWrites);
    REQUIRE(allow != nullptr);
    CHECK(allow->checkable);
    CHECK(allow->enabled);
    CHECK_FALSE(allow->checked);
    CHECK(m.tooltip.find("(changes need your approval)") != std::string::npos);   // off: every change asks
    writes.allowed = true;
    m = build_link_menu(paired("connected"), writes);
    CHECK(find_item(m, TrayAction::LinkToggleWrites)->checked);
    CHECK(m.tooltip.find("(changes allowed)") != std::string::npos);
    writes.enforced = true;   // the organisation: "Always ask at the desktop"
    m = build_link_menu(paired("connected"), writes);
    allow = find_item(m, TrayAction::LinkToggleWrites);
    CHECK_FALSE(allow->enabled);
    CHECK_FALSE(allow->checked);
    CHECK(allow->label.find("Approval is required by your administrator") != std::string::npos);
    CHECK(m.tooltip.find("(changes need your approval)") != std::string::npos);
    writes.enforced = false;
    writes.locked = true;     // --read-only / FAIRYFLY_READ_ONLY
    m = build_link_menu(paired("connected"), writes);
    allow = find_item(m, TrayAction::LinkToggleWrites);
    CHECK_FALSE(allow->enabled);
    CHECK_FALSE(allow->checked);
    CHECK(m.tooltip.find("(read-only)") != std::string::npos);
    CHECK(find_item(build_link_menu(paired("connected")), TrayAction::LinkToggleWrites) == nullptr);   // no switch: no item

    Fixture f;
    FakeWrites fake;
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    services.writes = &fake;
    LinkTrayController controller(services, f.paths);

    f.host.confirm_answer = false;   // turning writes on asks first
    controller.on_action(TrayAction::LinkToggleWrites);
    CHECK(fake.requests.empty());
    REQUIRE(f.host.confirms.size() == 1);
    CHECK(f.host.confirms[0].find("without asking") != std::string::npos);

    f.host.confirm_answer = true;
    controller.on_action(TrayAction::LinkToggleWrites);
    CHECK(fake.requests == std::vector<bool>{true});
    CHECK(f.host.balloons.back().find("Changes in SAP allowed") != std::string::npos);

    controller.on_action(TrayAction::LinkToggleWrites);   // back to asking: no question
    CHECK(fake.requests == std::vector<bool>{true, false});
    CHECK(f.host.confirms.size() == 2);
    CHECK(f.host.balloons.back().find("asks for your approval") != std::string::npos);

    fake.is_locked = true;
    controller.on_action(TrayAction::LinkToggleWrites);
    CHECK(fake.requests.size() == 2);
    fake.is_locked = false;
    fake.is_enforced = true;
    controller.on_action(TrayAction::LinkToggleWrites);
    CHECK(fake.requests.size() == 2);
}

// ================= audit trail notice (docs/LINK.md "Audit trail") =================
namespace {
LinkView with_trail(LinkView v, bool on, const std::string& mode = "every") {
    v.trail_on = on;
    v.trail_mode = mode;
    v.trail_retention_days = 90;
    return v;
}
bool has_label(const std::vector<std::string>& labels, const std::string& text) {
    for (const auto& l : labels)
        if (l.find(text) != std::string::npos) return true;
    return false;
}
} // namespace

TEST_CASE("tray: the audit trail is visible in the Cloud link submenu while it is on", "[tray][menu][trail]") {
    TrayState state;
    state.link = with_trail(paired("connected"), true);
    auto labels = link_labels(build_menu(make_status(true, true), state));
    CHECK(has_label(labels, "Audit trail on \xE2\x80\x94 your admin records steps and screenshots on this desktop [off]"));
    state.link = with_trail(paired("connected"), false);
    CHECK_FALSE(has_label(link_labels(build_menu(make_status(true, true), state)), "Audit trail"));
    CHECK(trail_menu_text(state.link).empty());
    CHECK(trail_menu_text(with_trail(paired("connected"), true, "changes")).find("changes") != std::string::npos);
}

TEST_CASE("link tray: persistent audit-trail entry, tooltip, and a balloon when it turns on or off", "[tray][link_tray][trail]") {
    auto m = build_link_menu(with_trail(paired("connected"), true));
    CHECK(has_label(top_labels(m), "Audit trail on \xE2\x80\x94 your admin records steps and screenshots on this desktop"));
    CHECK(m.tooltip.find("audit trail on") != std::string::npos);
    CHECK(m.tooltip.size() <= 127);
    LinkView long_names = with_trail(paired("connected"), true);
    long_names.organisation = std::string(120, 'O');
    CHECK(build_link_menu(long_names).tooltip.find("audit trail on") != std::string::npos);   // never cut off
    CHECK_FALSE(has_label(top_labels(build_link_menu(paired("connected"))), "Audit trail"));

    Fixture f;
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    LinkTrayController controller(services, f.paths);
    f.link.current = paired("connected");
    controller.on_tick();
    const auto base = f.host.balloons.size();
    f.link.current = with_trail(paired("connected"), true);
    controller.on_tick();
    controller.on_tick();
    REQUIRE(f.host.balloons.size() == base + 1);
    CHECK(f.host.balloons.back() ==
          "fairyfly link|Audit trail on: your administrator records the steps AI assistants take on this desktop, with screenshots (kept 90 days)");
    f.link.current = with_trail(paired("connected"), false);
    controller.on_tick();
    REQUIRE(f.host.balloons.size() == base + 2);
    CHECK(f.host.balloons.back().find("Audit trail off") != std::string::npos);
}

TEST_CASE("mcp tray: a balloon when the cloud link's audit trail turns on or off", "[tray][controller][trail]") {
    Fixture f;
    f.control.current = make_status(false, true);   // the MCP server is not running: only the trail speaks
    auto controller = f.make();
    controller.on_tick();
    CHECK(f.host.balloons.empty());
    f.link.current = with_trail(paired("connected"), true, "changes");
    controller.on_tick();
    REQUIRE(f.host.balloons.size() == 1);
    CHECK(f.host.balloons[0].find("fairyfly MCP|Audit trail on") == 0);
    CHECK(f.host.balloons[0].find("of the steps that change SAP") != std::string::npos);
    controller.on_tick();
    CHECK(f.host.balloons.size() == 1);
    f.link.current = paired("connected");
    controller.on_tick();
    REQUIRE(f.host.balloons.size() == 2);
    CHECK(f.host.balloons[1].find("Audit trail off") != std::string::npos);
}

// ================= starting the trays like a Windows app =================
namespace {
struct FakeSettings : TraySettings {
    std::map<std::string, std::string> values;
    std::optional<std::string> get(const std::string& n) override { auto it = values.find(n); if (it == values.end()) return std::nullopt; return it->second; }
    bool set(const std::string& n, const std::string& v) override { values[n] = v; return true; }
};

struct ShowingHost : FakeHost {
    bool other_tray = true;
    int shown = 0;
    bool show_running_instance() override { ++shown; return other_tray; }
};

AutostartPlan link_plan() {
    AutostartPlan plan;
    plan.name = kLinkAutostartValueName;
    plan.exe = "C:\\Users\\pat\\AppData\\Local\\Programs\\fairyfly\\fairyfly.exe";
    plan.program = "C:\\Users\\pat\\AppData\\Local\\Programs\\fairyfly\\fairyfly-tray.exe";
    plan.line = link_autostart_command(plan.program, {"link", "run", "--tray"});
    return plan;
}
} // namespace

TEST_CASE("tray settings: \"Allow changes in SAP\" is stored under its name in the settings table", "[tray][settings]") {
    // review 2026-10-27 (Codex): the link and the approval test each spelled the value and wired Standing themselves
    auto settings = std::make_shared<FakeSettings>();
    const auto standing = fairyfly::link::saved_standing(settings);
    CHECK_FALSE(standing->on());
    CHECK(standing->set(true));
    CHECK(settings->values.at(kLinkAllowChangesSetting) == "1");
    CHECK(fairyfly::link::saved_standing(settings)->on());   // read back by the next start
    CHECK(std::find(std::begin(kTraySettingNames), std::end(kTraySettingNames), std::string(kLinkAllowChangesSetting)) !=
          std::end(kTraySettingNames));
    CHECK(std::find(std::begin(kTraySettingNames), std::end(kTraySettingNames), std::string(kAutostartDefaultedSetting)) !=
          std::end(kTraySettingNames));
}

TEST_CASE("tray settings: no file under src/ spells a setting name or the settings key outside the table", "[tray][settings]") {
    namespace fs = std::filesystem;
    const fs::path src = FAIRYFLY_SOURCE_DIR;
    std::vector<std::string> offenders;
    for (const auto& e : fs::recursive_directory_iterator(src)) {
        const auto ext = e.path().extension();
        if (!e.is_regular_file() || (ext != ".cpp" && ext != ".h" && ext != ".hpp")) continue;
        const fs::path rel = fs::relative(e.path(), src);
        if (rel == fs::path("include") / "tray" / "tray.h" || rel == fs::path("include") / "tray" / "tray_contract.h") continue;
        std::ifstream in(e.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (const char* name : kTraySettingNames)
            if (text.find(std::string("\"") + name + "\"") != std::string::npos) offenders.push_back(rel.string() + ": " + name);
        if (text.find(R"(DataZoo\\fairyfly\\tray)") != std::string::npos) offenders.push_back(rel.string() + ": settings key");
    }
    CAPTURE(offenders);
    CHECK(offenders.empty());
}

TEST_CASE("tray start program: fairyfly-tray.exe next to fairyfly.exe when it is installed", "[tray][starter]") {
    const std::string exe = "C:\\Programs\\fairyfly\\fairyfly.exe";
    std::vector<std::filesystem::path> asked;
    CHECK(tray_start_program(exe, [&](const std::filesystem::path& p) { asked.push_back(p); return true; }) ==
          "C:\\Programs\\fairyfly\\fairyfly-tray.exe");
    CHECK(asked == std::vector<std::filesystem::path>{"C:\\Programs\\fairyfly\\fairyfly-tray.exe"});
    CHECK(tray_start_program(exe, [](const std::filesystem::path&) { return false; }) == exe);   // portable exe alone
    CHECK(tray_start_program(exe, nullptr) == exe);
    CHECK(link_autostart_command(tray_start_program(exe, [](const std::filesystem::path&) { return true; }), {"link", "run", "--tray"}) ==
          "C:\\Programs\\fairyfly\\fairyfly-tray.exe link run --tray");
}

TEST_CASE("Start with Windows: on after the first start through the starter, then the person decides", "[tray][autostart][starter]") {
    const AutostartPlan plan = link_plan();
    FakeRegistry run;
    FakeSettings settings;

    // started from a console (not the starter): nothing changes
    CHECK_FALSE(settle_autostart(plan, false, run, settings));
    CHECK(run.values.empty());
    CHECK(settings.values.empty());

    // the first start from the Start menu: on, and noted
    CHECK(settle_autostart(plan, true, run, settings));
    CHECK(run.values.at("fairyfly-link") == plan.line);
    CHECK(settings.values.at(kAutostartDefaultedSetting) == "1");
    CHECK_FALSE(settle_autostart(plan, true, run, settings));   // already right

    // turned off in the menu: stays off at the next start
    run.values.clear();
    CHECK_FALSE(settle_autostart(plan, true, run, settings));
    CHECK(run.values.empty());
}

TEST_CASE("Start with Windows: an older Run value that starts fairyfly.exe moves to the starter, options kept", "[tray][autostart][starter]") {
    const AutostartPlan plan = link_plan();
    FakeRegistry run;
    FakeSettings settings;
    run.values["fairyfly-link"] = quote_arg(plan.exe) + " link run --tray --state D:\\pairing.json";
    CHECK(settle_autostart(plan, true, run, settings));
    CHECK(run.values.at("fairyfly-link") == quote_arg(plan.program) + " link run --tray --state D:\\pairing.json");
    CHECK(settings.values.empty());   // the person had already chosen it

    // another copy's value (a different folder) is left alone, and so is everything without the starter installed
    run.values["fairyfly-link"] = "D:\\tools\\fairyfly.exe link run --tray";
    CHECK_FALSE(settle_autostart(plan, true, run, settings));
    CHECK(run.values.at("fairyfly-link") == "D:\\tools\\fairyfly.exe link run --tray");
    AutostartPlan portable = plan;
    portable.program = portable.exe;
    run.values["fairyfly-link"] = quote_arg(plan.exe) + " link run --tray";
    CHECK_FALSE(settle_autostart(portable, true, run, settings));
}

TEST_CASE("link tray menu: Start with Windows, checkable, switches the Run value", "[tray][link_tray][autostart]") {
    CHECK(find_item(build_link_menu(paired("connected")), TrayAction::ToggleAutostart) == nullptr);   // no Run key access
    AutostartView on;
    on.available = true;
    on.on = true;
    const auto m = build_link_menu(paired("connected"), {}, {}, on);
    const MenuItem* item = find_item(m, TrayAction::ToggleAutostart);
    REQUIRE(item != nullptr);
    CHECK(item->label == "Start with Windows");
    CHECK(item->checkable);
    CHECK(item->checked);

    Fixture f;
    FakeRegistry run;
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    services.run_key = &run;
    services.autostart = link_plan();
    LinkTrayController controller(services, f.paths);
    const MenuItem* before = nullptr;
    auto menu = controller.menu();
    before = find_item(menu, TrayAction::ToggleAutostart);
    REQUIRE(before != nullptr);
    CHECK_FALSE(before->checked);
    controller.on_action(TrayAction::ToggleAutostart);
    CHECK(run.values.at("fairyfly-link") == services.autostart.line);
    menu = controller.menu();
    CHECK(find_item(menu, TrayAction::ToggleAutostart)->checked);
    controller.on_action(TrayAction::ToggleAutostart);
    CHECK(run.values.empty());

    run.ok = false;
    controller.on_action(TrayAction::ToggleAutostart);
    REQUIRE_FALSE(f.host.balloons.empty());
    CHECK(f.host.balloons.back().find("Start with Windows") != std::string::npos);
}

TEST_CASE("a second start opens the running tray's menu instead of a message box", "[tray][run][starter]") {
    {
        ShowingHost host;
        FakeGuard guard;
        guard.other_running = true;
        LinkTrayServices services;
        services.host = &host;
        TrayPaths paths;
        bool ran = false;
        CHECK(run_link_with_tray([&] { ran = true; return 0; }, services, guard, paths) == 0);
        CHECK_FALSE(ran);
        CHECK(host.shown == 1);
        CHECK(host.boxes.empty());
    }
    {
        Fixture f;
        ShowingHost host;
        FakeGuard guard;
        guard.other_running = true;
        TrayServices services;
        services.host = &host;
        CHECK(run_with_tray(f.runner, services, guard, f.paths) == 0);
        CHECK_FALSE(f.runner.ran);
        CHECK(host.shown == 1);
        CHECK(host.boxes.empty());
    }
    {   // its window is not there (it is just starting or ending): the message box as before
        ShowingHost host;
        host.other_tray = false;
        FakeGuard guard;
        guard.other_running = true;
        LinkTrayServices services;
        services.host = &host;
        CHECK(run_link_with_tray([] { return 0; }, services, guard, TrayPaths{}) == 0);
        REQUIRE(host.boxes.size() == 1);
        CHECK(host.boxes[0].find("already running") != std::string::npos);
    }
}

TEST_CASE("tray menus: both trays show the running version, greyed out, above Check for updates", "[tray][menu]") {
    for (const auto& m : {build_menu(make_status(true, false, {}), {}), build_link_menu(paired("connected"))}) {
        const auto labels = top_labels(m);
        auto at = std::find(labels.begin(), labels.end(), kVersionItem);
        REQUIRE(at != labels.end());
        REQUIRE(at + 1 != labels.end());
        CHECK(*(at + 1) == "Check for updates (console)");
        CHECK_FALSE(m.items[size_t(at - labels.begin())].enabled);
    }
}

TEST_CASE("tray menu entries: one builder for both trays", "[tray][menu]") {
    const MenuItem version = version_item();
    CHECK(version.label == kVersionItem);
    CHECK_FALSE(version.enabled);
    CHECK(version.action == TrayAction::None);
    CHECK(separator().separator);
    const MenuItem quit = item("Quit", TrayAction::Quit);
    CHECK(quit.enabled);
    CHECK(quit.action == TrayAction::Quit);
}

TEST_CASE("tray tooltip: at most 127 UTF-16 units, cut between characters, with an ellipsis", "[tray][menu]") {
    CHECK(fit_tooltip("fairyfly link: connected") == "fairyfly link: connected");
    const std::string ascii(200, 'a');
    CHECK(fit_tooltip(ascii) == std::string(124, 'a') + "...");
    CHECK(fit_tooltip(std::string(127, 'a')) == std::string(127, 'a'));
    // 100 "ä" (2 bytes, 1 UTF-16 unit each) fit; a byte cut at 127 would split one
    std::string umlauts;
    for (int i = 0; i < 100; ++i) umlauts += "\xC3\xA4";
    CHECK(fit_tooltip(umlauts) == umlauts);
    std::string long_umlauts;
    for (int i = 0; i < 130; ++i) long_umlauts += "\xC3\xA4";
    const std::string cut = fit_tooltip(long_umlauts);
    CHECK(cut.size() == 124 * 2 + 3);
    CHECK(cut.substr(cut.size() - 3) == "...");
    // a character outside the BMP counts two units and is never split
    std::string emoji;
    for (int i = 0; i < 70; ++i) emoji += "\xF0\x9F\x98\x80";
    const std::string cut_emoji = fit_tooltip(emoji);
    CHECK(cut_emoji == emoji.substr(0, 62 * 4) + "...");
}

TEST_CASE("tray contract: the names other processes rely on are spelled once", "[tray][contract]") {
    // review 2026-10-25: the launcher (installer --close, autostart cleanup) hard-coded the tray's window class,
    // messages, registry keys and the notification CLSID; a rename on one side broke the other silently
    namespace fs = std::filesystem;
    const fs::path src = FAIRYFLY_SOURCE_DIR;
    const std::vector<std::string> names = {"fairyfly_tray_window", "fairyfly-tray-quit", "fairyfly-tray-show-menu",
                                            "DAAF1065", "DataZoo.fairyfly", "DataZoo\\fairyfly\\tray",
                                            "CurrentVersion\\Run\"", "L\"fairyfly-link\"", "L\"fairyfly-mcp\"",
                                            "ValueName = \""};  // the Run values (not the link's client label)
    std::vector<std::string> offenders;
    for (const auto& e : fs::recursive_directory_iterator(src)) {
        const auto ext = e.path().extension();
        if (!e.is_regular_file() || (ext != ".cpp" && ext != ".h")) continue;
        const fs::path rel = fs::relative(e.path(), src);
        if (rel == fs::path("include") / "tray" / "tray_contract.h") continue;
        std::ifstream in(e.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (const auto& name : names)
            if (text.find(name) != std::string::npos) offenders.push_back(rel.string() + ": " + name);
    }
    CAPTURE(offenders);
    CHECK(offenders.empty());
}

TEST_CASE("tray contract: the toast activator CLSID parses as a GUID", "[tray][contract]") {
    GUID clsid{};
    REQUIRE(SUCCEEDED(CLSIDFromString(fairyfly::tray::contract::kToastActivatorClsid, &clsid)));
    CHECK(clsid.Data1 == 0xDAAF1065u);  // the CLSID the Start-menu shortcut (packaging/fairyfly.wxs) names
}

TEST_CASE("tray actions: open, console and detached start, shared by both trays", "[tray][actions]") {
    FakeOpener opener;
    FakeLauncher launcher;
    std::vector<std::string> notices;
    const TrayActions actions{&opener, &launcher, "C:/fairyfly/fairyfly.exe",
                              [&](const std::string& text) { notices.push_back(text); }};
    actions.open("C:/logs/link.log", "the log");
    CHECK(opener.opened.size() == 1);
    CHECK(notices.empty());
    opener.ok = false;
    actions.open("C:/logs/link.log", "the log");
    REQUIRE(notices.size() == 1);
    CHECK(notices[0] == "Could not open the log: C:/logs/link.log");

    actions.console({"link", "status"});
    REQUIRE(launcher.requests.size() == 1);
    CHECK(launcher.requests[0].new_console);
    CHECK(launcher.requests[0].command_line == console_command_line("C:/fairyfly/fairyfly.exe", {"link", "status"}));

    CHECK(actions.start_detached({"link", "run", "--tray"}));
    REQUIRE(launcher.requests.size() == 2);
    CHECK(launcher.requests[1].detached);
    CHECK(launcher.requests[1].command_line == join_command_line("C:/fairyfly/fairyfly.exe", {"link", "run", "--tray"}));
    launcher.pid.reset();
    CHECK_FALSE(actions.start_detached({"link", "run", "--tray"}));
    actions.console({"update", "check"});
    CHECK(notices.back() == "Could not open a console");
    // without a launcher nothing starts, and a console says nothing (as before)
    const TrayActions none{nullptr, nullptr, "x.exe", [&](const std::string& text) { notices.push_back(text); }};
    const auto before = notices.size();
    none.console({"link", "pair"});
    CHECK_FALSE(none.start_detached({"mcp"}));
    none.open("y", "the log");
    CHECK(notices.size() == before + 1);   // only the failed open
}

TEST_CASE("tray headers: the shared tray header does not pull in the MCP server", "[tray][contract]") {
    // review 2026-10-25: tray.h (488 lines) pulled mcp/* into the link; the MCP server's tray is mcp_tray.h
    std::ifstream in(std::filesystem::path(FAIRYFLY_SOURCE_DIR) / "include" / "tray" / "tray.h", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("include/mcp/") == std::string::npos);
    CHECK(text.find("mcp::") == std::string::npos);
}

TEST_CASE("link tray: a desktop clock off by minutes is shown with the fix", "[tray][link_tray][clock]") {
    auto v = paired("connected");
    v.clock_skew_s = 346;
    const auto m = build_link_menu(v);
    CHECK(m.icon == IconState::Warning);
    const auto labels = top_labels(m);
    REQUIRE(labels.size() > 1);
    CHECK(labels[1].find("346 s behind") != std::string::npos);
    CHECK(labels[1].find("Sync now") != std::string::npos);
    CHECK(m.tooltip.find("clock 346 s behind") != std::string::npos);
    CHECK(m.tooltip.size() <= 127);
    v.clock_skew_s = -120;
    CHECK(clock_skew_text(v).find("120 s ahead") != std::string::npos);
    v.clock_skew_s = 0;
    CHECK(clock_skew_text(v).empty());
    CHECK(build_link_menu(v).icon == IconState::Running);
}

TEST_CASE("link tray controller: one balloon when the clock goes off, none while it stays off", "[tray][link_tray][clock]") {
    Fixture f;
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    LinkTrayController controller(services, f.paths);
    f.link.current = paired("connected");
    controller.on_tick();
    const auto before = f.host.balloons.size();
    f.link.current.clock_skew_s = 346;
    controller.on_tick();
    controller.on_tick();
    REQUIRE(f.host.balloons.size() == before + 1);
    CHECK(f.host.balloons.back().find("346 s behind") != std::string::npos);
    f.link.current.clock_skew_s = 0;
    controller.on_tick();
    CHECK(f.host.balloons.size() == before + 1);   // fixed: the menu line goes away, no extra noise
}

TEST_CASE("link tray: a call SAP GUI does not answer turns the icon amber and names it", "[tray][link_tray][hung]") {
    // live 2026-10-27: gui_screen_read hung in SAP GUI, every later call waited, and the tray stayed teal
    auto v = paired("connected");
    v.stuck_tool = "gui_screen_read";
    v.stuck_connection = "0";
    v.stuck_s = 75;
    const auto m = build_link_menu(v);
    CHECK(m.icon == IconState::Warning);
    const auto labels = top_labels(m);
    REQUIRE(labels.size() > 1);
    CHECK(labels[1] == "SAP GUI has not answered gui_screen_read on connection 0 for 75 s: look for a hidden SAP dialog "
                       "or a long-running step (e.g. a report started with F8), or end that SAP session");
    CHECK(m.tooltip.find("SAP GUI not answering gui_screen_read (75 s)") != std::string::npos);
    CHECK(m.tooltip.size() <= 127);
    v.stuck_connection.clear();
    CHECK(stuck_call_text(v).find("gui_screen_read for 75 s") != std::string::npos);
    v.stuck_tool.clear();   // the call returned
    CHECK(stuck_call_text(v).empty());
    CHECK(build_link_menu(v).icon == IconState::Running);
}

TEST_CASE("link tray controller: one balloon when a call hangs, back to normal when it returns", "[tray][link_tray][hung]") {
    Fixture f;
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    LinkTrayController controller(services, f.paths);
    f.link.current = paired("connected");
    controller.on_tick();
    CHECK(controller.view().icon == IconState::Running);
    const auto before = f.host.balloons.size();
    f.link.current.stuck_tool = "gui_screen_read";
    f.link.current.stuck_connection = "0";
    f.link.current.stuck_s = 61;
    controller.on_tick();
    f.link.current.stuck_s = 76;
    controller.on_tick();
    CHECK(controller.view().icon == IconState::Warning);
    REQUIRE(f.host.balloons.size() == before + 1);
    CHECK(f.host.balloons.back().find("SAP GUI has not answered gui_screen_read") != std::string::npos);
    f.link.current.stuck_tool.clear();
    f.link.current.stuck_s = 0;
    controller.on_tick();
    CHECK(controller.view().icon == IconState::Running);
    CHECK(f.host.balloons.size() == before + 1);
}

// ================= a single-instance guard held by another installation's tray =================
// Live 2026-10-29 (Windows 10): the MSI was uninstalled, its link tray (pid 2892) kept running from the deleted
// fairyfly.exe and held the guard; the Microsoft Store tray then opened that orphan's menu and ended without a word.
namespace {
const std::string kMsiExe = "C:\\Users\\ada\\AppData\\Local\\Programs\\fairyfly\\fairyfly.exe";
const std::string kMsixExe = "C:\\Program Files\\WindowsApps\\DataZoo.fairyfly_2026.1029.0.0_x64__73q587949tsjt\\fairyfly.exe";

/// The trays a held guard belongs to; a quit request frees the guard unless the tray ignores it.
struct FakeInstances : TrayInstances {
    std::vector<RunningTray> trays;
    std::vector<std::uint32_t> asked;
    FakeGuard* guard = nullptr;
    bool quits = true;
    std::vector<RunningTray> running() override { return trays; }
    bool ask_to_quit(std::uint32_t pid) override {
        asked.push_back(pid);
        if (quits && guard) guard->other_running = false;
        return true;
    }
};
std::function<bool(const std::filesystem::path&)> files(std::vector<std::string> present) {
    return [present](const std::filesystem::path& p) {
        return std::find(present.begin(), present.end(), p.string()) != present.end();
    };
}
}  // namespace

TEST_CASE("held tray: this installation's running tray opens its menu, as before", "[tray][held]") {
    const auto d = decide_held_tray({{2892, kMsiExe}}, kMsiExe, files({kMsiExe}), "fairyfly link tray");
    CHECK(d.action == HeldTrayAction::ShowRunning);
    // spelled differently by the process probe: still the same file
    const auto spelled = decide_held_tray({{2892, "\\\\?\\c:\\users\\ada\\appdata\\local\\programs\\fairyfly\\FAIRYFLY.exe"}}, kMsiExe,
                                          files({kMsiExe}), "fairyfly link tray");
    CHECK(spelled.action == HeldTrayAction::ShowRunning);
    // nothing to tell (no window found, or its executable could not be read): as before
    CHECK(decide_held_tray({}, kMsixExe, files({kMsixExe}), "fairyfly link tray").action == HeldTrayAction::ShowRunning);
    CHECK(decide_held_tray({{2892, ""}}, kMsixExe, files({kMsixExe}), "fairyfly link tray").action == HeldTrayAction::ShowRunning);
}

TEST_CASE("held tray: a tray whose fairyfly.exe was removed (uninstalled MSI) is asked to quit", "[tray][held]") {
    const auto d = decide_held_tray({{2892, kMsiExe}}, kMsixExe, files({kMsixExe}), "fairyfly link tray");
    CHECK(d.action == HeldTrayAction::TakeOver);
    CHECK(d.pids == std::vector<std::uint32_t>{2892});
    CHECK(d.message.find("2892") != std::string::npos);
    CHECK(d.message.find(kMsiExe) != std::string::npos);
    // one of them still this installation's: its menu, nothing is taken over
    CHECK(decide_held_tray({{2892, kMsiExe}, {4711, kMsixExe}}, kMsixExe, files({kMsixExe}), "fairyfly link tray").action ==
          HeldTrayAction::ShowRunning);
}

TEST_CASE("held tray: another installation's tray (MSI vs Microsoft Store) is named, not silently obeyed", "[tray][held]") {
    const auto d = decide_held_tray({{2892, kMsiExe}}, kMsixExe, files({kMsiExe, kMsixExe}), "fairyfly link tray");
    CHECK(d.action == HeldTrayAction::Explain);
    CHECK(d.pids == std::vector<std::uint32_t>{2892});
    CHECK(d.message.find("fairyfly link tray") != std::string::npos);
    CHECK(d.message.find(kMsiExe) != std::string::npos);
    CHECK(d.message.find("2892") != std::string::npos);
    CHECK(d.message.find("Quit") != std::string::npos);
}

TEST_CASE("run_link_with_tray: the tray of a removed installation is asked to quit and this tray runs", "[tray][link_tray][held]") {
    Fixture f;
    f.paths.exe = kMsixExe;
    f.link.current = paired("connected");
    FakeGuard guard;
    guard.other_running = true;
    FakeInstances instances;
    instances.trays = {{2892, kMsiExe}};
    instances.guard = &guard;
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    services.claim.instances = &instances;
    services.claim.exists = files({kMsixExe});
    services.claim.poll = std::chrono::milliseconds(1);
    bool ran = false;
    const int code = run_link_with_tray([&] { ran = true; return 0; }, services, guard, f.paths);
    CHECK(code == 0);
    CHECK(ran);
    CHECK(guard.held);
    CHECK(instances.asked == std::vector<std::uint32_t>{2892});
    CHECK(f.host.boxes.empty());
}

TEST_CASE("run_link_with_tray: a removed installation's tray that does not quit is named in a message box", "[tray][link_tray][held]") {
    Fixture f;
    f.paths.exe = kMsixExe;
    f.link.current = paired("connected");
    FakeGuard guard;
    guard.other_running = true;
    FakeInstances instances;
    instances.trays = {{2892, kMsiExe}};
    instances.guard = &guard;
    instances.quits = false;
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    services.claim.instances = &instances;
    services.claim.exists = files({kMsixExe});
    services.claim.takeover_wait = std::chrono::milliseconds(20);
    services.claim.poll = std::chrono::milliseconds(1);
    bool ran = false;
    const int code = run_link_with_tray([&] { ran = true; return 0; }, services, guard, f.paths);
    CHECK(code == 0);
    CHECK_FALSE(ran);
    CHECK(instances.asked == std::vector<std::uint32_t>{2892});
    REQUIRE(f.host.boxes.size() == 1);
    CHECK(f.host.boxes[0].find("2892") != std::string::npos);
    CHECK(f.host.boxes[0].find("Task Manager") != std::string::npos);
}

TEST_CASE("run_link_with_tray: another installation's running tray is named and left alone", "[tray][link_tray][held]") {
    Fixture f;
    f.paths.exe = kMsixExe;
    f.link.current = paired("connected");
    FakeGuard guard;
    guard.other_running = true;
    FakeInstances instances;
    instances.trays = {{2892, kMsiExe}};
    instances.guard = &guard;
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    services.claim.instances = &instances;
    services.claim.exists = files({kMsiExe, kMsixExe});
    bool ran = false;
    const int code = run_link_with_tray([&] { ran = true; return 0; }, services, guard, f.paths);
    CHECK(code == 0);
    CHECK_FALSE(ran);
    CHECK(instances.asked.empty());
    REQUIRE(f.host.boxes.size() == 1);
    CHECK(f.host.boxes[0].find(kMsiExe) != std::string::npos);
}

TEST_CASE("run_with_tray: the MCP tray takes over from a removed installation's tray as well", "[tray][run][held]") {
    Fixture f;
    f.paths.exe = kMsixExe;
    FakeGuard guard;
    guard.other_running = true;
    FakeInstances instances;
    instances.trays = {{2892, kMsiExe}};
    instances.guard = &guard;
    TrayServices services;
    services.host = &f.host;
    services.claim.instances = &instances;
    services.claim.exists = files({kMsixExe});
    services.claim.poll = std::chrono::milliseconds(1);
    auto driver = drive(f.host, [&](TrayListener& l) { l.on_action(TrayAction::Quit); });
    const int code = run_with_tray(f.runner, services, guard, f.paths);
    driver.join();
    CHECK(code == 0);
    CHECK(f.runner.ran);
    CHECK(instances.asked == std::vector<std::uint32_t>{2892});
}

TEST_CASE("run_link_with_tray: two trays taking over at once: the one that loses opens the winner's menu", "[tray][link_tray][held]") {
    Fixture f;
    f.paths.exe = kMsixExe;
    f.link.current = paired("connected");
    FakeGuard guard;
    guard.other_running = true;
    // the orphan quits when asked, but another tray of this installation started at the same moment wins the guard
    struct RacingInstances : FakeInstances {
        bool ask_to_quit(std::uint32_t pid) override {
            asked.push_back(pid);
            trays = {{5120, kMsixExe}};   // the winner's window; the guard stays held (by it)
            return true;
        }
    } instances;
    instances.trays = {{2892, kMsiExe}};
    LinkTrayServices services;
    services.host = &f.host;
    services.link = &f.link;
    services.claim.instances = &instances;
    services.claim.exists = files({kMsixExe});
    services.claim.takeover_wait = std::chrono::milliseconds(200);
    services.claim.poll = std::chrono::milliseconds(1);
    bool ran = false;
    const int code = run_link_with_tray([&] { ran = true; return 0; }, services, guard, f.paths);
    CHECK(code == 0);
    CHECK_FALSE(ran);
    REQUIRE(f.host.boxes.size() == 1);   // the fake host cannot open a menu: the "already running" fallback
    CHECK(f.host.boxes[0].find("already running") != std::string::npos);
    CHECK(f.host.boxes[0].find("Task Manager") == std::string::npos);
}
