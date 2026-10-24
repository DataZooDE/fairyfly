#include "include/fill_fields.h"

#include <algorithm>
#include <cctype>

namespace fairyfly::sap {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool checkbox_value_ok(const std::string& v) {
    return v == "X" || v == "x" || v == "1" || v == "true" || v == "True" || v == "0" || v == "false" || v == "False" ||
           v.empty();
}

}  // namespace

std::optional<std::vector<FillField>> parse_fill_fields(const nlohmann::json& fields, std::string& why) {
    if (!fields.is_array() || fields.empty() || fields.size() > kMaxFillFields) {
        why = "'fields' must be an array of 1 to " + std::to_string(kMaxFillFields) + " objects";
        return std::nullopt;
    }
    std::vector<FillField> out;
    out.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        const std::string at = "fields[" + std::to_string(i) + "]: ";
        if (!f.is_object()) {
            why = at + "must be an object";
            return std::nullopt;
        }
        for (auto it = f.begin(); it != f.end(); ++it) {
            static const char* known[] = {"element", "id", "element_id", "value", "clear", "row", "column", "checkbox", "commit"};
            if (std::none_of(std::begin(known), std::end(known), [&](const char* k) { return it.key() == k; })) {
                why = at + "unknown property '" + it.key() + "'";
                return std::nullopt;
            }
        }
        FillField field;
        int ids = 0;
        for (const char* key : {"element", "id", "element_id"}) {
            if (!f.contains(key) || f[key].is_null()) continue;
            if (!f[key].is_string() || f[key].get<std::string>().empty()) {
                why = at + "'" + key + "' must be a non-empty string";
                return std::nullopt;
            }
            if (ids++ && f[key].get<std::string>() != field.element) {
                why = at + "give the element id once ('element', 'id' and 'element_id' are aliases)";
                return std::nullopt;
            }
            field.element = f[key].get<std::string>();
        }
        if (field.element.empty()) {
            why = at + "'element' is required: the SAP element id (e.g. wnd[0]/usr/ctxtVBAK-AUART)";
            return std::nullopt;
        }
        auto flag = [&](const char* key, bool& target) {
            if (!f.contains(key) || f[key].is_null()) return true;
            if (!f[key].is_boolean()) {
                why = at + "'" + key + "' must be a boolean";
                return false;
            }
            target = f[key].get<bool>();
            return true;
        };
        if (!flag("clear", field.clear) || !flag("checkbox", field.checkbox) || !flag("commit", field.commit))
            return std::nullopt;
        const bool has_value = f.contains("value") && !f["value"].is_null();
        if (has_value && !f["value"].is_string()) {
            why = at + "'value' must be a string";
            return std::nullopt;
        }
        if (has_value == field.clear) {
            why = at + (field.clear ? "use either 'value' or 'clear', not both" : "provide 'value' or set 'clear' to true");
            return std::nullopt;
        }
        if (has_value) field.value = f["value"].get<std::string>();
        if (f.contains("row") && !f["row"].is_null()) {
            if (!f["row"].is_number_integer() || f["row"].get<long long>() < 0 || f["row"].get<long long>() > 1000000) {
                why = at + "'row' must be a zero-based integer >= 0";
                return std::nullopt;
            }
            field.row = static_cast<int>(f["row"].get<long long>());
        }
        if (f.contains("column") && !f["column"].is_null()) {
            if (!f["column"].is_string() || f["column"].get<std::string>().empty()) {
                why = at + "'column' must be a non-empty string";
                return std::nullopt;
            }
            field.column = f["column"].get<std::string>();
        }
        const bool grid = field.row || !field.column.empty() || field.checkbox || field.commit;
        if (grid && !(field.row && !field.column.empty())) {
            why = at + "GridView cells need both 'row' and 'column' (also required with 'checkbox' or 'commit')";
            return std::nullopt;
        }
        if (field.checkbox && has_value && !checkbox_value_ok(field.value)) {
            why = at + "with 'checkbox', 'value' must be X, 1, true (checked) or 0, false (unchecked)";
            return std::nullopt;
        }
        out.push_back(std::move(field));
    }
    return out;
}

std::optional<int> fill_then_vkey(const std::string& key) {
    const std::string k = lower(key);
    if (k == "enter") return 0;
    if (k == "f8") return 8;
    return std::nullopt;
}

}  // namespace fairyfly::sap
