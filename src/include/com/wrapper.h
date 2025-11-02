#pragma once

#include "include/core.h"
#include "include/sap_gui_base.h"
#include <string>
#include <memory>
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

/// COM-based SAP GUI element wrapper
class ComGuiElement : public SapGuiObject {
private:
    mutable GuiElementType cached_element_type_ = GuiElementType::Unknown;
    mutable bool element_type_cached_ = false;

    /// Classify element type from type string
    static GuiElementType classify_type(const std::string& type_str);

public:
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

    /// Set text in element (for text fields)
    void set_text(const std::string& text);

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

    /// Get SubType property for GuiShell elements
    /// Returns SubType string (e.g., "GridView", "Tree", "Toolbar") or empty string if not a GuiShell
    std::string get_subtype() const;

    /// Get all node keys from a tree control (GuiShell with SubType="Tree")
    /// Returns empty vector if element is not a tree or operation fails
    std::vector<std::string> get_all_node_keys() const;

    /// Get node text by key from a tree control
    /// Returns empty string if node not found or operation fails
    std::string get_node_text_by_key(const std::string& key) const;

    /// Get node path by key (e.g., "1\2\3" for hierarchical position)
    /// Returns empty string if node not found or operation fails
    std::string get_node_path_by_key(const std::string& key) const;

    /// Get column names from ColumnOrder collection
    /// Returns empty vector if no columns available
    std::vector<std::string> get_column_order() const;

    /// Get cell value from GridView
    /// @param row Row index (0-based)
    /// @param column_name Column identifier from ColumnOrder
    /// @return Cell value as string, empty if not available
    std::string get_cell_value(int row, const std::string& column_name) const;

    /// Get item text from tree node by column name
    /// Returns empty string if not found or operation fails
    std::string get_item_text(const std::string& node_key, const std::string& column_name) const;

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
public:
    explicit ComGuiApplication(IDispatchPtr sap_gui_app);

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

    /// Get raw COM application object - deprecated, use get_dispatch()
    IDispatch* get_app_object() const { return dispatch_; }

    ~ComGuiApplication() = default;
};

/// Utility function: Select SAP GUI window by mouse click
/// Prompts user to click on SAP window within timeout period
/// Returns window HWND when clicked
/// Throws ComException on timeout or error
HWND select_window_by_mouse_click(int timeout_seconds = 10);

} // namespace sap
} // namespace fairyfly
