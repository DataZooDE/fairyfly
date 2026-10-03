// Round two of the remote user test: pure shaping logic with lambdas as fakes (no SAP, no COM).
#include <catch2/catch_test_macros.hpp>
#include "include/action_status.h"
#include "include/field_fill_info.h"
#include "include/tab_guard.h"
#include "include/cli_handler.h"
#include "include/screenshot_handler.h"
#include "include/formatters/screen_markdown_formatter.h"
#include "include/server_clock.h"

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
    // `screen read --tab` takes a tab id or its trailing part, not the tab text (found by a live bug hunt)
    CHECK(hint.find("gui_screen_read tab=tabpADDR") != std::string::npos);
    CHECK(hint.find("tab=Address") == std::string::npos);
    CHECK(result->error.at("suggestions").dump().find("screen read --tab 'tabpADDR'") != std::string::npos);
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

// ---- Item 4: screenshot crop and scale ---------------------------------------------------------------

TEST_CASE("crop is in native pixels and applied before scaling", "[screenshot][crop][round2]") {
    cli::ScreenshotOptions options;
    options.crop_x = 100;
    options.crop_y = 50;
    options.crop_width = 400;
    options.crop_height = 200;
    options.scale = "0.5";
    const auto plan = ScreenshotHandler::plan_capture_geometry(1000, 800, options);
    CHECK(plan.cropped);
    CHECK(plan.crop_x == 100);
    CHECK(plan.crop_width == 400);
    CHECK(plan.output_width == 200);
    CHECK(plan.output_height == 100);
    const auto info = ScreenshotHandler::geometry_json(plan);
    CHECK(info.at("native_size") == json({{"width", 1000}, {"height", 800}}));
    CHECK(info.at("crop") == json({{"x", 100}, {"y", 50}, {"width", 400}, {"height", 200}}));
    CHECK(info.at("output_size") == json({{"width", 200}, {"height", 100}}));
    CHECK_FALSE(info.contains("crop_clamped"));
}

TEST_CASE("an integer scale is a target width of the cropped image", "[screenshot][crop][round2]") {
    cli::ScreenshotOptions options;
    options.crop_x = 0;
    options.crop_y = 0;
    options.crop_width = 600;
    options.crop_height = 300;
    options.scale = "300";
    const auto plan = ScreenshotHandler::plan_capture_geometry(1000, 800, options);
    CHECK(plan.output_width == 300);
    CHECK(plan.output_height == 150);
}

TEST_CASE("without crop or scale the output is the native size", "[screenshot][crop][round2]") {
    const auto plan = ScreenshotHandler::plan_capture_geometry(1280, 720, cli::ScreenshotOptions{});
    CHECK_FALSE(plan.cropped);
    CHECK(plan.output_width == 1280);
    const auto info = ScreenshotHandler::geometry_json(plan);
    CHECK_FALSE(info.contains("crop"));
    CHECK(info.at("output_size") == json({{"width", 1280}, {"height", 720}}));
}

TEST_CASE("a crop completely outside the image is INVALID_ARGUMENT material with the native size", "[screenshot][crop][round2]") {
    cli::ScreenshotOptions options;
    options.crop_x = 1000;  // as if taken from a scaled-up picture
    options.crop_y = 10;
    options.crop_width = 100;
    options.crop_height = 100;
    try {
        ScreenshotHandler::plan_capture_geometry(800, 600, options);
        FAIL("expected std::invalid_argument");
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        CHECK(message.find("800x600") != std::string::npos);
        CHECK(message.find("completely outside") != std::string::npos);
        CHECK(message.find("native") != std::string::npos);
    }
    options.crop_x = 0;
    options.crop_y = 600;
    CHECK_THROWS_AS(ScreenshotHandler::plan_capture_geometry(800, 600, options), std::invalid_argument);
}

TEST_CASE("a crop that overhangs the image is clamped and says so", "[screenshot][crop][round2]") {
    cli::ScreenshotOptions options;
    options.crop_x = 700;
    options.crop_y = 500;
    options.crop_width = 300;
    options.crop_height = 300;
    const auto plan = ScreenshotHandler::plan_capture_geometry(800, 600, options);
    CHECK(plan.crop_width == 100);
    CHECK(plan.crop_height == 100);
    CHECK(plan.crop_clamped);
    CHECK(ScreenshotHandler::geometry_json(plan).at("crop_clamped") == true);
}

