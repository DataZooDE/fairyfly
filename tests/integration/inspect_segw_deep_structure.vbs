' Deep inspection of SEGW screen structure (ContainerShell and nested elements)
Option Explicit

Dim SapGuiAuto, Connection, Session
Set SapGuiAuto = GetObject("SAPGUI")
Set Connection = SapGuiAuto.GetScriptingEngine.Children(0)
Set Session = Connection.Children(0)

Sub TraverseElement(elem, indent)
    On Error Resume Next
    Dim info
    info = Space(indent) & elem.Type & " - " & elem.Id

    ' Add SubType if available
    Dim subtype
    subtype = elem.SubType
    If Err.Number = 0 And Len(subtype) > 0 Then
        info = info & " (SubType: " & subtype & ")"
    End If
    Err.Clear

    ' Add row/column count if it's a grid
    Dim rows, cols
    rows = elem.RowCount
    cols = elem.ColumnCount
    If Err.Number = 0 Then
        info = info & " [" & rows & "×" & cols & "]"
    End If
    Err.Clear

    WScript.Echo info

    ' Recursively traverse children
    Dim childCount
    childCount = elem.Children.Count
    If Err.Number = 0 And childCount > 0 Then
        Dim i, child
        For i = 0 To childCount - 1
            Set child = elem.Children.ElementAt(i)
            If Err.Number = 0 And Not child Is Nothing Then
                TraverseElement child, indent + 2
            End If
            Err.Clear
        Next
    End If
    On Error GoTo 0
End Sub

' Start from window
Dim Window
Set Window = Session.FindById("wnd[0]")
WScript.Echo "=== SEGW Screen Structure (Full Traversal) ==="
WScript.Echo ""
TraverseElement Window, 0

WScript.Echo ""
WScript.Echo "=== Done ==="
