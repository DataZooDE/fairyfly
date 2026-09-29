#include <catch2/catch_test_macros.hpp>

#include <CLI/CLI.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "include/command_table.h"
#include "include/commands/cli_app.h"
#include "include/commands/global_options.h"
#include "include/mcp/dispatcher.h"
#include "include/mcp/run_mcp.h"
#include "include/mcp/tool_catalog.h"

using namespace fairyfly;
namespace ct = fairyfly::command_table;
using Argv = std::vector<std::string>;

namespace {

struct DryParse {
    bool ok = false;
    bool help = false;
    std::string error;
    std::string path;
    commands::GlobalOptions global;
    std::string help_text;
};

/// Parses `args` (no program name) against the real CLI11 tree, without executing anything.
DryParse dry_parse(const Argv& args) {
    DryParse out;
    CLI::App app{"fairyfly"};
    commands::add_global_options(app, out.global);
    commands::build_command_tree(app);
    Argv full{"fairyfly"};
    full.insert(full.end(), args.begin(), args.end());
    std::vector<const char*> ptrs;
    for (const auto& a : full) ptrs.push_back(a.c_str());
    try {
        app.parse(static_cast<int>(ptrs.size()), ptrs.data());
        out.ok = true;
    } catch (const CLI::CallForHelp&) {
        out.ok = true;
        out.help = true;
    } catch (const CLI::ParseError& e) {
        out.error = e.what();
    }
    out.path = commands::invoked_command_path(app);
    out.help_text = app.help();
    return out;
}

std::string join(const Argv& parts, const std::string& sep) {
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : sep) + p;
    return out;
}

} // namespace

TEST_CASE("command table: every entry resolves to a registered CLI path", "[command_table]") {
    for (const auto& spec : ct::all_commands()) {
        INFO(spec.path_string());
        Argv args = spec.path;
        args.push_back("--help");
        const auto parsed = dry_parse(args);
        CHECK(parsed.ok);
        CHECK(parsed.help);
        CHECK(parsed.path == spec.path_string());
    }
}

TEST_CASE("command table: tool names, families and uniqueness", "[command_table]") {
    std::set<std::string> paths, tools;
    std::set<std::string> known_families;
    for (const auto& spec : ct::all_commands()) {
        INFO(spec.path_string());
        CHECK(paths.insert(spec.path_string()).second);
        CHECK_FALSE(spec.summary.empty());
        if (spec.path.size() == 1 && spec.path.front() != "mcp") {
            CHECK(spec.help_group == "SYSTEM");  // root verbs: doctor, batch
        } else {
            REQUIRE(ct::find_group(spec.path.front()) != nullptr);
            CHECK(spec.help_group == ct::find_group(spec.path.front())->help_group);
        }
        if (!spec.is_tool()) {
            CHECK(spec.family.empty());
            continue;
        }
        CHECK(tools.insert(spec.tool_name).second);
        CHECK(spec.tool_name == ct::derived_tool_name(spec.path));  // no exceptions to the derivation rule
        CHECK(spec.tool_name == "gui_" + join(spec.path, "_"));
        if (spec.path.front() == "doctor") CHECK(spec.family == "system");
        else if (spec.path.front() == "batch") CHECK(spec.family == "batch");
        else CHECK(spec.family == spec.path.front());
        known_families.insert(spec.family);
        CHECK(ct::find_by_tool(spec.tool_name) == &spec);
        CHECK(ct::find_by_path(spec.path) == &spec);
    }
    const auto families = ct::families();
    CHECK(std::set<std::string>(families.begin(), families.end()) == known_families);
    CHECK(families.size() == known_families.size());
    CHECK(ct::find_by_tool("sap_click") == nullptr);
    CHECK(ct::find_by_tool("") == nullptr);
}

TEST_CASE("command table: catalog and table agree", "[command_table][mcp]") {
    const auto specs = mcp::all_tool_specs();
    std::set<std::string> catalog_names;
    for (const auto& spec : specs) {
        INFO(spec.def.name);
        catalog_names.insert(spec.def.name);
        const auto* command = ct::find_by_tool(spec.def.name);
        REQUIRE(command != nullptr);
        CHECK(spec.family == command->family);
    }
    std::set<std::string> table_names;
    for (const auto& spec : ct::all_commands())
        if (spec.is_tool()) table_names.insert(spec.tool_name);
    CHECK(catalog_names == table_names);
    // Old names are gone.
    for (const auto& spec : specs) CHECK(spec.def.name.rfind("sap_", 0) != 0);
}

