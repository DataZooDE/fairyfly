#include <catch2/catch_test_macros.hpp>
#include "include/read_only_guard.h"

using fairyfly::sap::matched_read_only_rule;

namespace {
const char* kPopupYes = "/app/con[0]/ses[0]/wnd[1]/usr/btnBUTTON_1";
std::string popup_rule(const std::string& text, const std::string& tooltip = "",
                       const std::string& id = "/app/con[0]/ses[0]/wnd[1]/usr/btnBUTTON_1") {
    return matched_read_only_rule("GuiButton", text, tooltip, id);
}
} // namespace

// Found live by a Codex bug hunt: under --read-only a click on the "Yes" button of the SU01 Delete
// Users confirmation (an id and label with no deny word) deleted the user.
TEST_CASE("read-only guard refuses answer buttons of popups it cannot classify", "[read_only][popup]") {
    SECTION("confirming answers are refused") {
        CHECK_FALSE(popup_rule("Yes").empty());
        CHECK_FALSE(popup_rule("&Yes").empty());
        CHECK_FALSE(popup_rule("OK").empty());
        CHECK_FALSE(popup_rule("Continue").empty());
        CHECK_FALSE(popup_rule("Ja").empty());
        CHECK_FALSE(popup_rule("Overwrite").empty());
        CHECK_FALSE(popup_rule("Confirm").empty());
        CHECK_FALSE(popup_rule("", "", "/app/con[0]/ses[0]/wnd[1]/tbar[0]/btn[0]").empty());    // icon-only Enter
        CHECK_FALSE(popup_rule("", "", "/app/con[0]/ses[0]/wnd[2]/usr/btnSPOP-OPTION1").empty());  // icon-only, unknown
        CHECK(popup_rule("Yes") == "popup-button:yes");
    }
    SECTION("dismissing and navigating buttons stay allowed") {
        CHECK(popup_rule("No").empty());
        CHECK(popup_rule("&No").empty());
        CHECK(popup_rule("Cancel").empty());
        CHECK(popup_rule("Close").empty());
        CHECK(popup_rule("Back").empty());
        CHECK(popup_rule("Details").empty());
        CHECK(popup_rule("Help").empty());
        CHECK(popup_rule("", "Cancel (F12)").empty());
        CHECK(popup_rule("", "", "/app/con[0]/ses[0]/wnd[1]/tbar[0]/btn[12]").empty());   // standard Cancel
    }
    SECTION("the existing word rules still win in popups") {
        CHECK(popup_rule("Delete") == "word:delete");
        CHECK(popup_rule("Cancel job") == "phrase:cancel job");
    }
    SECTION("main window buttons are classified exactly as before") {
        const std::string main_yes = "/app/con[0]/ses[0]/wnd[0]/usr/btnBUTTON_1";
        CHECK(matched_read_only_rule("GuiButton", "Yes", "", main_yes).empty());
        CHECK(matched_read_only_rule("GuiButton", "OK", "", main_yes).empty());
        CHECK(matched_read_only_rule("GuiButton", "Display", "", "/app/con[0]/ses[0]/wnd[0]/tbar[1]/btn[7]").empty());
    }
    SECTION("only buttons are affected: popup fields and labels keep their rules") {
        CHECK(matched_read_only_rule("GuiLabel", "Yes", "", "/app/con[0]/ses[0]/wnd[1]/usr/lblTEXT").empty());
        CHECK(matched_read_only_rule("GuiTextField", "OK", "", "/app/con[0]/ses[0]/wnd[1]/usr/txtFIELD").empty());
        CHECK(matched_read_only_rule("GuiCheckBox", "Yes", "", "/app/con[0]/ses[0]/wnd[1]/usr/chkX").empty());
    }
    SECTION("window index parsing does not misfire") {
        CHECK(matched_read_only_rule("GuiButton", "OK", "", "/app/con[0]/ses[0]/wnd[0]/usr/ssubSUB[1]/btnOK").empty());
        CHECK(matched_read_only_rule("GuiButton", "OK", "", "wnd[0]/usr/btnX").empty());
        CHECK_FALSE(matched_read_only_rule("GuiButton", "OK", "", "wnd[1]/usr/btnX").empty());  // relative id
    }
}
