// fairyfly-tray.exe: starts fairyfly's trays without a console window.
//
// fairyfly.exe is a console program, so Explorer (Start menu, autostart) opens a console for it before its tray hides
// itself again: a black window flashes. This small Windows-subsystem program gets no console; it starts fairyfly.exe
// next to it with CREATE_NO_WINDOW and the tray-child markers, and ends. Like pythonw.exe beside python.exe.
//
//   fairyfly-tray.exe                     fairyfly.exe link run --tray (the Start-menu entry)
//   fairyfly-tray.exe <args...>           fairyfly.exe <args...> (the Run values: link run --tray ..., mcp --tray ...)
//   fairyfly-tray.exe --close [<folder>]  ends fairyfly.exe and fairyfly-tray.exe started from <folder> (default: this
//                                         program's): the trays are asked to quit as from their menu, what still runs
//                                         after kCloseWaitMs is terminated (the installer, before it replaces or removes
//                                         the files; it runs its own copy of this program and names the folder)
//   fairyfly-tray.exe --uninstall [<folder>]  --close, then removes the Run values that start that folder's fairyfly,
//                                         the trays' settings and the notification registration (the installer, on
//                                         uninstall)
//   fairyfly-tray.exe -Embedding          the notification centre starts the link tray to answer an approval
//                                         (COM, the toast activator; src/link/desktop_approval_win32.cpp)
//
// It links nothing of fairyfly's own; the window class and message names match src/tray/tray_win32.cpp.
#include <windows.h>
#include "include/config/environment.h"
#include "include/tray/tray_close.h"
#include "include/tray/tray_contract.h"
#include "include/text/command_line.h"
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

namespace contract = fairyfly::tray::contract;

/// An ASCII name (the contract's Run values, config's environment names) for the wide Win32 calls.
std::wstring ascii_wide(const char* name) { return std::wstring(name, name + std::strlen(name)); }
constexpr DWORD kCloseWaitMs = 10000;

std::wstring module_dir() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::vector<std::wstring> arguments() {
    std::vector<std::wstring> out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return out;
    for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
    LocalFree(argv);
    return out;
}

void error_box(const std::wstring& text) { MessageBoxW(nullptr, text.c_str(), L"fairyfly", MB_OK | MB_ICONERROR | MB_SETFOREGROUND); }

int start(const std::wstring& dir, std::vector<std::wstring> args) {
    if (args.empty()) args = {L"link", L"run", L"--tray"};
    const std::wstring exe = dir + L"\\fairyfly.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error_box(L"fairyfly.exe was not found next to fairyfly-tray.exe:\n" + exe + L"\n\nInstall fairyfly again.");
        return 2;
    }
    std::wstring command = fairyfly::text::join_command_line<wchar_t>(exe, args);
    // the tray child (no relaunch, a dialog for startup errors) that this program started (Start with Windows default)
    // this small starter has no fairyfly_core (config::set_process_env); the names are config's
    for (const char* name : {fairyfly::config::env::kTrayChild, fairyfly::config::env::kTrayLaunched})
        SetEnvironmentVariableW(ascii_wide(name).c_str(), L"1");
    // the person started this from the Start menu: a tray that is already running may open its menu in front
    AllowSetForegroundWindow(ASFW_ANY);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        nullptr, nullptr, &si, &pi)) {
        error_box(L"fairyfly could not be started (error " + std::to_wstring(GetLastError()) + L"):\n" + exe);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

namespace closing = fairyfly::tray::closing;
constexpr DWORD kTerminateWaitMs = 3000;

/// The processes running now, with their executables (empty when not readable: another user's, elevated).
std::vector<closing::ProcessEntry> running_processes() {
    std::vector<closing::ProcessEntry> out;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        closing::ProcessEntry p;
        p.pid = entry.th32ProcessID;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid)) {
            wchar_t image[32768];
            DWORD size = ARRAYSIZE(image);
            if (QueryFullProcessImageNameW(h, 0, image, &size)) p.image.assign(image, size);
            CloseHandle(h);
        }
        out.push_back(std::move(p));
    }
    CloseHandle(snapshot);
    return out;
}

struct Target {
    DWORD pid = 0;
    HANDLE process = nullptr;
    bool asked = false;   ///< it has a tray window, which got the quit message
};

