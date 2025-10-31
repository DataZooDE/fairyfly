#include "include/cli_handler.h"
#include "include/com_automation_engine.h"
#include "include/formatters/tree_formatter.h"
#include "include/formatters/table_formatter.h"
#include "include/element_renderer_registry.h"
#include "include/semantic_classifier.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <set>

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

Result CommandHandler::handle_click(const std::string& element_id, std::optional<int> connection_id)
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
            "Element IDs should start with 'wnd' (e.g., wnd[0]/usr/btn[3])"
        });
        return result;
    }

    spdlog::info("Clicking element: {} on connection {}", element_id, conn_result.value.id);
    auto result = engine_->click_element(elem);

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
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

// Helper: Format screen hierarchy as markdown
// Helper: Build map of labels to their associated input fields
static std::map<std::string, std::string> build_label_field_map(const json& hierarchy) {
    std::map<std::string, std::string> label_map; // field_id -> label_text

    if (!hierarchy.contains("form_fields")) {
        return label_map;
    }

    const auto& fields = hierarchy["form_fields"];
    std::vector<json> labels;
    std::vector<json> inputs;

    // Separate labels from input fields
    for (const auto& elem : fields) {
        std::string type = elem.value("type", "");
        if (type == "GuiLabel") {
            labels.push_back(elem);
        } else if (type.find("TextField") != std::string::npos ||
                   type.find("ComboBox") != std::string::npos ||
                   type.find("CheckBox") != std::string::npos ||
                   type.find("RadioButton") != std::string::npos) {
            inputs.push_back(elem);
        }
    }

    // Match labels to fields by name pattern
    for (const auto& input : inputs) {
        std::string input_name = input.value("name", "");
        std::string input_id = input.value("id", "");

        for (const auto& label : labels) {
            std::string label_name = label.value("name", "");
            std::string label_text = label.value("text", "");

            // Remove "lbl" prefix from label name to match field name
            std::string label_base = label_name;
            if (label_base.find("lbl") == 0) {
                label_base = label_base.substr(3);
            }

            // Check if names match
            if (input_name.find(label_base) != std::string::npos ||
                label_base.find(input_name) != std::string::npos) {
                label_map[input_id] = label_text;
                break;
            }
        }
    }

    return label_map;
}

// Helper: Get state icon for an element
static std::string get_state_icon(bool enabled, bool changeable, const std::string& /* type */) {
    if (!enabled) {
        return "Disabled";
    } else if (!changeable) {
        return "Read-only";
    } else {
        return "Editable";
    }
}

// Helper: Format checkbox/radio button state
static std::string format_boolean_state(const std::string& type, bool selected) {
    if (type.find("CheckBox") != std::string::npos) {
        return selected ? "[X]" : "[ ]";
    } else if (type.find("RadioButton") != std::string::npos) {
        return selected ? "(•)" : "( )";
    }
    return "";
}

// Helper: Escape markdown special characters in text
static std::string escape_markdown(const std::string& text) {
    std::string result = text;
    // Replace pipe characters that would break tables
    size_t pos = 0;
    while ((pos = result.find("|", pos)) != std::string::npos) {
        result.replace(pos, 1, "\\|");
        pos += 2;
    }
    return result;
}

// Helper: Format menu structure recursively (compact mode)
static void format_menu(const json& menu, std::ostringstream& oss, const std::string& prefix = "", int level = 0) {
    std::string text = menu.value("text", menu.value("name", ""));
    if (text.empty()) return;  // Skip empty menu items (separators)

    auto children = menu.value("children", json::array());

    // Skip System and Help menus at top level (always the same)
    if (level == 0 && (text == "System" || text == "Help")) {
        return;
    }

    // Format this menu item
    if (level == 0) {
        // Top-level menu: show items inline
        if (!children.empty()) {
            oss << "- **" << escape_markdown(text) << "**: ";
            // Show first-level children inline
            std::vector<std::string> items;
            for (const auto& child : children) {
                std::string child_text = child.value("text", child.value("name", ""));
                if (!child_text.empty()) {
                    items.push_back(child_text);
                }
            }
            for (size_t i = 0; i < items.size(); ++i) {
                oss << items[i];
                if (i < items.size() - 1) oss << ", ";
            }
            oss << "\n";
        } else {
            oss << "- **" << escape_markdown(text) << "**\n";
        }
    }
    // Skip deeper levels in compact mode
}

