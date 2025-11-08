#include "include/com_automation_engine.h"
#include "include/com/raii_helpers.h"
#include "include/constants.h"
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
        #define cimg_display 2  // CIMG_DISPLAY_GDI value
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

        // Initialize connection launcher
        connection_launcher_ = std::make_unique<ConnectionLauncher>(app_);

        // Check if any connections exist
        if (app_->get_connection_count() > 0) {
            current_connection_ = app_->get_connection(0);
            if (current_connection_ && current_connection_->get_session_count() > 0) {
                current_session_ = current_connection_->get_session(0);
                spdlog::debug("Found existing connection and session");
                initialize_services();
            }
        }
    } catch (const ComException& e) {
        spdlog::warn("SAP GUI not available: {}", e.what());
        // Non-fatal - can still be initialized, just not connected
    }
}

void ComAutomationEngine::initialize_services() {
    if (current_session_) {
        screenshot_handler_ = std::make_unique<ScreenshotHandler>(current_session_);
        screen_reader_ = std::make_unique<ScreenReader>(current_session_);
        spdlog::debug("Service classes initialized");
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
        initialize_services();
    }
    return current_session_;
}

// Helper functions removed - moved to ConnectionLauncher, ScreenshotHandler, ScreenReader, ElementMetadataExtractor
// Removed: read_credentials_from_env, launch_sapshcut, wait_for_session (moved to ConnectionLauncher)
// Removed: extract_element_metadata, clear_extraction_cache (moved to ElementMetadataExtractor)
// Removed: discover_elements, traverse_element_tree, group_elements_by_container (moved to ScreenReader)
// Removed: extract_safearray_bytes, parse_scale_parameter, validate_subsection_complete (moved to ScreenshotHandler)
// Removed: read_credentials_from_env_DELETE_ME, launch_sapshcut, wait_for_session (moved to ConnectionLauncher)

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
        auto creds_opt = ConnectionLauncher::read_credentials_from_env(connection_name);
        if (!creds_opt.has_value()) {
            throw std::runtime_error("Failed to read credentials from trial.env");
        }

        ConnectionLauncher::Credentials creds = creds_opt.value();
        trace.push_back({
            {"step", "read_credentials"},
            {"username", creds.username},
            {"client", creds.client},
            {"status", "success"}
        });

        // Step 2: Launch sapshcut.exe with credentials
        bool launch_success = ConnectionLauncher::launch_sapshcut(connection_name, creds);
        trace.push_back({
            {"step", "launch_sapshcut"},
            {"success", launch_success}
        });

        if (!launch_success) {
            throw std::runtime_error("Failed to launch sapshcut.exe");
        }

        // Step 3: Wait for session to be created (polls every CONNECTION_POLL_INTERVAL_MS, timeout SESSION_CREATION_TIMEOUT_SEC)
        auto [connection, session] = connection_launcher_->wait_for_session(connection_name, constants::SESSION_CREATION_TIMEOUT_SEC);
        bool session_created = (connection != nullptr && session != nullptr);
        
        if (session_created) {
            current_connection_ = connection;
            current_session_ = session;
            initialize_services();
        }
        trace.push_back({
            {"step", "wait_for_session"},
            {"success", session_created},
            {"timeout_seconds", constants::SESSION_CREATION_TIMEOUT_SEC}
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

        // Resolve @active to actual window ID
        ElementId resolved_element = resolve_element_path(element);
        auto session = ensure_session();
        auto elem = session->find_element_by_id(resolved_element.path);

        if (!elem) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "Element not found at path: " + resolved_element.path;
            result.error["element"] = resolved_element.path;

            // Check if window mismatch (element path specifies different window than active)
            WindowId requested_window = resolved_element.get_window();
            WindowId active_window = get_active_window_id();
            bool window_mismatch = (requested_window.id != active_window.id);

            json suggestions = json::array({
                "Verify element path is correct",
                "Run 'screen read' to see available elements",
                "Check if screen is fully loaded"
            });

            if (window_mismatch) {
                result.error["window_mismatch"] = true;
                result.error["requested_window"] = requested_window.id;
                result.error["active_window"] = active_window.id;
                suggestions.push_back(
                    "Active window is " + active_window.id + ", not " + requested_window.id + " - did a popup open?"
                );
                suggestions.push_back(
                    "Try: fairyfly click " + active_window.id + "/" + resolved_element.get_element_path()
                );
                suggestions.push_back(
                    "Or use: fairyfly click @active/" + resolved_element.get_element_path()
                );
            }

            result.error["suggestions"] = suggestions;
            spdlog::warn("Element not found: {}|window_mismatch={}", resolved_element.path, window_mismatch);
            return result;
        }

        if (!elem->is_enabled()) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_DISABLED";
            result.error["message"] = "Element is disabled and cannot be clicked";
            result.error["element"] = resolved_element.path;
            return result;
        }

        // Press the button element
        elem->press();

        result.status = Result::Status::Success;
        result.data["element"] = resolved_element.path;
        if (element.path != resolved_element.path) {
            result.data["element_requested"] = element.path;  // Show original @active path
        }
        result.data["action"] = "click";
        result.data["element_type"] = elem->get_type();
        result.data["window"] = resolved_element.get_window().id;

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Clicked element {} (duration: {}ms)", resolved_element.path, result.duration.count());

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

        // Resolve @active to actual window ID
        ElementId resolved_element = resolve_element_path(element);
        auto session = ensure_session();
        auto elem = session->find_element_by_id(resolved_element.path);

        if (!elem) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "Element not found at path: " + resolved_element.path;
            result.error["element"] = resolved_element.path;

            // Check if window mismatch
            WindowId requested_window = resolved_element.get_window();
            WindowId active_window = get_active_window_id();
            bool window_mismatch = (requested_window.id != active_window.id);

            json suggestions = json::array({
                "Verify element path is correct",
                "Run 'screen read' to see available elements",
                "Check if screen is fully loaded"
            });

            if (window_mismatch) {
                result.error["window_mismatch"] = true;
                result.error["requested_window"] = requested_window.id;
                result.error["active_window"] = active_window.id;
                suggestions.push_back(
                    "Active window is " + active_window.id + ", not " + requested_window.id + " - did a popup open?"
                );
                suggestions.push_back(
                    "Try: fairyfly fill " + active_window.id + "/" + resolved_element.get_element_path() + " \"" + value + "\""
                );
                suggestions.push_back(
                    "Or use: fairyfly fill @active/" + resolved_element.get_element_path() + " \"" + value + "\""
                );
            }

            result.error["suggestions"] = suggestions;
            spdlog::warn("Element not found: {}|window_mismatch={}", resolved_element.path, window_mismatch);
            return result;
        }

        if (!elem->is_enabled()) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_DISABLED";
            result.error["message"] = "Element is disabled";
            result.error["element"] = resolved_element.path;
            return result;
        }

        // Set the text
        elem->set_text(value);

        result.status = Result::Status::Success;
        result.data["element"] = resolved_element.path;
        if (element.path != resolved_element.path) {
            result.data["element_requested"] = element.path;  // Show original @active path
        }
        result.data["value"] = value;
        result.data["action"] = "fill";
        result.data["window"] = resolved_element.get_window().id;

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Filled field {} (duration: {}ms)", resolved_element.path, result.duration.count());

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
// This function remains as a thin wrapper during migration
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

        // Interactive states - query for all elements (is_enabled handles missing property gracefully)
        bool enabled = elem->is_enabled();
        bool visible = elem->is_visible();
        bool changeable = elem->is_changeable();

        metadata["enabled"] = enabled;
        metadata["visible"] = visible;
        metadata["changeable"] = changeable;

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

        // Parse grid coordinates for grid-positioned labels (pattern: lbl[row,col])
        if (type == "GuiLabel" && elem_id.find("/lbl[") != std::string::npos) {
            size_t bracket_pos = elem_id.find("/lbl[");
            size_t comma_pos = elem_id.find(",", bracket_pos);
            size_t close_bracket = elem_id.find("]", comma_pos);

            if (comma_pos != std::string::npos && close_bracket != std::string::npos) {
                try {
                    std::string row_str = elem_id.substr(bracket_pos + 5, comma_pos - bracket_pos - 5);
                    std::string col_str = elem_id.substr(comma_pos + 1, close_bracket - comma_pos - 1);

                    metadata["grid_row"] = std::stoi(row_str);
                    metadata["grid_col"] = std::stoi(col_str);
                } catch (const std::invalid_argument&) {
                    // Failed to parse coordinates, skip
                } catch (const std::out_of_range&) {
                    // Number out of range, skip
                }
            }
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
                    options.max_rows = constants::MAX_TABLE_ROWS;
                    options.max_tree_depth = constants::MAX_TREE_DEPTH;
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

        // Extract text content from GuiShell with AbapEditor subtype (ABAP source code editor)
        if (type == "GuiShell" && subtype == "AbapEditor") {
            try {
                std::string editor_text = elem->get_property_string(L"Text");
                if (!editor_text.empty()) {
                    metadata["text_content"] = editor_text;
                    spdlog::debug("Extracted AbapEditor text content ({} chars)", editor_text.length());
                } else {
                    metadata["text_content"] = nullptr;  // Empty editor
                    spdlog::debug("AbapEditor is empty");
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to extract AbapEditor text: {}", e.what());
                metadata["text_content"] = nullptr;
            }
        }

        // Recursively process children (limit depth to MAX_ELEMENT_DEPTH levels to capture deeply nested tree controls)
        // SM59 tree structure: Window -> UserArea -> CustomControl -> ContainerShell -> SplitterShell -> ContainerShell[n] -> Tree
        // SEGW tree structure: Window -> UserArea -> ContainerShell -> SplitterShell -> ContainerShell -> ContainerShell -> SplitterShell -> ContainerShell[n] -> Tree
        // SEGW GridView is at depth 12: usr -> shellcont -> shell -> shellcont[1] -> shell -> shellcont[0] -> shell -> shellcont[0] -> shellcont -> shellcont -> shell -> shellcont[1] -> shell (GridView)
        if (depth < constants::MAX_ELEMENT_DEPTH) {
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

                    // For forced enumeration, try up to MAX_CHILDREN_TO_PROCESS items even if child_count is 0
                    int max_children = force_enumerate ?
                        (child_count > 0 ? (std::min)(child_count, constants::MAX_CHILDREN_TO_PROCESS) : constants::MAX_CHILDREN_TO_PROCESS) :
                        (std::min)(child_count, constants::MAX_CHILDREN_TO_PROCESS);

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
                        } catch (const SapGuiException& e) {
                            // Catch most specific exception first
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: SapGuiException at index {}, stopping: {}", i, e.what());
                                break;
                            }
                            spdlog::debug("extract_element_metadata: SapGuiException processing child {}: {}", i, e.what());
                        } catch (const ComException& e) {
                            // Catch COM-specific exception second
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: ComException at index {}, stopping: {}", i, e.what());
                                break;
                            }
                            spdlog::debug("extract_element_metadata: ComException processing child {}: {}", i, e.what());
                        } catch (const std::exception& e) {
                            // Catch general exception last (base class)
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: Exception at index {}, stopping: {}", i, e.what());
                                break;
                            }
                            spdlog::debug("extract_element_metadata: Exception processing child {}: {}", i, e.what());
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
    } catch (const SapGuiException& e) {
        spdlog::debug("extract_element_metadata: SapGuiException extracting metadata: {}", e.what());
        return nullptr;
    } catch (const ComException& e) {
        spdlog::debug("extract_element_metadata: ComException extracting metadata: {}", e.what());
        return nullptr;
    } catch (const std::exception& e) {
        spdlog::debug("extract_element_metadata: std::exception extracting metadata: {}", e.what());
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
            // Split buttons: toolbar buttons vs inline buttons (in /usr/)
            std::string id = elem.value("id", "");
            if (id.find("/tbar/") != std::string::npos) {
                grouped["buttons"].push_back(elem);  // Toolbar buttons
            } else {
                grouped["inline_buttons"].push_back(elem);  // Inline buttons
            }
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
    grouped["inline_buttons"] = json::array();
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
    deduplicate(grouped["inline_buttons"]);
    deduplicate(grouped["form_fields"]);
    deduplicate(grouped["tables"]);
    deduplicate(grouped["tabs"]);
    deduplicate(grouped["other"]);

    // Remove empty groups
    if (grouped["toolbar"].empty()) grouped.erase("toolbar");
    if (grouped["buttons"].empty()) grouped.erase("buttons");
    if (grouped["inline_buttons"].empty()) grouped.erase("inline_buttons");
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
                    } catch (const SapGuiException&) {
                        // Child access failed, will fall back to ID-based
                    } catch (const ComException&) {
                        // Child access failed, will fall back to ID-based
                    } catch (const std::exception&) {
                        // Child access failed, will fall back to ID-based
                    }
                }
                // Don't return here - continue with ID-based discovery
                // SAP GUI can have elements accessible via FindById but not Children collection
            }
        } catch (const std::exception&) {
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
            } catch (const std::exception&) {
                // Shell child not found, continue
            }

            // Try /shellcont[N] children (up to MAX_SHELLCONT_CHILDREN)
            for (int i = 0; i < constants::MAX_SHELLCONT_CHILDREN; ++i) {
                try {
                    std::string child_id = elem_id + "/shellcont[" + std::to_string(i) + "]";
                    auto child = session->find_element_by_id(child_id);
                    if (child) {
                        traverse_element_tree(child, collector, session, depth + 1);
                    }
                } catch (const std::exception&) {
                    break;  // No more shellcont children
                }
            }

            // Try /shellcont child (no index)
            try {
                auto shellcont_child = session->find_element_by_id(elem_id + "/shellcont");
                if (shellcont_child) {
                    traverse_element_tree(shellcont_child, collector, session, depth + 1);
                }
            } catch (const std::exception&) {
                // Shellcont child not found, continue
            }

            // Special handling for GuiUserArea: probe for grid-positioned labels
            // Pattern: /usr/lbl[row,col]
            if (type == "GuiUserArea") {
                spdlog::debug("{}Probing GuiUserArea for grid labels", std::string(depth * 2, ' '));

                // Probe grid bounds (up to MAX_GRID_ROW x MAX_GRID_COL, stop early if consecutive misses)
                // Allow for very sparse grids (SMICM has 26 empty rows)

                int consecutive_row_misses = 0;

                for (int row = 0; row < constants::MAX_GRID_ROW && consecutive_row_misses < constants::MAX_CONSECUTIVE_MISSES; ++row) {
                    int consecutive_col_misses = 0;
                    bool found_any_in_row = false;

                    for (int col = 0; col < constants::MAX_GRID_COL && consecutive_col_misses < constants::MAX_CONSECUTIVE_MISSES; ++col) {
                        try {
                            std::string label_id = elem_id + "/lbl[" + std::to_string(row) + "," + std::to_string(col) + "]";
                            auto label = session->find_element_by_id(label_id);

                            if (label) {
                                collector.add(label);
                                found_any_in_row = true;
                                consecutive_col_misses = 0;
                            } else {
                                consecutive_col_misses++;
                            }
                        } catch (const std::exception&) {
                            consecutive_col_misses++;
                        }
                    }

                    if (!found_any_in_row) {
                        consecutive_row_misses++;
                    } else {
                        consecutive_row_misses = 0;
                    }
                }

                spdlog::debug("{}Grid probing complete for {}", std::string(depth * 2, ' '), elem_id);
            }
        }
    } catch (const SapGuiException& e) {
        spdlog::debug("traverse_element_tree: SapGuiException: {}", e.what());
        // Skip elements that throw exceptions during processing
    } catch (const ComException& e) {
        spdlog::debug("traverse_element_tree: ComException: {}", e.what());
        // Skip elements that throw exceptions during processing
    } catch (const std::exception& e) {
        spdlog::debug("traverse_element_tree: std::exception: {}", e.what());
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
            } catch (const std::exception&) {
                // Skip problematic children
            }
        }
    } catch (const SapGuiException& e) {
        spdlog::warn("SapGuiException during element tree traversal: {}", e.what());
    } catch (const ComException& e) {
        spdlog::warn("ComException during element tree traversal: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("std::exception during element tree traversal: {}", e.what());
    }

    spdlog::info("Discovered {} unique elements via recursive traversal", collector.elements().size());
    return collector.elements();
}

