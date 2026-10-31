#pragma once
// The console's output code page. A Windows console decodes the bytes a program writes with its output code page
// (437 or 850 on most desktops), but fairyfly writes UTF-8: SAP texts such as "Größe" or a non-breaking space came
// out as mojibake ("├╢", "τá") in cmd.exe. fairyfly switches the console to UTF-8 (65001) for its run and puts the
// previous code page back when it ends. Files and pipes get the same UTF-8 bytes, the code page does not apply.

namespace fairyfly::console {

inline constexpr unsigned kUtf8CodePage = 65001;  // CP_UTF8

/// Whether the output code page has to be switched: a console is attached (stdout or stderr) and does not decode
/// UTF-8 yet.
constexpr bool needs_utf8_output(bool console_attached, unsigned output_code_page) {
    return console_attached && output_code_page != kUtf8CodePage;
}

/// At the start of a run: switches the attached console (stdout or stderr) to UTF-8 when it is not already.
/// Does nothing without a console (a pipe, a file, the tray) and elsewhere than on Windows.
void use_utf8_output();

/// At the end of a run: the code page the console had before use_utf8_output(), if it switched it.
void restore_output_code_page();

}  // namespace fairyfly::console
