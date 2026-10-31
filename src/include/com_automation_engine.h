#pragma once

#include "action_status.h"
#include "include/sap_gui_scripting.h"
#include "audit_log.h"
#include "automation_engine.h"
#include "com/wrapper.h"
#include "connection_launcher.h"
#include "screenshot_handler.h"
#include "screen_reader.h"
#include "element_metadata_extractor.h"
#include <functional>
#include <memory>
#include <cstdint>
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
class ComAutomationEngine : public AutomationEngine, public SapGuiScripting {
public:
    /// Where the engine gets SAP Logon's GuiApplication: the running one or a SapLogonNotRunningException /
    /// ComException (default ComGuiApplication::create). Asked again before every call (refresh_application).
    using ApplicationSource = std::function<ComGuiApplicationPtr()>;

private:
    ApplicationSource source_;
    ComGuiApplicationPtr app_;
    EngineStatus status_{false, false, "SAP Logon has not been looked up yet"};
    std::uint64_t generation_ = 0;  ///< ++ at every adopted SAP Logon
    ComGuiConnectionPtr current_connection_;
    ComGuiSessionPtr current_session_;

    // Extracted service classes
    std::unique_ptr<ConnectionLauncher> connection_launcher_;
    std::unique_ptr<ScreenshotHandler> screenshot_handler_;
    std::unique_ptr<ScreenReader> screen_reader_;
    bool probe_all_ = false;
    std::string tree_reader_;
    int row_offset_ = 0;
    bool grid_rows_needed_ = true;
    bool owner_window_guard_ = false;
    sap::SelectionInputPolicy selection_input_policy_;

    /// Take `app` as the SAP Logon of this engine and bind its first session (as at start-up).
    void adopt_application(ComGuiApplicationPtr app);
    /// Forget the SAP Logon and everything bound to it (session, connection, readers), releasing the COM references.
    void drop_application();
    /// The SAP Logon's application, looked up now when there is none; throws SapLogonNotRunningException or
    /// ComException (why) when there still is none.
    const ComGuiApplicationPtr& require_application();

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
    bool owner_window_allowed(const ComGuiSessionPtr& session) const noexcept;
    /// What SAP GUI's own OpenConnection yielded (launch_connection).
    struct NativeOpen {
        bool session_created = false;     ///< a session came up and is bound
        bool opened = false;              ///< the connection opened
        bool scripting_disabled = false;  ///< SAP reports backend GUI scripting disabled
        bool logon_missing = false;       ///< SAP Logon is not running
        std::string error;
    };
    bool launch_by_sapshcut(const std::string& connection_name, json& trace);
    void no_session_error(Result& result, bool used_sapshcut) const;
    NativeOpen open_native_connection(const std::string& connection_name, json& trace);
    std::vector<ConnectionLauncher::ExistingSession> open_sessions_now() const;

public:
    /// The engine on the running SAP Logon (none yet is not an error: every call looks it up again).
    ComAutomationEngine();
    /// The engine on a given SAP GUI application object (tests pass a fake one); binds its first session.
    explicit ComAutomationEngine(ComGuiApplicationPtr app);
    /// The engine on whatever `source` answers, now and before every call (tests script SAP Logon coming and going).
    explicit ComAutomationEngine(ApplicationSource source);

    void refresh_application() override;
    EngineStatus engine_status() const override { return status_; }
    std::uint64_t application_generation() const override { return generation_; }

    ~ComAutomationEngine() override = default;

    void set_owner_window_guard(bool enabled) noexcept override { owner_window_guard_ = enabled; }
    bool current_session_owner_window_allowed() const noexcept override {
        return current_session_ && owner_window_allowed(current_session_);
    }

