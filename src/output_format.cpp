// format_output: a Result as JSON, Markdown, plain text or TOON for the CLI and the MCP server. The command that
// produced the result says how it renders (command_table::Render); a result of another command is never rendered
// as a screen or a diagnosis, and a variant of the command whose data has another shape renders generically.
#include "include/cli_handler.h"
#include "include/formatters/compact_json.h"
#include "include/formatters/screen_markdown_formatter.h"
#include "include/formatters/toon_encoder.h"

#include <sstream>
#include <string>

namespace fairyfly {
namespace cli {

namespace {

using command_table::Render;

/// Markdown-safe text for one line (screen find).
std::string escape_markdown(const std::string& value) {
    std::string safe;
    for (char ch : value) {
        switch (ch) {
            case '&': safe += "&amp;"; break;
            case '<': safe += "&lt;"; break;
            case '>': safe += "&gt;"; break;
            case '\n': case '\r': safe += ' '; break;
            case '\\': case '`': case '*': case '_': case '[':
            case ']': case '|': safe += '\\'; safe += ch; break;
            default: safe += ch; break;
        }
    }
    return safe;
}

/// The JSON/TOON document of a result: `screen read --compact` replaces duplicated hierarchy objects by ids, and
/// suggestions are dropped unless verbose errors were asked for.
json document(const Result& result, bool verbose_errors, Render render) {
    json output = result.to_json();
    if (render == Render::ScreenRead && output.value("status", "") == "success") {
        auto data = output.find("data");
        if (data != output.end() && data->is_object() && data->value("compact", false) == true)
            *data = formatters::compact_screen_json(*data);
    }
    if (!verbose_errors && output.contains("error") && output["error"].contains("suggestions")) output["error"].erase("suggestions");
    return output;
}

std::string screen_find_markdown(const json& data) {
    std::ostringstream oss;
    oss << "# Screen matches\n\n";
    oss << "**Screen:** " << escape_markdown(data.value("title", "")) << "\n\n";
    oss << "**Scanned:** " << data.value("scanned_count", 0) << " controls\n\n";
    if (data.value("scan_limit_reached", false)) oss << "_Scan stopped at the 500-control limit._\n\n";
    const json elements = data.value("elements", json::array());
    if (elements.empty()) oss << "_No matches._\n";
    for (const auto& match : elements) {
        if (!match.is_object()) continue;
        oss << "- **" << escape_markdown(match.value("type", "Control")) << "**: " << escape_markdown(match.value("id", "")) << "\n";
        if (!match.value("name", "").empty()) oss << "  - Name: " << escape_markdown(match.value("name", "")) << "\n";
        if (match.contains("text") && match["text"].is_string())
            oss << "  - Text: " << escape_markdown(match["text"].get<std::string>()) << "\n";
        if (match.contains("tooltip") && match["tooltip"].is_string())
            oss << "  - Tooltip: " << escape_markdown(match["tooltip"].get<std::string>()) << "\n";
    }
    return oss.str();
}

std::string screen_read_markdown(const json& response) {
    std::ostringstream oss;
    const json& data = response["data"];
    oss << ScreenMarkdownFormatter::format(data, data.value("compact", false));
    if (response.contains("metadata") && response["metadata"].is_object() && response["metadata"].contains("duration_ms"))
        oss << "\n_Read time: " << response["metadata"]["duration_ms"].get<int>() << "ms_\n";
    return oss.str();
}

std::string doctor_markdown(const json& data) {
    std::ostringstream oss;
    const std::string health = data.value("overall_health", "");
    if (health == "ok") oss << "# ✅ Fairyfly Environment Diagnostics: Healthy\n\n";
    else if (health == "warning") oss << "# ⚠️ Fairyfly Environment Diagnostics: Warnings Detected\n\n";
    else oss << "# ❌ Fairyfly Environment Diagnostics: Issues Detected\n\n";
    oss << "| Status | Check | Details | Remediation |\n";
    oss << "| :--- | :--- | :--- | :--- |\n";
    for (const auto& check : data.value("checks", json::array())) {
        const std::string st = check.value("status", "info");
        const char* icon = st == "pass" ? "✅ PASS" : st == "warn" ? "⚠️ WARN" : st == "fail" ? "❌ FAIL" : "ℹ️ INFO";
        std::string det = check.value("details", "-"), rem = check.value("remediation", "-");
        if (det.empty()) det = "-";
        if (rem.empty()) rem = "-";
        oss << "| " << icon << " | " << check.value("message", "") << " | " << det << " | " << rem << " |\n";
    }
    return oss.str();
}

std::string generic_markdown(const json& response, bool verbose_errors) {
    std::ostringstream oss;
    oss << (response["status"] == "success" ? "# ✅ Success\n\n" : "# ❌ Error\n\n");
    if (response.contains("error")) {
        const auto& error = response["error"];
        if (error.contains("code")) oss << "**Error Code:** `" << error["code"].get<std::string>() << "`\n\n";
        if (error.contains("message")) oss << "**Message:** " << error["message"].get<std::string>() << "\n\n";
        if (verbose_errors && error.contains("suggestions") && error["suggestions"].is_array()) {
            oss << "**Suggestions:**\n";
            for (const auto& suggestion : error["suggestions"]) oss << "- " << suggestion.get<std::string>() << "\n";
            oss << "\n";
        }
    }
    if (response.contains("data") && !response["data"].empty()) oss << "## Data\n\n```json\n" << response["data"].dump(2) << "\n```\n";
    if (response.contains("metadata") && response["metadata"].is_object() && !response["metadata"].empty()) {
        const auto& meta = response["metadata"];
        oss << "## Metadata\n\n";
        if (meta.contains("duration_ms")) oss << "- **Duration:** " << meta["duration_ms"].get<int>() << "ms\n";
        if (meta.contains("timestamp")) oss << "- **Timestamp:** " << meta["timestamp"].get<std::string>() << "\n";
    }
    return oss.str();
}

/// The data a command's own renderer is written for. Other variants of the same command (`screen read
/// --dump-object-tree`, `screen read --no-children`, ...) return other data and render generically.
bool renders_as(Render render, const json& data) {
    switch (render) {
        case Render::ScreenFind: return data.contains("scanned_count") && data.contains("elements") && data["elements"].is_array();
        case Render::ScreenRead: return data.contains("screen_id") && data.contains("hierarchy");
        case Render::Doctor: return data.contains("overall_health") && data.contains("checks");
        case Render::Generic: return false;
    }
    return false;
}

std::string markdown(const Result& result, bool verbose_errors, Render render) {
    const json response = result.to_json();
    if (response["status"] == "success" && response.contains("data") && response["data"].is_object() &&
        renders_as(render, response["data"])) {
        switch (render) {
            case Render::ScreenFind: return screen_find_markdown(response["data"]);
            case Render::ScreenRead: return screen_read_markdown(response);
            case Render::Doctor: return doctor_markdown(response["data"]);
            case Render::Generic: break;
        }
    }
    return generic_markdown(response, verbose_errors);
}

std::string plain_text(const Result& result) {
    const json response = result.to_json();
    if (response["status"] == "success") return "SUCCESS\n";
    std::string out = "ERROR\n";
    if (response.contains("error") && response["error"].contains("message")) out += response["error"]["message"].get<std::string>() + "\n";
    return out;
}

}  // namespace

std::string format_output(const Result& result, OutputFormat format, bool verbose_errors, Render render) {
    switch (format) {
        case OutputFormat::Json: return document(result, verbose_errors, render).dump(2);
        case OutputFormat::Markdown: return markdown(result, verbose_errors, render);
        case OutputFormat::PlainText: return plain_text(result);
        case OutputFormat::Toon: {
            formatters::ToonOptions opts;
            opts.indent = 2;
            opts.delimiter = ',';
            opts.length_marker = false;
            return formatters::ToonEncoder().encode(document(result, verbose_errors, render), opts);
        }
    }
    return result.to_json().dump(2);
}

}  // namespace cli
}  // namespace fairyfly
