#include "include/object_tree.h"
#include "include/screen_element_collector.h"
#include "include/screen_reader.h"
#include "include/sensitive_data.h"

#include <nlohmann/json.hpp>

namespace fairyfly {
namespace sap {

const std::vector<std::string>& object_tree_requested_properties() {
    static const std::vector<std::string> props = {
        "Id", "Type", "Name", "Text", "DisplayedText", "Changeable", "SubType",
        "AccTooltip", "DefaultTooltip", "Tooltip", "LeftLabel", "RightLabel"};
    return props;
}

const std::string& ObjectTreeNode::prop(const std::string& name) const {
    static const std::string empty;
    auto it = props.find(name);
    return it == props.end() ? empty : it->second;
}

namespace {

using nlohmann::json;

// Builds the ObjectTree straight from the parser events (no intermediate DOM: a 200 KB
// answer is parsed in a few milliseconds). The grammar is fixed:
//   root   = { "children": [ NODE ] }
//   NODE   = { "properties": { "<name>": "<string>", ... }, "children": [ NODE, ... ] }
// Nesting is bounded while parsing, so a hostile answer cannot recurse deeply.
class TreeBuilder : public json::json_sax_t {
public:
    ObjectTree tree;
    const char* failure = nullptr;
    bool has_root = false;

    bool null() override { return fail("non-string value"); }
    bool boolean(bool) override { return fail("non-string value"); }
    bool number_integer(number_integer_t) override { return fail("non-string value"); }
    bool number_unsigned(number_unsigned_t) override { return fail("non-string value"); }
    bool number_float(number_float_t, const string_t&) override { return fail("non-string value"); }
    bool binary(binary_t&) override { return fail("non-string value"); }

    bool string(string_t& value) override {
        if (stack_.empty() || stack_.back().kind != Frame::Props) return fail("unexpected string");
        stack_.back().node->props.insert_or_assign(key_, std::move(value));
        return true;
    }

    bool key(string_t& value) override {
        key_ = std::move(value);
        return true;
    }

    bool start_object(std::size_t) override {
        if (stack_.empty()) {
            stack_.push_back({Frame::Root, nullptr});
            return true;
        }
        Frame& top = stack_.back();
        switch (top.kind) {
            case Frame::RootChildren:
                if (has_root) return fail("more than one root node");
                has_root = true;
                return open_node(&tree.root);
            case Frame::Children:
                top.node->children.emplace_back();
                return open_node(&top.node->children.back());
            case Frame::Node:
                if (key_ != "properties") return fail("unexpected key");
                stack_.push_back({Frame::Props, top.node});
                return true;
            default:
                return fail("unexpected object");
        }
    }

    bool end_object() override {
        const Frame frame = stack_.back();
        stack_.pop_back();
        if (frame.kind == Frame::Node) return close_node(*frame.node);
        return true;
    }

    bool start_array(std::size_t) override {
        if (stack_.empty()) return fail("answer is not an object");
        Frame& top = stack_.back();
        if (key_ != "children") return fail("unexpected key");
        if (top.kind == Frame::Root) {
            stack_.push_back({Frame::RootChildren, nullptr});
            return true;
        }
        if (top.kind == Frame::Node) {
            stack_.push_back({Frame::Children, top.node});
            return true;
        }
        return fail("unexpected array");
    }

    bool end_array() override {
        stack_.pop_back();
        return true;
    }

    bool parse_error(std::size_t, const std::string&, const json::exception&) override {
        return fail("invalid JSON");
    }

    bool fail(const char* reason) {
        if (!failure) failure = reason;
        return false;
    }

private:
    struct Frame {
        enum Kind { Root, RootChildren, Node, Props, Children } kind;
        ObjectTreeNode* node;
    };

    bool open_node(ObjectTreeNode* node) {
        int depth = 0;
        for (const auto& frame : stack_) depth += frame.kind == Frame::Node ? 1 : 0;
        if (depth > kObjectTreeMaxDepth) return fail("tree too deep");
        if (++tree.node_count > kObjectTreeMaxNodes) return fail("too many nodes");
        stack_.push_back({Frame::Node, node});
        return true;
    }

    bool close_node(ObjectTreeNode& node) {
        auto id_it = node.props.find("Id");
        auto type_it = node.props.find("Type");
        if (id_it == node.props.end() || id_it->second.empty()) return fail("node without Id");
        if (type_it == node.props.end()) return fail("node without Type");
        node.id = id_it->second;
        node.type = type_it->second;
        // Defence in depth: a password never leaves the parser, whatever the caller does.
        if (node.type == "GuiPasswordField") {
            node.props.erase("Text");
            node.props.erase("DisplayedText");
        }
        return true;
    }

