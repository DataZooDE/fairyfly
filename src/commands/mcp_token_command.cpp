#include "include/commands/command_base.h"

#include "include/auth/scopes.h"
#include "include/auth/secret_backend.h"
#include "include/auth/token_cli.h"
#include "include/auth/token_store.h"

// `fairyfly mcp token create|list|revoke|rotate|delete`: bearer tokens of the remote MCP endpoint, stored in
// the Windows Credential Manager ("fairyfly-mcp:<name>"). Allowed under --read-only (like credentials set):
// managing access tokens is not a change of SAP state; the invocation is audited as usual.
// A newly created token is printed exactly once and never appears in the audit trail (argv holds no secret).
namespace fairyfly {
namespace commands {

class McpTokenCommand : public CommandBase {
public:
    std::string name() const override { return "mcp token"; }
    std::string description() const override { return "Manage MCP access tokens"; }

    CLI::App* setup_cli(CLI::App& app) override {
        CLI::App* mcp = noun_app(app, "mcp");
        token_ = mcp->add_subcommand("token", "Manage access tokens of the remote MCP endpoint (Credential Manager)");
        token_->fallthrough();
        token_->require_subcommand(1);

        create_ = token_->add_subcommand("create", "Create a token (shown once)");
        create_->footer(R"HELP(Usage notes:
The secret is printed once; store it securely. Choose the minimum scopes needed.
Example: fairyfly mcp token create reader --scope session,connection,screen,element --read-only
)HELP");
        create_->fallthrough();
        create_->add_option("name", args_.name, "Token name (letters, digits, . _ -)")->required();
        create_->add_option("--scope", args_.scopes,
                            fairyfly::auth::scope_help_text())
            ->delimiter(',');
        create_->add_option("--system", args_.systems, "Allowed SAP systems SID/CLIENT, e.g. A4H/001 (globs allowed)")->delimiter(',');
        create_->add_option("--sap-identity", args_.sap_identities,
                            "Exact SAP identities SID/CLIENT/USER, e.g. A4H/001/ALICE (required for multi-user owner endpoints)")->delimiter(',');
        create_->add_option("--tcode", args_.tcodes, "Allowed T-codes, e.g. SE16,SM* (globs allowed). With --allow-selection-input only list transactions whose execution is read-only: Enter/F8 on a selection screen runs the report, and fairyfly cannot know what a custom report does")->delimiter(',');
        create_->add_flag("--allow-navigation", args_.allow_navigation,
                          "With --tcode: also allow gui_menu_select and navigating keys (F3, F12, ...); default is fail-closed");
        create_->add_flag("--allow-selection-input", args_.allow_selection_input,
                          "Read-only token with --tcode: may type into selection fields, but only on the initial screen of the transaction (no cells, no password fields, no commit). Enter/F8 then EXECUTE the selection: list only transactions whose execution is read-only in --tcode");
        create_->add_option("--connections", args_.connections,
                            "Allowed saved connections / SAP Logon entry names, e.g. DEV,QA* (globs allowed; no spaces)")->delimiter(',');
        create_->add_option("--ip", args_.ips, "Allowed client addresses or CIDR blocks")->delimiter(',');
        create_->add_option("--rate", args_.rate, "Calls per minute (0 = server default)")->check(CLI::NonNegativeNumber);
        create_->add_option("--rate-family", args_.rate_families,
                            "Calls per minute per tool family, e.g. element=10,key=10 (stricter than --rate for that family)")->delimiter(',');
        create_->add_option("--expires", args_.expires, "Expiry: 30d, 12h, 90m or a date such as 2026-12-31");
        create_->add_flag("--read-only", args_.read_only_flag,
                          "Token can never change SAP state (default when no --scope is given)");
        create_->add_flag("--yes", args_.yes, "Confirm a token with the wildcard scope *");
        add_output_option(create_, output_);

        list_ = token_->add_subcommand("list", "List tokens (never shows secrets or hashes)");
        list_->footer(R"HELP(Usage notes:
Example: fairyfly mcp token list
)HELP");
        list_->fallthrough();
        add_output_option(list_, output_);

        revoke_ = token_->add_subcommand("revoke", "Revoke a token immediately");
        revoke_->footer(R"HELP(Usage notes:
Existing clients using this token will lose access.
Example: fairyfly mcp token revoke reader
)HELP");
        revoke_->fallthrough();
        revoke_->add_option("name", args_.name, "Token name")->required();
        add_output_option(revoke_, output_);

        delete_ = token_->add_subcommand("delete", "Remove a token record for good (also a revoked one); needs --yes");
        delete_->footer(R"HELP(Usage notes:
Removes the stored token record. Explicit --yes is required.
Example: fairyfly mcp token delete reader --yes
)HELP");
        delete_->fallthrough();
        delete_->add_option("name", args_.name, "Token name")->required();
        delete_->add_flag("--yes", args_.yes, "Confirm the deletion");
        add_output_option(delete_, output_);

        rotate_ = token_->add_subcommand("rotate", "Issue a new secret for a token; the old one stops working at once");
        rotate_->footer(R"HELP(Usage notes:
Update clients with the returned secret; the prior secret immediately stops working.
Example: fairyfly mcp token rotate reader
)HELP");
        rotate_->fallthrough();
        rotate_->add_option("name", args_.name, "Token name")->required();
        add_output_option(rotate_, output_);
        return token_;
    }

    Result execute(cli::CommandHandler& handler) override {
        (void)handler;
        if (*create_) args_.action = "create";
        else if (*list_) args_.action = "list";
        else if (*revoke_) args_.action = "revoke";
        else if (*rotate_) args_.action = "rotate";
        else if (*delete_) args_.action = "delete";
        try {
            auth::TokenStore store(auth::make_credential_manager_backend(auth::kTokenTargetPrefix));
            return auth::run_token_action(args_, store);
        } catch (const std::exception& e) {
            Result r;
            r.status = Result::Status::Error;
            r.error = {{"code", "TOKEN_STORE_ERROR"}, {"message", e.what()}};
            return r;
        }
    }

    bool was_invoked() const override { return token_ && *token_; }

    std::optional<std::string> get_preferred_output_format() const override { return output_override(output_); }

private:
    CLI::App* token_ = nullptr;
    CLI::App* create_ = nullptr;
    CLI::App* list_ = nullptr;
    CLI::App* revoke_ = nullptr;
    CLI::App* rotate_ = nullptr;
    CLI::App* delete_ = nullptr;
    auth::TokenCliArgs args_;
    std::string output_;
};

std::unique_ptr<CommandBase> create_mcp_token_command() {
    return std::make_unique<McpTokenCommand>();
}

} // namespace commands
} // namespace fairyfly
