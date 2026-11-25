# ABAP editor scripting (spike for issue #158, 2026-11-25)

Bigfox (A4H trial, SAP GUI for Windows 8.00), SE38, program ZFFY_FILL_TEST, source code-based editor
`wnd[0]/usr/cntlEDITOR/shellcont/shell` (GuiShell, SubType `AbapEditor`).

What the control offers through SAP GUI Scripting:

- The type information (`IDispatch::GetTypeInfo`) only describes `ISapShell` (SetFocus, FindById, context menu methods).
  The editor's own methods are resolved by name at call time: `IDispatch::Invoke` by name works, a plain
  `GetIDsOfNames` probe only confirmed `GetLineCount` and `GetLineText`.
- Methods that answer (called by name): `GetLineCount`, `GetLineText(line)` (one-based), `SelectAll`, `ReplaceSelection(text)`,
  `GetSelectedText`, `IsModified`, `GetFirstVisibleLine`; `InsertText`, `DeleteRange`, `SelectRange` exist but need
  arguments. Not present: `SetText`, `GetText`, `SetSelectionIndexes`, any read-only property.
- `Text` answers the control's class name (`SAPGUI.AbapEditor.1`) and cannot be written ("Failed to set property").
- `SelectAll` + `ReplaceSelection(text)` replaces the whole source; `\n` and `\r\n` both end a line. `IsModified` then
  answers true; Save (Ctrl+S, VKey 11) and Activate (Ctrl+F3, VKey 27, confirm the inactive-objects list with Enter)
  work as usual.
- Display mode: `Changeable` is still true and `ReplaceSelection` still changes the text on screen; SAP throws that text
  away when the user switches to change mode (the saved source is shown again). The window's Save button
  `tbar[0]/btn[11]` is `Changeable = false` in display mode and true in change mode: fairyfly uses it to refuse the
  fill with `ELEMENT_READ_ONLY`.

Related, same session: choosing a combo box entry (`cmbTRDIR-SUBC` in the program attributes popup) or a radio button
(`SEUCUSTOM-PCMODE` in Utilities > Settings) runs a server round trip; the COM object that was written answers every
call with E_NOTIMPL afterwards, a new `FindById` returns the new object with the chosen value. Reading the result
through the old object is what made fills report an empty key / no `selected`.
