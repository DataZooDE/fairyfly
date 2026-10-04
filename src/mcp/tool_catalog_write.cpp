#include "include/mcp/tool_catalog.h"

#include <stdexcept>

#include "include/command_table.h"

namespace fairyfly::mcp {

namespace {

const json* find_arg(const json& args, const char* key) {
    if (!args.is_object()) return nullptr;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return nullptr;
    return &*it;
}

bool bool_arg(const json& args, const char* key) {
    const json* v = find_arg(args, key);
    if (!v) return false;
    if (!v->is_boolean()) throw std::invalid_argument(std::string("'") + key + "' must be a boolean");
    return v->get<bool>();
}

std::vector<std::string> build_fill_argv(const json& raw_args, const Policy& policy) {
    if (!raw_args.is_object()) throw std::invalid_argument("arguments must be an object");
    json args = raw_args;
    catalog::normalize_element_args(args);

    const json* element = find_arg(args, "element");
    if (!element || !element->is_string() || element->get<std::string>().empty())
        throw std::invalid_argument("'element' is required: the SAP element id (e.g. wnd[0]/usr/txtRSYST-BNAME)");
    const std::string element_id = element->get<std::string>();

    const json* value = find_arg(args, "value");
    if (value && !value->is_string()) throw std::invalid_argument("'value' must be a string");
    const bool clear = bool_arg(args, "clear");
    if (value && clear) throw std::invalid_argument("Use either 'value' or 'clear', not both");
    if (!value && !clear) throw std::invalid_argument("Provide 'value' (text to enter) or set 'clear' to true");

    const bool checkbox = bool_arg(args, "checkbox");
    const bool commit = bool_arg(args, "commit");

    const json* row = find_arg(args, "row");
    if (row && !(row->is_number_integer() && row->get<long long>() >= 0))
        throw std::invalid_argument("'row' must be a zero-based integer >= 0");
    const json* column = find_arg(args, "column");
    if (column && !column->is_string()) throw std::invalid_argument("'column' must be a string");
    const bool has_column = column && !column->get<std::string>().empty();
    if (column && !has_column) throw std::invalid_argument("'column' must not be empty");

    const bool grid = row || has_column || checkbox || commit;
    if (grid && !(row && has_column))
        throw std::invalid_argument("GridView cells need both 'row' and 'column' (also required with 'checkbox' or 'commit')");

    if (checkbox && value) {
        const std::string& v = value->get<std::string>();
        if (!(v == "X" || v == "x" || v == "1" || v == "true" || v == "True" || v == "0" || v == "false" ||
              v == "False" || v.empty()))
            throw std::invalid_argument("With 'checkbox', 'value' must be X, 1, true (checked) or 0, false (unchecked)");
    }

    std::vector<std::string> options;
    if (clear) options.push_back("--clear");
    if (row) {
        options.push_back("--row");
        options.push_back(std::to_string(row->get<long long>()));
        options.push_back("--column");
        options.push_back(column->get<std::string>());
    }
    if (checkbox) options.push_back("--checkbox");
    if (commit) options.push_back("--commit");

    std::optional<int> connection;
    if (const json* c = find_arg(args, "connection")) {
        if (!c->is_number_integer() || c->get<long long>() < 0)
            throw std::invalid_argument("'connection' must be a non-negative integer");
        connection = static_cast<int>(c->get<long long>());
    } else {
        connection = policy.default_connection;
    }
    if (connection) {
        options.push_back("--connection");
        options.push_back(std::to_string(*connection));
    }

    std::vector<std::string> argv{"element", "fill"};
    if (value && !value->get<std::string>().empty() && value->get<std::string>()[0] == '-') {
        // A value that looks like an option must come after "--".
        argv.insert(argv.end(), options.begin(), options.end());
        argv.push_back("--");
        argv.push_back(element_id);
        argv.push_back(value->get<std::string>());
        return argv;
    }
    argv.push_back(element_id);
    if (value) argv.push_back(value->get<std::string>());
    argv.insert(argv.end(), options.begin(), options.end());
    return argv;
}

ToolSpec make_fill_spec() {
    ToolSpec spec;
    spec.def.name = "gui_element_fill";
    spec.family = command_table::find_by_tool(spec.def.name)->family;
    spec.def.title = "Fill SAP field";
    spec.def.description =
        "Changes a value in the live SAP GUI: enters text into an input field, clears it, or edits a "
        "GridView cell (row + column; checkbox toggles a checkbox cell, commit notifies SAP after the change). "
        "Confirm with the user before changing values. Never put passwords or other secrets into fill values "
        "(use gui_session_login for authentication). The result echoes the value read back from the control, "
        "except for credential fields (shown redacted); audit logs never contain the value. Give exactly one of value or clear. "
        "Check boxes take true/false (also 1/0, yes/no, on/off, x) and report the resulting `selected`; radio buttons "
        "can only be selected; combo boxes accept the entry key or its displayed text and report the resulting key/value "
        "(INVALID_ARGUMENT lists the available entries when nothing matches).";
    spec.def.input_schema = json{
        {"type", "object"},
        {"properties",
         catalog::with_element_aliases({{"element", {{"type", "string"}, {"description", "SAP element id from gui_screen_read, e.g. wnd[0]/usr/txtFIELD (aliases: id, element_id)"}}},
          {"value", {{"type", "string"}, {"description", "Text to enter (omit when clear is true). For checkbox cells: X/1/true or 0/false. For a GuiCheckBox element: true/false/1/0/yes/no/on/off/x. For a GuiRadioButton: true/1/x selects it (it cannot be cleared). For a GuiComboBox: an entry key or its displayed value"}}},
          {"clear", {{"type", "boolean"}, {"description", "Empty the field instead of entering a value"}}},
          {"row", {{"type", "integer"}, {"minimum", 0}, {"description", "Zero-based GridView row (requires column)"}}},
          {"column", {{"type", "string"}, {"description", "GridView column id (requires row)"}}},
          {"checkbox", {{"type", "boolean"}, {"description", "Set a GridView checkbox cell (requires row and column)"}}},
          {"commit", {{"type", "boolean"}, {"description", "Notify SAP after the GridView cell change (requires row and column)"}}},
          {"connection", {{"type", "integer"}, {"minimum", 0}, {"description", "Connection index; defaults to the server default"}}}})},
        {"additionalProperties", false}};
    spec.def.annotations = json{{"title", "Fill SAP field"},
                                {"readOnlyHint", false},
                                {"destructiveHint", true},
                                {"idempotentHint", true},
                                {"openWorldHint", false}};
    spec.write_tool = true;
    spec.output = ToolOutput::Json;
    spec.build_argv = build_fill_argv;
    return spec;
}

ToolSpec make_lease_spec() {
    ToolSpec spec;
    spec.def.name = "gui_session_lease";
    spec.family = command_table::find_by_tool(spec.def.name)->family;
    spec.def.title = "Manage a SAP session lease";
    spec.def.description = "Acquire an exclusive lease for one saved SAP GUI session before navigation or writes. "
        "Pass its lease_id on each state-changing call. Renew before expiry and release when finished. "
        "Only available to authenticated clients of an owner-restricted HTTP endpoint.";
    spec.def.input_schema = catalog::make_schema({
        {"action", {{"type", "string"}, {"enum", json::array({"acquire", "renew", "release"})}}},
        {"connection", catalog::connection_property()},
        {"lease_id", {{"type", "string"}, {"minLength", 1}, {"description", "Required for renew and release."}}}
    }, {"action", "connection"});
    spec.def.annotations = json{{"title", spec.def.title}, {"readOnlyHint", true},
                                {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}};
    spec.def.meta = json{{"anthropic/maxResultSizeChars", 60000}};
    spec.write_tool = false;  // Coordination is required for navigation even on a read-only endpoint.
    spec.output = ToolOutput::Json;
    const json schema = spec.def.input_schema;
    spec.build_argv = [schema](const json& raw, const Policy&) {
        const json args = raw.is_null() ? json::object() : raw;
        validate_tool_arguments(args, schema);
        const std::string action = args.at("action").get<std::string>();
        if (action != "acquire" && !args.contains("lease_id"))
            throw std::invalid_argument("'lease_id' is required for renew and release");
        return std::vector<std::string>{"session", "lease"};  // audit label only; never passed to the CLI
    };
    return spec;
}

} // namespace

std::vector<ToolSpec> write_tool_specs() {
    std::vector<ToolSpec> specs;
    specs.push_back(make_fill_spec());
    specs.push_back(make_lease_spec());
    return specs;
}

} // namespace fairyfly::mcp
