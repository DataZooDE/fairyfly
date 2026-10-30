// fairyfly-tray.exe --close/--uninstall: what the MSI's close step runs (packaging/fairyfly.wxs) and which processes it
// closes. Live 2026-10-29: after `msiexec /x` the link tray of the removed MSI kept running and blocked the Microsoft
// Store tray. Pure: no process is started or ended here.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>

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

TEST_CASE("tray starter: COM's start of the toast activator's server starts the link tray", "[tray][close][toast]") {
    // MSI: LocalServer32 appends -Embedding; MSIX: the manifest's com:ExeServer Arguments="-Embedding" (and should Windows
    // append its own as well, the doubled argument still is the activator's start)
    for (const auto& args : {std::vector<std::wstring>{L"-Embedding"}, std::vector<std::wstring>{L"/Embedding"},
                             std::vector<std::wstring>{L"-Embedding", L"-Embedding"}}) {
        CHECK(is_com_server_start(args));
        const auto c = parse_starter_command(args, kFolder);
        CHECK(c.mode == StarterMode::Start);
        CHECK(c.args.empty());  // fairyfly.exe link run --tray, as from the Start menu
    }
    CHECK(std::wstring(fairyfly::tray::contract::kComServerArgument) == L"-Embedding");
    CHECK_FALSE(is_com_server_start({}));
    CHECK_FALSE(is_com_server_start({L"-Embedding", L"mcp"}));
    CHECK(parse_starter_command({L"mcp", L"-Embedding"}, kFolder).args == std::vector<std::wstring>{L"mcp", L"-Embedding"});
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
}

namespace {
/// A process as the close step holds it: the pid it was opened by, the program it really is, and which open it was.
struct FakeProcess {
    std::uint32_t pid = 0;
    std::wstring image;
    int handle = 0;
};
const std::wstring kStore = L"C:\\Program Files\\WindowsApps\\DataZoo.fairyfly_2026.1029.0.0_x64__73q587949tsjt\\fairyfly.exe";
}  // namespace

TEST_CASE("tray starter: the processes kept are the ones opened and checked, never reopened by pid", "[tray][close]") {
    // what the processes are when they are opened (a moment after the enumeration)
    std::map<std::uint32_t, std::wstring> now = {
        {2892, kFolder + L"\\fairyfly.exe"},        // the link tray
        {3100, kFolder + L"\\fairyfly.exe"},        // `fairyfly mcp` for a desktop assistant
        {3200, kFolder + L"\\fairyfly-tray.exe"},   // the installed starter itself (an older MSI's close step)
        {4711, kStore},
        {6000, L"C:\\Windows\\explorer.exe"},
    };
    int opened = 0;
    std::vector<std::uint32_t> opens;
    const auto open = [&](std::uint32_t pid) -> std::optional<FakeProcess> {
        opens.push_back(pid);
        const auto it = now.find(pid);
        if (it == now.end()) return std::nullopt;   // ended, or not ours to open
        return FakeProcess{pid, it->second, ++opened};
    };
    const auto image_of = [](const FakeProcess& p) { return p.image; };
    const std::vector<ProcessEntry> enumerated = {{2892, L""}, {3100, L""}, {3200, L""}, {4711, L""}, {5000, L""}, {6000, L""}};
    const auto kept = open_installed<FakeProcess>(enumerated, kFolder, 3200, open, image_of);
    REQUIRE(kept.size() == 2);
    CHECK(kept[0].pid == 2892);
    CHECK(kept[1].pid == 3100);
    // every pid is opened once, and what is kept is that open process (its handle), not a pid to open again
    CHECK(opens == std::vector<std::uint32_t>{2892, 3100, 4711, 5000, 6000});
    CHECK(kept[0].handle == 1);
    CHECK(kept[1].handle == 2);
}

TEST_CASE("tray starter: a pid reused by another program after the enumeration is not closed", "[tray][close]") {
    // enumerated as this installation's fairyfly.exe; by the time it is opened the pid belongs to notepad
    const std::vector<ProcessEntry> enumerated = {{2892, kFolder + L"\\fairyfly.exe"}, {3100, kFolder + L"\\fairyfly.exe"}};
    const auto open = [](std::uint32_t pid) -> std::optional<FakeProcess> {
        if (pid == 2892) return FakeProcess{pid, L"C:\\Windows\\notepad.exe", 1};
        return FakeProcess{pid, kFolder + L"\\fairyfly.exe", 2};
    };
    const auto kept = open_installed<FakeProcess>(enumerated, kFolder, 1, open, [](const FakeProcess& p) { return p.image; });
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].pid == 3100);
}
