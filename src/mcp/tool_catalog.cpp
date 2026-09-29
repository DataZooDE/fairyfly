#include "include/mcp/tool_catalog.h"

namespace fairyfly::mcp {

// PHASE 2: real read-tool catalog (owner: phase 2 worker). Placeholder keeps wiring testable.
std::vector<ToolSpec> read_tool_specs() {
    ToolSpec doctor;
    doctor.def.name = "sap_doctor";
    doctor.def.title = "SAP environment check";
    doctor.def.description = "Checks the SAP GUI scripting environment (placeholder).";
    doctor.def.input_schema = json{{"type", "object"}, {"properties", json::object()}};
    doctor.def.annotations = json{{"readOnlyHint", true}};
    doctor.write_tool = false;
    doctor.output = ToolOutput::Json;
    doctor.build_argv = [](const json&, const Policy&) { return std::vector<std::string>{"doctor"}; };
    std::vector<ToolSpec> specs;
    specs.push_back(std::move(doctor));
    return specs;
}

std::vector<ToolSpec> all_tool_specs() {
    auto specs = read_tool_specs();
    auto writes = write_tool_specs();
    for (auto& s : writes) specs.push_back(std::move(s));
    return specs;
}

} // namespace fairyfly::mcp