struct QuitRequest {
    std::vector<Target>* targets = nullptr;
    UINT quit = 0;
};

/// The tray windows of the processes to close get the quit message, as from their menu Quit (older trays do not
/// handle WM_CLOSE: it only destroyed their window while the link ran on).
BOOL CALLBACK ask_to_quit(HWND hwnd, LPARAM param) {
    auto* request = reinterpret_cast<QuitRequest*>(param);
    wchar_t cls[64] = {};
    if (!GetClassNameW(hwnd, cls, 64) || wcscmp(cls, contract::kWindowClass) != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    for (auto& t : *request->targets)
        if (t.pid == pid && PostMessageW(hwnd, request->quit, 0, 0)) t.asked = true;
    return TRUE;
}

/// Ends this installation's fairyfly (closing::processes_to_close): the trays quit as from their menu and get up to
/// kCloseWaitMs; what still runs then (a link stuck in a SAP call, a tray that ignores the request, `fairyfly mcp` for
/// an assistant, which has no window to ask) is terminated, so the installer never leaves fairyfly running from
/// removed files. A Microsoft Store fairyfly.exe or another copy is not touched.
void close_installed(const std::wstring& folder) {
    std::vector<Target> targets;
    for (const auto pid : closing::processes_to_close(running_processes(), folder, GetCurrentProcessId()))
        if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid)) targets.push_back({pid, h, false});
    if (targets.empty()) return;
    QuitRequest request{&targets, RegisterWindowMessageW(contract::kQuitMessage)};
    EnumWindows(&ask_to_quit, reinterpret_cast<LPARAM>(&request));
    const DWORD deadline = GetTickCount() + kCloseWaitMs;
    for (const auto& t : targets) {
        if (!t.asked) continue;
        const DWORD now = GetTickCount();
        WaitForSingleObject(t.process, now < deadline ? deadline - now : 0);
    }
    for (const auto& t : targets) {
        if (WaitForSingleObject(t.process, 0) != WAIT_OBJECT_0) {
            TerminateProcess(t.process, 1);
            WaitForSingleObject(t.process, kTerminateWaitMs);
        }
        CloseHandle(t.process);
    }
}

bool contains_ignoring_case(const std::wstring& text, const std::wstring& part) {
    if (part.empty() || text.size() < part.size()) return false;
    for (size_t i = 0; i + part.size() <= text.size(); ++i)
        if (CompareStringOrdinal(text.c_str() + i, static_cast<int>(part.size()), part.c_str(), static_cast<int>(part.size()),
                                 TRUE) == CSTR_EQUAL)
            return true;
    return false;
}

/// Run values that start a fairyfly in `dir` (fairyfly.exe or fairyfly-tray.exe); others belong to another copy.
void remove_run_values(const std::wstring& dir) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, contract::kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    for (const char* ascii_name : {contract::kLinkRunValue, contract::kMcpRunValue}) {
        const std::wstring name = ascii_wide(ascii_name);
        wchar_t value[4096] = {};
        DWORD size = sizeof(value) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(value), &size) != ERROR_SUCCESS) continue;
        if (contains_ignoring_case(value, dir + L"\\")) RegDeleteValueW(key, name.c_str());
    }
    RegCloseKey(key);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    auto args = arguments();
    // COM starts the toast activator's server with -Embedding: the link tray registers it once it runs (a running tray
    // answers itself; a second start only opens its menu)
    if (args.size() == 1 && (args[0] == L"-Embedding" || args[0] == L"/Embedding")) args.clear();
    const auto command = closing::parse_starter_command(std::move(args), module_dir());
    if (command.mode == closing::StarterMode::Close) {
        close_installed(command.folder);
        return 0;
    }
    if (command.mode == closing::StarterMode::Uninstall) {
        close_installed(command.folder);
        remove_run_values(command.folder);
        RegDeleteTreeW(HKEY_CURRENT_USER, contract::kSettingsKey);
        // what the link registers for its approval notifications
        RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(contract::kAppUserModelIdKeyPrefix) + contract::kAppUserModelId).c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(contract::kClsidKeyPrefix) + contract::kToastActivatorClsid).c_str());
        return 0;
    }
    return start(module_dir(), command.args);
}
