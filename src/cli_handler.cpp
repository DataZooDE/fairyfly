#include "include/cli_handler.h"
#include "include/com_automation_engine.h"
#include "include/constants.h"
#include "include/grid_analyzer.h"
#include "include/formatters/grid_renderer.h"
#include "include/formatters/screen_markdown_formatter.h"
#include "include/formatters/tree_formatter.h"
#include "include/formatters/table_formatter.h"
#include "include/formatters/markdown_table_formatter.h"
#include "include/formatters/toon_encoder.h"
#include "include/element_renderer_registry.h"
#include "include/semantic_classifier.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <set>
#include <unordered_set>
#include <climits>
#include <algorithm>

namespace fairyfly {
namespace cli {

CommandHandler::CommandHandler()
    : engine_(AutomationEngine::create()),
      conn_mgr_(std::make_unique<ConnectionManager>())
{
    // Initialize command handler
    spdlog::debug("CommandHandler initialized");
}

ResultT<Connection> CommandHandler::resolve_and_validate_connection(std::optional<int> explicit_conn_id)
{
    // Resolve connection (auto-detect or explicit)
    auto conn_result = conn_mgr_->resolve_connection(explicit_conn_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return conn_result;  // Return error as-is
    }

    Connection conn = conn_result.value;

    // Validate that the session still exists
    if (!engine_->validate_session(conn.session_id)) {
        spdlog::warn("Connection {} has invalid session {}, deleting", conn.id, conn.session_id);
        conn_mgr_->delete_connection(conn.id);

        ResultT<Connection> result;
        result.status = ResultT<Connection>::Status::Error;
        result.error["code"] = "INVALID_CONNECTION";
        result.error["message"] = fmt::format("Connection {} session no longer exists", conn.id);
        result.error["session_id"] = conn.session_id;
        result.error["suggestions"] = json::array({
            "The SAP session was closed or connection lost",
            "Run 'fairyfly attach' to create a new connection"
        });
        return result;
    }

    // Update last_validated timestamp
    conn_mgr_->touch_connection(conn.id);

    ResultT<Connection> result;
    result.status = ResultT<Connection>::Status::Success;
    result.value = conn;
    return result;
}

Result CommandHandler::handle_attach(int timeout_seconds)
{
    spdlog::info("Starting window attachment mode (timeout: {}s)", timeout_seconds);
    auto result = engine_->attach_by_click(timeout_seconds);

    if (result.status == Result::Status::Success) {
        spdlog::info("Successfully attached to SAP window");

        // Extract connection info from result
        std::string session_id = result.data.value("session_id", "");
        std::string connection_id = result.data.value("connection_id", "");
        std::string window_title = result.data.value("window_title", "");

        // Get connection metadata if available
        auto app_info = engine_->get_application_info();
        std::string description = "";
        std::string connection_string = "";

        if (app_info.contains("connections") && app_info["connections"].is_array() && !app_info["connections"].empty()) {
            auto conn = app_info["connections"][0];
            description = conn.value("description", "");
            connection_string = conn.value("connection_string", "");
        }

        // Create or update connection file
        Connection conn = conn_mgr_->create_or_update_connection(
            session_id,
            connection_id,
            description,
            connection_string,
            window_title
        );

        result.data["connection_file_id"] = conn.id;
        result.data["connection_file"] = conn.get_file_path();
        result.data["message"] = fmt::format("Attached to SAP GUI window (connection: {})", conn.id);

        spdlog::info("Created/updated connection file: {}", conn.get_file_path());
    } else {
        result.error["suggestions"] = json::array({
            "Ensure you clicked on an active SAP transaction window (not just SAP Logon)",
            "The window must contain an active SAP session with a transaction loaded",
            "Try running 'fairyfly diagnose' to check SAP GUI status"
        });
    }

    return result;
}

Result CommandHandler::handle_launch(const std::string& connection_name)
{
    spdlog::info("Launching SAP connection: {}", connection_name);
    auto result = engine_->launch_connection(connection_name);

    if (result.status == Result::Status::Success) {
        spdlog::info("Successfully launched SAP connection: {}", connection_name);

        // Extract connection info from result
        std::string session_id = result.data.value("session_id", "");
        std::string connection_id = result.data.value("connection_id", "");

        // Get connection metadata
        auto app_info = engine_->get_application_info();
        std::string description = connection_name;
        std::string connection_string = "";

        if (app_info.contains("connections") && app_info["connections"].is_array() && !app_info["connections"].empty()) {
            auto conn = app_info["connections"][0];
            description = conn.value("description", connection_name);
            connection_string = conn.value("connection_string", "");
        }

        // Create or update connection file
        Connection conn = conn_mgr_->create_or_update_connection(
            session_id,
            connection_id,
            description,
            connection_string,
            ""  // No window title for launch
        );

        result.data["connection_file_id"] = conn.id;
        result.data["connection_file"] = conn.get_file_path();
        result.data["message"] = fmt::format("Launched SAP connection '{}' (connection: {})", connection_name, conn.id);

        spdlog::info("Created/updated connection file: {}", conn.get_file_path());
    }

    return result;
}

Result CommandHandler::handle_disconnect(std::optional<int> connection_id)
{
    Result result;

    // Resolve which connection to disconnect
    if (!connection_id.has_value()) {
        // No explicit ID - check if single connection exists
        auto connections = conn_mgr_->list_connections();

        if (connections.empty()) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_CONNECTIONS";
            result.error["message"] = "No connection files found";
            return result;
        }

        if (connections.size() > 1) {
            result.status = Result::Status::Error;
            result.error["code"] = "MULTIPLE_CONNECTIONS";
            result.error["message"] = fmt::format("Found {} connections, specify which one to disconnect", connections.size());
            result.error["suggestions"] = json::array({
                "Use --connection <id> to specify which connection to disconnect",
                "Run 'fairyfly connections' to see all connections"
            });
            return result;
        }

        connection_id = connections[0].id;
    }

