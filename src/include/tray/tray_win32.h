#pragma once
// Real (Win32) implementations of the tray interfaces. Not used by unit tests; the manual tests
// tagged [.] in tests/unit/test_tray_win32_manual.cpp exercise them on a desktop.

#include <memory>
#include <string>
#include <vector>

#include "include/tray/tray.h"

namespace fairyfly::tray {

std::unique_ptr<TrayHost> make_win32_tray_host();
std::unique_ptr<FileOpener> make_win32_file_opener();
std::unique_ptr<ProcessLauncher> make_win32_process_launcher();
std::unique_ptr<RegistryRunKey> make_win32_run_key();
std::unique_ptr<SingleInstance> make_win32_single_instance(const std::string& name = kTrayMutexName);
std::unique_ptr<Clipboard> make_win32_clipboard();

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
/// Removes FAIRYFLY_TRAY_CHILD from this process's environment, so the processes it starts do not inherit it.
void clear_tray_child_marker();
/// Detaches from the console (tray child; no-op without one).
void detach_console();
/// Routes spdlog to <logs>\<file_name> with size rotation (3 files x 5 MB) and returns the file path.
std::string route_logging_to_file(const std::string& file_name = "mcp.log");

} // namespace fairyfly::tray
