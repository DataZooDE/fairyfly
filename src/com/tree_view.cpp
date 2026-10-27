// ComGuiTree: the tree methods of a GuiTree / tree shell (moved out of ComGuiElement).
#include "include/com/hresult.h"
#include "include/text/utf8.h"
#include "include/com/invoke.h"
#include "include/string_utils.h"
#include "include/com/wrapper.h"
#include "include/com/wrapper_helpers.h"
#include "include/com/raii_helpers.h"
#include "include/com/utf8.h"
#include "include/sensitive_data.h"
#include "include/display_text_policy.h"
#include "include/element_metadata_builder.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include "include/com/tree_view.h"

namespace fairyfly {
namespace sap {

std::vector<std::string> ComGuiTree::get_all_node_keys() const {
    std::vector<std::string> keys;
    try {
        const auto items = collection_member_items(dispatch_, L"GetAllNodeKeys", DISPATCH_METHOD);
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].vt == VT_BSTR) {
                const std::string context = fmt::format("get_all_node_keys item[{}]", i);
                std::string key = safe_bstr_to_string(items[i], context.c_str());
                if (!key.empty()) keys.push_back(std::move(key));
            } else if (items[i].vt == VT_I4) {
                keys.push_back(std::to_string(items[i].lVal));
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("get_all_node_keys failed: {}", e.what());
    }
    return keys;
}

std::vector<std::string> ComGuiTree::get_tree_column_names() const {
    std::vector<std::string> names;
    if (!dispatch_) return names;
    try {
        for (const auto& item : collection_member_items(dispatch_, L"GetColumnNames", DISPATCH_METHOD, 100))
            if (item.vt == VT_BSTR) names.push_back(safe_bstr_to_string(item, "get_tree_column_names item"));
    } catch (const std::exception& e) {
        spdlog::debug("get_tree_column_names failed: {}", e.what());
    }
    return names;
}

/// A string the tree answers for a node key (GetNodeTextByKey, GetNodePathByKey): "" when the control or the method
/// is missing or the call fails; a null or unreadable BSTR is "" too (safe_bstr_to_string).
std::string ComGuiTree::string_by_key(const wchar_t* method, const std::string& key, const char* operation) const {
    try {
        if (!dispatch_) {
            spdlog::warn("{} called with null dispatch pointer", operation);
            return "";
        }
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, method, &dispid);
        if (FAILED(hr)) {
            spdlog::debug("Could not get DISPID for {}", text::wide_to_utf8_lossy(method));
            return "";
        }
        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(key)}, result_guard.get());
        if (FAILED(hr)) return "";
        const std::string context = fmt::format("{} key='{}'", operation, key);
        return safe_bstr_to_string(result_var, context.c_str());
    } catch (const std::exception& e) {
        spdlog::debug("{}|exception|key={}|error={}", operation, key, e.what());
        return "";
    }
}

std::vector<std::string> ComGuiTree::item_texts(const std::string& key, const std::vector<std::string>& columns,
                                                bool skip_hierarchy) const {
    std::vector<std::string> texts;
    texts.reserve(columns.size());
    for (const auto& column : columns) {
        const bool skipped = skip_hierarchy && (column.empty() || column == "HierarchyHeader");
        texts.push_back(skipped ? std::string() : get_item_text(key, column));
    }
    return texts;
}

std::string ComGuiTree::get_node_text_by_key(const std::string& key) const {
    return string_by_key(L"GetNodeTextByKey", key, "get_node_text_by_key");
}

std::string ComGuiTree::get_node_path_by_key(const std::string& key) const {
    return string_by_key(L"GetNodePathByKey", key, "get_node_path_by_key");
}


std::string ComGuiTree::get_item_text(const std::string& node_key, const std::string& column_name) const {
    try {
        // Validate dispatch pointer before attempting COM calls
        if (!dispatch_) {
            spdlog::warn("get_item_text called with null dispatch pointer");
            return "";
        }

        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"GetItemText", &dispid);
        if (FAILED(hr)) {
            if (hr == E_FAIL) {
                spdlog::error("get_item_text|stale_com_pointer|key={}|column={}", node_key, column_name);
            }
            return "";
        }

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(node_key), bstr_arg(column_name)}, result_guard.get());

        // CRITICAL FIX: Use safe_bstr_to_string() to handle NULL BSTR and access violations
        std::string text;
        if (SUCCEEDED(hr)) {
            std::string context = fmt::format("get_item_text key='{}' col='{}'", node_key, column_name);
            text = safe_bstr_to_string(result_var, context.c_str());
        } else {
            spdlog::debug("get_item_text|invoke_failed|key={}|column={}|hr={}",
                         node_key, column_name, hresult_hex(hr));
        }

        // Cleanup happens automatically via VariantGuard RAII
        return text;

    } catch (const std::exception& e) {
        spdlog::debug("get_item_text|exception|key={}|column={}|error={}", node_key, column_name, e.what());
        return "";
    }
}

