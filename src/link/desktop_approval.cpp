// The approval of a cloud change at this desktop: the decision logic (see the header). The Windows notification and
// window are src/link/desktop_approval_win32.cpp.
#include "include/link/desktop_approval.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <random>

namespace fairyfly::link::approval {

namespace {

const char* verb(Answer a) {
    switch (a) {
        case Answer::Approve: return "approve";
        case Answer::Deny: return "deny";
        case Answer::Details: return "details";
        default: return "";
    }
}

std::string random_nonce() {
    std::random_device rd;
    std::uniform_int_distribution<uint64_t> dist;
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 2; i++) {
        uint64_t v = dist(rd);
        for (int k = 0; k < 16; k++, v >>= 4) out += hex[v & 0xF];
    }
    return out;
}

}  // namespace

std::string client_name(const std::string& client_id) {
    std::string id = client_id;
    while (!id.empty() && (id.back() == ' ' || id.back() == '/')) id.pop_back();
    if (id.empty() || id == "unknown client") return "an AI assistant";
    std::string lower;
    for (char c : id) lower += char(std::tolower(static_cast<unsigned char>(c)));
    // known assistants, by the host or the name they report
    struct Known {
        const char* part;
        const char* name;
    };
    static const Known known[] = {{"claude-code", "Claude Code"}, {"claude code", "Claude Code"}, {"claude desktop", "Claude Desktop"},
                                  {"claude.ai", "Claude"},        {"anthropic.com", "Claude"},    {"chatgpt.com", "ChatGPT"},
                                  {"openai.com", "ChatGPT"},      {"copilot", "Microsoft Copilot"}, {"gemini.google", "Gemini"}};
    for (const auto& k : known)
        if (lower.find(k.part) != std::string::npos) return k.name;
    const auto scheme = lower.find("://");
    if (scheme == std::string::npos) return id;  // a plain name
    std::string host = id.substr(scheme + 3);
    host = host.substr(0, host.find_first_of("/:?#"));
    if (host.rfind("www.", 0) == 0) host = host.substr(4);
    return host.empty() ? id : host;
}

std::string readable_step(const std::string& step) {
    // SAP GUI's standard toolbar (tbar[0]) is the same on every screen
    static const std::pair<const char*, const char*> buttons[] = {{"tbar[0]/btn[0]", "Enter"}, {"tbar[0]/btn[3]", "Back"},
                                                                  {"tbar[0]/btn[11]", "Save"}, {"tbar[0]/btn[12]", "Cancel"},
                                                                  {"tbar[0]/btn[15]", "Exit"}};
    std::string out = step;
    for (const auto& [id, name] : buttons) {
        const std::string suffix = std::string(id);
        for (size_t at = out.find(suffix); at != std::string::npos; at = out.find(suffix, at + 1)) {
            const size_t end = at + suffix.size();
            if (end < out.size() && out[end] != ' ' && out[end] != ',' && out[end] != ';') continue;  // btn[11] only, not btn[110]
            // the whole element id: back to the start of the word
            size_t begin = out.rfind(' ', at);
            begin = begin == std::string::npos ? 0 : begin + 1;
            const std::string element = out.substr(begin, end - begin);
            const std::string named = std::string(name) + " (" + element + ")";
            out.replace(begin, end - begin, named);
            at = begin + named.size() - 1;
        }
    }
    return out;
}

std::string answer_text(Answer a, const Request& r) { return std::string(verb(a)) + ":" + std::to_string(r.id) + ":" + r.nonce; }

std::optional<ParsedAnswer> parse_answer(const std::string& text) {
    const auto c1 = text.find(':');
    if (c1 == std::string::npos) return std::nullopt;
    const auto c2 = text.find(':', c1 + 1);
    if (c2 == std::string::npos) return std::nullopt;
    ParsedAnswer p;
    const std::string v = text.substr(0, c1), id = text.substr(c1 + 1, c2 - c1 - 1);
    p.nonce = text.substr(c2 + 1);
    if (v == "approve") p.answer = Answer::Approve;
    else if (v == "deny") p.answer = Answer::Deny;
    else if (v == "details") p.answer = Answer::Details;
    else return std::nullopt;
    if (id.empty() || id.size() > 19 || id.find_first_not_of("0123456789") != std::string::npos || p.nonce.empty()) return std::nullopt;
    p.id = std::stoull(id);
    return p;
}

