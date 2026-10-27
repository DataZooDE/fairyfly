// SemanticClassifier decides which screen elements are worth showing; ERR-137: unknown shells are never hidden.
#include <catch2/catch_test_macros.hpp>

#include "include/semantic_classifier.h"

using fairyfly::sap::SemanticClassifier;
using Category = SemanticClassifier::ElementCategory;
using json = nlohmann::json;

TEST_CASE("semantic classifier: controls are semantic, containers layout, splitters structural", "[semantic]") {
    for (const char* type : {"GuiTree", "GuiGridView", "GuiTableControl", "GuiButton", "GuiTextField", "GuiCTextField",
                             "GuiComboBox", "GuiCheckBox", "GuiRadioButton", "GuiLabel", "GuiTab", "GuiTabStrip",
                             "GuiTextedit"})
        CHECK(SemanticClassifier::classify(type, json::object()) == Category::SEMANTIC);
    for (const char* type : {"GuiContainerShell", "GuiCustomControl", "GuiSimpleContainer", "GuiScrollContainer",
                             "GuiDockShell", "GuiMenubar", "GuiOkCodeField", "GuiTitlebar", "GuiUserArea",
                             "GuiStatusbar"})
        CHECK(SemanticClassifier::classify(type, json::object()) == Category::LAYOUT);
    CHECK(SemanticClassifier::classify("GuiSplitterShell", json::object()) == Category::STRUCTURAL);
    CHECK(SemanticClassifier::classify("GuiBox", json::object()) == Category::STRUCTURAL);
    CHECK(SemanticClassifier::classify("GuiSomethingNew", json::object()) == Category::SEMANTIC);  // safe default
}

TEST_CASE("semantic classifier: a GuiShell is semantic by subtype or content, layout otherwise", "[semantic]") {
    for (const char* subtype : {"Tree", "Toolbar", "GridView", "TextEdit", "HTMLViewer", "Calendar"})
        CHECK(SemanticClassifier::classify("GuiShell", json{{"subtype", subtype}}) == Category::SEMANTIC);
    CHECK(SemanticClassifier::classify("GuiShell", json{{"tree_data", json::object()}}) == Category::SEMANTIC);
    CHECK(SemanticClassifier::classify("GuiShell", json{{"table_data", json::object()}}) == Category::SEMANTIC);
    CHECK(SemanticClassifier::classify("GuiShell", json{{"content_available", true}}) == Category::SEMANTIC);
    CHECK(SemanticClassifier::classify("GuiShell", json{{"tree_data", nullptr}}) == Category::LAYOUT);
    CHECK(SemanticClassifier::classify("GuiShell", json{{"subtype", "N/A"}}) == Category::LAYOUT);
    CHECK(SemanticClassifier::classify("GuiShell", json::object()) == Category::LAYOUT);
}

TEST_CASE("semantic classifier: layout is shown only with semantic descendants", "[semantic]") {
    const json empty = {{"type", "GuiContainerShell"}, {"children", json::array()}};
    CHECK_FALSE(SemanticClassifier::should_display(empty));
    const json nested = {{"type", "GuiContainerShell"},
                         {"children", json::array({{{"type", "GuiSimpleContainer"},
                                                    {"children", json::array({{{"type", "GuiButton"}}})}}})}};
    CHECK(SemanticClassifier::should_display(nested));
    const json only_layout = {{"type", "GuiSplitterShell"},
                              {"children", json::array({{{"type", "GuiContainerShell"}}})}};
    CHECK_FALSE(SemanticClassifier::should_display(only_layout));
    // children as id strings (the compact form) carry no type: nothing can be proven semantic
    CHECK_FALSE(SemanticClassifier::has_semantic_children(json{{"children", json::array({"wnd[0]/usr/btnX"})}}));
    CHECK(SemanticClassifier::should_display(json{{"type", "GuiButton"}}));
    CHECK_FALSE(SemanticClassifier::should_display(json("not an object")));
}

TEST_CASE("semantic classifier: layout-only types and descriptions", "[semantic]") {
    CHECK(SemanticClassifier::is_layout_only("GuiUserArea"));
    CHECK(SemanticClassifier::is_layout_only("GuiDockShell"));
    CHECK_FALSE(SemanticClassifier::is_layout_only("GuiShell"));
    CHECK_FALSE(SemanticClassifier::is_layout_only("GuiSplitterShell"));

    const json tree = {{"tree_data", {{"nodes", json::array({{{"children", json::array({json::object(), json::object()})}},
                                                             json::object()})}}}};
    CHECK(SemanticClassifier::get_semantic_description("GuiTree", tree) == "Tree (4 nodes)");
    CHECK(SemanticClassifier::get_semantic_description("GuiTree", json::object()) == "Tree");
    const json grid = {{"subtype", "GridView"}, {"table_data", {{"row_count", 3}, {"column_count", 2}}}};
    CHECK(SemanticClassifier::get_semantic_description("GuiShell", grid) == "GridView (3 rows \xC3\x97 2 cols)");
    CHECK(SemanticClassifier::get_semantic_description("GuiShell", json{{"subtype", "GridView"}}) == "GridView");
}
