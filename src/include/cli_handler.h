#pragma once

#include "core.h"
#include "automation_engine.h"
#include "connection_manager.h"
#include <string>
#include <memory>
#include <optional>

namespace fairyfly {
namespace cli {

/// Output format for responses
enum class OutputFormat {
    Json,
    Markdown,
    PlainText,
    Toon
};

/// Screenshot capture options
struct ScreenshotOptions {
    std::string output_file;       ///< Output file path (empty = auto-generate, "-" = stdout)
    std::string format = "png";    ///< Output format: png, base64
    std::string scale;             ///< Scale factor (0.0-1.0 float or width in pixels as int)
    std::optional<int> crop_x;     ///< X position for subsection capture
    std::optional<int> crop_y;     ///< Y position for subsection capture
    std::optional<int> crop_width; ///< Width for subsection capture
    std::optional<int> crop_height;///< Height for subsection capture
    bool show = false;             ///< Display screenshot in window after capture
};

/// CLI command handler with integration to automation engine
class CommandHandler {
private:
    std::unique_ptr<AutomationEngine> engine_;
    std::unique_ptr<ConnectionManager> conn_mgr_;
    SessionId current_session_;

    /// Resolve and validate connection before command execution
    /// \param explicit_conn_id Optional connection ID from --connection flag
    /// \return Connection if successful, error Result otherwise
    ResultT<Connection> resolve_and_validate_connection(std::optional<int> explicit_conn_id);

public:
    CommandHandler();

    // Connection management - two modes
    Result handle_attach(int timeout_seconds);
    Result handle_launch(const std::string& connection_name);
    Result handle_disconnect(std::optional<int> connection_id);

    // Connection listing and management
    Result handle_connections_list(bool cleanup);

    // Transaction execution
    Result handle_transaction(const std::string& tcode, std::optional<int> connection_id);

    // Element interactions
    Result handle_click(const std::string& element_id, std::optional<int> connection_id,
                        bool wait_for_window = false, int timeout_ms = 5000);
    Result handle_fill(const std::string& element_id, const std::string& value, std::optional<int> connection_id);
    Result handle_read_field(const std::string& element_id, std::optional<int> connection_id);

    // Screen operations
    Result handle_screen_read(bool include_children, std::optional<int> connection_id, bool expand_tabs = false);
    Result handle_screenshot(std::optional<int> connection_id, const ScreenshotOptions& options);

    // Diagnostic and enumeration
    Result handle_list_all();

    // Getters
    const SessionId& get_current_session() const { return current_session_; }
    ConnectionManager* get_connection_manager() { return conn_mgr_.get(); }
};

/// Convert result to different output formats
std::string format_output(const Result& result, OutputFormat format);

}  // namespace cli
}  // namespace fairyfly
