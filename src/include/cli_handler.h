#pragma once

#include "core.h"
#include "automation_engine.h"
#include "connection_manager.h"
#include "credential_store.h"
#include "audit_log.h"
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

/// Compose the result of `launch --login` from the (successful) launch result and the
/// login result. Success: launch data plus a `login` object (transaction, credential_source,
/// warnings). Login failure: the login error, carrying the launch data under error.launch
/// and `connection_open: true` so the caller knows the connection is still open.
Result compose_launch_login_result(Result launch, const Result& login);

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

/// Screen read filter options
struct ScreenFilterOptions {
    bool only_buttons = false;                ///< Show only GuiButton elements
    bool only_fields = false;                 ///< Show only changeable GuiTextField, GuiCTextField, GuiPasswordField
    bool only_editable = false;               ///< Show only changeable=true fields
    bool only_f4_fields = false;              ///< Show only fields with has_f4_help=true
    std::optional<std::string> text_contains; ///< Filter by text/tooltip containing string (case-insensitive)
    std::optional<std::string> id_contains;   ///< Filter by element ID containing string
    std::optional<std::string> type_filter;   ///< Filter by exact element type
    bool first_match_only = false;            ///< Return only first matching element
};

void apply_screen_filters(nlohmann::json& screen_data, const ScreenFilterOptions& filters);

/// CLI command handler with integration to automation engine
class CommandHandler {
private:
    std::unique_ptr<AutomationEngine> engine_;
    std::unique_ptr<ConnectionManager> conn_mgr_;
    SessionId current_session_;
    bool read_only_ = false;  ///< --read-only guard: refuse state-changing actions
    bool batch_mode_ = false; ///< inside `batch`: stdin belongs to the batch file, no prompts
    std::unique_ptr<cred::CredentialStore> credential_store_;

    /// Look up type/text/tooltip of an element (best effort, empty on failure).
    void describe_element(const std::string& element_id, std::string& type, std::string& text, std::string& tooltip);

    /// In read-only mode: READ_ONLY_REFUSED result when the element's action changes SAP state.
    std::optional<Result> guard_element(const std::string& element_id);

    /// Resolve and validate connection before command execution
    /// \param explicit_conn_id Optional connection ID from --connection flag
    /// \return Connection if successful, error Result otherwise
    ResultT<Connection> resolve_and_validate_connection(std::optional<int> explicit_conn_id);

    /// When several cache files exist, delete those whose SAP session is
    /// confirmed gone so auto-detect is not blocked by stale files.
    /// Entries whose liveness cannot be determined are kept.
    std::vector<int> prune_dead_entries();

public:
    explicit CommandHandler(std::unique_ptr<cred::CredentialStore> store = nullptr);

    /// Credential store used by login and the credentials command (Windows Credential Manager by default).
    cred::CredentialStore& credential_store() { return *credential_store_; }

    /// Batch mode disables interactive/stdin secret prompts (credentials set / import-env).
    void set_batch_mode(bool batch_mode) { batch_mode_ = batch_mode; }
    bool batch_mode() const { return batch_mode_; }

    /// Enable or disable the read-only guard (refuses saves, deletes, releases, ...).
    void set_read_only(bool read_only) { read_only_ = read_only; }
    bool read_only() const { return read_only_; }

    /// Non-secret facts about the current SAP session for the audit trail (empty when none).
    audit::SapFacts audit_facts() const noexcept;

    // Connection management - two modes
    Result handle_attach(int timeout_seconds, std::optional<std::string> session_id = std::nullopt);
    /// With `login`, runs the same logon path as `login` after the launch succeeded
    /// (credential_name empty = Credential Manager entry keyed by the connection name).
    Result handle_launch(const std::string& connection_name, bool allow_sapshcut = false,
                         bool login = false, const std::string& credential_name = {});
    Result handle_login(const std::string& credentials_file, std::optional<int> connection_id,
                        bool from_stdin = false, const std::string& credential_name = "");
    Result handle_disconnect(std::optional<int> connection_id, bool close_session = false);

    // Connection listing and management
    Result handle_connections_list(bool cleanup);

    // Transaction execution
    Result handle_transaction(const std::string& tcode, std::optional<int> connection_id);

    // Element interactions
    Result handle_click(const std::string& element_id, std::optional<int> connection_id,
                        bool wait_for_window = false, int timeout_ms = 5000,
                        const std::string& node_key = "", const std::string& tree_action = "doubleclick",
                        const std::string& menu_item = "", std::optional<int> row = std::nullopt,
                        const std::string& column = "", bool doubleclick = false);
    Result handle_fill(const std::string& element_id, const std::string& value,
                       std::optional<int> connection_id, std::optional<int> row = std::nullopt,
                       const std::string& column = "", bool checkbox = false, bool commit = false,
                       bool allow_fill = false);
    Result handle_read_field(const std::string& element_id, std::optional<int> connection_id, bool list_nodes = false);
    Result handle_press_f4(const std::string& element_id, std::optional<int> connection_id);

    // Screen operations
    Result handle_screen_read(bool include_children, std::optional<int> connection_id, bool expand_tabs = false,
                             const ScreenFilterOptions& filters = {}, bool skip_trees = false,
                             bool compact = false, int max_rows = 20,
                             const std::string& only_tab = "", bool probe_all = false);
    Result handle_screen_find(const sap::ScreenFindOptions& query,
                              std::optional<int> connection_id);
    Result handle_screenshot(std::optional<int> connection_id, const ScreenshotOptions& options);

    // Window-level actions (implemented in cli_handler_window_actions.cpp)
    Result handle_send_key(const std::string& key, const std::string& window,
                           std::optional<int> connection_id);
    Result handle_close(int vkey, std::optional<int> connection_id);
    Result handle_screen_menu(const std::string& select_path, const std::string& window,
                              std::optional<int> connection_id);

    // Credential store commands (implemented in cli_handler_credentials.cpp)
    Result handle_credentials_set(const std::string& connection, const std::string& user,
                                  const std::string& client, const std::string& language,
                                  bool password_stdin);
    Result handle_credentials_list();
    Result handle_credentials_delete(const std::string& connection);
    Result handle_credentials_import_env(const std::string& path, const std::string& connection,
                                         bool delete_file);

    // Diagnostic and enumeration
    Result handle_list_all();
    Result handle_doctor();

    // Getters
    const SessionId& get_current_session() const { return current_session_; }
    ConnectionManager* get_connection_manager() { return conn_mgr_.get(); }
};

/// Convert result to different output formats
/// @param result The result to format
/// @param format The output format
/// @param verbose_errors If true, include detailed error suggestions; if false, show compact errors (default)
std::string format_output(const Result& result, OutputFormat format, bool verbose_errors = false);

}  // namespace cli
}  // namespace fairyfly
