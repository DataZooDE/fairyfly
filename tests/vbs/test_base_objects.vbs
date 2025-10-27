' VBScript Ground Truth Test for SAP GUI Base Objects
' This script extracts all base object properties (Id, Name, Type, TypeAsNumber)
' for comparison with C++ SapGuiObject implementation

Option Explicit

' JSON helper functions
Function EscapeJSON(str)
    Dim result
    result = str
    result = Replace(result, "\", "\\")
    result = Replace(result, """", "\""")
    result = Replace(result, vbCrLf, "\n")
    result = Replace(result, vbCr, "\n")
    result = Replace(result, vbLf, "\n")
    result = Replace(result, vbTab, "\t")
    EscapeJSON = result
End Function

Function ObjectToJSON(obj, objType)
    Dim json
    json = "{"
    json = json & """object_type"": """ & objType & ""","

    ' Common base properties that all SAP GUI objects should have
    On Error Resume Next

    ' Id property
    Dim objId
    objId = obj.Id
    If Err.Number = 0 Then
        json = json & """id"": """ & EscapeJSON(objId) & ""","
    Else
        json = json & """id"": null,"
    End If
    Err.Clear

    ' Name property
    Dim objName
    objName = obj.Name
    If Err.Number = 0 Then
        json = json & """name"": """ & EscapeJSON(objName) & ""","
    Else
        json = json & """name"": null,"
    End If
    Err.Clear

    ' Type property
    Dim objTypeStr
    objTypeStr = obj.Type
    If Err.Number = 0 Then
        json = json & """type"": """ & EscapeJSON(objTypeStr) & ""","
    Else
        json = json & """type"": null,"
    End If
    Err.Clear

    ' TypeAsNumber property
    Dim objTypeNum
    objTypeNum = obj.TypeAsNumber
    If Err.Number = 0 Then
        json = json & """type_as_number"": " & objTypeNum & ","
    Else
        json = json & """type_as_number"": null,"
    End If
    Err.Clear

    On Error GoTo 0

    ' Remove trailing comma
    If Right(json, 1) = "," Then
        json = Left(json, Len(json) - 1)
    End If

    json = json & "}"
    ObjectToJSON = json
End Function

' Main script
Dim rotEntry, app, conn, sess, wnd
Dim i, j, k
Dim json, jsonObjects

' Initialize JSON array
jsonObjects = "["

' Get SAP GUI application
On Error Resume Next
Set rotEntry = GetObject("SAPGUI")
If Err.Number <> 0 Then
    WScript.Echo "ERROR: Cannot get SAPGUI object. Error: " & Err.Description
    WScript.Quit 1
End If
Err.Clear

Set app = rotEntry.GetScriptingEngine
If Err.Number <> 0 Then
    WScript.Echo "ERROR: Cannot get scripting engine. Error: " & Err.Description
    WScript.Quit 1
End If
Err.Clear
On Error GoTo 0

' Add GuiApplication object
jsonObjects = jsonObjects & ObjectToJSON(app, "GuiApplication") & ","

' Iterate through connections
If app.Connections.Count > 0 Then
    For i = 0 To app.Connections.Count - 1
        Set conn = app.Connections(CLng(i))

        ' Add GuiConnection object
        jsonObjects = jsonObjects & ObjectToJSON(conn, "GuiConnection") & ","

        ' Iterate through sessions
        If conn.Sessions.Count > 0 Then
            For j = 0 To conn.Sessions.Count - 1
                Set sess = conn.Sessions(CLng(j))

                ' Add GuiSession object
                jsonObjects = jsonObjects & ObjectToJSON(sess, "GuiSession") & ","

                ' Get main window if available
                On Error Resume Next
                Set wnd = sess.FindById("wnd[0]")
                If Err.Number = 0 And Not wnd Is Nothing Then
                    jsonObjects = jsonObjects & ObjectToJSON(wnd, "GuiFrameWindow") & ","

                    ' Get a few child elements if available
                    Dim children
                    On Error Resume Next
                    Set children = wnd.Children
                    If Err.Number = 0 And Not children Is Nothing Then
                        If children.Count > 0 Then
                            Dim maxChildren
                            maxChildren = children.Count
                            If maxChildren > 5 Then maxChildren = 5 ' Limit to first 5 children

                            For k = 0 To maxChildren - 1
                                Dim child
                                Set child = children(CLng(k))
                                If Err.Number = 0 And Not child Is Nothing Then
                                    Dim childType
                                    childType = child.Type
                                    If Err.Number = 0 Then
                                        jsonObjects = jsonObjects & ObjectToJSON(child, childType) & ","
                                    End If
                                End If
                                Err.Clear
                            Next
                        End If
                    End If
                End If
                Err.Clear
                On Error GoTo 0
            Next
        End If
    Next
End If

' Remove trailing comma
If Right(jsonObjects, 1) = "," Then
    jsonObjects = Left(jsonObjects, Len(jsonObjects) - 1)
End If

jsonObjects = jsonObjects & "]"

' Output JSON
json = "{"
json = json & """status"": ""success"","
json = json & """test_name"": ""test_base_objects"","
json = json & """description"": ""VBScript ground truth for SAP GUI base object properties"","
json = json & """objects"": " & jsonObjects
json = json & "}"

WScript.Echo json