// Helper: Format table/grid element with detailed information
static void format_table_element(std::ostringstream& oss, const json& elem) {
    std::string type = elem.value("type", "");
    std::string name = elem.value("name", "");
    std::string id = elem.value("id", "");
    int child_count = elem.value("child_count", 0);
    int row_count = elem.value("row_count", 0);
    int visible_rows = elem.value("visible_rows", 0);

    oss << "### " << (name.empty() ? "Table" : escape_markdown(name)) << "\n\n";
    oss << "| Property | Value |\n";
    oss << "|----------|-------|\n";
    oss << "| **Technical ID** | `" << id << "` |\n";
    oss << "| **Type** | " << type << " |\n";

    if (row_count > 0) {
        oss << "| **Total Rows** | " << row_count << " |\n";
    }
    if (visible_rows > 0) {
        oss << "| **Visible Rows** | " << visible_rows << " |\n";
    }
    if (child_count > 0) {
        oss << "| **Elements** | " << child_count << " |\n";
    }
    oss << "\n";

    // Add usage examples based on type
    if (type == "GuiGridView") {
        oss << "**Usage Examples:**\n";
        oss << "```python\n";
        oss << "# Read cell value\n";
        oss << "fairyfly get '" << id << "' --row 0 --column 'COLUMN_NAME'\n\n";
        oss << "# Set cell value\n";
        oss << "fairyfly fill '" << id << "' 'new_value' --row 0 --column 'COLUMN_NAME'\n\n";
        oss << "# Click cell to select row\n";
        oss << "fairyfly click '" << id << "' --row 0 --column 'COLUMN_NAME'\n";
        oss << "```\n\n";
    } else if (type == "GuiTableControl") {
        oss << "**Usage Examples:**\n";
        oss << "```python\n";
        oss << "# Access cell via element path\n";
        oss << "fairyfly get '" << id << "/txt[row,col]'\n\n";
        oss << "# Navigate to row\n";
        oss << "fairyfly click '" << id << "' --row 0\n";
        oss << "```\n\n";
    }
}

// Helper: Format tree element with navigation hints
// Note: Currently unused (kept for reference in commented code below)
[[maybe_unused]] static void format_tree_element(std::ostringstream& oss, const json& elem) {
    std::string type = elem.value("type", "");
    std::string name = elem.value("name", "");
    std::string id = elem.value("id", "");
    int child_count = elem.value("child_count", 0);

    oss << "### " << (name.empty() ? "Tree" : escape_markdown(name)) << "\n\n";
    oss << "| Property | Value |\n";
    oss << "|----------|-------|\n";
    oss << "| **Technical ID** | `" << id << "` |\n";
    oss << "| **Type** | " << type << " |\n";

    if (child_count > 0) {
        oss << "| **Node Count** | " << child_count << " |\n";
    }
    oss << "\n";

    // Add tree navigation examples
    oss << "**Tree Navigation:**\n";
    oss << "```python\n";
    oss << "# Select node by path\n";
    oss << "fairyfly click '" << id << "' --node 'path/to/node'\n\n";
    oss << "# Expand/collapse node\n";
    oss << "fairyfly click '" << id << "' --node 'node_key' --action expand\n\n";
    oss << "# Double-click to execute\n";
    oss << "fairyfly click '" << id << "' --node 'node_key' --double-click\n";
    oss << "```\n\n";
}

