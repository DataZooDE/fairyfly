' Inspect RSRT Query Monitor Screen Structure
Option Explicit

Dim SapGuiAuto, app, connection, session

' Connect to SAP
Set SapGuiAuto = GetObject("SAPGUI")
Set app = SapGuiAuto.GetScriptingEngine
Set connection = app.Children(0)
Set session = connection.Children(0)

WScript.Echo "=== RSRT Screen Structure ==="
WScript.Echo ""

Call PrintElement(session.FindById("wnd[0]"), 0)

WScript.Echo ""
WScript.Echo "=== Done ==="

Sub PrintElement(element, indent)
    Dim indentStr, i, child
    indentStr = ""
    For i = 1 To indent
        indentStr = indentStr & "  "
    Next

    Dim typeStr, nameStr, idStr
    typeStr = TypeName(element)

    On Error Resume Next
    nameStr = element.Name
    If Err.Number <> 0 Then nameStr = ""
    Err.Clear

    idStr = element.Id
    If Err.Number <> 0 Then idStr = ""
    Err.Clear

    ' Try to get subtype for GuiShell
    Dim subtypeStr
    subtypeStr = ""
    If typeStr = "GuiShell" Or typeStr = "GuiSplitterShell" Then
        On Error Resume Next
        subtypeStr = " (SubType: " & element.SubType & ")"
        If Err.Number <> 0 Then subtypeStr = ""
        Err.Clear
    End If

    WScript.Echo indentStr & typeStr & " - " & idStr & subtypeStr

    ' Recursively print children
    Dim childCount
    childCount = 0
    On Error Resume Next
    childCount = element.Children.Count
    If Err.Number <> 0 Then childCount = 0
    Err.Clear

    If childCount > 0 Then
        For i = 0 To childCount - 1
            Set child = element.Children(i)
            Call PrintElement(child, indent + 1)
        Next
    End If
    On Error Goto 0
End Sub