    std::vector<Frame> stack_;
    std::string key_;
};

} // namespace

std::optional<ObjectTree> parse_object_tree(const std::string& json_text,
                                            const std::string& expected_root_id,
                                            std::string* error_reason) {
    auto reject = [&](const char* reason) -> std::optional<ObjectTree> {
        if (error_reason) *error_reason = reason;
        return std::nullopt;
    };
    TreeBuilder builder;
    if (!json::sax_parse(json_text, &builder))
        return reject(builder.failure ? builder.failure : "invalid JSON");
    if (!builder.has_root) return reject("answer without children");
    if (builder.tree.root.id != expected_root_id) return reject("unexpected root id");
    return std::move(builder.tree);
}

namespace {

void index_node(const ObjectTreeNode& node, const ObjectTreeNode* parent,
                std::unordered_map<std::string, const ObjectTreeNode*>& by_id,
                std::unordered_map<std::string, const ObjectTreeNode*>& parents) {
    by_id.emplace(node.id, &node);
    if (parent) parents.emplace(node.id, parent);
    for (const auto& child : node.children) index_node(child, &node, by_id, parents);
}

} // namespace

TreeSnapshot::TreeSnapshot(ObjectTree tree)
    : tree_(std::make_shared<const ObjectTree>(std::move(tree))) {
    by_id_.reserve(tree_->node_count);
    parent_.reserve(tree_->node_count);
    index_node(tree_->root, nullptr, by_id_, parent_);
}

const ObjectTreeNode* TreeSnapshot::find(const std::string& id) const {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : it->second;
}

const std::vector<ObjectTreeNode>& TreeSnapshot::children_of(const std::string& id) const {
    static const std::vector<ObjectTreeNode> none;
    const auto* node = find(id);
    return node ? node->children : none;
}

const ObjectTreeNode* TreeSnapshot::parent_of(const std::string& id) const {
    auto it = parent_.find(id);
    return it == parent_.end() ? nullptr : it->second;
}

std::string TreeSnapshot::text_of(const std::string& id) const {
    const auto* node = find(id);
    return node ? node->prop("Text") : std::string();
}

std::string TreeSnapshot::label_text_for(const ObjectTreeNode& node) const {
    for (const char* name : {"LeftLabel", "RightLabel"}) {
        const std::string& label_id = node.prop(name);
        if (label_id.empty()) continue;
        if (const auto* label = find(label_id)) return label->prop("Text");
    }
    return std::string();
}

bool is_phase3_skipped_container(const std::string& type, const std::string& element_id) {
    const auto last_slash = element_id.rfind('/');
    const std::string base = last_slash != std::string::npos ? element_id.substr(last_slash + 1)
                                                             : element_id;
    return type == "GuiContainerShell" || type == "GuiCustomControl" ||
           type == "GuiSplitterShell" || type == "GuiSplitterContainer" ||
           type == "GuiDockShell" || type == "GuiContainerCtrl" ||
           base.rfind("shellcont", 0) == 0;
}

std::string redact_positioned_label_text(const std::string& text) {
    const bool structured = text.find('=') != std::string::npos ||
                            text.find(':') != std::string::npos;
    if (structured && contains_sensitive_data_name(text)) return redact_sensitive_response_text(text);
    return text;
}

namespace {

class Replay {
public:
    Replay(const TreeSnapshot& snapshot, bool skip_trees, ScreenElementCollector& collector)
        : snapshot_(snapshot), skip_trees_(skip_trees), collector_(collector) {}

    TreeReplayResult result;

