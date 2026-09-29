#include "include/console_prompt.h"

#include <iostream>

namespace fairyfly::cred {

namespace {

/// Restores the console mode on scope exit.
class ConsoleModeGuard {
public:
    ConsoleModeGuard(HANDLE handle, DWORD original) : handle_(handle), original_(original) {}
    ConsoleModeGuard(const ConsoleModeGuard&) = delete;
    ConsoleModeGuard& operator=(const ConsoleModeGuard&) = delete;
    ~ConsoleModeGuard() { SetConsoleMode(handle_, original_); }

private:
    HANDLE handle_;
    DWORD original_;
};

} // namespace

bool stdin_is_console() {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
}

SecretBuffer read_secret_line(std::istream& input) {
    std::string line;
    std::getline(input, line);
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    return SecretBuffer(std::move(line));
}

SecretBuffer read_secret(std::string_view prompt, PromptSource source) {
    if (source == PromptSource::Stdin) return read_secret_line(std::cin);

    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original = 0;
    if (!GetConsoleMode(input, &original)) return read_secret_line(std::cin);

    std::cerr << prompt << std::flush;
    ConsoleModeGuard guard(input, original);
    SetConsoleMode(input, (original & ~ENABLE_ECHO_INPUT) | ENABLE_LINE_INPUT);

    std::wstring buffer(2048, L'\0');
    DWORD read = 0;
    const BOOL ok = ReadConsoleW(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
    std::cerr << std::endl;  // the user's Enter was not echoed
    if (!ok) {
        scrub_string(buffer);
        return SecretBuffer();
    }
    buffer.resize(read);
    while (!buffer.empty() && (buffer.back() == L'\r' || buffer.back() == L'\n')) buffer.pop_back();
    return SecretBuffer(std::move(buffer));
}

} // namespace fairyfly::cred
