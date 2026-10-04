#include "include/bulk_screen_reader.h"
#include "include/constants.h"
#include "include/display_text_policy.h"
#include "include/sensitive_data.h"
#include <algorithm>
#include <iterator>

namespace fairyfly {
namespace sap {

using nlohmann::json;

bool is_bulk_plain_type(const std::string& type) {
    static const char* const kPlain[] = {
        "GuiButton", "GuiTextField", "GuiCTextField", "GuiPasswordField", "GuiLabel",
        "GuiCheckBox", "GuiRadioButton", "GuiComboBox", "GuiComboBoxControl", "GuiOkCodeField",
        "GuiTab", "GuiTabStrip", "GuiBox", "GuiUserArea", "GuiSimpleContainer",
        "GuiScrollContainer", "GuiSubScreen", "GuiToolbar", "GuiToolbarControl", "GuiMenubar",
        "GuiMenu", "GuiTitlebar", "GuiStatusbar", "GuiStatusPane"};
    return std::any_of(std::begin(kPlain), std::end(kPlain),
                       [&](const char* name) { return type == name; });
}

namespace {

std::string leaf_of(const std::string& id) {
    const auto separator = id.find_last_of('/');
    return id.substr(separator == std::string::npos ? 0 : separator + 1);
}

// The sibling name fields of a VALUE field, answered from the parent's children in the snapshot
// with the rules of ComGuiElement::get_text (direct leaf first, then a bounded row enumeration).
// A node without a parent in the snapshot is unverified.
bool probe_siblings(const TreeSnapshot& snapshot, const ObjectTreeNode& node,
                    const SiblingQuery& query, const SiblingVisitor& visit) {
    const ObjectTreeNode* parent = snapshot.parent_of(node.id);
    if (!parent) return false;
    const std::string prefix = parent->id + "/";
    bool needs_enumeration = !query.row.empty();
    if (query.row.empty()) {
        for (size_t index = 0; index < query.leaves.size(); ++index) {
            const ObjectTreeNode* sibling = snapshot.find(prefix + query.leaves[index]);
            if (!sibling) {
                needs_enumeration = true;
                continue;
            }
            if (!visit(index, sibling->prop("Text"))) return true;
        }
    }
    if (needs_enumeration) {
        const int count = static_cast<int>(parent->children.size());
        if (count <= 0 || count > 200) return false;
        for (const auto& child : parent->children) {
            if (child.id.empty()) return false;
            const auto [child_field, child_row] = split_field_index(leaf_of(child.id));
            if (child_row != query.row) continue;
            const auto match = std::find(query.leaves.begin(), query.leaves.end(), child_field);
            if (match == query.leaves.end()) continue;
            if (!visit(static_cast<size_t>(match - query.leaves.begin()), child.prop("Text")))
                return true;
        }
    }
    return true;
}

} // namespace

std::optional<json> build_bulk_plain_element(const TreeSnapshot& snapshot, const ObjectTreeNode& node,
                                             const ElementProbeSource& probe) {
    const std::string& type = node.type;
    if (!is_bulk_plain_type(type)) return std::nullopt;

    ElementFacts facts;
    facts.id = node.id;
    facts.type = type;
    facts.name = node.prop("Name");

    const bool wants_label = metadata_reads_label(type);
    const bool wants_selected = metadata_reads_selected(type);

    // One targeted FindById read per element that needs AccLabel and/or Selected.
    bool probed = false;
    ElementProbe probe_result;
    const auto ensure_probe = [&] {
        if (probed) return;
        probed = true;
        probe_result = probe(node.id, wants_label, wants_selected);
    };

    // AccLabel (SAP ignores it in the tree, so it is read per element), else the Text of the
    // element LeftLabel/RightLabel names. Memoised: the display text policy asks for it too.
    bool label_resolved = false;
    bool label_outside_snapshot = false;
    std::string label;
    const auto resolve_label = [&]() -> const std::string& {
        if (label_resolved) return label;
        label_resolved = true;
        if (!wants_label) return label;
        ensure_probe();
        label = probe_result.acc_label;
        if (!label.empty()) return label;
        for (const char* side : {"LeftLabel", "RightLabel"}) {
            const std::string& label_id = node.prop(side);
            if (label_id.empty()) continue;
            const ObjectTreeNode* label_node = snapshot.find(label_id);
            if (!label_node) {
                label_outside_snapshot = true;  // read this element the legacy way
                break;
            }
            label = label_node->prop("Text");
            if (!label.empty()) break;
        }
        return label;
    };

    DisplayTextInputs in;
    in.type = type;
    in.id = [&] { return node.id; };
    in.label = [&]() -> std::string { return resolve_label(); };
    in.changeable = [&]() -> std::optional<bool> {
        const auto found = node.props.find("Changeable");
        if (found == node.props.end() || found->second.empty()) return std::nullopt;
        return found->second == "true";
    };
    in.displayed_text = [&]() -> std::optional<std::string> {
        const auto found = node.props.find("DisplayedText");
        if (found == node.props.end()) return std::nullopt;
        return found->second;
    };
    in.text = [&] { return node.prop("Text"); };
    in.sibling_probe = [&](const SiblingQuery& query, const SiblingVisitor& visit) {
        return probe_siblings(snapshot, node, query, visit);
    };
    facts.text = resolve_display_text(in);

    // The tree has no Enabled/Visible: SAP only lists elements on the current screen and the
    // legacy reader assumes enabled and visible when the property is absent.
    facts.enabled = true;
    facts.visible = true;
    facts.changeable = metadata_reads_changeable(type) && node.prop("Changeable") == "true";

    if (wants_label) facts.label = resolve_label();
    if (label_outside_snapshot) return std::nullopt;
    if (wants_selected) {
        ensure_probe();
        facts.selected = probe_result.selected.value_or(false);
    }

    if (metadata_reads_tooltip(type)) {
        for (const char* name : {"AccTooltip", "DefaultTooltip", "Tooltip"}) {
            facts.tooltip = node.prop(name);
            if (!facts.tooltip.empty()) break;
        }
    }

    facts.container_type = classify_container_type(
        type, [&] { return static_cast<int>(node.children.size()); });

    if (metadata_lists_children(type, facts.container_type)) {
        const size_t limit = (std::min)(node.children.size(),
                                        static_cast<size_t>(constants::MAX_CHILDREN_TO_PROCESS));
        for (size_t i = 0; i < limit; ++i)
            if (!node.children[i].id.empty()) facts.child_ids.push_back(node.children[i].id);
    }
    return build_plain_metadata(facts);
}

ObjectTreeSource wrap_with_injected_fault(ObjectTreeSource real, const std::string& mode) {
    if (mode == "unsupported") {
        return [](const std::string&, const std::vector<std::string>&) -> std::optional<std::string> { return std::nullopt; };
    }
    if (mode == "garbage") {
        return [](const std::string&, const std::vector<std::string>&) -> std::optional<std::string> {
            return std::string("{not json");
        };
    }
    if (mode == "wrongroot") {
        return [](const std::string&, const std::vector<std::string>&) -> std::optional<std::string> {
            return std::string(R"({"children":[{"properties":{"Id":"/injected/wrong-root","Type":"GuiMainWindow"}}]})");
        };
    }
    if (mode == "fault") {
        return [](const std::string&, const std::vector<std::string>&) -> std::optional<std::string> {
            throw ObjectTreeServerFault();
        };
    }
    if (mode == "exception") {
        return [](const std::string&, const std::vector<std::string>&) -> std::optional<std::string> {
            throw std::runtime_error("injected object tree failure");
        };
    }
    return real;
}

std::string injected_fault_mode(const char* diag_env, const char* fault_env) {
    if (!diag_env || std::string(diag_env) != "1" || !fault_env) return "";
    return fault_env;
}

} // namespace sap
} // namespace fairyfly
