#include "include/commands/link_command.h"

#include "include/command_table.h"
#include "include/commands/command_groups.h"

namespace fairyfly {
namespace commands {

CLI::App* LinkCommand::setup_cli(CLI::App& app) {
    cmd_ = noun_app(app, "link");
    auto add_state = [this](CLI::App* c) { c->add_option("--state", options_.state_path, "Pairing state file (default %USERPROFILE%\\.fairyfly\\link\\state.json)"); };

    pair_cmd_ = add_leaf(app, {"link", "pair"});
    pair_cmd_->footer(R"HELP(Usage notes:
Shows a QR code and a number. Scan it with your phone, sign in, type the number; then confirm here
that the account and organisation shown are yours. Nothing is saved before that confirmation.
Without --gateway or FAIRYFLY_LINK_GATEWAY it asks for the service, https://fairyfly.io prefilled (Enter).
Example: fairyfly link pair
Example: fairyfly link pair --gateway https://fairyfly.io
)HELP");
    pair_cmd_->add_option("--gateway", options_.gateway, "Web address of the fairyfly service (default: FAIRYFLY_LINK_GATEWAY, else asked with https://fairyfly.io prefilled)");
    pair_cmd_->add_option("--qr-png", options_.qr_png, "Also write the QR code as a PNG file");
    pair_cmd_->add_option("--qr-style", options_.qr_style, "QR code in the console: auto (block characters where the console can show them, else ASCII) | blocks | ascii")
        ->check(CLI::IsMember({"auto", "blocks", "ascii"}));
    pair_cmd_->add_option("--timeout", options_.pair_timeout_s, "Seconds to wait for the phone (default 300)");
    add_state(pair_cmd_);

    status_cmd_ = add_leaf(app, {"link", "status"});
    status_cmd_->footer(R"HELP(Usage notes:
Shows whether this desktop is paired, with which account and organisation.
Example: fairyfly link status
)HELP");
    add_state(status_cmd_);

    run_cmd_ = add_leaf(app, {"link", "run"});
    run_cmd_->footer(R"HELP(Usage notes:
Connects to the gateway (outbound WebSocket) and serves the AI assistant's tool calls through
fairyfly's own MCP dispatcher. Every change needs the gateway's grant and an approval at this
desktop (a notification: Approve, Deny or Details; no answer in 45 s, a locked screen or no
interactive session deny it), unless "Allow changes in SAP" is on in the link tray: then changes
run without asking. --read-only refuses every change. Calls are audited with audit_source "link".
Stop with Ctrl+C.
--tray runs the link alone in the system tray (Pause/Resume, Allow changes in SAP, Health check,
Quit); it needs no
MCP server and no HTTP endpoint. --install-autostart registers it, with the same options, at logon.
Example: fairyfly link run
Example: fairyfly link run --tray --install-autostart
)HELP");
    run_cmd_->add_flag("--allow-write", options_.allow_write, "No effect: changes are possible, each approved at this desktop (kept for old command lines)");
    run_cmd_->add_option("--tools", tools_filter_, "Only these tool families, e.g. session,screen");
    run_cmd_->add_option("--approve", options_.approve, "Approval of changes: desktop (notification and approval window) | no (every change is denied)")
        ->check(CLI::IsMember({"desktop", "no"}));
    run_cmd_->add_flag("--tray", options_.tray, "Run in the system tray (detached from the console); no MCP server");
    run_cmd_->add_flag("--install-autostart", options_.install_autostart, "With --tray: start the link tray at logon (HKCU Run key)");
    run_cmd_->add_flag("--remove-autostart", options_.remove_autostart, "With --tray: remove the logon autostart");
    add_state(run_cmd_);

    pause_cmd_ = add_leaf(app, {"link", "pause"});
    pause_cmd_->footer(R"HELP(Usage notes:
Disconnects a running link from the gateway until resumed: the AI assistant cannot reach this
desktop meanwhile. Works from any console; the running link notices within a second.
Example: fairyfly link pause
)HELP");
    add_state(pause_cmd_);

    resume_cmd_ = add_leaf(app, {"link", "resume"});
    resume_cmd_->footer(R"HELP(Usage notes:
Lets a paused link connect to the gateway again.
Example: fairyfly link resume
)HELP");
    add_state(resume_cmd_);

    unpair_cmd_ = add_leaf(app, {"link", "unpair"});
    unpair_cmd_->footer(R"HELP(Usage notes:
Forgets the pairing on this desktop. Remove the desktop in the web app as well.
Example: fairyfly link unpair
)HELP");
    add_state(unpair_cmd_);
    return cmd_;
}

link::LinkOptions LinkCommand::options() const {
    link::LinkOptions o = options_;
    if (!tools_filter_.empty()) o.families = command_table::parse_family_list(tools_filter_);
    return o;
}

Result LinkCommand::execute(cli::CommandHandler&) { return execute_without_sap(); }

Result LinkCommand::execute_without_sap() {
    if (pair_cmd_ && *pair_cmd_) return link::link_pair(options());
    if (status_cmd_ && *status_cmd_) return link::link_status(options());
    if (unpair_cmd_ && *unpair_cmd_) return link::link_unpair(options());
    if (pause_cmd_ && *pause_cmd_) return link::link_pause(options());
    if (resume_cmd_ && *resume_cmd_) return link::link_resume(options());
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "NO_SUBCOMMAND";
    result.error["message"] = "No link subcommand specified (pair, status, run, pause, resume or unpair)";
    return result;
}

std::unique_ptr<CommandBase> create_link_command() { return std::make_unique<LinkCommand>(); }

} // namespace commands
} // namespace fairyfly
