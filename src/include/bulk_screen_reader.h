#pragma once

#include "element_metadata_builder.h"
#include "object_tree.h"
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fairyfly {
namespace sap {

/// GuiSession.GetObjectTree as the bulk reader sees it: the raw answer for an element id and the
/// requested properties, or nullopt when there is none. A source that learns SAP GUI answered with
/// RPC_E_SERVERFAULT throws ObjectTreeServerFault instead of returning nullopt.
using ObjectTreeSource = std::function<std::optional<std::string>(
    const std::string& id, const std::vector<std::string>& props)>;

/// SAP GUI refused the object tree request with RPC_E_SERVERFAULT: the bulk path is switched off
/// for the whole process.
struct ObjectTreeServerFault : std::runtime_error {
    ObjectTreeServerFault() : std::runtime_error("GetObjectTree raised RPC_E_SERVERFAULT") {}
};

/// Diagnostic fault injection for live robustness checks (FAIRYFLY_DIAG=1 and FAIRYFLY_BULK_FAULT=<mode>):
/// unsupported (no answer), garbage (not JSON), wrongroot (a tree for another element), fault
/// (ObjectTreeServerFault), exception (runtime_error). Any other mode returns `real` unchanged.
ObjectTreeSource wrap_with_injected_fault(ObjectTreeSource real, const std::string& mode);

/// The mode to inject: `fault_env` only when `diag_env` is exactly "1", else "".
std::string injected_fault_mode(const char* diag_env, const char* fault_env);

/// The two values a tree answer cannot carry (AccLabel is ignored by SAP, Selected faults the call).
struct ElementProbe {
    std::string acc_label;
    std::optional<bool> selected;
};

/// Targeted per-element read of AccLabel (only when `want_acc_label`) and Selected (only when
/// `want_selected`). May throw when the element is gone; the caller then reads it the legacy way.
using ElementProbeSource = std::function<ElementProbe(const std::string& id, bool want_acc_label,
                                                      bool want_selected)>;

/// Element types whose metadata is built from the tree node. Every GuiShell and unknown type
/// is read through ElementMetadataExtractor.
bool is_bulk_plain_type(const std::string& type);

/// The metadata JSON of one plain element, equal to what ElementMetadataExtractor::extract
/// produces for it, from its tree node plus the targeted reads. nullopt means the element must be
/// read the legacy way (type outside the allowlist, a label that lives outside the snapshot).
std::optional<nlohmann::json> build_bulk_plain_element(const TreeSnapshot& snapshot,
                                                       const ObjectTreeNode& node,
                                                       const ElementProbeSource& probe);

} // namespace sap
} // namespace fairyfly
