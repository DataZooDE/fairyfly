#pragma once

#include "include/core.h"
#include "include/sap_gui_base.h"
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <stdexcept>
#include <windows.h>
#include <comdef.h>

namespace fairyfly {
namespace sap {

/// Exception for COM-related errors (deprecated - use SapGuiException from sap_gui_base.h)
class ComException : public std::runtime_error {
private:
    HRESULT hresult_;

public:
    explicit ComException(const std::string& message, HRESULT hr = S_OK)
        : std::runtime_error(message), hresult_(hr) {}

    HRESULT hresult() const noexcept { return hresult_; }
};

/// COM apartment initialization failed; a disconnected SAP engine cannot recover.
class ComInitializationException : public ComException {
public:
    using ComException::ComException;
};

// get_dispid_via_typeinfo is now provided by sap_gui_base.h

// Forward declarations
class ComGuiElement;
class ComGuiWindow;
class ComGuiSession;
class ComGuiConnection;
class ComGuiApplication;

using ComGuiElementPtr = std::shared_ptr<ComGuiElement>;
using ComGuiWindowPtr = std::shared_ptr<ComGuiWindow>;
using ComGuiSessionPtr = std::shared_ptr<ComGuiSession>;
using ComGuiConnectionPtr = std::shared_ptr<ComGuiConnection>;
using ComGuiApplicationPtr = std::shared_ptr<ComGuiApplication>;

/// Element type enum for type-safe operations
enum class GuiElementType {
    Unknown,
    Button,
    TextField,
    ComboBox,
    CheckBox,
    RadioButton,
    Label,
    Table,
    Tree,
    StatusBar,
    MenuBar,
    Toolbar,
    Tab
};

struct AbapEditorContent {
    std::string text;
    int total_lines = 0;
    int lines_read = 0;
    bool truncated = false;
};

/// COM-based SAP GUI element wrapper
class ComGuiElement : public SapGuiObject {
private:
    mutable GuiElementType cached_element_type_ = GuiElementType::Unknown;
    mutable bool element_type_cached_ = false;
    mutable bool label_cached_ = false;
    mutable std::string cached_label_;
    // Children collection and its count are fetched at most once per wrapper (one
    // wrapper is created per FindById, so lifetime is a single read).
    mutable bool children_fetched_ = false;
    mutable IDispatchPtr children_dispatch_;
    mutable int children_count_ = -1;
    IDispatchPtr children_dispatch_cached() const;
    // Changeable is read at most once per wrapper by is_changeable() and get_text(). The strict
    // fail-closed reads (selection_input_guard) and set_text() always read it fresh.
    mutable std::optional<bool> cached_changeable_;
    bool changeable_cached() const;

public:
    /// Classify element type from type string
    static GuiElementType classify_type(const std::string& type_str);

    explicit ComGuiElement(IDispatchPtr elem);

    /// Create element wrapper from COM object
    static ComGuiElementPtr create(IDispatchPtr elem);

    // Base class get_id(), get_type(), get_name() inherited from SapGuiObject

    /// Get classified element type (enum)
    GuiElementType get_classified_type() const;

    /// Check if element is a specific type
    bool is_type(GuiElementType type) const { return get_classified_type() == type; }

    /// Get visible text of element
    std::string get_text() const;

    /// Read one element for direct CLI output, including positioned report-row context.
    std::string get_text_for_direct_read() const;

    /// Set text in element (for text fields). Returns false if SAP explicitly marks it read-only.
    bool set_text(const std::string& text);

    /// Check if element is enabled
    bool is_enabled() const;

    /// Check if element is visible
    bool is_visible() const;

    /// Get element position and size info
    struct Rect {
        int x = 0, y = 0, width = 0, height = 0;
    };

    /// Get element's screen position and size
    Rect get_rect() const;

    /// Press a button element (GuiButton, GuiOkCode, etc.)
    /// Throws ComException if element is not a button or operation fails
    void press();
    void set_focus();

    /// Select checkbox or radio button
    /// Throws ComException if element is not selectable
    void select(bool selected = true);

    /// Get element's associated label text (from AccLabel property)
    /// Returns empty string if no label found
    std::string get_label() const;

    /// Get element's tooltip text (cascades: AccTooltip → DefaultTooltip → Tooltip)
    /// Returns empty string if no tooltip found
    std::string get_tooltip() const;

    /// Check if element can be changed/modified by user
    /// Returns true for text fields, checkboxes, etc. when enabled
    bool is_changeable() const;

