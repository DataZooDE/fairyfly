#include "include/formatters/markdown_table_formatter.h"
#include <algorithm>
#include <sstream>

namespace fairyfly {
namespace formatters {

void MarkdownTableFormatter::add_column(const std::string& header, Alignment align) {
    column_headers_.push_back(header);
    alignments_.push_back(align);
}

void MarkdownTableFormatter::add_row(const std::vector<std::string>& cells, const std::vector<CellStyle>& styles) {
    Row row;
    row.cells = cells;
    row.styles = styles.empty() ? std::vector<CellStyle>(cells.size()) : styles;
    row.is_section_header = false;
    rows_.push_back(row);
}

void MarkdownTableFormatter::add_section_header(const std::string& title) {
    Row row;
    row.is_section_header = true;
    row.section_title = title;
    rows_.push_back(row);
}

std::string MarkdownTableFormatter::render() const {
    if (column_headers_.empty() || rows_.empty()) {
        return "";
    }

    std::ostringstream oss;
    size_t num_cols = column_headers_.size();

    // Calculate column widths based on content
    std::vector<size_t> col_widths(num_cols, 0);

    // Consider header widths
    for (size_t i = 0; i < num_cols; ++i) {
        col_widths[i] = column_headers_[i].length();
    }

    // Consider row cell widths (excluding markdown formatting)
    for (const auto& row : rows_) {
        if (row.is_section_header) {
            continue;  // Section headers don't affect column width
        }

        for (size_t i = 0; i < std::min(row.cells.size(), num_cols); ++i) {
            std::string normalized = normalize_cell(row.cells[i]);
            // Remove markdown formatting characters for width calculation
            std::string plain = normalized;

            // Remove ** for bold
            size_t pos = 0;
            while ((pos = plain.find("**", pos)) != std::string::npos) {
                plain.erase(pos, 2);
            }

            // Remove _ for italic
            pos = 0;
            while ((pos = plain.find("_", pos)) != std::string::npos) {
                plain.erase(pos, 1);
            }

            // Remove ` for code
            pos = 0;
            while ((pos = plain.find("`", pos)) != std::string::npos) {
                plain.erase(pos, 1);
            }

            col_widths[i] = std::max(col_widths[i], plain.length());
        }
    }

    // Render header row
    oss << "|";
    for (size_t i = 0; i < num_cols; ++i) {
        oss << " " << column_headers_[i];
        // Pad to column width
        size_t padding = col_widths[i] - column_headers_[i].length();
        oss << std::string(padding, ' ') << " |";
    }
    oss << "\n";

    // Render separator row with alignment indicators
    oss << "|";
    for (size_t i = 0; i < num_cols; ++i) {
        oss << get_alignment_separator(col_widths[i], i < alignments_.size() ? alignments_[i] : Alignment::Left);
        oss << "|";
    }
    oss << "\n";

    // Render data rows
    for (const auto& row : rows_) {
        if (row.is_section_header) {
            // Render section header as emphasized row
            oss << "| _**" << escape_pipes(row.section_title) << "**_";

            // Add empty cells for remaining columns
            size_t title_len = row.section_title.length() + 6;  // 6 = length of "_**" + "**_"
            size_t padding = col_widths[0] - std::min(title_len, col_widths[0]);
            oss << std::string(padding, ' ');

            for (size_t i = 1; i < num_cols; ++i) {
                oss << " | " << std::string(col_widths[i], ' ');
            }
            oss << " |\n";
        } else {
            // Render regular data row
            oss << "|";
            for (size_t i = 0; i < num_cols; ++i) {
                std::string cell_content = "";
                if (i < row.cells.size()) {
                    std::string normalized = normalize_cell(row.cells[i]);
                    CellStyle style = i < row.styles.size() ? row.styles[i] : CellStyle();
                    cell_content = apply_style(normalized, style);
                }

                oss << " " << cell_content;

                // Calculate padding (account for markdown formatting)
                std::string plain = cell_content;
                size_t pos = 0;
                while ((pos = plain.find("**", pos)) != std::string::npos) {
                    plain.erase(pos, 2);
                }
                pos = 0;
                while ((pos = plain.find("_", pos)) != std::string::npos) {
                    plain.erase(pos, 1);
                }
                pos = 0;
                while ((pos = plain.find("`", pos)) != std::string::npos) {
                    plain.erase(pos, 1);
                }

                size_t padding = col_widths[i] - std::min(plain.length(), col_widths[i]);
                oss << std::string(padding, ' ') << " |";
            }
            oss << "\n";
        }
    }

    return oss.str();
}

void MarkdownTableFormatter::clear() {
    column_headers_.clear();
    alignments_.clear();
    rows_.clear();
}

std::string MarkdownTableFormatter::normalize_cell(const std::string& content) const {
    if (content.empty()) {
        return "";
    }

    std::string result = content;

    // Replace literal newlines with <br> for markdown
    size_t pos = 0;
    while ((pos = result.find("\n", pos)) != std::string::npos) {
        result.replace(pos, 1, "<br>");
        pos += 4;
    }

    // Trim leading/trailing whitespace
    size_t start = result.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = result.find_last_not_of(" \t\r\n");
    result = result.substr(start, end - start + 1);

    // Collapse multiple spaces to single space
    pos = 0;
    while ((pos = result.find("  ", pos)) != std::string::npos) {
        result.replace(pos, 2, " ");
    }

    return escape_pipes(result);
}

std::string MarkdownTableFormatter::escape_pipes(const std::string& content) const {
    std::string result = content;
    size_t pos = 0;
    while ((pos = result.find("|", pos)) != std::string::npos) {
        result.replace(pos, 1, "\\|");
        pos += 2;
    }
    return result;
}

std::string MarkdownTableFormatter::apply_style(const std::string& content, const CellStyle& style) const {
    std::string result = content;

    if (style.code) {
        result = "`" + result + "`";
    }
    if (style.bold) {
        result = "**" + result + "**";
    }
    if (style.italic) {
        result = "_" + result + "_";
    }

    return result;
}

std::string MarkdownTableFormatter::get_alignment_separator(size_t width, Alignment align) const {
    std::string sep(width + 2, '-');  // +2 for spaces on each side

    switch (align) {
        case Alignment::Left:
            // :--- (default)
            sep[0] = ':';
            break;
        case Alignment::Center:
            // :--:
            sep[0] = ':';
            sep[sep.length() - 1] = ':';
            break;
        case Alignment::Right:
            // ---:
            sep[sep.length() - 1] = ':';
            break;
    }

    return sep;
}

} // namespace formatters
} // namespace fairyfly
