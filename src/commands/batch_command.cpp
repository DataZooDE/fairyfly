#include "include/commands/batch_command.h"

#include <nlohmann/json.hpp>

namespace fairyfly {
namespace commands {

namespace {
bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
}

BatchLine parse_batch_line(const std::string& line) {
    BatchLine out;
    size_t first = 0;
    // Tolerate a UTF-8 byte order mark (PowerShell pipes add one).
    if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) first = 3;
    while (first < line.size() && is_blank(line[first])) ++first;
    if (first == line.size() || line[first] == '#') {
        out.skip = true;
        return out;
    }

    if (line[first] == '[') {
        try {
            auto parsed = nlohmann::json::parse(line.substr(first));
            if (!parsed.is_array()) throw std::runtime_error("not an array");
            for (const auto& item : parsed) {
                if (!item.is_string()) throw std::runtime_error("array items must be strings");
                out.argv.push_back(item.get<std::string>());
                // A NUL would be silently truncated when the argument reaches CLI11 as a C string.
                if (out.argv.back().find('\0') != std::string::npos)
                    throw std::runtime_error("argument contains an embedded NUL (\\u0000)");
            }
        } catch (const std::exception& e) {
            out.ok = false;
            out.argv.clear();
            out.error = std::string("Invalid JSON argv array: ") + e.what();
            return out;
        }
        if (out.argv.empty()) out.skip = true;
        return out;
    }

    if (line.find('\0') != std::string::npos) {
        out.ok = false;
        out.error = "Line contains an embedded NUL character";
        return out;
    }

    std::string current;
    bool in_token = false;
    bool in_quotes = false;
    for (size_t i = first; i < line.size(); ++i) {
        const char c = line[i];
        if (in_quotes) {
            if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
                current.push_back(line[++i]);
            } else if (c == '"') {
                in_quotes = false;
            } else {
                current.push_back(c);
            }
        } else if (c == '"') {
            in_quotes = true;
            in_token = true;
        } else if (c == '\\' && i + 1 < line.size() &&
                   (line[i + 1] == ' ' || line[i + 1] == '"' || line[i + 1] == '\\')) {
            current.push_back(line[++i]);
            in_token = true;
        } else if (is_blank(c)) {
            if (in_token) {
                out.argv.push_back(current);
                current.clear();
                in_token = false;
            }
        } else {
            current.push_back(c);
            in_token = true;
        }
    }
    if (in_quotes) {
        out.ok = false;
        out.argv.clear();
        out.error = "Unterminated double quote";
        return out;
    }
    if (in_token) out.argv.push_back(current);
    return out;
}

CLI::App* BatchCommand::setup_cli(CLI::App& app) {
    cmd_ = add_leaf(app, {"batch"});
    cmd_->add_option("--file", file_, "Read commands from this file instead of stdin");
    cmd_->add_flag("--stop-on-error", stop_on_error_, "Stop at the first failing command");
    cmd_->footer("One command per line, without the program name: a JSON array ([\"transaction\",\"start\",\"SM37\"]) "
                 "or shell-style words (double quotes, backslash-escaped spaces). Blank lines and "
                 "lines starting with '#' are ignored. Each line prints one compact JSON result. "
                 "Exit code is non-zero if any command failed. Use --stop-on-error for dependent actions. "
                 "This is sequential execution: it cannot substitute a result from an earlier line into later arguments.\n"
                 "Example: fairyfly batch --file commands.jsonl --stop-on-error\n"
                 "Example commands.jsonl line: [\"element\",\"get\",\"TREE\",\"--list-nodes\",\"--connection\",\"3\"]\n");
    return cmd_;
}

Result BatchCommand::execute(cli::CommandHandler&) {
    // The batch loop runs inside run_cli (it rebuilds the registry per line, which would
    // destroy this object). Reaching here means batch was invoked outside run_cli.
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "BATCH_UNAVAILABLE";
    result.error["message"] = "batch must be run through the fairyfly CLI entry point";
    return result;
}

std::unique_ptr<CommandBase> create_batch_command() {
    return std::make_unique<BatchCommand>();
}

} // namespace commands
} // namespace fairyfly