static std::string format_screen_markdown(const json& data) {
    std::ostringstream oss;

    // Screen title, transaction code, and ID
    std::string title = data.value("title", "");
    std::string transaction = data.value("transaction", "");
    std::string screen_id = data.value("screen_id", "");

    oss << "# ";
    if (!title.empty()) {
        oss << title;
    } else {
        oss << "Screen";
    }
    oss << "\n\n";

    if (!transaction.empty()) {
        oss << "**Transaction:** `" << transaction << "`\n\n";
    }

    oss << "**Screen ID:** `" << screen_id << "`\n\n";

    // Check if we have hierarchy data
    if (!data.contains("hierarchy")) {
        oss << "_No screen structure available_\n";
        return oss.str();
    }

    const auto& hierarchy = data["hierarchy"];

    // Build label-to-field mapping
    auto label_map = build_label_field_map(hierarchy);

    // Menu bar section - extract from "other" category
    if (hierarchy.contains("other") && !hierarchy["other"].empty()) {
        // Find the menu bar (GuiMenubar) and extract top-level menus
        std::vector<json> menu_items;
        for (const auto& elem : hierarchy["other"]) {
            std::string type = elem.value("type", "");
            std::string id = elem.value("id", "");
            // Top-level menus have pattern: /wnd[0]/mbar/menu[N]
            if (type == "GuiMenu" && id.find("/mbar/menu[") != std::string::npos) {
                // Check if this is a direct child of mbar (top-level menu)
                size_t mbar_pos = id.find("/mbar/menu[");
                size_t after_menu = id.find("]", mbar_pos + 11);
                if (after_menu != std::string::npos && after_menu + 1 == id.length()) {
                    menu_items.push_back(elem);
                }
            }
        }

        if (!menu_items.empty()) {
            oss << "## Menu Bar\n\n";
            for (const auto& menu : menu_items) {
                format_menu(menu, oss);
            }
            oss << "\n";
        }
    }

    // Toolbar section (compact format)
    if (hierarchy.contains("toolbar") && !hierarchy["toolbar"].empty()) {
        oss << "## Toolbar\n\n";
        int toolbar_count = static_cast<int>(hierarchy["toolbar"].size());
        oss << "**System & Application Toolbars:** " << toolbar_count << " items\n\n";
    }

    // Form fields section with table format
    if (hierarchy.contains("form_fields") && !hierarchy["form_fields"].empty()) {
        oss << "## Form Fields\n\n";
        oss << "| Field | Value | State | Technical ID |\n";
        oss << "|-------|-------|-------|-------------|\n";

        for (const auto& elem : hierarchy["form_fields"]) {
            std::string type = elem.value("type", "");

            // Skip labels - they're used for semantic context only
            if (type == "GuiLabel") {
                continue;
            }

            std::string id = elem.value("id", "");
            std::string text = elem.value("text", "");
            std::string tooltip = elem.value("tooltip", "");
            bool changeable = elem.value("changeable", false);
            bool enabled = elem.value("enabled", true);
            bool selected = elem.value("selected", false);

            // Get label for this field
            std::string label = label_map.count(id) > 0 ? label_map[id] : "";
            if (label.empty()) {
                label = elem.value("name", "Field");
            }

            // Field name with optional tooltip as description
            oss << "| **" << escape_markdown(label) << "**";
            if (!tooltip.empty()) {
                oss << "<br>_" << escape_markdown(tooltip) << "_";
            }
            oss << " | ";

            // Value column
            if (type.find("CheckBox") != std::string::npos || type.find("RadioButton") != std::string::npos) {
                oss << format_boolean_state(type, selected);
            } else if (type.find("Password") != std::string::npos) {
                oss << "●●●●●●●●";
            } else if (text.empty()) {
                oss << "(empty)";
            } else {
                oss << escape_markdown(text);
            }
            oss << " | ";

            // State column
            oss << get_state_icon(enabled, changeable, type);
            if (!enabled) {
                oss << " Disabled";
            } else if (!changeable) {
                oss << " Read-only";
            } else {
                oss << " Editable";
            }
            oss << " | ";

            // Technical ID column
            oss << "`" << id << "` |\n";
        }
        oss << "\n";
    }

    // Buttons section with table format
    if (hierarchy.contains("buttons") && !hierarchy["buttons"].empty()) {
        oss << "## Action Buttons\n\n";
        oss << "| Button | Shortcut | State | Technical ID |\n";
        oss << "|--------|----------|-------|-------------|\n";

        for (const auto& elem : hierarchy["buttons"]) {
            std::string label = elem.value("text", "");
            if (label.empty()) {
                label = elem.value("name", "Button");
            }
            std::string tooltip = elem.value("tooltip", "");
            std::string id = elem.value("id", "");
            bool enabled = elem.value("enabled", true);

            // Extract shortcut from tooltip (usually in parentheses)
            std::string shortcut = "";
            if (!tooltip.empty()) {
                size_t start = tooltip.find("(");
                size_t end = tooltip.find(")");
                if (start != std::string::npos && end != std::string::npos && end > start) {
                    shortcut = tooltip.substr(start + 1, end - start - 1);
                }
            }

            oss << "| **" << escape_markdown(label) << "** | ";
            oss << escape_markdown(shortcut) << " | ";
            oss << (enabled ? "Enabled" : "Disabled") << " | ";
            oss << "`" << id << "` |\n";
        }
        oss << "\n";
    }

    // Tabs section with expanded content support
    bool tabs_expanded = data.value("tabs_expanded", false);

    if (tabs_expanded && data.contains("tabs_content") && !data["tabs_content"].empty()) {
        // Display expanded tabs with full content
        oss << "## 📑 Tabs (Expanded)\n\n";

        for (const auto& tab_data : data["tabs_content"]) {
            std::string tab_name = tab_data.value("tab_name", "Unnamed Tab");
            std::string tab_id = tab_data.value("tab_id", "");
            int elem_count = tab_data.value("element_count", 0);

            oss << "### " << escape_markdown(tab_name) << "\n";
            oss << "**Technical ID:** `" << tab_id << "` | **Elements:** " << elem_count << "\n\n";

            // Extract and display tab-specific fields
            if (tab_data.contains("hierarchy") && tab_data["hierarchy"].contains("form_fields")) {
                const auto& fields = tab_data["hierarchy"]["form_fields"];
                auto tab_label_map = build_label_field_map(tab_data["hierarchy"]);

                if (!fields.empty()) {
                    oss << "| Field | Value | State | Technical ID |\n";
                    oss << "|-------|-------|-------|-------------|\n";

                    for (const auto& elem : fields) {
                        std::string type = elem.value("type", "");
                        std::string id = elem.value("id", "");
                        std::string text = elem.value("text", "");
                        std::string name = elem.value("name", "");

                        // Skip labels (they're paired with fields)
                        if (type == "GuiLabel") continue;

                        // Get label text for this field
                        std::string label_text = "";
                        if (tab_label_map.count(id)) {
                            label_text = tab_label_map[id];
                        } else if (!name.empty()) {
                            label_text = name;
                        }

                        // Format value based on type
                        std::string value_str = escape_markdown(text);
                        if (value_str.empty()) {
                            value_str = "_empty_";
                        }

                        // Get state icon
                        bool enabled = elem.value("enabled", true);
                        bool changeable = elem.value("changeable", false);
                        std::string state_icon = get_state_icon(enabled, changeable, type);

                        // Build row
                        oss << "| ";
                        if (!label_text.empty()) {
                            oss << "**" << escape_markdown(label_text) << "**";
                        } else {
                            oss << escape_markdown(name);
                        }
                        oss << " | " << value_str;
                        oss << " | " << state_icon;
                        oss << " | `" << id << "` |\n";
                    }
                    oss << "\n";
                }
            }

            // Display buttons if any
            if (tab_data.contains("hierarchy") && tab_data["hierarchy"].contains("buttons")) {
                const auto& buttons = tab_data["hierarchy"]["buttons"];
                if (!buttons.empty()) {
                    oss << "**Buttons:** ";
                    bool first = true;
                    for (const auto& btn : buttons) {
                        if (!first) oss << " | ";
                        first = false;

                        std::string label = btn.value("text", btn.value("name", ""));
                        std::string id = btn.value("id", "");
                        oss << escape_markdown(label) << " `" << id << "`";
                    }
                    oss << "\n\n";
                }
            }
        }
    } else if (hierarchy.contains("tabs") && !hierarchy["tabs"].empty()) {
        // Display non-expanded tabs summary
        oss << "## 📑 Tabs\n\n";

        // Try to find the TabStrip container
        std::string tabstrip_id = "";
        for (const auto& elem : hierarchy["tabs"]) {
            std::string id = elem.value("id", "");
            if (id.find("tabs") != std::string::npos) {
                // Extract parent TabStrip ID
                size_t pos = id.find("/tabp");
                if (pos != std::string::npos) {
                    tabstrip_id = id.substr(0, pos);
                    break;
                }
            }
        }

        if (!tabstrip_id.empty()) {
            oss << "**Tab Container:** `" << tabstrip_id << "`\n\n";
        }

        oss << "| Tab Name | Status | Technical ID |\n";
        oss << "|----------|--------|-------------|\n";

        for (const auto& elem : hierarchy["tabs"]) {
            std::string label = elem.value("text", "");
            if (label.empty()) {
                label = elem.value("name", "Tab");
            }
            std::string id = elem.value("id", "");

            // Assume first tab or tab with type "GuiTabStrip" is active
            // In reality, would need to check Selected property
            bool is_active = false;  // Would need additional metadata

            oss << "| " << (is_active ? "**" : "") << escape_markdown(label) << (is_active ? "**" : "") << " | ";
            oss << (is_active ? "▶️ Active" : "Available") << " | ";
            oss << "`" << id << "` |\n";
        }
        oss << "\n";

        oss << "_Note: Tab expansion is enabled by default. Use `--no-tabs` flag to skip tab content capture for faster execution._\n\n";
    }

    // Tables and Grids section - use new formatters
    if (hierarchy.contains("tables") && !hierarchy["tables"].empty()) {
        oss << "## Tables and Grids\n\n";

        formatters::TableFormatter table_formatter;
        for (const auto& elem : hierarchy["tables"]) {
            std::string type = elem.value("type", "");
            std::string subtype = elem.value("subtype", "");

            // Use TableFormatter for GuiGridView, GuiTableControl, or GuiShell with GridView subtype
            if (table_formatter.can_format(type) ||
                (type == "GuiShell" && subtype == "GridView") ||
                elem.contains("table_data")) {
                // Use new formatter with table_data support
                table_formatter.format_to_markdown(elem, oss);
            } else {
                // Fallback to old formatting
                format_table_element(oss, elem);
            }
        }
    }

    // Trees section - DISABLED: Trees now shown in Screen Structure section below
    // (Keeping this code commented for reference, can be removed later)
    /*
    bool has_trees = false;
    if (hierarchy.contains("other")) {
        formatters::TreeFormatter tree_formatter;

        for (const auto& elem : hierarchy["other"]) {
            std::string type = elem.value("type", "");
            bool is_tree = (type.find("Tree") != std::string::npos) ||
                          (type == "GuiShell" && elem.contains("tree_data"));

            if (is_tree) {
                if (!has_trees) {
                    oss << "## Tree Controls\n\n";
                    has_trees = true;
                }

                if (tree_formatter.can_format(type) || elem.contains("tree_data")) {
                    // Use new formatter with tree_data support
                    tree_formatter.format_to_markdown(elem, oss);
                } else {
                    // Fallback to old formatting
                    format_tree_element(oss, elem);
                }
            }
        }
    }
    */

    // Semantic Screen Structure - walk full hierarchy recursively, show only semantic elements
    if (hierarchy.contains("other") && !hierarchy["other"].empty()) {
        oss << "## Screen Structure\n\n";
        oss << "_Showing semantic elements only (layout containers hidden):_\n\n";

        auto& registry = sap::ElementRendererRegistry::instance();

        // Build set of all element IDs to identify parent-child relationships
        std::set<std::string> all_ids;
        std::set<std::string> child_ids;

        for (const auto& elem : hierarchy["other"]) {
            std::string id = elem.value("id", "");
            if (!id.empty()) {
                all_ids.insert(id);
            }
        }

        // Collect all IDs that appear as children of other elements
        for (const auto& elem : hierarchy["other"]) {
            if (elem.contains("children") && elem["children"].is_array()) {
                for (const auto& child : elem["children"]) {
                    std::string child_id = child.value("id", "");
                    if (!child_id.empty()) {
                        child_ids.insert(child_id);
                    }
                }
            }
        }

        // Only render elements that are NOT children of other elements (i.e., true top-level)
        for (const auto& elem : hierarchy["other"]) {
            std::string id = elem.value("id", "");
            bool is_top_level = (id.empty() || child_ids.find(id) == child_ids.end());

            if (is_top_level && sap::SemanticClassifier::should_display(elem)) {
                std::string rendered = registry.render_to_markdown(elem, 0);
                if (!rendered.empty()) {
                    oss << rendered;
                }
            }
        }
    }

    // Other elements summary (excluding menus which are shown in menu bar section)
    if (hierarchy.contains("other") && !hierarchy["other"].empty()) {
        // Count element types (excluding menus which are already shown)
        std::map<std::string, int> type_counts;
        for (const auto& elem : hierarchy["other"]) {
            std::string type = elem.value("type", "Unknown");
            // Skip menus as they're shown in the menu bar section
            if (type != "GuiMenu") {
                type_counts[type]++;
            }
        }

        if (!type_counts.empty()) {
            oss << "## Other Elements\n\n";
            oss << "| Element Type | Count |\n";
            oss << "|--------------|-------|\n";
            for (const auto& [type, count] : type_counts) {
                oss << "| " << type << " | " << count << " |\n";
            }
            oss << "\n";
            oss << "_These elements include containers, shells, status bars, and other UI components._\n\n";
        }
    }

    // Summary stats
    int total_elements = data.value("element_count", 0);
    oss << "---\n";
    oss << "**Screen Elements:** " << total_elements << " top-level\n";

    return oss.str();
}

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
                oss << format_screen_markdown(response["data"]);

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
    }

    return result.to_json().dump(2);
}

}  // namespace cli
}  // namespace fairyfly
