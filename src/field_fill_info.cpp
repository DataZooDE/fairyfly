#include "include/field_fill_info.h"
#include "include/string_utils.h"
#include "include/sensitive_data.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace fairyfly::sap {

namespace {

bool digits_at(const std::string& text, size_t from, size_t count) {
    if (from + count > text.size()) return false;
    for (size_t i = from; i < from + count; ++i)
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    return true;
}

std::vector<std::string> name_tokens(const std::string& name) {
    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char c : name) {
        if (std::isalnum(c)) {
            current += static_cast<char>(std::toupper(c));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

bool name_says_date(const std::string& name) {
    for (const auto& token : name_tokens(name)) {
        if (token == "DATUM" || token == "DATE" || token == "DATAB" || token == "DATBI" || token == "DATUV" ||
            token.ends_with("DAT") || token.ends_with("DATUM") || token.ends_with("DATE"))
            return true;
    }
    return false;
}

bool name_says_time(const std::string& name) {
    for (const auto& token : name_tokens(name)) {
        if (token == "TIME" || token == "UZEIT" || token.ends_with("UZEIT") || token.ends_with("TIME"))
            return true;
    }
    return false;
}

std::string truncate_utf8(const std::string& text, size_t max_chars) {
    size_t chars = 0;
    size_t i = 0;
    while (i < text.size()) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (chars == max_chars) break;
            ++chars;
        }
        ++i;
    }
    if (i >= text.size()) return text;
    return text.substr(0, i) + "...";
}

} // namespace

std::string date_format_of_text(const std::string& raw) {
    const std::string text = utils::trim(raw);
    if (text.size() != 10) return {};
    if (digits_at(text, 0, 2) && text[2] == '.' && digits_at(text, 3, 2) && text[5] == '.' && digits_at(text, 6, 4))
        return "DD.MM.YYYY";
    if (digits_at(text, 0, 4) && text[4] == '.' && digits_at(text, 5, 2) && text[7] == '.' && digits_at(text, 8, 2))
        return "YYYY.MM.DD";
    if (digits_at(text, 0, 2) && text[2] == '/' && digits_at(text, 3, 2) && text[5] == '/' && digits_at(text, 6, 4))
        return "MM/DD/YYYY";
    if (digits_at(text, 0, 4) && text[4] == '/' && digits_at(text, 5, 2) && text[7] == '/' && digits_at(text, 8, 2))
        return "YYYY/MM/DD";
    if (digits_at(text, 0, 2) && text[2] == '-' && digits_at(text, 3, 2) && text[5] == '-' && digits_at(text, 6, 4))
        return "MM-DD-YYYY";
    if (digits_at(text, 0, 4) && text[4] == '-' && digits_at(text, 5, 2) && text[7] == '-' && digits_at(text, 8, 2))
        return "YYYY-MM-DD";
    return {};
}

std::string time_format_of_text(const std::string& raw) {
    const std::string text = utils::trim(raw);
    if (text.size() == 8 && digits_at(text, 0, 2) && text[2] == ':' && digits_at(text, 3, 2) && text[5] == ':' &&
        digits_at(text, 6, 2))
        return "HH:MM:SS";
    return {};
}

std::string infer_input_kind(const FieldProbe& probe) {
    if (!date_format_of_text(probe.text_before).empty()) return "date";
    if (!time_format_of_text(probe.text_before).empty()) return "time";
    const std::string label = utils::lower_ascii(probe.label);
    const bool length_fits_date = probe.max_length == 0 || probe.max_length == 10;
    const bool length_fits_time = probe.max_length == 0 || probe.max_length == 8;
    if (length_fits_date && (name_says_date(probe.name) || name_says_date(probe.id.substr(probe.id.rfind('/') + 1)) ||
                             label.find("date") != std::string::npos || label.find("datum") != std::string::npos))
        return "date";
    if (length_fits_time && (name_says_time(probe.name) || label.find("time") != std::string::npos ||
                             label.find("uhrzeit") != std::string::npos))
        return "time";
    if (probe.numerical) return "numeric";
    return {};
}

std::string fill_value_echo(const FieldProbe& probe, const std::string& typed, bool& redacted) {
    const std::string reason = sensitive_input_field_reason(probe.type, probe.id, probe.label);
    if (!reason.empty()) {
        redacted = true;
        return redaction_marker(reason.c_str());
    }
    redacted = false;
    const std::string& shown = probe.text_after_known ? probe.text_after : typed;
    return truncate_utf8(redact_sensitive_response_text(shown), kFillEchoMaxChars);
}

std::string grid_fill_value_echo(const GridCellProbe& probe, const std::string& typed, bool& redacted) {
    redacted = true;
    // the filled column names a credential itself (also one the column order does not list)
    if (const auto reason = sensitive_name_reason(probe.column); !reason.empty()) return redaction_marker(reason.c_str());
    // ... or its displayed title does ("Password", "Kennwort" over a Z column)
    if (probe.column_title)
        if (const auto reason = sensitive_name_reason(*probe.column_title); !reason.empty()) return redaction_marker(reason.c_str());
    if (!probe.row_known || probe.columns.empty() || probe.row.size() != probe.columns.size())
        return redaction_marker(redaction_reason::unverified);

    // the row as a screen read of the grid reports it, with the filled cell as the grid now shows it
    json columns = probe.columns;
    json row = probe.row;
    const auto at = std::find(probe.columns.begin(), probe.columns.end(), probe.column);
    std::size_t index = static_cast<std::size_t>(at - probe.columns.begin());
    if (at == probe.columns.end()) {
        columns.push_back(probe.column);
        row.push_back(typed);
    }
    json table = {{"columns", columns}, {"rows", json::array({row})}};
    redact_sensitive_header_rows(table);
    const std::string shown = table["rows"][0][index].get<std::string>();
    if (is_redaction_marker(shown)) return shown;
    redacted = false;
    return truncate_utf8(redact_sensitive_response_text(shown), kFillEchoMaxChars);
}

