#include "include/system/sap_gui_process.h"

#include <algorithm>
#include <cctype>

#include "include/system/unique_handle.h"
#include "include/system/window_owner.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace fairyfly::system {

bool is_sap_gui_image(const std::string& file_name) {
    std::string lower = file_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "saplogon.exe" || lower == "sapgui.exe" || lower == "saplgpad.exe";
}

nlohmann::json probe_json(const SapGuiProbe& probe) {
    nlohmann::json dialogs = nlohmann::json::array();
    for (const auto& d : probe.native_dialogs)
        dialogs.push_back({{"title", d.title}, {"class", d.class_name}, {"process", d.image}, {"pid", d.process_id}});
    nlohmann::json out = {{"sap_gui_processes", probe.process_ids.size()}, {"native_dialogs", dialogs}};
    if (probe.main_window_enabled) out["main_window_enabled"] = *probe.main_window_enabled;
    if (probe.main_window_hung) out["main_window_hung"] = *probe.main_window_hung;
    return out;
}

#ifdef _WIN32
namespace {

std::string narrow(const wchar_t* text) {
    if (!text || !*text) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

struct SapProcess {
    std::uint32_t pid = 0;
    std::string image;
};

/// SAP GUI processes of this logon session, saplogon.exe first.
std::vector<SapProcess> own_sap_processes() {
    std::vector<SapProcess> found;
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
        const std::string image = narrow(entry.szExeFile);
        if (!is_sap_gui_image(image)) continue;
        if (!process_owned_by_current_logon(entry.th32ProcessID)) continue;
        found.push_back({entry.th32ProcessID, image});
    }
    std::stable_partition(found.begin(), found.end(), [](const SapProcess& p) { return _stricmp(p.image.c_str(), "saplogon.exe") == 0; });
    return found;
}

struct DialogScan {
    const std::vector<SapProcess>* processes;
    std::vector<NativeDialog> dialogs;
};

}  // namespace

SapGuiProbe probe_sap_gui(std::uintptr_t main_window) noexcept {
    SapGuiProbe probe;
    try {
        const auto processes = own_sap_processes();
        for (const auto& p : processes) probe.process_ids.push_back(p.pid);
        DialogScan scan{&processes, {}};
        EnumWindows([](HWND window, LPARAM state) -> BOOL {
            auto* scan = reinterpret_cast<DialogScan*>(state);
            if (!IsWindowVisible(window)) return TRUE;
            wchar_t class_name[64] = {};
            GetClassNameW(window, class_name, 64);
            if (wcscmp(class_name, L"#32770") != 0) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            const auto owner = std::find_if(scan->processes->begin(), scan->processes->end(),
                                            [&](const SapProcess& p) { return p.pid == pid; });
            if (owner == scan->processes->end()) return TRUE;
            wchar_t title[256] = {};
            GetWindowTextW(window, title, 256);
            scan->dialogs.push_back({reinterpret_cast<std::uintptr_t>(window), pid, owner->image, narrow(title),
                                     narrow(class_name)});
            return TRUE;
        }, reinterpret_cast<LPARAM>(&scan));
        probe.native_dialogs = std::move(scan.dialogs);
        if (main_window && window_owned_by_current_logon(main_window)) {
            HWND hwnd = reinterpret_cast<HWND>(main_window);
            probe.main_window_enabled = IsWindowEnabled(hwnd) != FALSE;
            probe.main_window_hung = IsHungAppWindow(hwnd) != FALSE;
        }
    } catch (...) {
    }
    return probe;
}

int dismiss_native_dialogs(const std::vector<NativeDialog>& dialogs) noexcept {
    int sent = 0;
    try {
        for (const auto& d : dialogs) {
            HWND window = reinterpret_cast<HWND>(d.window);
            if (!IsWindow(window) || !window_owned_by_current_logon(d.window)) continue;
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid != d.process_id) continue;   // the handle was reused by another window
            if (PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0)) ++sent;
        }
    } catch (...) {
    }
    return sent;
}

TerminateOutcome terminate_sap_gui_processes(std::chrono::milliseconds wait) noexcept {
    TerminateOutcome outcome;
    try {
        for (const auto& p : own_sap_processes()) {
            outcome.found.push_back(p.pid);
            UniqueHandle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid));
            if (!process) {
                // it may have ended with saplogon.exe already
                if (GetLastError() == ERROR_INVALID_PARAMETER) outcome.ended.push_back(p.pid);
                else if (outcome.error.empty()) outcome.error = "cannot open " + p.image + " (pid " + std::to_string(p.pid) + ")";
                continue;
            }
            DWORD code = 0;
            if (GetExitCodeProcess(process.get(), &code) && code != STILL_ACTIVE) {
                outcome.ended.push_back(p.pid);
                continue;
            }
            if (!TerminateProcess(process.get(), 1) && outcome.error.empty())
                outcome.error = "cannot end " + p.image + " (pid " + std::to_string(p.pid) + ")";
            if (WaitForSingleObject(process.get(), static_cast<DWORD>(wait.count())) == WAIT_OBJECT_0)
                outcome.ended.push_back(p.pid);
            else if (outcome.error.empty())
                outcome.error = p.image + " (pid " + std::to_string(p.pid) + ") did not end in time";
        }
    } catch (...) {
        if (outcome.error.empty()) outcome.error = "unexpected failure while ending SAP GUI";
    }
    return outcome;
}

#else
SapGuiProbe probe_sap_gui(std::uintptr_t) noexcept { return {}; }
int dismiss_native_dialogs(const std::vector<NativeDialog>&) noexcept { return 0; }
TerminateOutcome terminate_sap_gui_processes(std::chrono::milliseconds) noexcept { return {}; }
#endif

}  // namespace fairyfly::system
