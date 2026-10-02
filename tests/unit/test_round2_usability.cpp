// Round two of the remote user test: pure shaping logic with lambdas as fakes (no SAP, no COM).
#include <catch2/catch_test_macros.hpp>
#include "include/action_status.h"
#include "include/field_fill_info.h"
#include "include/tab_guard.h"
#include "include/cli_handler.h"

using namespace fairyfly;
using namespace fairyfly::sap;
using nlohmann::json;

// ---- Item 1: fill result -----------------------------------------------------------------------------

namespace {
FieldProbe plain_field() {
    FieldProbe probe;
    probe.type = "GuiCTextField";
    probe.id = "wnd[0]/usr/ctxtP_DATUM";
    probe.name = "ctxtP_DATUM";
    probe.label = "Posting date";
    probe.max_length = 10;
    return probe;
}
}  // namespace

TEST_CASE("fill echoes the value read back from a plain field", "[fill][round2]") {
    auto probe = plain_field();
    probe.type = "GuiTextField";
    probe.id = "wnd[0]/usr/txtRSYST-BNAME";
    probe.name = "txtRSYST-BNAME";
    probe.label = "User";
    probe.text_after = "ZUSER01";
    probe.text_after_known = true;
    bool redacted = true;
    CHECK(fill_value_echo(probe, "zuser01", redacted) == "ZUSER01");  // what the control shows, not what was sent
    CHECK_FALSE(redacted);
}

TEST_CASE("fill echo falls back to the typed value when the read-back failed", "[fill][round2]") {
    auto probe = plain_field();
    bool redacted = true;
    CHECK(fill_value_echo(probe, "01.10.2026", redacted) == "01.10.2026");
    CHECK_FALSE(redacted);
}

TEST_CASE("fill echo is truncated to 200 characters", "[fill][round2]") {
    auto probe = plain_field();
    probe.text_after = std::string(500, 'a');
    probe.text_after_known = true;
    bool redacted = false;
    const auto echo = fill_value_echo(probe, probe.text_after, redacted);
    CHECK(echo.size() == 203);
    CHECK(echo.substr(200) == "...");
    // never splits a UTF-8 sequence
    probe.text_after = std::string(199, 'a') + "\xC3\xA4\xC3\xA4";
    const auto utf8 = fill_value_echo(probe, probe.text_after, redacted);
    CHECK(utf8 == std::string(199, 'a') + "\xC3\xA4" + "...");
}

TEST_CASE("fill keeps the marker with a reason for credential fields", "[fill][privacy][round2]") {
    bool redacted = false;
    SECTION("GuiPasswordField") {
        FieldProbe probe;
        probe.type = "GuiPasswordField";
        probe.id = "wnd[0]/usr/pwdRSYST-BCODE";
        probe.text_after = "hunter2";
        probe.text_after_known = true;
        const auto echo = fill_value_echo(probe, "hunter2", redacted);
        CHECK(redacted);
        CHECK(echo.find("hunter2") == std::string::npos);
        CHECK(echo.rfind("[REDACTED: ", 0) == 0);
        const auto field = build_fill_field_info(probe, "hunter2");
        CHECK(field.at("type") == "GuiPasswordField");
        CHECK_FALSE(field.contains("max_length"));
        CHECK_FALSE(field.contains("input_kind"));
    }
    SECTION("a text field with a credential-sounding element id") {
        FieldProbe probe;
        probe.type = "GuiTextField";
        probe.id = "wnd[0]/usr/txtUSR02-BCODE";
        probe.text_after = "hunter2";
        probe.text_after_known = true;
        const auto echo = fill_value_echo(probe, "hunter2", redacted);
        CHECK(redacted);
        CHECK(echo.find("hunter2") == std::string::npos);
    }
    SECTION("a text field with a password label") {
        FieldProbe probe;
        probe.type = "GuiTextField";
        probe.id = "wnd[0]/usr/txtFIELD1";
        probe.label = "Password";
        probe.text_after = "hunter2";
        probe.text_after_known = true;
        CHECK(fill_value_echo(probe, "hunter2", redacted).find("hunter2") == std::string::npos);
        CHECK(redacted);
    }
}

