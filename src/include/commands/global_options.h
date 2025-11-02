#pragma once

#include <string>
#include "include/cli_handler.h"

namespace fairyfly {
namespace commands {

/// Global CLI options shared across all commands
struct GlobalOptions {
    bool verbose = false;
    std::string output_format = "json";

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
