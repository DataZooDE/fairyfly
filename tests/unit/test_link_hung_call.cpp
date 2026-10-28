// The desktop link when SAP GUI stops answering scripting (live 2026-10-27: gui_screen_read on SU01 never returned,
// then gui_doctor and gui_session_list waited 126 s each behind it and only said "fairyfly is busy"). The main (COM)
// thread is held by a call that blocks on a latch this test controls; nothing touches SAP.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <regex>
#include <string>
#include <thread>

#include "include/link/main_thread_tools.h"
#include "support/eventually.h"

using fairyfly::link::MainThreadTools;
using fairyfly::test::eventually;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

/// Holds a call until release(): SAP GUI that does not answer.
struct Latch {
    std::mutex mu;
    std::condition_variable cv;
    bool open = false;
    void wait() {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [&] { return open; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> l(mu);
            open = true;
        }
        cv.notify_all();
    }
};

/// A steady clock the test moves by hand (no real 60 s waits).
struct FakeClock {
    std::atomic<std::chrono::steady_clock::rep> ticks{std::chrono::steady_clock::now().time_since_epoch().count()};
    std::chrono::steady_clock::time_point now() const {
        return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(ticks.load()));
    }
    void advance(std::chrono::steady_clock::duration d) { ticks += d.count(); }
};

/// The link's main thread with one SAP call (gui_screen_read) that hangs until the latch opens; every other tool
/// answers at once with its name.
struct HungLink {
    MainThreadTools tools;
    FakeClock clock;
    Latch sap;
    std::atomic<int> ran_doctor{0}, ran_session_list{0}, ran_other{0}, ran_fill{0};
    std::atomic<bool> stop{false};
    std::thread main;
    std::thread hung_caller;
    json hung_result;

    HungLink() {
        tools.set_clock([this] { return clock.now(); });
        tools.set_change_tools({"gui_element_fill", "gui_element_click", "gui_batch"});
        tools.set_handlers([] { return json::array(); },
                           [this](const std::string& name, const json&, const ffy::link::CallInfo&) {
                               if (name == "gui_screen_read") sap.wait();
                               else if (name == "gui_doctor") ++ran_doctor;
                               else if (name == "gui_session_list") ++ran_session_list;
                               else if (name == "gui_element_fill") ++ran_fill;
                               else ++ran_other;
                               return json{{"content", json::array({{{"type", "text"}, {"text", name}}})}, {"isError", false}};
                           });
        main = std::thread([this] { tools.pump(stop); });  // plays fairyfly's main (COM) thread
    }
    /// Starts the hanging gui_screen_read on connection 0 and waits until the main thread runs it.
    void hang() {
        hung_caller = std::thread([this] {
            hung_result = tools.rpc("tools/call", {{"name", "gui_screen_read"}, {"arguments", {{"connection", 0}, {"tab", "tabpLOGO"}}}},
                                    30000, {});
        });
        REQUIRE(eventually([&] { return tools.in_flight().has_value(); }));
    }
    ~HungLink() {
        sap.release();
        if (hung_caller.joinable()) hung_caller.join();
        stop = true;
        main.join();
        tools.shutdown();
    }
};

json call(MainThreadTools& tools, const std::string& name, int timeout_ms = 30000) {
    return tools.rpc("tools/call", {{"name", name}, {"arguments", json::object()}}, timeout_ms, {});
}

std::string text_of(const json& rpc) { return rpc["result"]["content"][0]["text"].get<std::string>(); }

}  // namespace

TEST_CASE("hung SAP call: gui_doctor and gui_session_list answer at once with SAP_GUI_NOT_RESPONDING", "[link][hung]") {
    HungLink link;
    link.hang();
    link.clock.advance(183s);
    for (const std::string tool : {"gui_doctor", "gui_session_list"}) {
        const auto t0 = std::chrono::steady_clock::now();
        const json r = call(link.tools, tool);
        CHECK(std::chrono::steady_clock::now() - t0 < 2s);  // not 126 s behind the hung call
        const json& res = r["result"];
        CHECK(res["isError"] == true);
        const json& err = res["structuredContent"]["error"];
        CHECK(err["code"] == "SAP_GUI_NOT_RESPONDING");
        CHECK(err["running_tool"] == "gui_screen_read");
        CHECK(err["connection"] == "0");
        CHECK(err["elapsed_ms"].get<long long>() >= 183000);
        CHECK(std::regex_match(err["started"].get<std::string>(), std::regex("[0-9]{2}:[0-9]{2}:[0-9]{2}")));
        const std::string text = text_of(r);
        CHECK(text.rfind("SAP_GUI_NOT_RESPONDING: ", 0) == 0);
        CHECK(text.find("SAP GUI has not answered gui_screen_read on connection 0 for 183 s (started ") != std::string::npos);
        CHECK(text.find("dialog") != std::string::npos);        // the hint: a hidden SAP dialog
        CHECK(text.find("ending the SAP session") != std::string::npos);
        CHECK(text.find(tool) != std::string::npos);            // which call could not run
    }
    // neither ran later on the main thread behind the hung call
    link.sap.release();
    REQUIRE(eventually([&] { return !link.tools.in_flight().has_value(); }));
    link.hung_caller.join();
    CHECK(text_of(link.hung_result) == "gui_screen_read");
    CHECK(link.ran_doctor == 0);
    CHECK(link.ran_session_list == 0);
    // SAP answers again: the diagnostic tools run as usual
    CHECK(text_of(call(link.tools, "gui_doctor")) == "gui_doctor");
    CHECK(link.ran_doctor == 1);
}