    /// Get child count if element is a container
    /// Returns 0 for non-container elements
    int get_child_count() const;

    /// Get child element by index
    /// Returns nullptr if index out of range or element is not a container
    ComGuiElementPtr get_child(int index) const;

    /// Get all children as an iterable collection
    /// Returns empty collection if element has no children
    SapGuiCollection<ComGuiElement> children() const;

    /// Get container type classification (toolbar, form, table, etc.)
    /// Returns empty string for non-container elements
    std::string get_container_type() const;

    /// Get integer property value from element (for tables/grids)
    /// Returns 0 if property doesn't exist or element doesn't support it
    int get_property_int(const std::wstring& property_name) const;

    /// Get boolean property value from element
    /// Returns false if property doesn't exist or element doesn't support it
    bool get_property_bool(const std::wstring& property_name) const;

    /// Get string property value from element
    /// Returns empty string if property doesn't exist or element doesn't support it
    std::string get_property_string(const std::wstring& property_name) const;

    /// Read ABAP editor source through GetLineCount/GetLineText without altering it.
    AbapEditorContent get_abap_editor_content(int max_lines) const;

    /// Get SubType property for GuiShell elements
    /// Returns SubType string (e.g., "GridView", "Tree", "Toolbar") or empty string if not a GuiShell
    std::string get_subtype() const;

    /// Get all node keys from a tree control (GuiShell with SubType="Tree")
    /// Returns empty vector if element is not a tree or operation fails
    std::vector<std::string> get_all_node_keys() const;
    std::vector<std::string> get_tree_column_names() const;

    /// Get node text by key from a tree control
    /// Returns empty string if node not found or operation fails
    std::string get_node_text_by_key(const std::string& key) const;

    /// Get node path by key (e.g., "1\2\3" for hierarchical position)
    /// Returns empty string if node not found or operation fails
    std::string get_node_path_by_key(const std::string& key) const;

    /// Get column names from ColumnOrder collection
    /// Returns empty vector if no columns available
    std::vector<std::string> get_column_order() const;

    // ============================================================================
    // GuiShell Toolbar Button Methods (for GuiShell with SubType="Toolbar")
    // ============================================================================

    /// Get number of toolbar buttons in GuiShell Toolbar
    /// Returns 0 if not a toolbar or property not available
    int get_button_count() const;

    /// Get button ID at specified position (0-based)
    /// Returns empty string if position invalid or method fails
    std::string get_button_id(int position) const;

    /// Get button text at specified position
    /// Returns empty string if position invalid or method fails
    std::string get_button_text(int position) const;

    /// Get button tooltip at specified position
    /// Returns empty string if position invalid or method fails
    std::string get_button_tooltip(int position) const;

    /// Get button type at specified position
    /// Returns type string: "Button", "ButtonAndMenu", "Menu", "Separator", "Group", "CheckBox"
    /// Returns empty string if position invalid or method fails
    std::string get_button_type(int position) const;

    /// Get button enabled state at specified position
    /// Returns false if position invalid or method fails
    bool get_button_enabled(int position) const;

    /// Look up a toolbar button (GuiShell toolbar, then GridView toolbar) by its ID and return its
    /// text and tooltip. Returns false when the button cannot be found or the lookup fails.
    bool find_toolbar_button_labels(const std::string& button_id, std::string& text,
                                    std::string& tooltip) const;

    /// Press a toolbar button by ID (for GuiShell Toolbar elements)
    /// @param button_id Button identifier (e.g., "TECH", "PERF")
    /// Throws ComException if not a toolbar or button not found
    void press_button(const std::string& button_id);

    /// Press F4 to open search help/value help (for GuiCTextField elements)
    /// Equivalent to clicking the search button or pressing F4 key
    /// Throws ComException if element doesn't support F4 help
    void press_f4();

    /// Get cell value from GridView
    /// @param row Row index (0-based)
    /// @param column_name Column identifier from ColumnOrder
    /// @return Cell value as string, empty if not available
    std::string get_cell_value(int row, const std::string& column_name) const;

    /// Select a GridView row and activate its cell (zero-based row, technical column ID).
    void select_grid_row(int row, const std::string& column_name);

    /// Double-click a GridView cell (SetCurrentCell then DoubleClickCurrentCell).
    void doubleclick_grid_cell(int row, const std::string& column_name);

    /// GridView built-in toolbar metadata (separate from GuiShell Toolbar buttons).
    int get_grid_toolbar_button_count() const;
    std::string get_grid_toolbar_button_id(int position) const;
    std::string get_grid_toolbar_button_tooltip(int position) const;

