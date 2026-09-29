#pragma once

#include <optional>
#include <string>
#include "include/cli_handler.h"

namespace fairyfly {
namespace commands {

/// Global CLI options shared across all commands
struct GlobalOptions {
    std::string log_level = "error"; ///< Logging level: trace, debug, info, warn, error (default), critical, off
    std::string output_format = "json";
    bool verbose_errors = false;     ///< Include detailed error suggestions (default: false for compact errors)
    bool read_only = false;          ///< Refuse state-changing actions (--read-only / FAIRYFLY_READ_ONLY=1)
    bool no_audit = false;           ///< Disable the audit trail (--no-audit / FAIRYFLY_AUDIT=0)
    bool audit_required = false;     ///< Fail commands when the audit trail is unwritable (--audit-required)
    std::optional<size_t> batch_line;///< Set for commands executed from a batch file (1-based line)

    /// Convert output format string to enum
    cli::OutputFormat get_output_format() const {
        if (output_format == "markdown") {
            return cli::OutputFormat::Markdown;
        } else if (output_format == "text" || output_format == "plain") {
            return cli::OutputFormat::PlainText;
        } else if (output_format == "toon") {
            return cli::OutputFormat::Toon;
        }
        return cli::OutputFormat::Json;
    }
};

} // namespace commands
} // namespace fairyfly