TEST_CASE("fill field info reports type, length and date format source", "[fill][round2]") {
    SECTION("format taken from the value the field showed before") {
        auto probe = plain_field();
        probe.text_before = "30.09.2026";
        probe.text_after = "01.10.2026";
        probe.text_after_known = true;
        const auto field = build_fill_field_info(probe, "01.10.2026");
        CHECK(field.at("type") == "GuiCTextField");
        CHECK(field.at("max_length") == 10);
        CHECK(field.at("input_kind") == "date");
        CHECK(field.at("format_hint") == "DD.MM.YYYY");
        CHECK(field.at("format_hint_source") == "field_value");
        CHECK_FALSE(field.contains("format_warning"));
    }
    SECTION("a wrong-format value is only a warning, never a rejection") {
        auto probe = plain_field();
        probe.text_before = "30.09.2026";
        probe.text_after = "10/01/2026";
        probe.text_after_known = true;
        const auto field = build_fill_field_info(probe, "10/01/2026");
        CHECK(field.at("format_hint") == "DD.MM.YYYY");
        REQUIRE(field.contains("format_warning"));
        CHECK(field.at("format_warning").get<std::string>().find("MM/DD/YYYY") != std::string::npos);
    }
    SECTION("an empty field of unknown format says unknown but keeps the type") {
        auto probe = plain_field();
        probe.text_after = "01.10.2026";
        probe.text_after_known = true;
        const auto field = build_fill_field_info(probe, "01.10.2026");
        CHECK(field.at("input_kind") == "date");
        CHECK(field.at("format_hint") == "unknown");
        CHECK_FALSE(field.contains("format_hint_source"));
    }
    SECTION("SAP normalised the typed value: its format is the user's format") {
        auto probe = plain_field();
        probe.text_after = "10/01/2026";
        probe.text_after_known = true;
        const auto field = build_fill_field_info(probe, "20261001");
        CHECK(field.at("format_hint") == "MM/DD/YYYY");
        CHECK(field.at("format_hint_source") == "normalized_input");
        CHECK(field.at("value_normalized") == true);
    }
    SECTION("time and plain fields") {
        FieldProbe time;
        time.type = "GuiCTextField";
        time.id = "wnd[0]/usr/ctxtP_UZEIT";
        time.name = "ctxtP_UZEIT";
        time.max_length = 8;
        time.text_before = "08:30:00";
        CHECK(build_fill_field_info(time, "09:00:00").at("format_hint") == "HH:MM:SS");
        FieldProbe plain;
        plain.type = "GuiTextField";
        plain.id = "wnd[0]/usr/txtP_NAME";
        plain.name = "txtP_NAME";
        plain.max_length = 12;
        const auto field = build_fill_field_info(plain, "x");
        CHECK(field.at("max_length") == 12);
        CHECK_FALSE(field.contains("input_kind"));
        CHECK_FALSE(field.contains("format_hint"));
    }
}

TEST_CASE("date and time shapes map to the SAP user formats", "[fill][round2]") {
    CHECK(date_format_of_text("01.10.2026") == "DD.MM.YYYY");
    CHECK(date_format_of_text("2026.10.01") == "YYYY.MM.DD");
    CHECK(date_format_of_text("10/01/2026") == "MM/DD/YYYY");
    CHECK(date_format_of_text("2026/10/01") == "YYYY/MM/DD");
    CHECK(date_format_of_text("10-01-2026") == "MM-DD-YYYY");
    CHECK(date_format_of_text("2026-10-01") == "YYYY-MM-DD");
    CHECK(date_format_of_text("20261001").empty());
    CHECK(date_format_of_text("").empty());
    CHECK(time_format_of_text("23:59:59") == "HH:MM:SS");
    CHECK(time_format_of_text("2359").empty());
}

