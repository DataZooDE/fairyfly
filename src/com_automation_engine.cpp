#include "include/com_automation_engine.h"
#include "include/trace.h"
#include "include/element_type_registry.h"
#include "include/screen_element_collector.h"
#include "include/table_data_extractor.h"
#include "include/cli_handler.h"
#include "include/string_utils.h"
#include "include/base64.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <regex>
#include <map>
#include <unordered_set>
#include <fmt/format.h>
#ifdef _WIN32
#include <cstdio>
#include <errno.h>
#endif

// CImg for image processing
// Force Windows GDI display (not X11)
#ifdef _WIN32
    #ifndef cimg_display
        #define cimg_display 2  // 2 = Windows GDI display
    #endif
#else
    #ifndef cimg_display
        #define cimg_display 0  // Disable display on non-Windows
    #endif
#endif
#include <CImg.h>

namespace fairyfly {
namespace sap {

using utils::TraceGuard;

ComAutomationEngine::ComAutomationEngine() {
    try {
        // Initialize COM and get SAP GUI application
        app_ = ComGuiApplication::create();
        spdlog::debug("ComAutomationEngine initialized successfully");

        // Check if any connections exist
        if (app_->get_connection_count() > 0) {
            current_connection_ = app_->get_connection(0);
            if (current_connection_ && current_connection_->get_session_count() > 0) {
                current_session_ = current_connection_->get_session(0);
                spdlog::debug("Found existing connection and session");
            }
        }
    } catch (const ComException& e) {
        spdlog::warn("SAP GUI not available: {}", e.what());
        // Non-fatal - can still be initialized, just not connected
    }
}

ComGuiConnectionPtr ComAutomationEngine::ensure_connection() {
    if (!current_connection_) {
        if (!app_ || app_->get_connection_count() == 0) {
            throw ComException("No SAP connections available");
        }
        current_connection_ = app_->get_connection(0);
    }
    return current_connection_;
}

ComGuiSessionPtr ComAutomationEngine::ensure_session() {
    if (!current_session_) {
        auto conn = ensure_connection();
        if (conn->get_session_count() == 0) {
            throw ComException("No sessions available in connection");
        }
        current_session_ = conn->get_session(0);
    }
    return current_session_;
}

// Helper: Read credentials from trial.env file
std::optional<ComAutomationEngine::Credentials> ComAutomationEngine::read_credentials_from_env(const std::string& connection_name) {
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

// Helper: Launch sapshcut.exe with credentials
bool ComAutomationEngine::launch_sapshcut(const std::string& connection_name, const Credentials& creds) {
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

// Helper: Wait for session to be created after sapshcut launch
bool ComAutomationEngine::wait_for_session(const std::string& connection_name, int timeout_seconds) {
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
                    current_connection_ = conn;
                    current_session_ = conn->get_session(0);

                    spdlog::info("✓ Session found after {}s: {}",
                        elapsed, current_session_->get_id());

                    return true;
                }
            }

            // No session yet, wait and retry
            if (conn_count == 0) {
                spdlog::warn("  No connections found - sapshcut may have failed or is still starting");
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));

        } catch (const ComException& e) {
            spdlog::warn("[Poll #{}] Error while polling: {}", poll_attempt, e.what());
            // Continue polling despite errors
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    spdlog::error("✗ Timeout waiting for session creation after {}s ({} poll attempts)",
        timeout_seconds, poll_attempt);
    spdlog::error("  Possible causes:");
    spdlog::error("    1. Incorrect credentials in trial.env");
    spdlog::error("    2. SAP system not accessible/responding");
    spdlog::error("    3. sapshcut command parameters incorrect");
    spdlog::error("    4. SAP GUI showing error dialog that needs manual dismissal");

    return false;
}

Result ComAutomationEngine::attach_by_click(int timeout_seconds) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!app_) {
            app_ = ComGuiApplication::create();
        }

        spdlog::info("Starting window selection (timeout: {}s)", timeout_seconds);

        // Get user to click on SAP window
        HWND selected_hwnd = select_window_by_mouse_click(timeout_seconds);

        spdlog::info("Window selected, searching for matching SAP session...");

        // Find the connection/session that matches this HWND
        SAPGuiWindow window = app_->find_window_by_hwnd(selected_hwnd);

        if (!window.is_valid()) {
            result.status = Result::Status::Error;
            result.error["code"] = "WINDOW_NOT_FOUND";
            result.error["message"] = "Selected window is not an active SAP GUI session";
            result.error["suggestions"] = json::array({
                "Ensure the window you clicked is a SAP transaction window (not just SAP Logon)",
                "The window must have an active SAP session with a transaction open"
            });
            return result;
        }

        // Use found connection and session
        current_connection_ = app_->get_connection(window.connection_idx);
        current_session_ = current_connection_->get_session(window.session_idx);

        result.status = Result::Status::Success;
        result.data["window_title"] = window.title;
        // Normalize SAP paths from 1-indexed to 0-indexed
        result.data["connection_id"] = normalize_sap_path(current_connection_->get_id());
        result.data["session_id"] = normalize_sap_path(current_session_->get_id());

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Attached to SAP session via window click (duration: {}ms)", result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "ATTACH_FAILED";
        result.error["message"] = e.what();
        spdlog::error("Window selection failed: {}", e.what());
    }

    return result;
}

