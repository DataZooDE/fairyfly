#pragma once
// The approval of a cloud change at this desktop (decision 2026-10-24: there is no phone approval; the second factor
// for cloud operations is the desktop where SAP GUI runs). libffl's link agent asks Approver::approve_change before it
// runs a change; the Broker decides: a notification (Approve / Deny / Details), the approval window on Details or when
// no notification can be shown, a deny when nobody answers in time, when the screen is locked or there is no
// interactive session. Several questions are asked one after another, never together. Portable: the Windows UI is
// src/link/desktop_approval_win32.cpp.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "approver.hpp"  // libffl

namespace fairyfly::link::approval {

enum class Answer { None, Approve, Deny, Details };

/// The SAP session the change would run in (empty fields: not known, e.g. no session attached yet).
struct SapContext {
    std::string system, client, transaction, user;
};

/// One question as the UI shows it.
struct Request {
    uint64_t id = 0;
    std::string nonce;  ///< part of the notification's answers: an answer has to name the question it was shown with
    std::string client, user, tool, summary;
    std::vector<std::string> steps;
    int window_s = 300;  ///< a yes allows further changes for this long without asking again
    SapContext sap;
    std::chrono::steady_clock::time_point deadline;   ///< no answer by then: denied
    std::chrono::system_clock::time_point expires_at;  ///< the same instant on the wall clock (shown)
};

/// The person's side: a notification and the approval window.
class Ui {
public:
    virtual ~Ui() = default;
    /// Shows the notification (Approve / Deny / Details; its answers come back through Broker::answer). False when
    /// this desktop cannot show one: the window opens instead.
    virtual bool notify(const Request& r) = 0;
    /// Removes the notification (answered, expired); also from the notification centre.
    virtual void withdraw(const Request& r) = 0;
    /// The approval window. Blocks until the person answers (Approve or Deny; Esc and closing it are Deny) or until
    /// `stop` returns true (Answer::None; it is asked a few times a second).
    virtual Answer ask(const Request& r, const std::function<bool()>& stop) = 0;
};

struct Desk {
    /// True while a person can answer: an interactive, active and unlocked session.
    std::function<bool()> can_ask;
    /// The SAP session the change would run in.
    std::function<SapContext()> sap;
};

struct Options {
    /// How long a question waits for an answer, also in the queue. The gateway gives a call 125 s.
    std::chrono::milliseconds answer_within{std::chrono::seconds(90)};
    /// How often a waiting question checks for expiry and a locked screen.
    std::chrono::milliseconds poll{std::chrono::milliseconds(250)};
};

/// The answer to an approval and why (for the log).
struct Decision {
    bool approved = false;
    std::string reason;  ///< approved | denied | expired | expired_in_queue | locked | window_closed
};

/// "approve:<id>:<nonce>" (deny, details) as a notification's answer; nullopt when it is none.
struct ParsedAnswer {
    Answer answer = Answer::None;
    uint64_t id = 0;
    std::string nonce;
};
std::optional<ParsedAnswer> parse_answer(const std::string& text);
std::string answer_text(Answer a, const Request& r);

class Broker {
public:
    Broker(std::shared_ptr<Ui> ui, Desk desk, Options options = {});

    /// Asks the person; blocks until decided. True only for an explicit yes.
    Decision decide(const ffy::link::ApprovalPrompt& prompt);
    /// A notification's answer (any thread). Ignored unless it names the question being asked, with its nonce.
    void answer(uint64_t id, const std::string& nonce, Answer a);
    /// Any thread: the text of a notification's button (parse_answer).
    void answer_text(const std::string& text);

private:
    Request build(const ffy::link::ApprovalPrompt& p, uint64_t id) const;

    std::shared_ptr<Ui> ui_;
    Desk desk_;
    Options options_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<uint64_t> queue_;
    uint64_t next_id_ = 0;
    // the question being asked: its answer from the notification
    uint64_t asking_ = 0;
    std::string asking_nonce_;
    Answer noticed_ = Answer::None;
};

/// libffl's Approver over a Broker: approve_change asks the broker; approve(text) (an agent without the structured
/// question) asks it with the text as the only step.
std::shared_ptr<ffy::link::Approver> make_approver(std::shared_ptr<Broker> broker);

}  // namespace fairyfly::link::approval
