#pragma once

#include <string>
#include "include/cli_handler.h"

namespace fairyfly {
namespace commands {

/// Global CLI options shared across all commands
struct GlobalOptions {
    std::string log_level = "error"; ///< Logging level: trace, debug, info, warn, error (default), critical, off
    std::string output_format = "json";
    bool verbose_errors = false;     ///< Include detailed error suggestions (default: false for compact errors)

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
