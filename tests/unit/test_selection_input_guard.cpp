#include <catch2/catch_test_macros.hpp>

#include <map>

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

TEST_CASE("display-only PASSWORD_EXT_PWD_STATE on the SU01 logon data tab is shown", "[sensitive][su01_state]") {
    using namespace fairyfly::sap;
    const std::string id = "/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpLOGO/ssubMAINAREA:SAPLSUID_MAINTENANCE:1101/txtPASSWORD_EXT_PWD_STATE";
    INFO("known=" << is_known_secret_field_name("txtPASSWORD_EXT_PWD_STATE")
         << " state_name=" << is_credential_state_name("txtPASSWORD_EXT_PWD_STATE")
         << " deny='" << sensitive_input_field_reason("GuiTextField", id, "", StateExemption::Deny) << "'"
         << " allow='" << sensitive_input_field_reason("GuiTextField", id, "", StateExemption::Allow) << "'");
    CHECK(sensitive_field_reason_for_display("GuiTextField", id, "", true, false, [] { return std::string("Production Password"); }).empty());
    CHECK(sensitive_field_reason_for_display("GuiTextField", id, "", true, false, [] { return std::string(); }).empty());
    CHECK_FALSE(sensitive_field_reason_for_display("GuiTextField", id, "", true, true, [] { return std::string("x"); }).empty());
}

TEST_CASE("state values padded to the field width are still recognised", "[sensitive][su01_state]") {
    using namespace fairyfly::sap;
    const std::string padded = "Production Password" + std::string(120, ' ');
    CHECK(looks_like_state_value(padded));
    CHECK(looks_like_state_value("  locked  "));
    CHECK_FALSE(looks_like_state_value(std::string(60, 'x')));
    CHECK_FALSE(looks_like_state_value("aB3$kQ9!zT7#" + std::string(100, ' ')));  // random-looking token, even when padded
    const std::string id = "/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpLOGO/ssubMAINAREA:SAPLSUID_MAINTENANCE:1101/txtPASSWORD_EXT_PWD_STATE";
    CHECK(sensitive_field_reason_for_display("GuiTextField", id, "Password Status", true, false, [&] { return padded; }).empty());
    CHECK_FALSE(sensitive_field_reason_for_display("GuiTextField", id, "Password Status", true, true, [&] { return padded; }).empty());
    CHECK_FALSE(sensitive_field_reason_for_display("GuiTextField", id, "Password Status", false, false, [&] { return padded; }).empty());
}

TEST_CASE("state values: an allowlist of state words, not a length heuristic", "[sensitive][su01_state]") {
    using namespace fairyfly::sap;
    const std::string pad(100, ' ');
    // allowlisted values, also padded to the field width
    for (const char* value : {"Production Password", "Initial", "Password set", "locked", "Not set", "gesperrt", "G\xC3\xBC" "ltig",
                              "Passwort ge" "\xC3\xA4" "ndert", "Nein", "Inaktiv", "valid_password", "Single x509", "Password Status"}) {
        INFO(value);
        CHECK(looks_like_state_value(value));
        CHECK(looks_like_state_value(std::string(value) + pad));
        CHECK(looks_like_state_value(pad + value));
    }
    // empty, digits, dates, times
    for (const char* value : {"", "   ", "0", "1", "01.10.2026", "2026-10-02", "12:30:00", "01.10.2026 12:30:00"}) {
        INFO(value);
        CHECK(looks_like_state_value(value));
        CHECK(looks_like_state_value(std::string(value) + pad));
    }
    // a short opaque token / a 7-character secret is NOT a state label (it used to be: < 8 characters)
    for (const char* value : {"Tr0ub4d", "abc123", "Sommer1", "geheim", "Pa$$w0r", "hunter2", "xyz", "Password1"}) {
        INFO(value);
        CHECK_FALSE(looks_like_state_value(value));
        CHECK_FALSE(looks_like_state_value(std::string(value) + pad));
    }
    // a state word next to a secret, or a secret with whitespace, is not a state label
    CHECK_FALSE(looks_like_state_value("Password Tr0ub4d"));
    CHECK_FALSE(looks_like_state_value("not set hunter2"));
    CHECK_FALSE(looks_like_state_value("correct horse battery"));
    // digits only is fine up to 24 characters, never more; other symbols are not a state label
    CHECK_FALSE(looks_like_state_value(std::string(25, '1')));
    CHECK_FALSE(looks_like_state_value("Password!"));
    CHECK_FALSE(looks_like_state_value("set;"));
    // at most 60 characters in total
    std::string long_state;
    while (long_state.size() <= 60) long_state += "password set ";
    CHECK_FALSE(looks_like_state_value(long_state));

    // the SU01 display field stays visible; a 7-character secret in the same display-only field is redacted
    const std::string id = "/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpLOGO/ssubMAINAREA:SAPLSUID_MAINTENANCE:1101/txtPASSWORD_EXT_PWD_STATE";
    CHECK(sensitive_field_reason_for_display("GuiTextField", id, "Password Status", true, false,
                                             [&] { return std::string("Production Password") + pad; }).empty());
    CHECK_FALSE(sensitive_field_reason_for_display("GuiTextField", id, "Password Status", true, false,
                                                   [&] { return std::string("Tr0ub4d") + pad; }).empty());
    CHECK_FALSE(sensitive_field_reason_for_display("GuiTextField", id, "Password Status", true, false,
                                                   [&] { return std::string("hunter2"); }).empty());
}

