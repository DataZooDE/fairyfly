' Recursively map the entire SEGW Panel 2 structure
Option Explicit

Dim SapGuiAuto, Connection, Session
Set SapGuiAuto = GetObject("SAPGUI")
Set Connection = SapGuiAuto.GetScriptingEngine.Children(0)
Set Session = Connection.Children(0)

' Recursive function to print element tree
Sub PrintElement(element, indent)
    On Error Resume Next
    Dim prefix, i, child, childPath
    prefix = String(indent * 2, " ")

    WScript.Echo prefix & "├── Type: " & element.Type
    If element.Type <> "" Then
        WScript.Echo prefix & "│   ID: " & element.Id

        ' Try to get SubType if it exists
        Dim subtype
        subtype = ""
        Err.Clear
        subtype = element.SubType
        If Err.Number = 0 And subtype <> "" Then
            WScript.Echo prefix & "│   SubType: " & subtype
        End If
        Err.Clear

        ' Try to get ContainerType
        Dim containerType
        containerType = ""
        Err.Clear
        containerType = element.ContainerType
        If Err.Number = 0 And containerType <> "" Then
            WScript.Echo prefix & "│   ContainerType: " & containerType
        End If
        Err.Clear

        ' For GuiShell with GridView, show row/col count
        If element.Type = "GuiShell" And subtype = "GridView" Then
            Dim rowCount, colCount
            Err.Clear
            rowCount = element.RowCount
            colCount = element.ColumnCount
            If Err.Number = 0 Then
                WScript.Echo prefix & "│   [GRIDVIEW] RowCount: " & rowCount & ", ColumnCount: " & colCount
            End If
            Err.Clear
        End If

        ' For GuiTree, show node count
        If element.Type = "GuiTree" Then
            Dim nodeCount
            Err.Clear
            nodeCount = element.GetNodeCount()
            If Err.Number = 0 Then
                WScript.Echo prefix & "│   [TREE] NodeCount: " & nodeCount
            End If
            Err.Clear
        End If

        ' Check for children
        Dim children
        Err.Clear
        Set children = element.Children
        If Err.Number = 0 And Not children Is Nothing Then
            If children.Count > 0 Then
                WScript.Echo prefix & "│   Children: " & children.Count
                WScript.Echo prefix & "│"
                For i = 0 To children.Count - 1
                    Err.Clear
                    Set child = children(i)
                    If Err.Number = 0 And Not child Is Nothing Then
                        PrintElement child, indent + 1
                    End If
                Next
            Else
                WScript.Echo prefix & "│   Children: 0"
            End If
        Else
            WScript.Echo prefix & "│   No Children property"
        End If
        Err.Clear
    End If
End Sub

WScript.Echo "=== SEGW Panel 2 Recursive Structure ==="
WScript.Echo ""

' Start from outer splitter
Dim OuterSplitter
Set OuterSplitter = Session.FindById("wnd[0]/usr/shellcont/shell")
WScript.Echo "Outer Splitter: " & OuterSplitter.Type
WScript.Echo ""

' Panel 2 (shellcont[1])
Dim Panel2
Set Panel2 = Session.FindById("wnd[0]/usr/shellcont/shell/shellcont[1]")
WScript.Echo "=== Panel 2 Container ==="
WScript.Echo "Type: " & Panel2.Type
WScript.Echo "ID: " & Panel2.Id
WScript.Echo ""

' Recursively print Panel 2 structure
WScript.Echo "=== Panel 2 Recursive Children ==="
PrintElement Panel2, 0
