#pragma once
// Error code for a failed COM element lookup, shared by the commands so that `element get`,
// `element click` and friends answer the same code for the same absent id.

#include <string>

#include "include/core.h"

namespace fairyfly::sap {

/// ELEMENT_NOT_FOUND when the COM wrapper reports a missing element ("Element not found: <id>"),
/// otherwise COM_ERROR.
inline std::string error_code_for_com_message(const std::string& message) {
    static const std::string kMissing = "Element not found";
    return message.compare(0, kMissing.size(), kMissing) == 0 ? "ELEMENT_NOT_FOUND" : "COM_ERROR";
}

/// ELEMENT_NOT_FOUND for `resolved` (click, fill, F4): the element and three suggestions. With a `retry_command`
/// ("element click") and another active window (a popup opened), also the windows and how to retry there;
/// `retry_tail` follows the element in those suggestions (fill: ` "<value>"`).
inline void set_element_not_found(Result& result, const ElementId& resolved, const std::string& active_window,
                                  const std::string& retry_command, const std::string& retry_tail = "") {
    result.set_error("ELEMENT_NOT_FOUND", "Element not found at path: " + resolved.path);
    result.error["element"] = resolved.path;
    json suggestions = json::array({"Verify element path is correct", "Run 'screen read' to see available elements",
                                    "Check if screen is fully loaded"});
    const WindowId requested = resolved.get_window();
    if (!retry_command.empty() && !window_ids_match(requested, WindowId(active_window))) {
        result.error["window_mismatch"] = true;
        result.error["requested_window"] = requested.id;
        result.error["active_window"] = active_window;
        suggestions.push_back("Active window is " + active_window + ", not " + requested.id + " - did a popup open?");
        suggestions.push_back("Try: fairyfly " + retry_command + " " + active_window + "/" + resolved.get_element_path() + retry_tail);
        suggestions.push_back("Or use: fairyfly " + retry_command + " @active/" + resolved.get_element_path() + retry_tail);
    }
    result.error["suggestions"] = suggestions;
}

} // namespace fairyfly::sap
