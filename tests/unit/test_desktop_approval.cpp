// The approval of a cloud change at this desktop (src/link/desktop_approval.cpp): the decision logic without a UI -
// answers from the notification, the window, expiry, a locked screen, several questions one after another.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

#include "include/link/desktop_approval.h"
#include "include/link/link_runner.h"
#include "support/eventually.h"

using namespace fairyfly::link::approval;
using namespace std::chrono_literals;
using fairyfly::test::eventually;

namespace {

ffy::link::ApprovalPrompt prompt(const std::string& summary = "fill fields ctxtA, ctxtB, then press f8") {
    ffy::link::ApprovalPrompt p;
    p.client = "Claude";
    p.user = "jr@example.com";
    p.tool = "gui_element_fill";
    p.summary = summary;
    p.steps = {summary};
    p.window_s = 300;
    p.text = "An AI assistant (Claude) ...";
    return p;
}

// The person, scripted: what the window answers, whether a notification can be shown; records what was shown.
struct FakeUi : Ui {
    bool can_notify = true;
    // the window's answer; None: waits until stopped
    Answer window_answer = Answer::None;
    bool window_remember = false;  // "Don't ask again" ticked
    std::chrono::milliseconds window_delay{0};
    // the window answers before it notices that it should close (the Win32 window checks only every 200 ms)
    bool ignore_stop = false;

    std::mutex mu;
    std::vector<Request> notified, withdrawn, windows;
    std::atomic<int> open_windows{0}, max_open_windows{0}, open_notices{0}, max_open_notices{0};

