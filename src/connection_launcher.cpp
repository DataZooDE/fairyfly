#include "include/connection_launcher.h"
#include "include/constants.h"
#include "include/com/wrapper.h"
#include "include/com/utf8.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <windows.h>
#include <cwchar>

namespace fairyfly {
namespace sap {

namespace {
std::vector<HWND> sap_gui_security_dialogs() {
    std::vector<HWND> windows;
    EnumWindows([](HWND window, LPARAM state) -> BOOL {
        if (!IsWindowVisible(window)) return TRUE;
        wchar_t title[128] = {};
        wchar_t class_name[64] = {};
        GetWindowTextW(window, title, 128);
        GetClassNameW(window, class_name, 64);
        if (_wcsicmp(title, L"SAP GUI Security") != 0 ||
            wcscmp(class_name, L"#32770") != 0) return TRUE;
        DWORD process_id = 0;
        GetWindowThreadProcessId(window, &process_id);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
        if (!process) return TRUE;
        wchar_t image[MAX_PATH] = {};
        DWORD length = MAX_PATH;
        const bool is_sap_gui = QueryFullProcessImageNameW(process, 0, image, &length) &&
            _wcsicmp(wcsrchr(image, L'\\') ? wcsrchr(image, L'\\') + 1 : image,
                     L"sapgui.exe") == 0;
        CloseHandle(process);
        if (is_sap_gui) reinterpret_cast<std::vector<HWND>*>(state)->push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}
}

ConnectionLauncher::ConnectionLauncher(ComGuiApplicationPtr app,
                                       std::function<bool()> security_prompt_detector)
    : app_(app), security_prompt_detector_(std::move(security_prompt_detector)) {
    if (!security_prompt_detector_) {
        const auto existing = sap_gui_security_dialogs();
        security_prompt_detector_ = [existing] {
            const auto current = sap_gui_security_dialogs();
            return std::any_of(current.begin(), current.end(), [&](HWND window) {
                return std::find(existing.begin(), existing.end(), window) == existing.end();
            });
        };
    }
}

std::wstring ConnectionLauncher::quote_windows_argument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
            backslashes = 0;
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring ConnectionLauncher::sapshcut_command_line(
    const std::wstring& executable_path, const std::string& connection_name) {
    const auto name = com::utf8_to_wide(connection_name);
    // sapshcut parses switches itself and treats a fully quoted switch as a
    // shortcut filename. Quote only the value when Windows parsing requires it.
    const bool needs_quotes = name.find_first_of(L" \t\r\n\"") != std::wstring::npos;
    return quote_windows_argument(executable_path) + L" -sysname=" +
        (needs_quotes ? quote_windows_argument(name) : name) + L" -maxgui";
}

bool ConnectionLauncher::launch_sapshcut(const std::string& connection_name) {
    // Find sapshcut.exe
    std::vector<std::string> possible_paths = {
        "C:\\Program Files\\SAP\\FrontEnd\\SAPgui\\sapshcut.exe",
        "C:\\Program Files (x86)\\SAP\\FrontEnd\\SAPgui\\sapshcut.exe"
    };

    std::string sapshcut_path;
    for (const auto& path : possible_paths) {
        std::ifstream test(path);
        if (test.good()) {
            sapshcut_path = path;
            break;
        }
    }

    if (sapshcut_path.empty()) {
        spdlog::error("sapshcut.exe not found in standard SAP GUI installation paths");
        return false;
    }

    const auto wide_path = com::utf8_to_wide(sapshcut_path);
    const std::wstring cmd = sapshcut_command_line(wide_path, connection_name);

    spdlog::info("Launching sapshcut for connection: {}", connection_name);
    spdlog::debug("sapshcut opening SAP Logon entry without credentials");

    // Launch process using CreateProcess
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // CreateProcessW may modify this command-line buffer.
    std::vector<wchar_t> cmd_buffer(cmd.begin(), cmd.end());
    cmd_buffer.push_back(L'\0');

    BOOL success = CreateProcessW(
        wide_path.c_str(),      // Exact executable, independent of command-line parsing
        cmd_buffer.data(),      // Command line
        NULL,                   // Process security attributes
        NULL,                   // Thread security attributes
        FALSE,                  // Inherit handles
        0,                      // Creation flags
        NULL,                   // Environment
        NULL,                   // Current directory
        &si,                    // Startup info
        &pi                     // Process information
    );

    if (!success) {
        spdlog::error("Failed to launch sapshcut.exe: {}", GetLastError());
        return false;
    }

    // Close process/thread handles (we don't need to wait for sapshcut to exit)
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    spdlog::info("sapshcut.exe launched successfully");
    return true;
}

bool ConnectionLauncher::matches_connection_name(const std::string& requested,
                                                 const std::string& description) {
    auto normalize = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        const auto last = value.find_last_not_of(" \t\r\n");
        value = value.substr(first, last - first + 1);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };
    const auto name = normalize(requested);
    return !name.empty() && name == normalize(description);
}

bool ConnectionLauncher::was_present_before_launch(
    const std::string& previous_id, const std::string& previous_server_key,
    const std::string& candidate_id, const std::string& candidate_server_key,
    bool same_com_object) {
    if (!previous_server_key.empty() && !candidate_server_key.empty())
        return previous_server_key == candidate_server_key;
    if (same_com_object) return true;
    if (previous_server_key.empty() && candidate_server_key.empty())
        return previous_id == candidate_id;
    return false;
}

std::pair<ComGuiConnectionPtr, ComGuiSessionPtr> ConnectionLauncher::wait_for_session(
    const std::string& connection_name,
    int timeout_seconds,
    const std::vector<ExistingSession>& existing_sessions) {
    spdlog::info("Waiting for SAP session creation (timeout: {}s)...", timeout_seconds);
    wait_failure_ = WaitFailure::None;

    auto start = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(timeout_seconds);
    int poll_attempt = 0;

    while (std::chrono::steady_clock::now() - start < timeout) {
        if (security_prompt_detector_()) {
            wait_failure_ = WaitFailure::SecurityPrompt;
            spdlog::warn("SAP GUI Security dialog is blocking the sapshcut launch");
            return {nullptr, nullptr};
        }
        poll_attempt++;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        try {
            // The application object exposes the live connection collection.
            if (!app_) {
                app_ = ComGuiApplication::create();
            }

            int conn_count = app_->get_connection_count();
            spdlog::debug("[Poll #{}] {}s elapsed - Connections: {}", poll_attempt, elapsed, conn_count);

            // Inspect all matches so concurrent launches cannot be chosen arbitrarily.
            std::pair<ComGuiConnectionPtr, ComGuiSessionPtr> candidate;
            for (int i = conn_count - 1; i >= 0; --i) {
                auto conn = app_->get_connection(i);
                if (!conn) continue;
                std::string conn_desc = conn->get_description();
                if (!matches_connection_name(connection_name, conn_desc)) continue;
                int sess_count = conn->get_session_count();

                spdlog::debug("  Connection[{}]: '{}' - {} sessions", i, conn_desc, sess_count);

                for (int s = sess_count - 1; s >= 0; --s) {
                    auto session = conn->get_session(s);
                    if (!session) continue;
                    const auto session_id = session->get_id();
                    const auto server_key = session->get_server_session_key();
                    const bool existed_before_launch = std::any_of(
                        existing_sessions.begin(), existing_sessions.end(),
                        [&](const ExistingSession& existing) {
                            return was_present_before_launch(
                                existing.id, existing.server_key, session_id, server_key,
                                existing.object && *existing.object == *session);
                        });
                    if (existed_before_launch) continue;

                    if (candidate.second) {
                        throw std::runtime_error(
                            "Multiple new sessions match the requested SAP connection; launch result is ambiguous");
                    }
                    candidate = {conn, session};
                }
            }

            if (candidate.second) {
                spdlog::info("Session found after {}s: {}", elapsed, candidate.second->get_id());
                return candidate;
            }

            // No session yet, wait and retry
            if (conn_count == 0) {
                spdlog::debug("  No connections found - sapshcut may have failed or is still starting");
            }

            std::this_thread::sleep_for(constants::Milliseconds(constants::CONNECTION_POLL_INTERVAL_MS));

        } catch (const ComException& e) {
            spdlog::warn("[Poll #{}] Error while polling: {}", poll_attempt, e.what());
            // Continue polling despite errors
            std::this_thread::sleep_for(constants::Milliseconds(constants::CONNECTION_POLL_INTERVAL_MS));
        }
    }

    spdlog::error("✗ Timeout waiting for session creation after {}s ({} poll attempts)",
        timeout_seconds, poll_attempt);
    spdlog::error("  Possible causes:");
    spdlog::error("    1. SAP Logon entry missing or unavailable");
    spdlog::error("    2. SAP system not accessible/responding");
    spdlog::error("    3. sapshcut command parameters incorrect");
    spdlog::error("    4. SAP GUI showing error dialog that needs manual dismissal");

    wait_failure_ = WaitFailure::Timeout;
    return {nullptr, nullptr};
}

} // namespace sap
} // namespace fairyfly