    // Mirrors ScreenReader::traverse_element_tree (no search context, no probe-all).
    void traverse(const ObjectTreeNode& node, int depth) {
        const int kMaxDepth = 15;
        if (depth >= kMaxDepth) return;
        const std::string& type = node.type;
        const std::string& id = node.id;

        if (skip_trees_ && type == "GuiTree") return;

        if (type == "GuiShell") {
            const auto kind = classify_shell_extraction(node.prop("SubType"));
            if (kind == ShellExtractionKind::Grid) {
                collector_.add_grid_id(id);
                return;
            }
            if (kind == ShellExtractionKind::Metadata) {
                collector_.add_id(id);  // legacy: collector.add(element), no known type
                return;
            }
            if (skip_trees_) return;
            collector_.add_tree_id(id);
            return;
        }

        if (type == "GuiGridView" || type == "GuiTableControl") {
            collector_.add_grid_id(id);
            return;
        }

        if (!collector_.add_id(id)) return;
        collector_.set_known_type(id, type);

        const bool is_known_leaf =
            type == "GuiButton" || type == "GuiTextField" || type == "GuiCTextField" ||
            type == "GuiPasswordField" || type == "GuiRadioButton" || type == "GuiCheckBox" ||
            type == "GuiLabel" || type == "GuiStatusbar" || type == "GuiMenu";
        if (is_known_leaf) return;

        const int child_count = static_cast<int>(node.children.size());
        const bool is_container =
            child_count > 0 || type == "GuiContainerShell" || type == "GuiSplitterShell" ||
            type == "GuiSimpleContainer" || type == "GuiScrollContainer" ||
            type == "GuiUserArea" || type == "GuiCustomControl" || type == "GuiToolbar" ||
            type == "GuiToolbarControl" || type == "GuiTitlebar" || type == "GuiTabStrip" ||
            type == "GuiTab" || type == "GuiSubScreen" || type == "GuiBox";
        if (!is_container) return;

        if (type == "GuiUserArea") {
            traverse_user_area(node, depth);
            return;  // legacy returns here unless --probe-all
        }

        for (const auto& child : node.children) traverse(child, depth + 1);

        // Legacy FindById safety net: only hosts without children can hide something the
        // tree does not show; the integration keeps a COM probe for exactly these.
        if (node.children.empty() &&
            !id_probe_candidates(type, id, 0, collector_, false).empty()) {
            result.probe_hosts.push_back(id);
        }
    }

private:
    // GuiUserArea: positioned labels /lbl[col,row] become grid cells, everything else is
    // traversed in native order; the cells are traversed afterwards when not tabular.
    void traverse_user_area(const ObjectTreeNode& node, int depth) {
        struct Cell {
            const ObjectTreeNode* node;
            int col;
            int row;
            std::string text;
        };
        std::vector<Cell> cells;
        const size_t total_items = std::max<size_t>(node.children.size(), 500);
        size_t enumerated = 0;
        for (const auto& child : node.children) {
            if (enumerated++ >= total_items) break;
            const std::string& child_id = child.id;
            size_t bracket_pos = child_id.find("/lbl[");
            if (bracket_pos != std::string::npos) {
                size_t comma_pos = child_id.find(",", bracket_pos);
                size_t close_bracket = child_id.find("]", comma_pos);
                if (comma_pos != std::string::npos && close_bracket != std::string::npos) {
                    try {
                        int col = std::stoi(child_id.substr(bracket_pos + 5, comma_pos - bracket_pos - 5));
                        int row = std::stoi(child_id.substr(comma_pos + 1, close_bracket - comma_pos - 1));
                        cells.push_back({&child, col, row, redact_positioned_label_text(child.prop("Text"))});
                        continue;
                    } catch (...) {}
                }
            }
            traverse(child, depth + 1);
        }
        if (cells.empty()) return;

        std::vector<std::tuple<int, int, std::string>> tuples;
        tuples.reserve(cells.size());
        for (const auto& cell : cells) tuples.emplace_back(cell.col, cell.row, cell.text);
        if (ScreenReader::is_tabular_userarea(tuples)) {
            collector_.add_grid_id(node.id);
            collector_.set_grid_cells(node.id, tuples);
        } else {
            for (const auto& cell : cells) traverse(*cell.node, depth + 1);
        }
    }

    const TreeSnapshot& snapshot_;
    bool skip_trees_;
    ScreenElementCollector& collector_;
};

} // namespace

TreeReplayResult replay_discovery(const TreeSnapshot& snapshot, const std::string& window_id,
                                  bool skip_trees, ScreenElementCollector& collector) {
    collector.reserve(200);
    Replay replay(snapshot, skip_trees, collector);
    const ObjectTreeNode* window = snapshot.find(window_id);
    if (!window) return replay.result;

    // Toolbars: FindById wnd/tbar[0..2]; the first miss ends the probe.
    for (int i = 0; i < 3; ++i) {
        const auto* tbar = snapshot.find(window_id + "/tbar[" + std::to_string(i) + "]");
        if (!tbar) break;
        replay.traverse(*tbar, 0);
    }
    if (const auto* titl = snapshot.find(window_id + "/titl")) replay.traverse(*titl, 0);
    for (const auto& child : window->children) replay.traverse(child, 0);
    return replay.result;
}

} // namespace sap
} // namespace fairyfly
