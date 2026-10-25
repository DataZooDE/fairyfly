// Golden output of ScreenMarkdownFormatter::format: every fixture in fixtures/screen_markdown holds an input screen,
// the compact flag and the exact Markdown the formatter produced when the fixture was recorded (2026-10-25, before
// the formatter was split into sections). The split must not change a byte.
#include <catch2/catch_test_macros.hpp>

#include "include/element_renderers.h"
#include "include/formatters/screen_markdown_formatter.h"

#include <filesystem>
#include <fstream>
#include <iterator>

TEST_CASE("screen markdown: every recorded screen renders exactly as recorded", "[markdown][golden]") {
    namespace fs = std::filesystem;
    fairyfly::sap::renderers::register_all_renderers();  // as cli_entry does once per process
    const fs::path dir = fs::path(FAIRYFLY_UNIT_FIXTURES) / "screen_markdown";
    REQUIRE(fs::is_directory(dir));
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const auto fixture = nlohmann::json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        CAPTURE(entry.path().filename().string());
        CHECK(fairyfly::cli::ScreenMarkdownFormatter::format(fixture.at("input"), fixture.at("compact").get<bool>()) ==
              fixture.at("output").get<std::string>());
        ++checked;
    }
    CHECK(checked >= 34);
}
