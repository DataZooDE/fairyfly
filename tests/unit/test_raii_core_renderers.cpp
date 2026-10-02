#include "include/vkey.h"
#include "include/menu_navigation.h"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include "include/core.h"
#include "include/com/raii_helpers.h"
#include "include/com/utf8.h"
#include "include/element_renderer_registry.h"
#include "include/element_renderers.h"
#include "include/action_status.h"
#include "include/cli_handler.h"
#include "include/table_data_extractor.h"
#include "include/screen_reader.h"

using json = nlohmann::json;
using namespace fairyfly;
using namespace fairyfly::sap;

TEST_CASE("Action status classifies SAP rejections", "[status][actions]") {
    const ActionStatus before{"", ""};
    const ActionStatus readonly{"Action not possible in read-only mode", "S"};
    auto rejected = classify_action_status(before, readonly, "wnd[0]/usr/tree");
    REQUIRE(rejected.has_value());
    REQUIRE(rejected->error.at("code") == "ACTION_FAILED");
    REQUIRE_FALSE(classify_action_status(readonly, readonly, "wnd[0]/usr/tree").has_value());
    auto repeated = classify_action_status(readonly, readonly, "wnd[0]/usr/btnPUSHSHOW", true);
    REQUIRE(repeated.has_value());
    REQUIRE(repeated->error.at("code") == "ACTION_OUTCOME_UNVERIFIED");
    REQUIRE(classify_action_status(before, {"Invalid input", "E"}, "wnd[0]/usr/field").has_value());
    REQUIRE_FALSE(classify_action_status(before, {"Data saved", "S"}, "wnd[0]/usr/field").has_value());

    const ActionStatus warning{"Check entries before continuing", "W"};
    REQUIRE_FALSE(classify_action_status(before, warning, "wnd[0]/usr/field").has_value());
    // A warning after a submitting click is a message, not a failure (see "benign status messages" below).
    REQUIRE_FALSE(classify_action_status(before, warning, "wnd[0]/tbar[0]/btn[11]", true).has_value());
    REQUIRE_FALSE(classify_action_status(warning, warning, "wnd[0]/tbar[0]/btn[11]", true).has_value());
    auto denied_warning = classify_action_status(before, {"Not authorized", "W"},
                                                 "wnd[0]/tbar[0]/btn[11]", true);
    REQUIRE(denied_warning.has_value());
    REQUIRE(denied_warning->error.at("code") == "ACTION_FAILED");
    auto rejected_list_header = classify_action_status(before,
        {"Line cannot be selected", "S"}, "wnd[2]/usr/lbl[2,2]", true);
    REQUIRE(rejected_list_header.has_value());
    REQUIRE(rejected_list_header->error.at("code") == "ACTION_FAILED");
}

TEST_CASE("Benign W/I/S status messages after a click are success with the message", "[status][actions]") {
    const ActionStatus before{"", ""};
    const std::string button = "wnd[0]/usr/btnTODAY";
    for (const char* type : {"W", "I", "S"}) {
        INFO(type);
        const ActionStatus after{"No short dumps match the selection criteria", type};
        REQUIRE_FALSE(classify_action_status(before, after, button, true).has_value());
        REQUIRE_FALSE(classify_action_status(before, after, button, false).has_value());

        Result result;
        result.status = Result::Status::Success;
        attach_status_bar(result, before, after);
        REQUIRE(result.data["status_message"]["type"] == type);
        REQUIRE(result.data["status_message"]["text"] == "No short dumps match the selection criteria");
        REQUIRE(result.data.contains("warning") == (std::string(type) == "W"));
        if (std::string(type) == "W") REQUIRE(result.data["warning"] == true);
    }
    // a W message that was already there before the click is not flagged as a new warning
    Result stale;
    stale.status = Result::Status::Success;
    attach_status_bar(stale, {"Old warning", "W"}, {"Old warning", "W"});
    REQUIRE(stale.data["status_message"]["text"] == "Old warning");
    REQUIRE_FALSE(stale.data.contains("warning"));

    // E and A still fail, with distinct codes
    auto error = classify_action_status(before, {"Entry is invalid", "E"}, button, true);
    REQUIRE(error.has_value());
    REQUIRE(error->error.at("code") == "ACTION_FAILED");
    auto aborted = classify_action_status(before, {"Terminated", "A"}, button, true);
    REQUIRE(aborted.has_value());
    REQUIRE(aborted->error.at("code") == "ACTION_ABORTED");
    // a W that reads like a rejection still fails
    REQUIRE(classify_action_status(before, {"Not authorized for this", "W"}, button, true).has_value());
}

