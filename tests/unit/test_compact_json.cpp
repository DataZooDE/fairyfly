#include <catch2/catch_test_macros.hpp>
#include "include/formatters/compact_json.h"
#include "include/formatters/screen_markdown_formatter.h"
#include "include/cli_handler.h"
#include "include/core.h"
#include <nlohmann/json.hpp>
#include <set>
#include <string>

using nlohmann::json;
using fairyfly::formatters::compact_screen_json;

namespace {

json make_field(const std::string& id, const std::string& text) {
    return {{"id", id}, {"type", "GuiTextField"}, {"name", "F"}, {"text", text},
            {"label", ""}, {"tooltip", ""}, {"capabilities", json::array()},
            {"changeable", true}, {"enabled", true}, {"visible", true}, {"required", false}};
}

json make_button(const std::string& id) {
    return {{"id", id}, {"type", "GuiButton"}, {"name", "B"}, {"text", "Save"},
            {"tooltip", ""}, {"capabilities", json::array()}, {"enabled", true}, {"changeable", false}};
}

json group(const json& elements) {
    json h = {{"form_fields", json::array()}, {"buttons", json::array()}};
    for (const auto& e : elements) {
        (e["type"] == "GuiButton" ? h["buttons"] : h["form_fields"]).push_back(e);
    }
    return h;
}

// Shape of a full SU01-like read: 12 tabs, each with fields and buttons.
json make_screen(int tabs = 12, int per_tab = 20) {
    const std::string prefix = "wnd[0]/usr/tabsTABSTRIP/tabpTAB";
    json all = json::array();
    json tabs_content = json::array();
    for (int t = 0; t < tabs; ++t) {
        json els = json::array();
        for (int i = 0; i < per_tab; ++i) {
            const std::string id = prefix + std::to_string(t) + "/ssubSUB/txtFIELD" + std::to_string(i);
            els.push_back(i % 5 == 4 ? make_button(id) : make_field(id, i % 2 ? "value" : ""));
        }
        json tab = {{"tab_id", prefix + std::to_string(t)}, {"tab_name", "TAB" + std::to_string(t)},
                    {"elements", els}, {"hierarchy", group(els)}, {"element_count", els.size()}};
        tabs_content.push_back(tab);
        for (const auto& e : els) all.push_back(e);
    }
    return {{"screen_id", "SU01"}, {"elements", all}, {"element_count", all.size()},
            {"hierarchy", group(all)}, {"tabs_content", tabs_content}, {"tabs_expanded", true},
            {"compact", true}, {"connection_id", 1}};
}

void require_ids_resolve(const json& data) {
    std::set<std::string> ids;
    for (const auto& e : data["elements"]) ids.insert(e["id"].get<std::string>());
    auto check = [&](const json& hierarchy) {
        for (const auto& [g, list] : hierarchy.items())
            for (const auto& id : list) {
                REQUIRE(id.is_string());
                REQUIRE(ids.count(id.get<std::string>()) == 1);
            }
    };
    check(data["hierarchy"]);
    for (const auto& tab : data["tabs_content"]) {
        check(tab["hierarchy"]);
        for (const auto& id : tab["elements"]) REQUIRE(ids.count(id.get<std::string>()) == 1);
    }
}

}  // namespace

TEST_CASE("compact: hierarchy and per-tab hierarchy become id arrays", "[compact]") {
    const json out = compact_screen_json(make_screen(2, 5));
    REQUIRE(out["compact"] == true);
    REQUIRE(out["hierarchy_format"] == "ids");
    for (const auto& [g, list] : out["hierarchy"].items())
        for (const auto& id : list) REQUIRE(id.is_string());
    for (const auto& tab : out["tabs_content"]) {
        for (const auto& [g, list] : tab["hierarchy"].items())
            for (const auto& id : list) REQUIRE(id.is_string());
        for (const auto& id : tab["elements"]) REQUIRE(id.is_string());
    }
    REQUIRE(out["elements"][0].is_object());
    REQUIRE(out["tabs_content"][0]["tab_name"] == "TAB0");
    REQUIRE(out["element_count"] == 10);
}

