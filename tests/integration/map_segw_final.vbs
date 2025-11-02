' Final mapping of the actual Panel 2 structure
Option Explicit

Dim SapGuiAuto, Connection, Session
Set SapGuiAuto = GetObject("SAPGUI")
Set Connection = SapGuiAuto.GetScriptingEngine.Children(0)
Set Session = Connection.Children(0)

Sub InspectElement(path, description)
    On Error Resume Next
    Dim elem
    WScript.Echo "=== " & description & " ==="
    WScript.Echo "Path: " & path

    Set elem = Session.FindById(path)
    If Err.Number <> 0 Or elem Is Nothing Then
        WScript.Echo "ERROR: Element not found"
        Err.Clear
        WScript.Echo ""
        Exit Sub
    End If

    WScript.Echo "Type: " & elem.Type

    Dim subtype
    Err.Clear
    subtype = elem.SubType
    If Err.Number = 0 And subtype <> "" Then
        WScript.Echo "SubType: " & subtype
    End If
    Err.Clear

    Dim children
    Err.Clear
    Set children = elem.Children
    If Err.Number = 0 And Not children Is Nothing Then
        WScript.Echo "Children.Count: " & children.Count
    End If

    ' For GridView, show row/col
    If elem.Type = "GuiShell" And subtype = "GridView" Then
        Dim rowCount, colCount
        Err.Clear
        rowCount = elem.RowCount
        colCount = elem.ColumnCount
        If Err.Number = 0 Then
            WScript.Echo "RowCount: " & rowCount & ", ColumnCount: " & colCount
        End If
    End If

    ' For Tree, show node count
    If elem.Type = "GuiTree" Or (elem.Type = "GuiShell" And subtype = "Tree") Then
        Dim nodeCount
        Err.Clear
        nodeCount = elem.GetNodeCount()
        If Err.Number = 0 Then
            WScript.Echo "NodeCount: " & nodeCount
        End If
    End If

    WScript.Echo ""
End Sub

WScript.Echo "=== ACTUAL Panel 2 Structure ==="
WScript.Echo ""

' The left panel of the inner splitter contains another splitter
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell", "Panel 2 → Inner Left → Splitter"

' That splitter's children
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell/shellcont[0]", "Splitter → Left (Tree)"
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell/shellcont[1]", "Splitter → Right (Grid)"

' Find the tree
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell/shellcont[0]/shell", "Tree location"

' Find the grid (from deep path)
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell/shellcont[0]/shellcont/shellcont/shell/shellcont[1]/shell", "GridView location"