Broker::Broker(std::shared_ptr<Ui> ui, Desk desk, Options options) : ui_(std::move(ui)), desk_(std::move(desk)), options_(options) {}

Request Broker::build(const ffy::link::ApprovalPrompt& p, uint64_t id) const {
    Request r;
    r.id = id;
    r.nonce = random_nonce();
    r.client = client_name(p.client);
    r.client_id = client_id(p);
    r.user = p.user;
    r.tool = p.tool;
    r.summary = readable_step(p.summary.empty() ? p.text : p.summary);
    for (const auto& s : p.steps) r.steps.push_back(readable_step(s));
    if (r.steps.empty()) r.steps.push_back(r.summary);
    r.remember_s = int(std::chrono::duration_cast<std::chrono::seconds>(options_.remember_for).count());
    if (desk_.sap) {
        try {
            r.sap = desk_.sap();
        } catch (const std::exception& e) {
            spdlog::warn("approval: the SAP session could not be read: {}", e.what());
        }
    }
    return r;
}

Decision Broker::decide(const ffy::link::ApprovalPrompt& prompt) {
    using clock = std::chrono::steady_clock;
    auto within = options_.answer_within;
    if (prompt.answer_s > 0) within = std::min(within, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(prompt.answer_s)));
    const auto deadline = clock::now() + within;
    const auto can_ask = [this] { return !desk_.can_ask || desk_.can_ask(); };
    uint64_t id = 0;
    // one question at a time, in the order they came; waiting counts against the question's own time
    {
        std::unique_lock<std::mutex> l(mu_);
        id = ++next_id_;
        queue_.push_back(id);
        if (!cv_.wait_until(l, deadline, [&] { return queue_.front() == id; })) {
            queue_.erase(std::find(queue_.begin(), queue_.end(), id));
            cv_.notify_all();
            spdlog::info("approval {}: denied, expired while another approval was open ({} {})", id, prompt.client, prompt.tool);
            return {false, "expired_in_queue"};
        }
    }
    struct Done {
        Broker& b;
        ~Done() {
            std::lock_guard<std::mutex> l(b.mu_);
            b.queue_.pop_front();
            b.asking_ = 0;
            b.asking_nonce_.clear();
            b.noticed_ = Answer::None;
            b.cv_.notify_all();
        }
    } done{*this};

    if (!can_ask()) {
        spdlog::info("approval {}: denied, the screen is locked or there is no interactive session ({} {})", id, prompt.client, prompt.tool);
        return {false, "locked"};
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        auto i = remembered_.find(remember_key(prompt));
        if (i != remembered_.end()) {
            if (clock::now() < i->second) {
                spdlog::info("approval {}: remembered (\"don't ask again\" for {} by {}: {})", id, client_name(prompt.client), prompt.user,
                             prompt.summary.empty() ? prompt.tool : prompt.summary);
                return {true, "remembered"};
            }
            remembered_.erase(i);
        }
    }
    Request r = build(prompt, id);
    r.deadline = deadline;
    r.expires_at = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(deadline - clock::now());
    {
        std::lock_guard<std::mutex> l(mu_);
        asking_ = id;
        asking_nonce_ = r.nonce;
        noticed_ = Answer::None;
    }
    const bool noticed = ui_->notify(r);
    bool window = !noticed;
    Decision d{false, "expired"};
    while (true) {
        if (window) {
            bool remember = false;
            const Answer a = ui_->ask(
                r,
                [&] {
                    std::lock_guard<std::mutex> l(mu_);
                    return clock::now() >= deadline || noticed_ == Answer::Approve || noticed_ == Answer::Deny || !can_ask();
                },
                remember);
            Answer first;
            {
                std::lock_guard<std::mutex> l(mu_);
                first = noticed_;
            }
            if ((a == Answer::Approve || a == Answer::Deny) && (first == Answer::Approve || first == Answer::Deny)) {
                // the notification was answered before the window's next check: its answer counts, and it remembers nothing
                d = {first == Answer::Approve, first == Answer::Approve ? "approved" : "denied"};
                spdlog::info("approval {}: the notification was answered before the window; the window's answer is ignored", id);
                break;
            }
            if (a == Answer::Approve && !(clock::now() < deadline && can_ask())) {
                // a click between the deadline (or the lock) and the window's next check: not a yes, nothing remembered
                d = {false, can_ask() ? "expired" : "locked"};
                spdlog::info("approval {}: a yes in the window came after the time was up or the screen locked", id);
                break;
            }
            if (a == Answer::Approve || a == Answer::Deny) {
                d = {a == Answer::Approve, a == Answer::Approve ? "approved" : "denied"};
                if (a == Answer::Approve && remember) {
                    std::lock_guard<std::mutex> l(mu_);
                    remembered_[remember_key(prompt)] = clock::now() + options_.remember_for;
                    spdlog::info("approval {}: further changes by {} for {} approved without asking until the time is up", id, r.client, r.user);
                }
                break;
            }
            window = false;  // stopped: the notification answered, it expired or the screen locked
        }
        Answer got;
        {
            std::lock_guard<std::mutex> l(mu_);
            got = noticed_;
        }
        if (got == Answer::Approve || got == Answer::Deny) {
            d = {got == Answer::Approve, got == Answer::Approve ? "approved" : "denied"};
            break;
        }
        if (clock::now() >= deadline) break;
        if (!can_ask()) {
            d = {false, "locked"};
            break;
        }
        std::unique_lock<std::mutex> l(mu_);
        if (noticed_ == Answer::Details) {
            noticed_ = Answer::None;
            window = true;
            continue;
        }
        cv_.wait_until(l, std::min(deadline, clock::now() + options_.poll), [&] { return noticed_ != Answer::None; });
    }
    if (noticed) ui_->withdraw(r);
    spdlog::info("approval {}: {} ({} for {}: {})", id, d.reason, r.client, r.user, r.summary);
    return d;
}

