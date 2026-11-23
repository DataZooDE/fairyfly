#pragma once
// Issue #155: the frontend side of a hung SAP GUI. A native (non-scripting) modal dialog owned by saplogon.exe or
// sapgui.exe disables the SAP main window: SendVKey and SendCommand are accepted and do nothing. probe_sap_gui()
// gathers that evidence, dismiss_native_dialogs() answers such dialogs with Cancel, and terminate_sap_gui_processes()
// ends this logon session's SAP GUI processes for a restart. Only processes of the current user's logon session are
// ever touched (process_owned_by_current_logon), never another user's SAP GUI.
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fairyfly::system {

/// saplogon.exe, sapgui.exe and saplgpad.exe (file name only, case-insensitive).
bool is_sap_gui_image(const std::string& file_name);

struct NativeDialog {
    std::uintptr_t window = 0;
    std::uint32_t process_id = 0;
    std::string image;   ///< file name of the owning process, e.g. saplogon.exe
    std::string title;
    std::string class_name;
};

struct SapGuiProbe {
    std::vector<std::uint32_t> process_ids;          ///< SAP GUI processes of this logon session
    std::vector<NativeDialog> native_dialogs;        ///< visible #32770 dialogs they own
    std::optional<bool> main_window_enabled;         ///< IsWindowEnabled of the session's wnd[0] (nullopt: unknown)
    std::optional<bool> main_window_hung;            ///< IsHungAppWindow of wnd[0]
};

/// Never throws. `main_window` is the native handle of the session's main window (0 = unknown).
SapGuiProbe probe_sap_gui(std::uintptr_t main_window) noexcept;
nlohmann::json probe_json(const SapGuiProbe& probe);

/// Sends Cancel (WM_COMMAND IDCANCEL) to each listed dialog; returns how many were still there to receive it.
int dismiss_native_dialogs(const std::vector<NativeDialog>& dialogs) noexcept;

/// Does the SAP GUI frontend of this logon session process window messages? Any thread, no COM: a visible top-level
/// window of saplogon/sapgui that IsHungAppWindow reports, or that does not answer WM_NULL within `answer_within`,
/// means frozen. A frontend that waits for the SAP server still answers.
struct FrontendResponse {
    bool running = false;          ///< a SAP GUI process of this logon session runs
    bool hung = false;
    int windows = 0;               ///< visible top-level windows checked
    int hung_windows = 0;
    int session_windows = 0;       ///< visible SAP GUI session windows (class SAP_FRONTEND_SESSION)
};
FrontendResponse sap_gui_frontend_response(std::chrono::milliseconds answer_within) noexcept;

struct TerminateOutcome {
    std::vector<std::uint32_t> found;   ///< SAP GUI processes of this logon session
    std::vector<std::uint32_t> ended;   ///< of those, ended within the wait
    std::string error;                  ///< "" or why one could not be ended
};

/// Ends this logon session's SAP GUI processes (saplogon.exe first, then sapgui.exe/saplgpad.exe that remain) and waits
/// up to `wait` for each. Processes of other users or logon sessions are never touched.
TerminateOutcome terminate_sap_gui_processes(std::chrono::milliseconds wait) noexcept;

}  // namespace fairyfly::system
