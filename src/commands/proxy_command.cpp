#include "include/commands/proxy_command.h"

#include "include/command_table.h"
#include "include/commands/command_groups.h"

namespace fairyfly {
namespace commands {

CLI::App* ProxyCommand::setup_cli(CLI::App& app) {
    cmd_ = noun_app(app, "proxy");
    auto add_setting = [this](CLI::App* c, int i) {
        mode_opt_[i] = c->add_option("--mode", mode_, "auto (detect) | manual | pac | direct")
                           ->check(CLI::IsMember({"auto", "manual", "pac", "direct"}, CLI::ignore_case));
        server_opt_[i] = c->add_option("--server", server_, "With --mode manual: the HTTP proxy as host:port (no https://, no SOCKS)");
        bypass_opt_[i] = c->add_option("--bypass", bypass_, "With --mode manual: exceptions, ';'-separated (<local>;*.corp.example;10.*)");
        pac_opt_[i] = c->add_option("--pac", pac_, "With --mode pac: the PAC script's http:// or https:// address");
        c->add_option("--state", state_, "Pairing state file whose link address is tested (default %USERPROFILE%\\.fairyfly\\link\\state.json)");
    };

    show_cmd_ = add_leaf(app, {"proxy", "show"});
    show_cmd_->footer(R"HELP(Usage notes:
Shows the proxy setting (auto, manual, pac, direct; locked when your administrator sets it by policy),
the route to the gateway in plain words, where it came from and whether the proxy may get the Windows login.
Order: fairyfly policy > your setting > HTTPS_PROXY > browser policy (Edge, Chrome, Firefox) > Windows.
Example: fairyfly proxy show
)HELP");
    show_cmd_->add_option("--state", state_, "Pairing state file (default %USERPROFILE%\\.fairyfly\\link\\state.json)");

    set_cmd_ = add_leaf(app, {"proxy", "set"});
    set_cmd_->footer(R"HELP(Usage notes:
Saves your proxy setting for fairyfly (per user). A running link reconnects along the new route at once.
Refused (PROXY_SETTING_LOCKED) when the administrator sets the proxy by policy. Only HTTP proxies:
no https:// proxy address, no SOCKS. Credentials never go here: the Windows login is used, or a
Credential Manager entry fairyfly-proxy:<host> for Basic.
Example: fairyfly proxy set --mode manual --server proxy.corp.example:8080 --bypass "<local>;*.corp.example"
Example: fairyfly proxy set --mode pac --pac https://pac.corp.example/proxy.pac
Example: fairyfly proxy set --mode direct
)HELP");
    add_setting(set_cmd_, 0);

    test_cmd_ = add_leaf(app, {"proxy", "test"});
    test_cmd_->footer(R"HELP(Usage notes:
Connects to the gateway through the route (HTTPS GET /health, then the link's WebSocket upgrade, closed at
once) and says what happened. With --mode (and its fields) it tests that setting without saving it.
Example: fairyfly proxy test
Example: fairyfly proxy test --mode manual --server proxy.corp.example:8080
)HELP");
    add_setting(test_cmd_, 1);

    reset_cmd_ = add_leaf(app, {"proxy", "reset"});
    reset_cmd_->footer(R"HELP(Usage notes:
Forgets your proxy setting: back to auto (HTTPS_PROXY, browser policies, the Windows settings).
Example: fairyfly proxy reset
)HELP");
    return cmd_;
}

Result ProxyCommand::execute(cli::CommandHandler&) { return execute_without_sap(); }

Result ProxyCommand::execute_without_sap() {
    link::proxycfg::CliArgs a;
    int i = -1;
    if (show_cmd_ && *show_cmd_) a.verb = "show";
    else if (set_cmd_ && *set_cmd_) a.verb = "set", i = 0;
    else if (test_cmd_ && *test_cmd_) a.verb = "test", i = 1;
    else if (reset_cmd_ && *reset_cmd_) a.verb = "reset";
    else return Result::fail("NO_SUBCOMMAND", "No proxy subcommand specified (show, set, test or reset)");
    if (i >= 0) {
        if (mode_opt_[i]->count()) a.mode = mode_;
        if (server_opt_[i]->count()) a.server = server_;
        if (bypass_opt_[i]->count()) a.bypass = bypass_;
        if (pac_opt_[i]->count()) a.pac = pac_;
    }
    auto services = link::proxycfg::process_services(state_);
    return link::proxycfg::run_cli(a, services);
}

std::unique_ptr<CommandBase> create_proxy_command() { return std::make_unique<ProxyCommand>(); }

} // namespace commands
} // namespace fairyfly
