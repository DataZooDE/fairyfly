#pragma once
// UTF-8 (fairyfly's strings) <-> UTF-16 (Windows, COM) in one place. Two policies:
//  - strict (utf8_to_wide, wide_to_utf8): throw std::invalid_argument on invalid input; for strings that reach SAP.
//  - lossy (*_lossy): never throw; an invalid sequence becomes U+FFFD; only for text people read: logs, window
//    titles, tooltips, console output. Never for identities, secrets, paths, command lines or anything a decision
//    is based on: those stay strict.
#include <windows.h>

#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fairyfly::text {

namespace detail {
inline int checked_length(std::size_t size) {
    if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) throw std::length_error("text input is too long");
    return static_cast<int>(size);
}

inline std::wstring to_wide(std::string_view value, DWORD flags) {
    if (value.empty()) return {};
    const int length = checked_length(value.size());
    const int required = MultiByteToWideChar(CP_UTF8, flags, value.data(), length, nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, flags, value.data(), length, result.data(), required) != required) return {};
    return result;
}

inline std::string to_utf8(std::wstring_view value, DWORD flags) {
    if (value.empty()) return {};
    const int length = checked_length(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, flags, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, flags, value.data(), length, result.data(), required, nullptr, nullptr) != required) return {};
    return result;
}
}  // namespace detail

/// Strict: throws std::invalid_argument for invalid UTF-8.
inline std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    std::wstring result = detail::to_wide(value, MB_ERR_INVALID_CHARS);
    if (result.empty()) throw std::invalid_argument("Invalid UTF-8 input");
    return result;
}

/// Strict: throws std::invalid_argument for invalid UTF-16 (a lone surrogate).
inline std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    std::string result = detail::to_utf8(value, WC_ERR_INVALID_CHARS);
    if (result.empty()) throw std::invalid_argument("Invalid UTF-16 input");
    return result;
}

/// Lossy: never throws (also not for oversized input: then ""); invalid sequences become U+FFFD.
inline std::wstring utf8_to_wide_lossy(std::string_view value) noexcept {
    try {
        return detail::to_wide(value, 0);
    } catch (...) {
        return {};
    }
}

/// Lossy: never throws; a lone surrogate becomes U+FFFD.
inline std::string wide_to_utf8_lossy(std::wstring_view value) noexcept {
    try {
        return detail::to_utf8(value, 0);
    } catch (...) {
        return {};
    }
}

/// Lossy, for a C wide string (COM member names in log lines); nullptr gives "".
inline std::string wide_to_utf8_lossy(const wchar_t* value) noexcept {
    return value ? wide_to_utf8_lossy(std::wstring_view(value)) : std::string();
}

/// A path from UTF-8 text (an environment value, a config entry). std::filesystem::path(std::string) would read
/// the bytes in the ANSI code page on Windows.
inline std::filesystem::path utf8_path(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

/// A path as UTF-8 text (path::string() is the ANSI code page on Windows).
inline std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

}  // namespace fairyfly::text
