// Issue #155: hang detection is pure logic over finished calls (no COM).
#include <catch2/catch_test_macros.hpp>

#include "include/hang_detector.h"

using namespace fairyfly;
using namespace fairyfly::hang;

namespace {

Result key_result(int vkey, bool screen_changed) {
    Result r;
    r.status = Result::Status::Success;
    r.data["action"] = "send_key";
    r.data["vkey"] = vkey;
    r.data["screen_changed"] = screen_changed;
    r.duration = std::chrono::milliseconds(7);
    return r;
}

Result tcode_not_started(bool busy, bool screen_changed) {
    auto r = Result::fail("TRANSACTION_NOT_STARTED", "Requested transaction did not become active");
    r.error["session_busy"] = busy;
    r.error["screen_changed"] = screen_changed;
    r.error["tcode"] = "/nSE38";
    return r;
}

Observation noop_f3() { return classify("gui_key_send", {{"key", "F3"}}, key_result(3, false)); }

}  // namespace

TEST_CASE("hang: a navigation key that changes nothing is a no-op", "[hang]") {
    CHECK(classify("gui_key_send", {{"key", "F3"}}, key_result(3, false)).signal == Signal::NoOp);
    CHECK(classify("gui_key_send", {{"key", "F12"}}, key_result(12, false)).signal == Signal::NoOp);
    CHECK(classify("gui_key_send", {{"key", "shift+f3"}}, key_result(15, false)).signal == Signal::NoOp);
}

TEST_CASE("hang: Enter, F8 and other keys that change nothing say nothing", "[hang]") {
    CHECK(classify("gui_key_send", {{"key", "enter"}}, key_result(0, false)).signal == Signal::Neutral);
    CHECK(classify("gui_key_send", {{"key", "F8"}}, key_result(8, false)).signal == Signal::Neutral);
    CHECK(classify("gui_key_send", {{"key", "pagedown"}}, key_result(82, false)).signal == Signal::Neutral);
}

TEST_CASE("hang: any key that changes the screen proves the GUI reacts", "[hang]") {
    CHECK(classify("gui_key_send", {{"key", "enter"}}, key_result(0, true)).signal == Signal::Change);
    CHECK(classify("gui_key_send", {{"key", "F3"}}, key_result(3, true)).signal == Signal::Change);
}

TEST_CASE("hang: a key result without the screen check says nothing", "[hang]") {
    Result r;
    r.status = Result::Status::Success;
    r.data["vkey"] = 3;   // older worker without screen_changed
    CHECK(classify("gui_key_send", {{"key", "F3"}}, r).signal == Signal::Neutral);
}

TEST_CASE("hang: a transaction that did not start on an idle unchanged session is a no-op", "[hang]") {
    CHECK(classify("gui_transaction_start", {{"code", "/nSE38"}}, tcode_not_started(false, false)).signal == Signal::NoOp);
    CHECK(classify("gui_transaction_start", {{"code", "/nSE38"}}, tcode_not_started(true, false)).signal == Signal::Neutral);
    CHECK(classify("gui_transaction_start", {{"code", "/nSE38"}}, tcode_not_started(false, true)).signal == Signal::Neutral);
    Result started;
    started.status = Result::Status::Success;
    CHECK(classify("gui_transaction_start", {{"code", "SE38"}}, started).signal == Signal::Change);
    CHECK(classify("gui_transaction_start", {{"code", "XX"}}, Result::fail("TRANSACTION_FAILED", "unknown")).signal ==
          Signal::Neutral);
}

TEST_CASE("hang: reads and other tools say nothing", "[hang]") {
    Result ok;
    ok.status = Result::Status::Success;
    CHECK(classify("gui_screen_read", json::object(), ok).signal == Signal::Neutral);
    CHECK(classify("gui_element_click", {{"element", "wnd[0]/tbar[1]/btn[8]"}}, ok).signal == Signal::Neutral);
}

TEST_CASE("hang: three no-ops in a row trip the detector with the evidence", "[hang]") {
    Detector detector;
    CHECK_FALSE(detector.observe("5", noop_f3()).tripped);
    CHECK_FALSE(detector.observe("5", noop_f3()).tripped);
    const Verdict verdict = detector.observe("5", noop_f3());
    CHECK(verdict.tripped);
    CHECK(verdict.consecutive_noops == 3);
    REQUIRE(verdict.evidence.size() == 3);
    CHECK(verdict.evidence[0]["tool"] == "gui_key_send");
    CHECK(verdict.evidence[0]["sap_action_ms"] == 7);
}

TEST_CASE("hang: key and transaction no-ops count together", "[hang]") {
    Detector detector;
    const auto tcode = classify("gui_transaction_start", {{"code", "/nSE38"}}, tcode_not_started(false, false));
    detector.observe("5", tcode);
    detector.observe("5", noop_f3());
    CHECK(detector.observe("5", tcode).tripped);
}

TEST_CASE("hang: a screen change resets the count", "[hang]") {
    Detector detector;
    detector.observe("5", noop_f3());
    detector.observe("5", noop_f3());
    detector.observe("5", classify("gui_key_send", {{"key", "enter"}}, key_result(0, true)));
    CHECK(detector.consecutive_noops("5") == 0);
    CHECK_FALSE(detector.observe("5", noop_f3()).tripped);
}

TEST_CASE("hang: neutral calls between no-ops neither count nor reset", "[hang]") {
    Detector detector;
    Result ok;
    ok.status = Result::Status::Success;
    detector.observe("5", noop_f3());
    detector.observe("5", classify("gui_screen_read", json::object(), ok));
    detector.observe("5", noop_f3());
    CHECK(detector.observe("5", noop_f3()).tripped);
}

TEST_CASE("hang: sessions are counted separately", "[hang]") {
    Detector detector;
    detector.observe("5", noop_f3());
    detector.observe("5", noop_f3());
    CHECK_FALSE(detector.observe("6", noop_f3()).tripped);
    CHECK(detector.observe("5", noop_f3()).tripped);
}

TEST_CASE("hang: reset forgets a session", "[hang]") {
    Detector detector;
    detector.observe("5", noop_f3());
    detector.observe("5", noop_f3());
    detector.reset("5");
    CHECK_FALSE(detector.observe("5", noop_f3()).tripped);
}

TEST_CASE("hang: one restart per episode, renewed after the GUI reacted", "[hang]") {
    Detector detector;
    CHECK_FALSE(detector.restart_used());
    detector.note_restart();
    CHECK(detector.restart_used());
    detector.observe("7", noop_f3());
    CHECK(detector.restart_used());   // a no-op does not end the episode
    detector.observe("7", classify("gui_key_send", {{"key", "F3"}}, key_result(3, true)));
    CHECK_FALSE(detector.restart_used());
}
