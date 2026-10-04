#include "include/commands/cli_app.h"

#include <cstdint>
#include <memory>

#include "include/commands/command_registry.h"
#include "include/version.h"

namespace fairyfly {
namespace commands {

namespace {
constexpr const char* kGlobalGroup = "GLOBAL FLAGS";

// Render from the live CLI11 tree: option types, defaults, validators and each
// command's local usage notes stay beside the implementation that owns them.
class AgentHelpFormatter final : public CLI::Formatter {
public:
    std::string make_footer(const CLI::App*) const override {
        // CLI11's paragraph wrapping collapses whitespace inside quoted tree
        // keys. Examples must remain copyable, including their exact spacing.
        // Suppress the base formatter's footer; render_command appends it after
        // CLI11's paragraph-formatting pass.
        return {};
    }

    std::string make_help(const CLI::App* app, std::string name,
                          CLI::AppFormatMode mode) const override {
        if (mode == CLI::AppFormatMode::Sub)
            return CLI::Formatter::make_help(app, name, mode);
        auto result = render_command(app, name);
        append_commands(result, app, name);
        return result;
    }

private:
    std::string render_command(const CLI::App* app, const std::string& path) const {
        auto result = CLI::Formatter::make_help(app, path, CLI::AppFormatMode::Normal);
        const auto footer = app->get_footer();
        if (!footer.empty()) result += "\n" + footer + "\n";
        return result;
    }

    void append_commands(std::string& result, const CLI::App* app,
                         const std::string& path) const {
        for (const auto* child : app->get_subcommands([](const CLI::App* item) {
                 return !item->get_name().empty() && !item->get_group().empty();
             })) {
            const auto child_path = path + " " + child->get_name();
            result += "\n=== " + child_path + " ===\n";
            result += render_command(child, child_path);
            append_commands(result, child, child_path);
        }
    }
};
}

void add_global_options(CLI::App& app, GlobalOptions& global_opts) {
    app.set_version_flag("--version", fairyfly::FAIRYFLY_VERSION)->group(kGlobalGroup);
    if (CLI::Option* help = app.get_help_ptr()) help->group(kGlobalGroup);

    app.add_option("--log-level", global_opts.log_level,
                   "Set logging level: trace, debug, info, warn, error (default), critical, off")
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "warning", "error", "err", "critical", "crit", "off"}))
        ->group(kGlobalGroup);
    app.add_flag("-v,--verbose", [&global_opts](std::int64_t) { global_opts.log_level = "debug"; },
                 "Shorthand for --log-level debug")->group(kGlobalGroup);
    app.add_option("--output", global_opts.output_format, "Output format: json (default), markdown, toon")
        ->check(CLI::IsMember({"json", "markdown", "toon"}))
        ->group(kGlobalGroup);
    app.add_flag("--verbose-errors", global_opts.verbose_errors,
                 "Include detailed error suggestions (default: compact errors)")->group(kGlobalGroup);
    app.add_flag("--read-only", global_opts.read_only,
                 "Refuse state-changing actions (save, delete, release, ...); also FAIRYFLY_READ_ONLY=1")
        ->group(kGlobalGroup);
    app.add_flag("--no-audit", global_opts.no_audit,
                 "Do not write the audit trail for this run; also FAIRYFLY_AUDIT=0")->group(kGlobalGroup);
    app.add_flag("--audit-required", global_opts.audit_required,
                 "Fail with AUDIT_UNAVAILABLE when the audit trail cannot be written; also FAIRYFLY_AUDIT=required")
        ->group(kGlobalGroup);
}

void build_command_tree(CLI::App& app) {
    // Disable Windows-style options (/opt) to allow SAP element IDs starting with /
    // SAP element paths like /app/con[0]/ses[0]/wnd[0]/usr/txtField would otherwise
    // be interpreted as option flags on Windows, causing argument parsing failures
    app.allow_windows_style_options(false);
    app.formatter(std::make_shared<AgentHelpFormatter>());
    register_all_commands();
    CommandRegistry::instance().setup_all_commands(app);
    app.footer(R"HELP(AGENT WORKFLOW
This help includes every public command below, including nested MCP commands.
Use <command> --help for just that command and its descendants. Help runs without SAP.
Examples use fairyfly on PATH. From a Windows PowerShell build checkout, replace it
with .\build\Release\fairyfly.exe (or & 'C:\path with spaces\fairyfly.exe').
Example IDs, paths, names and values are placeholders; replace them with observed values.

1. Discover live sessions and saved connections. SAP session paths and list indices
   are different from saved numeric Fairyfly connection IDs. Attach an observed
   session path if needed; use the returned connection_id for subsequent calls.
2. Reuse a logged-in session. Launch opens a logon screen unless --login is supplied.
   Read the screen before navigating; resolve logon screens or popups first.
3. Find a control, then read that control. Preserve IDs, grid column IDs and tree
   node keys exactly, including spaces. A collapsed tree lists categories only;
   expand the returned parent keys and read again to discover its children.
4. Inspect status and error.code on every result. After a stale connection error,
   rediscover sessions and attach a live one; do not keep retrying an old ID.
   Read the screen after navigation. An accepted click does not prove task completion.
5. Batch known sequential commands to reduce process startup overhead. Stop and
   inspect an error before dependent actions. Do not discard intermediate results.

Global flags work before or after a command. --connection belongs to individual
commands: place it after the noun/verb. JSON is the default result format; inspect
status, data and error. --output toon reduces text overhead; --compact on screen
read reduces repeated structure. Global --read-only guards SAP state-changing actions.
All syntax/options below are generated from command registrations. Usage notes and
examples are attached to those same commands; hidden internal diagnostics are omitted.
)HELP");
}

std::string invoked_command_path(const CLI::App& app) {
    std::string path;
    const CLI::App* current = &app;
    while (true) {
        const auto parsed = current->get_subcommands();
        if (parsed.empty()) break;
        current = parsed.front();
        if (!path.empty()) path += ' ';
        path += current->get_name();
    }
    return path;
}

} // namespace commands
} // namespace fairyfly