TEST_CASE("Positioned list hotspots use focus and F2", "[actions][labels]") {
    REQUIRE(should_activate_positioned_label("GuiLabel", "wnd[0]/usr/lbl[1,9]", 0, true, false));
    REQUIRE(should_activate_positioned_label("GuiLabel", "wnd[0]/usr/lbl[1,9]", 0, false, true));
    REQUIRE(should_activate_positioned_label("GuiLabel", "wnd[1]/usr/lbl[4,8]", 1, false, false));
    REQUIRE_FALSE(should_activate_positioned_label("GuiLabel", "wnd[0]/usr/lbl[1,5]", 0, false, false));
    REQUIRE_FALSE(should_activate_positioned_label("GuiButton", "wnd[0]/usr/lbl[1,9]", 0, true, true));
}

TEST_CASE("List label F2 does not claim success for an unchanged screen", "[actions][labels]") {
    const ActionStatus no_status{"", ""};
    auto unchanged = classify_list_label_outcome(true, true, true, no_status, no_status,
                                                  "wnd[0]/usr/lbl[0,0]");
    REQUIRE(unchanged.has_value());
    REQUIRE(unchanged->error.at("code") == "ACTION_OUTCOME_UNVERIFIED");
    REQUIRE_FALSE(classify_list_label_outcome(false, true, true, no_status, no_status,
                                              "wnd[0]/usr/lbl[1,9]").has_value());
    REQUIRE_FALSE(classify_list_label_outcome(true, false, true, no_status, no_status,
                                              "wnd[1]/usr/lbl[4,8]").has_value());
    REQUIRE_FALSE(classify_list_label_outcome(true, true, false, no_status, no_status,
                                              "wnd[0]/usr/lbl[1,9]").has_value());
    REQUIRE_FALSE(classify_list_label_outcome(true, true, true, no_status,
                                              {"Selection changed", "S"},
                                              "wnd[0]/usr/lbl[1,9]").has_value());
}

TEST_CASE("Transaction startup rejects SAP information modal", "[status][transaction]") {
    auto rejected = classify_transaction_modal("/app/con[1]/ses[0]/wnd[1]",
                                               "Cannot start transaction SESSION_MANAGER          ",
                                               "SESSION_MANAGER");
    REQUIRE(rejected.has_value());
    REQUIRE(rejected->error.at("code") == "TRANSACTION_FAILED");
    REQUIRE(rejected->error.at("message") == "Cannot start transaction SESSION_MANAGER");
    REQUIRE_FALSE(classify_transaction_modal("wnd[0]", "Cannot start transaction SESSION_MANAGER",
                                             "SESSION_MANAGER").has_value());
    REQUIRE_FALSE(classify_transaction_modal("wnd[1]", "Service was created",
                                             "SEGW").has_value());
    REQUIRE_FALSE(classify_transaction_modal("wnd[1]", "Cannot start transaction SM59",
                                             "SESSION_MANAGER").has_value());
}

TEST_CASE("F4 reports a missing search-help dialog as an error", "[status][f4]") {
    const auto element = "wnd[0]/usr/ctxtSUID_ST_NODE_LOGONDATA-USERALIAS";
    auto missing = classify_f4_outcome(false, {"No values found", "S"}, element);
    REQUIRE(missing.has_value());
    REQUIRE(missing->status == Result::Status::Error);
    REQUIRE(missing->error.at("code") == "F4_DIALOG_NOT_OPENED");
    REQUIRE(missing->error.at("message") == "No values found");
    REQUIRE(missing->error.at("element") == element);
    REQUIRE_FALSE(classify_f4_outcome(true, {}, element).has_value());
}

