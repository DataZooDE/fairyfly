#pragma once

#include <string>
#include <vector>
#include <sstream>

namespace fairyfly {
namespace formatters {

/// Markdown table formatter with consistent column widths and section headers
class MarkdownTableFormatter {
public:
    enum class Alignment {
        Left,
        Center,
        Right
    };

    struct CellStyle {
        bool bold = false;
        bool italic = false;
        bool code = false;
    };

    struct Row {
        std::vector<std::string> cells;
        std::vector<CellStyle> styles;
        bool is_section_header = false;
        std::string section_title;

        Row() = default;
        Row(const std::vector<std::string>& c, const std::vector<CellStyle>& s = {})
            : cells(c), styles(s) {}
    };

    MarkdownTableFormatter() = default;

    /// Add a column with header and alignment
    void add_column(const std::string& header, Alignment align = Alignment::Left);

    /// Add a regular data row
    void add_row(const std::vector<std::string>& cells, const std::vector<CellStyle>& styles = {});

    /// Add a section header (rendered as emphasized row spanning columns)
    void add_section_header(const std::string& title);

    /// Render the complete table as markdown
    std::string render() const;

    /// Clear all data
    void clear();

private:
    std::vector<std::string> column_headers_;
    std::vector<Alignment> alignments_;
    std::vector<Row> rows_;

    /// Normalize cell content (trim whitespace, handle newlines)
    std::string normalize_cell(const std::string& content) const;

    /// Escape pipe characters for markdown
    std::string escape_pipes(const std::string& content) const;

    /// Apply cell styling (bold, italic, code)
    std::string apply_style(const std::string& content, const CellStyle& style) const;

    /// Get alignment string for separator row
    std::string get_alignment_separator(size_t width, Alignment align) const;
};

} // namespace formatters
} // namespace fairyfly