    // Load the connection
    auto conn = conn_mgr_->load_connection(connection_id.value());
    if (!conn.has_value()) {
        result.status = Result::Status::Error;
        result.error["code"] = "CONNECTION_NOT_FOUND";
        result.error["message"] = fmt::format("Connection {} not found", connection_id.value());
        return result;
    }

    // Delete the connection file
    bool deleted = conn_mgr_->delete_connection(connection_id.value());

    result.status = Result::Status::Success;
    result.data["connection_id"] = connection_id.value();
    result.data["session_id"] = conn.value().session_id;
    result.data["file_deleted"] = deleted;
    result.data["message"] = fmt::format("Disconnected connection {}", connection_id.value());

    spdlog::info("Disconnected and removed connection file: fairyfly.{}.con", connection_id.value());

    return result;
}

Result CommandHandler::handle_connections_list(bool cleanup)
{
    Result result;

    auto connections = conn_mgr_->list_connections();

    if (cleanup) {
        // Cleanup invalid connections
        int deleted = conn_mgr_->cleanup_invalid_connections(
            [this](const std::string& session_id) {
                return engine_->validate_session(session_id);
            }
        );

        result.data["cleaned_up"] = deleted;
        spdlog::info("Cleaned up {} invalid connection(s)", deleted);

        // Reload connections after cleanup
        connections = conn_mgr_->list_connections();
    }

    // Build connection list with validation status
    json conn_list = json::array();
    for (const auto& conn : connections) {
        bool valid = engine_->validate_session(conn.session_id);

        conn_list.push_back({
            {"id", conn.id},
            {"file", conn.get_file_path()},
            {"session_id", conn.session_id},
            {"connection_id", conn.connection_id},
            {"description", conn.connection_description},
            {"connection_string", conn.connection_string},
            {"window_title", conn.window_title},
            {"created_at", conn.created_at},
            {"last_validated", conn.last_validated},
            {"valid", valid}
        });
    }

    result.status = Result::Status::Success;
    result.data["connections"] = conn_list;
    result.data["count"] = connections.size();

    return result;
}

Result CommandHandler::handle_transaction(const std::string& tcode, std::optional<int> connection_id)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    spdlog::info("Executing transaction: {} on connection {}", tcode, conn_result.value.id);

