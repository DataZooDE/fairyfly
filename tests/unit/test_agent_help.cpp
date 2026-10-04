#include <catch2/catch_test_macros.hpp>
#include <CLI/CLI.hpp>
#include <sstream>
#include <functional>
#include "include/commands/cli_app.h"
#include "include/commands/batch_command.h"

namespace {
struct HelpTree {
    fairyfly::commands::GlobalOptions options;
    CLI::App app{"fairyfly", "fairyfly"};
    HelpTree() {
        fairyfly::commands::add_global_options(app, options);
        fairyfly::commands::build_command_tree(app);
    }
};

void visit(const CLI::App& app, const std::string& path,
           const std::function<void(const CLI::App&, const std::string&)>& check) {
    for (const auto* child : app.get_subcommands([](const CLI::App* item) {
             return !item->get_name().empty() && !item->get_group().empty();
         })) {
        const auto child_path = path + " " + child->get_name();
        check(*child, child_path);
        visit(*child, child_path, check);
    }
}
}

TEST_CASE("agent help includes all public descendants and command documentation", "[help][agent_help]") {
    HelpTree tree;
    const auto help = tree.app.help();
    CHECK(help.find("AGENT WORKFLOW") != std::string::npos);
    visit(tree.app, "fairyfly", [&](const CLI::App& command, const std::string& path) {
        INFO(path);
        const auto heading = "=== " + path + " ===";
        const auto position = help.find(heading);
        REQUIRE(position != std::string::npos);
        CHECK(help.find(heading, position + heading.size()) == std::string::npos);
        const auto section = help.substr(position, help.find("\n=== ", position) - position);
        for (const auto* option : command.get_options()) {
            if (option->get_group().empty()) continue;
            CHECK(section.find(option->get_name()) != std::string::npos);
        }
        if (command.get_subcommands([](const CLI::App*) { return true; }).empty()) {
            CHECK(command.get_footer().find("Example") != std::string::npos);
            CHECK(section.find(command.get_footer()) != std::string::npos);
        }
    });
    CHECK(help.find("--dump-object-tree") == std::string::npos);
    CHECK(help.find("--apply-plan") == std::string::npos);
    CHECK(help.find("--node-key \"          1\"") != std::string::npos);
}

TEST_CASE("agent help follows new registrations without a second manual", "[help][agent_help]") {
    HelpTree tree;
    auto* future = tree.app.get_subcommand("element")->add_subcommand("future", "Future command");
    std::string future_value;
    future->add_option("--future-option", future_value, "Future option");
    future->footer("Example: fairyfly element future --future-option value");
    const auto help = tree.app.help();
    CHECK(help.find("=== fairyfly element future ===") != std::string::npos);
    CHECK(help.find("--future-option") != std::string::npos);
    CHECK(help.find(future->get_footer()) != std::string::npos);
}

TEST_CASE("agent help scopes to the requested command", "[help][agent_help]") {
    HelpTree tree;
    const char* argv[] = {"fairyfly", "element", "--help"};
    CHECK_THROWS_AS(tree.app.parse(3, argv), CLI::CallForHelp);
    auto help = tree.app.help();
    CHECK(help.find("=== fairyfly element get ===") != std::string::npos);
    CHECK(help.find("=== fairyfly mcp") == std::string::npos);

    const auto* leaf = tree.app.get_subcommand("element")->get_subcommand("get");
    help = leaf->help("fairyfly element");
    CHECK(help.find("--list-nodes") != std::string::npos);
    CHECK(help.find("=== ") == std::string::npos);
}

TEST_CASE("command help examples parse against the real CLI without running actions", "[help][agent_help]") {
    // Collect before rebuilding: the registry owns command objects and is reset
    // for each new tree, so no pointers to prior commands survive a rebuild.
    std::vector<std::string> examples;
    {
        HelpTree tree;
        visit(tree.app, "fairyfly", [&](const CLI::App& command, const std::string&) {
            std::istringstream lines(command.get_footer());
            std::string line;
            while (std::getline(lines, line)) {
                const auto start = line.find("fairyfly ");
                if (start == std::string::npos) continue;
                if (line.substr(0, start).find_first_not_of(' ') != std::string::npos &&
                    line.rfind("Example: ", 0) != 0) continue;
                examples.push_back(line.substr(start));
            }
        });
    }
    REQUIRE(examples.size() >= 38);
    for (const auto& example : examples) {
        INFO(example);
        auto parsed = fairyfly::commands::parse_batch_line(example);
        REQUIRE(parsed.ok);
        HelpTree tree;
        std::vector<const char*> argv;
        for (const auto& arg : parsed.argv) argv.push_back(arg.c_str());
        CHECK_NOTHROW(tree.app.parse(static_cast<int>(argv.size()), argv.data()));
    }
}
