' Map specific paths in SEGW to understand structure
Option Explicit

Dim SapGuiAuto, Connection, Session
Set SapGuiAuto = GetObject("SAPGUI")
Set Connection = SapGuiAuto.GetScriptingEngine.Children(0)
Set Session = Connection.Children(0)

Sub InspectElement(path, description)
    On Error Resume Next
    Dim elem
    WScript.Echo "=== " & description & " ==="
    WScript.Echo "Path: " & path

    Set elem = Session.FindById(path)
    If Err.Number <> 0 Or elem Is Nothing Then
        WScript.Echo "ERROR: Element not found"
        Err.Clear
        WScript.Echo ""
        Exit Sub
    End If

    WScript.Echo "Type: " & elem.Type

    Dim subtype
    Err.Clear
    subtype = elem.SubType
    If Err.Number = 0 And subtype <> "" Then
        WScript.Echo "SubType: " & subtype
    End If
    Err.Clear

    Dim children
    Err.Clear
    Set children = elem.Children
    If Err.Number = 0 And Not children Is Nothing Then
        WScript.Echo "Children.Count: " & children.Count

        ' List immediate children
        Dim i, child
        For i = 0 To children.Count - 1
            Err.Clear
            Set child = children(i)
            If Err.Number = 0 And Not child Is Nothing Then
                WScript.Echo "  Child " & i & ": Type=" & child.Type & ", ID=" & child.Id

                ' Check child's subtype
                Dim childSubtype
                Err.Clear
                childSubtype = child.SubType
                If Err.Number = 0 And childSubtype <> "" Then
                    WScript.Echo "           SubType=" & childSubtype
                End If
                Err.Clear
            End If
        Next
    End If

    WScript.Echo ""
End Sub

' Map the known hierarchy
InspectElement "wnd[0]/usr/shellcont/shell", "Outer Splitter"
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[0]", "Panel 1 (left)"
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]", "Panel 2 (right)"

' Panel 2's first child
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell", "Panel 2 → Inner Splitter"

' Inner splitter's children
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]", "Inner Splitter → Left Panel"
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[1]", "Inner Splitter → Right Panel"

' Try to find tree and grid
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell", "Left Panel → shell"
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[1]/shell", "Right Panel → shell"

' Deep path from previous VBScript
InspectElement "wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell/shellcont[0]/shellcont/shellcont/shell/shellcont[1]/shell", "Deep GridView path"
