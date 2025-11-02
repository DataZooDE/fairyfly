' VBScript to inspect tree structure and columns in SM59
On Error Resume Next

Set SapGuiAuto = GetObject("SAPGUI")
Set application = SapGuiAuto.GetScriptingEngine
Set connection = application.Children(0)
Set session = connection.Children(0)

WScript.Echo "=== SAP TableTreeControl Structure Analysis ==="
WScript.Echo ""

' Get the tree control
Set shell = session.FindById("wnd[0]/usr/cntlSM59CNTL_AREA/shellcont/shell/shellcont[1]/shell[1]")

If IsObject(shell) Then
    WScript.Echo "Tree Control: " & shell.Id
    WScript.Echo "Type: " & shell.Type & " / SubType: " & shell.SubType
    WScript.Echo ""

    ' Get column information
    WScript.Echo "=== COLUMNS ==="
    colCount = shell.GetColumnCol
    WScript.Echo "Column Count (GetColumnCol): " & colCount

    ' Try different methods to get column names
    On Error Resume Next
    For i = 0 To 10
        colName = shell.GetColumnTitleFromPos(i)
        If Err.Number = 0 And colName <> "" Then
            WScript.Echo "  Column " & i & ": " & colName
        End If
        Err.Clear
    Next

    WScript.Echo ""
    WScript.Echo "=== NODE HIERARCHY ==="

    ' Get all node keys
    Set allKeys = shell.GetAllNodeKeys()
    WScript.Echo "Total nodes: " & allKeys.Count
    WScript.Echo ""

    ' Analyze first few nodes for hierarchy
    For i = 0 To 9
        If i < allKeys.Count Then
            nodeKey = allKeys(i)
            nodeText = shell.GetNodeTextByKey(nodeKey)

            ' Try to get parent
            On Error Resume Next
            parentKey = shell.GetNodePathByKey(nodeKey)
            If Err.Number <> 0 Then
                parentKey = "<error>"
            End If
            Err.Clear

            ' Check if it's a top node
            isTopNode = shell.IsTopNode(nodeKey)
            If Err.Number <> 0 Then
                isTopNode = "<unknown>"
            End If
            Err.Clear

            ' Get children
            Set children = shell.GetSubNodesCol(nodeKey)
            If Err.Number = 0 And IsObject(children) Then
                childCount = children.Count
            Else
                childCount = "<unknown>"
            End If
            Err.Clear

            WScript.Echo "Node " & i & ":"
            WScript.Echo "  Key: " & nodeKey
            WScript.Echo "  Text: " & nodeText
            WScript.Echo "  IsTopNode: " & isTopNode
            WScript.Echo "  Children: " & childCount
            WScript.Echo "  Path: " & parentKey

            ' Try to get column values for this node
            WScript.Echo "  Columns:"
            For col = 0 To 2
                cellValue = shell.GetNodeItemByKey(nodeKey, col)
                If Err.Number = 0 Then
                    WScript.Echo "    Col " & col & ": " & cellValue
                End If
                Err.Clear
            Next

            WScript.Echo ""
        End If
    Next

Else
    WScript.Echo "ERROR: Could not find tree control"
End If

WScript.Echo "Analysis complete."
