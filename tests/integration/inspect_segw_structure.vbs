' Inspect SEGW screen structure to understand splitter layout
Option Explicit

Dim SapGuiAuto, Connection, Session
Set SapGuiAuto = GetObject("SAPGUI")
Set Connection = SapGuiAuto.GetScriptingEngine.Children(0)
Set Session = Connection.Children(0)

Dim Window
Set Window = Session.FindById("wnd[0]")

WScript.Echo "Window: " & Window.Id & " - " & Window.Text
WScript.Echo "Children: " & Window.Children.Count
WScript.Echo ""

' Enumerate all direct children
On Error Resume Next
Dim i, Child
For i = 0 To Window.Children.Count - 1
    Set Child = Window.Children.ElementAt(i)
    If Err.Number = 0 And Not Child Is Nothing Then
        WScript.Echo "Child " & i & ": " & Child.Type & " - " & Child.Id
        If Child.Type = "GuiUserArea" Then
            WScript.Echo "  UserArea has " & Child.Children.Count & " children"

            ' Look at children of UserArea
            Dim j, SubChild
            For j = 0 To Child.Children.Count - 1
                Set SubChild = Child.Children.ElementAt(j)
                If Err.Number = 0 And Not SubChild Is Nothing Then
                    WScript.Echo "    SubChild " & j & ": " & SubChild.Type & " - " & SubChild.Id

                    ' If it's a splitter, show its children
                    If InStr(SubChild.Type, "Splitter") > 0 Then
                        Dim k, SplitChild
                        WScript.Echo "      Splitter has " & SubChild.Children.Count & " children"
                        For k = 0 To SubChild.Children.Count - 1
                            Set SplitChild = SubChild.Children.ElementAt(k)
                            If Err.Number = 0 And Not SplitChild Is Nothing Then
                                WScript.Echo "        SplitChild " & k & ": " & SplitChild.Type & " - " & SplitChild.Id

                                ' If it's a GuiShell, check SubType
                                If SplitChild.Type = "GuiShell" Then
                                    WScript.Echo "          SubType: " & SplitChild.SubType
                                    Dim RowCount, ColCount
                                    RowCount = SplitChild.RowCount
                                    ColCount = SplitChild.ColumnCount
                                    If Err.Number = 0 Then
                                        WScript.Echo "          Grid: " & RowCount & " rows, " & ColCount & " columns"
                                    End If
                                    Err.Clear
                                End If
                            End If
                        Next
                    End If
                End If
            Next
        End If
    End If
    Err.Clear
Next
On Error GoTo 0

WScript.Echo ""
WScript.Echo "=== Looking for specific elements ==="

' Try to find the right panel with Entity Sets table
On Error Resume Next
Dim Shell1, Shell2
Set Shell1 = Session.FindById("wnd[0]/usr/ssubSUB_MAIN:SAPLSED2_MODEL_2:0201/subSUB_CONT_1:SAPLSED2_MODEL_2:0202/cntlALV_CONTAINER_1/shellcont/shell")
If Not Shell1 Is Nothing Then
    WScript.Echo "Found right panel shell: " & Shell1.Id
    WScript.Echo "  Type: " & Shell1.Type
    WScript.Echo "  SubType: " & Shell1.SubType
    WScript.Echo "  RowCount: " & Shell1.RowCount
    WScript.Echo "  ColumnCount: " & Shell1.ColumnCount
End If

Set Shell2 = Session.FindById("wnd[0]/usr/ssubSUB_MAIN:SAPLSED2_MODEL_2:0201/subSUB_CONT_2:SAPLSED2_MODEL_2:0203/cntlALV_CONTAINER_2/shellcont/shell")
If Not Shell2 Is Nothing Then
    WScript.Echo "Found tree shell: " & Shell2.Id
    WScript.Echo "  Type: " & Shell2.Type
    WScript.Echo "  SubType: " & Shell2.SubType
End If
On Error GoTo 0

WScript.Echo ""
WScript.Echo "Done"
