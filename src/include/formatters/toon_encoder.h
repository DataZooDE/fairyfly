#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <sstream>
#include <optional>

namespace fairyfly {
namespace formatters {

using json = nlohmann::json;

/// Options for TOON encoding
struct ToonOptions {
    /// Number of spaces per indentation level (default: 2)
    int indent = 2;

    /// Delimiter for array values: ',' (comma), '\t' (tab), '|' (pipe)
    char delimiter = ',';

    /// Optional marker to prefix array lengths (e.g., [#3] instead of [3])
    bool length_marker = false;
};

/// TOON (Token-Oriented Object Notation) Encoder
///
/// Implements the TOON v1.3 specification for compact, LLM-friendly data serialization.
/// Achieves 30-60% token reduction vs JSON for uniform tabular data.
///
/// Specification: https://github.com/johannschopplich/toon/blob/main/SPEC.md
///
/// Key Features:
/// - Tabular format for uniform object arrays (keys declared once)
/// - Minimal quoting (only when necessary)
/// - Indentation-based structure (like YAML)
/// - Explicit array lengths for validation
///
/// Example:
/// ```cpp
/// ToonEncoder encoder;
/// ToonOptions opts;
/// json data = {{"users", json::array({
///     {{"id", 1}, {"name", "Alice"}, {"role", "admin"}},
///     {{"id", 2}, {"name", "Bob"}, {"role", "user"}}
/// })}};
/// std::string toon = encoder.encode(data, opts);
/// // Output:
/// // users[2]{id,name,role}:
/// //   1,Alice,admin
/// //   2,Bob,user
/// ```
class ToonEncoder {
public:
    ToonEncoder() = default;

    /// Encode a JSON value to TOON format
    /// @param value JSON value to encode (object, array, or primitive)
    /// @param options Encoding options (indent, delimiter, length_marker)
    /// @return TOON-formatted string (no trailing newline)
    std::string encode(const json& value, const ToonOptions& options = ToonOptions());

private:
    // Encoding state
    ToonOptions opts_;
    std::ostringstream output_;
    int depth_ = 0;

    // Delimiter context stack for nested arrays
    std::vector<char> delimiter_stack_;

    /// Get current active delimiter (from nearest array header)
    char active_delimiter() const;

    /// Get document delimiter (for non-array contexts)
    char document_delimiter() const { return opts_.delimiter; }

    // Core encoding methods
    void encode_value(const json& value);
    void encode_object(const json& obj);
    void encode_array(const json& arr);
    void encode_primitive(const json& value);

    // Array format detection and encoding
    bool is_tabular_array(const json& arr) const;
    void encode_tabular_array(const json& arr);
    void encode_list_array(const json& arr);
    void encode_inline_array(const json& arr);

    // String encoding and quoting
    std::string encode_string(const std::string& str, bool is_key = false) const;
    bool needs_quoting(const std::string& str, bool is_key = false) const;
    std::string escape_string(const std::string& str) const;
    bool is_valid_unquoted_key(const std::string& key) const;

    // Helper methods
    void write_indent();
    void write_array_header(size_t length, const std::optional<std::vector<std::string>>& fields = std::nullopt);
    std::string format_number(double num) const;
    bool is_numeric_string(const std::string& str) const;
    bool is_reserved_word(const std::string& str) const;
};

} // namespace formatters
} // namespace fairyfly
