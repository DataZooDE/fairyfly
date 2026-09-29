#include <iostream>

#include "include/commands/mcp_extras.h"

namespace fairyfly {
namespace commands {

int run_client_config_command(McpExtras& x) {
    const std::string invalid = config::validate_client_options(x.client);
    if (!invalid.empty()) return report_error(x.output, "CLIENT_CONFIG_INVALID", invalid);
    const auto snippets = config::build_client_configs(x.client);
    if (x.output == "json") {
        nlohmann::json out;
        out["status"] = "success";
        out["data"] = config::render_client_configs_json(snippets, x.client);
        std::cout << out.dump(2) << std::endl;
    } else {
        std::cout << config::render_client_configs_text(snippets);
    }
    return 0;
}

} // namespace commands
} // namespace fairyfly