    /// Modify a GridView cell and optionally notify the backend after a batch.
    void modify_grid_cell(int row, const std::string& column_name,
                          const std::string& value, bool checkbox, bool commit);

    /// Get item text from tree node by column name
    /// Returns empty string if not found or operation fails
    std::string get_item_text(const std::string& node_key, const std::string& column_name) const;

    // ============================================================================
    // GuiShell Tree Navigation Methods (for GuiShell with SubType="Tree")
    // ============================================================================

    /// Select a tree node without expanding it
    /// @param node_key Node identifier (from GetAllNodeKeys)
    /// Throws ComException if element is not a tree or node not found
    void select_node(const std::string& node_key);

    /// Expand a collapsed tree node
    /// @param node_key Node identifier (from GetAllNodeKeys)
    /// Throws ComException if element is not a tree or node not found
    void expand_node(const std::string& node_key);

    /// Collapse an expanded tree node
    /// @param node_key Node identifier (from GetAllNodeKeys)
    /// Throws ComException if element is not a tree or node not found
    void collapse_node(const std::string& node_key);

    /// Double-click a tree node (default action: expand/collapse or drill-in)
    /// @param node_key Node identifier (from GetAllNodeKeys)
    /// Throws ComException if element is not a tree or node not found
    void doubleclick_node(const std::string& node_key);

    /// Open a tree node's context menu and select an item by its visible text.
    void select_node_context_item(const std::string& node_key, const std::string& item_text);

    /// Get raw COM object (for advanced use) - deprecated, use get_dispatch()
    IDispatch* get_com_object() const { return dispatch_; }
};

/// COM-based SAP GUI window wrapper (container of elements)
class ComGuiWindow : public SapGuiObject {
public:
    explicit ComGuiWindow(IDispatchPtr wnd);

    /// Create window wrapper from COM object
    static ComGuiWindowPtr create(IDispatchPtr wnd);

    // Base class get_id(), get_type(), get_name() inherited from SapGuiObject

    /// Get window title
    std::string get_title() const;

    /// Get number of child elements
    int get_child_count() const;

    /// Get child element by index (legacy method)
    ComGuiElementPtr get_child(int index) const;

    /// Get children collection (modern STL-compatible API)
    SapGuiCollection<ComGuiElement> children() const;

    /// Send virtual key to window (F3, F8, Enter, etc.)
    /// Common keys: 0=Enter, 1=F1, 4=F4, 8=F8, 3=F3, 12=F12, etc.
    /// Throws ComException if operation fails
    void send_vkey(int vkey);

    /// Close the window via its own Close method (GuiModalWindow.Close).
    /// Only meant for popups (wnd[N>0]); callers must not use it on wnd[0].
    void close();

    /// Get raw COM object - deprecated, use get_dispatch()
    IDispatch* get_com_object() const { return dispatch_; }
};

/// COM-based SAP GUI session wrapper
class ComGuiSession : public SapGuiObject {
public:
    explicit ComGuiSession(IDispatchPtr sess);

    /// Create session wrapper from COM object
    static ComGuiSessionPtr create(IDispatchPtr sess);

    // Base class get_id(), get_type(), get_name(), get_type_as_number() inherited from SapGuiObject

    /// Check if session is currently busy communicating with server
    bool is_busy() const;

    /// Check if session is still alive
    bool is_alive() const;

    /// Get active (top) window
    ComGuiWindowPtr get_active_window() const;

    /// Find element by ID/path
    ComGuiElementPtr find_element_by_id(const std::string& id) const;

    /// Execute SAP transaction by code
    void start_transaction(const std::string& tcode);

    /// Wait for session to complete current operation
    void wait_for_completion(int timeout_ms = 30000);

    /// Send virtual key to session (F3, F8, Enter, etc.)
    /// Common keys: 0=Enter, 1=F1, 8=F8, 3=F3, 12=F12, etc.
    /// Throws ComException if operation fails
    void send_vkey(int vkey);

    /// Wait for element to appear with polling
    /// @param element_id Element path to wait for (e.g., "wnd[0]/usr/btn[99]")
    /// @param timeout_ms Maximum time to wait in milliseconds
    /// @return Found element pointer or nullptr if timeout
    ComGuiElementPtr wait_for_element(const std::string& element_id, int timeout_ms = 30000);

    /// Get current transaction code from session Info object
    /// @return Transaction code (e.g., "SM59") or empty string if not available
    std::string get_transaction_code() const;

    /// Authenticated SAP user from GuiSessionInfo, or empty before logon.
    std::string get_user() const;

