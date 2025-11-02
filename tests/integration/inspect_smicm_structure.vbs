Option Explicit

' Deep inspection of SMICM screen structure
Dim SapGuiAuto, Connection, Session, Window, UserArea

Set SapGuiAuto = GetObject("SAPGUI")
Set Connection = SapGuiAuto.GetScriptingEngine.Children(0)
Set Session = Connection.Children(0)
Set Window = Session.ActiveWindow

WScript.Echo "=== SMICM Screen Structure Inspection ==="
WScript.Echo "Window ID: " & Window.Id
WScript.Echo "Window Type: " & Window.Type
WScript.Echo "Window Text: " & Window.Text
WScript.Echo ""

' Get UserArea
On Error Resume Next
Set UserArea = Window.FindById("wnd[0]/usr")
If Err.Number = 0 And Not UserArea Is Nothing Then
    WScript.Echo "=== UserArea Found ==="
    WScript.Echo "Type: " & UserArea.Type
    WScript.Echo "Children count: " & UserArea.Children.Count
    WScript.Echo ""

    ' Sample first 10 children
    WScript.Echo "=== First 10 Children ==="
    Dim i, child
    For i = 0 To 9
        If i >= UserArea.Children.Count Then Exit For

        Err.Clear
        Set child = UserArea.Children(i)
        If Err.Number = 0 And Not child Is Nothing Then
            WScript.Echo "Child " & i & ":"
            WScript.Echo "  ID: " & child.Id
            WScript.Echo "  Type: " & child.Type
            WScript.Echo "  Name: " & child.Name

            Dim text
            Err.Clear
            text = child.Text
            If Err.Number = 0 Then
                WScript.Echo "  Text: " & text
            End If

            ' Try to get ScreenLeft/ScreenTop for position
            Dim screenLeft, screenTop, width, height
            Err.Clear
            screenLeft = child.ScreenLeft
            screenTop = child.ScreenTop
            width = child.Width
            height = child.Height
            If Err.Number = 0 Then
                WScript.Echo "  Position: (" & screenLeft & "," & screenTop & ") Size: " & width & "x" & height
            End If

            WScript.Echo ""
        End If
    Next

    ' Check if there's a GuiShell or GuiTableControl
    WScript.Echo "=== Searching for Shells/Tables ==="
    For i = 0 To UserArea.Children.Count - 1
        Err.Clear
        Set child = UserArea.Children(i)
        If Err.Number = 0 And Not child Is Nothing Then
            Dim childType
            childType = child.Type
            If InStr(childType, "Shell") > 0 Or InStr(childType, "Table") > 0 Or InStr(childType, "Grid") > 0 Then
                WScript.Echo "Found: " & childType & " at " & child.Id

                ' Check for SubType
                Err.Clear
                Dim subtype
                subtype = child.SubType
                If Err.Number = 0 Then
                    WScript.Echo "  SubType: " & subtype
                End If

                ' Check children count
                Err.Clear
                Dim childCount
                childCount = child.Children.Count
                If Err.Number = 0 Then
                    WScript.Echo "  Children: " & childCount
                End If
            End If
        End If
    Next

    ' Try to access specific label coordinates
    WScript.Echo ""
    WScript.Echo "=== Testing Specific Label Access ==="
    Dim testLabels
    testLabels = Array("lbl[0,2]", "lbl[1,2]", "lbl[2,2]", "lbl[0,3]", "lbl[1,3]")

    Dim labelId, label
    For i = 0 To UBound(testLabels)
        labelId = "wnd[0]/usr/" & testLabels(i)
        Err.Clear
        Set label = Session.FindById(labelId)
        If Err.Number = 0 And Not label Is Nothing Then
            WScript.Echo labelId & ": '" & label.Text & "'"
        Else
            WScript.Echo labelId & ": NOT FOUND"
        End If
    Next

Else
    WScript.Echo "ERROR: Could not access UserArea"
    WScript.Echo "Error: " & Err.Description
End If

Set UserArea = Nothing
Set Window = Nothing
Set Session = Nothing
Set Connection = Nothing
Set SapGuiAuto = Nothing
