#pragma once

#include <string>

namespace fairyfly::sap {

struct HtmlViewerText {
    std::string text;
    bool truncated = false;
};

// Result of one UIA read attempt; DocumentEmpty and NoDocument are worth retrying (the
// browser window exists but has not created or rendered its document yet), the others
// (no UIA, no root element) will not change within the budget.
enum class HtmlReadOutcome { NoAutomation, NoRoot, NoDocument, DocumentEmpty, Text };

// Time allowed for the whole read. A zero total_ms means a single attempt.
struct HtmlReadBudget {
    int total_ms = 300;
    int poll_ms = 50;
};

// True when another attempt fits in the budget after elapsed_ms.
inline bool retry_decision(HtmlReadOutcome outcome, int elapsed_ms, const HtmlReadBudget& budget) {
    const bool transient = outcome == HtmlReadOutcome::DocumentEmpty || outcome == HtmlReadOutcome::NoDocument;
    return transient && elapsed_ms + budget.poll_ms < budget.total_ms;
}

// Reads the accessible document text of a SAP GUI HTMLViewer window.
// Returns an empty value when the browser does not expose a UIA text pattern.
// Falls back to any element exposing a UIA text pattern when no Document
// control exists. Retries only while the document is missing or empty, within the budget
// (generic probes pass a zero budget for a single attempt).
HtmlViewerText read_html_viewer_text(int native_handle, HtmlReadBudget budget = {});

// True when the window or one of its child windows has a browser host class
// (Internet Explorer_Server, Shell DocObject View, Shell Embedding,
// Chrome_WidgetWin_*, WebView2). Used to gate the generic UIA probe.
bool handle_hosts_browser(int native_handle);

// Gate for the generic shell probe: when true, UIA is skipped for shells that
// host no browser window. Off until a live diff shows no output change.
constexpr bool kGateGenericHtmlProbe = false;

} // namespace fairyfly::sap
