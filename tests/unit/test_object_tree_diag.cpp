#include <catch2/catch_test_macros.hpp>
#include "include/object_tree_diag.h"

using fairyfly::diag::build_object_tree_diagnostic;
using fairyfly::diag::count_object_tree_nodes;
using fairyfly::diag::diag_enabled;

TEST_CASE("diagnostic dumps are only enabled with FAIRYFLY_DIAG=1", "[diag][bulk]") {
    REQUIRE_FALSE(diag_enabled(nullptr));
    REQUIRE_FALSE(diag_enabled(""));
    REQUIRE_FALSE(diag_enabled("0"));
    REQUIRE_FALSE(diag_enabled("true"));
    REQUIRE(diag_enabled("1"));
}

TEST_CASE("object tree node counting follows the children arrays", "[diag][bulk]") {
    const auto tree = nlohmann::json::parse(
        R"({"children":[{"Id":"wnd[0]","Type":"GuiMainWindow","children":[)"
        R"({"Id":"wnd[0]/usr","Type":"GuiUserArea","children":[{"Id":"wnd[0]/usr/btn","Type":"GuiButton","children":[]}]},)"
        R"({"Id":"wnd[0]/tbar[0]","Type":"GuiToolbar","children":[]}]}]})");
    REQUIRE(count_object_tree_nodes(tree) == 4);
    REQUIRE(count_object_tree_nodes(nlohmann::json::parse("{}")) == 0);
    REQUIRE(count_object_tree_nodes(nlohmann::json::parse("[1,2]")) == 0);
}

TEST_CASE("object tree diagnostic reports support, size and the parsed tree", "[diag][bulk]") {
    SECTION("a valid answer is parsed and counted") {
        const auto json = build_object_tree_diagnostic(
            "wnd[0]", {"Id", "Type"},
            [](const std::string& id, const std::vector<std::string>& props) -> std::optional<std::string> {
                REQUIRE(id == "wnd[0]");
                REQUIRE(props.size() == 2);
                return std::string(R"({"children":[{"Id":"wnd[0]","Type":"GuiMainWindow","children":[]}]})");
            });
        REQUIRE(json["supported"] == true);
        REQUIRE(json["id"] == "wnd[0]");
        REQUIRE(json["props"] == nlohmann::json::array({"Id", "Type"}));
        REQUIRE(json["node_count"] == 1);
        REQUIRE(json["bytes"] == 67);
        REQUIRE(json["tree"]["children"][0]["Id"] == "wnd[0]");
        REQUIRE(json.contains("call_ms"));
        REQUIRE_FALSE(json.contains("raw"));
    }
    SECTION("a missing method reports supported=false and no tree") {
        const auto json = build_object_tree_diagnostic(
            "wnd[0]", {}, [](const std::string&, const std::vector<std::string>&) { return std::nullopt; });
        REQUIRE(json["supported"] == false);
        REQUIRE_FALSE(json.contains("tree"));
        REQUIRE_FALSE(json.contains("raw"));
    }
    SECTION("an unparsable answer is returned raw with the parse error") {
        const auto json = build_object_tree_diagnostic(
            "wnd[0]", {},
            [](const std::string&, const std::vector<std::string>&) { return std::optional<std::string>("{not json"); });
        REQUIRE(json["supported"] == true);
        REQUIRE(json["raw"] == "{not json");
        REQUIRE(json.contains("parse_error"));
        REQUIRE_FALSE(json.contains("tree"));
    }
}
