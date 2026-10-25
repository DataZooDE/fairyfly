// The Windows UI of the desktop approval (src/link/desktop_approval_win32.cpp): what the toast and the window show,
// and that the window closes by itself when its time is up. The [.][approval_manual] cases show the real toast and
// window on an interactive desktop and wait for a person; never run them on a build agent:
//   unit_tests.exe "[approval_manual]"
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "include/link/desktop_approval_win32.h"
#include "include/tray/tray_win32.h"

using namespace fairyfly::link::approval;
using namespace std::chrono_literals;

namespace {

Request sample() {
    Request r;
    r.id = 7;
    r.nonce = "n0nce";
    r.client = "Claude <Code> & co";
    r.client_id = "https://claude.ai/oauth/";
    r.user = "jr@example.com";
    r.tool = "gui_batch";
    r.summary = "run 2 steps: 1. fill fields ctxtVBAK-AUART, then press enter; 2. click wnd[0]/tbar[0]/btn[11]";
    r.steps = {"fill fields ctxtVBAK-AUART, then press enter", "click wnd[0]/tbar[0]/btn[11]"};
    r.sap = {"BIG", "100", "VA02", "JR"};
    r.deadline = std::chrono::steady_clock::now() + 90s;
    r.expires_at = std::chrono::system_clock::now() + 90s;
    return r;
}

bool contains(const std::wstring& s, const std::wstring& part) { return s.find(part) != std::wstring::npos; }

}  // namespace

TEST_CASE("desktop approval toast: headline, transaction, three answers with the nonce, escaped", "[link][approval]") {
    const std::wstring x = toast_xml(sample());
    CHECK(contains(x, L"Claude &lt;Code&gt; &amp; co wants to change SAP"));
    CHECK(contains(x, L"VA02 – run 2 steps"));
    CHECK(contains(x, L"for jr@example.com"));
    CHECK(contains(x, L"arguments=\"approve:7:n0nce\""));
    CHECK(contains(x, L"arguments=\"deny:7:n0nce\""));
    CHECK(contains(x, L"arguments=\"details:7:n0nce\""));
    CHECK(contains(x, L"launch=\"details:7:n0nce\""));  // a click on the toast opens the window
    CHECK(contains(x, L"scenario=\"reminder\""));       // stays on screen until answered
    CHECK_FALSE(contains(x, L"<Code>"));
}

TEST_CASE("desktop approval window: who, where, and every step numbered", "[link][approval]") {
    const std::wstring t = window_text(sample());
    CHECK(contains(t, L"Assistant: Claude <Code> & co (https://claude.ai/oauth/)"));
    CHECK(contains(t, L"Acting for: jr@example.com"));
    CHECK(contains(t, L"SAP: system BIG, client 100, transaction VA02, user JR"));
    CHECK(contains(t, L"1. fill fields ctxtVBAK-AUART, then press enter\n2. click wnd[0]/tbar[0]/btn[11]"));
    Request r = sample();
    r.sap = {};
    CHECK(contains(window_text(r), L"SAP: the session fairyfly works in"));
}

TEST_CASE("desktop approval window: \"Don't ask again\" turns on Allow changes in SAP, unless the administrator requires approval", "[link][approval]") {
    Request r = sample();
    CHECK(contains(dont_ask_text(r), L"Don't ask again"));
    CHECK(contains(dont_ask_text(r), L"Allow changes in SAP"));
    CHECK_FALSE(contains(window_text(r), L"administrator"));
    r.offer_standing = false;
    CHECK(dont_ask_text(r).empty());  // no checkbox
    CHECK(contains(window_text(r), L"Approval is required by your administrator"));
}

