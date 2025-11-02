Set SapGui = GetObject("SAPGUI")
Set Conn = SapGui.GetScriptingEngine.Children(0)
Set Sess = Conn.Children(0)
Set Elem = Sess.FindById("wnd[0]/usr/shellcont/shell/shellcont[1]/shell/shellcont[0]/shell/shellcont[0]/shellcont/shellcont/shell/shellcont[1]/shell")
WScript.Echo "Type: " & Elem.Type
WScript.Echo "SubType: " & Elem.SubType
WScript.Echo "RowCount: " & Elem.RowCount
WScript.Echo "ColumnCount: " & Elem.ColumnCount
