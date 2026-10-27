#pragma once
// An HRESULT as text: always unsigned hex (0x80020009), never the signed long that fmt prints for {:08X}
// (0x-7FFDFFF7).
#include <windows.h>

#include <string>

#include <fmt/format.h>

namespace fairyfly {

inline std::string hresult_hex(HRESULT hr) { return fmt::format("0x{:08X}", static_cast<unsigned long>(hr)); }

}  // namespace fairyfly