TEST_CASE("F4 rejects a field behind a modal and waits for the next dialog", "[status][f4]") {
    const auto element = "/app/con[0]/ses[0]/wnd[0]/usr/ctxtSUID_ST_BNAME-BNAME";
    auto mismatch = classify_f4_target_window("/app/con[0]/ses[0]/wnd[1]", element);
    REQUIRE(mismatch.has_value());
    REQUIRE(mismatch->error.at("code") == "WINDOW_MISMATCH");
    REQUIRE(mismatch->error.at("active_window") == "/app/con[0]/ses[0]/wnd[1]");
    REQUIRE_FALSE(classify_f4_target_window("/app/con[0]/ses[0]/wnd[0]", element).has_value());
    REQUIRE(f4_dialog_path(0) == "wnd[1]");
    REQUIRE(f4_dialog_path(1) == "wnd[2]");
}

TEST_CASE("Missing SAP elements reach window diagnostics", "[com][lookup]") {
    REQUIRE(is_missing_element_error("Element not found: /app/con[1]/ses[0]/wnd[1]/usr/btnX"));
    REQUIRE_FALSE(is_missing_element_error("Failed to access SAP session"));
    REQUIRE_FALSE(window_ids_match(WindowId("/app/con[1]/ses[0]/wnd[1]"),
                                   WindowId("/app/con[1]/ses[0]/wnd[0]")));
    REQUIRE(window_ids_match(WindowId("wnd[0]"),
                             WindowId("/app/con[1]/ses[0]/wnd[0]")));
    REQUIRE_FALSE(window_ids_match(WindowId("/app/con[2]/ses[0]/wnd[0]"),
                                   WindowId("/app/con[1]/ses[0]/wnd[0]")));
}

TEST_CASE("Screen filters update every returned element view", "[screen][filters]") {
    json data = {
        {"elements", json::array({
            {{"id", "button-a"}, {"type", "GuiButton"}},
            {{"id", "field"}, {"type", "GuiTextField"}},
            {{"id", "button-b"}, {"type", "GuiButton"}}
        })},
        {"element_count", 3},
        {"tabs_content", json::array({
            {{"tab_name", "General"},
             {"elements", json::array({{{"id", "tab-field"}, {"type", "GuiTextField"}},
                                       {{"id", "tab-button"}, {"type", "GuiButton"}}})},
             {"element_count", 2},
             {"hierarchy", {{"form_fields", json::array({{{"id", "tab-field"}, {"type", "GuiTextField"}}})},
                            {"buttons", json::array({{{"id", "tab-button"}, {"type", "GuiButton"}}})}}}}
        })},
        {"hierarchy", {{"buttons", json::array({{{"id", "button-a"}, {"type", "GuiButton"}},
                                                     {{"id", "button-b"}, {"type", "GuiButton"}}})},
                        {"form_fields", json::array({{{"id", "field"}, {"type", "GuiTextField"}}})}}}
    };
    cli::ScreenFilterOptions filters;
    filters.only_buttons = true;
    cli::apply_screen_filters(data, filters);
    REQUIRE(data.at("elements").size() == 2);
    REQUIRE(data.at("element_count") == 2);
    REQUIRE(data.at("hierarchy").at("buttons").size() == 2);
    REQUIRE_FALSE(data.at("hierarchy").contains("form_fields"));
    REQUIRE(data.at("elements").at(0).at("id") == "button-a");
    REQUIRE(data.at("elements").at(1).at("id") == "button-b");
    REQUIRE(data.at("filter_stats").at("filtered_element_count") == 2);
    REQUIRE(data.at("tabs_content").at(0).at("elements").size() == 1);
    REQUIRE(data.at("tabs_content").at(0).at("hierarchy").at("buttons").size() == 1);
    REQUIRE(data.at("tabs_content").at(0).at("element_count") == 1);
}

TEST_CASE("Screen filters find objects after string child references", "[screen][filters]") {
    json data = {
        {"elements", json::array({
            {{"id", "wnd[0]/usr"}, {"type", "GuiUserArea"},
             {"children", json::array({
                 "wnd[0]/usr/lbl[0,0]",
                 {{"id", "wnd[0]/usr/btnGO"}, {"type", "GuiButton"},
                  {"text", "Execute"}}
             })}}
        })},
        {"element_count", 1}
    };
    cli::ScreenFilterOptions filters;
    filters.only_buttons = true;
    cli::apply_screen_filters(data, filters);
    REQUIRE(data.at("element_count") == 1);
    REQUIRE(data.at("elements").at(0).at("id") == "wnd[0]/usr/btnGO");
    REQUIRE(data.at("filter_stats").at("total_elements_checked") == 2);
}