Result ComAutomationEngine::launch_connection(const std::string& connection_name) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;
    json trace = json::array();  // Diagnostic trace

    try {
        trace.push_back({{"step", "initialize"}, {"message", "Starting launch_connection with sapshcut"}});

        spdlog::info("Launching SAP connection using sapshcut: {}", connection_name);
        trace.push_back({{"step", "launch_start"}, {"connection_name", connection_name}});

        // Step 1: Read credentials from trial.env
        auto creds_opt = read_credentials_from_env(connection_name);
        if (!creds_opt.has_value()) {
            throw std::runtime_error("Failed to read credentials from trial.env");
        }

        Credentials creds = creds_opt.value();
        trace.push_back({
            {"step", "read_credentials"},
            {"username", creds.username},
            {"client", creds.client},
            {"status", "success"}
        });

        // Step 2: Launch sapshcut.exe with credentials
        bool launch_success = launch_sapshcut(connection_name, creds);
        trace.push_back({
            {"step", "launch_sapshcut"},
            {"success", launch_success}
        });

        if (!launch_success) {
            throw std::runtime_error("Failed to launch sapshcut.exe");
        }

        // Step 3: Wait for session to be created (polls every 500ms, timeout 10s)
        bool session_created = wait_for_session(connection_name, 10);
        trace.push_back({
            {"step", "wait_for_session"},
            {"success", session_created},
            {"timeout_seconds", 10}
        });

        if (!session_created) {
            result.status = Result::Status::Error;
            result.error["code"] = "SESSION_TIMEOUT";
            result.error["message"] = "Session was not created within timeout period";
            result.error["suggestions"] = json::array({
                "Verify credentials in trial.env are correct",
                "Check that SAP GUI scripting is enabled",
                "Ensure SAP system is accessible and responding",
                "Try increasing timeout or manual login first"
            });
            result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start);
            result.diagnostics["trace"] = trace;
            return result;
        }

        // Step 4: Session found! Capture details
        // Normalize SAP paths from 1-indexed to 0-indexed
        std::string conn_id = current_connection_ ? normalize_sap_path(current_connection_->get_id()) : "";
        std::string sess_id = current_session_ ? normalize_sap_path(current_session_->get_id()) : "";

        trace.push_back({
            {"step", "session_attached"},
            {"connection_id", conn_id},
            {"session_id", sess_id},
            {"status", "success"}
        });

        auto end = std::chrono::high_resolution_clock::now();
        result.status = Result::Status::Success;
        result.data["connection_name"] = connection_name;
        result.data["connection_id"] = conn_id;
        result.data["session_id"] = sess_id;
        result.data["message"] = fmt::format("Launched SAP connection '{}' using sapshcut", connection_name);

        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        result.diagnostics["trace"] = trace;
        result.diagnostics["timing_ms"] = result.duration.count();

        trace.push_back({
            {"step", "complete"},
            {"duration_ms", result.duration.count()}
        });

        spdlog::info("Successfully launched SAP connection '{}' (duration: {}ms)", connection_name, result.duration.count());

    } catch (const std::exception& e) {
        auto end = std::chrono::high_resolution_clock::now();

        trace.push_back({
            {"step", "exception"},
            {"type", "std::exception"},
            {"message", e.what()}
        });

        result.status = Result::Status::Error;
        result.error["code"] = "LAUNCH_FAILED";
        result.error["message"] = e.what();
        result.error["suggestions"] = json::array({
            "Ensure trial.env file exists with valid credentials",
            "Verify sapshcut.exe is installed (part of SAP GUI)",
            "Check SAP GUI scripting is enabled (Options → Accessibility & Scripting)",
            fmt::format("Verify connection '{}' is configured in SAP Logon", connection_name)
        });
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        result.diagnostics["trace"] = trace;
        result.diagnostics["timing_ms"] = result.duration.count();

        spdlog::error("Launch connection failed: {}", e.what());
    }

    return result;
}

Result ComAutomationEngine::disconnect() {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        current_session_ = nullptr;
        current_connection_ = nullptr;
        // Note: app_ is kept alive for potential reconnection

        result.status = Result::Status::Success;
        result.data["message"] = "Disconnected from SAP";

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Disconnected from SAP");

    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

bool ComAutomationEngine::is_connected() const {
    return current_session_ != nullptr && current_session_->is_alive();
}

bool ComAutomationEngine::validate_session(const std::string& session_id) const {
    TraceGuard trace("ComAutomationEngine::validate_session");
    (void)trace;
    spdlog::debug("validate_session: session_id={}", session_id);

    if (!app_) {
        spdlog::debug("Cannot validate session: no application");
        return false;
    }

    try {
        // Parse session_id (e.g., "/app/con[0]/ses[0]")
        // Extract connection index and session index
        int conn_idx = 0;
        int sess_idx = 0;

        size_t con_pos = session_id.find("/con[");
        size_t ses_pos = session_id.find("/ses[");

        if (con_pos != std::string::npos) {
            size_t bracket_end = session_id.find("]", con_pos);
            if (bracket_end != std::string::npos) {
                std::string conn_str = session_id.substr(con_pos + 5, bracket_end - con_pos - 5);
                conn_idx = std::stoi(conn_str);
            }
        }

        if (ses_pos != std::string::npos) {
            size_t bracket_end = session_id.find("]", ses_pos);
            if (bracket_end != std::string::npos) {
                std::string sess_str = session_id.substr(ses_pos + 5, bracket_end - ses_pos - 5);
                sess_idx = std::stoi(sess_str);
            }
        }

        // Get the connection
        auto conn = app_->get_connection(conn_idx);
        if (!conn) {
            spdlog::debug("Connection {} not found", conn_idx);
            return false;
        }

        // Get the session
        auto sess = conn->get_session(sess_idx);
        if (!sess) {
            spdlog::debug("Session {} not found in connection {}", sess_idx, conn_idx);
            return false;
        }

        // Check if session is alive
        bool alive = sess->is_alive();
        spdlog::debug("Session {} validation: {}", session_id, alive ? "valid" : "invalid");
        return alive;

    } catch (const std::exception& e) {
        spdlog::debug("Exception validating session {}: {}", session_id, e.what());
        return false;
    }
}

Result ComAutomationEngine::execute_transaction(const std::string& tcode) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        auto session = ensure_session();
        session->start_transaction(tcode);

        // Wait briefly for transaction to start
        session->wait_for_completion(500);

        result.status = Result::Status::Success;
        result.data["tcode"] = tcode;
        result.data["message"] = "Transaction started";

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Executed transaction {} (duration: {}ms)", tcode, result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "TRANSACTION_FAILED";
        result.error["message"] = e.what();
        result.error["tcode"] = tcode;
        spdlog::error("Transaction {} failed: {}", tcode, e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

Result ComAutomationEngine::click_element(const ElementId& element) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!element.is_valid()) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid element ID format";
            result.error["element"] = element.path;
            return result;
        }

        auto session = ensure_session();
        auto elem = session->find_element_by_id(element.path);

        if (!elem) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "Element not found at path: " + element.path;
            result.error["element"] = element.path;
            result.error["suggestions"] = json::array({
                "Verify element path is correct",
                "Run 'screen read' to see available elements",
                "Check if screen is fully loaded"
            });
            spdlog::warn("Element not found: {}", element.path);
            return result;
        }

        if (!elem->is_enabled()) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_DISABLED";
            result.error["message"] = "Element is disabled and cannot be clicked";
            result.error["element"] = element.path;
            return result;
        }

        // Press the button element
        elem->press();

        result.status = Result::Status::Success;
        result.data["element"] = element.path;
        result.data["action"] = "click";
        result.data["element_type"] = elem->get_type();

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Clicked element {} (duration: {}ms)", element.path, result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        result.error["element"] = element.path;
        spdlog::error("Click failed for {}: {}", element.path, e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

Result ComAutomationEngine::fill_field(const ElementId& element, const std::string& value) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!element.is_valid()) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid element ID format";
            return result;
        }

        auto session = ensure_session();
        auto elem = session->find_element_by_id(element.path);

        if (!elem) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "Element not found at path: " + element.path;
            result.error["element"] = element.path;
            return result;
        }

        if (!elem->is_enabled()) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_DISABLED";
            result.error["message"] = "Element is disabled";
            return result;
        }

        // Set the text
        elem->set_text(value);

        result.status = Result::Status::Success;
        result.data["element"] = element.path;
        result.data["value"] = value;
        result.data["action"] = "fill";

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Filled field {} (duration: {}ms)", element.path, result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        spdlog::error("Fill failed for {}: {}", element.path, e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

