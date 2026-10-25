#include "approver.hpp"

#include <chrono>
#include <future>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ffy::link {

ApprovalResult normalize(ApprovalResult r) {
    namespace k = approval_reason;
    if (r.approved) r.reason = r.reason == k::kRemembered || r.reason == k::kStanding || r.reason == k::kUnattended ? r.reason : std::string(k::kApproved);
    else if (r.reason == k::kExpired || r.reason == "expired_in_queue") r.reason = k::kExpired;
    else if (r.reason != k::kLocked) r.reason = k::kDenied;
    return r;
}

namespace {
struct Fixed : Approver {
    explicit Fixed(bool a) : a_(a) {}
    bool approve(const std::string&) override { return a_; }
    // nobody was asked: a yes is "unattended", never a person's "approved"
    ApprovalResult decide(const ApprovalPrompt&) override { return a_ ? ApprovalResult{true, approval_reason::kUnattended} : ApprovalResult{false, approval_reason::kDenied}; }
    bool a_;
};
struct Standing : Approver {
    bool approve(const std::string&) override { return false; }
    ApprovalResult decide(const ApprovalPrompt&) override { return {true, approval_reason::kStanding}; }
};
#ifdef _WIN32
struct Desktop : Approver {
    bool approve(const std::string& text) override { return ask(text, 45).approved; }
    ApprovalResult decide(const ApprovalPrompt& prompt) override { return ask(prompt.text, prompt.answer_s); }
    static ApprovalResult ask(const std::string& text, int answer_s) {
        auto p = std::make_shared<std::promise<bool>>();
        auto f = p->get_future();
        std::thread([p, text] {
            int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            std::wstring w(size_t(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), n);
            int r = MessageBoxW(nullptr, w.c_str(), L"fairyfly AI access - confirm SAP change", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
            p->set_value(r == IDYES);
        }).detach();
        if (f.wait_for(std::chrono::seconds(answer_s > 0 ? answer_s : 1)) != std::future_status::ready) return {false, approval_reason::kExpired};
        return f.get() ? ApprovalResult{true, approval_reason::kApproved} : ApprovalResult{false, approval_reason::kDenied};
    }
};
#endif
}  // namespace

std::shared_ptr<Approver> fixed_approver(bool a) { return std::make_shared<Fixed>(a); }
std::shared_ptr<Approver> standing_approver() { return std::make_shared<Standing>(); }
std::shared_ptr<Approver> desktop_approver() {
#ifdef _WIN32
    return std::make_shared<Desktop>();
#else
    return std::make_shared<Fixed>(false);
#endif
}

}  // namespace ffy::link