TEST_CASE("Only input fields excludes read-only text display elements", "[screen][filters]") {
    json data = {
        {"elements", json::array({
            {{"id", "caption"}, {"type", "GuiTextField"}, {"changeable", false}},
            {{"id", "user"}, {"type", "GuiCTextField"}, {"changeable", true}},
            {{"id", "display"}, {"type", "GuiCTextField"}, {"changeable", false}},
            {{"id", "password"}, {"type", "GuiPasswordField"}, {"changeable", true}},
            {{"id", "checkbox"}, {"type", "GuiCheckBox"}, {"changeable", true}}
        })},
        {"element_count", 5}
    };
    cli::ScreenFilterOptions filters;
    filters.only_fields = true;
    cli::apply_screen_filters(data, filters);
    REQUIRE(data.at("element_count") == 2);
    REQUIRE(data.at("elements").at(0).at("id") == "user");
    REQUIRE(data.at("elements").at(1).at("id") == "password");
}

TEST_CASE("Grid rows reuse one column-order snapshot", "[table][grid]") {
    const std::vector<std::string> columns{"SERVICE_NAME", "SERVICE_VERSION"};
    int cell_reads = 0;
    const auto rows = read_grid_rows(64, 2, 3, columns,
                                    [&](int row, const std::string& column) {
                                        ++cell_reads;
                                        return column + std::to_string(row);
                                    });
    REQUIRE(rows.size() == 3);
    REQUIRE(rows.at(2).at(0) == "SERVICE_NAME2");
    REQUIRE(rows.at(2).at(1) == "SERVICE_VERSION2");
    REQUIRE(cell_reads == 6);

    const auto later_rows = read_grid_rows(64, 2, 64, columns,
                                          [](int row, const std::string& column) {
                                              return column + std::to_string(row);
                                          });
    REQUIRE(later_rows.size() == 64);
    REQUIRE(later_rows.at(40).at(0) == "SERVICE_NAME40");
}

TEST_CASE("Screen reader bounds explicitly requested grid rows", "[screen][grid]") {
    ScreenReader reader(nullptr);
    const auto zero = reader.read(true, false, 0);
    REQUIRE(zero.status == Result::Status::Error);
    REQUIRE(zero.error.at("code") == "INVALID_MAX_ROWS");
    const auto excessive = reader.read(true, false, 201);
    REQUIRE(excessive.error.at("code") == "INVALID_MAX_ROWS");
}

TEST_CASE("UTF-8 COM boundary conversion", "[com][utf8]") {
    const std::string text = "Gr\xC3\xBC\xC3\x9F" "e \xE4\xB8\xAD\xE6\x96\x87";
    const auto wide = com::utf8_to_wide(text);
    REQUIRE(com::wide_to_utf8(wide) == text);
    BSTR bstr = SysAllocStringLen(wide.data(), static_cast<UINT>(wide.size()));
    REQUIRE(bstr != nullptr);
    REQUIRE(com::bstr_to_utf8(bstr) == text);
    SysFreeString(bstr);
    REQUIRE_THROWS_AS(com::utf8_to_wide(std::string("\xC3\x28", 2)), std::invalid_argument);
}

TEST_CASE("TableRenderer uses extracted row data", "[renderers][table]") {
    json table = {
        {"id", "wnd[0]/usr/grid"},
        {"type", "GuiShell"},
        {"subtype", "GridView"},
        {"table_data", {
            {"columns", json::array({"SERVICE_NAME", "DESCRIPTION"})},
            {"rows", json::array({json::array({"ZTEST_SRV", "Test service"})})},
            {"total_row_count", 1},
            {"visible_row_count", 1}
        }}
    };

    auto rendered = renderers::TableRenderer::to_markdown(table, 0);
    REQUIRE(rendered.find("SERVICE_NAME") != std::string::npos);
    REQUIRE(rendered.find("ZTEST_SRV") != std::string::npos);

    auto semantic = renderers::TableRenderer::to_json(table);
    REQUIRE(semantic.at("row_count") == 1);
    REQUIRE(semantic.at("column_count") == 2);

    table["table_data"] = nullptr;
    REQUIRE_NOTHROW(renderers::TableRenderer::to_json(table));
}

