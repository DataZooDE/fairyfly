#include "include/mcp/tool_catalog.h"

#include <stdexcept>

#include "include/command_table.h"
#include "include/fill_fields.h"

namespace fairyfly::mcp {

namespace {

const json* find_arg(const json& args, const char* key) {
    if (!args.is_object()) return nullptr;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return nullptr;
    return &*it;
}

std::optional<int> connection_arg(const json& args, const Policy& policy) {
    if (const json* c = find_arg(args, "connection")) {
        if (!c->is_number_integer() || c->get<long long>() < 0)
            throw std::invalid_argument("'connection' must be a non-negative integer");
        return static_cast<int>(c->get<long long>());
    }
    return policy.default_connection;
}

/// `fields` (+ `then_key`, `stop_on_error`): one `element fill --fields <json>` call for a whole screen.
std::vector<std::string> build_fill_fields_argv(const json& args, const Policy& policy) {
    for (const char* key : {"element", "element_id", "id", "value", "clear", "row", "column", "checkbox", "commit"})
        if (find_arg(args, key))
            throw std::invalid_argument(std::string("'fields' replaces '") + key + "': put it into the field entries");
    std::string why;
    if (!sap::parse_fill_fields(*find_arg(args, "fields"), why)) throw std::invalid_argument(why);
    std::vector<std::string> argv{"element", "fill", "--fields", find_arg(args, "fields")->dump()};
    if (const json* key = find_arg(args, "then_key")) {
        if (!key->is_string() || !sap::fill_then_vkey(key->get<std::string>()))
            throw std::invalid_argument("'then_key' must be \"enter\" or \"f8\" (save, back and cancel are separate calls)");
        argv.push_back("--then-key");
        argv.push_back(key->get<std::string>());
    }
    if (const json* stop = find_arg(args, "stop_on_error")) {
        if (!stop->is_boolean()) throw std::invalid_argument("'stop_on_error' must be a boolean");
        if (!stop->get<bool>()) argv.push_back("--keep-going");
    }
    if (auto connection = connection_arg(args, policy)) {
        argv.push_back("--connection");
        argv.push_back(std::to_string(*connection));
    }
    return argv;
}

std::vector<std::string> build_fill_argv(const json& raw_args, const Policy& policy) {
    if (!raw_args.is_object()) throw std::invalid_argument("arguments must be an object");
    if (find_arg(raw_args, "fields")) return build_fill_fields_argv(raw_args, policy);
    for (const char* key : {"then_key", "stop_on_error"})
        if (find_arg(raw_args, key)) throw std::invalid_argument(std::string("'") + key + "' needs 'fields'");
    json args = raw_args;
    catalog::normalize_element_args(args);

    const json* element = find_arg(args, "element");
    if (!element || !element->is_string() || element->get<std::string>().empty())
        throw std::invalid_argument("'element' is required: the SAP element id (e.g. wnd[0]/usr/txtRSYST-BNAME), or 'fields' "
                                    "for several fields");
    const std::string element_id = element->get<std::string>();

    // the field's own rules are the fields list's (sap::parse_fill_field)
    json field = json::object();
    for (const char* key : {"element", "value", "clear", "row", "column", "checkbox", "commit"})
        if (const json* given = find_arg(args, key)) field[key] = *given;
    std::string why;
    const auto parsed = sap::parse_fill_field(field, why);
    if (!parsed) throw std::invalid_argument(why);
    const json* value = find_arg(args, "value");
    const bool clear = parsed->clear, checkbox = parsed->checkbox, commit = parsed->commit;
    const std::optional<int> row = parsed->row;
    const std::string& column = parsed->column;

    std::vector<std::string> options;
    if (clear) options.push_back("--clear");
    if (row) {
        options.push_back("--row");
        options.push_back(std::to_string(*row));
        options.push_back("--column");
        options.push_back(column);
    }
    if (checkbox) options.push_back("--checkbox");
    if (commit) options.push_back("--commit");

    if (auto connection = connection_arg(args, policy)) {
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
        "Changes values in the live SAP GUI. Fill the requested fields of a WHOLE SCREEN IN ONE CALL: pass `fields` = [{element, value}, ...] "
        "(up to 50{?gui_screen_read:; get the ids from gui_screen_read with only=editable}) and optionally `then_key` enter or f8, pressed once "
        "after every field succeeded. Prefer this to one call per field: it is faster and needs one approval. "
        "Single field: enters text into an input field, clears it, or edits a "
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
          {"fields", {{"type", "array"}, {"minItems", 1}, {"maxItems", 50},
                      {"description", "Several fields of the current screen in order, instead of element/value: each "
                                      "{element, value} or {element, clear: true}; grid cells {element, row, column, value, checkbox?, commit?}"},
                      {"items", {{"type", "object"},
                                 {"required", json::array({"element"})},
                                 {"additionalProperties", false},
                                 {"properties", {{"element", {{"type", "string"}, {"description", "SAP element id"}}},
                                                 {"value", {{"type", "string"}}},
                                                 {"clear", {{"type", "boolean"}}},
                                                 {"row", {{"type", "integer"}, {"minimum", 0}}},
                                                 {"column", {{"type", "string"}}},
                                                 {"checkbox", {{"type", "boolean"}}},
                                                 {"commit", {{"type", "boolean"}}}}}}}}},
          {"then_key", {{"type", "string"}, {"enum", json::array({"enter", "f8"})},
                        {"description", "With fields: press Enter or F8 (Execute) on the main window after every field succeeded. "
                                        "Saving stays a separate gui_key_send / gui_element_click"}}},
          {"stop_on_error", {{"type", "boolean"}, {"description", "With fields: stop at the first failing field (default true)"}}},
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
