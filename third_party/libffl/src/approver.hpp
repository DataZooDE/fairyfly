// Asks the person at the keyboard to confirm a state-changing AI action. This is the only approval of a change: the
// second factor for cloud operations is the desktop where SAP GUI runs (decision 2026-10-24, no phone approval).
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace ffy::link {

// What the person at the desktop is asked. The link agent builds it from the call it is about to run (never from text
// the gateway sends), after the assertion and the grants are verified. Nothing in it is a field value or screen content.
struct ApprovalPrompt {
    std::string client;              // the AI client asking (assertion "client", at most 24 characters; "unknown client")
    std::string user;                // the account it acts for (assertion "sub")
    std::string tool;                // the tool called ("gui_batch" for a batch)
    std::string summary;             // approval::summarize: every step, its target and field names, never a value
    std::vector<std::string> steps;  // approval::steps: the same, one text per step (a single call is one step)
    int window_s = 300;              // a yes allows further changes for this long without asking again
    std::string text;                // the whole question as one text: what the built-in dialog shows
};

class Approver {
public:
    virtual ~Approver() = default;
    virtual bool approve(const std::string& text) = 0;
    // The structured question; true only for an explicit yes. The default asks approve(prompt.text), so an Approver
    // that only implements the text question keeps working. A tray UI overrides this to show the steps as a list.
    virtual bool approve_change(const ApprovalPrompt& prompt) { return approve(prompt.text); }
};

std::shared_ptr<Approver> fixed_approver(bool answer);  // tests, or an explicitly unattended machine (--approve yes)
// Windows: topmost Yes/No dialog on the interactive desktop (no answer in 60 s = deny).
// Other platforms: denies (there is no desktop dialog); use fixed_approver(true) for unattended test rigs.
std::shared_ptr<Approver> desktop_approver();

}  // namespace ffy::link
