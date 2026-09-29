#pragma once

#include <windows.h>
#include <oleauto.h>
#include <string>
#include <string_view>
#include <limits>
#include <stdexcept>

namespace fairyfly::com {

// Strings crossing the CLI/COM boundary are UTF-8 in C++ and UTF-16 in BSTRs.
inline std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        throw std::length_error("UTF-8 input is too long");
    const int length = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              value.data(), length, nullptr, 0);
    if (required <= 0) throw std::invalid_argument("Invalid UTF-8 input");
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length,
                            result.data(), required) != required)
        throw std::runtime_error("UTF-8 conversion failed");
    return result;
}

inline std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        throw std::length_error("UTF-16 input is too long");
    const int length = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                              value.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::invalid_argument("Invalid UTF-16 input");
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                            result.data(), required, nullptr, nullptr) != required)
        throw std::runtime_error("UTF-16 conversion failed");
    return result;
}

inline std::string bstr_to_utf8(BSTR value) {
    if (!value) return {};
    return wide_to_utf8(std::wstring_view(value, SysStringLen(value)));
}

} // namespace fairyfly::com