Result ComAutomationEngine::read_field(const ElementId& element) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!element.is_valid()) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid element ID format";
            return result;
        }

        auto session = ensure_session();
        auto elem = session->find_element_by_id(element.path);

        if (!elem) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "Element not found";
            result.error["element"] = element.path;
            return result;
        }

        std::string text = elem->get_text();
        result.status = Result::Status::Success;
        result.data["element"] = element.path;
        result.data["value"] = text;
        result.data["element_type"] = elem->get_type();

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Read field {} (duration: {}ms)", element.path, result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        spdlog::error("Read failed for {}: {}", element.path, e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

// Note: Element type checking logic moved to ElementTypeRegistry
// These functions remain for now as thin wrappers during migration
static bool is_visual_component(const std::string& type) {
    return ElementTypeRegistry::instance().supports_visual_properties(type);
}

static bool is_non_visual_container(const std::string& type) {
    return !ElementTypeRegistry::instance().supports_accessibility(type);
}

// Helper: Derive element capabilities from type
static json derive_capabilities(const std::string& type, bool enabled, bool changeable) {
    auto caps = ElementTypeRegistry::instance().derive_capabilities(type, enabled, changeable);

    json capabilities = json::array();
    for (const auto& cap : caps) {
        capabilities.push_back(cap);
    }

    return capabilities;
}

// Cache for tree/grid extraction results to avoid duplicate extractions
// Key: element ID, Value: extracted table/tree data JSON
static std::map<std::string, json> tree_grid_cache;

// Helper: Clear the tree/grid extraction cache
static void clear_extraction_cache() {
    tree_grid_cache.clear();
}

