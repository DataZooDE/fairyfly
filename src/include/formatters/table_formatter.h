#pragma once

#include "element_formatter.h"

namespace fairyfly {
namespace formatters {

/// Formatter for GuiGridView and GuiTableControl elements
class TableFormatter : public ElementFormatter {
public:
    void format_to_markdown(const json& element_json, std::ostringstream& oss) const override;
    bool can_format(const std::string& type) const override;

private:
    /// Format table data as markdown table
    void format_table_data(
        const json& table_data,
        std::ostringstream& oss
    ) const;
};

} // namespace formatters
} // namespace fairyfly
