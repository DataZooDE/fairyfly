#pragma once
#include <string>
#include <vector>
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Read-only tools (sap_screen_read, ...). Implemented in tool_catalog.cpp.
std::vector<ToolSpec> read_tool_specs();
/// State-changing tools (sap_click, sap_fill, ...), all with write_tool = true. tool_catalog_write.cpp.
std::vector<ToolSpec> write_tool_specs();
/// read_tool_specs() followed by write_tool_specs().
std::vector<ToolSpec> all_tool_specs();

/// Validates `args` against the subset of JSON Schema used by the catalog (type, enum, minimum,
/// maximum, minLength, minItems, maxItems, properties, required, additionalProperties, items).
/// Throws std::invalid_argument with a model-readable message. Shared by all catalog builders.
void validate_tool_arguments(const json& args, const json& schema);

/// Helpers shared by the read and write catalogs.
namespace catalog {
/// Builds a JSON Schema object with additionalProperties:false.
json make_schema(const json& properties, const std::vector<std::string>& required = {});
/// The optional `connection` property schema.
json connection_property();
/// Pushes "--connection N": args.connection, else policy.default_connection.
void push_connection(std::vector<std::string>& argv, const json& args, const Policy& policy);
/// Pushes `name value`, using `name=value` when value starts with '-' (so it can never be parsed as an option).
void push_option(std::vector<std::string>& argv, const std::string& name, const std::string& value);
/// Throws unless `value` is safe as a CLI positional (non-empty, not starting with '-').
void require_positional(const std::string& field, const std::string& value);
} // namespace catalog

} // namespace fairyfly::mcp
