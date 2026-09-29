#pragma once

#include <string>

namespace fairyfly::sap {

struct HtmlViewerText {
    std::string text;
    bool truncated = false;
};

// Reads the accessible document text of a SAP GUI HTMLViewer window.
// Returns an empty value when the browser does not expose a UIA text pattern.
HtmlViewerText read_html_viewer_text(int native_handle);

} // namespace fairyfly::sap