TEST_CASE("standing approval: saved in the real tray settings (HKCU) and read back by a new process' Standing", "[link][approval][win32]") {
    // the same key as "Allow changes in SAP" (link_allow_changes), under a value of its own that the test removes
    const char* name = "link_allow_changes_unit_test";
    std::shared_ptr<fairyfly::tray::TraySettings> settings = fairyfly::tray::make_win32_tray_settings();
    auto open = [&] {
        return std::make_shared<Standing>([&] { return settings->get(name); }, [&](const std::string& v) { return settings->set(name, v); });
    };
    CHECK_FALSE(open()->on());
    CHECK(open()->set(true));
    CHECK(settings->get(name) == std::optional<std::string>("1"));
    CHECK(open()->on());  // a restart (or the next version) reads it back
    CHECK(open()->set(false));
    CHECK_FALSE(open()->on());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\DataZoo\\fairyfly\\tray", L"link_allow_changes_unit_test");
    CHECK_FALSE(settings->get(name).has_value());
}

TEST_CASE("desktop approval window: closes by itself when stopped (expired), nothing left on screen", "[link][approval][win32]") {
    auto ui = make_win32_ui([](const std::string&) {});
    Request r = sample();
    r.deadline = std::chrono::steady_clock::now() + 400ms;
    const auto start = std::chrono::steady_clock::now();
    bool remember = true;
    const Answer a = ui->ask(r, [&] { return std::chrono::steady_clock::now() >= r.deadline; }, remember);
    CHECK_FALSE(remember);
    CHECK(a == Answer::None);
    CHECK(std::chrono::steady_clock::now() - start < 5s);
    CHECK(FindWindowW(nullptr, L"fairyfly – approve a change in SAP") == nullptr);
}

TEST_CASE("manual: approve a change through the toast or the window (90 s)", "[.][approval_manual]") {
    Desk d;
    d.sap = [] { return SapContext{"BIG", "100", "VA02", "JR"}; };
    auto approver = make_desktop_approver(std::move(d));
    ffy::link::ApprovalPrompt p;
    p.client = "Claude";
    p.user = "jr@example.com";
    p.tool = "gui_batch";
    p.steps = {"fill fields ctxtVBAK-AUART, ctxtVBAK-VKORG, then press enter", "click wnd[0]/tbar[0]/btn[11]"};
    p.summary = "run 2 steps: 1. " + p.steps[0] + "; 2. " + p.steps[1];
    p.window_s = 300;
    std::cout << "desktop_can_ask: " << desktop_can_ask() << "\nanswer the toast (or Details -> window)..." << std::endl;
    const bool approved = approver->approve_change(p);
    std::cout << "approved: " << approved << std::endl;
}

TEST_CASE("manual: two questions at once are asked one after another", "[.][approval_manual][queue]") {
    auto approver = make_desktop_approver(Desk{});
    ffy::link::ApprovalPrompt p;
    p.client = "Claude";
    p.user = "jr@example.com";
    p.tool = "gui_key_send";
    p.summary = "press enter";
    p.steps = {"press enter"};
    std::thread second([&] {
        std::this_thread::sleep_for(500ms);
        auto q = p;
        q.summary = q.steps[0] = "press f8";
        std::cout << "second: " << approver->approve_change(q) << std::endl;
    });
    std::cout << "first: " << approver->approve_change(p) << std::endl;
    second.join();
}

TEST_CASE("manual: the approval window for 20 s (screenshots)", "[.][approval_manual][window]") {
    auto ui = make_win32_ui([](const std::string&) {});
    Request r = sample();
    r.client = "Claude";
    r.deadline = std::chrono::steady_clock::now() + 20s;
    bool remember = false;
    const Answer a = ui->ask(r, [&] { return std::chrono::steady_clock::now() >= r.deadline; }, remember);
    std::cout << "answer " << int(a) << " remember " << remember << std::endl;
}

TEST_CASE("manual: the approval window when the administrator requires approval, 20 s (screenshots)", "[.][approval_manual][window_enforced]") {
    auto ui = make_win32_ui([](const std::string&) {});
    Request r = sample();
    r.client = "Claude";
    r.offer_standing = false;
    r.deadline = std::chrono::steady_clock::now() + 20s;
    bool remember = false;
    const Answer a = ui->ask(r, [&] { return std::chrono::steady_clock::now() >= r.deadline; }, remember);
    std::cout << "answer " << int(a) << " remember " << remember << std::endl;
}
