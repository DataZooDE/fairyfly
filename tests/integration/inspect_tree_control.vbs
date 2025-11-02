' VBScript to inspect SAP TableTreeControl in SM59
' This script will help us understand what properties/methods are available

If Not IsObject(WScript) Then
    WScript.Echo "This script must be run with cscript.exe or wscript.exe"
    WScript.Quit 1
End If

On Error Resume Next

' Connect to SAP GUI
Set SapGuiAuto = GetObject("SAPGUI")
If IsObject(SapGuiAuto) Then
    Set application = SapGuiAuto.GetScriptingEngine
    If Not IsObject(application) Then
        WScript.Echo "ERROR: Scripting is disabled. Please enable it in SAP GUI options."
        WScript.Quit 1
    End If
Else
    WScript.Echo "ERROR: Could not connect to SAP GUI"
    WScript.Quit 1
End If

' Get first connection and session
Set connection = application.Children(0)
Set session = connection.Children(0)

WScript.Echo "Connected to SAP session: " & session.Info.SystemName

' Navigate to SM59 (assuming we're already there)
WScript.Echo ""
WScript.Echo "Looking for TableTreeControl in SM59..."
WScript.Echo ""

' Find the GuiShell containing the table tree
Set shell = session.FindById("wnd[0]/usr/cntlSM59CNTL_AREA/shellcont/shell/shellcont[1]/shell[1]")

If IsObject(shell) Then
    WScript.Echo "Found GuiShell: " & shell.Id
    WScript.Echo "Type: " & shell.Type
    WScript.Echo "SubType: " & shell.SubType
    WScript.Echo "Text (ProgID): " & shell.Text
    WScript.Echo ""

    ' Check if it has children (SAP GUI children)
    WScript.Echo "Children count (SAP GUI): " & shell.Children.Count
    WScript.Echo ""

    ' Try to access the ActiveX control directly
    WScript.Echo "Attempting to access ActiveX control properties..."
    WScript.Echo ""

    ' Common TableTreeControl properties to try
    Dim props
    props = Array("ColumnCount", "RowCount", "TopNode", "SelectedNode", "NodeCount")

    For Each prop In props
        On Error Resume Next
        value = Eval("shell." & prop)
        If Err.Number = 0 Then
            WScript.Echo "  " & prop & " = " & value
        Else
            WScript.Echo "  " & prop & " = <not available>"
        End If
        Err.Clear
    Next

    WScript.Echo ""
    WScript.Echo "Attempting to enumerate columns..."
    On Error Resume Next
    colCount = shell.ColumnCount
    If Err.Number = 0 And colCount > 0 Then
        For i = 0 To colCount - 1
            colName = shell.GetColumnName(i)
            If Err.Number = 0 Then
                WScript.Echo "  Column " & i & ": " & colName
            End If
        Next
    Else
        WScript.Echo "  Could not get column count or no columns"
    End If

    WScript.Echo ""
    WScript.Echo "Attempting to get all nodes..."
    On Error Resume Next
    Set allNodes = shell.GetAllNodeKeys()
    If Err.Number = 0 And IsObject(allNodes) Then
        WScript.Echo "  Total nodes: " & allNodes.Count
        If allNodes.Count > 0 And allNodes.Count < 100 Then
            For i = 0 To allNodes.Count - 1
                nodeKey = allNodes(i)
                nodeText = shell.GetNodeTextByKey(nodeKey)
                WScript.Echo "    Node " & i & ": key=" & nodeKey & ", text=" & nodeText
            Next
        End If
    Else
        WScript.Echo "  GetAllNodeKeys() failed or not available"
        Err.Clear
    End If

Else
    WScript.Echo "ERROR: Could not find TableTreeControl shell"
    WScript.Echo "Make sure you are on SM59 transaction"
End If

WScript.Echo ""
WScript.Echo "Inspection complete."