// Helper: Extract rich metadata from element
static json extract_element_metadata(ComGuiElementPtr elem, int depth = 0) {
    if (!elem) return nullptr;

    try {
        json metadata;
        std::string elem_id = elem->get_id();
        metadata["id"] = elem_id;
        std::string type = elem->get_type();
        metadata["type"] = type;
        metadata["name"] = elem->get_name();

        // Text content (available on most elements)
        std::string text = elem->get_text();
        metadata["text"] = text;

        // Interactive states - only query for visual components
        bool enabled = false;
        bool visible = true;
        bool changeable = false;

        if (is_visual_component(type)) {
            enabled = elem->is_enabled();
            visible = elem->is_visible();
            changeable = elem->is_changeable();

            metadata["enabled"] = enabled;
            metadata["visible"] = visible;
            metadata["changeable"] = changeable;
        }

        // Accessibility labels and tooltips - not available on non-visual containers
        if (!is_non_visual_container(type)) {
            std::string label = elem->get_label();
            std::string tooltip = elem->get_tooltip();

            if (!label.empty()) metadata["label"] = label;
            if (!tooltip.empty()) metadata["tooltip"] = tooltip;
        }

        // Container classification
        std::string container_type = elem->get_container_type();
        if (!container_type.empty()) {
            metadata["container_type"] = container_type;
        }

        // SubType for GuiShell elements (GridView, Tree, Toolbar, etc.)
        if (type == "GuiShell") {
            std::string subtype = elem->get_subtype();
            if (!subtype.empty()) {
                metadata["subtype"] = subtype;
            } else {
                metadata["subtype"] = "N/A";
            }
        }

        // Special handling for GuiBox (grouping container)
        if (type == "GuiBox") {
            metadata["is_group"] = true;
        }

        // Derive capabilities
        metadata["capabilities"] = derive_capabilities(type, enabled, changeable);

        // Extract table/tree data for specialized controls
        // Also check if GuiShell contains a tree control based on text (ActiveX ProgID)
        bool is_tree_control = (type == "GuiTree" ||
                               (type == "GuiShell" && text.find("TableTreeControl") != std::string::npos) ||
                               (type == "GuiShell" && text.find("TreeControl") != std::string::npos));

        // Check if GuiShell has SubType=GridView
        std::string subtype = (type == "GuiShell") ? metadata.value("subtype", "") : "";
        bool is_gridview = (type == "GuiGridView" || (type == "GuiShell" && subtype == "GridView"));

        if (is_gridview || type == "GuiTableControl" || is_tree_control) {
            // Check cache first to avoid duplicate expensive extractions
            auto cache_it = tree_grid_cache.find(elem_id);
            if (cache_it != tree_grid_cache.end()) {
                // Reuse cached extraction results
                if (cache_it->second.contains("table_data")) {
                    metadata["table_data"] = cache_it->second["table_data"];
                }
                if (cache_it->second.contains("tree_data")) {
                    metadata["tree_data"] = cache_it->second["tree_data"];
                }
                spdlog::debug("Reused cached extraction data for element: {}", elem_id);
            } else {
                // Extract and cache the results
                try {
                    TableExtractionOptions options;
                    options.max_rows = 20;  // Limit to first 20 rows
                    options.max_tree_depth = 10;
                    options.include_headers = true;

                    TableDataExtractor extractor(options);
                    json cached_data;

                    if (is_gridview) {
                        auto grid_data = extractor.extract_grid_data(elem);
                        if (!grid_data.rows.empty() || !grid_data.columns.empty()) {
                            json table_json;
                            table_json["columns"] = grid_data.columns;
                            table_json["rows"] = grid_data.rows;
                            table_json["total_row_count"] = grid_data.total_row_count;
                            table_json["visible_row_count"] = grid_data.visible_row_count;
                            metadata["table_data"] = table_json;
                            cached_data["table_data"] = table_json;
                            spdlog::debug("Extracted grid data: {} rows × {} columns",
                                         grid_data.rows.size(), grid_data.columns.size());
                        }
                    } else if (type == "GuiTableControl") {
                        auto table_data = extractor.extract_table_data(elem);
                        if (table_data.total_row_count > 0) {
                            json table_json;
                            table_json["total_row_count"] = table_data.total_row_count;
                            metadata["table_data"] = table_json;
                            cached_data["table_data"] = table_json;
                        }
                    } else if (is_tree_control) {
                        auto tree_data = extractor.extract_tree_data(elem);
                        if (!tree_data.nodes.empty()) {
                            json tree_json;
                            tree_json["columns"] = tree_data.columns;

                            // Convert tree nodes to JSON
                            json nodes_array = json::array();
                            std::function<void(const TreeNode&, json&)> convert_node;
                            convert_node = [&](const TreeNode& node, json& node_json) {
                                node_json["text"] = node.text;
                                node_json["key"] = node.key;
                                node_json["level"] = node.level;
                                node_json["expanded"] = node.expanded;
                                if (!node.column_values.empty()) {
                                    node_json["column_values"] = node.column_values;
                                }
                                if (!node.children.empty()) {
                                    json children_array = json::array();
                                    for (const auto& child : node.children) {
                                        json child_json;
                                        convert_node(child, child_json);
                                        children_array.push_back(child_json);
                                    }
                                    node_json["children"] = children_array;
                                }
                            };

                            for (const auto& node : tree_data.nodes) {
                                json node_json;
                                convert_node(node, node_json);
                                nodes_array.push_back(node_json);
                            }

                            tree_json["nodes"] = nodes_array;
                            metadata["tree_data"] = tree_json;
                            cached_data["tree_data"] = tree_json;
                            spdlog::info("Extracted tree data: {} top-level nodes, {} columns",
                                        tree_data.nodes.size(), tree_data.columns.size());
                        }
                    }

                    // Cache the extraction results
                    if (!cached_data.empty()) {
                        tree_grid_cache[elem_id] = cached_data;
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to extract table/tree data for {}: {}", type, e.what());
                }
            }
        }

        // Recursively process children (limit depth to 15 levels to capture deeply nested tree controls)
        // SM59 tree structure: Window -> UserArea -> CustomControl -> ContainerShell -> SplitterShell -> ContainerShell[n] -> Tree
        // SEGW tree structure: Window -> UserArea -> ContainerShell -> SplitterShell -> ContainerShell -> ContainerShell -> SplitterShell -> ContainerShell[n] -> Tree
        // SEGW GridView is at depth 12: usr -> shellcont -> shell -> shellcont[1] -> shell -> shellcont[0] -> shell -> shellcont[0] -> shellcont -> shellcont -> shell -> shellcont[1] -> shell (GridView)
        if (depth < 15) {
            int child_count = elem->get_child_count();

            // Special handling for GuiContainerShell - always try to enumerate children
            // even if get_child_count() returns 0, because SAP GUI sometimes reports 0
            // for containers that actually have shell controls
            bool force_enumerate = (type == "GuiContainerShell" || type == "GuiCustomControl" ||
                                   type == "GuiSplitterContainer" || type == "GuiContainerCtrl" ||
                                   type == "GuiSplitterShell" || type == "GuiDockShell");

            if (child_count > 0 || force_enumerate) {
                spdlog::debug("extract_element_metadata: Processing {} children of {} at depth {} (force={})",
                             child_count, elem->get_id(), depth, force_enumerate);
                json children = json::array();

                try {
                    // Use the .item(index) method which works with SAP GUI collections
                    auto children_collection = elem->children();

                    // For forced enumeration, try up to 10 items even if child_count is 0
                    int max_children = force_enumerate ?
                        (child_count > 0 ? (std::min)(child_count, 50) : 10) :
                        (std::min)(child_count, 50);

                    for (int i = 0; i < max_children; ++i) {
                        try {
                            ComGuiElementPtr child_ptr = children_collection.item(i);
                            if (!child_ptr) {
                                // If we're force-enumerating and hit null, stop trying
                                if (force_enumerate && child_count == 0) {
                                    spdlog::debug("Force enumerate: Reached end at index {}", i);
                                    break;
                                }
                                spdlog::debug("extract_element_metadata: child {} returned nullptr", i);
                                continue;
                            }

                            json child_data = extract_element_metadata(child_ptr, depth + 1);
                            if (!child_data.is_null()) {
                                children.push_back(child_data);
                            }
                        } catch (const std::exception& e) {
                            // If we're force-enumerating and hit exception, stop trying
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: Exception at index {}, stopping: {}", i, e.what());
                                break;
                            }
                            spdlog::debug("extract_element_metadata: Exception processing child {}: {}", i, e.what());
                        } catch (...) {
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: Unknown exception at index {}, stopping", i);
                                break;
                            }
                            spdlog::debug("extract_element_metadata: Unknown exception processing child {}", i);
                        }
                    }
                } catch (const std::exception& e) {
                    spdlog::debug("extract_element_metadata: Exception getting children collection: {}", e.what());
                }

                if (!children.empty()) {
                    metadata["children"] = children;
                    metadata["child_count"] = children.size();
                } else if (child_count > 0) {
                    spdlog::debug("extract_element_metadata: No children extracted for {} (had {} children)",
                                 elem->get_id(), child_count);
                }
            }
        }

        return metadata;
    } catch (const std::exception& e) {
        spdlog::debug("extract_element_metadata: Exception extracting metadata: {}", e.what());
        return nullptr;
    } catch (...) {
        spdlog::debug("extract_element_metadata: Unknown exception extracting metadata");
        return nullptr;
    }
}

// Helper: Recursively flatten and group elements by container type
static void flatten_and_group_elements(const json& elements, json& grouped) {
    for (const auto& elem : elements) {
        std::string type = elem.value("type", "");
        std::string container = elem.value("container_type", "");

        // Group by container type or element type
        if (container == "toolbar" || type == "GuiToolbar" || type == "GuiMenubar") {
            grouped["toolbar"].push_back(elem);
        } else if (type == "GuiButton") {
            grouped["buttons"].push_back(elem);
        } else if (type == "GuiTextField" || type == "GuiCTextField" ||
                   type == "GuiPasswordField" || type == "GuiComboBox" ||
                   type == "GuiCheckBox" || type == "GuiRadioButton") {
            grouped["form_fields"].push_back(elem);
        } else if (type == "GuiLabel") {
            // Labels go to form_fields if they're next to input fields
            grouped["form_fields"].push_back(elem);
        } else if (container == "table" || container == "grid" ||
                   type == "GuiTableControl" || type == "GuiGridView" ||
                   (type == "GuiShell" && elem.value("subtype", "") == "GridView")) {
            grouped["tables"].push_back(elem);
        } else if (container == "tabs" || type == "GuiTabStrip" || type == "GuiTab") {
            grouped["tabs"].push_back(elem);
        } else {
            grouped["other"].push_back(elem);
        }

        // Recursively process children
        if (elem.contains("children") && elem["children"].is_array()) {
            flatten_and_group_elements(elem["children"], grouped);
        }
    }
}

