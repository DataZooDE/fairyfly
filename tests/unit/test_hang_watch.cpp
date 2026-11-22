// Issue #155, second variant: a call that never returns. The watch decides at the deadline (no COM, any thread).
#include <catch2/catch_test_macros.hpp>

#include "include/hang_watch.h"

using namespace fairyfly;
using namespace fairyfly::hang;

namespace {

struct Desk {
    FrontendState state{true, true, json{{"hung_windows", 1}}};
    int ended = 0;
    bool end_fails = false;
    BlockedCallWatch watch;
    Desk() {
        watch.set_probe([this] { return state; });
        watch.set_end_sap_gui([this] {
            ++ended;
            if (end_fails) return Result::fail("SAP_GUI_RESTART_FAILED", "did not end");
            state = FrontendState{};
            return Result::ok({{"ended", 1}});
        });
    }
};

RestartPermit allowed() { return RestartPermit{true, "", json::array()}; }

}  // namespace

TEST_CASE("watch: nothing armed means the generic timeout", "[hang][watch]") {
    Desk desk;
    CHECK_FALSE(desk.watch.on_deadline(120000).has_value());
    CHECK(desk.ended == 0);
}

TEST_CASE("watch: a frozen frontend with the restart allowed is ended once", "[hang][watch]") {
    Desk desk;
    desk.watch.arm("gui_screen_read", allowed());
    const auto first = desk.watch.on_deadline(120000);
    REQUIRE(first.has_value());
    CHECK((*first)["code"] == kGuiRestartedCode);
    CHECK((*first)["sap_gui_restarted"] == true);
    CHECK((*first)["unsaved_input_lost"] == true);
    CHECK((*first)["original_operation_run"] == "unknown");
    CHECK(desk.ended == 1);
    const auto again = desk.watch.on_deadline(125000);   // a second deadline of the same call
    REQUIRE(again.has_value());
    CHECK((*again)["code"] == kGuiRestartedCode);
    CHECK(desk.ended == 1);
    CHECK(desk.watch.disarm());
}

TEST_CASE("watch: a frozen frontend that may not be restarted is reported with the reason", "[hang][watch]") {
    Desk desk;
    desk.watch.arm("gui_key_send", RestartPermit{false, "SAP_AUTOSTART_DISABLED", json::array({"Enable it in the tray."})});
    const auto answer = desk.watch.on_deadline(120000);
    REQUIRE(answer.has_value());
    CHECK((*answer)["code"] == kGuiNotRespondingCode);
    CHECK((*answer)["restart"]["allowed"] == false);
    CHECK((*answer)["restart"]["reason"] == "SAP_AUTOSTART_DISABLED");
    CHECK((*answer)["frontend"]["hung_windows"] == 1);
    CHECK(desk.ended == 0);
    CHECK_FALSE(desk.watch.disarm());
}

TEST_CASE("watch: SAP GUI that still reacts waits for the server and is never ended", "[hang][watch]") {
    Desk desk;
    desk.state.frontend_hung = false;
    desk.watch.arm("gui_key_send", allowed());
    const auto answer = desk.watch.on_deadline(120000);
    REQUIRE(answer.has_value());
    CHECK((*answer)["code"] == kServerNotRespondingCode);
    CHECK(desk.ended == 0);
}

TEST_CASE("watch: no SAP GUI at all means the generic timeout", "[hang][watch]") {
    Desk desk;
    desk.state = FrontendState{};
    desk.watch.arm("gui_screen_read", allowed());
    CHECK_FALSE(desk.watch.on_deadline(120000).has_value());
}

TEST_CASE("watch: one restart per episode, renewed by a call that returned", "[hang][watch]") {
    Desk desk;
    desk.watch.arm("gui_screen_read", allowed());
    REQUIRE(desk.watch.on_deadline(120000));
    desk.watch.disarm();
    desk.state = FrontendState{true, true, json::object()};   // the new GUI freezes too
    desk.watch.arm("gui_screen_read", allowed());
    const auto second = desk.watch.on_deadline(120000);
    REQUIRE(second.has_value());
    CHECK((*second)["code"] == kGuiNotRespondingCode);
    CHECK((*second)["restart"]["reason"] == "already_restarted");
    CHECK(desk.ended == 1);
    desk.watch.disarm();
    desk.watch.note_responsive();
    desk.watch.arm("gui_screen_read", allowed());
    REQUIRE(desk.watch.on_deadline(120000));
    CHECK(desk.ended == 2);
}

TEST_CASE("watch: a failed end uses the episode's restart and says so", "[hang][watch]") {
    Desk desk;
    desk.end_fails = true;
    desk.watch.arm("gui_screen_read", allowed());
    const auto answer = desk.watch.on_deadline(120000);
    REQUIRE(answer.has_value());
    CHECK((*answer)["code"] == kGuiNotRespondingCode);
    CHECK((*answer)["restart"]["failed"] == "did not end");
    CHECK_FALSE(desk.watch.disarm());
    desk.watch.arm("gui_screen_read", allowed());
    CHECK((*desk.watch.on_deadline(120000))["restart"]["reason"] == "already_restarted");
    CHECK(desk.ended == 1);
}