    // Connection management - two modes
    Result attach_by_click(int timeout_seconds = 10) override;
    Result attach_by_session_id(const std::string& session_id) override;
    Result launch_connection(const std::string& connection_name, bool allow_sapshcut = false) override;
    Result start_sap_logon() override;
    Result disconnect() override;
    Result close_current_session() override;
    bool is_connected() const override;
    std::optional<std::string> live_session_key(const std::string& session_id) const override;
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
                          const std::string& value, bool checkbox, bool commit) override;
    Result select_grid_row(const ElementId& element, int row, const std::string& column) override;
    Result doubleclick_grid_cell(const ElementId& element, int row, const std::string& column) override;
    // Window-level actions (implemented in com_automation_engine_actions.cpp)
    Result send_key(int vkey, const std::string& window = "@active") override;
    Result close_popup(int vkey = 12) override;
    Result read_menu(const std::string& window = "wnd[0]") override;
    /// read_only: refuse (READ_ONLY_REFUSED) when the RESOLVED menu item's real text is state-changing.
    Result select_menu(const std::string& menu_path, const std::string& window = "wnd[0]",
                       bool read_only = false) override;
    Result read_field(const ElementId& element) override;
    /// activate_tab: when the element lives on an inactive tab page, select the page (and restore the previous
    /// one afterwards) instead of returning ELEMENT_ON_INACTIVE_TAB.
    Result read_field(const ElementId& element, bool activate_tab) override;
    Result press_toolbar_button(const ElementId& toolbar_element, const std::string& button_id) override;
    Result press_f4(const ElementId& element) override;

    // Screen operations
    Result read_screen(bool include_structure = true, bool skip_trees = false,
                       int max_rows = 20) override;
    Result read_screen_with_tabs(bool skip_trees = false, int max_rows = 20,
                                 const std::string& only_tab = "") override;
    Result find_screen(const ScreenFindOptions& query) override;
    Result dump_object_tree(const std::string& id, const std::vector<std::string>& props) override;
    void set_probe_all(bool probe_all) override { probe_all_ = probe_all; }
    void set_tree_reader(const std::string& mode) override { tree_reader_ = mode; }
    void set_row_offset(int row_offset) override { row_offset_ = row_offset < 0 ? 0 : row_offset; }
    void set_grid_rows_needed(bool needed) override { grid_rows_needed_ = needed; }
    bool set_selection_input_policy(const sap::SelectionInputPolicy& policy) override { selection_input_policy_ = policy; return true; }
    Result capture_screenshot(const cli::ScreenshotOptions& options) override;
    std::shared_ptr<sap::ComGuiSession> attached_com_session() const override;

    // Getters for testing/debugging
    ComGuiApplicationPtr get_app() const { return app_; }
    ComGuiConnectionPtr get_connection() const { return current_connection_; }
    ComGuiSessionPtr get_session() const { return current_session_; }
    ComGuiSessionPtr unchecked_session() const override { return current_session_; }
    SapGuiScripting* scripting() override { return this; }

    /// Read-only lookup of system/client/user/transaction of a NAMED session; never selects, attaches or
    /// changes anything. Empty facts when the session is gone, the server key differs or COM fails.
    audit::SapFacts peek_session_facts(const std::string& session_id,
                                       const std::string& server_session_key = "") const noexcept override;
    /// Stable live window handle used only to distinguish keyless prelogin windows.
    std::uintptr_t peek_session_window_handle(const std::string& session_id) const noexcept override;
    /// Description (SAP Logon entry name) of the connection that owns `session_id`; empty when unknown. Read-only.
    std::string peek_session_connection_description(const std::string& session_id) const noexcept override;

    // Enumerate all connections, sessions, and windows
    nlohmann::json get_application_info() const;

    // Window management helpers
    WindowId get_active_window_id() const;

    /// Cheap snapshot (window, title, transaction, status text) for change detection
    ScreenSnapshot capture_screen_snapshot() const override;
    ElementId resolve_element_path(const ElementId& element) const;

    /// Detect if a modal dialog is currently active
    /// Returns dialog metadata if modal dialog exists (non-wnd[0]), or empty json if on main window
    /// Dialog metadata includes: window_id, type, title, message, buttons
    nlohmann::json detect_modal_dialog() const override;

};

} // namespace sap
} // namespace fairyfly
