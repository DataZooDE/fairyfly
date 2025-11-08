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

public:
    /// Constructor - initializes COM but doesn't connect to SAP
    ComAutomationEngine();

    ~ComAutomationEngine() override = default;

    // Connection management - two modes
    Result attach_by_click(int timeout_seconds = 10) override;
    Result launch_connection(const std::string& connection_name) override;
    Result disconnect() override;
    bool is_connected() const override;
    bool validate_session(const std::string& session_id) const override;

    // Transaction execution
    Result execute_transaction(const std::string& tcode) override;

    // Element interaction
    Result click_element(const ElementId& element) override;
    Result fill_field(const ElementId& element, const std::string& value) override;
    Result read_field(const ElementId& element) override;

    // Screen operations
    Result read_screen(bool include_structure = true) override;
    Result read_screen_with_tabs() override;
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

private:
    /// Normalize SAP GUI path from 1-indexed to 0-indexed
    /// SAP GUI returns paths like "/app/con[1]/ses[0]" but we use 0-indexed "/app/con[0]/ses[0]"
    static std::string normalize_sap_path(const std::string& sap_path);
};

} // namespace sap
} // namespace fairyfly
