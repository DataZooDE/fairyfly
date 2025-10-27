#pragma once

#include "core.h"
#include <memory>

namespace fairyfly {

class AutomationEngine {
public:
    virtual ~AutomationEngine() = default;

    // Connection management - two modes supported
    /// Attach to existing SAP GUI window via mouse click
    /// User has 'timeout_seconds' to click on SAP window
    virtual Result attach_by_click(int timeout_seconds = 10) = 0;

    /// Launch new SAP connection using SAP Logon configured connection name
    virtual Result launch_connection(const std::string& connection_name) = 0;

    virtual Result disconnect() = 0;
    virtual bool is_connected() const = 0;

    /// Validate that a session still exists and is alive
    /// \param session_id Full session path (e.g., /app/con[0]/ses[0])
    /// \return true if session exists and is alive, false otherwise
    virtual bool validate_session(const std::string& session_id) const = 0;

    // Transaction execution
    virtual Result execute_transaction(const std::string& tcode) = 0;

    // Element interaction
    virtual Result click_element(const ElementId& element) = 0;
    virtual Result fill_field(const ElementId& element, const std::string& value) = 0;
    virtual Result read_field(const ElementId& element) = 0;

    // Screen operations
    virtual Result read_screen(bool include_structure = true) = 0;
    virtual Result read_screen_with_tabs() = 0;
    virtual Result capture_screenshot() = 0;

    // Enumeration and diagnostics
    virtual nlohmann::json get_application_info() const = 0;

    // Static factory
    static std::unique_ptr<AutomationEngine> create();
};

} // namespace fairyfly
