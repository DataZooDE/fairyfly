#include "include/formatters/toon_encoder.h"
#include <sstream>
#include <algorithm>
#include <regex>
#include <cmath>
#include <iomanip>
#include <limits>

namespace fairyfly {
namespace formatters {

// ============================================================================
// Public API
// ============================================================================

std::string ToonEncoder::encode(const json& value, const ToonOptions& options) {
    opts_ = options;
    output_.str("");
    output_.clear();
    depth_ = 0;
    delimiter_stack_.clear();

    encode_value(value);

    return output_.str();
}

// ============================================================================
// Core Encoding Logic
// ============================================================================

void ToonEncoder::encode_value(const json& value) {
    if (value.is_null()) {
        output_ << "null";
    } else if (value.is_boolean()) {
        output_ << (value.get<bool>() ? "true" : "false");
    } else if (value.is_number()) {
        encode_primitive(value);
    } else if (value.is_string()) {
        output_ << encode_string(value.get<std::string>());
    } else if (value.is_object()) {
        encode_object(value);
    } else if (value.is_array()) {
        encode_array(value);
    }
}

void ToonEncoder::encode_object(const json& obj) {
    if (obj.empty()) {
        // Empty object - just write nothing (handled by caller context)
        return;
    }

    bool first = true;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!first) {
            output_ << "\n";
        }
        first = false;

        write_indent();

        // Encode key
        std::string key = it.key();
        output_ << encode_string(key, true);

        const json& val = it.value();

        if (val.is_primitive()) {
            // Primitive value on same line
            output_ << ": ";
            encode_value(val);
        } else if (val.is_array()) {
            // Array - write header and content (no space before array header)
            encode_array(val);
        } else if (val.is_object()) {
            // Nested object
            output_ << ":";
            if (val.empty()) {
                // Empty nested object - just the key: with no content
            } else {
                // Non-empty nested object - increase depth
                depth_++;
                output_ << "\n";
                encode_object(val);
                depth_--;
            }
        }
    }
}

void ToonEncoder::encode_array(const json& arr) {
    size_t length = arr.size();

    // Empty array
    if (length == 0) {
        write_array_header(0);
        return;
    }

    // Check if all elements are primitives (inline format)
    bool all_primitives = true;
    for (const auto& elem : arr) {
        if (!elem.is_primitive()) {
            all_primitives = false;
            break;
        }
    }

    if (all_primitives) {
        encode_inline_array(arr);
    } else if (is_tabular_array(arr)) {
        encode_tabular_array(arr);
    } else {
        encode_list_array(arr);
    }
}

void ToonEncoder::encode_inline_array(const json& arr) {
    size_t length = arr.size();

    // Write header
    write_array_header(length);

    // Push delimiter context
    delimiter_stack_.push_back(opts_.delimiter);

    // Write values inline
    output_ << " ";
    for (size_t i = 0; i < length; ++i) {
        if (i > 0) {
            output_ << active_delimiter();
        }
        encode_value(arr[i]);
    }

    // Pop delimiter context
    delimiter_stack_.pop_back();
}

bool ToonEncoder::is_tabular_array(const json& arr) const {
    if (arr.empty()) return false;

    // All elements must be objects
    for (const auto& elem : arr) {
        if (!elem.is_object()) {
            return false;
        }
    }

    // All objects must have the same keys
    if (arr[0].empty()) return false;

    std::vector<std::string> first_keys;
    for (auto it = arr[0].begin(); it != arr[0].end(); ++it) {
        first_keys.push_back(it.key());
    }

    for (size_t i = 1; i < arr.size(); ++i) {
        const json& obj = arr[i];

        // Check same number of keys
        if (obj.size() != first_keys.size()) {
            return false;
        }

        // Check all keys present (order may vary)
        for (const auto& key : first_keys) {
            if (!obj.contains(key)) {
                return false;
            }
        }
    }

    // All values across all objects must be primitives
    for (const auto& obj : arr) {
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!it.value().is_primitive()) {
                return false;
            }
        }
    }

    return true;
}

