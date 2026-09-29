#pragma once

#include "core.h"
#include <memory>

namespace fairyfly {

// Forward declarations
namespace cli {
    struct ScreenshotOptions;
}
namespace sap {
    struct ScreenFindOptions;
}

class AutomationEngine {
public:
    virtual ~AutomationEngine() = default;

    // Connection management - two modes supported
    /// Attach to existing SAP GUI window via mouse click
    /// User has 'timeout_seconds' to click on SAP window
    virtual Result attach_by_click(int timeout_seconds = 10) = 0;
    virtual Result attach_by_session_id(const std::string& session_id) = 0;

    /// Launch new SAP connection using SAP Logon configured connection name
    virtual Result launch_connection(const std::string& connection_name, bool allow_sapshcut = false) = 0;

    virtual Result disconnect() = 0;
    virtual Result close_current_session() = 0;
    virtual bool is_connected() const = 0;

    /// Validate that a session still exists and is alive
    /// \param session_id Full session path (e.g., /app/con[0]/ses[0])
    /// \return true if session exists and is alive, false otherwise
    virtual bool validate_session(const std::string& session_id,
                                  const std::string& server_session_key = "") const = 0;

    /// Cleanup may delete a saved connection; implementations must throw when
    /// the live session's absence cannot be confirmed.
    virtual bool validate_session_for_cleanup(const std::string& session_id,
                                              const std::string& server_session_key = "") const = 0;

    /// Bind subsequent operations to the specified live SAP GUI session.
    virtual bool select_session(const std::string& session_id,
                                const std::string& server_session_key = "") = 0;
    virtual std::string current_server_session_key() const = 0;

    // Transaction execution
    virtual Result execute_transaction(const std::string& tcode) = 0;

    // Element interaction
    virtual Result click_element(const ElementId& element) = 0;
    virtual Result fill_field(const ElementId& element, const std::string& value) = 0;
    virtual Result read_field(const ElementId& element) = 0;
    virtual Result press_toolbar_button(const ElementId& toolbar_element, const std::string& button_id) = 0;
    virtual Result press_f4(const ElementId& element) = 0;

    // Screen operations
    virtual Result read_screen(bool include_structure = true, bool skip_trees = false,
                               int max_rows = 20) = 0;
    virtual Result read_screen_with_tabs(bool skip_trees = false, int max_rows = 20,
                                         const std::string& only_tab = "") = 0;
    virtual Result find_screen(const sap::ScreenFindOptions& query) = 0;
    virtual Result capture_screenshot(const cli::ScreenshotOptions& options) = 0;

    // Enumeration and diagnostics
    virtual nlohmann::json get_application_info() const = 0;

    // Window management
    virtual WindowId get_active_window_id() const = 0;
    virtual ElementId resolve_element_path(const ElementId& element) const = 0;

    // Static factory
    static std::unique_ptr<AutomationEngine> create();
};

} // namespace fairyfly