void Broker::answer(uint64_t id, const std::string& nonce, Answer a) {
    if (a == Answer::None) return;
    std::lock_guard<std::mutex> l(mu_);
    if (id == 0 || id != asking_ || nonce != asking_nonce_) {
        spdlog::info("approval: an answer for a question that is not open was ignored");
        return;
    }
    // the first Approve or Deny counts; Details only opens the window
    if (noticed_ == Answer::Approve || noticed_ == Answer::Deny) return;
    noticed_ = a;
    cv_.notify_all();
}

void Broker::answer_text(const std::string& text) {
    if (auto p = parse_answer(text)) answer(p->id, p->nonce, p->answer);
}

namespace {
struct BrokerApprover : ffy::link::Approver {
    explicit BrokerApprover(std::shared_ptr<Broker> b) : broker(std::move(b)) {}
    bool approve(const std::string& text) override {
        ffy::link::ApprovalPrompt p;
        p.client = "unknown client";
        p.text = text;
        p.summary = text;
        p.steps = {text};
        return broker->decide(p).approved;
    }
    bool approve_change(const ffy::link::ApprovalPrompt& prompt) override { return broker->decide(prompt).approved; }
    ffy::link::ApprovalResult decide(const ffy::link::ApprovalPrompt& prompt) override {
        const Decision d = broker->decide(prompt);
        return {d.approved, d.reason == "expired_in_queue" ? ffy::link::approval_reason::kExpired : d.reason};
    }
    std::shared_ptr<Broker> broker;
};
}  // namespace

std::shared_ptr<ffy::link::Approver> make_approver(std::shared_ptr<Broker> broker) { return std::make_shared<BrokerApprover>(std::move(broker)); }

}  // namespace fairyfly::link::approval
