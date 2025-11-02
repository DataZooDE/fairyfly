#include "include/formatters/table_formatter.h"
#include <spdlog/spdlog.h>

namespace fairyfly {
namespace formatters {

bool TableFormatter::can_format(const std::string& type) const {
    return type == "GuiGridView" || type == "GuiTableControl";
}

void TableFormatter::format_to_markdown(const json& element_json, std::ostringstream& oss) const {
    std::string type = element_json.value("type", "");
    std::string name = element_json.value("name", "");
    std::string id = element_json.value("id", "");

    // Header
    oss << "### " << (name.empty() ? "Table" : escape_markdown(name)) << "\n\n";

    // Check if we have table_data
    if (!element_json.contains("table_data")) {
        oss << "_Table data not available_\n\n";
        oss << "| Property | Value |\n";
        oss << "|----------|-------|\n";
        oss << "| **Technical ID** | `" << id << "` |\n";
        oss << "| **Type** | " << type << " |\n\n";
        return;
    }

    const auto& table_data = element_json["table_data"];

    // Show metadata
    oss << "| Property | Value |\n";
    oss << "|----------|-------|\n";
    oss << "| **Technical ID** | `" << id << "` |\n";
    oss << "| **Type** | " << type << " |\n";

    int total_rows = table_data.value("total_row_count", 0);
    int visible_rows = table_data.value("visible_row_count", 0);

    if (total_rows > 0) {
        oss << "| **Total Rows** | " << total_rows << " |\n";
    }
    if (visible_rows > 0) {
        oss << "| **Visible Rows** | " << visible_rows << " |\n";
    }
    oss << "\n";

    // Format the actual table data
    format_table_data(table_data, oss);

    // Add usage examples
    if (type == "GuiGridView") {
        oss << "**Usage Examples:**\n";
        oss << "```bash\n";
        oss << "# Read cell value\n";
        oss << "fairyfly get '" << id << "' --row 0 --column 'COLUMN_NAME'\n\n";
        oss << "# Set cell value\n";
        oss << "fairyfly fill '" << id << "' 'value' --row 0 --column 'COLUMN_NAME'\n";
        oss << "```\n\n";
    }
}

void TableFormatter::format_table_data(
    const json& table_data,
    std::ostringstream& oss
) const {
    auto columns = table_data.value("columns", json::array());
    auto rows = table_data.value("rows", json::array());

    if (columns.empty() && rows.empty()) {
        oss << "_No data available_\n\n";
        return;
    }

    // Calculate column widths based on max content width
    std::vector<size_t> col_widths;
    std::vector<std::string> col_names;

    if (!columns.empty()) {
        // Initialize with column header widths
        for (const auto& col : columns) {
            std::string col_name = escape_markdown(col.get<std::string>());
            col_names.push_back(col_name);
            col_widths.push_back(col_name.length());
        }

        // Update widths based on cell content
        for (const auto& row : rows) {
            if (row.is_array()) {
                for (size_t i = 0; i < row.size() && i < col_widths.size(); ++i) {
                    std::string cell_value = row[i].is_string() ? row[i].get<std::string>() : "";
                    if (cell_value.empty()) {
                        cell_value = "_empty_";
                    }
                    std::string escaped = escape_markdown(cell_value);
                    col_widths[i] = std::max(col_widths[i], escaped.length());
                }
            }
        }

        // Cap maximum width at 50 characters per column
        for (auto& width : col_widths) {
            width = std::min(width, size_t(50));
        }

        // Header row with padding
        oss << "|";
        for (size_t i = 0; i < col_names.size(); ++i) {
            oss << " " << col_names[i];
            // Pad to column width
            if (col_names[i].length() < col_widths[i]) {
                oss << std::string(col_widths[i] - col_names[i].length(), ' ');
            }
            oss << " |";
        }
        oss << "\n|";

        // Separator row matching column widths
        for (size_t i = 0; i < col_widths.size(); ++i) {
            oss << "-" << std::string(col_widths[i], '-') << "-|";
        }
        oss << "\n";
    }

    // Data rows with padding
    int row_count = 0;
    for (const auto& row : rows) {
        if (row.is_array()) {
            oss << "|";
            for (size_t i = 0; i < row.size() && i < col_widths.size(); ++i) {
                std::string cell_value = row[i].is_string() ? row[i].get<std::string>() : "";
                if (cell_value.empty()) {
                    cell_value = "_empty_";
                }
                std::string escaped = escape_markdown(cell_value);

                // Truncate if necessary
                if (escaped.length() > 50) {
                    escaped = escaped.substr(0, 47) + "...";
                }

                oss << " " << escaped;
                // Pad to column width
                if (escaped.length() < col_widths[i]) {
                    oss << std::string(col_widths[i] - escaped.length(), ' ');
                }
                oss << " |";
            }
            oss << "\n";
            row_count++;
        }
    }

    oss << "\n";

    // Show truncation note if applicable
    int total_rows = table_data.value("total_row_count", 0);
    if (total_rows > row_count) {
        oss << "_Showing " << row_count << " of " << total_rows << " rows_\n\n";
    }
}

} // namespace formatters
} // namespace fairyfly
