#pragma once
// Small Win32 helpers shared by the real setup hosts (internal to src/setup).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "include/setup/setup_hosts.h"

namespace fairyfly::setup::win {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::filesystem::path to_path(const std::string& utf8) { return std::filesystem::path(widen(utf8)); }

inline std::string hex_upper(const unsigned char* data, size_t n) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

inline std::vector<unsigned char> hex_to_bytes(const std::string& hex) {
    std::vector<unsigned char> out;
    auto val = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<unsigned char>(val(hex[i]) * 16 + val(hex[i + 1])));
    return out;
}

inline std::string error_text(unsigned long code) {
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text = n ? narrow(std::wstring(buffer, n)) : "error " + std::to_string(code);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
}

/// Throws a HostError with a stable code for a Win32 error.
[[noreturn]] inline void throw_win(unsigned long code, const std::string& what) {
    const char* c = code == ERROR_ACCESS_DENIED ? "ACCESS_DENIED" : code == ERROR_ALREADY_EXISTS ? "ALREADY_EXISTS"
                    : code == ERROR_FILE_NOT_FOUND ? "NOT_FOUND" : "HOST_ERROR";
    throw HostError(c, what + ": " + error_text(code) + " (" + std::to_string(code) + ")");
}

} // namespace fairyfly::setup::win
