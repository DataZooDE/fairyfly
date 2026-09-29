#include "include/commands/cli_app.h"

#include <cstdint>

#include "include/commands/command_registry.h"
#include "include/version.h"

namespace fairyfly {
namespace commands {

namespace {
constexpr const char* kGlobalGroup = "GLOBAL FLAGS";
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
    register_all_commands();
    CommandRegistry::instance().setup_all_commands(app);
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
