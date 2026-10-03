#include "include/display_text_policy.h"
#include "include/sensitive_data.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace fairyfly {
namespace sap {

std::pair<std::string, std::string> split_field_index(const std::string& field) {
    const auto open = field.rfind('[');
    const auto comma = open == std::string::npos ? std::string::npos
        : field.find(',', open + 1);
    if (comma == std::string::npos || field.back() != ']')
        return std::pair{field, std::string{}};
    const auto row = field.substr(comma + 1, field.size() - comma - 2);
    if (row.empty() || !std::all_of(row.begin(), row.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        })) return std::pair{field, std::string{}};
    return std::pair{field.substr(0, open), row};
}

std::string resolve_display_text(const DisplayTextInputs& in) {
    const auto& type = in.type;
    if (type == "GuiPasswordField") return redaction_marker(redaction_reason::password_field);
    // Name/value dialogs can give the value field a neutral ID and label.
    // Look for corresponding name/key fields under the same parent before
    // reading a VALUE field. Gateway's known header-value control fails
    // closed if its NAME sibling cannot be observed.
    if (type == "GuiTextField" || type == "GuiCTextField") {
        const auto id = in.id();
        const auto separator = id.find_last_of('/');
        const auto leaf = id.substr(separator == std::string::npos ? 0 : separator + 1);
        // Table controls append [column,row] to each field ID. Preserve the
        // row so a name in another visible row cannot classify this value.
        const auto [field_leaf, value_row] = split_field_index(leaf);
        const bool known_header = field_leaf == "txtIP_HEADER_VALUE";
        if ((field_leaf.starts_with("txt") || field_leaf.starts_with("ctxt")) &&
            field_leaf.ends_with("VALUE")) {
            const auto base = field_leaf.substr(0, field_leaf.size() - 5);
            SiblingQuery query;
            query.leaves = {base + "NAME", base + "KEY", base + "FIELDNAME", base + "HEADERNAME"};
            query.row = value_row;
            std::array<bool, 4> sibling_found{};
            bool credential = false;
            const bool verified = in.sibling_probe && in.sibling_probe(
                query, [&](std::size_t index, const std::string& name) {
                    sibling_found[index] = true;
                    if (normalize_sensitive_name(name).empty() ||
                        is_redaction_marker(name) || contains_sensitive_data_name(name)) {
                        credential = true;
                        return false;
                    }
                    return true;
                });
            if (credential) return redaction_marker(redaction_reason::paired_name);
            if (!verified) return redaction_marker(redaction_reason::unverified);
            if (known_header && !sibling_found[0]) return redaction_marker(redaction_reason::unverified);
        }
    }
    if (type == "GuiTextField" || type == "GuiCTextField" ||
        type == "GuiComboBox" || type == "GuiComboBoxControl") {
        // A name that merely reports a credential's state (PASSWORD_EXT_PWD_STATE) is shown only for a field that is KNOWN
        // to be display-only and whose value looks like a state label; changeable or unknown changeability stays redacted.
        const auto changeable = in.changeable();
        const auto reason = sensitive_field_reason_for_display(
            type, in.id(), in.label(), changeable.has_value(), changeable.value_or(false), [&] {
                auto shown = in.displayed_text();
                if (!shown) throw std::runtime_error("DisplayedText unavailable");
                if (shown->empty()) return in.text();
                return *shown;
            });
        if (!reason.empty()) return redaction_marker(reason.c_str());
    }

    const auto filter_text = [&](const std::string& value) {
        const bool structured = value.find('=') != std::string::npos ||
                                value.find(':') != std::string::npos;
        if (type == "GuiTextedit" || type == "GuiShell" ||
            (structured && contains_sensitive_data_name(value))) {
            return redact_sensitive_response_text(value);
        }
        return value;
    };

    // For text fields (GuiTextField, GuiCTextField), DisplayedText contains the actual value
    // For other elements (GuiLabel, etc.), Text contains the displayed text
    // Try DisplayedText first (preferred for input fields), then fall back to Text
    const auto displayed_text = in.displayed_text();
    if (displayed_text && !displayed_text->empty()) return filter_text(*displayed_text);

    // Fall back to Text property
    return filter_text(in.text());
}

} // namespace sap
} // namespace fairyfly
