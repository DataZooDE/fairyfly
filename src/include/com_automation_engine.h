#pragma once

#include "automation_engine.h"
#include "com/wrapper.h"
#include "connection_launcher.h"
#include "screenshot_handler.h"
#include "screen_reader.h"
#include "element_metadata_extractor.h"
#include <memory>
#include <optional>

namespace fairyfly {
namespace sap {

/// Read one GUI element as a CLI value, including bounded ABAP editor source.
json read_element_value(const ComGuiElementPtr& element);

/// Observe exact SAP GUI session identity. Throw when enumeration is uncertain.
bool session_present_checked(const ComGuiApplicationPtr& app,
                             const std::string& session_id,
                             const std::string& server_session_key);

/// COM-based SAP GUI automation engine
/// Implements the AutomationEngine interface using Windows COM
/// for actual SAP GUI interactions
class ComAutomationEngine : public AutomationEngine {
private:
    ComGuiApplicationPtr app_;
    ComGuiConnectionPtr current_connection_;
    ComGuiSessionPtr current_session_;

    // Extracted service classes
    std::unique_ptr<ConnectionLauncher> connection_launcher_;
    std::unique_ptr<ScreenshotHandler> screenshot_handler_;
    std::unique_ptr<ScreenReader> screen_reader_;

    // Helper to ensure connection exists
    ComGuiConnectionPtr ensure_connection();

    // Helper to ensure session exists
    ComGuiSessionPtr ensure_session();

    // Initialize service classes (called after session is available)
    void initialize_services();

    // Publish a session only after its dependent services are ready.
    void bind_session(ComGuiConnectionPtr connection, ComGuiSessionPtr session);

    std::pair<ComGuiConnectionPtr, ComGuiSessionPtr> find_session_by_id(
        const std::string& session_id) const;

public:
    /// Constructor - initializes COM but doesn't connect to SAP
    ComAutomationEngine();

    ~ComAutomationEngine() override = default;

    // Connection management - two modes
    Result attach_by_click(int timeout_seconds = 10) override;
    Result attach_by_session_id(const std::string& session_id) override;
    Result launch_connection(const std::string& connection_name, bool allow_sapshcut = false) override;
    Result disconnect() override;
    Result close_current_session() override;
    bool is_connected() const override;
    bool validate_session(const std::string& session_id,
                          const std::string& server_session_key = "") const override;
    bool validate_session_for_cleanup(const std::string& session_id,
                                      const std::string& server_session_key = "") const override;
    bool select_session(const std::string& session_id,
                        const std::string& server_session_key = "") override;
    std::string current_server_session_key() const override;

    // Transaction execution
    Result execute_transaction(const std::string& tcode) override;

    // Element interaction
    Result click_element(const ElementId& element) override;
    Result fill_field(const ElementId& element, const std::string& value) override;
    Result fill_grid_cell(const ElementId& element, int row, const std::string& column,
                          const std::string& value, bool checkbox, bool commit);
    Result select_grid_row(const ElementId& element, int row, const std::string& column);
    Result read_field(const ElementId& element) override;
    Result press_toolbar_button(const ElementId& toolbar_element, const std::string& button_id) override;
    Result press_f4(const ElementId& element) override;

    // Screen operations
    Result read_screen(bool include_structure = true, bool skip_trees = false,
                       int max_rows = 20) override;
    Result read_screen_with_tabs(bool skip_trees = false, int max_rows = 20,
                                 const std::string& only_tab = "") override;
    Result find_screen(const ScreenFindOptions& query) override;
    Result capture_screenshot(const cli::ScreenshotOptions& options) override;

    // Getters for testing/debugging
    ComGuiApplicationPtr get_app() const { return app_; }
    ComGuiConnectionPtr get_connection() const { return current_connection_; }
    ComGuiSessionPtr get_session() const { return current_session_; }

    // Enumerate all connections, sessions, and windows
    nlohmann::json get_application_info() const;

    // Window management helpers
    WindowId get_active_window_id() const;
    ElementId resolve_element_path(const ElementId& element) const;

    /// Detect if a modal dialog is currently active
    /// Returns dialog metadata if modal dialog exists (non-wnd[0]), or empty json if on main window
    /// Dialog metadata includes: window_id, type, title, message, buttons
    nlohmann::json detect_modal_dialog() const;

};

} // namespace sap
} // namespace fairyfly
