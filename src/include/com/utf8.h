#pragma once

#include <windows.h>
#include <oleauto.h>
#include <string>

#include "include/text/utf8.h"

namespace fairyfly::com {

// Strings crossing the CLI/COM boundary are UTF-8 in C++ and UTF-16 in BSTRs: strict conversion (text/utf8.h).
using text::utf8_to_wide;
using text::wide_to_utf8;

inline std::string bstr_to_utf8(BSTR value) {
    if (!value) return {};
    return wide_to_utf8(std::wstring_view(value, SysStringLen(value)));
}

} // namespace fairyfly::com