TEST_CASE("an unchanged status bar after a fill is stale and omitted", "[fill][status][round2]") {
    ActionStatus stale{"Enter a valid date", "E", "", ""};
    Result result;
    result.status = Result::Status::Success;
    result.data = json::object();
    attach_fresh_status_bar(result, stale, stale);
    CHECK_FALSE(result.data.contains("status_bar"));
    CHECK_FALSE(result.data.contains("status_message"));

    ActionStatus fresh{"Field value changed", "S", "", ""};
    attach_fresh_status_bar(result, stale, fresh);
    REQUIRE(result.data.contains("status_bar"));
    CHECK(result.data["status_bar"].at("text") == "Field value changed");
    CHECK(result.data["status_bar"].at("changed") == true);

    Result failed;
    failed.status = Result::Status::Error;
    failed.error = json::object();
    attach_fresh_status_bar(failed, stale, stale);
    CHECK_FALSE(failed.error.contains("status_bar"));
}

// ---- Item 2: inactive tabs ---------------------------------------------------------------------------

TEST_CASE("tab pages are found in an element id", "[tabs][round2]") {
    const auto pages = tab_pages_in_path("wnd[0]/usr/tabsTAB_STRIP/tabpTAB2/ssubSUB:SAPLXX:0100/txtFIELD");
    REQUIRE(pages.size() == 1);
    CHECK(pages[0].strip_id == "wnd[0]/usr/tabsTAB_STRIP");
    CHECK(pages[0].page_id == "wnd[0]/usr/tabsTAB_STRIP/tabpTAB2");
    CHECK(pages[0].page_name == "tabpTAB2");

    const auto nested = tab_pages_in_path("wnd[0]/usr/tabsA/tabpP1/ssub/tabsB/tabpP2/txtX");
    REQUIRE(nested.size() == 2);
    CHECK(nested[0].page_id == "wnd[0]/usr/tabsA/tabpP1");
    CHECK(nested[1].strip_id == "wnd[0]/usr/tabsA/tabpP1/ssub/tabsB");

    CHECK(tab_pages_in_path("wnd[0]/usr/txtFIELD").empty());
    CHECK(tab_pages_in_path("wnd[0]/usr/tabsTS/tabpP1").empty());   // the page itself is not "under" a page
    CHECK(tab_pages_in_path("wnd[0]/usr/tabpP1/txtX").empty());     // a tabp segment needs a tabs strip before it
}

TEST_CASE("an element on an inactive tab gets ELEMENT_ON_INACTIVE_TAB", "[tabs][round2]") {
    const std::string element = "wnd[0]/usr/tabsTS/tabpADDR/ssubSUB/txtADDR-CITY";
    const TabPageLookup inactive = [](const TabPageRef& ref) {
        TabPageState state;
        state.found = true;
        state.selected = ref.page_name == "tabpLOGON";
        state.text = "Address";
        return state;
    };
    auto result = classify_element_on_inactive_tab(element, inactive, true);
    REQUIRE(result.has_value());
    CHECK(result->status == Result::Status::Error);
    CHECK(result->error.at("code") == "ELEMENT_ON_INACTIVE_TAB");
    CHECK(result->error.at("tab_id") == "wnd[0]/usr/tabsTS/tabpADDR");
    CHECK(result->error.at("tab_text") == "Address");
    CHECK(result->error.at("element") == element);
    const auto hint = result->error.at("hint").get<std::string>();
    CHECK(hint.find("activate the tab first (gui_element_click on the tab)") != std::string::npos);
    CHECK(hint.find("gui_screen_read tab=Address") != std::string::npos);
    CHECK(result->error.at("suggestions").dump().find("--activate-tab") != std::string::npos);

    auto no_activate = classify_element_on_inactive_tab(element, inactive, false);
    REQUIRE(no_activate.has_value());
    CHECK(no_activate->error.at("suggestions").dump().find("--activate-tab") == std::string::npos);
}

