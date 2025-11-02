' Quick diagnostic for RSA1 screen
Option Explicit

Dim SapGuiAuto, app, connection, session, i

Set SapGuiAuto = GetObject("SAPGUI")
Set app = SapGuiAuto.GetScriptingEngine
Set connection = app.Children(0)
Set session = connection.Children(0)

WScript.Echo "=== RSA1 Screen Quick Diagnostic ==="
WScript.Echo ""

Dim wnd
Set wnd = session.FindById("wnd[0]")
WScript.Echo "Window ID: " & wnd.Id
WScript.Echo "Window Text: " & wnd.Text
WScript.Echo ""

' Check user area
Dim usr
On Error Resume Next
Set usr = session.FindById("wnd[0]/usr")
If Err.Number = 0 Then
    WScript.Echo "UserArea found, children: " & usr.Children.Count

    ' List immediate children
    For i = 0 To usr.Children.Count - 1
        Dim child
        Set child = usr.Children(i)
        WScript.Echo "  Child " & i & ": " & TypeName(child) & " - " & child.Id

        ' Check if it's a shell with subtype
        If TypeName(child) = "GuiShell" Or TypeName(child) = "GuiSplitterShell" Then
            On Error Resume Next
            WScript.Echo "    SubType: " & child.SubType
            On Error Goto 0
        End If
    Next
End If

WScript.Echo ""
WScript.Echo "=== Done ==="