std::vector<std::string> grid_fill_context_columns(const std::string& column, const std::vector<std::string>& columns) {
    std::vector<std::string> out;
    const auto add = [&](const std::string& c) {
        if (c != column && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    };
    if (columns.empty()) return out;
    add(columns.front());  // the row's anchor: a re-sort after the edit moves it
    bool named = false;
    for (const auto& c : columns) {
        const auto n = normalize_sensitive_name(c);
        if (n == "name" || n == "headername") {
            add(c);
            named = true;
        }
    }
    // a grid without titles may be a NAME/VALUE list whose name sits in any cell: redact_sensitive_header_rows scans
    // the whole row then, so the decision needs all of it
    if (!named && std::all_of(columns.begin(), columns.end(), [](const std::string& c) { return is_generic_column_title(c); }))
        for (const auto& c : columns) add(c);
    return out;
}

GridFillEcho fill_grid_cell_echo(const GridAccess& grid, int row, const std::string& column, const std::string& typed) {
    const std::vector<std::string> columns = grid.column_order ? grid.column_order() : std::vector<std::string>{};
    const auto context = grid_fill_context_columns(column, columns);
    const auto read_context = [&](std::vector<std::string>& values) {
        bool ok = static_cast<bool>(grid.cell);
        for (const auto& c : context) {
            const auto v = ok ? grid.cell(row, c) : std::nullopt;
            if (!v) ok = false;
            values.push_back(v.value_or(std::string()));
        }
        return ok;
    };
    std::vector<std::string> before, after;
    const bool before_ok = read_context(before);
    if (grid.modify) grid.modify();
    const bool after_ok = read_context(after);
    const auto filled = grid.cell ? grid.cell(row, column) : std::nullopt;

    GridCellProbe probe;
    probe.column = column;
    if (grid.title) probe.column_title = grid.title(column);
    // every needed cell read, before and after, and the context unchanged: the same row (no re-sort on commit)
    probe.row_known = !columns.empty() && before_ok && after_ok && filled.has_value() && before == after;
    if (probe.row_known) {
        probe.columns = context;
        probe.row = after;
        probe.columns.push_back(column);
        probe.row.push_back(*filled);
    }
    GridFillEcho echo;
    echo.value = grid_fill_value_echo(probe, typed, echo.redacted);
    return echo;
}

json build_fill_field_info(const FieldProbe& probe, const std::string& typed) {
    json info = json::object();
    info["type"] = probe.type;
    if (!sensitive_input_field_reason(probe.type, probe.id, probe.label).empty()) return info;
    if (probe.max_length > 0) info["max_length"] = probe.max_length;
    if (probe.numerical) info["numerical"] = true;
    if (probe.required) info["required"] = true;

    const std::string kind = infer_input_kind(probe);
    if (!kind.empty()) info["input_kind"] = kind;
    const bool normalized = probe.text_after_known && probe.text_after != typed;
    if (normalized) info["value_normalized"] = true;

    if (kind == "date" || kind == "time") {
        const auto shape = kind == "date" ? date_format_of_text : time_format_of_text;
        std::string format = shape(probe.text_before);
        std::string source = format.empty() ? std::string() : "field_value";
        if (format.empty() && normalized) {
            format = shape(probe.text_after);
            if (!format.empty()) source = "normalized_input";
        }
        if (format.empty()) {
            // The scripting API has no user date format (USR01-DATFM) and the field shows no sample.
            info["format_hint"] = "unknown";
        } else {
            info["format_hint"] = format;
            info["format_hint_source"] = source;
            const std::string typed_shape = shape(typed);
            // Only SAP's own normalisation is evidence of the user's format. The text the field held before the
            // fill is unvalidated (it may be an earlier rejected input), so it stays an informational hint.
            if (source == "normalized_input" && !typed_shape.empty() && typed_shape != format) {
                info["format_warning"] = "the typed value looks like " + typed_shape +
                                         " but this SAP user's format is " + format;
            }
        }
    }
    return info;
}

bool type_shows_tooltip(const std::string& type) {
    return type == "GuiButton" || type == "GuiTab" || type == "GuiCheckBox" || type == "GuiRadioButton";
}

void attach_tooltip_fields(json& data, const std::string& type, const std::string& tooltip,
                           const std::string& text) {
    if (!type_shows_tooltip(type) || tooltip.empty() || !data.is_object()) return;
    data["tooltip"] = tooltip;
    const std::string reported = data.contains("value") && data["value"].is_string()
        ? data["value"].get<std::string>()
        : data.contains("text") && data["text"].is_string() ? data["text"].get<std::string>() : std::string();
    if (!text.empty() && text != reported) data["text"] = text;
}

} // namespace fairyfly::sap