static json group_elements_by_container(const json& elements) {
    json grouped;
    grouped["toolbar"] = json::array();
    grouped["buttons"] = json::array();
    grouped["form_fields"] = json::array();
    grouped["tables"] = json::array();
    grouped["tabs"] = json::array();
    grouped["other"] = json::array();

    flatten_and_group_elements(elements, grouped);

    // Deduplicate all groups by element ID
    auto deduplicate = [](json& arr) {
        std::unordered_set<std::string> seen_ids;
        json deduped = json::array();
        for (const auto& elem : arr) {
            std::string id = elem.value("id", "");
            if (!id.empty() && seen_ids.insert(id).second) {
                deduped.push_back(elem);
            }
        }
        arr = deduped;
    };

    deduplicate(grouped["toolbar"]);
    deduplicate(grouped["buttons"]);
    deduplicate(grouped["form_fields"]);
    deduplicate(grouped["tables"]);
    deduplicate(grouped["tabs"]);
    deduplicate(grouped["other"]);

    // Remove empty groups
    if (grouped["toolbar"].empty()) grouped.erase("toolbar");
    if (grouped["buttons"].empty()) grouped.erase("buttons");
    if (grouped["form_fields"].empty()) grouped.erase("form_fields");
    if (grouped["tables"].empty()) grouped.erase("tables");
    if (grouped["tabs"].empty()) grouped.erase("tabs");
    if (grouped["other"].empty()) grouped.erase("other");

    return grouped;
}

/// Recursively traverse element tree and collect all elements
/// Handles containers that don't expose Children collection by trying ID patterns
/// @param element The element to traverse
/// @param collector The collector to add elements to
/// @param session Session for FindById lookups
/// @param depth Current recursion depth (for safety limits)
static void traverse_element_tree(
    ComGuiElementPtr element,
    ScreenElementCollector& collector,
    const std::shared_ptr<ComGuiSession>& session,
    int depth = 0
) {
    const int MAX_DEPTH = 15;  // Limit depth (SEGW needs depth 10+)

    if (!element || depth >= MAX_DEPTH) {
        return;
    }

    try {
        std::string type = element->get_type();
        std::string elem_id = element->get_id();

        // Add current element to collector (with O(1) deduplication)
        // If already seen, skip processing to avoid duplicate work
        if (!collector.add(element)) {
            return;  // Already processed this element
        }

        spdlog::debug("{}[depth={}] {} ({})",
                     std::string(depth * 2, ' '), depth, elem_id, type);

        // Container types that have nested structure
        bool is_container = (type == "GuiContainerShell" ||
                           type == "GuiSplitterShell" ||
                           type == "GuiUserArea" ||
                           type == "GuiCustomControl");

        if (!is_container) {
            return;  // Leaf element, no children to traverse
        }

        // Try Children collection first, but track if it actually works
        bool children_collection_worked = false;
        try {
            int child_count = element->get_child_count();
            if (child_count > 0) {
                for (int i = 0; i < child_count; ++i) {
                    try {
                        auto child = element->get_child(i);
                        if (child) {
                            traverse_element_tree(child, collector, session, depth + 1);
                            children_collection_worked = true;
                        }
                    } catch (...) {
                        // Child access failed, will fall back to ID-based
                    }
                }
                // Don't return here - continue with ID-based discovery
                // SAP GUI can have elements accessible via FindById but not Children collection
            }
        } catch (...) {
            // Children collection not available
        }

        spdlog::debug("{}[depth={}] Trying ID-based discovery for {}",
                      std::string(depth * 2, ' '), depth, elem_id);

        // For ALL container types (GuiUserArea, GuiContainerShell, GuiSplitterShell),
        // try common child patterns via FindById
        if (is_container) {
            // Try /shell child
            try {
                auto shell_child = session->find_element_by_id(elem_id + "/shell");
                if (shell_child) {
                    traverse_element_tree(shell_child, collector, session, depth + 1);
                }
            } catch (...) { }

            // Try /shellcont[N] children (up to 4)
            for (int i = 0; i < 4; ++i) {
                try {
                    std::string child_id = elem_id + "/shellcont[" + std::to_string(i) + "]";
                    auto child = session->find_element_by_id(child_id);
                    if (child) {
                        traverse_element_tree(child, collector, session, depth + 1);
                    }
                } catch (...) {
                    break;  // No more shellcont children
                }
            }

            // Try /shellcont child (no index)
            try {
                auto shellcont_child = session->find_element_by_id(elem_id + "/shellcont");
                if (shellcont_child) {
                    traverse_element_tree(shellcont_child, collector, session, depth + 1);
                }
            } catch (...) { }
        }
    } catch (...) {
        // Skip elements that throw exceptions during processing
    }
}

/// Discover all UI elements from a window using O(1) deduplication with recursive traversal
/// @param window The SAP GUI window to scan
/// @param session The session for element discovery by ID
/// @return Vector of unique ComGuiElementPtr objects
static std::vector<ComGuiElementPtr> discover_elements(
    const std::shared_ptr<ComGuiWindow>& window,
    const std::shared_ptr<ComGuiSession>& session
) {
    ScreenElementCollector collector;
    collector.reserve(200);  // Reserve space for typical complex screens

    try {
        int child_count = window->get_child_count();
        spdlog::debug("Window has {} top-level children, starting recursive traversal", child_count);

        // Recursively traverse all top-level children
        for (int i = 0; i < child_count; ++i) {
            try {
                auto child = window->get_child(i);
                if (child) {
                    traverse_element_tree(child, collector, session, 0);
                }
            } catch (...) {
                // Skip problematic children
            }
        }
    } catch (...) {
        spdlog::warn("Exception during element tree traversal");
    }

    spdlog::info("Discovered {} unique elements via recursive traversal", collector.elements().size());
    return collector.elements();
}

