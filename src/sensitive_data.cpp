#include "include/sensitive_data.h"
#include <cctype>
#include <map>
#include <regex>

namespace fairyfly::sap {

namespace {

std::string decode_url_parameter_name(const std::string& name) {
    const auto hex_digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };

    std::string decoded;
    decoded.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '%' && i + 2 < name.size()) {
            const int high = hex_digit(name[i + 1]);
            const int low = hex_digit(name[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(name[i]);
    }
    return decoded;
}

void append_utf8_codepoint(std::string& output, int codepoint) {
    if (codepoint < 0x80) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string decode_json_name_escapes(const std::string& name) {
    const auto hex_digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    const auto code_unit_at = [&](size_t start) -> int {
        if (start + 4 > name.size()) return -1;
        int code_unit = 0;
        for (size_t j = start; j < start + 4; ++j) {
            const int digit = hex_digit(name[j]);
            if (digit < 0) return -1;
            code_unit = code_unit * 16 + digit;
        }
        return code_unit;
    };
    std::string decoded;
    decoded.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '\\' && i + 5 < name.size() && name[i + 1] == 'u') {
            int codepoint = code_unit_at(i + 2);
            size_t consumed = 5;
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF && i + 11 < name.size() &&
                name[i + 6] == '\\' && name[i + 7] == 'u') {
                const int low = code_unit_at(i + 8);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    consumed = 11;
                }
            }
            if (codepoint >= 0 && (codepoint < 0xD800 || codepoint > 0xDFFF)) {
                append_utf8_codepoint(decoded, codepoint);
                i += consumed;
                continue;
            }
        }
        decoded.push_back(name[i]);
    }
    return decoded;
}

std::string decode_xml_name_entities(const std::string& name) {
    const auto hex_digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };

    std::string decoded;
    decoded.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] != '&') {
            decoded.push_back(name[i]);
            continue;
        }
        const auto end = name.find(';', i + 1);
        if (end == std::string::npos) {
            decoded.push_back(name[i]);
            continue;
        }
        const auto entity = name.substr(i + 1, end - i - 1);
        int codepoint = -1;
        if (entity.size() > 1 && entity[0] == '#') {
            const bool hex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X');
            const size_t start = hex ? 2 : 1;
            const int base = hex ? 16 : 10;
            codepoint = 0;
            if (start == entity.size()) codepoint = -1;
            for (size_t j = start; codepoint >= 0 && j < entity.size(); ++j) {
                const int digit = hex_digit(entity[j]);
                if (digit < 0 || digit >= base || codepoint > (0x10FFFF - digit) / base) {
                    codepoint = -1;
                } else {
                    codepoint = codepoint * base + digit;
                }
            }
        } else if (entity == "amp") codepoint = '&';
        else if (entity == "lt") codepoint = '<';
        else if (entity == "gt") codepoint = '>';
        else if (entity == "quot") codepoint = '"';
        else if (entity == "apos") codepoint = '\'';

        if (codepoint >= 0 && (codepoint < 0xD800 || codepoint > 0xDFFF)) {
            append_utf8_codepoint(decoded, codepoint);
            i = end;
        } else {
            decoded.push_back(name[i]);
        }
    }
    return decoded;
}

} // namespace

void redact_sensitive_report_labels(nlohmann::json& elements) {
    if (!elements.is_array()) return;
    using RowKey = std::pair<std::string, int>;
    std::map<RowKey, std::vector<size_t>> rows;
    for (size_t index = 0; index < elements.size(); ++index) {
        const auto& element = elements[index];
        if (!element.is_object() || element.value("type", "") != "GuiLabel" ||
            !element.contains("grid_row") || !element["grid_row"].is_number_integer() ||
            !element.contains("text") || !element["text"].is_string()) continue;
        const auto id = element.value("id", "");
        const auto label_pos = id.find("/lbl[");
        if (label_pos == std::string::npos) continue;
        rows[{id.substr(0, label_pos), element["grid_row"].get<int>()}].push_back(index);
    }

    for (const auto& [key, indices] : rows) {
        bool sensitive = false;
        for (const auto index : indices) {
            // Captions that merely contain a credential word count; technical object names
            // (roles, profiles) in the value column do not.
            if (!sensitive_cell_reason(elements[index]["text"].get<std::string>(), true).empty()) {
                sensitive = true;
                break;
            }
        }
        if (sensitive) {
            const auto marker = redaction_marker(redaction_reason::label_row);
            for (const auto index : indices) elements[index]["text"] = marker;
        }
    }
}