TEST_CASE("HTML viewer reports inaccessible content", "[renderers][html]") {
    renderers::register_all_renderers();
    json viewer = {
        {"id", "wnd[0]/usr/html"},
        {"type", "GuiShell"},
        {"subtype", "HTMLViewer"},
        {"text", "SAP.HTMLControl.1"}
    };
    auto& registry = ElementRendererRegistry::instance();
    auto rendered = registry.render_to_json(viewer);
    REQUIRE(rendered.at("subtype") == "HTMLViewer");
    REQUIRE(rendered.at("content_available") == false);
    const auto markdown = registry.render_to_markdown(viewer);
    REQUIRE(markdown.find("HTML viewer") != std::string::npos);
    REQUIRE(markdown.find("content unavailable") != std::string::npos);
    REQUIRE(markdown.find("SAP.HTMLControl.1") == std::string::npos);
}

TEST_CASE("HTML viewer renders accessible diagnostic text", "[renderers][html]") {
    renderers::register_all_renderers();
    json viewer = {
        {"id", "wnd[0]/usr/html"},
        {"type", "GuiShell"},
        {"subtype", "HTMLViewer"},
        {"content_available", true},
        {"text", "Comparing: T000 (Active RTOBJ), T000 (Active Source)"}
    };
    auto& registry = ElementRendererRegistry::instance();
    const auto rendered = registry.render_to_json(viewer);
    REQUIRE(rendered.at("content_available") == true);
    REQUIRE(rendered.at("text") == viewer.at("text"));
    const auto markdown = registry.render_to_markdown(viewer);
    REQUIRE(markdown.find("Comparing: T000") != std::string::npos);
    REQUIRE(markdown.find("content unavailable") == std::string::npos);

    viewer["text"] = "Authorization: Bearer FFLY_HTML_SECRET";
    REQUIRE(registry.render_to_json(viewer).dump().find("FFLY_HTML_SECRET") == std::string::npos);
    REQUIRE(registry.render_to_markdown(viewer).find("FFLY_HTML_SECRET") == std::string::npos);
}

TEST_CASE("VariantGuard stack memory safety", "[raii][variant]") {
    SECTION("Default constructor initializes VT_EMPTY and get() is valid") {
        com::VariantGuard guard;
        REQUIRE(guard.get() != nullptr);
        REQUIRE(guard.get()->vt == VT_EMPTY);
    }

    SECTION("External VARIANT pointer is managed correctly") {
        VARIANT v;
        VariantInit(&v);
        {
            com::VariantGuard guard(&v);
            REQUIRE(guard.get() == &v);
            guard.get()->vt = VT_I4;
            guard.get()->lVal = 42;
            REQUIRE(guard.get()->lVal == 42);
        }
        REQUIRE(v.vt == VT_EMPTY);
    }

    SECTION("Default constructed VariantGuard clears BSTR on destruction without leak") {
        {
            com::VariantGuard guard;
            guard.get()->vt = VT_BSTR;
            guard.get()->bstrVal = SysAllocString(L"test_string");
            REQUIRE(guard.get()->bstrVal != nullptr);
        }
        // Exiting scope clears the BSTR via VariantClear
        SUCCEED();
    }
}

TEST_CASE("WindowId and ElementId path validation", "[core][ids]") {
    SECTION("Standard wnd paths are valid") {
        WindowId wnd("wnd[0]");
        REQUIRE(wnd.is_valid());
        REQUIRE(wnd.get_index() == 0);

        ElementId elem("wnd[0]/usr/btn[1]");
        REQUIRE(elem.is_valid());
        REQUIRE(elem.get_window().id == "wnd[0]");
        REQUIRE(elem.get_element_path() == "usr/btn[1]");
    }

    SECTION("Special @active and @main selectors are valid") {
        WindowId active("@active");
        REQUIRE(active.is_valid());
        REQUIRE(active.is_active_selector());
        REQUIRE(active.get_index() == -1);

        ElementId elem("@active/usr/txt[0]");
        REQUIRE(elem.is_valid());
        REQUIRE(elem.get_window().id == "@active");
        REQUIRE(elem.get_element_path() == "usr/txt[0]");

        WindowId main_wnd("@main");
        REQUIRE(main_wnd.is_valid());
        REQUIRE(main_wnd.is_main_selector());
    }

    SECTION("Full SAP canonical paths starting with /app are valid") {
        WindowId full_wnd("/app/con[0]/ses[0]/wnd[0]");
        REQUIRE(full_wnd.is_valid());
        REQUIRE(full_wnd.get_index() == 0);

        WindowId full_wnd1("/app/con[0]/ses[0]/wnd[1]");
        REQUIRE(full_wnd1.is_valid());
        REQUIRE(full_wnd1.get_index() == 1);

        ElementId full_elem("/app/con[0]/ses[0]/wnd[0]/usr/btn[5]");
        REQUIRE(full_elem.is_valid());
        REQUIRE(full_elem.get_window().id == "/app/con[0]/ses[0]/wnd[0]");
        REQUIRE(full_elem.get_element_path() == "usr/btn[5]");
        REQUIRE(full_elem.with_window(WindowId("wnd[1]")).path == "wnd[1]/usr/btn[5]");
    }
}

