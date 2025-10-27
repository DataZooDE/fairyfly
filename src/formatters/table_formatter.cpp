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
    oss << "### 📊 " << (name.empty() ? "Table" : escape_markdown(name)) << "\n\n";

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

    // Display data as markdown table
    if (!columns.empty()) {
        // Header row
        oss << "|";
        for (const auto& col : columns) {
            oss << " " << escape_markdown(col.get<std::string>()) << " |";
        }
        oss << "\n|";

        // Separator row
        for (size_t i = 0; i < columns.size(); ++i) {
            oss << "----------|";
        }
        oss << "\n";
    }

    // Data rows
    int row_count = 0;
    for (const auto& row : rows) {
        if (row.is_array()) {
            oss << "|";
            for (const auto& cell : row) {
                std::string cell_value = cell.is_string() ? cell.get<std::string>() : "";
                if (cell_value.empty()) {
                    cell_value = "_empty_";
                }
                oss << " " << escape_markdown(cell_value) << " |";
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
