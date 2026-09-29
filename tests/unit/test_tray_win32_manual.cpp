// MANUAL tests of the real Win32 tray implementations. Hidden by the [.] tag: they are never part of
// ctest or the CI filter. Run on an interactive desktop, one at a time, e.g.
//   unit_tests.exe "[tray_manual][icon]"
// They create a REAL tray icon / registry value / mutex, so never run them on a build agent.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "include/tray/tray_win32.h"

using namespace fairyfly::tray;

namespace {

struct DemoListener : TrayListener {
    std::atomic<int> tick{0};
    TrayMenuModel menu() override {
        fairyfly::mcp::ServerStatus s;
        s.running = true;
        s.endpoint = "http://127.0.0.1:8383/mcp";
        s.warnings = {"manual test warning"};
        return build_menu(s, {});
    }
    TrayView view() override {
        const IconState states[] = {IconState::Running, IconState::Warning, IconState::Stopped, IconState::Error};
        return {states[(tick / 1) % 4], "fairyfly manual tray test"};
    }
    void on_action(TrayAction) override {}
    std::string left_click_text() override { return "manual test: left click"; }
    void on_tick() override { ++tick; }
};

} // namespace

TEST_CASE("manual: tray icon cycles green/yellow/grey/red for 10 s (right-click for the menu)", "[.][tray_manual][icon]") {
    auto host = make_win32_tray_host();
    DemoListener listener;
    std::thread ui([&] { host->run(listener, [](bool ok) { REQUIRE(ok); }); });
    std::this_thread::sleep_for(std::chrono::seconds(10));
    host->balloon("fairyfly MCP", "manual balloon test");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    host->post_quit();
    ui.join();
}

TEST_CASE("manual: Run-key round trip uses a throwaway value name", "[.][tray_manual][registry]") {
    auto key = make_win32_run_key();
    const std::string name = "fairyfly-mcp-manual-test";
    REQUIRE(key->set(name, autostart_command("C:\\fairyfly\\fairyfly.exe", "")));
    CHECK(key->get(name).value_or("") == "\"C:\\fairyfly\\fairyfly.exe\" mcp --tray");
    REQUIRE(key->remove(name));
    CHECK_FALSE(key->get(name).has_value());
}

TEST_CASE("manual: single-instance mutex", "[.][tray_manual][mutex]") {
    auto first = make_win32_single_instance();
    auto second = make_win32_single_instance();
    REQUIRE(first->acquire());
    CHECK(second->exists());
    CHECK_FALSE(second->acquire());
}

TEST_CASE("manual: opener and launcher open real windows", "[.][tray_manual][shell]") {
    CHECK(make_win32_file_opener()->open(std::filesystem::temp_directory_path()));
    LaunchRequest request;
    request.command_line = console_command_line("cmd.exe", {"/c", "echo", "manual"});
    request.new_console = true;
    CHECK(make_win32_process_launcher()->launch(request).has_value());
}
