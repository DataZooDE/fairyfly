#pragma once
// Error code for a failed COM element lookup, shared by the commands so that `element get`,
// `element click` and friends answer the same code for the same absent id.

#include <string>

namespace fairyfly::sap {

/// ELEMENT_NOT_FOUND when the COM wrapper reports a missing element ("Element not found: <id>"),
/// otherwise COM_ERROR.
inline std::string error_code_for_com_message(const std::string& message) {
    static const std::string kMissing = "Element not found";
    return message.compare(0, kMissing.size(), kMissing) == 0 ? "ELEMENT_NOT_FOUND" : "COM_ERROR";
}

} // namespace fairyfly::sap
