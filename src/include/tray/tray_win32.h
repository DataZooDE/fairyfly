#pragma once
// Real (Win32) implementations of the tray interfaces. Not used by unit tests; the manual tests
// tagged [.] in tests/unit/test_tray_win32_manual.cpp exercise them on a desktop.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "include/tray/tray.h"

namespace fairyfly::tray {

std::unique_ptr<TrayHost> make_win32_tray_host(TrayKind kind = TrayKind::Mcp);
std::unique_ptr<TraySettings> make_win32_tray_settings();
std::unique_ptr<FileOpener> make_win32_file_opener();
std::unique_ptr<ProcessLauncher> make_win32_process_launcher();
std::unique_ptr<RegistryRunKey> make_win32_run_key();
std::unique_ptr<SingleInstance> make_win32_single_instance(const std::string& name = kTrayMutexName);
/// The running trays of `kind` (their windows, any installation) and their quit message.
std::unique_ptr<TrayInstances> make_win32_tray_instances(TrayKind kind);

/// Full path of the running fairyfly.exe.
std::string current_exe_path();
/// Command-line arguments of this process without the program name (UTF-8).
std::vector<std::string> current_args();
/// True when the process has a console window.
bool process_has_console();
/// Processes attached to this process's console (GetConsoleProcessList); 0 without a console.
unsigned console_process_count();
/// startup_error_needs_dialog() for this process: true when nobody would read the error in a console.
bool startup_error_needs_dialog_here();
/// True when FAIRYFLY_TRAY_CHILD=1 (this is the detached tray process).
bool is_tray_child_process();
/// True when fairyfly-tray.exe started this tray (FAIRYFLY_TRAY_LAUNCHED=1 with FAIRYFLY_TRAY_CHILD=1).
bool is_tray_launched_process();
/// True when `path` is an existing file.
bool path_exists(const std::filesystem::path& path);
/// False only when `path` is known not to exist (file or folder not found); true also when it cannot be checked, so
/// a tray is never taken for a removed installation's by mistake (TrayClaim::exists).
bool file_not_missing(const std::filesystem::path& path);
/// Removes FAIRYFLY_TRAY_CHILD and FAIRYFLY_TRAY_LAUNCHED from this process's environment, so the processes it starts
/// do not inherit them.
void clear_tray_child_marker();
/// Detaches from the console (tray child; no-op without one).
void detach_console();
/// Routes spdlog to <logs>\<file_name> with size rotation (3 files x 5 MB) and returns the file path.
std::string route_logging_to_file(const std::string& file_name = "mcp.log");

/// The start of either tray (`mcp --tray`, `link run --tray`) with the Win32 services: what the tray entry decides on
/// and what the tray keeps using afterwards (the Run key, the launcher, the instance guard).
struct TrayStart {
    TrayEntry entry;                ///< tray_requested, is_child, has_console, exe and args set; the caller adds the rest
    std::string program;            ///< what the Run value starts: fairyfly-tray.exe when installed, else fairyfly.exe
    std::unique_ptr<RegistryRunKey> registry;
    std::unique_ptr<ProcessLauncher> launcher;
    std::unique_ptr<SingleInstance> guard;
    bool launched = false;          ///< after settle_tray_start: the Start menu, the autostart or the installer started it
};
TrayStart prepare_tray_start(const std::string& mutex_name);

/// decide_tray_entry with the Win32 services, a message box where nobody reads the console, and a running tray asked
/// to show itself. nullopt: this process is the tray now (console detached, child marker cleared, `launched` read
/// before that); otherwise the exit code to return.
std::optional<int> settle_tray_start(TrayStart& start, TrayKind kind, const std::string& title);

} // namespace fairyfly::tray
