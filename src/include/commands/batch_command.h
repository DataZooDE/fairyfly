#pragma once

#include "include/commands/command_base.h"

#include <string>
#include <vector>

namespace fairyfly {
namespace commands {

/// Result of parsing one line of `fairyfly batch` input.
struct BatchLine {
    bool ok = true;                  ///< false when the line is malformed (see error)
    bool skip = false;               ///< true for blank lines and '#' comments
    std::vector<std::string> argv;   ///< command arguments, without the program name
    std::string error;               ///< human readable parse error when !ok
};

/// Parse one batch input line into argv. Pure function (no SAP, no I/O).
///
/// Accepted formats:
///   - JSON array of strings:   ["transaction","start","SM37"]
///   - shell-style words:       transaction start SM37 / element click "wnd[0]/usr/btn[1]" / element fill ID a\ b
///     Words are split on blanks. Double quotes group a word (inside quotes \" and \\
///     are escapes). Outside quotes a backslash escapes a following space, double quote
///     or backslash; before any other character it is kept literally (Windows paths work).
///   - blank lines and lines whose first non-blank character is '#' are skipped.
BatchLine parse_batch_line(const std::string& line);

/// `fairyfly batch`: runs many commands in one process. The loop itself lives in
/// cli_entry.cpp (run_cli) because it must rebuild the command registry per line.
class BatchCommand : public CommandBase {
public:
    std::string name() const override { return "batch"; }
    std::string description() const override {
        return "Run many commands from stdin (or --file), one per line, in one process";
    }

    CLI::App* setup_cli(CLI::App& app) override;
    Result execute(cli::CommandHandler& handler) override;
    bool was_invoked() const override { return cmd_ && *cmd_; }

    const std::string& file() const { return file_; }
    bool stop_on_error() const { return stop_on_error_; }

private:
    CLI::App* cmd_ = nullptr;
    std::string file_;
    bool stop_on_error_ = false;
};

} // namespace commands
} // namespace fairyfly