TEST_CASE("a missing element on the selected tab or outside tabs stays a plain miss", "[tabs][round2]") {
    const TabPageLookup selected = [](const TabPageRef&) {
        TabPageState state;
        state.found = true;
        state.selected = true;
        return state;
    };
    CHECK_FALSE(classify_element_on_inactive_tab("wnd[0]/usr/tabsTS/tabpA/txtX", selected, true).has_value());
    CHECK_FALSE(classify_element_on_inactive_tab("wnd[0]/usr/txtX", selected, true).has_value());
    const TabPageLookup gone = [](const TabPageRef&) { return TabPageState{}; };
    CHECK_FALSE(classify_element_on_inactive_tab("wnd[0]/usr/tabsTS/tabpA/txtX", gone, true).has_value());
}

TEST_CASE("the outermost inactive page of nested tab strips is reported", "[tabs][round2]") {
    const TabPageLookup lookup = [](const TabPageRef& ref) {
        TabPageState state;
        state.found = true;
        state.selected = ref.page_name == "tabpOUTER_OK";
        state.text = ref.page_name;
        return state;
    };
    auto inactive = find_inactive_tab_page("wnd[0]/usr/tabsA/tabpOUTER_OK/sub/tabsB/tabpINNER/txtX", lookup);
    REQUIRE(inactive.has_value());
    CHECK(inactive->first.page_name == "tabpINNER");
}

// ---- Item 3: tooltips --------------------------------------------------------------------------------

TEST_CASE("icon buttons report their tooltip in get and find results", "[tooltip][round2]") {
    json get = {{"value", ""}, {"element_type", "GuiButton"}};
    attach_tooltip_fields(get, "GuiButton", "Display <-> Change", "");
    CHECK(get.at("tooltip") == "Display <-> Change");
    CHECK_FALSE(get.contains("text"));  // empty text stays absent

    json labelled = {{"value", "Save"}};
    attach_tooltip_fields(labelled, "GuiButton", "Save (Ctrl+S)", "Save");
    CHECK(labelled.at("tooltip") == "Save (Ctrl+S)");
    CHECK(labelled.at("value") == "Save");
    CHECK_FALSE(labelled.contains("text"));  // same as value: not repeated

    json differs = {{"value", "Save"}};
    attach_tooltip_fields(differs, "GuiButton", "Save (Ctrl+S)", "Save as");
    CHECK(differs.at("text") == "Save as");

    json found = {{"id", "wnd[0]/tbar[1]/btn[5]"}, {"type", "GuiButton"}, {"text", ""}};
    attach_tooltip_fields(found, "GuiButton", "Delete", "");
    CHECK(found.at("tooltip") == "Delete");
}

TEST_CASE("tooltips stay absent when empty or for other control types", "[tooltip][round2]") {
    json data = {{"value", "x"}};
    attach_tooltip_fields(data, "GuiButton", "", "");
    CHECK_FALSE(data.contains("tooltip"));
    attach_tooltip_fields(data, "GuiTextField", "Some help", "");
    CHECK_FALSE(data.contains("tooltip"));
    CHECK(type_shows_tooltip("GuiTab"));
    CHECK_FALSE(type_shows_tooltip("GuiLabel"));
}

TEST_CASE("screen find Markdown shows the tooltip", "[tooltip][markdown][round2]") {
    Result result;
    result.status = Result::Status::Success;
    result.data = {{"screen_id", "wnd[0]"}, {"title", "SU01"}, {"scanned_count", 10},
                   {"match_limit_reached", false}, {"scan_limit_reached", false},
                   {"elements", json::array({{{"id", "wnd[0]/tbar[1]/btn[8]"}, {"type", "GuiButton"},
                                              {"name", "btn[8]"}, {"text", ""}, {"tooltip", "Display"}}})}};
    const auto markdown = cli::format_output(result, cli::OutputFormat::Markdown);
    CHECK(markdown.find("Tooltip: Display") != std::string::npos);
    const auto as_json = cli::format_output(result, cli::OutputFormat::Json);
    CHECK(as_json.find("\"tooltip\": \"Display\"") != std::string::npos);
}

