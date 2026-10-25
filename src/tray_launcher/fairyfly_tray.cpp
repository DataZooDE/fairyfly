// fairyfly-tray.exe: starts fairyfly's trays without a console window.
//
// fairyfly.exe is a console program, so Explorer (Start menu, autostart) opens a console for it before its tray hides
// itself again: a black window flashes. This small Windows-subsystem program gets no console; it starts fairyfly.exe
// next to it with CREATE_NO_WINDOW and the tray-child markers, and ends. Like pythonw.exe beside python.exe.
//
//   fairyfly-tray.exe                     fairyfly.exe link run --tray (the Start-menu entry)
//   fairyfly-tray.exe <args...>           fairyfly.exe <args...> (the Run values: link run --tray ..., mcp --tray ...)
//   fairyfly-tray.exe --close             asks the running trays to quit and waits for them (the installer, before it
//                                         replaces or removes fairyfly.exe)
//   fairyfly-tray.exe --uninstall         --close, then removes the Run values that start this folder's fairyfly,
//                                         the trays' settings and the notification registration (the installer, on
//                                         uninstall)
//   fairyfly-tray.exe -Embedding          the notification centre starts the link tray to answer an approval
//                                         (COM, the toast activator; src/link/desktop_approval_win32.cpp)
//
// It links nothing of fairyfly's own; the window class and message names match src/tray/tray_win32.cpp.
#include <windows.h>
#include "include/config/environment.h"
#include "include/text/command_line.h"
#include <shellapi.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t* kWindowClass = L"fairyfly_tray_window";
constexpr const wchar_t* kQuitMessage = L"fairyfly-tray-quit";
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kSettingsKey = L"Software\\DataZoo\\fairyfly\\tray";
constexpr const wchar_t* kRunValues[] = {L"fairyfly-link", L"fairyfly-mcp"};
// what the link registers for its approval notifications (src/include/link/desktop_approval_win32.h)
constexpr const wchar_t* kNotificationKeys[] = {L"Software\\Classes\\AppUserModelId\\DataZoo.fairyfly",
                                                L"Software\\Classes\\CLSID\\{DAAF1065-EF17-4CFF-B3D2-EFAF70594959}"};
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
        SetEnvironmentVariableW(std::wstring(name, name + std::strlen(name)).c_str(), L"1");  // ASCII names
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

struct Found {
    std::vector<HANDLE> processes;
    UINT quit = 0;
};

BOOL CALLBACK ask_to_quit(HWND hwnd, LPARAM param) {
    auto* found = reinterpret_cast<Found*>(param);
    wchar_t cls[64] = {};
    if (!GetClassNameW(hwnd, cls, 64) || wcscmp(cls, kWindowClass) != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) found->processes.push_back(h);
    PostMessageW(hwnd, found->quit, 0, 0);
    return TRUE;
}

/// Every fairyfly tray of this user (link and MCP) quits as from its menu; waits until they have ended.
void close_trays() {
    Found found;
    found.quit = RegisterWindowMessageW(kQuitMessage);
    EnumWindows(&ask_to_quit, reinterpret_cast<LPARAM>(&found));
    const DWORD deadline = GetTickCount() + kCloseWaitMs;
    for (HANDLE h : found.processes) {
        const DWORD now = GetTickCount();
        WaitForSingleObject(h, now < deadline ? deadline - now : 0);
        CloseHandle(h);
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
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    for (const wchar_t* name : kRunValues) {
        wchar_t value[4096] = {};
        DWORD size = sizeof(value) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value), &size) != ERROR_SUCCESS) continue;
        if (contains_ignoring_case(value, dir + L"\\")) RegDeleteValueW(key, name);
    }
    RegCloseKey(key);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::wstring dir = module_dir();
    auto args = arguments();
    // COM starts the toast activator's server with -Embedding: the link tray registers it once it runs (a running tray
    // answers itself; a second start only opens its menu)
    if (args.size() == 1 && (args[0] == L"-Embedding" || args[0] == L"/Embedding")) args.clear();
    if (args.size() == 1 && args[0] == L"--close") {
        close_trays();
        return 0;
    }
    if (args.size() == 1 && args[0] == L"--uninstall") {
        close_trays();
        remove_run_values(dir);
        RegDeleteTreeW(HKEY_CURRENT_USER, kSettingsKey);
        for (const wchar_t* key : kNotificationKeys) RegDeleteTreeW(HKEY_CURRENT_USER, key);
        return 0;
    }
    return start(dir, args);
}