    /// SAP system name (GuiSessionInfo.SystemName), empty when unavailable.
    std::string get_system_name() const;

    /// SAP client (GuiSessionInfo.Client), empty when unavailable.
    std::string get_client() const;

    /// ABAP program of the current screen (GuiSessionInfo.Program), empty when unavailable.
    std::string get_program() const;

    /// Dynpro number of the current screen (GuiSessionInfo.ScreenNumber) as text; empty when unavailable or 0.
    std::string get_screen_number() const;

    /// Backend session identity when SAP exposes GuiSessionInfo.
    /// Combines SystemSessionId and SessionNumber; empty when unavailable.
    std::string get_server_session_key() const;

    /// Get raw COM object - deprecated, use get_dispatch()
    IDispatch* get_com_object() const { return dispatch_; }
};

/// COM-based SAP GUI connection wrapper
class ComGuiConnection : public SapGuiObject {
public:
    explicit ComGuiConnection(IDispatchPtr conn);

    /// Create connection wrapper from COM object
    static ComGuiConnectionPtr create(IDispatchPtr conn);

    // Base class get_id(), get_type(), get_name(), get_type_as_number() inherited from SapGuiObject

    /// Get connection description (system info)
    std::string get_description() const;

    /// Get connection string
    std::string get_connection_string() const;

    /// Get number of active sessions (max 6)
    int get_session_count() const;

    /// Get session by index (legacy method)
    ComGuiSessionPtr get_session(int index) const;

    /// Get sessions collection (modern STL-compatible API)
    SapGuiCollection<ComGuiSession> sessions() const;

    /// Get raw COM object - deprecated, use get_dispatch()
    IDispatch* get_com_object() const { return dispatch_; }
};

/// COM-based SAP GUI application wrapper (root object)
class ComGuiApplication : public SapGuiObject {
    int com_uninit_count_ = 0;
public:
    explicit ComGuiApplication(IDispatchPtr sap_gui_app, int com_uninit_count = 0);
    explicit ComGuiApplication(IDispatchPtr sap_gui_app, bool owns_com_apartment);
    ComGuiApplication(const ComGuiApplication&) = delete;
    ComGuiApplication& operator=(const ComGuiApplication&) = delete;

    /// Create SAP GUI application COM wrapper
    /// Initializes COM library and creates root SAPGUI object
    /// Throws ComException if SAP GUI not installed or not accessible
    static ComGuiApplicationPtr create();

    // Base class get_id(), get_type(), get_name() inherited from SapGuiObject

    /// Get number of open connections
    int get_connection_count() const;

    /// Get connection by index (legacy method)
    ComGuiConnectionPtr get_connection(int index) const;

    /// Get connections collection (modern STL-compatible API)
    SapGuiCollection<ComGuiConnection> connections() const;

    /// Check if any connections are open
    bool has_connections() const { return get_connection_count() > 0; }

    /// Find window by HWND and return connection/session indices
    /// @param hwnd Window handle to search for
    /// @return SAPGuiWindow with valid indices if found, invalid (indices=-1) if not found
    SAPGuiWindow find_window_by_hwnd(HWND hwnd) const;

    /// Open a connection by description (SAP Logon entry name)
    /// @param connection_name Name of the connection entry in SAP Logon (e.g. "Bigfox")
    /// @param sync Whether the call blocks until the connection is established (default true)
    /// @param raise_error Whether to raise an exception on failure (default false)
    /// @return ComGuiConnectionPtr on success, nullptr on failure
    ComGuiConnectionPtr open_connection(const std::string& connection_name, bool sync = true, bool raise_error = false);

    /// Open a connection by connection string
    /// @param connection_string Connection string (e.g. "/H/bigfox/S/3200")
    /// @param sync Whether the call blocks until the connection is established (default true)
    /// @param raise_error Whether to raise an exception on failure (default false)
    /// @return ComGuiConnectionPtr on success, nullptr on failure
    ComGuiConnectionPtr open_connection_by_connection_string(const std::string& connection_string, bool sync = true, bool raise_error = false);

    /// Get raw COM application object - deprecated, use get_dispatch()
    IDispatch* get_app_object() const { return dispatch_; }

    ~ComGuiApplication() override;
};

/// Utility function: Select SAP GUI window by mouse click
/// Prompts user to click on SAP window within timeout period
/// Returns window HWND when clicked
/// Throws ComException on timeout or error
HWND select_window_by_mouse_click(int timeout_seconds = 10);

} // namespace sap
} // namespace fairyfly
