# SAP GUI Scripting API - Complete Documentation

## Table of Contents

1. [Introduction](#introduction)
2. [Getting Started](#getting-started)
3. [Configuration and Security](#configuration-and-security)
4. [Object Hierarchy](#object-hierarchy)
5. [Core Objects](#core-objects)
6. [GUI Components](#gui-components)
7. [Container Objects](#container-objects)
8. [Control Elements](#control-elements)
9. [Common Properties](#common-properties)
10. [Common Methods](#common-methods)
11. [VKey Codes](#vkey-codes)
12. [Error Handling](#error-handling)
13. [Programming Languages Support](#programming-languages-support)
14. [Development Tools](#development-tools)
15. [Best Practices](#best-practices)
16. [Code Examples](#code-examples)

---

## Introduction

### Purpose

SAP GUI Scripting is an API (Application Programming Interface) that enables automation of SAP GUI interactions. Ever since the release of SAP system version 4.6C, SAP GUI has provided scripting support for both Windows and Java platforms.

### Key Features

- **Automation**: Automate repetitive tasks in SAP GUI
- **Integration**: Integrate SAP processes with external applications
- **Recording**: Record user interactions and convert them to reusable scripts
- **Cross-platform**: Available for both Windows and Java platforms
- **COM-based**: Uses Component Object Model (COM) for Windows, allowing use with any COM-enabled language

### Technical Architecture

The SAP GUI Scripting API is implemented in the file `sapfewse.ocx` (FEWSE = Front End Windows Scripting Engine) located in the directory `C:\Program Files (x86)\SAP\FrontEnd\SapGui`. It is a Component Object Model (COM) library registered in the Windows registry.

---

## Getting Started

### Prerequisites

1. **SAP GUI Installation**: SAP GUI for Windows or SAP GUI for Java must be installed
2. **Scripting Enabled**: Scripting must be enabled on both server and client side
3. **Proper Permissions**: User must have appropriate authorization (S_SCR authority object)

### Enabling Scripting

#### Server-Side Configuration

By default, SAP GUI Scripting is disabled on any given SAP system. The administrator must enable support by setting profile parameters:

**Primary Parameter:**
- `sapgui/user_scripting = TRUE` - Enable scripting on the application server

**Using Transaction RZ11:**
```
1. Execute transaction RZ11
2. Enter parameter name: sapgui/user_scripting
3. Press Enter
4. Change value to TRUE
```

**Using Transaction RZ10 (Permanent Configuration):**
```
1. Execute transaction RZ10
2. Modify the instance profile
3. Add parameter: sapgui/user_scripting = TRUE
4. Save and restart the SAP server
```

**Additional Security Parameters:**

- `sapgui/user_scripting_per_user` - Enable scripting for specific users only (default: FALSE)
- `sapgui/user_scripting_set_readonly` - Enable read-only mode (limits API to read operations only)
- `sapgui/user_scripting_disable_recording` - Disable script recording while allowing execution

#### Client-Side Configuration

**SAP GUI Options:**
```
1. Open SAP GUI
2. Navigate to: Customize Local Layout (ALT+F12)
3. Go to: Options > Accessibility & Scripting > Scripting
4. Check "Enable scripting" under User Settings
5. Uncheck other restrictive options
```

**Security Configuration:**
```
1. In SAP GUI Options, select Security folder
2. Click Security Configuration
3. Click "Open Security Configuration"
4. Change Status to "Customized"
5. Set Default Action to "Allow"
```

**Registry Settings (Windows):**
Registry key: `HKEY_LOCAL_MACHINE\SOFTWARE\SAP\SAPGUI Front\SAP Frontend Server\Security\UserScripting`
- Value: 0 = Disabled, 1 = Enabled

### Recording Your First Script

SAP GUI includes a built-in script recorder:

```
1. In SAP GUI, click Customize Local Layout
2. Select: Script Recording and Playback > Record Script
3. Choose script path and filename
4. Perform your SAP operations
5. Stop recording
6. Review and edit the generated script
```

---

## Configuration and Security

### Security Considerations

**Default State:**
- Scripting is disabled by default for security reasons
- Automation can potentially be misused, so proper controls are essential

**Security Best Practices:**
1. Enable scripting only for authorized users
2. Use `sapgui/user_scripting_per_user` parameter for granular control
3. Implement proper authorization checks (S_SCR authority object)
4. Consider read-only mode for reporting/monitoring scripts
5. Test all scripts in sandbox/development systems first
6. Implement audit logging for script execution

**Risk Mitigation:**
- Scripts execute significantly faster than manual operations, potentially increasing system load
- Scripts can make mistakes rapidly if not properly tested
- Implement proper error handling and validation
- Use read-only mode when modifications are not required

### Authorization Objects

**S_SCR Authority Object:**
Required for users to execute SAP GUI scripts when `sapgui/user_scripting_per_user` is set to TRUE.

---

## Object Hierarchy

### Hierarchy Structure

The SAP GUI Scripting objects are organized in a hierarchical tree structure:

```
GuiApplication (Root)
    └── GuiConnection (one or more)
        └── GuiSession (one or more)
            └── GuiMainWindow / GuiModalWindow
                ├── GuiTitlebar
                ├── GuiMenubar
                ├── GuiToolbar (System)
                ├── GuiToolbar (Application)
                ├── GuiUserArea
                │   └── [Various GUI Components]
                └── GuiStatusbar
```

### Object ID Structure

Every object in the hierarchy has a unique ID in URL-like format:

**Format:** `/app/con[x]/ses[x]/wnd[x]/usr/...`

**Examples:**
```
/app                                    - GuiApplication
/app/con[0]                            - First GuiConnection
/app/con[0]/ses[0]                     - First GuiSession
/app/con[0]/ses[0]/wnd[0]              - Main window
/app/con[0]/ses[0]/wnd[0]/usr          - User area
/app/con[0]/ses[0]/wnd[0]/usr/txtRSYST-BNAME  - Text field
/app/con[0]/ses[0]/wnd[0]/tbar[0]/btn[0]      - Toolbar button
```

### Predefined Objects

The following objects can be addressed directly without using `findById()`:
- `application` - GuiApplication object
- `connection` - Active GuiConnection
- `session` - Active GuiSession
- `window` - Active GuiFrameWindow
- `userarea` - GuiUserArea of active window

---

## Core Objects

### GuiApplication

**Description:** Represents the SAP GUI process in which all activity takes place. This is the root object and entry point for scripting.

**Characteristics:**
- Creatable class
- Only one instance per process
- Extends GuiContainer
- Contains all connections

**Key Properties:**
- `Children` (GuiComponentCollection) - All direct children (connections)
- `HistoryEnabled` (Boolean) - Whether input history is enabled
- `Name` (String, Read-only) - Name of the application
- `Type` (String, Read-only) - Type identifier
- `Id` (String, Read-only) - Object ID ("/app")

**Key Methods:**
- `OpenConnection(ConnectionString As String, Sync As Boolean) As GuiConnection` - Opens a new connection
- `OpenConnectionByConnectionString(ConnectionString As String) As GuiConnection`
- `CloseConnection(ConnectionString As String)`
- `findById(Id As String, [Optional] raise As Boolean = True) As GuiComponent`

**Usage Example:**
```vbscript
' Create or get GuiApplication object
Set SapGuiAuto = GetObject("SAPGUI")
Set application = SapGuiAuto.GetScriptingEngine

' Open connection
Set connection = application.OpenConnection("SAP ERP System", True)
```

### GuiConnection

**Description:** Represents the connection between SAP GUI and an application server.

**Characteristics:**
- Child of GuiApplication
- All children are GuiSession objects
- Represents a single SAP system connection

**Key Properties:**
- `Children` (GuiComponentCollection) - All sessions for this connection
- `ConnectionString` (String, Read-only) - Connection identifier
- `Description` (String, Read-only) - Description of the connection
- `DisabledByServer` (Boolean, Read-only) - Whether scripting is disabled server-side
- `Name` (String, Read-only) - Connection name
- `Sessions` (GuiSessionCollection, Read-only) - All sessions

**Key Methods:**
- `CloseSession(SessionId As String)`
- `CloseConnection()`
- `findById(Id As String, [Optional] raise As Boolean = True) As GuiComponent`

**Important Note:**
The Children property of GuiConnection can block execution before it's possible to use the Busy property. Use with caution in loops.

### GuiSession

**Description:** Represents a specific task or user session within a connection. Each session executes exactly one transaction at a time.

**Characteristics:**
- Child of GuiConnection
- Most scripting activities occur within a session context
- Usually has one GuiMainWindow as child

**Key Properties:**
- `ActiveWindow` (GuiFrameWindow, Read-only) - Currently active window
- `Busy` (Boolean, Read-only) - Whether session is processing
- `BusyAction` (String, Read-only) - Description of current operation
- `Children` (GuiComponentCollection) - Child objects (usually windows)
- `Id` (String, Read-only) - Session ID
- `Info` (GuiSessionInfo, Read-only) - Session information
- `IsActive` (Boolean, Read-only) - Whether session is active
- `Record` (Boolean) - Enable/disable recording
- `TestToolMode` (Long) - Test tool operation mode

**Key Methods:**
- `StartTransaction(Transaction As String)` - Start a transaction
- `EndTransaction()` - End current transaction
- `SendCommand(Command As String)` - Send a command
- `CreateSession()` - Create new session
- `findById(Id As String, [Optional] raise As Boolean = True) As GuiComponent`
- `findByIdEx(Id As String, [Optional] raise As Boolean = True) As GuiComponent`
- `LockSessionUI()` - Lock the UI during script execution
- `UnlockSessionUI()` - Unlock the UI

**Usage Example:**
```vbscript
Set session = connection.Children(0)
session.StartTransaction "SE38"
session.findById("wnd[0]/usr/txtRS38M-PROGRAMM").Text = "ZTEST"
session.findById("wnd[0]").sendVKey 8  ' F8 - Execute
```

### GuiSessionInfo

**Description:** Provides information about a session.

**Key Properties:**
- `ApplicationServer` (String, Read-only) - Application server name
- `Client` (String, Read-only) - Client number
- `Codepage` (Long, Read-only) - Code page
- `Flushes` (Long, Read-only) - Number of flushes
- `Group` (String, Read-only) - Logon group
- `GuiCodepage` (Long, Read-only) - GUI code page
- `I18NMode` (Boolean, Read-only) - Internationalization mode
- `InterpretationTimeEver` (Long, Read-only) - Total interpretation time
- `IsLowSpeedConnection` (Boolean, Read-only) - Low speed connection indicator
- `Language` (String, Read-only) - Logon language
- `Program` (String, Read-only) - Current program
- `ResponseTimeEver` (Long, Read-only) - Total response time
- `RoundTrips` (Long, Read-only) - Number of round trips
- `ScreenNumber` (Long, Read-only) - Current screen number
- `ScriptingModeReadOnly` (Boolean, Read-only) - Read-only mode indicator
- `ScriptingModeRecordingDisabled` (Boolean, Read-only) - Recording disabled indicator
- `SessionNumber` (Long, Read-only) - Session number
- `SystemName` (String, Read-only) - System name
- `SystemNumber` (Long, Read-only) - System number
- `SystemSessionId` (String, Read-only) - System session ID
- `Transaction` (String, Read-only) - Current transaction
- `User` (String, Read-only) - User name

---

## GUI Components

### GuiFrameWindow

**Description:** Abstract interface for high-level visual objects (main windows and modal dialogs).

**Characteristics:**
- Abstract interface (cannot be instantiated directly)
- Extended by GuiMainWindow and GuiModalWindow
- Represents top-level windows

**Base Interfaces:**
- GuiComponent
- GuiVComponent
- GuiVContainer

**Key Properties:**
- `Height` (Long) - Window height
- `Width` (Long) - Window width
- `Iconic` (Boolean) - True if window is iconified/minimized
- `Left` (Long) - Left position
- `Top` (Long) - Top position
- `WorkingPaneHeight` (Long, Read-only) - Height of working pane in character metric
- `WorkingPaneWidth` (Long, Read-only) - Width of working pane in character metric
- `WindowHandle` (Long, Read-only) - Handle of underlying Windows window

**Key Methods:**
- `Close()` - Close the window
- `Iconify()` - Minimize the window
- `Maximize()` - Maximize the window
- `Restore()` - Restore window to normal size
- `sendVKey(VKey As Long)` - Send virtual key

### GuiMainWindow

**Description:** Represents the main window of a SAP GUI session.

**Characteristics:**
- Extends GuiFrameWindow
- Primary window for a session
- Contains user area, toolbars, menubar, statusbar

**Additional Methods:**
- `resizeWorkingPane(width As Long, height As Long)` - Resize window so working area has specified dimensions

**Child Objects:**
- GuiTitlebar
- GuiMenubar
- GuiToolbar (system toolbar - tbar[0])
- GuiToolbar (application toolbar - tbar[1])
- GuiUserArea
- GuiStatusbar

### GuiModalWindow

**Description:** Represents a modal dialog popup window.

**Characteristics:**
- Extends GuiFrameWindow
- Blocks interaction with other windows while open
- Often used for confirmations, messages, or data entry dialogs

---

## Container Objects

### GuiTitlebar

**Description:** Represents the title bar at the top of a window.

**Characteristics:**
- Extends GuiVContainer
- Type prefix: `titl`
- Name: `titl`

**Usage:**
Access via: `session.findById("wnd[0]/titl")`

### GuiMenubar

**Description:** Represents the menu bar of the main window.

**Characteristics:**
- Extends GuiVContainer
- Type prefix: `mbar`
- Name: `mbar`
- Only the main window has a menubar
- Children are GuiMenu objects

**Usage:**
```vbscript
Set menubar = session.findById("wnd[0]/mbar")
```

### GuiMenu

**Description:** Represents a menu or menu item.

**Characteristics:**
- Extends GuiVContainer
- May have other GuiMenu objects as children (submenus)
- Type prefix: `menu`
- Name: Text of the menu item

### GuiToolbar

**Description:** Represents a toolbar containing buttons.

**Characteristics:**
- Extends GuiVContainer
- Type prefix: `tbar`
- Name: `tbar`
- Every GuiFrameWindow has a GuiToolbar
- GuiMainWindow has two toolbars:
  - `tbar[0]` - System toolbar (upper)
  - `tbar[1]` - Application toolbar (lower, unless disabled)

**Usage:**
```vbscript
' Press button on system toolbar
session.findById("wnd[0]/tbar[0]/btn[0]").press

' Press button on application toolbar
session.findById("wnd[0]/tbar[1]/btn[3]").press
```

### GuiStatusbar

**Description:** Represents the status bar at the bottom of the window, displaying messages and system information.

**Characteristics:**
- Type prefix: `sbar`
- Name: `sbar`
- All properties are read-only

**Key Properties:**
- `Text` (String, Read-only) - Status bar text
- `MessageType` (String, Read-only) - Message type: "S" (Success), "W" (Warning), "E" (Error), "A" (Abort), "I" (Information)
- `MessageNumber` (String, Read-only) - Message number from ABAP message call
- `MessageId` (String, Read-only) - Message class from ABAP message call
- `MessageParameter(index As Long)` (String, Read-only) - Parameters used to expand placeholders (index: 0-7)
- `MessageAsPopup` (Boolean, Read-only) - Whether message appears as popup

**Usage Example:**
```vbscript
Set statusbar = session.findById("wnd[0]/sbar")

If statusbar.MessageType = "E" Or statusbar.MessageType = "W" Then
    WScript.Echo "Error/Warning: " & statusbar.Text
    WScript.Echo "Message ID: " & statusbar.MessageId
    WScript.Echo "Message Number: " & statusbar.MessageNumber
End If
```

### GuiUserArea

**Description:** Represents the main working area of a window where most UI controls are placed.

**Characteristics:**
- Extends GuiVContainer
- Type prefix: `usr`
- Name: `usr`
- Contains the dynpro elements (fields, buttons, etc.)

**Usage:**
```vbscript
Set userArea = session.findById("wnd[0]/usr")
```

### GuiSimpleContainer

**Description:** Container representing non-scrollable subscreens.

**Characteristics:**
- Extends GuiVContainer
- Type number: 71
- No additional functionality beyond inherited interfaces

### GuiScrollContainer

**Description:** Container representing scrollable subscreens.

**Characteristics:**
- Extends GuiVContainer
- Type number: 72
- May have scrollbars depending on data volume and GuiUserArea size
- A subscreen may be scrollable without actually having visible scrollbars

### GuiSplitterContainer

**Description:** Container that can be split into multiple panes.

**Characteristics:**
- Part of GuiVContainer hierarchy
- Allows dynamic resizing of contained areas

### GuiTabStrip

**Description:** Container whose children are tabs (GuiTab objects).

**Characteristics:**
- Extends GuiVContainer
- Type prefix: `tabs`
- Name from SAP data dictionary

**Behavior:**
- All tabs are always available
- For server-driven tab strips: only children of the selected tab exist in the object hierarchy
- Tab selection may trigger server communication

**Usage Example:**
```vbscript
' Select a tab
session.findById("wnd[0]/usr/tabsTABSTRIP/tabpTAB1").select
```

### GuiTab

**Description:** Represents an individual tab within a GuiTabStrip.

**Characteristics:**
- Extends GuiVContainer
- Children of GuiTabStrip
- Type prefix: `tabp`
- Name: ID of the tab's button from data dictionary

**Key Methods:**
- `select()` - Select this tab

---

## Control Elements

### GuiTextField

**Description:** Standard text input field.

**Characteristics:**
- Extends GuiVComponent
- Type prefix: `txt`
- Name: Fieldname from SAP data dictionary

**Key Properties:**
- `Text` (String) - Text content of the field
- `MaxLength` (Long, Read-only) - Maximum length in code units
- `Changeable` (Boolean, Read-only) - Whether field can be modified
- `Modified` (Boolean) - Whether field has been modified
- `Highlighted` (Boolean) - Whether field is highlighted

**Usage Example:**
```vbscript
session.findById("wnd[0]/usr/txtBUSER").Text = "USERNAME"
```

### GuiCTextField

**Description:** Text field with combo box button (F4 help).

**Characteristics:**
- Similar to GuiTextField
- Has combo box button for value help

**Usage:**
```vbscript
' Set value
session.findById("wnd[0]/usr/ctxtBUKRS").Text = "1000"

' Open value help
session.findById("wnd[0]/usr/ctxtBUKRS").pressF4
```

### GuiPasswordField

**Description:** Password input field (masked text).

**Characteristics:**
- Extends GuiVComponent and GuiTextField
- Type prefix: Similar to GuiTextField
- Text property cannot be read (always returns empty string)
- Text property can be written to set password

**Usage Example:**
```vbscript
session.findById("wnd[0]/usr/pwdRSYST-BCODE").Text = "MyPassword"
' Reading .Text will return empty string
```

### GuiButton

**Description:** Represents all push buttons (on dynpros, toolbar, or table controls).

**Characteristics:**
- Extends GuiVComponent
- Emulates manually pressing a button
- May trigger server communication

**Base Interfaces:**
- GuiComponent
- GuiVComponent

**Key Methods:**
- `press()` - Press the button

**Usage Example:**
```vbscript
session.findById("wnd[0]/usr/btnBUTTON").press
session.findById("wnd[0]/tbar[0]/btn[0]").press  ' Toolbar button
```

### GuiCheckBox

**Description:** Checkbox control for boolean selections.

**Characteristics:**
- Extends GuiVComponent
- Type prefix: `chk`
- Name: Fieldname from SAP data dictionary

**Key Properties:**
- `Selected` (Boolean) - Whether checkbox is checked

**Key Methods:**
- `select()` - Check the checkbox
- `SetFocus()` - Set focus to the checkbox

**Usage Example:**
```vbscript
' Check a checkbox
session.findById("wnd[0]/usr/chkPARAM").Selected = True

' Or use select method
session.findById("wnd[0]/usr/chkPARAM").select
```

**Note:** Checking a checkbox can cause server communication depending on ABAP screen painter definition.

### GuiRadioButton

**Description:** Radio button for mutually exclusive selections.

**Characteristics:**
- Extends GuiVComponent
- Type prefix: `rad`
- Name: Fieldname from SAP data dictionary
- Part of a radio button group (only one can be selected)

**Key Properties:**
- `Selected` (Boolean, Read-only) - Whether radio button is selected

**Key Methods:**
- `select()` - Select this radio button

**Usage Example:**
```vbscript
session.findById("wnd[0]/usr/radRADIO1").select
```

### GuiComboBox

**Description:** Dropdown list with predefined choices.

**Characteristics:**
- Extends GuiVComponent
- Different from GuiCTextField
- All choices retrieved from server on initialization
- Selection done on client side

**Key Properties:**
- `Key` (String) - Selected entry key
- `Value` (String, Read-only) - Selected entry value
- `Entries` (GuiCollection, Read-only) - All available entries

**Entries Collection:**
Each entry is of type GuiComboBoxEntry with properties:
- `Key` (String) - Entry key
- `Value` (String) - Entry display value

**Usage Example:**
```vbscript
' Set by key
session.findById("wnd[0]/usr/cmbCOMBO").Key = "01"

' Get value
currentValue = session.findById("wnd[0]/usr/cmbCOMBO").Value

' List all entries
Set combo = session.findById("wnd[0]/usr/cmbCOMBO")
For i = 0 To combo.Entries.Count - 1
    Set entry = combo.Entries(i)
    WScript.Echo entry.Key & " - " & entry.Value
Next
```

### GuiLabel

**Description:** Static text label.

**Characteristics:**
- Extends GuiVComponent
- Name: Fieldname from SAP data dictionary
- Display-only, non-interactive

**Key Properties:**
- `Text` (String, Read-only) - Label text

### GuiOkCodeField

**Description:** Command field (transaction code input field) on the upper toolbar.

**Characteristics:**
- Combo box where commands can be entered
- Located on main window toolbar
- Setting text doesn't execute until server communication starts (e.g., pressing Enter)

**Usage Example:**
```vbscript
' Enter transaction code
session.findById("wnd[0]/tbar[0]/okcd").Text = "/nSE38"
session.findById("wnd[0]").sendVKey 0  ' Press Enter to execute
```

---

## Advanced Control Elements

### GuiGridView

**Description:** Grid control for displaying and editing tabular data (ALV Grid).

**Characteristics:**
- Extends GuiVComponent
- Powerful control for table operations
- Supports selection, editing, sorting

**Key Properties:**
- `RowCount` (Long, Read-only) - Number of rows
- `ColumnCount` (Long, Read-only) - Number of columns
- `CurrentCellRow` (Long) - Current cell row index
- `CurrentCellColumn` (String) - Current cell column name
- `SelectedRows` - Collection of selected rows
- `SelectedColumns` - Collection of selected columns
- `SelectedCells` - Collection of selected cells

**Key Methods:**
- `GetCellValue(Row As Long, Column As String) As String` - Get cell value
- `SetCellValue(Row As Long, Column As String, Value As String)` - Set cell value
- `SetCurrentCell(Row As Long, Column As String)` - Set current cell
- `DoubleClickCurrentCell()` - Double-click current cell
- `SelectAll()` - Select all cells
- `ClearSelection()` - Clear selection
- `PressToolbarButton(ButtonId As String)` - Press toolbar button
- `PressToolbarContextButton(ButtonId As String)` - Press context button
- `SelectColumn(Column As String)` - Select entire column
- `DeselectColumn(Column As String)` - Deselect column
- `GetColumnNames() As Variant` - Get array of column names

**Usage Example:**
```vbscript
Set grid = session.findById("wnd[0]/usr/cntlGRID/shellcont/shell")

' Get cell value
cellValue = grid.GetCellValue(0, "MATNR")

' Set cell value
grid.SetCellValue(0, "MATNR", "100-100")

' Set current cell and double-click
grid.SetCurrentCell 0, "MATNR"
grid.DoubleClickCurrentCell

' Iterate through rows
For i = 0 To grid.RowCount - 1
    material = grid.GetCellValue(i, "MATNR")
    WScript.Echo "Material: " & material
Next
```

### GuiTableControl

**Description:** Table control for displaying rows of data.

**Characteristics:**
- Different from GuiGridView
- Older table control type
- Has scrollbar for navigation

**Key Properties:**
- `RowCount` (Long, Read-only) - Total number of rows
- `VerticalScrollbar` - Scrollbar object

**Usage Example:**
```vbscript
Set table = session.findById("wnd[0]/usr/tblSAPMTable")

' Access cell
session.findById("wnd[0]/usr/tblSAPMTable/txtFIELD[0,0]").Text = "Value"

' Scroll table
table.VerticalScrollbar.Position = 10
```

### GuiTree

**Description:** Tree control for hierarchical data display.

**Characteristics:**
- Extends GuiVContainer
- Displays data in tree/hierarchy structure
- Supports node expansion, selection, and navigation

**Key Methods:**
- `ExpandNode(NodeKey As String)` - Expand tree node
- `CollapseNode(NodeKey As String)` - Collapse tree node
- `SelectNode(NodeKey As String)` - Select tree node
- `DoubleClickNode(NodeKey As String)` - Double-click node
- `GetNodeText(NodeKey As String) As String` - Get node text

### GuiShell

**Description:** Abstract object for SAP GUI controls (containers and custom controls).

**Characteristics:**
- Extends GuiVContainer
- Abstract interface supported by all controls
- Objects of type GuiShell do not have a name
- When multiple GuiShell objects exist on same level, a one-dimensional index is appended for uniqueness

**Subtypes:**
- GuiBarChart
- GuiChart
- GuiCalendar
- GuiPicture
- GuiTextedit
- GuiCustomControl
- Various other controls

### GuiBarChart

**Description:** Control to display and modify time scale diagrams.

**Characteristics:**
- Extends GuiShell
- Very technical nature
- Should only be used for recording and playback
- Parameters cannot be easily determined manually

### GuiChart

**Description:** Chart control for data visualization.

**Characteristics:**
- Very technical nature
- Should only be used for recording and playback
- Parameters cannot be easily determined manually

### GuiCalendar

**Description:** Calendar control for date selection.

**Characteristics:**
- Extends GuiShell
- Used to select single dates or time periods

### GuiPicture

**Description:** Picture control to display images on SAP GUI screens.

**Characteristics:**
- Extends GuiShell
- Displays static or dynamic images

### GuiTextedit

**Description:** Multiline text editor control.

**Characteristics:**
- Extends GuiShell
- Offers multiline editing capabilities
- Can protect text parts against editing

### GuiCustomControl

**Description:** Custom control container.

**Characteristics:**
- Type number: 50
- Used for specialized custom controls

---

## Common Properties

### Properties Available on Most Objects

#### GuiComponent Properties (Base Interface)

- `ContainerType` (Boolean, Read-only) - Whether object is a container
- `Id` (String, Read-only) - Unique object identifier
- `Name` (String, Read-only) - Object name
- `Parent` (GuiComponent, Read-only) - Parent object
- `Type` (String, Read-only) - Type identifier (e.g., "GuiTextField")
- `TypeAsNumber` (Long, Read-only) - Numeric type identifier

#### GuiVComponent Properties (Visual Component)

- `AccLabel` (String, Read-only) - Accessibility label
- `AccText` (String, Read-only) - Accessibility text
- `AccTooltip` (String, Read-only) - Accessibility tooltip
- `Changeable` (Boolean, Read-only) - Whether object can be changed
- `DefaultTooltip` (String, Read-only) - Default tooltip text
- `Height` (Long) - Height in pixels
- `IconName` (String, Read-only) - Icon identifier
- `IsSymbolFont` (Boolean, Read-only) - Whether symbol font is used
- `Left` (Long) - Left position in pixels
- `Modified` (Boolean) - Whether object has been modified
- `Parent` (GuiVComponent, Read-only) - Parent component
- `ScreenLeft` (Long, Read-only) - Screen position left
- `ScreenTop` (Long, Read-only) - Screen position top
- `Text` (String) - Text property (varies by object type)
- `Tooltip` (String, Read-only) - Tooltip text
- `Top` (Long) - Top position in pixels
- `Width` (Long) - Width in pixels

#### GuiVContainer Properties (Visual Container)

- `Children` (GuiComponentCollection, Read-only) - All direct children

### State Properties

**Changeable:**
An object is changeable if it is neither disabled nor read-only. This property indicates whether the field can accept user input.

**Highlighted:**
Indicates the field is highlighted (defined in data dictionary).

**Modified:**
Indicates whether the field value has been changed from its original value.

---

## Common Methods

### Navigation and Interaction Methods

#### findById

**Syntax:**
```vbscript
Function findById(Id As String, Optional raise As Boolean = True) As GuiComponent
```

**Description:**
Searches through object descendants for a given ID. Objects exposing the GuiVContainer interface support this method.

**Parameters:**
- `Id` - Object ID to find
- `raise` - Whether to raise exception if not found (default: True)

**Returns:** GuiComponent or Nothing

**Usage Example:**
```vbscript
Set textField = session.findById("wnd[0]/usr/txtRSYST-BNAME")
```

#### startTransaction

**Syntax:**
```vbscript
Sub startTransaction(Transaction As String)
```

**Description:**
Starts a given SAP transaction in the session.

**Parameters:**
- `Transaction` - Technical name of the transaction

**Usage Example:**
```vbscript
session.startTransaction "VA01"
```

#### sendVKey

**Syntax:**
```vbscript
Sub sendVKey(VKey As Long)
```

**Description:**
Sends a virtual key (function key) to the window.

**Parameters:**
- `VKey` - Virtual key number (see VKey Codes section)

**Usage Example:**
```vbscript
session.findById("wnd[0]").sendVKey 0   ' Enter
session.findById("wnd[0]").sendVKey 3   ' F3 - Back
session.findById("wnd[0]").sendVKey 8   ' F8 - Execute
```

#### SendCommand

**Syntax:**
```vbscript
Sub SendCommand(Command As String)
```

**Description:**
Sends a command directly to the session.

**Usage Example:**
```vbscript
session.SendCommand "/nSE38"  ' Go to transaction SE38
```

### Control-Specific Methods

#### press (GuiButton)

**Syntax:**
```vbscript
Sub press()
```

**Description:**
Emulates pressing a button.

#### select (GuiRadioButton, GuiCheckBox, GuiTab)

**Syntax:**
```vbscript
Sub select()
```

**Description:**
Selects the radio button, checks the checkbox, or activates the tab.

#### SetFocus

**Syntax:**
```vbscript
Sub SetFocus()
```

**Description:**
Sets input focus to the component.

#### SetCurrentCell (GuiGridView)

**Syntax:**
```vbscript
Sub SetCurrentCell(Row As Long, Column As String)
```

**Description:**
Sets the current cell in a grid.

#### DoubleClick, DoubleClickCurrentCell

**Syntax:**
```vbscript
Sub DoubleClick()
Sub DoubleClickCurrentCell()  ' For GuiGridView
```

**Description:**
Performs a double-click action on the component.

### Session Management Methods

#### CreateSession

**Syntax:**
```vbscript
Function CreateSession() As GuiSession
```

**Description:**
Creates a new session in the current connection.

#### LockSessionUI / UnlockSessionUI

**Syntax:**
```vbscript
Sub LockSessionUI()
Sub UnlockSessionUI()
```

**Description:**
Locks or unlocks the session UI during script execution to prevent user interaction.

### Window Methods

#### resizeWorkingPane (GuiMainWindow)

**Syntax:**
```vbscript
Sub resizeWorkingPane(width As Long, height As Long)
```

**Description:**
Resizes the window so the working area has specified dimensions in character metric.

#### Close, Maximize, Iconify, Restore

**Syntax:**
```vbscript
Sub Close()
Sub Maximize()
Sub Iconify()
Sub Restore()
```

**Description:**
Window manipulation methods.

---

## VKey Codes

### Virtual Key Reference

VKey codes are used with the `sendVKey()` method to simulate function keys and special keys.

**Common VKey Codes:**

| VKey | Key | Description |
|------|-----|-------------|
| 0 | Enter | Enter/Confirm |
| 1 | F1 | Help |
| 2 | F2 | (varies) |
| 3 | F3 | Back |
| 4 | F4 | Possible Entries (Value Help) |
| 5 | F5 | (varies) |
| 6 | F6 | (varies) |
| 7 | F7 | (varies) |
| 8 | F8 | Execute |
| 9 | F9 | (varies) |
| 10 | F10 | (varies) |
| 11 | F11 | Save |
| 12 | F12 | Cancel/Exit |
| 15 | Shift+F3 | (varies) |
| 25 | Shift+F1 | (varies) |

**Note:** The exact function of each key may vary depending on the current screen and transaction. Use the `getVKeyDescription()` method to translate VKey numbers to readable text.

**Reference Location:**
Complete VKey list is available in:
- SAP GUI Scripting API help file: `C:\Program Files (x86)\SAP\Frontend\SAPgui\SAPguihelp\SAPGUIScripting.chm`
- Look for table GUI_FKEY

**Usage Example:**
```vbscript
' Get VKey description
description = session.findById("wnd[0]").getVKeyDescription(0)
' Returns: "Enter"
```

---

## Scrollbar Control

### VerticalScrollbar and HorizontalScrollbar

Both scrollbar objects are accessible on scrollable containers.

**Key Properties:**

- `Position` (Long) - Current scroll position (cursor position)
- `Maximum` (Long, Read-only) - Maximum scroll position (total rows/columns)
- `Minimum` (Long, Read-only) - Minimum scroll position (usually 0 or first row/column)
- `PageSize` (Long, Read-only) - Number of rows/columns visible per page

**Usage Example:**
```vbscript
' Get scrollbar
Set vScroll = session.findById("wnd[0]/usr").VerticalScrollbar

' Get maximum position
maxPos = vScroll.Maximum

' Scroll to position
vScroll.Position = 10

' Scroll incrementally
vScroll.Position = vScroll.Position + 1

' Scroll to end
vScroll.Position = vScroll.Maximum
```

**Important Note:**
Changing the scrollbar position causes server communication and may invalidate previous object references. Always re-obtain object references after scrolling.

---

## Collections and Iteration

### GuiComponentCollection

**Description:** Collection of GuiComponent objects.

**Properties:**
- `Count` (Long, Read-only) - Number of items in collection
- `Length` (Long, Read-only) - Same as Count

**Methods:**
- `Item(Index As Long) As GuiComponent` - Get item by index (0-based)
- `ElementAt(Index As Long) As GuiComponent` - Same as Item

**Usage Example:**
```vbscript
Set children = session.findById("wnd[0]/usr").Children

' Iterate through children
For i = 0 To children.Count - 1
    Set child = children.Item(i)
    WScript.Echo child.Id & " - " & child.Type
Next

' Alternative syntax
For Each child In children
    WScript.Echo child.Id
Next
```

### GuiCollection

**Description:** Similar to GuiComponentCollection, but members are not necessarily extensions of GuiComponent.

**Properties:**
- `Count` (Long, Read-only) - Number of items
- `ItemCount` (Long, Read-only) - Same as Count

**Important Note on Children Property:**
The Children property of GuiConnection can block execution before it's possible to use the Busy property. This blocks execution until the session is available again. Use with caution in loops or when checking busy state.

---

## Error Handling

### Error Detection

**Key Principle:**
SAP GUI Scripting does not automatically throw errors when SAP application errors occur. It only throws errors when it cannot find elements or encounters scripting issues.

**To detect SAP application errors, monitor the status bar:**

```vbscript
Set statusbar = session.findById("wnd[0]/sbar")

If statusbar.MessageType = "E" Or statusbar.MessageType = "W" Then
    WScript.Echo "Error/Warning: " & statusbar.Text
    WScript.Echo "Message ID: " & statusbar.MessageId
    WScript.Echo "Message Number: " & statusbar.MessageNumber
    ' Handle error
End If
```

### Exception Types

#### GuiComponentException

**E_ACCESSDENIED:**
Raised by `openConnection()` if scripting support has been disabled by administrator.

**Handling Connection Errors:**
Set the `sync` parameter to True in `openConnection()` to handle connection errors gracefully:

```vbscript
On Error Resume Next
Set connection = application.OpenConnection("System", True)  ' Sync = True
If Err.Number <> 0 Then
    WScript.Echo "Connection failed: " & Err.Description
    WScript.Quit
End If
On Error GoTo 0
```

### VBScript Error Handling Example

```vbscript
On Error Resume Next

' Attempt operation
Set element = session.findById("wnd[0]/usr/txtFIELD")
If Err.Number <> 0 Then
    WScript.Echo "Element not found: " & Err.Description
    Err.Clear
    ' Handle error
End If

' Set value
element.Text = "Value"
If Err.Number <> 0 Then
    WScript.Echo "Could not set value: " & Err.Description
    Err.Clear
End If

On Error GoTo 0
```

### Best Practices for Error Handling

1. **Always check the status bar** after operations that might fail
2. **Use On Error Resume Next** judiciously in VBScript
3. **Verify object existence** before using findById results
4. **Check Busy property** before operations to avoid timing issues
5. **Use sync parameter** in connection operations
6. **Log errors** for debugging and auditing
7. **Implement retry logic** for transient errors
8. **Clean up resources** in error scenarios

---

## Programming Languages Support

### Supported Languages

SAP GUI Scripting API is a COM-based interface and can be used with any COM-enabled language:

**Primary Languages:**
- **VBScript** - Most commonly documented, built-in Windows scripting
- **Visual Basic for Applications (VBA)** - Excel, Word, Access integration
- **Visual Basic / VB.NET** - Full .NET support
- **C#** - Full .NET support via COM interop
- **PowerShell** - Both Desktop and Core versions
- **Python** - Via PyWin32 (win32com library)
- **JavaScript** - Via Windows Script Host or SAP GUI for Java built-in engine
- **AutoIt** - Windows automation language
- **Java** - Via Java-COM-Bridge (JaCoB)

### Language-Specific Setup

#### VBScript (Windows Script Host)

No setup required. Use `GetObject("SAPGUI")` to access the API.

```vbscript
Set SapGuiAuto = GetObject("SAPGUI")
Set application = SapGuiAuto.GetScriptingEngine
Set connection = application.Children(0)
Set session = connection.Children(0)
```

#### VBA (Excel, Word, Access)

**Setup:**
1. In VBA Editor, go to Tools > References
2. Click Browse
3. Navigate to `C:\Program Files (x86)\SAP\FrontEnd\SAPgui`
4. Select `sapfewse.ocx`
5. Click OK

**Code:**
```vba
Sub SAPAutomation()
    Dim SapGuiAuto As Object
    Dim application As Object
    Dim connection As Object
    Dim session As Object

    Set SapGuiAuto = GetObject("SAPGUI")
    Set application = SapGuiAuto.GetScriptingEngine
    Set connection = application.Children(0)
    Set session = connection.Children(0)

    ' Your automation code here
End Sub
```

#### C#

**Setup:**
Add reference to `sapfewse.ocx` or use dynamic typing.

**Code:**
```csharp
using System;
using SAPFEWSELib;

class Program
{
    static void Main()
    {
        // Get SAP GUI Scripting engine
        dynamic SapGuiAuto = System.Runtime.InteropServices.Marshal.GetActiveObject("SAPGUI");
        dynamic application = SapGuiAuto.GetScriptingEngine;
        dynamic connection = application.Children(0);
        dynamic session = connection.Children(0);

        // Your automation code here
        session.StartTransaction("SE38");
    }
}
```

#### Python

**Setup:**
Install PyWin32: `pip install pywin32`

**Code:**
```python
import win32com.client

# Get SAP GUI Scripting engine
SapGuiAuto = win32com.client.GetObject("SAPGUI")
application = SapGuiAuto.GetScriptingEngine
connection = application.Children(0)
session = connection.Children(0)

# Your automation code here
session.StartTransaction("SE38")
session.findById("wnd[0]/usr/txtRS38M-PROGRAMM").Text = "ZTEST"
session.findById("wnd[0]").sendVKey(8)  # F8 - Execute
```

#### PowerShell

**Code:**
```powershell
# Get SAP GUI Scripting engine
$SapGuiAuto = [System.Runtime.InteropServices.Marshal]::GetActiveObject("SAPGUI")
$application = $SapGuiAuto.GetScriptingEngine
$connection = $application.Children(0)
$session = $connection.Children(0)

# Your automation code here
$session.StartTransaction("SE38")
$session.findById("wnd[0]/usr/txtRS38M-PROGRAMM").Text = "ZTEST"
$session.findById("wnd[0]").sendVKey(8)
```

---

## Development Tools

### Script Recorder

**Built-in SAP GUI Tool**

**Access:**
```
Customize Local Layout (ALT+F12) > Script Recording and Playback > Record Script
```

**Features:**
- Records user interactions in SAP GUI
- Generates VBScript code
- Captures all user actions (clicks, text entry, navigation)
- Provides foundation for script development

**Workflow:**
1. Start recorder
2. Choose output file location
3. Perform SAP operations
4. Stop recorder
5. Review and optimize generated script

**Limitations:**
- Generated code may be verbose
- May include unnecessary steps
- Requires manual optimization
- Limited to VBScript output

### Scripting Tracker

**Third-Party Development Tool (Retired)**

**Key Features:**
- SAP GUI analyzer and recorder
- Tree view of all SAP sessions and scripting objects
- Shows technical details: ID, position, properties
- Supports multiple script languages: VB, AutoIt, MiniRobot, PowerShell
- Comparator: Compare screen elements between screens
- DumpState: Detailed hierarchy and state information
- Enhanced editor for script development

**Note:** Scripting Tracker has been officially retired. Use with caution and awareness of guarantee exclusion.

**Alternative:** SAP GUI Scripting Spy

### SAP GUI Scripting Spy

**Built-in Object Inspector**

**Features:**
- Identify SAP GUI objects by clicking on them
- Retrieve properties of UI controls
- View object IDs and technical details
- Useful for script development and debugging

**Access:**
Available through SAP GUI scripting development features.

### Scripting Tracker Lite

**Simplified Version**
- Lightweight alternative to full Scripting Tracker
- Basic object identification and recording
- Suitable for simple automation tasks

### NwbcPropertyCollector

**Property Inspector Tool**

**Location:**
`C:\Program Files\SAP\NWBC800\NwbcPropertyCollector.exe`

**Purpose:**
- View detailed properties of SAP GUI fields
- Useful for understanding object structure
- Helps in script development

### SAP GUI Scripting Help

**Built-in Documentation**

**Location:**
`C:\Program Files (x86)\SAP\Frontend\SAPgui\SAPguihelp\SAPGUIScripting.chm`

**Contents:**
- Complete API reference
- Object model documentation
- VKey reference (GUI_FKEY table)
- Code examples
- Best practices

**Access:**
From SAP GUI: Customize button menu > SAP GUI Scripting Help

---

## Best Practices

### General Best Practices

1. **Enable Scripting Properly**
   - Enable on both server and client side
   - Use profile parameters for server configuration
   - Set appropriate security settings

2. **Test in Development/Sandbox First**
   - Never test scripts in production
   - Verify logic and error handling
   - Automation can make mistakes rapidly

3. **Implement Proper Error Handling**
   - Check status bar for SAP errors
   - Use On Error Resume Next appropriately
   - Log errors for debugging
   - Implement retry logic for transient errors

4. **Use Descriptive Object IDs**
   - Rely on findById with full paths
   - Avoid hardcoding indexes where possible
   - Document non-obvious object IDs

5. **Manage Session State**
   - Check Busy property before operations
   - Use LockSessionUI/UnlockSessionUI for user protection
   - Clean up sessions and connections properly

6. **Optimize Performance**
   - Minimize server roundtrips
   - Batch operations where possible
   - Avoid unnecessary scrolling or screen changes

### Performance Considerations

1. **Script Execution Speed**
   - Scripts execute significantly faster than manual operations
   - May increase system load
   - Monitor performance impact

2. **Performance Degradation Over Time**
   - Long-running scripts may slow down
   - Each operation can increase in duration
   - Solution: Close and reopen sessions periodically

3. **Server Communication**
   - Many operations trigger server communication
   - Plan operations to minimize roundtrips
   - Use batch methods when available

### Security Best Practices

1. **Use User-Specific Scripting**
   - Enable `sapgui/user_scripting_per_user`
   - Assign S_SCR authority only to authorized users
   - Monitor script execution

2. **Protect Credentials**
   - Never hardcode passwords in scripts
   - Use secure credential storage
   - Implement proper authentication

3. **Read-Only Mode for Reporting**
   - Use `sapgui/user_scripting_set_readonly` for read-only operations
   - Reduces risk of accidental modifications
   - Appropriate for data extraction scripts

4. **Disable Recording When Not Needed**
   - Use `sapgui/user_scripting_disable_recording`
   - Prevents unauthorized script recording
   - Allows execution of existing scripts

### Code Organization Best Practices

1. **Modular Design**
   - Break scripts into reusable functions/subroutines
   - Separate connection logic from business logic
   - Create libraries for common operations

2. **Configuration Management**
   - Externalize configuration (connection strings, paths)
   - Use configuration files or environment variables
   - Avoid hardcoding system-specific values

3. **Logging and Auditing**
   - Log script execution start/end
   - Log significant operations
   - Record errors and exceptions
   - Implement audit trail for compliance

4. **Documentation**
   - Document script purpose and usage
   - Comment complex logic
   - Maintain version history
   - Document dependencies and requirements

### Script Development Workflow

1. **Record First**
   - Use script recorder to capture basic flow
   - Understand object IDs and structure
   - Provides foundation for development

2. **Optimize Recorded Script**
   - Remove unnecessary operations
   - Add error handling
   - Add variables and logic
   - Make code reusable

3. **Test Incrementally**
   - Test each section independently
   - Verify error handling
   - Test edge cases and failure scenarios

4. **Deploy with Controls**
   - Version control scripts
   - Document deployment procedures
   - Implement change management
   - Provide user training

### Common Pitfalls to Avoid

1. **Not Checking Status Bar**
   - Scripting doesn't throw errors for SAP application errors
   - Always check MessageType after critical operations

2. **Hardcoding Object Indexes**
   - Object positions may change
   - Use names and stable identifiers where possible

3. **Ignoring Busy State**
   - Operations on busy sessions may fail
   - Always check or wait for session to be available

4. **Not Handling Popups**
   - Modal windows change the object hierarchy
   - Script must handle unexpected popups
   - Implement popup detection and handling

5. **Overusing Children Property**
   - Children property on GuiConnection can block
   - Use specific object access when possible

6. **Not Cleaning Up Resources**
   - Close windows and sessions properly
   - Disconnect when finished
   - Prevent resource leaks

---

## Code Examples

### Example 1: Basic Connection and Login

```vbscript
' Basic SAP GUI Scripting - Connection and Navigation
Set SapGuiAuto = GetObject("SAPGUI")
Set application = SapGuiAuto.GetScriptingEngine

' Open connection (or use existing)
If application.Connections.Count = 0 Then
    Set connection = application.OpenConnection("SAP System Name", True)
Else
    Set connection = application.Connections(0)
End If

' Get or create session
If connection.Sessions.Count = 0 Then
    Set session = connection.Children(0)
Else
    Set session = connection.Sessions(0)
End If

' Navigate to transaction
session.StartTransaction "SE38"

' Fill in program name
session.findById("wnd[0]/usr/txtRS38M-PROGRAMM").Text = "ZTEST_PROGRAM"

' Execute (F8)
session.findById("wnd[0]").sendVKey 8

WScript.Echo "Script completed successfully"
```

### Example 2: Error Handling with Status Bar Check

```vbscript
' SAP GUI Scripting with Error Handling
Set SapGuiAuto = GetObject("SAPGUI")
Set application = SapGuiAuto.GetScriptingEngine
Set connection = application.Children(0)
Set session = connection.Children(0)

On Error Resume Next

' Start transaction
session.StartTransaction "VA01"

' Fill sales order data
session.findById("wnd[0]/usr/ctxtVBAK-VKORG").Text = "1000"
session.findById("wnd[0]/usr/ctxtVBAK-VTWEG").Text = "10"
session.findById("wnd[0]/usr/ctxtVBAK-SPART").Text = "00"

' Press Enter
session.findById("wnd[0]").sendVKey 0

' Check for errors
Set statusbar = session.findById("wnd[0]/sbar")
If statusbar.MessageType = "E" Then
    WScript.Echo "Error occurred: " & statusbar.Text
    WScript.Echo "Message ID: " & statusbar.MessageId
    WScript.Echo "Message Number: " & statusbar.MessageNumber
    WScript.Quit 1
ElseIf statusbar.MessageType = "W" Then
    WScript.Echo "Warning: " & statusbar.Text
End If

On Error GoTo 0
WScript.Echo "Operation completed successfully"
```

### Example 3: Excel VBA Integration

```vba
Sub ExportSAPDataToExcel()
    Dim SapGuiAuto As Object
    Dim application As Object
    Dim connection As Object
    Dim session As Object
    Dim ws As Worksheet

    ' Set up worksheet
    Set ws = ThisWorkbook.Sheets("SAP Data")

    ' Connect to SAP
    Set SapGuiAuto = GetObject("SAPGUI")
    Set application = SapGuiAuto.GetScriptingEngine
    Set connection = application.Children(0)
    Set session = connection.Children(0)

    ' Navigate to transaction
    session.StartTransaction "MB52"

    ' Fill selection criteria from Excel
    session.findById("wnd[0]/usr/ctxtMATNR-LOW").Text = ws.Range("B2").Value
    session.findById("wnd[0]/usr/ctxtWERKS-LOW").Text = ws.Range("B3").Value

    ' Execute
    session.findById("wnd[0]").sendVKey 8

    ' Wait for processing
    Do While session.Busy
        DoEvents
    Loop

    ' Check for errors
    If session.findById("wnd[0]/sbar").MessageType = "E" Then
        MsgBox "Error: " & session.findById("wnd[0]/sbar").Text
        Exit Sub
    End If

    ' Export to Excel (example assumes ALV grid)
    Set grid = session.findById("wnd[0]/usr/cntlGRID/shellcont/shell")

    Dim row As Long
    Dim col As Long

    ' Write headers
    For col = 0 To grid.ColumnCount - 1
        ws.Cells(1, col + 1).Value = grid.GetColumnNames()(col)
    Next col

    ' Write data
    For row = 0 To grid.RowCount - 1
        For col = 0 To grid.ColumnCount - 1
            ws.Cells(row + 2, col + 1).Value = grid.GetCellValue(row, grid.GetColumnNames()(col))
        Next col
    Next row

    MsgBox "Data exported successfully!"
End Sub
```

### Example 4: Python Integration

```python
import win32com.client
import time

def sap_material_lookup(material_number):
    """
    Look up material information in SAP
    """
    try:
        # Connect to SAP
        SapGuiAuto = win32com.client.GetObject("SAPGUI")
        application = SapGuiAuto.GetScriptingEngine
        connection = application.Children(0)
        session = connection.Children(0)

        # Navigate to transaction
        session.StartTransaction("MM03")

        # Enter material number
        session.findById("wnd[0]/usr/ctxtRMMG1-MATNR").Text = material_number

        # Press Enter
        session.findById("wnd[0]").sendVKey(0)

        # Wait for processing
        while session.Busy:
            time.sleep(0.1)

        # Check for errors
        statusbar = session.findById("wnd[0]/sbar")
        if statusbar.MessageType == "E":
            return {"error": statusbar.Text}

        # Extract material data
        material_data = {
            "material": session.findById("wnd[0]/usr/subSUB_ALL:SAPLMGMM:0100/subSUB_MAT:SAPLMGMM:0101/txtRMMG1-MATNR").Text,
            "description": session.findById("wnd[0]/usr/subSUB_ALL:SAPLMGMM:0100/subSUB_MAT:SAPLMGMM:0101/txtMAKT-MAKTX").Text,
            # Add more fields as needed
        }

        # Go back
        session.findById("wnd[0]").sendVKey(3)

        return material_data

    except Exception as e:
        return {"error": str(e)}

# Usage
result = sap_material_lookup("100-100")
print(result)
```

### Example 5: Working with Grid Controls

```vbscript
' Working with SAP GUI Grid Control
Set SapGuiAuto = GetObject("SAPGUI")
Set application = SapGuiAuto.GetScriptingEngine
Set connection = application.Children(0)
Set session = connection.Children(0)

' Navigate to transaction with ALV grid
session.StartTransaction "SE16"
session.findById("wnd[0]/usr/ctxtDATABROWSE-TABLENAME").Text = "MARA"
session.findById("wnd[0]").sendVKey 0

' Fill selection criteria
session.findById("wnd[0]/usr/txtMAX_SEL").Text = "100"
session.findById("wnd[0]/tbar[1]/btn[8]").press  ' Execute

' Get grid reference
Set grid = session.findById("wnd[0]/usr/cntlGRID1/shellcont/shell")

' Get column names
columnNames = grid.GetColumnNames()

' Write data to file
Set fso = CreateObject("Scripting.FileSystemObject")
Set outputFile = fso.CreateTextFile("output.csv", True)

' Write headers
headerLine = ""
For Each colName In columnNames
    headerLine = headerLine & colName & ","
Next
outputFile.WriteLine Left(headerLine, Len(headerLine) - 1)

' Write data rows
For row = 0 To grid.RowCount - 1
    dataLine = ""
    For Each colName In columnNames
        cellValue = grid.GetCellValue(row, colName)
        dataLine = dataLine & cellValue & ","
    Next
    outputFile.WriteLine Left(dataLine, Len(dataLine) - 1)
Next row

outputFile.Close
WScript.Echo "Data exported to output.csv"
```

### Example 6: Handling Multiple Sessions

```vbscript
' Working with Multiple SAP Sessions
Set SapGuiAuto = GetObject("SAPGUI")
Set application = SapGuiAuto.GetScriptingEngine
Set connection = application.Children(0)

' Get or create first session
Set session1 = connection.Children(0)

' Create second session
session1.CreateSession

' Get reference to new session
Set session2 = connection.Children(1)

' Work in session 1
session1.StartTransaction "VA01"
session1.findById("wnd[0]/usr/ctxtVBAK-VKORG").Text = "1000"

' Simultaneously work in session 2
session2.StartTransaction "VA02"
session2.findById("wnd[0]/usr/ctxtVBAK-VBELN").Text = "12345"

' Continue with parallel operations
WScript.Echo "Multiple sessions active"
```

### Example 7: Scrolling Through Table Control

```vbscript
' Scrolling Through SAP Table Control
Set SapGuiAuto = GetObject("SAPGUI")
Set application = SapGuiAuto.GetScriptingEngine
Set connection = application.Children(0)
Set session = connection.Children(0)

' Navigate to transaction with table
session.StartTransaction "VA02"
' ... fill header data ...

' Get table reference
Set table = session.findById("wnd[0]/usr/tblSAPMV45ATCTRL_U_ERF_AUFTRAG")

' Get scrollbar
Set vScroll = table.VerticalScrollbar

' Process all rows
currentPos = 0
Do While currentPos <= vScroll.Maximum
    ' Process visible rows
    For i = 0 To table.VisibleRowCount - 1
        ' Process row data
        material = session.findById("wnd[0]/usr/tblSAPMV45ATCTRL_U_ERF_AUFTRAG/txtVBAP-MATNR[1," & i & "]").Text
        WScript.Echo "Material: " & material
    Next

    ' Scroll down
    currentPos = currentPos + table.VisibleRowCount
    vScroll.Position = currentPos
Loop

WScript.Echo "All rows processed"
```

### Example 8: Robust Connection with Error Handling

```vbscript
' Robust SAP Connection with Full Error Handling
Function ConnectToSAP()
    On Error Resume Next

    ' Get SAP GUI Scripting engine
    Set SapGuiAuto = GetObject("SAPGUI")
    If Err.Number <> 0 Then
        WScript.Echo "Error: SAP GUI not found. Please ensure SAP GUI is installed."
        Err.Clear
        ConnectToSAP = False
        Exit Function
    End If

    Set application = SapGuiAuto.GetScriptingEngine
    If Err.Number <> 0 Then
        WScript.Echo "Error: Could not get scripting engine. Please enable SAP GUI Scripting."
        Err.Clear
        ConnectToSAP = False
        Exit Function
    End If

    ' Check if connection exists
    If application.Connections.Count = 0 Then
        WScript.Echo "Error: No active SAP connection found. Please log in to SAP first."
        ConnectToSAP = False
        Exit Function
    End If

    Set connection = application.Connections(0)

    ' Check if session exists
    If connection.Sessions.Count = 0 Then
        WScript.Echo "Error: No active session found."
        ConnectToSAP = False
        Exit Function
    End If

    Set session = connection.Sessions(0)

    ' Verify scripting is not disabled server-side
    If connection.DisabledByServer Then
        WScript.Echo "Error: Scripting is disabled on the SAP server. Contact your administrator."
        ConnectToSAP = False
        Exit Function
    End If

    On Error GoTo 0
    ConnectToSAP = True
End Function

' Main script
If ConnectToSAP() Then
    ' Your automation code here
    session.StartTransaction "SE38"
    WScript.Echo "Successfully connected and navigated to SE38"
Else
    WScript.Echo "Connection failed. Script terminated."
    WScript.Quit 1
End If
```

---

## Batch Input vs SAP GUI Scripting

### Understanding the Difference

**Batch Input (BDC - Batch Data Communication):**
- ABAP-based technique for uploading data
- Two methods: Session Method and Call Transaction
- Server-side processing
- Primarily for data migration and mass uploads

**SAP GUI Scripting:**
- Client-side automation of SAP GUI
- Uses COM API
- Simulates user interactions
- Suitable for process automation and testing

### Batch Input Methods Comparison

#### Session Method
- **Processing:** Asynchronous (batch mode)
- **Error Logging:** Automatic log file generated
- **Data Volume:** Supports both small and large amounts
- **Performance:** Slower than Call Transaction
- **Database Updates:** Sequential transaction processing
- **Error Handling:** Errors logged in SM35 for reprocessing
- **Use Case:** Large data uploads, migration projects

#### Call Transaction Method
- **Processing:** Synchronous (real-time)
- **Error Logging:** No automatic log (manual handling required)
- **Data Volume:** Best for small amounts
- **Performance:** Faster than Session Method
- **Database Updates:** Immediate commit before and after
- **Error Handling:** Must be implemented in program
- **Use Case:** Real-time processing, small batches

### When to Use Each Approach

**Use Batch Input When:**
- Uploading large volumes of data
- Need automatic error logging
- Performing data migration
- Require reprocessing capabilities
- Server-side processing is acceptable

**Use SAP GUI Scripting When:**
- Automating user processes
- Testing user interfaces
- Integrating SAP with external applications
- Need client-side control
- Simulating user interactions is required

---

## API Reference Summary

### Object Type Numbers

Quick reference for typeAsNumber property:

| Type Number | Object Type |
|-------------|-------------|
| 50 | GuiCustomControl |
| 71 | GuiSimpleContainer |
| 72 | GuiScrollContainer |
| 74 | GuiUserArea |

### Type Prefixes

Common type prefixes for object IDs:

| Prefix | Object Type |
|--------|-------------|
| app | GuiApplication |
| con | GuiConnection |
| ses | GuiSession |
| wnd | GuiFrameWindow |
| usr | GuiUserArea |
| txt | GuiTextField |
| ctxt | GuiCTextField |
| pwd | GuiPasswordField |
| btn | GuiButton |
| chk | GuiCheckBox |
| rad | GuiRadioButton |
| cmb | GuiComboBox |
| tbar | GuiToolbar |
| mbar | GuiMenubar |
| menu | GuiMenu |
| titl | GuiTitlebar |
| sbar | GuiStatusbar |
| tabs | GuiTabStrip |
| tabp | GuiTab |
| tbl | GuiTableControl |
| shell | GuiShell |

---

## Frequently Asked Questions

### How do I enable SAP GUI Scripting?

**Server Side:**
- Set profile parameter `sapgui/user_scripting = TRUE` using RZ11 or RZ10
- Restart SAP server

**Client Side:**
- SAP GUI Options > Accessibility & Scripting > Scripting > Enable scripting
- Security Configuration > Customized > Allow

### Why doesn't my script detect SAP errors?

SAP GUI Scripting doesn't automatically throw errors for SAP application errors. Always check the status bar:
```vbscript
If session.findById("wnd[0]/sbar").MessageType = "E" Then
    ' Handle error
End If
```

### How do I find object IDs?

Use these methods:
1. **Script Recorder** - Record actions to see object IDs
2. **Scripting Tracker** - Visual object explorer
3. **SAP GUI Scripting Spy** - Click objects to see IDs
4. **Manual inspection** - Hover over fields and check properties

### Can I use SAP GUI Scripting with SAP GUI for Java?

Yes, SAP GUI Scripting is available for both Windows and Java platforms. The Java version uses JavaScript as the built-in scripting engine and can also be accessed via Java-COM-Bridge.

### How do I handle popup windows?

Popup windows are GuiModalWindow objects with their own object IDs:
```vbscript
' Main window
session.findById("wnd[0]/...")

' Popup window
session.findById("wnd[1]/...")

' Second popup (if any)
session.findById("wnd[2]/...")
```

### Why is my script slow?

Common causes:
- Server communication on every operation
- Inefficient object lookups
- Unnecessary screen changes
- Long-running scripts degrading over time

Solutions:
- Minimize server roundtrips
- Cache object references
- Restart sessions periodically for long processes

### How do I automate complex transactions with dynamic screens?

Use conditional logic and error handling:
```vbscript
On Error Resume Next
Set element = session.findById("wnd[0]/usr/txtField")
If Err.Number = 0 Then
    ' Field exists, process it
    element.Text = "Value"
Else
    ' Field doesn't exist, handle alternate flow
    Err.Clear
End If
On Error GoTo 0
```

### Can I run SAP GUI scripts from web browsers?

SAP GUI Scripting is for SAP GUI (client software) only. For web-based SAP (SAP Fiori, SAP GUI for HTML), you need different automation approaches (Selenium, etc.).

### What are the security implications?

- Scripts can execute rapidly, potentially causing damage
- Always test in development/sandbox first
- Implement proper authorization (S_SCR authority object)
- Use read-only mode when possible
- Never hardcode credentials
- Audit script execution

### How do I debug scripts?

Techniques:
1. Add logging/echo statements throughout
2. Use step-by-step execution (in IDE)
3. Check status bar after each operation
4. Validate object existence before use
5. Use SAP GUI Scripting Help for reference
6. Test small sections independently

---

## Additional Resources

### Official Documentation

- **SAP GUI Scripting API PDF**
  - Latest version: 7.60, 7.70, 8.00
  - Location: SAP Help Portal (help.sap.com)
  - Complete object model reference

- **SAP GUI Scripting Security Guide**
  - Security considerations and configurations
  - Profile parameters reference
  - Best practices for secure scripting

- **SAP GUI Scripting User Guide**
  - Getting started guide
  - Tutorial and examples
  - Recording and playback instructions

### Local Help Files

- **Location:** `C:\Program Files (x86)\SAP\Frontend\SAPgui\SAPguihelp\`
- **Main Help:** `SAPGUIScripting.chm`
- **API Reference:** `ScriptingAPI\index.html`

### Community Resources

- **SAP Community** (community.sap.com)
  - Scripting tag
  - Questions and answers
  - Blog posts and tutorials

- **Stack Overflow**
  - Tag: sap-gui
  - Many practical examples and solutions

### GitHub Repositories

Numerous examples and libraries available on GitHub for various languages.

---

## Conclusion

SAP GUI Scripting provides a powerful COM-based API for automating SAP GUI interactions. This documentation covers:

- Complete object hierarchy and structure
- All major GUI objects and controls
- Properties, methods, and usage patterns
- Error handling and best practices
- Multi-language support and examples
- Development tools and debugging techniques
- Security considerations

### Key Takeaways

1. **Proper Configuration is Critical**
   - Enable scripting on both server and client
   - Implement appropriate security measures

2. **Error Handling is Essential**
   - Always check status bar for SAP errors
   - Implement robust exception handling

3. **Use Appropriate Tools**
   - Script recorder for learning
   - Scripting Tracker for analysis
   - Built-in help for reference

4. **Follow Best Practices**
   - Test in development first
   - Implement logging and auditing
   - Document your scripts
   - Use version control

5. **Security First**
   - Never hardcode credentials
   - Use appropriate authorization
   - Audit script execution
   - Consider read-only mode

### Next Steps

1. Review your specific use case requirements
2. Set up scripting in development environment
3. Start with simple automation tasks
4. Gradually build more complex scripts
5. Implement proper error handling and logging
6. Deploy with appropriate controls and monitoring

---

## Version History

**Documentation Version:** 1.0
**Date:** 2025
**Based on:** SAP GUI Scripting API versions 6.20 through 8.00
**Compiled from:** Official SAP documentation, community resources, and practical implementations

---

## Disclaimer

This documentation is compiled from publicly available SAP GUI Scripting resources and community knowledge. Always refer to official SAP documentation for your specific SAP GUI version. SAP GUI Scripting features and behavior may vary between versions.

**Important:** Test all scripts thoroughly in development/sandbox environments before production use. Automation can execute rapidly and potentially cause unintended consequences.

---

*End of SAP GUI Scripting Documentation*