TEST_CASE("a scale that would produce an empty image is rejected", "[screenshot][crop][round2]") {
    cli::ScreenshotOptions options;
    options.crop_x = 0;
    options.crop_y = 0;
    options.crop_width = 10;
    options.crop_height = 10;
    options.scale = "0.01";
    CHECK_THROWS_AS(ScreenshotHandler::plan_capture_geometry(800, 600, options), std::invalid_argument);
}

// ---- Item 5: text filter across tabs -----------------------------------------------------------------

namespace {
json tabbed_read(bool expanded, bool only_second) {
    const json tab1 = {{"id", "wnd[0]/usr/tabsTS/tabpONE"}, {"type", "GuiTab"}, {"text", "Logon data"}};
    const json tab2 = {{"id", "wnd[0]/usr/tabsTS/tabpTWO"}, {"type", "GuiTab"}, {"text", "Address"}};
    const json tab3 = {{"id", "wnd[0]/usr/tabsTS/tabpTHREE"}, {"type", "GuiTab"}, {"text", "Roles"}};
    json data = {{"elements", json::array({tab1, tab2, tab3,
                                           {{"id", "wnd[0]/usr/txtNAME"}, {"type", "GuiTextField"}, {"text", "ALICE"}}})},
                 {"hierarchy", {{"tabs", json::array({tab1, tab2, tab3})},
                                {"form_fields", json::array({{{"id", "wnd[0]/usr/txtNAME"}, {"type", "GuiTextField"},
                                                              {"text", "ALICE"}}})}}},
                 {"element_count", 4}};
    if (expanded) {
        json content = json::array();
        if (!only_second)
            content.push_back({{"tab_id", tab1["id"]}, {"tab_name", "Logon data"}, {"elements", json::array()},
                               {"hierarchy", json::object()}, {"element_count", 0}});
        content.push_back({{"tab_id", tab2["id"]}, {"tab_name", "Address"},
                           {"elements", json::array({{{"id", "wnd[0]/usr/tabsTS/tabpTWO/txtCITY"},
                                                      {"type", "GuiTextField"}, {"text", "Berlin"}}})},
                           {"hierarchy", {{"form_fields", json::array({{{"id", "wnd[0]/usr/tabsTS/tabpTWO/txtCITY"},
                                                                         {"type", "GuiTextField"}, {"text", "Berlin"}}})}}},
                           {"element_count", 1}});
        data["tabs_content"] = content;
        data["tabs_expanded"] = true;
    }
    return data;
}
}  // namespace

TEST_CASE("tab coverage lists searched and skipped tabs", "[screen][filters][tabs][round2]") {
    SECTION("all tabs expanded") {
        auto data = tabbed_read(true, false);
        data["tabs_failed"] = json::array({{{"tab_id", "wnd[0]/usr/tabsTS/tabpTHREE"}, {"tab_name", "Roles"},
                                            {"reason", "busy_timeout"}}});
        const auto coverage = cli::describe_tab_coverage(data);
        CHECK(coverage.at("searched") == json::array({"Logon data", "Address"}));
        REQUIRE(coverage.at("skipped").size() == 1);
        CHECK(coverage.at("skipped").at(0).at("reason") == "busy_timeout");
    }
    SECTION("--no-tabs: nothing expanded") {
        const auto coverage = cli::describe_tab_coverage(tabbed_read(false, false));
        CHECK(coverage.at("searched").empty());
        REQUIRE(coverage.at("skipped").size() == 3);
        CHECK(coverage.at("skipped").at(0).at("reason") == "not_expanded");
    }
    SECTION("--tab: the others are not requested") {
        const auto coverage = cli::describe_tab_coverage(tabbed_read(true, true));
        CHECK(coverage.at("searched") == json::array({"Address"}));
        REQUIRE(coverage.at("skipped").size() == 2);
        CHECK(coverage.at("skipped").at(0).at("reason") == "not_requested");
    }
    SECTION("a screen without tabs has no coverage") {
        json plain = {{"elements", json::array({{{"id", "x"}, {"type", "GuiTextField"}}})}};
        CHECK(cli::describe_tab_coverage(plain).empty());
    }
}