    bool notify(const Request& r) override {
        if (!can_notify) return false;
        std::lock_guard<std::mutex> l(mu);
        notified.push_back(r);
        int n = ++open_notices;
        if (n > max_open_notices) max_open_notices = n;
        return true;
    }
    void withdraw(const Request& r) override {
        std::lock_guard<std::mutex> l(mu);
        withdrawn.push_back(r);
        --open_notices;
    }
    Answer ask(const Request& r, const std::function<bool()>& stop, bool& remember) override {
        remember = window_remember;
        {
            std::lock_guard<std::mutex> l(mu);
            windows.push_back(r);
        }
        int n = ++open_windows;
        if (n > max_open_windows) max_open_windows = n;
        const auto until = std::chrono::steady_clock::now() + window_delay;
        Answer a = Answer::None;
        while (true) {
            if (!ignore_stop && stop()) break;
            if (window_answer != Answer::None && std::chrono::steady_clock::now() >= until) {
                a = window_answer;
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        --open_windows;
        return a;
    }
    Request last_notified() {
        std::lock_guard<std::mutex> l(mu);
        return notified.back();
    }
    size_t notified_count() {
        std::lock_guard<std::mutex> l(mu);
        return notified.size();
    }
};

Options fast(std::chrono::milliseconds within = 2s) {
    Options o;
    o.answer_within = within;
    o.poll = 5ms;
    return o;
}

Desk desk(std::shared_ptr<std::atomic<bool>> unlocked = std::make_shared<std::atomic<bool>>(true),
          std::shared_ptr<Standing> standing = std::make_shared<Standing>()) {
    Desk d;
    d.can_ask = [unlocked] { return unlocked->load(); };
    d.sap = [] { return SapContext{"BIG", "100", "VA02", "JR"}; };
    d.standing = std::move(standing);
    return d;
}

// The saved "Allow changes in SAP", in memory: what Standing loads and saves.
struct Saved {
    std::optional<std::string> value;
    int saves = 0;
    std::shared_ptr<Standing> standing() {
        return std::make_shared<Standing>([this] { return value; },
                                          [this](const std::string& v) {
                                              value = v;
                                              saves++;
                                              return true;
                                          });
    }
};

}  // namespace

TEST_CASE("desktop approval: the notification's Approve approves, and the notification is withdrawn", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    Broker b(ui, desk(), fast());
    auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
    REQUIRE(eventually([&] { return ui->notified_count() == 1; }));
    const Request r = ui->last_notified();
    CHECK(r.client == "Claude");
    CHECK(r.user == "jr@example.com");
    CHECK(r.sap.system == "BIG");
    CHECK(r.sap.transaction == "VA02");
    CHECK(r.steps == std::vector<std::string>{"fill fields ctxtA, ctxtB, then press Execute (F8)"});
    CHECK(r.offer_standing);  // the window offers "Don't ask again"
    CHECK(!r.nonce.empty());
    b.answer(r.id, r.nonce, Answer::Approve);
    const Decision d = f.get();
    CHECK(d.approved);
    CHECK(d.reason == "approved");
    CHECK(ui->withdrawn.size() == 1);
    CHECK(ui->windows.empty());
}

TEST_CASE("desktop approval: the notification's Deny denies", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    Broker b(ui, desk(), fast());
    auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
    REQUIRE(eventually([&] { return ui->notified_count() == 1; }));
    const Request r = ui->last_notified();
    b.answer_text(answer_text(Answer::Deny, r));
    const Decision d = f.get();
    CHECK_FALSE(d.approved);
    CHECK(d.reason == "denied");
}

TEST_CASE("desktop approval: an answer without the question's nonce, or for another question, is ignored", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    Broker b(ui, desk(), fast(300ms));
    auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
    REQUIRE(eventually([&] { return ui->notified_count() == 1; }));
    const Request r = ui->last_notified();
    b.answer(r.id, "guessed", Answer::Approve);
    b.answer(r.id + 1, r.nonce, Answer::Approve);
    b.answer_text("approve:" + std::to_string(r.id));
    const Decision d = f.get();
    CHECK_FALSE(d.approved);
    CHECK(d.reason == "expired");
}

TEST_CASE("desktop approval: Details opens the window, which decides", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->window_answer = Answer::Approve;
    Broker b(ui, desk(), fast());
    auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
    REQUIRE(eventually([&] { return ui->notified_count() == 1; }));
    const Request r = ui->last_notified();
    b.answer(r.id, r.nonce, Answer::Details);
    const Decision d = f.get();
    CHECK(d.approved);
    REQUIRE(ui->windows.size() == 1);
    CHECK(ui->windows[0].id == r.id);
    CHECK(ui->withdrawn.size() == 1);
}

TEST_CASE("desktop approval: Deny (or Esc) in the window denies", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->window_answer = Answer::Deny;
    ui->can_notify = false;  // no notification: the window opens at once
    Broker b(ui, desk(), fast());
    const Decision d = b.decide(prompt());
    CHECK_FALSE(d.approved);
    CHECK(d.reason == "denied");
    CHECK(ui->windows.size() == 1);
    CHECK(ui->withdrawn.empty());
}

TEST_CASE("desktop approval: the notification's Approve while the window is open closes the window and approves", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();  // the window waits
    Broker b(ui, desk(), fast());
    auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
    REQUIRE(eventually([&] { return ui->notified_count() == 1; }));
    const Request r = ui->last_notified();
    b.answer(r.id, r.nonce, Answer::Details);
    REQUIRE(eventually([&] { return ui->open_windows.load() == 1; }));
    b.answer(r.id, r.nonce, Answer::Approve);
    const Decision d = f.get();
    CHECK(d.approved);
    CHECK(ui->open_windows.load() == 0);
}

TEST_CASE("desktop approval: no answer in time is a deny, and the window closes", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    Broker b(ui, desk(), fast(150ms));
    const auto start = std::chrono::steady_clock::now();
    const Decision d = b.decide(prompt());
    CHECK_FALSE(d.approved);
    CHECK(d.reason == "expired");
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    CHECK(ui->open_windows.load() == 0);
}

TEST_CASE("desktop approval: an unanswered notification expires and is withdrawn", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    Broker b(ui, desk(), fast(150ms));
    const Decision d = b.decide(prompt());
    CHECK_FALSE(d.approved);
    CHECK(d.reason == "expired");
    CHECK(ui->withdrawn.size() == 1);
}

TEST_CASE("desktop approval: a locked screen denies without asking", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    auto unlocked = std::make_shared<std::atomic<bool>>(false);
    Broker b(ui, desk(unlocked), fast());
    const Decision d = b.decide(prompt());
    CHECK_FALSE(d.approved);
    CHECK(d.reason == "locked");
    CHECK(ui->notified_count() == 0);
    CHECK(ui->windows.empty());
}

TEST_CASE("desktop approval: locking the screen while asked denies and closes the window", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    auto unlocked = std::make_shared<std::atomic<bool>>(true);
    Broker b(ui, desk(unlocked), fast());
    auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
    REQUIRE(eventually([&] { return ui->open_windows.load() == 1; }));
    *unlocked = false;
    const Decision d = f.get();
    CHECK_FALSE(d.approved);
    CHECK(d.reason == "locked");
    CHECK(ui->open_windows.load() == 0);
}

TEST_CASE("desktop approval: several questions are asked one after another, each answered on its own", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    Broker b(ui, desk(), fast(3s));
    auto first = std::async(std::launch::async, [&] { return b.decide(prompt("click wnd[0]/tbar[0]/btn[11]")); });
    REQUIRE(eventually([&] { return ui->notified_count() == 1; }));
    auto second = std::async(std::launch::async, [&] { return b.decide(prompt("press enter")); });
    std::this_thread::sleep_for(50ms);
    CHECK(ui->notified_count() == 1);  // the second waits; never shown beside the first
    const Request r1 = ui->last_notified();
    CHECK(r1.summary == "click Save (wnd[0]/tbar[0]/btn[11])");
    b.answer(r1.id, r1.nonce, Answer::Approve);
    CHECK(first.get().approved);
    REQUIRE(eventually([&] { return ui->notified_count() == 2; }));
    const Request r2 = ui->last_notified();
    CHECK(r2.summary == "press Enter");
    CHECK(r2.id != r1.id);
    // the first question's answer does not answer the second
    b.answer(r1.id, r1.nonce, Answer::Approve);
    std::this_thread::sleep_for(30ms);
    b.answer(r2.id, r2.nonce, Answer::Deny);
    CHECK_FALSE(second.get().approved);
    CHECK(ui->max_open_notices.load() == 1);
}

TEST_CASE("desktop approval: a question that expires while waiting behind another is denied without being shown", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;  // the first question's window waits until it expires
    Broker b(ui, desk(), fast(300ms));
    auto first = std::async(std::launch::async, [&] { return b.decide(prompt("first")); });
    REQUIRE(eventually([&] { return ui->open_windows.load() == 1; }));
    std::this_thread::sleep_for(100ms);
    auto second = std::async(std::launch::async, [&] { return b.decide(prompt("second")); });
    CHECK(first.get().reason == "expired");
    const Decision d = second.get();
    CHECK_FALSE(d.approved);
    // it had 300 ms from its own start: shown for the rest, or expired in the queue; never two windows at once
    CHECK((d.reason == "expired" || d.reason == "expired_in_queue"));
    CHECK(ui->max_open_windows.load() == 1);
}

TEST_CASE("desktop approval: libffl's Approver asks the broker", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Approve;
    auto approver = make_approver(std::make_shared<Broker>(ui, desk(), fast()));
    CHECK(approver->approve_change(prompt()));
    ui->window_answer = Answer::Deny;
    CHECK_FALSE(approver->approve_change(prompt()));
    // the text question of an older agent: one step, the text
    ui->window_answer = Answer::Approve;
    CHECK(approver->approve("An AI assistant wants to change data"));
    CHECK(ui->windows.back().steps == std::vector<std::string>{"An AI assistant wants to change data"});
}

TEST_CASE("desktop approval: libffl's decide gets the broker's reason", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Approve;
    ui->window_remember = true;
    auto unlocked = std::make_shared<std::atomic<bool>>(true);
    auto standing = std::make_shared<Standing>();
    auto approver = make_approver(std::make_shared<Broker>(ui, desk(unlocked, standing), fast(200ms)));
    auto r = approver->decide(prompt());
    CHECK(r.approved);
    CHECK(r.reason == "approved");
    r = approver->decide(prompt());  // "Don't ask again" turned on "Allow changes in SAP"
    CHECK(r.approved);
    CHECK(r.reason == "standing");
    standing->set(false);
    auto other = prompt();
    other.user = "someone@example.com";
    ui->window_remember = false;
    ui->window_answer = Answer::Deny;
    r = approver->decide(other);
    CHECK_FALSE(r.approved);
    CHECK(r.reason == "denied");
    ui->window_answer = Answer::None;  // nobody answers
    r = approver->decide(other);
    CHECK_FALSE(r.approved);
    CHECK(r.reason == "expired");
    *unlocked = false;
    r = approver->decide(other);
    CHECK_FALSE(r.approved);
    CHECK(r.reason == "locked");
}

TEST_CASE("desktop approval: the question waits the prompt's answer_s when that is shorter", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;  // the window waits until stopped
    Broker b(ui, desk(), fast(10s));
    auto p = prompt();
    p.answer_s = 1;
    const auto start = std::chrono::steady_clock::now();
    const Decision d = b.decide(p);
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(d.reason == "expired");
    CHECK(took >= 900ms);
    CHECK(took < 5s);
}

TEST_CASE("desktop approval: every change asks; a yes approves only that change", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Approve;
    Broker b(ui, desk(), fast());
    CHECK(b.decide(prompt()).approved);
    CHECK(b.decide(prompt()).approved);
    CHECK(ui->windows.size() == 2);  // asked twice
}

TEST_CASE("desktop approval: a yes in the window after the time is up is not a yes, and remembers nothing", "[link][approval]") {
    // review of #34: the window notices the deadline only at its next timer tick; an Approve in between came through
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Approve;
    ui->window_remember = true;
    ui->window_delay = 250ms;  // answered after the 100 ms deadline, before the window noticed
    ui->ignore_stop = true;
    auto standing = std::make_shared<Standing>();
    Broker b(ui, desk(std::make_shared<std::atomic<bool>>(true), standing), fast(100ms));
    const Decision d = b.decide(prompt());
    CHECK_FALSE(d.approved);
    CHECK(d.reason == "expired");
    CHECK_FALSE(standing->on());
    // nothing was remembered: the next change is asked
    ui->window_delay = 0ms;
    ui->ignore_stop = false;
    ui->window_remember = false;
    ui->window_answer = Answer::Deny;
    CHECK(b.decide(prompt()).reason == "denied");
    CHECK(ui->windows.size() == 2);
}

TEST_CASE("desktop approval: the notification's Deny before the window's yes denies, and turns nothing on", "[link][approval]") {
    // review of #36: the window notices the notification's answer only at its next timer tick; a yes in between came through
    auto ui = std::make_shared<FakeUi>();
    ui->window_answer = Answer::Approve;
    ui->window_remember = true;
    ui->window_delay = 150ms;
    ui->ignore_stop = true;
    Broker b(ui, desk(), fast());
    auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
    REQUIRE(eventually([&] { return ui->notified_count() == 1; }));
    const Request r = ui->last_notified();
    b.answer(r.id, r.nonce, Answer::Details);
    REQUIRE(eventually([&] { return ui->open_windows.load() == 1; }));
    b.answer(r.id, r.nonce, Answer::Deny);  // before the click arrives
    const Decision d = f.get();
    CHECK_FALSE(d.approved);
    CHECK(d.reason == "denied");
    CHECK(ui->withdrawn.size() == 1);
    // nothing was remembered: the next change is asked
    ui->can_notify = false;
    ui->window_delay = 0ms;
    ui->ignore_stop = false;
    ui->window_remember = false;
    ui->window_answer = Answer::Deny;
    CHECK(b.decide(prompt()).reason == "denied");
    CHECK(ui->windows.size() == 2);
}

TEST_CASE("desktop approval: the notification's Approve before the window's yes approves, and turns nothing on", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->window_answer = Answer::Approve;
    ui->window_remember = true;
    ui->window_delay = 150ms;
    ui->ignore_stop = true;
    Broker b(ui, desk(), fast());
    auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
    REQUIRE(eventually([&] { return ui->notified_count() == 1; }));
    const Request r = ui->last_notified();
    b.answer(r.id, r.nonce, Answer::Details);
    REQUIRE(eventually([&] { return ui->open_windows.load() == 1; }));
    b.answer(r.id, r.nonce, Answer::Approve);
    const Decision d = f.get();
    CHECK(d.approved);
    CHECK(d.reason == "approved");
    ui->can_notify = false;
    ui->window_delay = 0ms;
    ui->ignore_stop = false;
    ui->window_remember = false;
    ui->window_answer = Answer::Deny;
    CHECK(b.decide(prompt()).reason == "denied");  // asked again: the notification's Approve never remembers
    CHECK(ui->windows.size() == 2);
}

TEST_CASE("desktop approval: a yes in the window after the screen locked is not a yes", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Approve;
    ui->window_remember = true;
    ui->window_delay = 150ms;
    ui->ignore_stop = true;
    auto unlocked = std::make_shared<std::atomic<bool>>(true);
    auto standing = std::make_shared<Standing>();
    Broker b(ui, desk(unlocked, standing), fast());
    auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
    REQUIRE(eventually([&] { return ui->open_windows.load() == 1; }));
    *unlocked = false;  // locked before the click arrives
    const Decision d = f.get();
    CHECK_FALSE(d.approved);
    CHECK(d.reason == "locked");
    CHECK_FALSE(standing->on());
    *unlocked = true;
    ui->window_delay = 0ms;
    ui->ignore_stop = false;
    ui->window_remember = false;
    ui->window_answer = Answer::Deny;
    CHECK(b.decide(prompt()).reason == "denied");  // asked again: nothing remembered
    CHECK(ui->windows.size() == 2);
}

TEST_CASE("desktop approval: libffl's decide through the notification", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();  // can_notify: the toast path
    auto broker = std::make_shared<Broker>(ui, desk(), fast(300ms));
    auto approver = make_approver(broker);
    // the toast's buttons: answered through the button text, as the toast activator delivers it
    auto via_toast = [&](Answer a) {
        const size_t before = ui->notified_count();
        auto f = std::async(std::launch::async, [&] { return approver->decide(prompt()); });
        REQUIRE(eventually([&] { return ui->notified_count() == before + 1; }));
        if (a != Answer::None) broker->answer_text(answer_text(a, ui->last_notified()));
        return f.get();
    };
    auto r = via_toast(Answer::Approve);
    CHECK(r.approved);
    CHECK(r.reason == "approved");
    r = via_toast(Answer::Approve);  // the notification's Approve never remembers: asked again
    CHECK(r.reason == "approved");
    CHECK(ui->notified_count() == 2);
    r = via_toast(Answer::Deny);
    CHECK_FALSE(r.approved);
    CHECK(r.reason == "denied");
    r = via_toast(Answer::None);  // nobody answers
    CHECK_FALSE(r.approved);
    CHECK(r.reason == "expired");
    CHECK(ui->windows.empty());
    CHECK(ui->withdrawn.size() == 4);
}

TEST_CASE("desktop approval: libffl's decide reports a question that expired in the queue as expired", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;  // the first question's window waits until it expires
    auto broker = std::make_shared<Broker>(ui, desk(), fast(3s));
    auto approver = make_approver(broker);
    auto first = std::async(std::launch::async, [&] { return broker->decide(prompt("first")); });
    REQUIRE(eventually([&] { return ui->open_windows.load() == 1; }));
    auto p = prompt("second");
    p.answer_s = 1;  // expires behind the first, never shown
    const Decision queued = broker->decide(p);
    CHECK(queued.reason == "expired_in_queue");  // what the broker says ...
    const auto r = approver->decide(p);
    CHECK_FALSE(r.approved);
    CHECK(r.reason == "expired");  // ... and what libffl hears (the gateway's vocabulary)
    CHECK(first.get().reason == "expired");
    CHECK(ui->windows.size() == 1);
}

TEST_CASE("desktop approval: \"Don't ask again\" with Deny turns nothing on", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Deny;
    ui->window_remember = true;
    auto standing = std::make_shared<Standing>();
    Broker b(ui, desk(std::make_shared<std::atomic<bool>>(true), standing), fast());
    CHECK_FALSE(b.decide(prompt()).approved);
    CHECK_FALSE(standing->on());
    ui->window_answer = Answer::Approve;
    ui->window_remember = false;
    CHECK(b.decide(prompt()).approved);
    CHECK(ui->windows.size() == 2);
}

TEST_CASE("desktop approval: the notification's Approve never turns on \"Allow changes in SAP\"", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    Broker b(ui, desk(), fast());
    for (int i = 0; i < 2; i++) {
        auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
        REQUIRE(eventually([&] { return ui->notified_count() == size_t(i + 1); }));
        const Request r = ui->last_notified();
        b.answer(r.id, r.nonce, Answer::Approve);
        CHECK(f.get().reason == "approved");
    }
}

TEST_CASE("desktop approval: the assistant's name, not its URL", "[link][approval]") {
    CHECK(client_name("https://claude.ai/oauth/") == "Claude");
    CHECK(client_name("https://claude.ai") == "Claude");
    CHECK(client_name("claude-code") == "Claude Code");
    CHECK(client_name("Claude Desktop") == "Claude Desktop");
    CHECK(client_name("https://chatgpt.com/connector") == "ChatGPT");
    CHECK(client_name("https://www.example.org/mcp") == "example.org");
    CHECK(client_name("") == "an AI assistant");
    CHECK(client_name("unknown client") == "an AI assistant");
    // the request: the name for the headline, the full identifier for the window
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Deny;
    Broker b(ui, desk(), fast());
    auto p = prompt();
    p.client = "https://claude.ai/oauth/";
    b.decide(p);
    REQUIRE(ui->windows.size() == 1);
    CHECK(ui->windows[0].client == "Claude");
    CHECK(ui->windows[0].client_id == "https://claude.ai/oauth/");
}

TEST_CASE("desktop approval: the standard toolbar's buttons are named in the steps", "[link][approval]") {
    CHECK(readable_step("click wnd[0]/tbar[0]/btn[11]") == "click Save (wnd[0]/tbar[0]/btn[11])");
    CHECK(readable_step("click wnd[0]/tbar[0]/btn[3]") == "click Back (wnd[0]/tbar[0]/btn[3])");
    CHECK(readable_step("run 2 steps: 1. press enter; 2. click wnd[0]/tbar[0]/btn[11]; 3. click wnd[1]/tbar[0]/btn[0]") ==
          "run 2 steps: 1. press Enter; 2. click Save (wnd[0]/tbar[0]/btn[11]); 3. click Enter (wnd[1]/tbar[0]/btn[0])");
    CHECK(readable_step("click wnd[0]/tbar[0]/btn[110]") == "click wnd[0]/tbar[0]/btn[110]");
    CHECK(readable_step("click wnd[0]/tbar[1]/btn[8]") == "click wnd[0]/tbar[1]/btn[8]");  // application toolbar: varies
}

TEST_CASE("desktop approval: keys are named as people know them", "[link][approval]") {
    CHECK(readable_step("press 11") == "press Save (Ctrl+S)");
    CHECK(readable_step("press enter") == "press Enter");
    CHECK(readable_step("press 0") == "press Enter");
    CHECK(readable_step("press f3") == "press Back (F3)");
    CHECK(readable_step("press F8") == "press Execute (F8)");
    CHECK(readable_step("press 12") == "press Cancel (F12)");
    CHECK(readable_step("press 15") == "press Exit (Shift+F3)");
    CHECK(readable_step("press shift+f4") == "press Shift+F4");
    CHECK(readable_step("press 26") == "press Ctrl+F2");
    CHECK(readable_step("press pagedown") == "press Page Down");
    CHECK(readable_step("fill fields ctxtA, ctxtB, then press f8") == "fill fields ctxtA, ctxtB, then press Execute (F8)");
    CHECK(readable_step("run 2 steps: 1. press 11; 2. click wnd[0]/tbar[0]/btn[3]") ==
          "run 2 steps: 1. press Save (Ctrl+S); 2. click Back (wnd[0]/tbar[0]/btn[3])");
    CHECK(readable_step("press nonsense") == "press nonsense");  // not a key: as it is
}

// ---- approval model v2: "Allow changes in SAP" is the standing approval at this desktop ----

TEST_CASE("standing approval: saved for the user, off by default, refused while the organisation enforces asking", "[link][approval]") {
    Saved saved;
    auto s = saved.standing();
    CHECK_FALSE(s->on());  // nothing saved: off
    CHECK(s->set(true));
    CHECK(s->on());
    CHECK(saved.value == "1");
    CHECK(saved.standing()->on());  // the next start (and the next version) reads it back
    CHECK(s->set(false));
    CHECK(saved.value == "0");
    CHECK_FALSE(saved.standing()->on());

    s->set(true);
    s->set_enforced(true);  // "Always ask at the desktop"
    CHECK(s->enforced());
    CHECK_FALSE(s->on());   // counts as off ...
    CHECK(s->saved());      // ... the person's choice is kept for when the organisation lets them decide again
    CHECK_FALSE(s->set(true));
    s->set_enforced(false);
    CHECK(s->on());
    Standing memory_only;  // no store (tests, a console run): in memory
    CHECK(memory_only.set(true));
    CHECK(memory_only.on());
}

TEST_CASE("standing approval: the tray's switch is off with FAIRYFLY_READ_ONLY in any spelling run_mcp accepts", "[link][approval]") {
    // Codex review: the link read only "1", "true", "TRUE", "yes", "on", while the dispatcher lower-cases the value
    for (const char* v : {"ON", "Yes", "True", "1"}) {
        _putenv_s("FAIRYFLY_READ_ONLY", v);
        INFO(v);
        CHECK(fairyfly::link::make_write_mode_control(false)->locked());
    }
    _putenv_s("FAIRYFLY_READ_ONLY", "");
    CHECK_FALSE(fairyfly::link::make_write_mode_control(false)->locked());
    CHECK(fairyfly::link::make_write_mode_control(true)->locked());  // link run --read-only
}

TEST_CASE("standing approval: every process sees the saved switch at once (a console link and the tray)", "[link][approval]") {
    // Codex review: the switch was read once per process, so a console link kept running changes without asking
    // after the tray turned it off
    Saved saved;
    auto console_link = saved.standing();
    auto tray = saved.standing();
    CHECK(tray->set(true));
    CHECK(console_link->on());
    CHECK(tray->set(false));
    CHECK_FALSE(console_link->on());
    CHECK_FALSE(console_link->saved());
}

TEST_CASE("desktop approval: a cloud change gets write rights only for the call this desktop approved", "[link][approval]") {
    // Codex review: libffl skips the approver for a grant with approval "none"; the desktop must not trust that.
    // A yes leaves a permit for exactly that call (tool and summary), on the thread that asked, used once.
    auto key = [](const std::string& k) { return nlohmann::json{{"key", k}}; };
    CHECK_FALSE(take_permit("gui_key_send", key("11")));  // nobody approved anything
    ffy::link::ApprovalPrompt p = prompt("press 11");
    p.tool = "gui_key_send";
    grant_permit(p);
    CHECK_FALSE(take_permit("gui_key_send", key("12")));  // another call
    CHECK_FALSE(std::async(std::launch::async, [&] { return take_permit("gui_key_send", key("11")); }).get());  // another thread
    CHECK(take_permit("gui_key_send", key("11")));
    CHECK_FALSE(take_permit("gui_key_send", key("11")));  // once

    // the broker's yes (also the standing approval) leaves the permit; a no leaves none
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Approve;
    auto standing = std::make_shared<Standing>();
    auto approver = make_approver(std::make_shared<Broker>(ui, desk(std::make_shared<std::atomic<bool>>(true), standing), fast()));
    CHECK(approver->decide(p).approved);
    CHECK(take_permit("gui_key_send", key("11")));
    ui->window_answer = Answer::Deny;
    CHECK_FALSE(approver->decide(p).approved);
    CHECK_FALSE(take_permit("gui_key_send", key("11")));
    standing->set(true);
    CHECK(approver->decide(p).reason == "standing");
    CHECK(take_permit("gui_key_send", key("11")));
}

TEST_CASE("desktop approval: with \"Allow changes in SAP\" on, a change runs without asking, also on a locked screen", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    auto unlocked = std::make_shared<std::atomic<bool>>(true);
    auto standing = std::make_shared<Standing>();
    standing->set(true);
    auto approver = make_approver(std::make_shared<Broker>(ui, desk(unlocked, standing), fast()));
    auto r = approver->decide(prompt());
    CHECK(r.approved);
    CHECK(r.reason == "standing");
    *unlocked = false;  // the person turned it on: it stands while they are away
    r = approver->decide(prompt());
    CHECK(r.approved);
    CHECK(r.reason == "standing");
    CHECK(ui->notified.empty());
    CHECK(ui->windows.empty());
}

TEST_CASE("desktop approval: \"Don't ask again\" approves this change and turns on \"Allow changes in SAP\"", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Approve;
    ui->window_remember = true;
    Saved saved;
    auto standing = saved.standing();
    Broker b(ui, desk(std::make_shared<std::atomic<bool>>(true), standing), fast());
    const Decision d = b.decide(prompt());
    CHECK(d.approved);
    CHECK(d.reason == "approved");
    REQUIRE(ui->windows.size() == 1);
    CHECK(ui->windows[0].offer_standing);
    CHECK(standing->on());
    CHECK(saved.value == "1");  // saved: it stays on after a restart
    auto other = prompt();       // for every assistant and account at this desktop
    other.user = "someone@example.com";
    other.client = "https://chatgpt.com/";
    CHECK(b.decide(other).reason == "standing");
    CHECK(ui->windows.size() == 1);
}

TEST_CASE("desktop approval: when the organisation enforces asking, every change asks and nothing turns it off", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Approve;
    ui->window_remember = true;  // a stale tick cannot turn anything on
    auto standing = std::make_shared<Standing>();
    standing->set(true);
    standing->set_enforced(true);
    Broker b(ui, desk(std::make_shared<std::atomic<bool>>(true), standing), fast());
    CHECK(b.decide(prompt()).reason == "approved");
    CHECK(b.decide(prompt()).reason == "approved");
    REQUIRE(ui->windows.size() == 2);
    CHECK_FALSE(ui->windows[0].offer_standing);  // no "Don't ask again", the window says why
    CHECK_FALSE(standing->on());
}

TEST_CASE("desktop approval: a change the administrator requires asked for asks with the switch on, and offers no \"Don't ask again\"", "[link][approval]") {
    // libffl: the call was signed "always", or the push said so (ApprovalPrompt::enforced); it refuses "standing" then
    auto ui = std::make_shared<FakeUi>();
    ui->can_notify = false;
    ui->window_answer = Answer::Approve;
    ui->window_remember = true;
    Saved saved;
    auto standing = saved.standing();
    REQUIRE(standing->set(false));
    Broker b(ui, desk(std::make_shared<std::atomic<bool>>(true), standing), fast());
    auto p = prompt();
    p.enforced = true;
    CHECK(b.decide(p).reason == "approved");
    REQUIRE(ui->windows.size() == 1);
    CHECK_FALSE(ui->windows[0].offer_standing);
    CHECK_FALSE(standing->on());  // the tick turned nothing on
    REQUIRE(standing->set(true));
    CHECK(b.decide(p).reason == "approved");  // asked, not "standing"
    CHECK(ui->windows.size() == 2);
    CHECK(b.decide(prompt()).reason == "standing");
}

TEST_CASE("desktop approval: libffl's approval_enforced greys out the switch until the administrator lets the user decide", "[link][approval]") {
    auto standing = std::make_shared<Standing>();
    REQUIRE(standing->set(true));
    auto approver = make_approver(std::make_shared<Broker>(std::make_shared<FakeUi>(), desk(std::make_shared<std::atomic<bool>>(true), standing), fast()));
    approver->approval_enforced(true);
    CHECK(standing->enforced());
    CHECK_FALSE(standing->on());
    CHECK(standing->saved());
    approver->approval_enforced(false);
    CHECK_FALSE(standing->enforced());
    CHECK(standing->on());
}

TEST_CASE("desktop approval: the administrator's push while a notification waits takes \"Don't ask again\" out of its window", "[link][approval]") {
    // Codex review of #39: the question was built before the push; Details opened the window with the checkbox
    auto ui = std::make_shared<FakeUi>();
    ui->window_answer = Answer::Approve;
    ui->window_remember = true;
    auto standing = std::make_shared<Standing>();
    Broker b(ui, desk(std::make_shared<std::atomic<bool>>(true), standing), fast());
    auto f = std::async(std::launch::async, [&] { return b.decide(prompt()); });
    REQUIRE(eventually([&] { return ui->notified_count() == 1; }));
    const Request r = ui->last_notified();
    CHECK(r.offer_standing);
    b.approval_enforced(true);
    b.answer(r.id, r.nonce, Answer::Details);
    CHECK(f.get().reason == "approved");
    REQUIRE(ui->windows.size() == 1);
    CHECK_FALSE(ui->windows[0].offer_standing);
    CHECK_FALSE(standing->saved());
}

TEST_CASE("desktop approval: a change that cannot run here (fairyfly read-only) is never asked", "[link][approval]") {
    auto ui = std::make_shared<FakeUi>();
    Desk d = desk();
    d.can_change = [] { return false; };  // --read-only / FAIRYFLY_READ_ONLY: fairyfly refuses it with its own message
    Broker b(ui, d, fast());
    const Decision r = b.decide(prompt());
    CHECK(r.reason == "not_asked");
    CHECK(ui->notified.empty());
    CHECK(ui->windows.empty());
    // nobody approved it: no permit for the call (Codex review of #39)
    auto approver = make_approver(std::make_shared<Broker>(ui, d, fast()));
    ffy::link::ApprovalPrompt p = prompt("press 11");
    p.tool = "gui_key_send";
    CHECK(approver->decide(p).approved);
    CHECK_FALSE(take_permit("gui_key_send", nlohmann::json{{"key", "11"}}));
}

TEST_CASE("desktop approval: notification answers round-trip", "[link][approval]") {
    Request r;
    r.id = 42;
    r.nonce = "abc123";
    for (Answer a : {Answer::Approve, Answer::Deny, Answer::Details}) {
        auto p = parse_answer(answer_text(a, r));
        REQUIRE(p);
        CHECK(p->answer == a);
        CHECK(p->id == 42);
        CHECK(p->nonce == "abc123");
    }
    CHECK_FALSE(parse_answer(""));
    CHECK_FALSE(parse_answer("approve"));
    CHECK_FALSE(parse_answer("approve:x:abc"));
    CHECK_FALSE(parse_answer("launch:1:abc"));
    CHECK_FALSE(parse_answer("approve:1:"));
}

TEST_CASE("link runtime: the tray's write switch acts on the runtime's standing approval", "[link][approval][runtime]") {
    // review 2026-10-25: the switch always used the process's HKCU-backed approval, so it could not be tested
    fairyfly::link::LinkRuntime runtime(std::make_shared<fairyfly::link::approval::Standing>());  // in memory
    auto writes = fairyfly::link::make_write_mode_control(false, runtime);
    CHECK_FALSE(writes->allowed());
    CHECK(writes->set_allowed(true));
    CHECK(writes->allowed());
    CHECK(runtime.standing->on());
    CHECK(writes->set_allowed(false));
    CHECK_FALSE(runtime.standing->on());
    runtime.standing->set_enforced(true);  // "Always ask at the desktop"
    CHECK(writes->enforced());
    CHECK_FALSE(writes->set_allowed(true));
    // a --read-only link never turns it on, but can turn it off
    runtime.standing->set_enforced(false);
    auto locked = fairyfly::link::make_write_mode_control(true, runtime);
    CHECK_FALSE(locked->set_allowed(true));
    CHECK_FALSE(runtime.standing->on());
    CHECK(locked->set_allowed(false));
}

TEST_CASE("link runtime: stop requests and background threads belong to one runtime", "[link][runtime]") {
    fairyfly::link::LinkRuntime runtime(std::make_shared<fairyfly::link::approval::Standing>());
    CHECK_FALSE(runtime.stop);
    runtime.request_stop();
    CHECK(runtime.stop);
    CHECK(runtime.background_threads() == 0);
    {
        const auto guard = runtime.background_thread();
        CHECK(runtime.background_threads() == 1);
    }
    CHECK(runtime.background_threads() == 0);
    // the process runtime is the one Ctrl+C, the tray's Quit and main()'s exit check use
    CHECK(&fairyfly::link::LinkRuntime::process() == &fairyfly::link::LinkRuntime::process());
}
