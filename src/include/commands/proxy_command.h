#pragma once

#include "include/commands/command_base.h"
#include "include/link/proxy_cli.h"

namespace fairyfly {
namespace commands {

/// `fairyfly proxy show|set|test|reset`: the proxy of the cloud link and of every HTTPS request (docs/LINK.md
/// "Corporate proxies"). Never touches SAP: cli_entry.cpp runs it without the shared CommandHandler, like `link pair`.
class ProxyCommand : public CommandBase {
public:
    CLI::App* setup_cli(CLI::App& app) override;
    Result execute(cli::CommandHandler& handler) override;
    Result execute_without_sap();

private:
    CLI::App* show_cmd_ = nullptr;
    CLI::App* set_cmd_ = nullptr;
    CLI::App* test_cmd_ = nullptr;
    CLI::App* reset_cmd_ = nullptr;
    std::string mode_, server_, bypass_, pac_, state_;
    CLI::Option* mode_opt_[2] = {nullptr, nullptr};
    CLI::Option* server_opt_[2] = {nullptr, nullptr};
    CLI::Option* bypass_opt_[2] = {nullptr, nullptr};
    CLI::Option* pac_opt_[2] = {nullptr, nullptr};
};

} // namespace commands
} // namespace fairyfly