TEST_CASE("hung SAP call: a diagnostic tool behind a call that only just started waits briefly, then diagnoses", "[link][hung]") {
    HungLink link;
    link.tools.set_diagnostic_wait(50ms);
    link.hang();   // the fake clock does not move: the call is 0 s old
    const auto t0 = std::chrono::steady_clock::now();
    const json r = call(link.tools, "gui_doctor");
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    CHECK(r["result"]["structuredContent"]["error"]["code"] == "SAP_GUI_NOT_RESPONDING");
    link.sap.release();
    link.hung_caller.join();
    CHECK(link.ran_doctor == 0);   // abandoned: it never runs late
}

TEST_CASE("hung SAP call: a diagnostic tool behind a quick call runs normally", "[link][hung]") {
    HungLink link;
    link.tools.set_diagnostic_wait(5s);
    link.hang();
    std::thread later([&] {
        std::this_thread::sleep_for(50ms);
        link.sap.release();   // SAP answers within the wait
    });
    const json r = call(link.tools, "gui_session_list");
    later.join();
    CHECK(text_of(r) == "gui_session_list");
    CHECK(link.ran_session_list == 1);
}

TEST_CASE("hung SAP call: another tool that did not start names the blocking call and its age", "[link][hung]") {
    HungLink link;
    link.hang();
    link.clock.advance(30s);   // not stuck for 60 s yet: it waits its time limit, then names the blocker
    try {
        call(link.tools, "gui_element_click", 1000);
        FAIL("expected the busy error");
    } catch (const std::exception& e) {
        const std::string msg = e.what();
        CHECK(msg.rfind("fairyfly is busy: the call did not start within 1 s", 0) == 0);
        CHECK(msg.find("SAP GUI has not answered gui_screen_read on connection 0 for 30 s (started ") != std::string::npos);
        CHECK(msg.find("ending the SAP session") != std::string::npos);
    }
    link.sap.release();
    link.hung_caller.join();
    CHECK(link.ran_other == 0);
}

TEST_CASE("hung SAP call: once it is stuck for 60 s, another tool fails at once instead of waiting", "[link][hung]") {
    // libffl serves only a few local calls at a time: callers waiting 126 s each would keep gui_doctor out as well
    HungLink link;
    link.hang();
    link.clock.advance(61s);
    const auto t0 = std::chrono::steady_clock::now();
    try {
        call(link.tools, "gui_screen_read", 30000);
        FAIL("expected the busy error");
    } catch (const std::exception& e) {
        const std::string msg = e.what();
        CHECK(msg.rfind("fairyfly is busy: the call did not start; SAP GUI has not answered gui_screen_read on connection 0 for 61 s", 0) == 0);
    }
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    link.sap.release();
    link.hung_caller.join();
    CHECK(text_of(call(link.tools, "gui_element_click")) == "gui_element_click");   // SAP answers again: as usual
}

TEST_CASE("hung SAP call: the hung call's own timeout says SAP did not answer, not that it did not start", "[link][hung]") {
    HungLink link;
    std::string msg;
    std::thread caller([&] {
        try {
            link.tools.rpc("tools/call", {{"name", "gui_screen_read"}, {"arguments", {{"connection", 0}}}}, 200, {});
        } catch (const std::exception& e) {
            msg = e.what();
        }
    });
    REQUIRE(eventually([&] { return link.tools.in_flight().has_value(); }));
    caller.join();
    CHECK(msg.rfind("SAP_GUI_NOT_RESPONDING: SAP GUI has not answered gui_screen_read on connection 0", 0) == 0);
    CHECK(msg.find("did not start") == std::string::npos);
}