    auto result = engine_->execute_transaction(tcode);

    if (result.status == Result::Status::Success) {
        result.data["transaction"] = tcode;
        result.data["connection_id"] = conn_result.value.id;
    }

    return result;
}

Result CommandHandler::handle_click(const std::string& element_id, std::optional<int> connection_id,
                                     bool wait_for_window, int timeout_ms)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    ElementId elem(element_id);
    if (!elem.is_valid()) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "INVALID_ELEMENT";
        result.error["message"] = fmt::format("Invalid element ID: {}", element_id);
        result.error["suggestions"] = json::array({
            "Element IDs should start with 'wnd' or '@active' (e.g., wnd[0]/usr/btn[3] or @active/usr/btn[3])"
        });
        return result;
    }

    // Get current active window before click (if monitoring for new windows)
    WindowId window_before;
    if (wait_for_window) {
        window_before = engine_->get_active_window_id();
        spdlog::info("Clicking element: {} on connection {} (monitoring for new window, current={})",
                     element_id, conn_result.value.id, window_before.id);
    } else {
        spdlog::info("Clicking element: {} on connection {}", element_id, conn_result.value.id);
    }

    auto result = engine_->click_element(elem);

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;

        // If requested, wait for new window to appear
        if (wait_for_window) {
            auto start = std::chrono::high_resolution_clock::now();
            bool window_changed = false;
            WindowId new_window = window_before;

            // Poll for window change with 100ms interval
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                new_window = engine_->get_active_window_id();
                if (new_window.id != window_before.id) {
                    window_changed = true;
                    break;
                }

                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - start
                );
                if (elapsed.count() > timeout_ms) {
                    break;
                }
            }

            if (window_changed) {
                result.data["window_changed"] = true;
                result.data["window_before"] = window_before.id;
                result.data["window_after"] = new_window.id;
                result.data["new_window"] = new_window.id;  // For easy access
                spdlog::info("New window detected after click: {} -> {}", window_before.id, new_window.id);
            } else {
                result.data["window_changed"] = false;
                result.data["waited_ms"] = timeout_ms;
                spdlog::debug("No new window detected after click (waited {}ms)", timeout_ms);
            }
        }
    }

    return result;
}

Result CommandHandler::handle_fill(const std::string& element_id, const std::string& value, std::optional<int> connection_id)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    ElementId elem(element_id);
    if (!elem.is_valid()) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "INVALID_ELEMENT";
        result.error["message"] = fmt::format("Invalid element ID: {}", element_id);
        return result;
    }

    spdlog::info("Filling element: {} with value: {} on connection {}", element_id, value, conn_result.value.id);
    auto result = engine_->fill_field(elem, value);

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }

    return result;
}

Result CommandHandler::handle_read_field(const std::string& element_id, std::optional<int> connection_id)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    ElementId elem(element_id);
    if (!elem.is_valid()) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "INVALID_ELEMENT";
        result.error["message"] = fmt::format("Invalid element ID: {}", element_id);
        return result;
    }

    spdlog::info("Reading field: {} on connection {}", element_id, conn_result.value.id);
    auto result = engine_->read_field(elem);

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }

    return result;
}

Result CommandHandler::handle_screen_read(bool include_children, std::optional<int> connection_id, bool expand_tabs)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    spdlog::info("Reading screen structure (include_children={}, expand_tabs={}) on connection {}",
                 include_children, expand_tabs, conn_result.value.id);

    Result result;
    if (expand_tabs) {
        result = engine_->read_screen_with_tabs();
    } else {
        result = engine_->read_screen(include_children);
    }

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }

    return result;
}

Result CommandHandler::handle_screenshot(std::optional<int> connection_id, const ScreenshotOptions& options)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    spdlog::info("Capturing screenshot on connection {}", conn_result.value.id);
    auto result = engine_->capture_screenshot(options);

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }

    return result;
}

