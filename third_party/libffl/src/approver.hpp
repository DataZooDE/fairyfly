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
    std::string client;              // the AI client asking, for display (assertion "client", at most 24 characters; "unknown client")
    // The whole assertion "client" ("unknown client" when there is none). What an Approver that remembers a yes keys
    // it by: `client` is cut to 24 characters, so "https://claude.ai/oauth/claude-code-client-metadata" and every other
    // client starting with "https://claude.ai/oauth/" would share it (review of fairyfly #34).
    std::string client_id;
    std::string user;                // the account it acts for (assertion "sub")
    std::string tool;                // the tool called ("gui_batch" for a batch)
    std::string summary;             // approval::summarize: every step, its target and field names, never a value
    std::vector<std::string> steps;  // approval::steps: the same, one text per step (a single call is one step)
    int window_s = 300;              // a yes allows further changes for this long without asking again
    // How long the question waits for an answer; then it is "expired". 45 s: the answer (or APPROVAL_EXPIRED) must reach
    // the assistant before its MCP client gives up (about 60 s); strictly below the gateway's 50 s (approval_timeout_ms).
    int answer_s = 45;
    std::string text;                // the whole question as one text: what the built-in dialog shows
    // The administrator requires a question for every change (approval model v2: the organisation's approval "always",
    // pushed as proto::LinkPolicy::approval_enforced or signed into this call). Then the answer must be the person's
    // own for exactly this change: "standing" (the tray's "Allow changes in SAP") and "remembered" ("Don't ask
    // again") run nothing, the agent refuses with APPROVAL_REQUIRED. The tray greys out its toggle and the checkbox.
    bool enforced = false;
};

// The answer and why. The link agent reports `reason` to the gateway (res.approval, the audit record's "approval") and
// refuses with APPROVAL_DENIED / APPROVAL_EXPIRED / APPROVAL_LOCKED (APPROVAL_REQUIRED: a yes that did not ask while
// approval is enforced). Only `approved` decides; reasons it does not know are read as "approved" (approved true) or
// "denied" (approved false), "expired_in_queue" as "expired".
namespace approval_reason {
inline constexpr const char* kApproved = "approved";      // the person said yes
inline constexpr const char* kRemembered = "remembered";  // an earlier yes covers it ("don't ask again", the agent's window)
// Ran without a question because the desktop's "Allow changes in SAP" is on (approval model v2, the user decides). Not
// allowed while ApprovalPrompt::enforced: the agent refuses with APPROVAL_REQUIRED, the gateway too.
inline constexpr const char* kStanding = "standing";
// Yes without a person: an Approver that always says yes (fixed_approver(true), ffy-link --approve yes). Never a
// person's answer, so refused like standing while ApprovalPrompt::enforced.
inline constexpr const char* kUnattended = "unattended";
inline constexpr const char* kDenied = "denied";          // the person said no (or closed the question)
inline constexpr const char* kExpired = "expired";        // nobody answered within answer_s (also: waited too long in the queue)
inline constexpr const char* kLocked = "locked";          // the screen is locked, or nobody is signed in to answer
inline constexpr const char* kNone = "none";              // nothing to approve (reads, approvals off), or refused before asking
}  // namespace approval_reason
struct ApprovalResult {
    bool approved = false;
    std::string reason;
};
ApprovalResult normalize(ApprovalResult r);

class Approver {
public:
    virtual ~Approver() = default;
    virtual bool approve(const std::string& text) = 0;
    // The structured question; true only for an explicit yes. The default asks approve(prompt.text), so an Approver
    // that only implements the text question keeps working. A tray UI overrides this to show the steps as a list.
    virtual bool approve_change(const ApprovalPrompt& prompt) { return approve(prompt.text); }
    // The structured question with the reason (what the link agent calls). The default asks approve_change(prompt):
    // an Approver that answers only yes/no is reported as approved / denied.
    virtual ApprovalResult decide(const ApprovalPrompt& prompt) {
        bool yes = approve_change(prompt);
        return {yes, yes ? approval_reason::kApproved : approval_reason::kDenied};
    }
    // The administrator's setting (proto::LinkPolicy::approval_enforced), also when no question is pending: on every
    // connect and whenever it changes. A tray greys out "Allow changes in SAP" while it is true. Called from the link's
    // connection thread; must not block.
    virtual void approval_enforced(bool enforced) { (void)enforced; }
};

// tests, or an explicitly unattended machine (--approve yes): a yes is reported as "unattended" (refused while enforced)
std::shared_ptr<Approver> fixed_approver(bool answer);
// Every change runs under a standing approval (ffy-link --approve standing: the CLI form of the tray's "Allow changes
// in SAP"). Nobody can be asked here, so while approval is enforced nothing runs (APPROVAL_REQUIRED).
std::shared_ptr<Approver> standing_approver();
// Windows: topmost Yes/No dialog on the interactive desktop (no answer within prompt.answer_s = expired).
// Other platforms: denies (there is no desktop dialog); use fixed_approver(true) for unattended test rigs.
std::shared_ptr<Approver> desktop_approver();

}  // namespace ffy::link