TEST_CASE("text_contains with collapsed tabs says what it did not search", "[screen][filters][tabs][round2]") {
    auto data = tabbed_read(false, false);
    cli::ScreenFilterOptions filters;
    filters.text_contains = "berlin";
    cli::apply_screen_filters(data, filters);
    CHECK(data.at("element_count") == 0);
    CHECK(data.at("tabs_searched").empty());
    CHECK(data.at("tabs_skipped").size() == 3);
    const auto note = data.at("text_filter_note").get<std::string>();
    CHECK(note.find("0 matches") != std::string::npos);
    CHECK(note.find("tabs not expanded: [Logon data, Address, Roles]") != std::string::npos);
    CHECK(note.find("(use tab=...)") != std::string::npos);

    const auto markdown = cli::ScreenMarkdownFormatter::format(data, true);
    CHECK(markdown.find("tabs not expanded") != std::string::npos);
}

TEST_CASE("text_contains over expanded tabs finds the match and names the searched tabs", "[screen][filters][tabs][round2]") {
    auto data = tabbed_read(true, false);
    cli::ScreenFilterOptions filters;
    filters.text_contains = "berlin";
    cli::apply_screen_filters(data, filters);
    CHECK(data.at("tabs_searched") == json::array({"Logon data", "Address"}));
    CHECK(data.at("tabs_content").at(1).at("element_count") == 1);
    // the third tab was not read: even with a match the caller is told
    REQUIRE(data.at("tabs_skipped").size() == 1);
    CHECK(data.at("tabs_skipped").at(0).at("tab_name") == "Roles");
    CHECK(data.at("text_filter_note") == "Searched tabs [Logon data, Address]; tabs not expanded: [Roles] (use tab=...)");

    SECTION("every tab read and a match: no note") {
        auto all = tabbed_read(true, false);
        all["tabs_content"].push_back({{"tab_id", "wnd[0]/usr/tabsTS/tabpTHREE"}, {"tab_name", "Roles"},
                                       {"elements", json::array()}, {"hierarchy", json::object()},
                                       {"element_count", 0}});
        cli::apply_screen_filters(all, filters);
        CHECK_FALSE(all.contains("tabs_skipped"));
        CHECK_FALSE(all.contains("text_filter_note"));
        CHECK(all.at("tabs_searched").size() == 3);
    }
}

TEST_CASE("text_contains with --tab reports the other tabs as not requested", "[screen][filters][tabs][round2]") {
    auto data = tabbed_read(true, true);
    cli::ScreenFilterOptions filters;
    filters.text_contains = "nomatch";
    cli::apply_screen_filters(data, filters);
    CHECK(data.at("tabs_searched") == json::array({"Address"}));
    const auto note = data.at("text_filter_note").get<std::string>();
    CHECK(note.find("0 matches in tabs [Address]") != std::string::npos);
    CHECK(note.find("tabs not expanded: [Logon data, Roles]") != std::string::npos);
}

TEST_CASE("without text_contains no tab fields are added", "[screen][filters][tabs][round2]") {
    auto data = tabbed_read(false, false);
    cli::ScreenFilterOptions filters;
    filters.only_buttons = true;
    cli::apply_screen_filters(data, filters);
    CHECK_FALSE(data.contains("tabs_searched"));
    CHECK_FALSE(data.contains("text_filter_note"));
}

// ---- Item 6: server clock ----------------------------------------------------------------------------

TEST_CASE("the server clock is reported as unavailable with a labelled PC fallback", "[session][clock][round2]") {
    const auto at = std::chrono::system_clock::time_point{std::chrono::seconds{1790000000}};  // 2026-09-21T14:13:20Z
    const auto fields = server_time_fields_at(at, 120);
    CHECK(fields.at("server_time").is_null());
    CHECK(fields.at("server_time_source") == "unavailable");
    CHECK(fields.at("client_time") == "2026-09-21T16:13:20+02:00");
    CHECK(fields.at("client_utc_offset") == "+02:00");
    CHECK(fields.at("server_time_note").get<std::string>().find("not the SAP server's") != std::string::npos);
    const auto summary = server_time_summary(fields);
    CHECK(summary.find("SAP server time: unavailable") != std::string::npos);
    CHECK(summary.find("2026-09-21T16:13:20+02:00") != std::string::npos);
}

