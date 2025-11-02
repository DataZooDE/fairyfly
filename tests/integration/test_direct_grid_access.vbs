' Test direct access to the Entity Sets grid discovered earlier
Option Explicit

Dim SapGuiAuto, Connection, Session
Set SapGuiAuto = GetObject("SAPGUI")
Set Connection = SapGuiAuto.GetScriptingEngine.Children(0)
Set Session = Connection.Children(0)

' Try to access the grid directly using the ID we found
On Error Resume Next
Dim Grid
Set Grid = Session.FindById("wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell/shellcont[0]/shellcont/shellcont/shell/shellcont[1]/shell")

If Err.Number <> 0 Then
    WScript.Echo "ERROR: Could not find grid element"
    WScript.Echo "Error: " & Err.Description
    WScript.Quit 1
End If

If Grid Is Nothing Then
    WScript.Echo "ERROR: Grid is Nothing"
    WScript.Quit 1
End If

WScript.Echo "SUCCESS: Found grid element"
WScript.Echo "Type: " & Grid.Type
WScript.Echo "SubType: " & Grid.SubType
WScript.Echo "ID: " & Grid.Id
WScript.Echo "RowCount: " & Grid.RowCount
WScript.Echo "ColumnCount: " & Grid.ColumnCount
WScript.Echo ""

' Try to get column order
WScript.Echo "Getting column information..."
Dim ColumnOrder
Set ColumnOrder = Grid.ColumnOrder
WScript.Echo "Columns: " & ColumnOrder.Count

Dim i
For i = 0 To ColumnOrder.Count - 1
    WScript.Echo "  Column " & i & ": " & ColumnOrder.ElementAt(i)
Next

WScript.Echo ""
WScript.Echo "Getting first few cell values..."

' Try to read some cell values
For i = 0 To 2
    Dim rowText
    rowText = "Row " & i & ": "
    Dim j
    For j = 0 To 3
        On Error Resume Next
        Dim cellValue
        Dim colName
        colName = ColumnOrder.ElementAt(j)
        cellValue = Grid.GetCellValue(i, colName)
        If Err.Number = 0 Then
            rowText = rowText & cellValue & " | "
        Else
            rowText = rowText & "[error] | "
        End If
        Err.Clear
    Next
    WScript.Echo rowText
Next
