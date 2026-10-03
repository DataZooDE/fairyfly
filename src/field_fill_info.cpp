#include "include/field_fill_info.h"
#include "include/sensitive_data.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace fairyfly::sap {

namespace {

std::string trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool digits_at(const std::string& text, size_t from, size_t count) {
    if (from + count > text.size()) return false;
    for (size_t i = from; i < from + count; ++i)
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    return true;
}

bool ends_with(const std::string& value, const char* suffix) {
    const std::string s = suffix;
    return value.size() >= s.size() && value.compare(value.size() - s.size(), s.size(), s) == 0;
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
            ends_with(token, "DAT") || ends_with(token, "DATUM") || ends_with(token, "DATE"))
            return true;
    }
    return false;
}

bool name_says_time(const std::string& name) {
    for (const auto& token : name_tokens(name)) {
        if (token == "TIME" || token == "UZEIT" || ends_with(token, "UZEIT") || ends_with(token, "TIME"))
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
    const std::string text = trim(raw);
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
    const std::string text = trim(raw);
    if (text.size() == 8 && digits_at(text, 0, 2) && text[2] == ':' && digits_at(text, 3, 2) && text[5] == ':' &&
        digits_at(text, 6, 2))
        return "HH:MM:SS";
    return {};
}

std::string infer_input_kind(const FieldProbe& probe) {
    if (!date_format_of_text(probe.text_before).empty()) return "date";
    if (!time_format_of_text(probe.text_before).empty()) return "time";
    const std::string label = lower(probe.label);
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
