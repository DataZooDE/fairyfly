' List all SAP windows
Option Explicit

Dim SapGuiAuto, app, connection, session, i

' Connect to SAP
Set SapGuiAuto = GetObject("SAPGUI")
Set app = SapGuiAuto.GetScriptingEngine
Set connection = app.Children(0)
Set session = connection.Children(0)

WScript.Echo "=== SAP Windows ==="
WScript.Echo "Session has " & session.Children.Count & " windows"
WScript.Echo ""

For i = 0 To session.Children.Count - 1
    Dim wnd
    Set wnd = session.Children(i)

    On Error Resume Next
    Dim wndId, wndText, wndType
    wndId = wnd.Id
    wndText = wnd.Text
    wndType = TypeName(wnd)

    WScript.Echo "Window " & i & ":"
    WScript.Echo "  ID: " & wndId
    WScript.Echo "  Type: " & wndType
    WScript.Echo "  Text: " & wndText
    WScript.Echo ""
Next
