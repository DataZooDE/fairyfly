#pragma once
// What fairyfly-tray.exe does for the installer (packaging/fairyfly.wxs): its command and which processes it closes.
// Header-only and dependency-free like tray_contract.h: the starter is built without fairyfly_core, and the unit
// tests check the same functions.
//
//   fairyfly-tray.exe --close [<folder>]       close fairyfly in <folder> before its files are replaced or removed
//   fairyfly-tray.exe --uninstall [<folder>]   --close, then remove the Run values, settings and registrations
//
// The MSI runs the starter from its own Binary table (a temporary copy), so it names the install folder; without one
// the starter's own folder is meant. Only processes started from that folder are closed: a Microsoft Store (MSIX)
// fairyfly.exe, or another copy, keeps running.
#include <cstdint>
#include <string>
#include <vector>

namespace fairyfly::tray::closing {

enum class StarterMode { Start, Close, Uninstall };

struct StarterCommand {
    StarterMode mode = StarterMode::Start;
    std::wstring folder;              ///< Close/Uninstall: the install folder (no trailing separator)
    std::vector<std::wstring> args;   ///< Start: the arguments for fairyfly.exe
};

/// A path compared by Windows rules: "\\?\" prefix dropped, '/' as '\', ASCII case folded, no trailing '\'.
inline std::wstring comparable_path(std::wstring path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
    for (auto& c : path) {
        if (c == L'/') c = L'\\';
        else if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    while (path.size() > 1 && path.back() == L'\\') path.pop_back();
    return path;
}

inline StarterCommand parse_starter_command(std::vector<std::wstring> args, const std::wstring& own_folder) {
    StarterCommand command;
    if (!args.empty() && args.size() <= 2 && (args[0] == L"--close" || args[0] == L"--uninstall")) {
        command.mode = args[0] == L"--close" ? StarterMode::Close : StarterMode::Uninstall;
        command.folder = args.size() == 2 ? args[1] : own_folder;
        while (command.folder.size() > 1 && (command.folder.back() == L'\\' || command.folder.back() == L'/')) command.folder.pop_back();
        return command;
    }
    command.args = std::move(args);
    return command;
}

/// fairyfly.exe or fairyfly-tray.exe directly in `folder`: what the installer replaces.
inline bool is_installed_program(const std::wstring& image, const std::wstring& folder) {
    if (image.empty() || folder.empty()) return false;
    const std::wstring path = comparable_path(image);
    const std::wstring dir = comparable_path(folder);
    for (const wchar_t* name : {L"fairyfly.exe", L"fairyfly-tray.exe"})
        if (path == dir + L"\\" + name) return true;
    return false;
}

struct ProcessEntry {
    std::uint32_t pid = 0;
    std::wstring image;   ///< as enumerated (may be empty or stale: a pid can be reused at any moment)
};

/// The processes to close before `folder`'s files are replaced: this installation's fairyfly.exe and fairyfly-tray.exe,
/// never `self`. Each enumerated pid is opened ONCE (`open(pid)` -> optional<Process>), its executable is read from that
/// open process (`image_of(process)`), and the processes kept are the ones the caller then waits on and terminates:
/// nothing is reopened by pid, so a pid reused by another program between enumeration and termination is never
/// touched (an open handle keeps its process, not its pid).
template <class Process, class Open, class ImageOf>
std::vector<Process> open_installed(const std::vector<ProcessEntry>& enumerated, const std::wstring& folder, std::uint32_t self,
                                    Open open, ImageOf image_of) {
    std::vector<Process> out;
    for (const auto& e : enumerated) {
        if (e.pid == self || !is_installed_program(e.image, folder)) continue;
        if (auto p = open(e.pid)) out.push_back(std::move(*p));
    }
    (void)image_of;
    return out;
}

inline std::vector<std::uint32_t> processes_to_close(const std::vector<ProcessEntry>& processes, const std::wstring& folder,
                                                     std::uint32_t self) {
    std::vector<std::uint32_t> out;
    for (const auto& p : processes)
        if (p.pid != self && is_installed_program(p.image, folder)) out.push_back(p.pid);
    return out;
}

}  // namespace fairyfly::tray::closing