void ComGuiTree::select_node(const std::string& node_key) {
    try {
        // SelectNode may add to an existing tree selection. A subsequent SAP toolbar
        // action can then operate on the stale node instead of the requested one.
        DISPID unselect_dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"UnselectAll", &unselect_dispid);
        if (FAILED(hr)) {
            throw ComException("UnselectAll method not found - element may not be a tree control", hr);
        }
        hr = invoke(dispatch_, unselect_dispid, DISPATCH_METHOD);
        if (FAILED(hr)) {
            throw ComException("Failed to clear earlier tree selections", hr);
        }

        DISPID dispid;
        hr = get_dispid_via_typeinfo(dispatch_, L"SelectNode", &dispid);
        if (FAILED(hr)) {
            throw ComException("SelectNode method not found - element may not be a tree control", hr);
        }

        // the key as UTF-8 (it went through the ANSI code page before)
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(node_key)});

        if (FAILED(hr)) {
            throw ComException("Failed to select tree node '" + node_key + "'", hr);
        }

        spdlog::debug("Successfully selected tree node: {}", node_key);

    } catch (const std::exception& e) {
        spdlog::error("select_node failed for key '{}': {}", node_key, e.what());
        throw;
    }
}

void ComGuiTree::expand_node(const std::string& node_key) {
    try {
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"ExpandNode", &dispid);
        if (FAILED(hr)) {
            throw ComException("ExpandNode method not found - element may not be a tree control", hr);
        }

        // the key as UTF-8 (it went through the ANSI code page before)
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(node_key)});

        if (FAILED(hr)) {
            throw ComException("Failed to expand tree node '" + node_key + "'", hr);
        }

        spdlog::debug("Successfully expanded tree node: {}", node_key);

    } catch (const std::exception& e) {
        spdlog::error("expand_node failed for key '{}': {}", node_key, e.what());
        throw;
    }
}

void ComGuiTree::collapse_node(const std::string& node_key) {
    try {
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"CollapseNode", &dispid);
        if (FAILED(hr)) {
            throw ComException("CollapseNode method not found - element may not be a tree control", hr);
        }

        // the key as UTF-8 (it went through the ANSI code page before)
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(node_key)});

        if (FAILED(hr)) {
            throw ComException("Failed to collapse tree node '" + node_key + "'", hr);
        }

        spdlog::debug("Successfully collapsed tree node: {}", node_key);

    } catch (const std::exception& e) {
        spdlog::error("collapse_node failed for key '{}': {}", node_key, e.what());
        throw;
    }
}

void ComGuiTree::doubleclick_node(const std::string& node_key) {
    try {
        // Some list trees (SUIM) expose the visible label as a TEXT item while
        // GetNodeTextByKey returns an empty string. DoubleClickNode succeeds but
        // does not activate that text item on those controls.
        if (get_node_text_by_key(node_key).empty()) {
            const auto item_columns = get_tree_column_names();
            if (std::find(item_columns.begin(), item_columns.end(), "TEXT") != item_columns.end() &&
                !get_item_text(node_key, "TEXT").empty()) {
                DISPID item_dispid;
                if (SUCCEEDED(get_dispid_via_typeinfo(dispatch_, L"DoubleClickItem", &item_dispid))) {
                    // DoubleClickItem(node key, item name "TEXT")
                    HRESULT hr = invoke(dispatch_, item_dispid, DISPATCH_METHOD, {bstr_arg(node_key), bstr_arg("TEXT")});
                    if (FAILED(hr)) throw ComException("Failed to double-click tree text item", hr);
                    return;
                }
            }
        }

        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"DoubleClickNode", &dispid);
        if (FAILED(hr)) {
            throw ComException("DoubleClickNode method not found - element may not be a tree control", hr);
        }

        // the key as UTF-8 (it went through the ANSI code page before)
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(node_key)});

        if (FAILED(hr)) {
            throw ComException("Failed to double-click tree node '" + node_key + "'", hr);
        }

        spdlog::debug("Successfully double-clicked tree node: {}", node_key);

    } catch (const std::exception& e) {
        spdlog::error("doubleclick_node failed for key '{}': {}", node_key, e.what());
        throw;
    }
}

void ComGuiTree::select_node_context_item(const std::string& node_key,
                                             const std::string& item_text) {
    if (!dispatch_) throw ComException("Null tree element");
    if (item_text.empty()) throw ComException("Context-menu item text is empty");

    // UTF-8 key and item text (they went through the ANSI code page before), both converted before the menu opens:
    // a text that is not UTF-8 must not leave an open context menu behind
    const _variant_t key = bstr_arg(node_key);
    const _variant_t item = bstr_arg(item_text);
    element_.call(L"NodeContextMenu", {key}, "Tree context-menu method not found", "Tree context-menu action failed");
    element_.call(L"SelectContextMenuItemByText", {item}, "Tree context-menu method not found",
         "Tree context-menu action failed");
}

} // namespace sap
} // namespace fairyfly
