#include "include/commands/command_groups.h"

#include <stdexcept>

#include "include/command_table.h"

namespace fairyfly {
namespace commands {

CLI::App* noun_app(CLI::App& root, const std::string& noun) {
    if (CLI::App* existing = root.get_subcommand_no_throw(noun)) return existing;
    const auto* group = command_table::find_group(noun);
    if (!group) throw std::logic_error("command table has no group '" + noun + "'");
    CLI::App* app = root.add_subcommand(group->noun, group->summary);
    app->group(group->help_group);
    app->fallthrough();
    if (noun != "mcp") app->require_subcommand(1);
    return app;
}

CLI::App* add_leaf(CLI::App& root, const std::vector<std::string>& path) {
    const auto* spec = command_table::find_by_path(path);
    if (!spec) throw std::logic_error("command table has no entry for this path");
    CLI::App* leaf = nullptr;
    if (path.size() == 1) {
        leaf = root.add_subcommand(path[0], spec->summary);
        leaf->group(spec->help_group);
    } else {
        leaf = noun_app(root, path[0])->add_subcommand(path[1], spec->summary);
    }
    leaf->fallthrough();
    return leaf;
}

} // namespace commands
} // namespace fairyfly
