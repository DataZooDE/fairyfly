' Map the structure of SEGW Panel 2 to understand where the GridView is
Option Explicit

Dim SapGuiAuto, Connection, Session
Set SapGuiAuto = GetObject("SAPGUI")
Set Connection = SapGuiAuto.GetScriptingEngine.Children(0)
Set Session = Connection.Children(0)

' Start from the outer splitter
Dim OuterSplitter
Set OuterSplitter = Session.FindById("wnd[0]/usr/shellcont/shell")
WScript.Echo "=== Outer Splitter ==="
WScript.Echo "Type: " & OuterSplitter.Type
WScript.Echo "Children: " & OuterSplitter.Children.Count
WScript.Echo ""

' Panel 2 is shellcont[1]
Dim Panel2Container
Set Panel2Container = Session.FindById("wnd[0]/usr/shellcont/shell/shellcont[1]")
WScript.Echo "=== Panel 2 Container ==="
WScript.Echo "Type: " & Panel2Container.Type
WScript.Echo "ID: " & Panel2Container.Id
WScript.Echo ""

' Look for inner splitter
On Error Resume Next
Dim InnerSplitter
Set InnerSplitter = Session.FindById("wnd[0]/usr/shellcont/shell/shellcont[1]/shell")
If Err.Number = 0 And Not InnerSplitter Is Nothing Then
    WScript.Echo "=== Inner Splitter (Panel 2) ==="
    WScript.Echo "Type: " & InnerSplitter.Type
    WScript.Echo "Children: " & InnerSplitter.Children.Count
    WScript.Echo ""

    ' Try to find both children (left tree, right grid)
    Dim LeftPanel, RightPanel
    Set LeftPanel = Session.FindById("wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]")
    Set RightPanel = Session.FindById("wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[1]")

    If Not LeftPanel Is Nothing Then
        WScript.Echo "=== Left Panel ==="
        WScript.Echo "Type: " & LeftPanel.Type
        WScript.Echo "ID: " & LeftPanel.Id

        ' Try to find the tree
        Dim Tree
        Set Tree = Session.FindById("wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell")
        If Not Tree Is Nothing Then
            WScript.Echo "  Tree Type: " & Tree.Type
            WScript.Echo "  Tree SubType: " & Tree.SubType
        End If
        WScript.Echo ""
    End If

    If Not RightPanel Is Nothing Then
        WScript.Echo "=== Right Panel ==="
        WScript.Echo "Type: " & RightPanel.Type
        WScript.Echo "ID: " & RightPanel.Id

        ' Try to find the grid - it might be deeply nested
        Dim Grid
        Set Grid = Session.FindById("wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[1]/shell")
        If Err.Number <> 0 Or Grid Is Nothing Then
            Err.Clear
            ' Try deeper path
            Set Grid = Session.FindById("wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell/shellcont[0]/shellcont/shellcont/shell/shellcont[1]/shell")
        End If

        If Not Grid Is Nothing Then
            WScript.Echo "  Grid Type: " & Grid.Type
            WScript.Echo "  Grid SubType: " & Grid.SubType
            WScript.Echo "  Grid ID: " & Grid.Id
            WScript.Echo "  Grid RowCount: " & Grid.RowCount
            WScript.Echo "  Grid ColumnCount: " & Grid.ColumnCount
        Else
            WScript.Echo "  Grid not found at expected location"
        End If
    End If
End If