TEST_CASE("ElementRendererRegistry non-object robustness", "[renderers][json]") {
    auto& registry = ElementRendererRegistry::instance();

    SECTION("render_to_json handles non-object metadata without throwing") {
        json string_child = "wnd[0]/usr/btn[1]";
        REQUIRE_NOTHROW(registry.render_to_json(string_child));

        json int_node = 42;
        REQUIRE_NOTHROW(registry.render_to_json(int_node));

        json obj_with_string_children = {
            {"id", "wnd[0]/usr"},
            {"type", "GuiUserArea"},
            {"children", json::array({"wnd[0]/usr/lbl[1]", "wnd[0]/usr/btn[2]"})}
        };
        REQUIRE_NOTHROW(registry.render_to_json(obj_with_string_children));
    }

    SECTION("render_to_markdown handles non-object metadata without throwing") {
        json string_child = "wnd[0]/usr/btn[1]";
        REQUIRE_NOTHROW(registry.render_to_markdown(string_child));

        json obj_with_string_children = {
            {"id", "wnd[0]/usr"},
            {"type", "GuiUserArea"},
            {"children", json::array({"wnd[0]/usr/lbl[1]", "wnd[0]/usr/btn[2]"})}
        };
        REQUIRE_NOTHROW(registry.render_to_markdown(obj_with_string_children));
    }
}

TEST_CASE("status_bar_json reports text, type and change", "[action][status_bar]") {
    for (const char* type : {"S", "W", "E", "A", "I"}) {
        const ActionStatus before{"old", "S"};
        const ActionStatus after{"Message text", type};
        const auto bar = status_bar_json(after, &before);
        REQUIRE(bar["text"] == "Message text");
        REQUIRE(bar["message_type"] == type);
        REQUIRE(bar["changed"] == true);
    }
    const ActionStatus same{"Same", "S"};
    REQUIRE(status_bar_json(same, &same)["changed"] == false);
    REQUIRE(status_bar_json({}, &same).is_null());
    REQUIRE_FALSE(status_bar_json(same).contains("changed"));
}

TEST_CASE("attach_status_bar targets data on success and error on failure", "[action][status_bar]") {
    const ActionStatus before{"", ""};
    const ActionStatus after{"Saved", "S"};
    Result ok;
    ok.status = Result::Status::Success;
    attach_status_bar(ok, before, after);
    REQUIRE(ok.data["status_bar"]["text"] == "Saved");
    REQUIRE(ok.data["status_bar"]["changed"] == true);

    Result failed;
    failed.status = Result::Status::Error;
    failed.error["code"] = "X";
    attach_status_bar(failed, before, {"Bad input", "E"});
    REQUIRE(failed.error["status_bar"]["message_type"] == "E");
    REQUIRE(failed.error["code"] == "X");

    Result quiet;
    quiet.status = Result::Status::Success;
    attach_status_bar(quiet, before, {});
    REQUIRE_FALSE(quiet.data.contains("status_bar"));
}