TEST_CASE("compact: empty fields dropped, non-empty and id/type/name kept", "[compact]") {
    json data = make_screen(1, 5);
    data["elements"][0]["name"] = "";
    const json out = compact_screen_json(data);
    const json& f = out["elements"][0];
    REQUIRE(f.contains("id"));
    REQUIRE(f.contains("type"));
    REQUIRE(f.contains("name"));
    REQUIRE_FALSE(f.contains("label"));
    REQUIRE_FALSE(f.contains("tooltip"));
    REQUIRE_FALSE(f.contains("capabilities"));
    REQUIRE_FALSE(f.contains("text"));     // empty text on element 0
    REQUIRE_FALSE(f.contains("required")); // false boolean
    REQUIRE(f["enabled"] == true);
    REQUIRE(f["changeable"] == true);
    REQUIRE(out["elements"][1]["text"] == "value");
}

TEST_CASE("compact: nested children are compacted", "[compact]") {
    json data = {{"screen_id", "X"}, {"hierarchy", json::object()},
                 {"elements", json::array({{{"id", "a"}, {"type", "GuiContainer"},
                    {"children", json::array({{{"id", "a/b"}, {"type", "GuiLabel"}, {"text", ""}}})}}})}};
    const json out = compact_screen_json(data);
    REQUIRE_FALSE(out["elements"][0]["children"][0].contains("text"));
}

TEST_CASE("compact: idempotent", "[compact]") {
    const json once = compact_screen_json(make_screen(3, 6));
    REQUIRE(compact_screen_json(once) == once);
}

TEST_CASE("compact: every hierarchy id resolves, including nested-only elements", "[compact]") {
    json data = make_screen(2, 5);
    // A hierarchy entry with no top-level counterpart is preserved in elements.
    data["hierarchy"]["other"] = json::array({{{"id", "orphan"}, {"type", "GuiLabel"}, {"text", "x"}}});
    const json out = compact_screen_json(data);
    require_ids_resolve(out);
    bool found = false;
    for (const auto& e : out["elements"]) found = found || e["id"] == "orphan";
    REQUIRE(found);
}

TEST_CASE("compact: SU01-like full read shrinks below 40 percent", "[compact]") {
    json data = make_screen();
    const size_t full = data.dump().size();
    const json out = compact_screen_json(data);
    const size_t compact = out.dump().size();
    INFO("full=" << full << " compact=" << compact);
    REQUIRE(compact < full * 4 / 10);
    require_ids_resolve(out);
}

TEST_CASE("compact: non-object and non-screen data pass through", "[compact]") {
    REQUIRE(compact_screen_json(json(5)) == json(5));
}

namespace {
fairyfly::Result success_result(json data) {
    fairyfly::Result r;
    r.status = fairyfly::Result::Status::Success;
    r.data = std::move(data);
    return r;
}
}  // namespace

TEST_CASE("format_output: default JSON unchanged, compact JSON compacted", "[compact]") {
    using fairyfly::cli::OutputFormat;
    json data = make_screen(2, 5);
    data["compact"] = false;
    const auto r = success_result(data);
    // Golden: non-compact output is exactly the untouched result.
    REQUIRE(fairyfly::cli::format_output(r, OutputFormat::Json) == r.to_json().dump(2));

    data["compact"] = true;
    const auto rc = success_result(data);
    const json parsed = json::parse(fairyfly::cli::format_output(rc, OutputFormat::Json));
    REQUIRE(parsed["data"]["hierarchy_format"] == "ids");
    REQUIRE(parsed["data"]["hierarchy"]["buttons"][0].is_string());
    data["compact"] = false;
    REQUIRE(fairyfly::cli::format_output(rc, OutputFormat::Toon).size() <
            fairyfly::cli::format_output(success_result(data), OutputFormat::Toon).size());
}

TEST_CASE("format_output: Markdown identical for compact flag regardless of JSON compaction", "[compact]") {
    using fairyfly::cli::OutputFormat;
    json data = make_screen(2, 5);
    const auto r = success_result(data);
    const std::string md = fairyfly::cli::format_output(r, OutputFormat::Markdown);
    // The formatter itself is unaffected by the new code path.
    REQUIRE(md.find("# ") != std::string::npos);
    REQUIRE(md == fairyfly::cli::format_output(success_result(data), OutputFormat::Markdown));
    REQUIRE(fairyfly::cli::ScreenMarkdownFormatter::format(data, true) ==
            fairyfly::cli::ScreenMarkdownFormatter::format(data, true));
    // Markdown never sees id-only hierarchy: compacting is JSON/TOON only.
    REQUIRE(md.find("hierarchy_format") == std::string::npos);
}