TEST_CASE("root help lists every registered group and the global flags", "[command_table][help]") {
    const auto parsed = dry_parse({"--help"});
    REQUIRE(parsed.help);
    const std::string& help = parsed.help_text;
    for (const auto& section : ct::help_sections()) {
        INFO(section);
        CHECK(help.find("\n" + section + ":") != std::string::npos);
    }
    CHECK(help.find("GLOBAL FLAGS:") != std::string::npos);
    for (const auto& group : ct::groups()) {
        INFO(group.noun);
        CHECK(help.find("\n  " + group.noun + " ") != std::string::npos);
    }
    CHECK(help.find("\n  doctor ") != std::string::npos);
    CHECK(help.find("\n  batch ") != std::string::npos);
    // Sections appear in the documented order.
    std::size_t last = 0;
    for (const auto& section : ct::help_sections()) {
        const auto pos = help.find("\n" + section + ":");
        REQUIRE(pos != std::string::npos);
        CHECK(pos >= last);
        last = pos;
    }
}

TEST_CASE("global options also work after the noun/verb", "[command_table][cli]") {
    auto parsed = dry_parse({"screen", "read", "--read-only"});
    REQUIRE(parsed.ok);
    CHECK(parsed.path == "screen read");
    CHECK(parsed.global.read_only);

    parsed = dry_parse({"element", "click", "wnd[0]/usr/btn", "--read-only", "--log-level", "debug", "--no-audit"});
    REQUIRE(parsed.ok);
    CHECK(parsed.path == "element click");
    CHECK(parsed.global.read_only);
    CHECK(parsed.global.log_level == "debug");
    CHECK(parsed.global.no_audit);

    parsed = dry_parse({"menu", "list", "--verbose-errors", "--output", "toon"});
    REQUIRE(parsed.ok);
    CHECK(parsed.global.verbose_errors);
    CHECK(parsed.global.output_format == "toon");

    parsed = dry_parse({"--read-only", "menu", "list"});
    REQUIRE(parsed.ok);
    CHECK(parsed.global.read_only);

    parsed = dry_parse({"doctor", "--read-only"});
    REQUIRE(parsed.ok);
    CHECK(parsed.global.read_only);

    parsed = dry_parse({"element", "fill", "wnd[0]/usr/txtX", "value", "--connection", "2", "--read-only"});
    REQUIRE(parsed.ok);
    CHECK(parsed.global.read_only);
}

TEST_CASE("old flat command names are gone", "[command_table][cli]") {
    for (const char* old : {"list", "attach", "launch", "login", "disconnect", "connections", "click", "fill", "get",
                            "press_f4", "send-key", "close", "tcode", "serve"}) {
        INFO(old);
        const auto parsed = dry_parse({old});
        CHECK_FALSE(parsed.ok);
        CHECK(parsed.error.find("not expected") != std::string::npos);
    }
    CHECK_FALSE(dry_parse({"screen", "menu"}).ok);          // split into `menu list` / `menu select`
    CHECK_FALSE(dry_parse({"credentials"}).ok);              // a noun alone needs a verb
    CHECK_FALSE(dry_parse({"session"}).ok);
}

TEST_CASE("menu list and menu select keep their options", "[command_table][cli]") {
    auto parsed = dry_parse({"menu", "list", "--window", "@active", "--connection", "1"});
    REQUIRE(parsed.ok);
    CHECK(parsed.path == "menu list");
    parsed = dry_parse({"menu", "select", "Runtime Errors/Display", "--window", "@active", "--connection", "1"});
    REQUIRE(parsed.ok);
    CHECK(parsed.path == "menu select");
    CHECK_FALSE(dry_parse({"menu", "select"}).ok);  // the path is required
}

TEST_CASE("mcp is a command with an optional subcommand", "[command_table][cli]") {
    auto parsed = dry_parse({"mcp"});
    REQUIRE(parsed.ok);
    CHECK(parsed.path == "mcp");
    parsed = dry_parse({"mcp", "--allow-write", "--tools", "session,screen", "--format", "json"});
    REQUIRE(parsed.ok);
    CHECK(parsed.path == "mcp");
    parsed = dry_parse({"mcp", "tools", "--markdown"});
    REQUIRE(parsed.ok);
    CHECK(parsed.path == "mcp tools");
}

