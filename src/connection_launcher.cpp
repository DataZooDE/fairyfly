#include "include/connection_launcher.h"
#include "include/constants.h"
#include "include/com/wrapper.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <thread>
#include <chrono>
#include <fmt/format.h>
#include <windows.h>

namespace fairyfly {
namespace sap {

ConnectionLauncher::ConnectionLauncher(ComGuiApplicationPtr app) : app_(app) {
}

std::optional<ConnectionLauncher::Credentials> ConnectionLauncher::read_credentials_from_env(
    const std::string& connection_name) {
    (void)connection_name;  // Unused for now - will be used when we support multiple profiles
    std::string env_path = "trial.env";
    std::ifstream file(env_path);

    if (!file.is_open()) {
        spdlog::warn("trial.env not found, credentials unavailable");
        return std::nullopt;
    }

    Credentials creds;
    std::string line;

    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Parse "Key: Value" format
        auto colon_pos = line.find(':');
        if (colon_pos == std::string::npos) continue;

        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);

        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);

        if (key == "Connection") creds.system_id = value;
        else if (key == "Username") creds.username = value;
        else if (key == "Password") creds.password = value;
        else if (key == "System ID") creds.client = value;
        else if (key == "Instance Number") creds.instance = value;
    }

    // Validate required fields
    if (creds.username.empty() || creds.password.empty()) {
        spdlog::error("trial.env missing required credentials (Username/Password)");
        return std::nullopt;
    }

    spdlog::debug("Loaded credentials from trial.env for user: {}", creds.username);
    return creds;
}

bool ConnectionLauncher::launch_sapshcut(const std::string& connection_name, const Credentials& creds) {
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

    // Build command: sapshcut.exe -sysname=X -client=X -user=X -pw=X -language=EN
    // Use -sysname for SAP Logon connection names (not -system which is for system IDs)
    std::string cmd = fmt::format(
        "\"{}\" -sysname=\"{}\" -client={} -user={} -pw=\"{}\" -language=EN -maxgui",
        sapshcut_path,
        connection_name,
        creds.client,
        creds.username,
        creds.password
    );

    spdlog::info("Launching sapshcut for connection: {}", connection_name);
    spdlog::debug("sapshcut command (password redacted)");

    // Launch process using CreateProcess
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Create command buffer (CreateProcess may modify the string)
    std::vector<char> cmd_buffer(cmd.begin(), cmd.end());
    cmd_buffer.push_back('\0');

    BOOL success = CreateProcessA(
        NULL,                   // Application name
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

std::pair<ComGuiConnectionPtr, ComGuiSessionPtr> ConnectionLauncher::wait_for_session(
    const std::string& connection_name, 
    int timeout_seconds) {
    (void)connection_name;  // Unused for now - will be used when we need to match specific connections
    spdlog::info("Waiting for SAP session creation (timeout: {}s)...", timeout_seconds);

    auto start = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(timeout_seconds);
    int poll_attempt = 0;

    while (std::chrono::steady_clock::now() - start < timeout) {
        poll_attempt++;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        try {
            // Recreate app object to get fresh connection list
            if (!app_) {
                app_ = ComGuiApplication::create();
            }

            int conn_count = app_->get_connection_count();
            spdlog::info("[Poll #{}] {}s elapsed - Connections: {}", poll_attempt, elapsed, conn_count);

            // Check each connection for sessions
            for (int i = 0; i < conn_count; ++i) {
                auto conn = app_->get_connection(i);
                std::string conn_desc = conn->get_description();
                int sess_count = conn->get_session_count();

                spdlog::info("  Connection[{}]: '{}' - {} sessions", i, conn_desc, sess_count);

                if (sess_count > 0) {
                    // Found a session!
                    auto connection = conn;
                    auto session = conn->get_session(0);

                    spdlog::info("✓ Session found after {}s: {}",
                        elapsed, session->get_id());

                    return {connection, session};
                }
            }

            // No session yet, wait and retry
            if (conn_count == 0) {
                spdlog::warn("  No connections found - sapshcut may have failed or is still starting");
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
    spdlog::error("    1. Incorrect credentials in trial.env");
    spdlog::error("    2. SAP system not accessible/responding");
    spdlog::error("    3. sapshcut command parameters incorrect");
    spdlog::error("    4. SAP GUI showing error dialog that needs manual dismissal");

    return {nullptr, nullptr};
}

} // namespace sap
} // namespace fairyfly