TEST_CASE("hung SAP call: the status names it only after the threshold and clears when it returns", "[link][hung][tray]") {
    HungLink link;
    CHECK(fairyfly::link::stuck_call_status(link.tools.in_flight(), 60s).is_null());   // idle
    link.hang();
    auto in_flight = link.tools.in_flight();
    REQUIRE(in_flight);
    CHECK(in_flight->tool == "gui_screen_read");
    CHECK(in_flight->connection == "0");
    link.clock.advance(59s);
    CHECK(fairyfly::link::stuck_call_status(link.tools.in_flight(), 60s).is_null());
    link.clock.advance(2s);
    const json s = fairyfly::link::stuck_call_status(link.tools.in_flight(), 60s);
    REQUIRE(s.is_object());
    CHECK(s["tool"] == "gui_screen_read");
    CHECK(s["connection"] == "0");
    CHECK(s["elapsed_s"] == 61);
    link.sap.release();
    link.hung_caller.join();
    CHECK(fairyfly::link::stuck_call_status(link.tools.in_flight(), 60s).is_null());
}

TEST_CASE("hung SAP call: a change the gateway cancelled while it waited never runs when SAP answers again", "[link][hung]") {
    // hang at t=0, a fill queued at t=5 s, the gateway gives up at t=30 s, the dialog is closed at t=50 s: before,
    // the fill still ran in SAP although its caller had been told it failed
    HungLink link;
    link.hang();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    ffy::link::CallInfo info;
    info.write_granted = true;
    info.cancel = cancel;
    std::string error;
    std::thread fill([&] {
        try {
            link.tools.rpc("tools/call", {{"name", "gui_element_fill"}, {"arguments", {{"fields", json::object()}}}}, 30000, info);
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    std::this_thread::sleep_for(50ms);   // queued behind the hung call
    *cancel = true;                      // the gateway's deadline
    link.sap.release();                  // SAP answers again
    fill.join();
    link.hung_caller.join();
    CHECK(link.ran_fill == 0);
    CHECK(error.find("cancelled") != std::string::npos);
    // the main thread goes on with the next call
    CHECK(text_of(call(link.tools, "gui_doctor")) == "gui_doctor");
}

TEST_CASE("hung SAP call: a change whose caller gave up may still be applied, a read says nothing of that", "[link][hung]") {
    for (const std::string tool : {"gui_element_fill", "gui_screen_read"}) {
        MainThreadTools tools;
        tools.set_change_tools({"gui_element_fill"});
        Latch sap;
        tools.set_handlers([] { return json::array(); }, [&](const std::string&, const json&, const ffy::link::CallInfo&) {
            sap.wait();
            return json{{"content", json::array()}, {"isError", false}};
        });
        std::atomic<bool> stop{false};
        std::thread main([&] { tools.pump(stop); });
        std::string msg;
        try {
            call(tools, tool, 200);
        } catch (const std::exception& e) {
            msg = e.what();
        }
        sap.release();
        stop = true;
        main.join();
        CHECK(msg.rfind("SAP_GUI_NOT_RESPONDING: SAP GUI has not answered " + tool, 0) == 0);
        const bool warns = msg.find("may still be applied once SAP answers") != std::string::npos &&
                           msg.find("read the screen before you retry") != std::string::npos;
        CHECK(warns == (tool == "gui_element_fill"));
    }
}

TEST_CASE("hung SAP call: the hint also names a long-running step", "[link][hung]") {
    HungLink link;
    link.hang();
    link.clock.advance(61s);
    const std::string text = text_of(call(link.tools, "gui_doctor"));
    CHECK(text.find("long-running step (e.g. a report started with F8)") != std::string::npos);
    CHECK(text.find("dialog") != std::string::npos);
}

TEST_CASE("hung SAP call: a legitimate call over 60 s that completes clears the record, the next call runs", "[link][hung]") {
    HungLink link;
    link.hang();
    link.clock.advance(75s);
    CHECK(fairyfly::link::stuck_call_status(link.tools.in_flight()).is_object());   // the tray is amber now
    link.sap.release();   // the report finished
    link.hung_caller.join();
    CHECK(text_of(link.hung_result) == "gui_screen_read");   // its caller gets the result
    CHECK_FALSE(link.tools.in_flight().has_value());
    CHECK(fairyfly::link::stuck_call_status(link.tools.in_flight()).is_null());   // the tray goes back to normal
    CHECK(text_of(call(link.tools, "gui_element_click")) == "gui_element_click");
    CHECK(link.ran_other == 1);
}
