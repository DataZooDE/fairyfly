#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace fairyfly {
namespace sap {

/// Property values of one element, already read (from COM or from a parsed object tree).
/// Strings that are empty are treated as absent where the metadata omits them.
struct ElementFacts {
    std::string id;
    std::string type;
    std::string name;
    /// Already credential-safe (see resolve_display_text).
    std::string text;
    bool enabled = true;
    bool visible = true;
    bool changeable = false;
    /// Only meaningful for GuiCheckBox and GuiRadioButton.
    std::optional<bool> selected;
    std::string label;
    std::string tooltip;
    std::string container_type;
    /// Child ids in collection order; at most MAX_CHILDREN_TO_PROCESS are emitted.
    std::vector<std::string> child_ids;
    /// GuiShell subtype ("" is reported as "N/A").
    std::string subtype;
};

/// Which properties the plain metadata reads for a type, so a COM reader can skip the reads
/// whose result the builder would drop anyway.
bool metadata_reads_changeable(const std::string& type);
bool metadata_reads_selected(const std::string& type);
bool metadata_reads_label(const std::string& type);
bool metadata_reads_tooltip(const std::string& type);

/// The container classification of ComGuiElement::get_container_type: leaf types and the fixed
/// container types are decided by the type alone; any other type is "container" when
/// `child_count()` (only called then) is positive, else "".
std::string classify_container_type(const std::string& type, const std::function<int()>& child_count);

/// Whether ElementMetadataExtractor lists the child ids of an element of this type and
/// container classification (the caller still needs at least one child).
bool metadata_lists_children(const std::string& type, const std::string& container_type);

/// Adds "children" and "child_count" when there is at least one child id.
void add_child_ids(nlohmann::json& metadata, const std::vector<std::string>& child_ids);

/// The metadata JSON of a non-shell, non-grid element (everything ElementMetadataExtractor
/// emits before the GuiShell/grid/tree/editor specific parts).
nlohmann::json build_plain_metadata(const ElementFacts& facts);

} // namespace sap
} // namespace fairyfly