TEST_CASE("UTC offsets are formatted with sign and minutes", "[session][clock][round2]") {
    CHECK(format_utc_offset(0) == "+00:00");
    CHECK(format_utc_offset(330) == "+05:30");
    CHECK(format_utc_offset(-300) == "-05:00");
    CHECK(server_time_fields().at("server_time_source") == "unavailable");
}

// ---- activate_tab: the previous tabs are restored on every exit path --------------------------------

namespace {
// What the engine does around a read, with lambdas as the GUI: the selected page of one strip is a string.
struct FakeStrip {
    std::string selected = "tabs/tabpA";
    std::vector<std::string> selects;
};
}  // namespace

TEST_CASE("TabActivationGuard restores the previous tab when the read throws after the select", "[tabs][activate-tab][round4]") {
    FakeStrip strip;
    bool threw = false;
    try {
        TabActivationGuard guard([&](const std::string& id) { strip.selects.push_back(id); strip.selected = id; return true; });
        guard.record({"tabs/tabpB", "Logon data", strip.selected});  // recorded BEFORE selecting
        strip.selected = "tabs/tabpB";
        throw std::runtime_error("read failed");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(strip.selected == "tabs/tabpA");  // the destructor restored it while the exception unwound
    REQUIRE(strip.selects.size() == 1);
}

TEST_CASE("TabActivationGuard restores a tab whose own select threw (recorded first)", "[tabs][activate-tab][round4]") {
    FakeStrip strip;
    TabActivationGuard guard([&](const std::string& id) { strip.selects.push_back(id); strip.selected = id; return true; });
    guard.record({"tabs/tabpB", "B", "tabs/tabpA"});
    CHECK(guard.restore());
    CHECK(strip.selected == "tabs/tabpA");
    CHECK(guard.restored());
    CHECK(guard.restore_error().empty());
    // idempotent: a second restore (and the destructor) selects nothing more
    guard.restore();
    CHECK(strip.selects.size() == 1);
}

TEST_CASE("TabActivationGuard restores innermost first and reports a failing restore", "[tabs][activate-tab][round4]") {
    std::vector<std::string> order;
    TabActivationGuard guard([&](const std::string& id) {
        order.push_back(id);
        if (id == "outer/tabpPrev") throw std::runtime_error("COM went away");
        return true;
    });
    guard.record({"outer/tabpNew", "Outer", "outer/tabpPrev"});
    guard.record({"outer/tabpNew/inner/tabpNew2", "Inner", "outer/tabpNew/inner/tabpPrev2"});
    CHECK_FALSE(guard.restore());
    REQUIRE(order.size() == 2);
    CHECK(order[0] == "outer/tabpNew/inner/tabpPrev2");  // innermost first
    CHECK(order[1] == "outer/tabpPrev");
    CHECK_FALSE(guard.restored());
    CHECK(guard.restore_error().find("COM went away") != std::string::npos);
}

TEST_CASE("TabActivationGuard cannot restore an unknown previous tab or a page that is gone", "[tabs][activate-tab][round4]") {
    {
        TabActivationGuard guard([](const std::string&) { return true; });
        guard.record({"tabs/tabpB", "B", ""});
        CHECK_FALSE(guard.restore());
        CHECK_FALSE(guard.restore_error().empty());
    }
    {
        TabActivationGuard guard([](const std::string&) { return false; });  // page not found
        guard.record({"tabs/tabpB", "B", "tabs/tabpA"});
        CHECK_FALSE(guard.restore());
        CHECK(guard.restore_error().find("could not be found") != std::string::npos);
    }
    {
        TabActivationGuard idle([](const std::string&) -> bool { throw std::runtime_error("never called"); });
        CHECK(idle.restore());  // nothing was activated: nothing to restore
    }
}