void ToonEncoder::encode_tabular_array(const json& arr) {
    size_t length = arr.size();

    // Extract field names from first object (in encounter order)
    std::vector<std::string> field_names;
    for (auto it = arr[0].begin(); it != arr[0].end(); ++it) {
        field_names.push_back(it.key());
    }

    // Write header with field names
    write_array_header(length, field_names);

    // Push delimiter context
    delimiter_stack_.push_back(opts_.delimiter);

    // Write rows
    for (const auto& obj : arr) {
        output_ << "\n";
        depth_++;
        write_indent();
        depth_--;

        // Write values in field order
        for (size_t i = 0; i < field_names.size(); ++i) {
            if (i > 0) {
                output_ << active_delimiter();
            }
            encode_value(obj[field_names[i]]);
        }
    }

    // Pop delimiter context
    delimiter_stack_.pop_back();
}

void ToonEncoder::encode_list_array(const json& arr) {
    size_t length = arr.size();

    // Write header
    write_array_header(length);

    // Push delimiter context
    delimiter_stack_.push_back(opts_.delimiter);

    // Write list items
    for (const auto& elem : arr) {
        output_ << "\n";
        depth_++;
        write_indent();
        output_ << "- ";

        if (elem.is_primitive()) {
            encode_value(elem);
        } else if (elem.is_array() && elem.size() > 0 && elem[0].is_primitive()) {
            // Nested primitive array on hyphen line
            encode_inline_array(elem);
        } else if (elem.is_object()) {
            // Object as list item - first field on hyphen line
            if (elem.empty()) {
                // Empty object - just the hyphen
                output_.seekp(-2, std::ios_base::end); // Remove "- "
            } else {
                // Encode first field on hyphen line
                auto it = elem.begin();
                std::string key = it.key();
                output_ << encode_string(key, true) << ": ";

                const json& val = it.value();
                if (val.is_primitive()) {
                    encode_value(val);
                } else if (val.is_array()) {
                    encode_array(val);
                } else if (val.is_object()) {
                    // Nested object - increase depth for nested fields
                    depth_++;
                    output_ << "\n";
                    encode_object(val);
                    depth_--;
                }

                // Remaining fields at depth+1 (one more than the hyphen)
                ++it;
                if (it != elem.end()) {
                    depth_++;  // Increment for remaining fields
                    for (; it != elem.end(); ++it) {
                        output_ << "\n";
                        write_indent();
                        output_ << encode_string(it.key(), true) << ": ";

                        const json& field_val = it.value();
                        if (field_val.is_primitive()) {
                            encode_value(field_val);
                        } else if (field_val.is_array()) {
                            encode_array(field_val);
                        } else if (field_val.is_object()) {
                            depth_++;
                            output_ << "\n";
                            encode_object(field_val);
                            depth_--;
                        }
                    }
                    depth_--;  // Decrement after remaining fields
                }
            }
        } else if (elem.is_array()) {
            // Nested non-primitive array
            encode_array(elem);
        }

        depth_--;
    }

    // Pop delimiter context
    delimiter_stack_.pop_back();
}

void ToonEncoder::encode_primitive(const json& value) {
    if (value.is_number_integer()) {
        output_ << value.get<int64_t>();
    } else if (value.is_number_unsigned()) {
        output_ << value.get<uint64_t>();
    } else if (value.is_number_float()) {
        double num = value.get<double>();

        // Handle special values
        if (std::isnan(num) || std::isinf(num)) {
            output_ << "null";
            return;
        }

        // Format number without scientific notation
        output_ << format_number(num);
    }
}

// ============================================================================
// String Encoding and Quoting
// ============================================================================

std::string ToonEncoder::encode_string(const std::string& str, bool is_key) const {
    if (needs_quoting(str, is_key)) {
        return "\"" + escape_string(str) + "\"";
    }
    return str;
}

bool ToonEncoder::needs_quoting(const std::string& str, bool is_key) const {
    // Empty string
    if (str.empty()) {
        return true;
    }

    // Keys have different rules
    if (is_key) {
        return !is_valid_unquoted_key(str);
    }

    // Value quoting rules (spec §7.2)

    // Leading or trailing whitespace
    if (str.front() == ' ' || str.back() == ' ') {
        return true;
    }

    // Reserved words
    if (is_reserved_word(str)) {
        return true;
    }

    // Numeric-like strings
    if (is_numeric_string(str)) {
        return true;
    }

    // Starts with hyphen
    if (str[0] == '-') {
        return true;
    }

    // Contains special characters
    char delim = active_delimiter();
    for (char ch : str) {
        if (ch == ':' || ch == '"' || ch == '\\' ||
            ch == delim ||
            ch == '[' || ch == ']' || ch == '{' || ch == '}' ||
            ch == '\n' || ch == '\r' || ch == '\t') {
            return true;
        }
    }

    return false;
}

