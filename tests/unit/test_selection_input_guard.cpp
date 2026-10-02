#include <catch2/catch_test_macros.hpp>

#include "include/selection_input_guard.h"

// Handler-level live validation for selection input (pure logic; the engine fills the probe from the real control).

using namespace fairyfly::sap;

namespace {

SelectionInputPolicy policy() {
    SelectionInputPolicy p;
    p.active = true;
    p.program = "SAPLSUU5";
    p.screen_number = "100";
    return p;
}

SelectionInputProbe plain_field() {
    SelectionInputProbe probe;
    probe.type = "GuiCTextField";
    probe.id = "wnd[0]/usr/ctxtUSR02-BNAME";
    probe.name = "USR02-BNAME";
    probe.label = "User";
    probe.tooltip = "User name";
    probe.changeable = true;
    probe.changeable_known = true;
    probe.program = "SAPLSUU5";
    probe.screen_number = "100";
    return probe;
}

std::string code_of(const SelectionInputProbe& probe) {
    const auto refusal = evaluate_selection_input(probe, policy());
    return refusal ? refusal->code : std::string();
}

}  // namespace

TEST_CASE("selection guard: a plain changeable text field on the recorded screen passes", "[selection-input][guard]") {
    CHECK_FALSE(evaluate_selection_input(plain_field(), policy()).has_value());
    auto text = plain_field();
    text.type = "GuiTextField";
    CHECK_FALSE(evaluate_selection_input(text, policy()).has_value());
    // not active: the guard does nothing (ordinary fills are untouched)
    CHECK_FALSE(evaluate_selection_input(SelectionInputProbe{}, SelectionInputPolicy{}).has_value());
}

TEST_CASE("selection guard: every other control type is refused", "[selection-input][guard]") {
    for (const char* type : {"GuiPasswordField", "GuiComboBox", "GuiComboBoxControl", "GuiCheckBox", "GuiRadioButton",
                             "GuiTableControl", "GuiGridView", "GuiTextedit", "GuiShell", "GuiButton", "GuiLabel", ""}) {
        auto probe = plain_field();
        probe.type = type;
        INFO(type);
        CHECK(code_of(probe) == "INPUT_TARGET_DENIED");
    }
}

TEST_CASE("selection guard: unknown or false changeability is refused", "[selection-input][guard]") {
    auto locked = plain_field();
    locked.changeable = false;
    CHECK(code_of(locked) == "INPUT_TARGET_DENIED");
    auto unknown = plain_field();
    unknown.changeable_known = false;
    CHECK(code_of(unknown) == "INPUT_TARGET_DENIED");
}

TEST_CASE("selection guard: credential names in id, name, label or tooltip are refused", "[selection-input][guard]") {
    auto by_id = plain_field();
    by_id.id = "wnd[0]/usr/txtRSYST-BCODE";
    by_id.name = "RSYST-BCODE";
    by_id.label = "";
    CHECK(code_of(by_id) == "INPUT_TARGET_DENIED");

    // a harmless id and name, the LIVE label gives it away
    auto by_label = plain_field();
    by_label.id = "wnd[0]/usr/txtFIELD1";
    by_label.name = "FIELD1";
    by_label.label = "New password";
    CHECK(code_of(by_label) == "INPUT_TARGET_DENIED");
    by_label.label = "Kennwort";
    CHECK(code_of(by_label) == "INPUT_TARGET_DENIED");

    auto by_tooltip = plain_field();
    by_tooltip.id = "wnd[0]/usr/txtFIELD1";
    by_tooltip.name = "FIELD1";
    by_tooltip.label = "";
    by_tooltip.tooltip = "Enter secret";
    CHECK(code_of(by_tooltip) == "INPUT_TARGET_DENIED");

    // a state-flag looking name on a changeable field is not exempt here (fail closed)
    auto state = plain_field();
    state.id = "wnd[0]/usr/txtPASSWORD_STATE";
    state.name = "PASSWORD_STATE";
    CHECK(code_of(state) == "INPUT_TARGET_DENIED");
}

TEST_CASE("selection guard: a screen change between the pre-call facts and the write is refused", "[selection-input][guard]") {
    auto moved = plain_field();
    moved.screen_number = "200";
    const auto refusal = evaluate_selection_input(moved, policy());
    REQUIRE(refusal.has_value());
    CHECK(refusal->code == "INPUT_SCREEN_DENIED");

    auto other_program = plain_field();
    other_program.program = "SAPMSUU5";
    CHECK(code_of(other_program) == "INPUT_SCREEN_DENIED");

    auto unknown_program = plain_field();
    unknown_program.program.clear();
    CHECK(code_of(unknown_program) == "INPUT_SCREEN_DENIED");
    auto unknown_screen = plain_field();
    unknown_screen.screen_number.clear();
    CHECK(code_of(unknown_screen) == "INPUT_SCREEN_DENIED");

    // a policy without a recorded screen can never match
    SelectionInputPolicy empty;
    empty.active = true;
    CHECK(evaluate_selection_input(plain_field(), empty).has_value());
}