Result ComAutomationEngine::read_screen(bool include_structure) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        auto session = ensure_session();
        auto window = session->get_active_window();

        if (!window) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_WINDOW";
            result.error["message"] = "No active window found";
            return result;
        }

        result.status = Result::Status::Success;
        result.data["screen_id"] = window->get_id();
        result.data["title"] = window->get_title();
        result.data["transaction"] = session->get_transaction_code();
        result.data["child_count"] = window->get_child_count();

        if (include_structure) {
            // Clear extraction cache at start of each screen read
            clear_extraction_cache();

            // Discover all unique elements (O(1) deduplication)
            auto element_objects = discover_elements(window, session);

            // Extract metadata for each element
            json elements = json::array();
            for (const auto& elem : element_objects) {
                try {
                    json elem_data = extract_element_metadata(elem);
                    if (!elem_data.is_null()) {
                        elements.push_back(elem_data);
                    }
                } catch (...) {
                    // Skip elements with metadata extraction errors
                }
            }

            result.data["elements"] = elements;
            result.data["element_count"] = elements.size();

            // Group elements by type for better LLM understanding
            result.data["hierarchy"] = group_elements_by_container(elements);
        }

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Read screen with {} elements (duration: {}ms)",
                     result.data.value("element_count", 0), result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        spdlog::error("Screen read failed: {}", e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

Result ComAutomationEngine::read_screen_with_tabs() {
    TraceGuard trace("ComAutomationEngine::read_screen_with_tabs");
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        // Get initial screen structure
        Result initial_result = read_screen(true);
        if (initial_result.status != Result::Status::Success) {
            return initial_result;
        }

        json screen_data = initial_result.data;
        json tabs_content = json::array();

        // Find all GuiTab elements in the hierarchy
        std::vector<json> tab_elements;
        if (screen_data.contains("hierarchy") && screen_data["hierarchy"].contains("tabs")) {
            for (const auto& tab : screen_data["hierarchy"]["tabs"]) {
                std::string type = tab.value("type", "");
                if (type == "GuiTab") {
                    tab_elements.push_back(tab);
                }
            }
        }

        if (tab_elements.empty()) {
            spdlog::info("No tabs found in screen, returning normal screen read");
            return initial_result;
        }

        spdlog::info("Found {} tabs to expand", tab_elements.size());

        auto session = ensure_session();

        // For each tab, select it and capture content
        for (size_t i = 0; i < tab_elements.size(); ++i) {
            const auto& tab = tab_elements[i];
            std::string tab_id = tab.value("id", "");
            std::string tab_name = tab.value("text", "Unnamed Tab");

            try {
                spdlog::debug("Selecting tab {}/{}: {} [{}]", i + 1, tab_elements.size(), tab_name, tab_id);

                // Find and select the tab
                auto tab_elem = session->find_element_by_id(tab_id);
                if (!tab_elem) {
                    spdlog::warn("Tab element not found: {}", tab_id);
                    continue;
                }

                // Select the tab (triggers server communication)
                tab_elem->select();

                // Wait for session to be ready (server response)
                int wait_attempts = 0;
                const int max_wait_ms = 5000;
                const int poll_interval_ms = 100;
                while (session->is_busy() && wait_attempts < (max_wait_ms / poll_interval_ms)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
                    wait_attempts++;
                }

                if (session->is_busy()) {
                    spdlog::warn("Timeout waiting for tab {} to load", tab_name);
                    continue;
                }

                // Small additional delay to ensure content is loaded
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                // Re-read screen to get tab content
                Result tab_result = read_screen(true);
                if (tab_result.status != Result::Status::Success) {
                    spdlog::warn("Failed to read content for tab {}: {}",
                                   tab_name, tab_result.error.value("message", "unknown error"));
                    continue;
                }

                // Store tab data
                json tab_data;
                tab_data["tab_id"] = tab_id;
                tab_data["tab_name"] = tab_name;
                tab_data["tab_type"] = tab["type"];
                tab_data["tab_index"] = i;
                tab_data["elements"] = tab_result.data["elements"];
                tab_data["hierarchy"] = tab_result.data["hierarchy"];
                tab_data["element_count"] = tab_result.data.value("element_count", 0);

                tabs_content.push_back(tab_data);

                int elem_count = tab_data["element_count"].get<int>();
                spdlog::info("Captured tab {} with {} elements", tab_name, elem_count);

            } catch (const ComException& e) {
                spdlog::warn("Failed to expand tab {}: {}", tab_name, e.what());
                continue;
            } catch (const std::exception& e) {
                spdlog::warn("Exception expanding tab {}: {}", tab_name, e.what());
                continue;
            }
        }

        // Add tabs content to result
        screen_data["tabs_content"] = tabs_content;
        screen_data["tabs_expanded"] = true;
        screen_data["expanded_tab_count"] = tabs_content.size();

        result.status = Result::Status::Success;
        result.data = screen_data;

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Read screen with {} tabs expanded (duration: {}ms)",
                    tabs_content.size(), result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        spdlog::error("Screen read with tabs failed: {}", e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
        spdlog::error("Exception in read_screen_with_tabs: {}", e.what());
    }

    return result;
}

// Helper: Extract SAFEARRAY bytes from VARIANT
static std::vector<uint8_t> extract_safearray_bytes(VARIANT& var) {
    if (var.vt != (VT_ARRAY | VT_UI1)) {
        throw std::runtime_error("Expected byte array (VT_ARRAY | VT_UI1) from HardCopyToMemory");
    }

    SAFEARRAY* psa = var.parray;
    if (!psa) {
        throw std::runtime_error("SAFEARRAY pointer is null");
    }

    BYTE* data = nullptr;
    HRESULT hr = SafeArrayAccessData(psa, reinterpret_cast<void**>(&data));
    if (FAILED(hr)) {
        throw std::runtime_error(fmt::format("SafeArrayAccessData failed: 0x{:08X}", hr));
    }

    long size = psa->rgsabound[0].cElements;
    std::vector<uint8_t> result(data, data + size);

    SafeArrayUnaccessData(psa);
    return result;
}

// Helper: Parse scale parameter (float for relative, int for absolute width)
static void parse_scale_parameter(const std::string& scale, int orig_width, int orig_height,
                                  int& new_width, int& new_height) {
    if (scale.find('.') != std::string::npos) {
        // Float: relative scale (0.5 = 50%)
        float factor = std::stof(scale);
        if (factor <= 0.0f || factor > 10.0f) {
            throw std::invalid_argument("Scale factor must be between 0.0 and 10.0");
        }
        new_width = static_cast<int>(orig_width * factor);
        new_height = static_cast<int>(orig_height * factor);
    } else {
        // Integer: target width (maintain aspect ratio)
        new_width = std::stoi(scale);
        if (new_width <= 0) {
            throw std::invalid_argument("Width must be positive");
        }
        new_height = static_cast<int>(orig_height * (new_width / static_cast<float>(orig_width)));
    }
}

// Helper: Validate subsection parameters (all-or-nothing)
static void validate_subsection_complete(const cli::ScreenshotOptions& opts) {
    bool has_any = opts.crop_x.has_value() || opts.crop_y.has_value() ||
                   opts.crop_width.has_value() || opts.crop_height.has_value();
    bool has_all = opts.crop_x.has_value() && opts.crop_y.has_value() &&
                   opts.crop_width.has_value() && opts.crop_height.has_value();

    if (has_any && !has_all) {
        throw std::invalid_argument(
            "All subsection parameters (--x, --y, --width, --height) must be provided together");
    }

    if (has_all) {
        if (opts.crop_x.value() < 0 || opts.crop_y.value() < 0) {
            throw std::invalid_argument("Subsection x and y must be non-negative");
        }
        if (opts.crop_width.value() <= 0 || opts.crop_height.value() <= 0) {
            throw std::invalid_argument("Subsection width and height must be positive");
        }
    }
}

Result ComAutomationEngine::capture_screenshot(const cli::ScreenshotOptions& options) {
    TraceGuard trace("ComAutomationEngine::capture_screenshot");
    auto start = std::chrono::high_resolution_clock::now();

    Result result;

    try {
        if (!current_session_) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_SESSION";
            result.error["message"] = "No active SAP session";
            return result;
        }

        // Validate subsection parameters
        validate_subsection_complete(options);

        // Get active window
        auto window = current_session_->get_active_window();
        if (!window) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_WINDOW";
            result.error["message"] = "No active window found";
            return result;
        }

        // Get window title for filename generation
        std::string window_title = window->get_title();

        // Determine if we need CImg processing
        bool needs_processing = options.show ||
                               !options.scale.empty() ||
                               options.format == "base64" ||
                               options.output_file == "-" ||
                               (options.crop_x.has_value() && (options.format == "base64" || options.output_file == "-"));

        if (!needs_processing && !options.output_file.empty() && options.output_file != "-") {
            // Fast path: Direct HardCopy to file with subsection support
            std::string filename = options.output_file;

            // Call HardCopy COM method
            VARIANT filename_var;
            VariantInit(&filename_var);
            filename_var.vt = VT_BSTR;
            filename_var.bstrVal = SysAllocString(std::wstring(filename.begin(), filename.end()).c_str());

            VARIANT image_type;
            VariantInit(&image_type);
            image_type.vt = VT_I4;
            image_type.lVal = 2; // PNG

            VARIANT result_path;
            VariantInit(&result_path);

            IDispatch* window_dispatch = window->get_dispatch();

            if (options.crop_x.has_value()) {
                // HardCopy with subsection
                DISPID dispid;
                LPOLESTR method_name = const_cast<LPOLESTR>(L"HardCopy");
                HRESULT hr = window_dispatch->GetIDsOfNames(IID_NULL, &method_name, 1, LOCALE_USER_DEFAULT, &dispid);

                if (SUCCEEDED(hr)) {
                    VARIANT args[6];
                    args[5] = filename_var;
                    args[4] = image_type;
                    args[3].vt = VT_I4; args[3].lVal = options.crop_x.value();
                    args[2].vt = VT_I4; args[2].lVal = options.crop_y.value();
                    args[1].vt = VT_I4; args[1].lVal = options.crop_width.value();
                    args[0].vt = VT_I4; args[0].lVal = options.crop_height.value();

                    DISPPARAMS params;
                    params.rgvarg = args;
                    params.cArgs = 6;
                    params.rgdispidNamedArgs = nullptr;
                    params.cNamedArgs = 0;

                    hr = window_dispatch->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                                 DISPATCH_METHOD, &params, &result_path, nullptr, nullptr);

                    if (FAILED(hr)) {
                        VariantClear(&filename_var);
                        VariantClear(&image_type);
                        throw std::runtime_error(fmt::format("HardCopy failed: 0x{:08X}", hr));
                    }
                }
            } else {
                // HardCopy without subsection
                DISPID dispid;
                LPOLESTR method_name = const_cast<LPOLESTR>(L"HardCopy");
                HRESULT hr = window_dispatch->GetIDsOfNames(IID_NULL, &method_name, 1, LOCALE_USER_DEFAULT, &dispid);

                if (SUCCEEDED(hr)) {
                    VARIANT args[2];
                    args[1] = filename_var;
                    args[0] = image_type;

                    DISPPARAMS params;
                    params.rgvarg = args;
                    params.cArgs = 2;
                    params.rgdispidNamedArgs = nullptr;
                    params.cNamedArgs = 0;

                    hr = window_dispatch->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                                 DISPATCH_METHOD, &params, &result_path, nullptr, nullptr);

                    if (FAILED(hr)) {
                        VariantClear(&filename_var);
                        VariantClear(&image_type);
                        throw std::runtime_error(fmt::format("HardCopy failed: 0x{:08X}", hr));
                    }
                }
            }

            VariantClear(&filename_var);
            VariantClear(&image_type);

            result.status = Result::Status::Success;
            result.data["filepath"] = filename;
            spdlog::info("Screenshot saved to: {}", filename);

            VariantClear(&result_path);
        } else {
            // Processing path: Use HardCopyToMemory + CImg
            VARIANT image_type;
            VariantInit(&image_type);
            image_type.vt = VT_I4;
            image_type.lVal = 2; // PNG

            VARIANT png_bytes_var;
            VariantInit(&png_bytes_var);

            IDispatch* window_dispatch = window->get_dispatch();

            DISPID dispid;
            LPOLESTR method_name = const_cast<LPOLESTR>(L"HardCopyToMemory");
            HRESULT hr = window_dispatch->GetIDsOfNames(IID_NULL, &method_name, 1, LOCALE_USER_DEFAULT, &dispid);

            if (SUCCEEDED(hr)) {
                VARIANT args[1];
                args[0] = image_type;

                DISPPARAMS params;
                params.rgvarg = args;
                params.cArgs = 1;
                params.rgdispidNamedArgs = nullptr;
                params.cNamedArgs = 0;

                hr = window_dispatch->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                             DISPATCH_METHOD, &params, &png_bytes_var, nullptr, nullptr);

                if (FAILED(hr)) {
                    VariantClear(&image_type);
                    throw std::runtime_error(fmt::format("HardCopyToMemory failed: 0x{:08X}", hr));
                }
            }

            VariantClear(&image_type);

            // Extract PNG bytes
            std::vector<uint8_t> png_bytes = extract_safearray_bytes(png_bytes_var);
            VariantClear(&png_bytes_var);

            spdlog::debug("Captured {} bytes from HardCopyToMemory", png_bytes.size());

            // Load into CImg
            cimg_library::CImg<unsigned char> img;
