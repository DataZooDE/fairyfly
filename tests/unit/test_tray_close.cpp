// fairyfly-tray.exe --close/--uninstall: what the MSI's close step runs (packaging/fairyfly.wxs) and which processes it
// closes. Live 2026-10-29: after `msiexec /x` the link tray of the removed MSI kept running and blocked the Microsoft
// Store tray. Pure: no process is started or ended here.
#include <catch2/catch_test_macros.hpp>

#include "include/tray/tray_close.h"

using namespace fairyfly::tray::closing;

namespace {
const std::wstring kFolder = L"C:\\Users\\ada\\AppData\\Local\\Programs\\fairyfly";
}

TEST_CASE("tray starter: the installer names the install folder it closes", "[tray][close]") {
    // the MSI runs its own copy of the starter from a temporary file: [INSTALLFOLDER] ends with '\'
    const auto close = parse_starter_command({L"--close", kFolder + L"\\"}, L"C:\\Users\\ada\\AppData\\Local\\Temp");
    CHECK(close.mode == StarterMode::Close);
    CHECK(close.folder == kFolder);
    const auto uninstall = parse_starter_command({L"--uninstall", kFolder + L"\\"}, L"C:\\Temp");
    CHECK(uninstall.mode == StarterMode::Uninstall);
    CHECK(uninstall.folder == kFolder);
    // as installed MSIs up to 2026.10.28 run it: no folder, the starter's own
    const auto old = parse_starter_command({L"--close"}, kFolder);
    CHECK(old.mode == StarterMode::Close);
    CHECK(old.folder == kFolder);
    // anything else starts fairyfly.exe with these arguments
    const auto start = parse_starter_command({L"link", L"run", L"--tray"}, kFolder);
    CHECK(start.mode == StarterMode::Start);
    CHECK(start.args == std::vector<std::wstring>{L"link", L"run", L"--tray"});
}

TEST_CASE("tray starter: only this installation's fairyfly.exe and fairyfly-tray.exe are closed", "[tray][close]") {
    CHECK(is_installed_program(kFolder + L"\\fairyfly.exe", kFolder));
    CHECK(is_installed_program(L"\\\\?\\c:/users/ada/appdata/local/programs/FAIRYFLY/Fairyfly-Tray.exe", kFolder + L"\\"));
    // the Microsoft Store package's fairyfly.exe, another copy, a subfolder, other programs: left alone
    CHECK_FALSE(is_installed_program(L"C:\\Program Files\\WindowsApps\\DataZoo.fairyfly_2026.1029.0.0_x64__73q587949tsjt\\fairyfly.exe", kFolder));
    CHECK_FALSE(is_installed_program(L"C:\\tools\\fairyfly.exe", kFolder));
    CHECK_FALSE(is_installed_program(kFolder + L"\\old\\fairyfly.exe", kFolder));
    CHECK_FALSE(is_installed_program(kFolder + L"\\notepad.exe", kFolder));
    CHECK_FALSE(is_installed_program(kFolder + L"2\\fairyfly.exe", kFolder));
    CHECK_FALSE(is_installed_program(L"", kFolder));
    CHECK_FALSE(is_installed_program(kFolder + L"\\fairyfly.exe", L""));

    const std::vector<ProcessEntry> processes = {
        {2892, kFolder + L"\\fairyfly.exe"},        // the link tray
        {3100, kFolder + L"\\fairyfly.exe"},        // `fairyfly mcp` for a desktop assistant
        {3200, kFolder + L"\\fairyfly-tray.exe"},   // the installed starter itself (an older MSI's close step)
        {4711, L"C:\\Program Files\\WindowsApps\\DataZoo.fairyfly_2026.1029.0.0_x64__73q587949tsjt\\fairyfly.exe"},
        {5000, L""},                                 // not readable: not ours to end
        {6000, L"C:\\Windows\\explorer.exe"},
    };
    CHECK(processes_to_close(processes, kFolder, 3200) == std::vector<std::uint32_t>{2892, 3100});
    CHECK(processes_to_close(processes, kFolder, 1) == std::vector<std::uint32_t>{2892, 3100, 3200});
}