TEST_CASE("normalize_transaction_request handles OK-code prefixes", "[action][tcode]") {
    auto bare = normalize_transaction_request("/n");
    REQUIRE(bare.valid);
    REQUIRE(bare.use_send_command);
    REQUIRE(bare.command == "/n");
    REQUIRE(bare.expected_tcode == "SESSION_MANAGER");

    auto lower = normalize_transaction_request("/nSE38");
    REQUIRE(lower.valid);
    REQUIRE(lower.use_send_command);
    REQUIRE(lower.command == "/nSE38");
    REQUIRE(lower.expected_tcode == "SE38");

    auto upper = normalize_transaction_request("/NSM37");
    REQUIRE(upper.use_send_command);
    REQUIRE(upper.command == "/nSM37");
    REQUIRE(upper.expected_tcode == "SM37");

    auto plain = normalize_transaction_request("SE38");
    REQUIRE(plain.valid);
    REQUIRE_FALSE(plain.use_send_command);
    REQUIRE(plain.command == "SE38");
    REQUIRE(plain.expected_tcode == "SE38");

    for (const char* input : {"/o", "/oSE38", "/nex", "/NEX", "/i"}) {
        auto rejected = normalize_transaction_request(input);
        REQUIRE_FALSE(rejected.valid);
        REQUIRE(rejected.error_code == "UNSUPPORTED_OK_CODE");
    }
}

TEST_CASE("screen_snapshot_changed detects in-place screen changes", "[action][snapshot]") {
    const ScreenSnapshot base{"wnd[0]", "Job Overview", "SM37", ""};
    REQUIRE_FALSE(screen_snapshot_changed(base, base));
    auto titled = base;
    titled.title = "Job Log";
    REQUIRE(screen_snapshot_changed(base, titled));
    auto window = base;
    window.window_id = "wnd[1]";
    REQUIRE(screen_snapshot_changed(base, window));
    auto tcode = base;
    tcode.transaction = "SE38";
    REQUIRE(screen_snapshot_changed(base, tcode));
    auto status = base;
    status.statusbar_text = "Job log displayed";
    REQUIRE(screen_snapshot_changed(base, status));
}

TEST_CASE("Key names map to SAP virtual keys", "[vkey]") {
    using fairyfly::sap::parse_vkey;
    REQUIRE(parse_vkey("enter") == 0);
    REQUIRE(parse_vkey(" Enter ") == 0);
    REQUIRE(parse_vkey("f1") == 1);
    REQUIRE(parse_vkey("F8") == 8);
    REQUIRE(parse_vkey("f12") == 12);
    REQUIRE(parse_vkey("shift+f1") == 13);
    REQUIRE(parse_vkey("Shift+F12") == 24);
    REQUIRE(parse_vkey("15") == 15);
    REQUIRE(parse_vkey("0") == 0);
    REQUIRE_FALSE(parse_vkey("").has_value());
    REQUIRE_FALSE(parse_vkey("f13").has_value());
    REQUIRE_FALSE(parse_vkey("f0").has_value());
    REQUIRE_FALSE(parse_vkey("shift+enter").has_value());
    REQUIRE_FALSE(parse_vkey("banana").has_value());
    REQUIRE_FALSE(parse_vkey("100").has_value());
    REQUIRE_FALSE(parse_vkey("-1").has_value());
}

TEST_CASE("Menu path matching ignores accelerators and case", "[menu]") {
    using namespace fairyfly::sap;
    REQUIRE(normalize_menu_label(" &Runtime Errors ") == "runtime errors");
    REQUIRE(menu_label_matches("Runtime &Errors", "runtime errors"));
    REQUIRE_FALSE(menu_label_matches("Runtime Errors", "Runtime"));
    REQUIRE_FALSE(menu_label_matches("", ""));
    const auto path = split_menu_path(" Runtime Errors / Display ");
    REQUIRE(path == std::vector<std::string>{"Runtime Errors", "Display"});
    REQUIRE(split_menu_path("").empty());
    REQUIRE(split_menu_path("//A//").size() == 1);
}

TEST_CASE("Close outcome compares popup window indexes", "[close]") {
    using fairyfly::sap::CloseOutcome;
    using fairyfly::sap::classify_close_outcome;
    REQUIRE(classify_close_outcome(0, 0) == CloseOutcome::NoPopup);
    REQUIRE(classify_close_outcome(-1, -1) == CloseOutcome::NoPopup);
    REQUIRE(classify_close_outcome(1, 0) == CloseOutcome::Closed);
    REQUIRE(classify_close_outcome(2, 1) == CloseOutcome::Closed);
    REQUIRE(classify_close_outcome(1, 1) == CloseOutcome::StillOpen);
    REQUIRE(classify_close_outcome(1, 2) == CloseOutcome::StillOpen);
}