std::string ToonEncoder::escape_string(const std::string& str) const {
    std::string result;
    result.reserve(str.size() * 2); // Pre-allocate for worst case

    for (char ch : str) {
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"':  result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += ch; break;
        }
    }

    return result;
}

bool ToonEncoder::is_valid_unquoted_key(const std::string& key) const {
    if (key.empty()) {
        return false;
    }

    // Must start with letter or underscore
    char first = key[0];
    if (!std::isalpha(first) && first != '_') {
        return false;
    }

    // Remaining chars: letter, digit, underscore, or dot
    for (size_t i = 1; i < key.size(); ++i) {
        char ch = key[i];
        if (!std::isalnum(ch) && ch != '_' && ch != '.') {
            return false;
        }
    }

    return true;
}

bool ToonEncoder::is_reserved_word(const std::string& str) const {
    return str == "true" || str == "false" || str == "null";
}

bool ToonEncoder::is_numeric_string(const std::string& str) const {
    if (str.empty()) return false;

    // Check for numeric pattern: /^-?\d+(\.\d+)?(e[+-]?\d+)?$/i
    std::regex numeric_pattern(R"(^-?\d+(\.\d+)?(e[+-]?\d+)?$)", std::regex::icase);
    if (std::regex_match(str, numeric_pattern)) {
        return true;
    }

    // Check for leading-zero decimals: /^0\d+$/
    if (str.size() > 1 && str[0] == '0' && std::isdigit(str[1])) {
        return true;
    }

    return false;
}

// ============================================================================
// Helper Methods
// ============================================================================

void ToonEncoder::write_indent() {
    for (int i = 0; i < depth_ * opts_.indent; ++i) {
        output_ << ' ';
    }
}

void ToonEncoder::write_array_header(size_t length, const std::optional<std::vector<std::string>>& fields) {
    output_ << "[";

    // Optional length marker
    if (opts_.length_marker) {
        output_ << "#";
    }

    output_ << length;

    // Delimiter indicator (if not comma)
    if (opts_.delimiter != ',') {
        output_ << opts_.delimiter;
    }

    output_ << "]";

    // Field names for tabular format
    if (fields.has_value()) {
        output_ << "{";
        const auto& field_names = fields.value();
        for (size_t i = 0; i < field_names.size(); ++i) {
            if (i > 0) {
                output_ << opts_.delimiter;
            }
            output_ << encode_string(field_names[i], true);
        }
        output_ << "}";
    }

    output_ << ":";
}

std::string ToonEncoder::format_number(double num) const {
    // Handle -0
    if (num == 0.0) {
        return "0";
    }

    // Use default precision which gives reasonable output
    // This matches JavaScript's Number.toString() behavior
    std::ostringstream oss;
    oss << num;
    std::string result = oss.str();

    // Check if scientific notation was used
    if (result.find('e') != std::string::npos || result.find('E') != std::string::npos) {
        // Convert to non-exponential form with sufficient precision
        oss.str("");
        oss.clear();

        // Use high precision for conversion but then trim
        oss << std::fixed << std::setprecision(15) << num;
        result = oss.str();

        // Remove trailing zeros after decimal point
        if (result.find('.') != std::string::npos) {
            result.erase(result.find_last_not_of('0') + 1, std::string::npos);
            // Remove trailing decimal point if all zeros were removed
            if (result.back() == '.') {
                result.pop_back();
            }
        }
    }

    return result;
}

char ToonEncoder::active_delimiter() const {
    // Return delimiter from nearest array header, or document delimiter
    if (!delimiter_stack_.empty()) {
        return delimiter_stack_.back();
    }
    return document_delimiter();
}

} // namespace formatters
} // namespace fairyfly
