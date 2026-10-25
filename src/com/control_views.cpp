// ComGuiGrid and ComGuiToolbar: the grid and toolbar methods of SAP GUI controls (moved out of ComGuiElement).
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
#include "include/com/control_views.h"

namespace fairyfly {
namespace sap {

namespace {

/// A toolbar's by-position answer that is a string ("" when the call fails or answers something else).
std::string text_result(HRESULT hr, const _variant_t& result) {
    return SUCCEEDED(hr) && result.vt == VT_BSTR ? com::bstr_to_utf8(result.bstrVal) : std::string();
}

}  // namespace

std::string ComGuiGrid::get_cell_value(int row, const std::string& column_name) const {
    try {
        if (!dispatch_) {
            spdlog::warn("get_cell_value called with null dispatch pointer");
            return "";
        }

        // Call GetCellValue(row As Long, column As String) As String
        DISPID dispid;
        HRESULT hr = element_.resolve_dispid(L"GetCellValue", &dispid);
        if (FAILED(hr)) {
            return "";
        }

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {_variant_t(static_cast<long>(row)), bstr_arg(column_name)},
                    result_guard.get());

        // CRITICAL FIX: Use safe_bstr_to_string() to handle NULL BSTR and access violations
        std::string cell_value;
        if (SUCCEEDED(hr)) {
            std::string context = fmt::format("get_cell_value row={} col='{}'", row, column_name);
            cell_value = safe_bstr_to_string(result_var, context.c_str());
        }

        // Cleanup happens automatically via RAII guards
        return cell_value;

    } catch (const std::exception& e) {
        spdlog::debug("get_cell_value|exception|row={}|column={}|error={}", row, column_name, e.what());
        return "";
    }
}

void ComGuiGrid::select_grid_row(int row, const std::string& column_name) {
    if (!dispatch_) throw ComException("Null GridView element");
    if (row < 0 || column_name.empty()) throw ComException("Invalid GridView row or column");

    const auto invoke_cell = [&](const wchar_t* method_name) {
        element_.call(method_name, {_variant_t(row), _variant_t(com::utf8_to_wide(column_name).c_str())},
             "GridView cell action is unavailable", "Failed to activate GridView cell");
    };

    // Click refreshes some detail panes, but read-only grids can reject it.
    // SelectedRows is the operation required for row-targeted toolbar actions.
    try {
        invoke_cell(L"SetCurrentCell");
        try { invoke_cell(L"Click"); }
        catch (const ComException&) { /* Read-only cells can reject Click. */ }
        element_.set_string_property(L"SelectedRows", std::to_string(row));
    } catch (...) {
        try { element_.set_string_property(L"SelectedRows", ""); }
        catch (...) { /* Preserve the original selection error. */ }
        throw;
    }
}

void ComGuiGrid::doubleclick_grid_cell(int row, const std::string& column_name) {
    if (!dispatch_) throw ComException("Null GridView element");
    if (row < 0 || column_name.empty()) throw ComException("Invalid GridView row or column");

    element_.call(L"SetCurrentCell", {_variant_t(row), _variant_t(com::utf8_to_wide(column_name).c_str())},
         "GridView SetCurrentCell method not found", "Failed to set GridView current cell");
    element_.call(L"DoubleClickCurrentCell", {}, "GridView DoubleClickCurrentCell method not found",
         "Failed to double-click GridView cell");
}

void ComGuiGrid::modify_grid_cell(int row, const std::string& column_name,
                                     const std::string& value, bool checkbox, bool commit) {
    if (!dispatch_) throw ComException("Null GridView element");
    if (row < 0 || column_name.empty()) throw ComException("Invalid GridView cell coordinates");

    const wchar_t* method_name = checkbox ? L"ModifyCheckBox" : L"ModifyCell";
    DISPID available;   // the method first, then the value (as before): a grid without it says so
    if (const HRESULT hr = get_dispid_via_typeinfo(dispatch_, method_name, &available); FAILED(hr))
        throw ComException("GridView cell modification is unavailable", hr);
    _variant_t cell_value;
    if (checkbox) {
        bool checked;
        if (value == "X" || value == "x" || value == "1" || value == "true" || value == "True") {
            checked = true;
        } else if (value.empty() || value == "0" || value == "false" || value == "False") {
            checked = false;
        } else {
            throw ComException("Checkbox value must be X, 1, true, 0, or false");
        }
        cell_value = _variant_t(checked);
    } else {
        cell_value = _variant_t(com::utf8_to_wide(value).c_str());
    }
    element_.call(method_name, {_variant_t(row), _variant_t(com::utf8_to_wide(column_name).c_str()), cell_value},
         "GridView cell modification is unavailable", "Failed to modify GridView cell");
    if (commit) element_.call(L"TriggerModified", {}, "GridView TriggerModified method not found", "Failed to commit GridView changes");
}

std::vector<std::string> ComGuiGrid::get_column_order() const {
    std::vector<std::string> columns;

    try {
        // Get ColumnOrder property (returns a collection)
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"ColumnOrder", &dispid);
        if (FAILED(hr)) {
            return columns;
        }

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = invoke(dispatch_, dispid, DISPATCH_PROPERTYGET, {}, result_guard.get());

        if (SUCCEEDED(hr) && result_var.vt == VT_DISPATCH && result_var.pdispVal) {
            IDispatch* coll = result_var.pdispVal;

            // Get Count property
            DISPID count_dispid;
            hr = get_dispid_via_typeinfo(coll, L"Count", &count_dispid);
            if (SUCCEEDED(hr)) {
                VARIANT count_result_var;
                com::VariantGuard count_result_guard(&count_result_var);

                hr = invoke(coll, count_dispid, DISPATCH_PROPERTYGET, {}, count_result_guard.get());

                if (SUCCEEDED(hr) && count_result_var.vt == VT_I4) {
                    int count = count_result_var.lVal;

                    // Get Item method dispid
                    DISPID item_dispid;
                    hr = get_dispid_via_typeinfo(coll, L"Item", &item_dispid);
                    if (SUCCEEDED(hr)) {
                        for (int i = 0; i < count; ++i) {
                            VARIANT item_result_var;
                            com::VariantGuard item_result_guard(&item_result_var);

                            hr = invoke(coll, item_dispid, DISPATCH_METHOD, {_variant_t(static_cast<long>(i))}, item_result_guard.get());   // VT_I4, as SAP expects

                            // CRITICAL FIX: Use safe_bstr_to_string() to handle NULL BSTR and access violations
                            if (SUCCEEDED(hr)) {
                                std::string context = fmt::format("get_column_order item[{}]", i);
                                std::string col = safe_bstr_to_string(item_result_var, context.c_str());
                                if (!col.empty()) {
                                    columns.push_back(col);
                                }
                            }
                        }
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        spdlog::debug("get_column_order failed: {}", e.what());
    }

    return columns;
}

int ComGuiToolbar::get_button_count() const {
    return element_.get_int_property(L"ButtonCount");
}

std::string ComGuiToolbar::get_button_id(int position) const { return button_string(L"GetButtonId", position); }

std::string ComGuiToolbar::button_string(const wchar_t* method, int position) const {
    try {
        _variant_t result;
        const HRESULT hr = element_.call_method(method, {_variant_t(position)}, result);
        if (FAILED(hr))
            spdlog::debug("{} failed for position {}: hr=0x{:08X}", text::wide_to_utf8_lossy(method), position, (unsigned)hr);
        return text_result(hr, result);
    } catch (const std::exception& e) {
        spdlog::debug("{} failed for position {}: {}", text::wide_to_utf8_lossy(method), position, e.what());
        return "";
    }
}

std::string ComGuiToolbar::get_button_text(int position) const { return button_string(L"GetButtonText", position); }

std::string ComGuiToolbar::get_button_tooltip(int position) const { return button_string(L"GetButtonTooltip", position); }

std::string ComGuiToolbar::get_button_type(int position) const { return button_string(L"GetButtonType", position); }

bool ComGuiToolbar::get_button_enabled(int position) const {
    try {
        _variant_t result;
        if (FAILED(element_.call_method(L"GetButtonEnabled", {_variant_t(position)}, result))) return false;
        return (result.vt == VT_UI1 || result.vt == VT_BOOL) && result.boolVal != 0;   // a byte or a boolean
    } catch (const std::exception& e) {
        spdlog::debug("get_button_enabled failed for position {}: {}", position, e.what());
        return false;
    }
}

bool ComGuiToolbar::find_toolbar_button_labels(const std::string& button_id, std::string& text,
                                               std::string& tooltip) const {
    try {
        int count = 0;
        try { count = get_button_count(); } catch (const std::exception&) {}
        for (int i = 0; i < count; ++i) {
            if (get_button_id(i) != button_id) continue;
            text = get_button_text(i);
            tooltip = get_button_tooltip(i);
            return true;
        }
        const int grid_count = get_grid_toolbar_button_count();
        for (int i = 0; i < grid_count; ++i) {
            if (get_grid_toolbar_button_id(i) != button_id) continue;
            text.clear();
            tooltip = get_grid_toolbar_button_tooltip(i);
            return true;
        }
    } catch (const std::exception& e) {
        spdlog::debug("find_toolbar_button_labels failed for '{}': {}", button_id, e.what());
    }
    return false;
}

void ComGuiToolbar::press_button(const std::string& button_id) {
    try {
        const std::wstring wide_id = com::utf8_to_wide(button_id);
        HRESULT last_error = DISP_E_MEMBERNOTFOUND;
        // GuiShell Toolbar uses PressButton; GuiGridView's built-in toolbar
        // uses PressToolbarButton. Both are exposed as synthetic CLI buttons.
        for (const wchar_t* method_name : {L"PressButton", L"PressToolbarButton"}) {
            _variant_t result;
            // plain: an access violation in PressButton must not be followed by PressToolbarButton
            const HRESULT hr = element_.call_method(method_name, {_variant_t(wide_id.c_str())}, result, SapGuiObject::Lookup::TypeInfo, SapGuiObject::Protection::Plain);
            if (SUCCEEDED(hr)) {
                spdlog::debug("Pressed toolbar button: {}", button_id);
                return;
            }
            last_error = hr;
        }
        throw ComException("Failed to press button '" + button_id + "'", last_error);
    } catch (const std::exception& e) {
        spdlog::error("press_button failed for button_id '{}': {}", button_id, e.what());
        throw;
    }
}

int ComGuiToolbar::get_grid_toolbar_button_count() const {
    try { return element_.get_int_property(L"ToolbarButtonCount"); }
    catch (const std::exception&) { return 0; }
}

std::string ComGuiToolbar::get_grid_toolbar_button_id(int position) const {
    try {
        _variant_t result;
        return text_result(element_.call_method(L"GetToolbarButtonId", {_variant_t(position)}, result, SapGuiObject::Lookup::Cached), result);
    } catch (const std::exception&) { return ""; }
}

std::string ComGuiToolbar::get_grid_toolbar_button_tooltip(int position) const {
    try {
        _variant_t result;
        return text_result(element_.call_method(L"GetToolbarButtonTooltip", {_variant_t(position)}, result, SapGuiObject::Lookup::Cached), result);
    } catch (const std::exception&) { return ""; }
}

} // namespace sap
} // namespace fairyfly
