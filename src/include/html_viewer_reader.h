#pragma once

#include <string>

namespace fairyfly::sap {

struct HtmlViewerText {
    std::string text;
    bool truncated = false;
};

// Reads the accessible document text of a SAP GUI HTMLViewer window.
// Returns an empty value when the browser does not expose a UIA text pattern.
// Falls back to any element exposing a UIA text pattern when no Document
// control exists. max_attempts bounds the 100 ms retry loop (generic probes use 1).
HtmlViewerText read_html_viewer_text(int native_handle, int max_attempts = 8);

} // namespace fairyfly::sap
