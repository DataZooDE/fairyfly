// Child process of test_link_console.cpp, started in a new console: sets the console's output code page, shows the
// pairing QR code the way `fairyfly link pair` does (or, mode "raw", the way it did before: UTF-8 bytes), then
// reads the console's screen buffer back and saves it as UTF-8.
//   link_console_fixture <blocks|ascii|raw> <codepage> <text> <out file>
#include <windows.h>

#include <fstream>
#include <string>

#include "include/link/link_console.h"
#include "qr.hpp"

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const std::string mode = argv[1];
    if (!SetConsoleOutputCP(static_cast<UINT>(std::stoi(argv[2])))) return 3;
    HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (con == INVALID_HANDLE_VALUE) return 4;
    SetConsoleScreenBufferSize(con, COORD{200, 400});

    const auto m = ffy::qr::encode(argv[3]);
    const std::string text = "Contacting https://fairyfly.io\xE2\x80\xA6\n\n" +
                             (mode == "ascii" ? fairyfly::link::render_qr(m, fairyfly::link::QrStyle::Ascii)
                                              : fairyfly::link::render_qr(m, fairyfly::link::QrStyle::Blocks));
    if (mode == "raw") {  // what std::cerr << to_terminal(...) did: the bytes, decoded by the console's code page
        DWORD written = 0;
        if (!WriteFile(con, text.data(), DWORD(text.size()), &written, nullptr)) return 5;
    } else if (!fairyfly::link::write_utf8(text, con)) {
        return 6;
    }

    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(con, &info)) return 7;
    std::ofstream out(argv[4], std::ios::binary);
    std::wstring row(info.dwSize.X, L' ');
    for (SHORT y = 0; y <= info.dwCursorPosition.Y; y++) {
        DWORD read = 0;
        if (!ReadConsoleOutputCharacterW(con, row.data(), DWORD(row.size()), COORD{0, y}, &read)) return 8;
        std::wstring line = row.substr(0, read);
        line.erase(line.find_last_not_of(L' ') + 1);
        int n = WideCharToMultiByte(CP_UTF8, 0, line.data(), int(line.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.data(), int(line.size()), utf8.data(), n, nullptr, nullptr);
        out << utf8 << "\n";
    }
    return out ? 0 : 9;
}
