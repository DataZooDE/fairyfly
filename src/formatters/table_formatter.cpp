#include "include/formatters/table_formatter.h"
#include <spdlog/spdlog.h>

namespace fairyfly {
namespace formatters {

bool TableFormatter::can_format(const std::string& type) const {
    return type == "GuiGridView" || type == "GuiTableControl" || type == "GuiUserArea";
}

void TableFormatter::format_to_markdown(const json& element_json, std::ostringstream& oss) const {
    std::string type = element_json.value("type", "");
    std::string name = element_json.value("name", "");
    std::string id = element_json.value("id", "");

    // Header
    oss << "### " << (name.empty() ? "Table" : escape_markdown(name)) << "\n\n";

    // Check if we have table_data or grid_data
    const json* data_ptr = nullptr;
    if (element_json.contains("table_data") && !element_json["table_data"].is_null()) {
        data_ptr = &element_json["table_data"];
    } else if (element_json.contains("grid_data") && !element_json["grid_data"].is_null()) {
        data_ptr = &element_json["grid_data"];
    }

    if (!data_ptr || (!data_ptr->contains("columns") && !data_ptr->contains("rows"))) {
        oss << "_Table data not available_\n\n";
        oss << "| Property | Value |\n";
        oss << "|----------|-------|\n";
        oss << "| **Technical ID** | `" << id << "` |\n";
        oss << "| **Type** | " << type << " |\n\n";
        return;
    }

    const auto& table_data = *data_ptr;

    // Show metadata
    oss << "| Property | Value |\n";
    oss << "|----------|-------|\n";
    oss << "| **Technical ID** | `" << id << "` |\n";
    oss << "| **Type** | " << type << " |\n";

    int total_rows = table_data.value("total_row_count", 0);
    int visible_rows = table_data.value("visible_row_count", 0);

    // Total Rows: rows in the control. Visible Rows: rows the control shows in its own viewport (SAP
    // VisibleRowCount; says nothing about how many rows were read). Returned Rows: rows listed below.
    if (total_rows > 0) {
        oss << "| **Total Rows** | " << total_rows << " |\n";
    }
    if (visible_rows > 0) {
        oss << "| **Visible Rows (viewport)** | " << visible_rows << " |\n";
    }
    if (table_data.contains("returned") && table_data["returned"].is_number_integer()) {
        oss << "| **Returned Rows** | " << table_data["returned"].get<int>() << " |\n";
        const int offset = table_data.value("offset", 0);
        if (offset > 0) oss << "| **Offset** | " << offset << " |\n";
    }
    if (table_data.contains("rows_matched") && table_data["rows_matched"].is_number_integer()) {
        oss << "| **Rows Matched** | " << table_data["rows_matched"].get<int>() << " of "
            << table_data.value("rows_total", 0) << " |\n";
    }
    oss << "\n";

    // Format the actual table data
    format_table_data(table_data, oss);

    // Add usage examples
    if (type == "GuiGridView" ||
        (type == "GuiShell" && element_json.value("subtype", "") == "GridView")) {
        oss << "**Usage Examples:**\n";
        oss << "```bash\n";
        oss << "# Select a row\n";
        oss << "fairyfly element click '" << id << "' --row 0 --column 'COLUMN_NAME'\n\n";
        oss << "# Set cell value\n";
        oss << "fairyfly element fill '" << id << "' 'value' --row 0 --column 'COLUMN_NAME'\n";
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

        // Cap padding, but keep the full cell value in the Markdown output.
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

    // Say what part of the control the rows above are.
    int total_rows = table_data.value("total_row_count", 0);
    if (table_data.contains("returned")) {
        const int offset = table_data.value("offset", 0);
        const int trimmed = table_data.value("empty_rows_trimmed", 0);
        // With a text filter `row_count` only counts the matching rows; the window is what was read.
        const int window = table_data.value("returned", row_count);
        if (table_data.contains("rows_matched")) {
            oss << "_Text filter: " << table_data.value("rows_matched", 0) << " of "
                << table_data.value("rows_total", window) << " rows read match_\n\n";
        }
        if (table_data.contains("next_offset")) {
            oss << "_Showing rows " << offset << "-" << (offset + window - 1) << " of " << total_rows
                << "; more rows: offset=" << table_data["next_offset"].get<int>() << " (--offset "
                << table_data["next_offset"].get<int>() << ")_\n\n";
        } else if (trimmed > 0) {
            oss << "_The control exposes " << table_data.value("exposed_rows", offset + window)
                << " of " << total_rows << " rows (" << trimmed << " empty padding row(s) trimmed)_\n\n";
        } else if (offset > 0) {
            oss << "_Showing rows " << offset << "-" << (offset + window - 1) << " of " << total_rows
                << "_\n\n";
        }
    } else if (total_rows > row_count) {
        oss << "_Showing " << row_count << " of " << total_rows << " rows_\n\n";
    }
}

} // namespace formatters
} // namespace fairyfly
