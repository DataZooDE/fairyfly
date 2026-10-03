#include <catch2/catch_test_macros.hpp>
#include "include/element_metadata_builder.h"

using namespace fairyfly::sap;
using nlohmann::json;

namespace {
ElementFacts facts(const char* type, const char* id) {
    ElementFacts f;
    f.type = type;
    f.id = id;
    return f;
}
}  // namespace

TEST_CASE("plain metadata: text field", "[metadata][builder]") {
    auto f = facts("GuiTextField", "wnd[0]/usr/txtBNAME");
    f.name = "BNAME";
    f.text = "Miller";
    f.changeable = true;
    f.label = "User";
    f.tooltip = "User name";
    CHECK(build_plain_metadata(f) == json::parse(R"({
        "id":"wnd[0]/usr/txtBNAME","type":"GuiTextField","name":"BNAME","text":"Miller",
        "enabled":true,"visible":true,"changeable":true,"label":"User","tooltip":"User name",
        "capabilities":["fillable","readable"]})"));
}

TEST_CASE("plain metadata: ctext field adds F4 help", "[metadata][builder]") {
    auto f = facts("GuiCTextField", "wnd[0]/usr/ctxtBNAME");
    f.name = "BNAME";
    f.text = "Miller";
    f.changeable = true;
    const auto metadata = build_plain_metadata(f);
    CHECK(metadata.at("has_f4_help") == true);
    CHECK(metadata.at("capabilities") == json::array({"fillable", "readable", "has_f4_help"}));
    CHECK_FALSE(metadata.contains("label"));
    CHECK_FALSE(metadata.contains("tooltip"));
}

TEST_CASE("plain metadata: label with grid coordinates", "[metadata][builder]") {
    auto f = facts("GuiLabel", "wnd[0]/usr/lbl[12,3]");
    f.text = "Hello";
    f.label = "ignored: labels carry no accessibility label";
    f.tooltip = "ignored";
    const auto metadata = build_plain_metadata(f);
    CHECK(metadata.at("grid_col") == 12);
    CHECK(metadata.at("grid_row") == 3);
    CHECK(metadata.at("changeable") == false);
    CHECK_FALSE(metadata.contains("label"));
    CHECK_FALSE(metadata.contains("tooltip"));
    CHECK_FALSE(metadata.contains("container_type"));

    SECTION("an unparsable position is skipped") {
        auto bad = facts("GuiLabel", "wnd[0]/usr/lbl[a,b]");
        const auto result = build_plain_metadata(bad);
        CHECK_FALSE(result.contains("grid_col"));
        CHECK_FALSE(result.contains("grid_row"));
    }
}

TEST_CASE("plain metadata: button", "[metadata][builder]") {
    auto f = facts("GuiButton", "wnd[0]/tbar[0]/btn[0]");
    f.name = "btn[0]";
    f.text = "Execute";
    f.tooltip = "Execute (F8)";
    f.label = "dropped for buttons";
    f.enabled = false;
    const auto metadata = build_plain_metadata(f);
    CHECK(metadata.at("tooltip") == "Execute (F8)");
    CHECK(metadata.at("enabled") == false);
    CHECK_FALSE(metadata.contains("label"));
    CHECK(metadata.at("capabilities").is_array());
}

TEST_CASE("plain metadata: checkbox reports selected", "[metadata][builder]") {
    auto f = facts("GuiCheckBox", "wnd[0]/usr/chkX");
    f.changeable = true;
    f.selected = true;
    f.label = "Lock";
    CHECK(build_plain_metadata(f).at("selected") == true);
    f.selected = false;
    CHECK(build_plain_metadata(f).at("selected") == false);
    f.selected.reset();
    CHECK_FALSE(build_plain_metadata(f).contains("selected"));

    SECTION("types without a selection ignore the value") {
        auto text = facts("GuiTextField", "wnd[0]/usr/txtX");
        text.selected = true;
        CHECK_FALSE(build_plain_metadata(text).contains("selected"));
    }
}

TEST_CASE("plain metadata: group box", "[metadata][builder]") {
    auto f = facts("GuiBox", "wnd[0]/usr/boxB");
    f.container_type = "group";
    const auto metadata = build_plain_metadata(f);
    CHECK(metadata.at("is_group") == true);
    CHECK(metadata.at("container_type") == "group");
    CHECK(metadata.at("capabilities") == json::array({"container"}));
}

TEST_CASE("plain metadata: container children and the 50-child cap", "[metadata][builder]") {
    auto f = facts("GuiSimpleContainer", "wnd[0]/usr/subSUB");
    f.container_type = "form";
    SECTION("no children, no keys") {
        const auto metadata = build_plain_metadata(f);
        CHECK_FALSE(metadata.contains("children"));
        CHECK_FALSE(metadata.contains("child_count"));
    }
    SECTION("children keep their order") {
        f.child_ids = {"wnd[0]/usr/txtA", "wnd[0]/usr/txtB"};
        const auto metadata = build_plain_metadata(f);
        CHECK(metadata.at("children") == json::array({"wnd[0]/usr/txtA", "wnd[0]/usr/txtB"}));
        CHECK(metadata.at("child_count") == 2);
    }
    SECTION("exactly 50 children are all emitted") {
        for (int i = 0; i < 50; ++i) f.child_ids.push_back("c" + std::to_string(i));
        const auto metadata = build_plain_metadata(f);
        CHECK(metadata.at("children").size() == 50);
        CHECK(metadata.at("child_count") == 50);
    }
    SECTION("more than 50 children are capped") {
        for (int i = 0; i < 80; ++i) f.child_ids.push_back("c" + std::to_string(i));
        const auto metadata = build_plain_metadata(f);
        CHECK(metadata.at("children").size() == 50);
        CHECK(metadata.at("children").back() == "c49");
        CHECK(metadata.at("child_count") == 50);
    }
}

TEST_CASE("plain metadata: tab", "[metadata][builder]") {
    auto f = facts("GuiTab", "wnd[0]/usr/tabsTS/tabpA");
    f.name = "tabpA";
    f.text = "Address";
    f.tooltip = "Address data";
    f.container_type = "container";
    f.child_ids = {"wnd[0]/usr/tabsTS/tabpA/txtX"};
    const auto metadata = build_plain_metadata(f);
    CHECK(metadata.at("tooltip") == "Address data");
    CHECK(metadata.at("container_type") == "container");
    CHECK(metadata.at("child_count") == 1);
    CHECK_FALSE(metadata.contains("label"));
}

TEST_CASE("plain metadata: shell subtype placeholder", "[metadata][builder]") {
    auto f = facts("GuiShell", "wnd[0]/shellcont/shell");
    CHECK(build_plain_metadata(f).at("subtype") == "N/A");
    f.subtype = "GridView";
    CHECK(build_plain_metadata(f).at("subtype") == "GridView");
}

TEST_CASE("metadata read predicates match the plain metadata", "[metadata][builder]") {
    CHECK(metadata_reads_changeable("GuiOkCodeField"));
    CHECK_FALSE(metadata_reads_changeable("GuiButton"));
    CHECK(metadata_reads_selected("GuiRadioButton"));
    CHECK_FALSE(metadata_reads_selected("GuiTab"));
    CHECK(metadata_reads_label("GuiComboBoxControl"));
    CHECK_FALSE(metadata_reads_label("GuiButton"));
    CHECK(metadata_reads_tooltip("GuiTab"));
    CHECK_FALSE(metadata_reads_tooltip("GuiCheckBox"));
}