std::string redact_sensitive_response_text(const std::string& text) {
    std::string result = text;
    auto first = result.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && result.compare(first, 3, "\xEF\xBB\xBF") == 0) {
        first = result.find_first_not_of(" \t\r\n", first + 3);
    }
    if (first != std::string::npos && (result[first] == '{' || result[first] == '[')) {
        auto document = nlohmann::json::parse(result, nullptr, false);
        if (!document.is_discarded()) {
            bool changed = false;
            auto mask = [&](auto&& self, nlohmann::json& node) -> void {
                if (node.is_object()) {
                    bool named_secret = false;
                    for (auto it = node.begin(); it != node.end(); ++it) {
                        const auto label = normalize_sensitive_name(it.key());
                        if ((label == "name" || label == "headername" ||
                             label == "key" || label == "fieldname") &&
                            it.value().is_string() &&
                            contains_sensitive_data_name(it.value().get<std::string>())) {
                            named_secret = true;
                            break;
                        }
                    }
                    for (auto it = node.begin(); it != node.end(); ++it) {
                        const auto label = normalize_sensitive_name(it.key());
                        const bool is_label = label == "name" || label == "headername" ||
                                              label == "key" || label == "fieldname";
                        const bool sensitive_label = is_label && it.value().is_string() &&
                            contains_sensitive_data_name(it.value().get<std::string>());
                        if (contains_sensitive_data_name(it.key()) ||
                            (named_secret && !sensitive_label)) {
                            it.value() = "[REDACTED]";
                            changed = true;
                        } else {
                            self(self, it.value());
                        }
                    }
                } else if (node.is_array()) {
                    if (node.size() >= 2 && node[0].is_string() &&
                        contains_sensitive_data_name(node[0].get<std::string>())) {
                        for (size_t i = 1; i < node.size(); ++i) node[i] = "[REDACTED]";
                        changed = true;
                    } else {
                        for (auto& item : node) self(self, item);
                    }
                }
            };
            mask(mask, document);
            if (changed) result = document.dump(2);
        } else {
            // A truncated JSON response cannot be safely rewritten field by
            // field. Suppress it when a recognizable credential key remains.
            static const std::regex json_key(
                R"rx("((?:[^"\\]|\\u[0-9A-Fa-f]{4})+)"[ \t\r\n]*:)rx");
            // Some GUI editors contain object-like text with unquoted keys and
            // values. A generic FieldName/Value pair must still fail closed.
            static const std::regex unquoted_key(
                R"rx((?:^|[\{,])[ \t\r\n]*((?:[A-Za-z_]|\\u[0-9A-Fa-f]{4})(?:[A-Za-z0-9_.-]|\\u[0-9A-Fa-f]{4}){0,80})[ \t\r\n]*:)rx");
            static const std::regex object_pair(
                R"rx((?:^|[\{,])[ \t\r\n]*["']?((?:[A-Za-z_]|\\u[0-9A-Fa-f]{4})(?:[A-Za-z0-9_.-]|\\u[0-9A-Fa-f]{4}){0,80})["']?[ \t\r\n]*:[ \t\r\n]*["']?([^,"'\{\[\}\]\r\n]+))rx");
            static const std::regex array_pair_name(
                R"rx(\[[ \t\r\n]*["']?([A-Za-z_][A-Za-z0-9_.-]{0,80})["']?[ \t\r\n]*,)rx");
            for (std::sregex_iterator it(result.begin(), result.end(), json_key), end;
                 it != end; ++it) {
                if (contains_sensitive_data_name(decode_json_name_escapes((*it)[1].str())))
                    return "[REDACTED]";
            }
            for (std::sregex_iterator it(result.begin(), result.end(), unquoted_key), end;
                 it != end; ++it) {
                if (contains_sensitive_data_name(decode_json_name_escapes((*it)[1].str())))
                    return "[REDACTED]";
            }
            for (std::sregex_iterator it(result.begin(), result.end(), object_pair), end;
                 it != end; ++it) {
                const auto label = normalize_sensitive_name(decode_json_name_escapes((*it)[1].str()));
                if ((label == "name" || label == "headername" || label == "key" ||
                     label == "fieldname") &&
                    contains_sensitive_data_name(decode_json_name_escapes((*it)[2].str())))
                    return "[REDACTED]";
            }
            for (std::sregex_iterator it(result.begin(), result.end(), array_pair_name), end;
                 it != end; ++it) {
                if (contains_sensitive_data_name((*it)[1].str())) return "[REDACTED]";
            }
        }
    } else if (first != std::string::npos && result[first] == '<') {
        // Without an XML parser, avoid leaking partial values from a sensitive
        // element or attribute by suppressing that response as a whole.
        static const std::regex xml_named_key(
            R"(<[ \t\r\n]*(?:[A-Za-z_][A-Za-z0-9_.-]*:)?(Name|HeaderName|Key|FieldName)(?:[ \t\r\n][^>]*)?>([\s\S]*?)</[ \t\r\n]*(?:[A-Za-z_][A-Za-z0-9_.-]*:)?\1[ \t\r\n]*>)",
            std::regex_constants::icase);
        for (std::sregex_iterator it(result.begin(), result.end(), xml_named_key), end;
             it != end; ++it) {
            if (contains_sensitive_data_name(decode_xml_name_entities((*it)[2].str()))) return "[REDACTED]";
        }
        // A bounded or malformed editor read may end before the generic key's
        // closing tag. Its immediately visible text is still enough to classify.
        static const std::regex xml_partial_named_key(
            R"(<[ \t\r\n]*(?:[A-Za-z_][A-Za-z0-9_.-]*:)?(?:Name|HeaderName|Key|FieldName)(?:[ \t\r\n][^>]*)?>([^<]*))",
            std::regex_constants::icase);
        for (std::sregex_iterator it(result.begin(), result.end(), xml_partial_named_key), end;
             it != end; ++it) {
            if (contains_sensitive_data_name(decode_xml_name_entities((*it)[1].str()))) return "[REDACTED]";
        }
        static const std::regex xml_named_attribute(
            R"rx((?:[< \t\r\n])(?:[A-Za-z_][A-Za-z0-9_.-]*:)?(?:Name|HeaderName|Key|FieldName)[ \t\r\n]*=[ \t\r\n]*(["'])(.*?)\1)rx",
            std::regex_constants::icase);
        for (std::sregex_iterator it(result.begin(), result.end(), xml_named_attribute), end;
             it != end; ++it) {
            if (contains_sensitive_data_name(decode_xml_name_entities((*it)[2].str()))) return "[REDACTED]";
        }
        // A diagnostic editor may contain incomplete or malformed XML. Keep
        // the same fail-closed behavior when a generic key has no quotes.
        static const std::regex xml_unquoted_named_attribute(
            R"((?:[< \t\r\n])(?:[A-Za-z_][A-Za-z0-9_.-]*:)?(?:Name|HeaderName|Key|FieldName)[ \t\r\n]*=[ \t\r\n]*([^"' \t\r\n/>][^ \t\r\n/>]*))",
            std::regex_constants::icase);
        for (std::sregex_iterator it(result.begin(), result.end(), xml_unquoted_named_attribute), end;
             it != end; ++it) {
            if (contains_sensitive_data_name(decode_xml_name_entities((*it)[1].str()))) return "[REDACTED]";
        }
        static const std::regex xml_tag(R"(<[ \t\r\n]*/?([^ \t\r\n/>]+)(?:[ \t\r\n/>]))");
        static const std::regex xml_attribute(R"(([^ \t\r\n/=>]+)[ \t\r\n]*=[ \t\r\n]*["'])");
        for (std::sregex_iterator it(result.begin(), result.end(), xml_tag), end;
             it != end; ++it) {
            if (contains_sensitive_data_name((*it)[1].str())) return "[REDACTED]";
        }
        for (std::sregex_iterator it(result.begin(), result.end(), xml_attribute), end;
             it != end; ++it) {
            if (contains_sensitive_data_name((*it)[1].str())) return "[REDACTED]";
        }
    }

    // SAP GUI editors can return CR-only lines. A generic name/value pair may
    // span two lines, leaving the value unlabeled unless the pair is classified.
    static const std::regex plain_named_pair(
        R"((?:^|[\r\n])[ \t]*(?:Name|HeaderName|Key|FieldName)[ \t]*[:=][ \t]*([^\r\n]*)(?:\r\n|\r|\n)[ \t]*(?:Value|HeaderValue|FieldValue)[ \t]*[:=])",
        std::regex_constants::icase);
    for (std::sregex_iterator it(result.begin(), result.end(), plain_named_pair), end;
         it != end; ++it) {
        if (contains_sensitive_data_name((*it)[1].str())) return "[REDACTED]";
    }

    // API tools also display form-style and YAML-like responses. Match complete
    // lines so ordinary prose containing the same words remains readable.
    static const std::regex named_line(
        R"((^|[\r\n])([ \t]*)([A-Za-z_][A-Za-z0-9_.-]{0,80})([ \t]*[:=][ \t]*)([^\r\n]*))");
    std::string filtered;
    std::string::const_iterator cursor = result.cbegin();
    for (std::sregex_iterator it(result.cbegin(), result.cend(), named_line), end;
         it != end; ++it) {
        const auto& match = *it;
        filtered.append(cursor, match[0].first);
        if (contains_sensitive_data_name(match[3].str())) {
            filtered += match[1].str() + match[2].str() + match[3].str() +
                        match[4].str() + "[REDACTED]";
        } else {
            filtered += match.str();
        }
        cursor = match[0].second;
    }
    filtered.append(cursor, result.cend());

    // A credential can also be embedded in a URL query or fragment that appears
    // in an editor, HTTP status line, or JSON string under a generic field name.
    static const std::regex query_parameter(
        R"(([?&#;])([A-Za-z_%][A-Za-z0-9_.%+-]{0,80})=([^&#; \t\r\n"'<>),\]]*))");
    std::string query_filtered;
    cursor = filtered.cbegin();
    for (std::sregex_iterator it(filtered.cbegin(), filtered.cend(), query_parameter), end;
         it != end; ++it) {
        const auto& match = *it;
        query_filtered.append(cursor, match[0].first);
        if (contains_sensitive_data_name(decode_url_parameter_name(match[2].str()))) {
            query_filtered += match[1].str() + match[2].str() + "=[REDACTED]";
        } else {
            query_filtered += match.str();
        }
        cursor = match[0].second;
    }
    query_filtered.append(cursor, filtered.cend());
    return query_filtered;
}

std::string redact_sensitive_abap_source(const std::string& text) {
    std::string result;
    std::vector<std::string> statement_lines;
    bool sensitive = false;
    char quote = 0;

    const auto flush = [&](bool suppress_incomplete = false) {
        for (const auto& line : statement_lines) {
            if (sensitive || suppress_incomplete) {
                result += "[REDACTED]";
                if (!line.empty() && line.back() == '\n') result += '\n';
            } else {
                result += line;
            }
        }
        statement_lines.clear();
        sensitive = false;
    };

    size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto length = end == std::string::npos ? text.size() - start : end - start + 1;
        const auto line = text.substr(start, length);
        statement_lines.push_back(line);
        sensitive = sensitive || contains_sensitive_data_name(line);

        bool complete = false;
        if (!line.empty() && line.front() == '*') {
            complete = statement_lines.size() == 1;
        } else {
            for (size_t i = 0; i < line.size(); ++i) {
                if (quote == '|' && line[i] == '\\' && i + 1 < line.size()) {
                    complete = false;
                    ++i;  // Escaped character in an ABAP string template.
                } else if (quote != 0 && line[i] == quote &&
                           quote != '|' && i + 1 < line.size() && line[i + 1] == quote) {
                    complete = false;
                    ++i;  // Doubled delimiter inside a quoted literal.
                } else if (quote != 0 && line[i] == quote) {
                    complete = false;
                    quote = 0;
                } else if (quote == 0 && (line[i] == '\'' || line[i] == '`' || line[i] == '|')) {
                    complete = false;
                    quote = line[i];
                } else if (line[i] == '"' && quote == 0) {
                    break;  // ABAP end-of-line comment
                } else if (line[i] == '.' && quote == 0) {
                    complete = true;
                } else if (!std::isspace(static_cast<unsigned char>(line[i]))) {
                    complete = false;
                }
            }
        }
        if (complete) {
            flush();
            quote = 0;
        }
        start += length;
    }
    // The editor read is bounded. A value at the end of the range may only be
    // identified as a credential by a name on the next, unread line.
    flush(true);
    return result;
}

} // namespace fairyfly::sap
