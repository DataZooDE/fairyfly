#include "include/commands/command_base.h"

#include <spdlog/spdlog.h>

namespace fairyfly {
namespace commands {

class ServeCommand : public CommandBase {
public:
    std::string name() const override { return "serve"; }

    std::string description() const override {
        return "Start MCP server";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
        cmd_->add_option("--transport", transport_, "Transport: stdio, http")
            ->check(CLI::IsMember({"stdio", "http"}));
        cmd_->add_option("--port", port_, "HTTP port (when transport=http)");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        spdlog::info("Starting MCP server with transport: {}", transport_);
        if (transport_ == "http") {
            spdlog::info("HTTP server on port: {}", port_);
        }

        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "NOT_IMPLEMENTED";
        result.error["message"] = "MCP server implementation coming in Phase 3";
        return result;
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string transport_ = "stdio";
    int port_ = 8080;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_serve_command() {
    return std::make_unique<ServeCommand>();
}

} // namespace commands
} // namespace fairyfly