#ifdef _WIN32
            std::FILE* tmpfile = nullptr;
            errno_t err_code = tmpfile_s(&tmpfile);
            if (err_code != 0 || !tmpfile) {
                throw std::runtime_error("Failed to create temporary file for PNG loading");
            }
#else
            std::FILE* tmpfile = std::tmpfile();
            if (!tmpfile) {
                throw std::runtime_error("Failed to create temporary file for PNG loading");
            }
#endif
            std::fwrite(png_bytes.data(), 1, png_bytes.size(), tmpfile);
            std::rewind(tmpfile);
            img.load_png(tmpfile);
            std::fclose(tmpfile);

            spdlog::debug("Loaded PNG into CImg: {}x{} pixels", img.width(), img.height());

            // Apply cropping if requested
            if (options.crop_x.has_value()) {
                int x1 = options.crop_x.value();
                int y1 = options.crop_y.value();
                int x2 = x1 + options.crop_width.value() - 1;
                int y2 = y1 + options.crop_height.value() - 1;

                img.crop(x1, y1, x2, y2);
                spdlog::debug("Cropped to: {}x{} pixels", img.width(), img.height());
            }

            // Apply scaling if requested
            if (!options.scale.empty()) {
                int new_width, new_height;
                parse_scale_parameter(options.scale, img.width(), img.height(), new_width, new_height);
                img.resize(new_width, new_height, -100, -100, 5); // 5 = Lanczos interpolation
                spdlog::debug("Scaled to: {}x{} pixels", new_width, new_height);
            }

            // Display if requested
            if (options.show) {
                std::string window_display_title = "fairyfly - Screenshot Preview";
                cimg_library::CImgDisplay display(img, window_display_title.c_str());

                spdlog::info("Displaying screenshot. Close window to continue...");

                // Wait for user to close the window
                while (!display.is_closed()) {
                    display.wait();
                }

                spdlog::info("Preview window closed");
                result.data["displayed"] = true;
            }

            // Output
            if (options.format == "base64") {
                // Save to temporary memory buffer
#ifdef _WIN32
                std::FILE* mem_tmpfile = nullptr;
                errno_t err_code2 = tmpfile_s(&mem_tmpfile);
                if (err_code2 != 0 || !mem_tmpfile) {
                    throw std::runtime_error("Failed to create temporary file for PNG encoding");
                }
#else
                std::FILE* mem_tmpfile = std::tmpfile();
                if (!mem_tmpfile) {
                    throw std::runtime_error("Failed to create temporary file for PNG encoding");
                }
#endif
                img.save_png(mem_tmpfile);
                std::rewind(mem_tmpfile);

                // Read back bytes
                std::fseek(mem_tmpfile, 0, SEEK_END);
                long file_size = std::ftell(mem_tmpfile);
                std::rewind(mem_tmpfile);

                std::vector<uint8_t> output_bytes(file_size);
                std::fread(output_bytes.data(), 1, file_size, mem_tmpfile);
                std::fclose(mem_tmpfile);

                std::string base64_data = utils::base64_encode(output_bytes);
                result.data["screenshot"] = "data:image/png;base64," + base64_data;
                result.data["format"] = "base64";
                spdlog::info("Screenshot encoded as base64 ({} bytes)", base64_data.size());
            } else if (options.output_file == "-") {
                // Write to stdout
                img.save_png(stdout);
                result.data["output"] = "stdout";
                spdlog::info("Screenshot written to stdout");
            } else {
                // Save to file
                std::string filename = options.output_file.empty()
                    ? utils::generate_screenshot_filename(window_title)
                    : options.output_file;

                img.save_png(filename.c_str());
                result.data["filepath"] = filename;
                spdlog::info("Screenshot saved to: {}", filename);
            }

            result.status = Result::Status::Success;
        }

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    } catch (const std::invalid_argument& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "INVALID_ARGUMENT";
        result.error["message"] = e.what();
        spdlog::error("Invalid argument in capture_screenshot: {}", e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
        spdlog::error("Exception in capture_screenshot: {}", e.what());
    }

    return result;
}

