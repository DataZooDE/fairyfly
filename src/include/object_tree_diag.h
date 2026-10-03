#pragma once
// Diagnostics for GuiSession.GetObjectTree: a raw, UNREDACTED dump of the object tree used to
// verify the bulk screen reader against a live system. Only reachable with FAIRYFLY_DIAG=1.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fairyfly::diag {

/// True only for the exact value "1" (a diagnostic dump is raw data: never on by accident).
bool diag_enabled(const char* env_value);
bool diag_enabled_from_environment();

/// Number of nodes in a GetObjectTree answer: every object under a "children" array, recursively.
int count_object_tree_nodes(const nlohmann::json& tree);

using ObjectTreeFetcher =
    std::function<std::optional<std::string>(const std::string& id, const std::vector<std::string>& props)>;

/// {id, props, supported, call_ms, bytes, node_count, tree} (tree = parsed JSON), or {..., raw,
/// parse_error} when the answer is not JSON, or {..., supported:false} when the call returned nothing.
nlohmann::json build_object_tree_diagnostic(const std::string& id, const std::vector<std::string>& props,
                                            const ObjectTreeFetcher& fetch);

} // namespace fairyfly::diag