TEST_CASE("family list parsing and command_of_argv", "[command_table]") {
    CHECK(ct::parse_family_list("session,screen") == Argv{"session", "screen"});
    CHECK(ct::parse_family_list(" Session  screen,, ELEMENT ") == Argv{"session", "screen", "element"});
    CHECK(ct::parse_family_list("screen,screen") == Argv{"screen"});
    CHECK(ct::parse_family_list("").empty());
    CHECK(ct::unknown_families({"screen", "nope", "system"}) == Argv{"nope"});
    CHECK(ct::unknown_families({}).empty());

    CHECK(ct::command_of_argv({"element", "click", "wnd[0]/x"}) == "element click");
    CHECK(ct::command_of_argv({"element", "fill", "--", "e", "v"}) == "element fill");
    CHECK(ct::command_of_argv({"doctor"}) == "doctor");
    CHECK(ct::command_of_argv({"batch", "--file", "x"}) == "batch");
    CHECK(ct::command_of_argv({"mcp", "tools"}) == "mcp tools");
    CHECK(ct::command_of_argv({"mcp", "--allow-write"}) == "mcp");
    CHECK(ct::command_of_argv({"credentials", "list"}) == "credentials list");
    CHECK(ct::command_of_argv({"unknown", "thing"}) == "unknown");
    CHECK(ct::command_of_argv({}).empty());
}

TEST_CASE("--tools filter removes tools from list and call", "[command_table][mcp]") {
    const auto filtered = mcp::retain_families(mcp::all_tool_specs(), {"screen", "system"});
    REQUIRE_FALSE(filtered.empty());
    for (const auto& spec : filtered) CHECK((spec.family == "screen" || spec.family == "system"));
    CHECK(mcp::retain_families(mcp::all_tool_specs(), {}).size() == mcp::all_tool_specs().size());

    mcp::Policy policy;
    policy.read_only = false;
    policy.allow_write = true;
    int invoked = 0;
    mcp::CommandDispatcher dispatcher(
        [&invoked](const Argv&) {
            ++invoked;
            Result r;
            r.status = Result::Status::Success;
            return r;
        },
        policy, nullptr, filtered);

    std::set<std::string> listed;
    for (const auto& def : dispatcher.list_tools()) listed.insert(def.name);
    CHECK(listed == std::set<std::string>{"gui_screen_read", "gui_screen_find", "gui_screen_capture", "gui_doctor"});
    CHECK(dispatcher.has_tool("gui_screen_read"));
    CHECK_FALSE(dispatcher.has_tool("gui_element_click"));
    CHECK_FALSE(dispatcher.has_tool("gui_element_fill"));  // write tools of other families are gone entirely
    CHECK_FALSE(dispatcher.has_tool("gui_batch"));
    CHECK_FALSE(dispatcher.has_tool("sap_screen_read"));  // legacy names are not accepted

    mcp::CallContext ctx;
    const auto refused = dispatcher.call_tool("gui_element_click", mcp::json{{"element", "wnd[0]/usr/btn"}}, ctx);
    CHECK(refused.is_error);
    CHECK(refused.content.at(0).at("text").get<std::string>().find("TOOL_NOT_FOUND") != std::string::npos);
    CHECK(invoked == 0);
}

TEST_CASE("run_mcp rejects an unknown family with exit code 99", "[command_table][mcp]") {
    mcp::ServeOptions options;
    options.families = {"screen", "bogus"};
    bool handler_requested = false;
    const int code = mcp::run_mcp(
        options,
        [&handler_requested]() -> cli::CommandHandler& {
            handler_requested = true;
            throw std::logic_error("must not be called");
        },
        commands::GlobalOptions{});
    CHECK(code == 99);
    CHECK_FALSE(handler_requested);
}

TEST_CASE("mcp tools table lists every tool and matches docs/MCP.md", "[command_table][mcp][docs]") {
    const std::string markdown = mcp::tool_table_text(true);
    const std::string plain = mcp::tool_table_text(false);
    for (const auto& spec : ct::all_commands()) {
        if (!spec.is_tool()) continue;
        INFO(spec.tool_name);
        CHECK(markdown.find("`" + spec.tool_name + "`") != std::string::npos);
        CHECK(plain.find(spec.tool_name) != std::string::npos);
        CHECK(markdown.find("`fairyfly " + spec.path_string() + "`") != std::string::npos);
    }
    CHECK(markdown.find("sap_") == std::string::npos);
    CHECK(markdown.find("| `gui_element_fill` | element | `fairyfly element fill` | yes |") != std::string::npos);

    // docs/MCP.md embeds the generated table verbatim between two markers.
    const std::filesystem::path doc = std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "docs" / "MCP.md";
    std::ifstream in(doc, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    const std::string begin_marker = "<!-- tool-table:begin -->\n";
    const std::string end_marker = "<!-- tool-table:end -->";
    const auto begin = text.find(begin_marker);
    const auto end = text.find(end_marker);
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    CHECK(text.substr(begin + begin_marker.size(), end - begin - begin_marker.size()) == markdown);
}