nlohmann::json ComAutomationEngine::get_application_info() const
{
    json info;
    info["connections"] = json::array();
    info["total_connections"] = 0;
    info["total_sessions"] = 0;
    
    if (!app_) {
        info["error"] = "No SAP GUI application available. Is SAP Logon running?";
        return info;
    }
    
    try {
        int conn_count = app_->get_connection_count();
        info["total_connections"] = conn_count;
        
        json connections_array = json::array();
        
        for (int c = 0; c < conn_count; ++c) {
            auto connection = app_->get_connection(c);
            if (!connection) continue;
            
            json conn_obj;
            conn_obj["index"] = c;
            conn_obj["id"] = connection->get_id();
            conn_obj["description"] = connection->get_description();
            conn_obj["type"] = connection->get_type();
            conn_obj["type_as_number"] = connection->get_type_as_number();
            conn_obj["connection_string"] = connection->get_connection_string();
            
            int sess_count = connection->get_session_count();
            json sessions_array = json::array();
            
            for (int s = 0; s < sess_count; ++s) {
                auto session = connection->get_session(s);
                if (!session) continue;
                
                json sess_obj;
                sess_obj["index"] = s;
                sess_obj["id"] = session->get_id();
                sess_obj["name"] = session->get_name();
                sess_obj["type"] = session->get_type();
                sess_obj["type_as_number"] = session->get_type_as_number();
                sess_obj["busy"] = session->is_busy();
                sess_obj["alive"] = session->is_alive();
                
                // Try to get active window
                auto active_window = session->get_active_window();
                if (active_window) {
                    sess_obj["active_window_title"] = active_window->get_title();
                }
                
                sessions_array.push_back(sess_obj);
            }
            
            conn_obj["sessions"] = sessions_array;
            conn_obj["session_count"] = sess_count;
            connections_array.push_back(conn_obj);
            info["total_sessions"] = info["total_sessions"].get<int>() + sess_count;
        }
        
        info["connections"] = connections_array;
        
    } catch (const std::exception& e) {
        info["error"] = e.what();
    }

    return info;
}

std::string ComAutomationEngine::normalize_sap_path(const std::string& sap_path) {
    // SAP GUI COM API returns 1-indexed paths like "/app/con[1]/ses[0]"
    // But we use 0-indexed paths internally like "/app/con[0]/ses[0]"
    // This function converts SAP's 1-indexed to our 0-indexed format

    std::string normalized = sap_path;
    std::regex con_pattern(R"(/con\[(\d+)\])");
    std::regex ses_pattern(R"(/ses\[(\d+)\])");

    // Convert connection index: con[N] -> con[N-1]
    std::smatch match;
    if (std::regex_search(normalized, match, con_pattern)) {
        int idx = std::stoi(match[1].str());
        if (idx > 0) {
            std::string replacement = "/con[" + std::to_string(idx - 1) + "]";
            normalized = std::regex_replace(normalized, con_pattern, replacement);
        }
    }

    // Session indices from SAP are already 0-indexed, so no conversion needed for /ses[N]

    spdlog::debug("Normalized SAP path: {} -> {}", sap_path, normalized);
    return normalized;
}


} // namespace sap
} // namespace fairyfly
