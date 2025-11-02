Option Explicit

' Map the entire SMICM label grid
Dim SapGuiAuto, Connection, Session

Set SapGuiAuto = GetObject("SAPGUI")
Set Connection = SapGuiAuto.GetScriptingEngine.Children(0)
Set Session = Connection.Children(0)

WScript.Echo "=== SMICM Grid Mapping ==="
WScript.Echo ""

' Try to discover grid bounds by probing
Dim maxRow, maxCol, row, col, labelId, label
maxRow = 50  ' Try up to row 50
maxCol = 50  ' Try up to column 50

Dim foundLabels
foundLabels = 0

' Find all non-empty labels
For row = 0 To maxRow
    For col = 0 To maxCol
        labelId = "wnd[0]/usr/lbl[" & row & "," & col & "]"

        On Error Resume Next
        Err.Clear
        Set label = Session.FindById(labelId)

        If Err.Number = 0 And Not label Is Nothing Then
            Dim text
            text = label.Text

            If Len(Trim(text)) > 0 Then
                WScript.Echo "lbl[" & row & "," & col & "]: '" & text & "'"
                foundLabels = foundLabels + 1
            End If
        End If
    Next
Next

WScript.Echo ""
WScript.Echo "Total non-empty labels found: " & foundLabels

Set Session = Nothing
Set Connection = Nothing
Set SapGuiAuto = Nothing
