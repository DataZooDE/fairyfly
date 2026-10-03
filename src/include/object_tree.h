#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fairyfly {
namespace sap {

class ScreenElementCollector;

/// Properties requested from GuiSession.GetObjectTree (verified on SAP GUI 8000.1.7).
/// Every value comes back as a string ("true"/"false" for booleans, "" when not applicable).
/// Never add `Selected` (SAP GUI raises RPC_E_SERVERFAULT and crashes saplogon); AccLabel,
/// Enabled, Visible and ContainerType are not usable either.
const std::vector<std::string>& object_tree_requested_properties();

/// Upper bounds enforced by parse_object_tree.
constexpr int kObjectTreeMaxDepth = 64;
constexpr size_t kObjectTreeMaxNodes = 20000;

struct ObjectTreeNode {
    std::string id;    // properties.Id
    std::string type;  // properties.Type
    std::map<std::string, std::string> props;  // every returned property (including Id, Type)
    std::vector<ObjectTreeNode> children;

    /// Property value or "" when absent.
    const std::string& prop(const std::string& name) const;
};

/// A validated GetObjectTree answer: `root` is the queried element (first child of the answer).
struct ObjectTree {
    ObjectTreeNode root;
    size_t node_count = 0;
};

/// Strict validator/parser for the GetObjectTree answer
/// `{"children":[ NODE ]}`, NODE = `{"properties":{string values}, "children":[NODE...]}`.
/// Anything unexpected (bad JSON, wrong root id, missing/non-string Id/Type, non-string
/// property values, depth > kObjectTreeMaxDepth, more than kObjectTreeMaxNodes nodes) yields
/// nullopt and a short constant reason that never contains payload text. Text and
/// DisplayedText of every GuiPasswordField are erased right after parsing.
std::optional<ObjectTree> parse_object_tree(const std::string& json_text,
                                            const std::string& expected_root_id,
                                            std::string* error_reason = nullptr);

/// Read-only index over an ObjectTree: id -> node and id -> parent.
class TreeSnapshot {
public:
    explicit TreeSnapshot(ObjectTree tree);

    const ObjectTreeNode& root() const { return tree_->root; }
    size_t size() const { return tree_->node_count; }

    /// Node by id (first one when ids repeat) or nullptr.
    const ObjectTreeNode* find(const std::string& id) const;
    /// Children of the node with that id (empty when unknown or a leaf).
    const std::vector<ObjectTreeNode>& children_of(const std::string& id) const;
    /// Parent node or nullptr for the root and unknown ids.
    const ObjectTreeNode* parent_of(const std::string& id) const;
    /// Text property of the node with that id, "" when unknown.
    std::string text_of(const std::string& id) const;
    /// Text of the label referenced by LeftLabel (preferred) or RightLabel, "" when none.
    std::string label_text_for(const ObjectTreeNode& node) const;

private:
    std::shared_ptr<const ObjectTree> tree_;
    std::unordered_map<std::string, const ObjectTreeNode*> by_id_;
    std::unordered_map<std::string, const ObjectTreeNode*> parent_;
};

/// Phase-3 skip rule of ScreenReader::extract_metadata_for_collector: containers whose
/// content is not extractable (container/splitter/dock shells, custom controls, shellcont*).
bool is_phase3_skipped_container(const std::string& type, const std::string& element_id);

/// Text of a positioned label cell (/lbl[col,row]) with the redaction of
/// ComGuiElement::get_text for labels (structured text carrying sensitive names).
std::string redact_positioned_label_text(const std::string& text);

struct TreeReplayResult {
    /// Hosts the legacy reader would probe with FindById that the tree cannot answer:
    /// shell hosts and empty subscreen hosts without children. The caller keeps a COM probe
    /// for them (see id_probe_candidates).
    std::vector<std::string> probe_hosts;
};

/// Replays ScreenReader::discover_elements/traverse_element_tree phase 1 over the snapshot
/// without COM calls: fills the collector exactly like the legacy traversal does for the
/// window `window_id` (tbar[0..2], titl, window children). Grids, trees and tabular
/// user areas are registered by id for phase 2; their cells (set_grid_cells) are filled
/// from the node Text. `--probe-all` is not replayed.
TreeReplayResult replay_discovery(const TreeSnapshot& snapshot, const std::string& window_id,
                                  bool skip_trees, ScreenElementCollector& collector);

} // namespace sap
} // namespace fairyfly
