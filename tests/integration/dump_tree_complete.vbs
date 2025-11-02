' Complete tree data dump with hierarchy and columns
On Error Resume Next

Set SapGuiAuto = GetObject("SAPGUI")
Set application = SapGuiAuto.GetScriptingEngine
Set connection = application.Children(0)
Set session = connection.Children(0)

Set shell = session.FindById("wnd[0]/usr/cntlSM59CNTL_AREA/shellcont/shell/shellcont[1]/shell[1]")

If IsObject(shell) Then
    ' Get all nodes
    Set allKeys = shell.GetAllNodeKeys()

    ' Build hierarchy map
    Set dict = CreateObject("Scripting.Dictionary")

    For i = 0 To allKeys.Count - 1
        nodeKey = allKeys(i)
        nodePath = shell.GetNodePathByKey(nodeKey)
        nodeText = shell.GetNodeTextByKey(nodeKey)

        ' Parse path to determine level
        pathParts = Split(nodePath, "\")
        level = UBound(pathParts)

        ' Get column values using GetItemText
        col1 = shell.GetItemText(nodeKey, "C          1")
        If Err.Number <> 0 Then col1 = ""
        Err.Clear

        col3 = shell.GetItemText(nodeKey, "C          3")
        If Err.Number <> 0 Then col3 = ""
        Err.Clear

        col4 = shell.GetItemText(nodeKey, "C          4")
        If Err.Number <> 0 Then col4 = ""
        Err.Clear

        WScript.Echo nodeKey & "|" & level & "|" & nodePath & "|" & nodeText & "|" & col1 & "|" & col3 & "|" & col4
    Next
End If