Result ComAutomationEngine::read_screen(bool include_structure) {
        auto session = ensure_session();
    if (!screen_reader_) {
        initialize_services();
    }
    if (!screen_reader_) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "NO_SESSION";
        result.error["message"] = "Unable to initialize screen reader - no session";
    return result;
}
    return screen_reader_->read(include_structure);
        }

Result ComAutomationEngine::read_screen_with_tabs() {
    if (!screen_reader_) {
        auto session = ensure_session();
        initialize_services();
    }
    if (!screen_reader_) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "NO_SESSION";
        result.error["message"] = "Unable to initialize screen reader - no session";
    return result;
}
    return screen_reader_->read_with_tabs();
}

Result ComAutomationEngine::capture_screenshot(const cli::ScreenshotOptions& options) {
    if (!screenshot_handler_) {
        auto session = ensure_session();
        initialize_services();
    }
    if (!screenshot_handler_) {
    Result result;
            result.status = Result::Status::Error;
            result.error["code"] = "NO_SESSION";
        result.error["message"] = "Unable to initialize screenshot handler - no session";
            return result;
        }
    return screenshot_handler_->capture(options);
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

WindowId ComAutomationEngine::get_active_window_id() const
{
    if (!current_session_) {
        return WindowId("wnd[0]");  // Default to main window if no session
    }

    try {
        auto active_window = current_session_->get_active_window();
        if (!active_window) {
            return WindowId("wnd[0]");
        }

        std::string window_id = active_window->get_id();
        spdlog::debug("get_active_window_id|window_id={}", window_id);
        return WindowId(window_id);
    } catch (const std::exception& e) {
        spdlog::warn("get_active_window_id|failed|error={}|defaulting_to_wnd[0]", e.what());
        return WindowId("wnd[0]");
    }
}

ElementId ComAutomationEngine::resolve_element_path(const ElementId& element) const
{
    if (!element.is_valid()) {
        return element;  // Return as-is if invalid
    }

    WindowId window = element.get_window();
    if (!window.is_active_selector()) {
        return element;  // Already has explicit window, no resolution needed
    }

    // Resolve @active to actual window ID
    WindowId active_window = get_active_window_id();
    ElementId resolved = element.with_window(active_window);

    spdlog::debug("resolve_element_path|original={}|resolved={}",
                  element.path, resolved.path);
    return resolved;
}


} // namespace sap
} // namespace fairyfly