// ---- strict collection of the live probe (fail closed on unreadable properties) --------------------------

namespace {

using Props = std::map<std::string, PropertyRead>;

PropertyRead ok(const std::string& v) { return PropertyRead{PropertyStatus::Ok, v}; }
PropertyRead unsupported() { return PropertyRead{PropertyStatus::NotSupported, ""}; }
PropertyRead failed() { return PropertyRead{PropertyStatus::Failed, ""}; }

/// A plain GuiCTextField as the scripting API reports it: Name/Tooltip present, no AccLabel, left label only.
Props plain_props() {
    return {{"Type", ok("GuiCTextField")}, {"Id", ok("/app/con[0]/ses[0]/wnd[0]/usr/ctxtUSR02-BNAME")},
            {"Name", ok("USR02-BNAME")},   {"Changeable", ok("true")},
            {"AccLabel", unsupported()},   {"LeftLabel.Text", ok("User")},
            {"RightLabel.Text", unsupported()}, {"AccTooltip", unsupported()},
            {"DefaultTooltip", unsupported()}, {"Tooltip", ok("")}};
}

std::optional<SelectionInputRefusal> collect(const Props& props, SelectionInputProbe& probe) {
    return collect_selection_probe(
        [&](const std::string& property) {
            const auto it = props.find(property);
            return it == props.end() ? unsupported() : it->second;
        },
        probe);
}

void place_on_initial_screen(SelectionInputProbe& probe) {
    probe.id = "wnd[0]/usr/ctxtUSR02-BNAME";
    probe.program = "SAPLSUU5";
    probe.screen_number = "100";
}

}  // namespace

TEST_CASE("selection guard: the plain selection field is collected and allowed (optional properties may be missing)",
          "[selection-input][guard]") {
    SelectionInputProbe probe;
    CHECK_FALSE(collect(plain_props(), probe).has_value());
    CHECK(probe.type == "GuiCTextField");
    CHECK(probe.name == "USR02-BNAME");
    CHECK(probe.label == "User");
    CHECK(probe.tooltip.empty());
    CHECK(probe.changeable_known);
    CHECK(probe.changeable);
    place_on_initial_screen(probe);
    CHECK_FALSE(evaluate_selection_input(probe, policy()).has_value());

    // no label object at all and an empty tooltip are harmless: empty strings, not errors
    Props bare = plain_props();
    bare["LeftLabel.Text"] = unsupported();
    SelectionInputProbe bare_probe;
    CHECK_FALSE(collect(bare, bare_probe).has_value());
    CHECK(bare_probe.label.empty());
}

TEST_CASE("selection guard: a failed read of any property refuses and names it", "[selection-input][guard]") {
    for (const char* property : {"Type", "Id", "Name", "Changeable", "AccLabel", "LeftLabel.Text", "RightLabel.Text",
                                 "AccTooltip", "DefaultTooltip", "Tooltip"}) {
        Props props = plain_props();
        props[property] = failed();
        SelectionInputProbe probe;
        const auto refusal = collect(props, probe);
        INFO(property);
        REQUIRE(refusal.has_value());
        CHECK(refusal->code == "INPUT_TARGET_DENIED");
        CHECK(refusal->message.find(std::string("'") + property + "'") != std::string::npos);
    }
}

TEST_CASE("selection guard: required properties must exist; a malformed Changeable refuses", "[selection-input][guard]") {
    for (const char* property : {"Type", "Id", "Name", "Changeable"}) {
        Props props = plain_props();
        props[property] = unsupported();
        SelectionInputProbe probe;
        const auto refusal = collect(props, probe);
        INFO(property);
        REQUIRE(refusal.has_value());
        CHECK(refusal->code == "INPUT_TARGET_DENIED");
    }
    Props props = plain_props();
    props["Changeable"] = ok("maybe");
    SelectionInputProbe probe;
    CHECK(collect(props, probe).has_value());
}

TEST_CASE("selection guard: a credential hint in any collected label or tooltip still refuses", "[selection-input][guard]") {
    Props props = plain_props();
    props["LeftLabel.Text"] = ok("Password");
    SelectionInputProbe probe;
    REQUIRE_FALSE(collect(props, probe).has_value());
    place_on_initial_screen(probe);
    CHECK(code_of(probe) == "INPUT_TARGET_DENIED");

    // a hint in a later label source is not hidden by an earlier non-empty one
    Props both = plain_props();
    both["AccLabel"] = ok("Value");
    both["RightLabel.Text"] = ok("Password");
    SelectionInputProbe both_probe;
    REQUIRE_FALSE(collect(both, both_probe).has_value());
    place_on_initial_screen(both_probe);
    CHECK(code_of(both_probe) == "INPUT_TARGET_DENIED");
}
