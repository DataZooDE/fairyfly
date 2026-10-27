#include "include/commands/update_command.h"

#include "include/commands/command_groups.h"
#include "include/update/updater.h"
#include "include/version.h"

namespace fairyfly {
namespace commands {

namespace {
}  // namespace

CLI::App* UpdateCommand::setup_cli(CLI::App& app) {
    cmd_ = noun_app(app, "update");
    check_cmd_ = add_leaf(app, {"update", "check"});
    check_cmd_->footer(R"HELP(Usage notes:
Asks the project's GitHub releases whether a newer fairyfly exists. Changes nothing.
Example: fairyfly update check
)HELP");
    apply_cmd_ = add_leaf(app, {"update", "apply"});
    apply_cmd_->footer(R"HELP(Usage notes:
Downloads the latest release, checks it against SHA256SUMS and its Authenticode signature (it must
be signed by the publisher this build pins, by default O=DataZoo GmbH, C=DE; a signed installation
never takes an unsigned release) and replaces this fairyfly.exe; the previous one stays as fairyfly.exe.old until the next
update. Restart running fairyfly processes (tray, link) afterwards.
Example: fairyfly update apply
)HELP");
    apply_cmd_->add_flag("--allow-unsigned", allow_unsigned_, "Accept an unsigned release on an unsigned installation (self-built or a release from before signing)");
    if (!packaging::self_updates(package_)) {
        const std::string store = "This fairyfly is a Microsoft Store package: the Store updates it, and both verbs answer\n"
                                  "UPDATE_MANAGED_BY_STORE without asking GitHub.\n";
        check_cmd_->footer(store);
        apply_cmd_->footer(store);
    }
    return cmd_;
}

Result UpdateCommand::execute(cli::CommandHandler&) {
    try {
        if (check_cmd_ && *check_cmd_) {
            auto c = update::check(FAIRYFLY_VERSION);
            Result r;
            r.status = Result::Status::Success;
            r.data = {{"current", c.current}, {"latest", c.latest}, {"update_available", c.available}, {"release", c.page}};
            if (c.available) r.data["next"] = "fairyfly update apply";
            return r;
        }
        if (apply_cmd_ && *apply_cmd_) {
            auto a = update::apply(FAIRYFLY_VERSION, allow_unsigned_);
            if (a.decision.verdict == update::Verdict::Refuse) return Result::fail(a.decision.code, a.decision.message);
            Result r;
            r.status = Result::Status::Success;
            r.data = {{"updated", a.decision.verdict == update::Verdict::Install}, {"message", a.decision.message}};
            if (!a.decision.warning.empty()) r.data["warning"] = a.decision.warning;
            if (!a.installed_path.empty()) {
                r.data["installed"] = a.installed_path;
                r.data["previous"] = a.previous_path;
                r.data["next"] = "restart running fairyfly processes (tray, link run)";
            }
            return r;
        }
    } catch (const std::exception& e) {
        return Result::fail("UPDATE_FAILED", e.what());
    }
    return Result::fail("NO_SUBCOMMAND", "No update subcommand specified (check or apply)");
}

std::unique_ptr<CommandBase> create_update_command() { return std::make_unique<UpdateCommand>(); }

} // namespace commands
} // namespace fairyfly
