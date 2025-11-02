Option Explicit

' Test script to check actual element states in SAP GUI
Dim SapGuiAuto, Connection, Session, Element

Set SapGuiAuto = GetObject("SAPGUI")
Set Connection = SapGuiAuto.GetScriptingEngine.Children(0)
Set Session = Connection.Children(0)

' Check a few specific elements
Dim TestElements
TestElements = Array( _
    "/app/con[0]/ses[0]/wnd[0]/usr/cmbRSDGSCSEL-OBJVERS", _
    "/app/con[0]/ses[0]/wnd[0]/usr/radRSDGSCSEL-CHA", _
    "/app/con[0]/ses[0]/wnd[0]/usr/ctxtRSDGSCSEL-IOBJNM", _
    "/app/con[0]/ses[0]/wnd[0]/usr/btnPUSH_DISPLAY_GROUP" _
)

Dim i, id
For i = 0 To UBound(TestElements)
    id = TestElements(i)
    On Error Resume Next
    Set Element = Session.FindById(id)
    If Err.Number = 0 And Not Element Is Nothing Then
        WScript.Echo "Element: " & id
        WScript.Echo "  Type: " & Element.Type

        ' Try Enabled property
        Err.Clear
        Dim enabled
        enabled = Element.Enabled
        If Err.Number = 0 Then
            WScript.Echo "  Enabled: " & enabled
        Else
            WScript.Echo "  Enabled: ERROR - " & Err.Description
        End If

        ' Try Changeable property
        Err.Clear
        Dim changeable
        changeable = Element.Changeable
        If Err.Number = 0 Then
            WScript.Echo "  Changeable: " & changeable
        Else
            WScript.Echo "  Changeable: ERROR - " & Err.Description
        End If

        WScript.Echo "  Text: " & Element.Text
        WScript.Echo ""
    Else
        WScript.Echo "ERROR accessing " & id & ": " & Err.Description
        Err.Clear
    End If
    On Error GoTo 0
Next

Set Element = Nothing
Set Session = Nothing
Set Connection = Nothing
Set SapGuiAuto = Nothing