Result CommandHandler::handle_list_all()
{
    spdlog::info("Listing all SAP GUI connections, sessions, and windows");
    
    Result result;
    result.status = Result::Status::Success;
    
    try {
        // Get application info
        json info = engine_->get_application_info();
        result.data = info;
        result.status = Result::Status::Success;
        
        // Log summary
        int total_conns = info["total_connections"].get<int>();
        int total_sess = info["total_sessions"].get<int>();
        spdlog::info("Found {} connection(s) with {} total session(s)", total_conns, total_sess);
        
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "ENUMERATION_FAILED";
        result.error["message"] = e.what();
        spdlog::error("Failed to enumerate SAP GUI: {}", e.what());
    }
    
    return result;
}

// format_screen_markdown and all helper functions moved to ScreenMarkdownFormatter class
// Grid layout functions moved to GridAnalyzer and GridRenderer classes

std::string format_output(const Result& result, OutputFormat format)
{
    switch (format) {
        case OutputFormat::Json: {
            return result.to_json().dump(2);
        }
        case OutputFormat::Markdown: {
            std::ostringstream oss;
            const auto& response = result.to_json();

            // Check if this is screen data (special formatting)
            if (response["status"] == "success" &&
                response.contains("data") &&
                response["data"].contains("screen_id") &&
                response["data"].contains("hierarchy")) {
                // Use special screen markdown formatter
                oss << ScreenMarkdownFormatter::format(response["data"]);

                // Add metadata footer
                if (response.contains("metadata") && response["metadata"].is_object()) {
                    const auto& meta = response["metadata"];
                    if (meta.contains("duration_ms")) {
                        oss << "\n_Read time: " << meta["duration_ms"].get<int>() << "ms_\n";
                    }
                }

                return oss.str();
            }

            // Standard markdown formatting for non-screen results
            // Title
            if (response["status"] == "success") {
                oss << "# ✅ Success\n\n";
            } else {
                oss << "# ❌ Error\n\n";
            }

            // Error details
            if (response.contains("error")) {
                const auto& error = response["error"];
                if (error.contains("code")) {
                    oss << "**Error Code:** `" << error["code"].get<std::string>() << "`\n\n";
                }
                if (error.contains("message")) {
                    oss << "**Message:** " << error["message"].get<std::string>() << "\n\n";
                }
                if (error.contains("suggestions") && error["suggestions"].is_array()) {
                    oss << "**Suggestions:**\n";
                    for (const auto& suggestion : error["suggestions"]) {
                        oss << "- " << suggestion.get<std::string>() << "\n";
                    }
                    oss << "\n";
                }
            }

            // Data
            if (response.contains("data") && !response["data"].empty()) {
                oss << "## Data\n\n```json\n";
                oss << response["data"].dump(2) << "\n```\n";
            }

            // Metadata
            if (response.contains("metadata") && response["metadata"].is_object()) {
                const auto& meta = response["metadata"];
                if (!meta.empty()) {
                    oss << "## Metadata\n\n";
                    if (meta.contains("duration_ms")) {
                        oss << "- **Duration:** " << meta["duration_ms"].get<int>() << "ms\n";
                    }
                    if (meta.contains("timestamp")) {
                        oss << "- **Timestamp:** " << meta["timestamp"].get<std::string>() << "\n";
                    }
                }
            }

            return oss.str();
        }
        case OutputFormat::PlainText: {
            std::ostringstream oss;
            const auto& response = result.to_json();

            if (response["status"] == "success") {
                oss << "SUCCESS\n";
            } else {
                oss << "ERROR\n";
                if (response.contains("error")) {
                    const auto& error = response["error"];
                    if (error.contains("message")) {
                        oss << error["message"].get<std::string>() << "\n";
                    }
                }
            }

            return oss.str();
        }
        case OutputFormat::Toon: {
            formatters::ToonEncoder encoder;
            formatters::ToonOptions opts;
            opts.indent = 2;
            opts.delimiter = ',';
            opts.length_marker = false;

            // Encode the full result structure (status, data, error, metadata)
            return encoder.encode(result.to_json(), opts);
        }
    }

    return result.to_json().dump(2);
}

}  // namespace cli
}  // namespace fairyfly
