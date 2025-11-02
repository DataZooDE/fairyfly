' Get column information from SM59 tree
On Error Resume Next

Set SapGuiAuto = GetObject("SAPGUI")
Set application = SapGuiAuto.GetScriptingEngine
Set connection = application.Children(0)
Set session = connection.Children(0)

Set shell = session.FindById("wnd[0]/usr/cntlSM59CNTL_AREA/shellcont/shell/shellcont[1]/shell[1]")

If IsObject(shell) Then
    WScript.Echo "=== Column Detection ==="

    ' Try to get the GuiShell's internal table/tree
    ' Check for ColumnCollection property
    On Error Resume Next
    Set colColl = shell.ColumnCollection
    If Err.Number = 0 And IsObject(colColl) Then
        WScript.Echo "ColumnCollection Count: " & colColl.Count
        For i = 0 To colColl.Count - 1
            Set col = colColl(i)
            WScript.Echo "  Column " & i & ": " & col.Title
        Next
    Else
        WScript.Echo "ColumnCollection: Not available"
    End If
    Err.Clear

    ' Try ColumnOrder
    Set colOrder = shell.ColumnOrder
    If Err.Number = 0 And IsObject(colOrder) Then
        WScript.Echo "ColumnOrder Count: " & colOrder.Count
        For i = 0 To colOrder.Count - 1
            WScript.Echo "  Column " & i & ": " & colOrder(i)
        Next
    Else
        WScript.Echo "ColumnOrder: Not available"
    End If
    Err.Clear

    ' Get a sample node and check its item texts
    Set allKeys = shell.GetAllNodeKeys()
    If allKeys.Count > 1 Then
        ' Get second node (A4H) which should have column data
        nodeKey = allKeys(1)
        nodeText = shell.GetNodeTextByKey(nodeKey)

        WScript.Echo ""
        WScript.Echo "=== Sample Node: " & nodeText & " ==="
        WScript.Echo "Key: " & nodeKey

        ' Try GetItemText with different column indices
        For col = 0 To 5
            itemText = shell.GetItemText(nodeKey, col)
            If Err.Number = 0 And itemText <> "" Then
                WScript.Echo "  Column " & col & ": " & itemText
            End If
            Err.Clear
        Next
    End If
End If
