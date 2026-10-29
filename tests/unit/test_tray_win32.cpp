// The real Win32 tray window, without a server or a link: how other processes end a tray. Creates a hidden window
// and, where a taskbar exists, a notification icon for a moment (removed again); no registry value, mutex or process.
// Live 2026-10-29: `taskkill /im fairyfly.exe` (no /f) did not end the link tray, and after `msiexec /x` it kept
// running from the deleted fairyfly.exe.
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "include/system/process_probe.h"
#include "include/tray/tray_contract.h"
#include "include/tray/tray_win32.h"

using namespace fairyfly::tray;

namespace {

struct QuitListener : TrayListener {
    std::atomic<int> quits{0};
    TrayMenuModel menu() override { return {}; }
    TrayView view() override { return {IconState::Running, "fairyfly window test"}; }
    void on_action(TrayAction action) override {
        if (action == TrayAction::Quit) ++quits;
    }
    std::string left_click_text() override { return {}; }
    void on_tick() override {}
};

/// The link tray window of this process (a real link tray of the person may run beside the test).
HWND own_tray_window() {
    struct Search {
        DWORD pid = GetCurrentProcessId();
        HWND found = nullptr;
    } search;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            wchar_t cls[64] = {};
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != s->pid || !GetClassNameW(hwnd, cls, 64) || wcscmp(cls, contract::kWindowClass) != 0) return TRUE;
            s->found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

/// Runs a link tray host on its own thread for the test.
struct RunningHost {
    std::unique_ptr<TrayHost> host = make_win32_tray_host(TrayKind::Link);
    QuitListener listener;
    std::thread ui;
    bool ok = false;
    RunningHost() {
        std::promise<bool> ready;
        auto up = ready.get_future();
        ui = std::thread([this, &ready] { host->run(listener, [&ready](bool r) { ready.set_value(r); }); });
        ok = up.get();
    }
    ~RunningHost() {
        host->post_quit();
        if (ui.joinable()) ui.join();
    }
    bool wait_for_quit() {
        for (int i = 0; i < 200 && listener.quits == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return listener.quits > 0;
    }
};

}  // namespace

TEST_CASE("tray window: WM_CLOSE (taskkill without /f) quits the tray as its menu's Quit does", "[tray][win32][close]") {
    RunningHost tray;
    REQUIRE(tray.ok);
    const HWND hwnd = own_tray_window();
    REQUIRE(hwnd != nullptr);
    // what `taskkill /im fairyfly.exe` sends: the tray must end the process (Quit stops the link first), not only lose
    // its window while the link runs on without a tray
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    CHECK(tray.wait_for_quit());
    CHECK(IsWindow(hwnd));   // the window ends with the tray, once Quit has stopped what runs
}

TEST_CASE("tray window: another process finds the running tray by its window and its executable, and asks it to quit",
          "[tray][win32][held]") {
    RunningHost tray;
    REQUIRE(tray.ok);
    auto instances = make_win32_tray_instances(TrayKind::Link);
    const auto running = instances->running();
    const auto own = std::find_if(running.begin(), running.end(),
                                  [](const RunningTray& t) { return t.pid == GetCurrentProcessId(); });
    REQUIRE(own != running.end());
    CHECK(fairyfly::system::same_executable(own->image, current_exe_path()));
    // the MCP tray's instances do not include a link tray
    for (const auto& t : make_win32_tray_instances(TrayKind::Mcp)->running()) CHECK(t.pid != GetCurrentProcessId());
    CHECK(instances->ask_to_quit(GetCurrentProcessId()));
    CHECK(tray.wait_for_quit());
}
