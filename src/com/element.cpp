#include "include/com/wrapper.h"
#include "include/com/wrapper_helpers.h"
#include "include/com/raii_helpers.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>

namespace fairyfly {
namespace sap {

// ============================================================================
// ComGuiElement Implementation
// ============================================================================

ComGuiElement::ComGuiElement(IDispatchPtr elem) : SapGuiObject(elem) {
    if (!elem) throw ComException("Invalid element pointer");
}

ComGuiElementPtr ComGuiElement::create(IDispatchPtr elem) {
    return std::make_shared<ComGuiElement>(elem);
}

// get_id() and get_type() now inherited from SapGuiObject

std::string ComGuiElement::get_text() const {
    // For text fields (GuiTextField, GuiCTextField), DisplayedText contains the actual value
    // For other elements (GuiLabel, etc.), Text contains the displayed text
    // Try DisplayedText first (preferred for input fields), then fall back to Text
    try {
        std::string displayed_text = get_string_property(L"DisplayedText");
        if (!displayed_text.empty()) {
            return displayed_text;
        }
    } catch (const ComException&) {
        // DisplayedText property not available on this element type
    }

    // Fall back to Text property
    return get_string_property(L"Text");
}

void ComGuiElement::set_text(const std::string& text) {
    set_string_property(L"Text", text);
    spdlog::debug("Set element text: {}", text);
}

bool ComGuiElement::is_enabled() const {
    if (!dispatch_) return false;

    // Try to get Enabled property via DISPID
    _bstr_t prop(L"Enabled");
    DISPID dispid;
    HRESULT hr = get_dispid_via_typeinfo(dispatch_, prop.GetBSTR(), &dispid);

    if (FAILED(hr)) {
        // Property doesn't exist - most SAP GUI elements don't have "Enabled"
        // If the property doesn't exist, assume the element is enabled
        // (the Changeable property is the real indicator of editability)
        return true;
    }

    // Property exists, get its value
    _variant_t result;
    DISPPARAMS params = {nullptr, nullptr, 0, 0};
    hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
                           &params, &result, nullptr, nullptr);
    if (FAILED(hr)) return true;  // If we can't read it, assume enabled

    // Verify variant type before accessing boolVal to avoid undefined behavior
    if (result.vt == VT_BOOL) {
        return result.boolVal != VARIANT_FALSE;
    }

    spdlog::debug("is_enabled|unexpected type|vt={}", (int)result.vt);
    return true;  // If unexpected type, assume enabled
}

bool ComGuiElement::is_visible() const {
    return get_bool_property(L"Visible");
}

// Static helper to classify element type
GuiElementType ComGuiElement::classify_type(const std::string& type_str) {
    if (type_str.find("Button") != std::string::npos) return GuiElementType::Button;
    if (type_str.find("TextField") != std::string::npos) return GuiElementType::TextField;
    if (type_str.find("ComboBox") != std::string::npos) return GuiElementType::ComboBox;
    if (type_str.find("CheckBox") != std::string::npos) return GuiElementType::CheckBox;
    if (type_str.find("RadioButton") != std::string::npos) return GuiElementType::RadioButton;
    if (type_str.find("Label") != std::string::npos) return GuiElementType::Label;
    if (type_str.find("Table") != std::string::npos) return GuiElementType::Table;
    if (type_str.find("Tree") != std::string::npos) return GuiElementType::Tree;
    if (type_str.find("StatusBar") != std::string::npos) return GuiElementType::StatusBar;
    if (type_str.find("MenuBar") != std::string::npos) return GuiElementType::MenuBar;
    if (type_str.find("Toolbar") != std::string::npos) return GuiElementType::Toolbar;
    if (type_str.find("Tab") != std::string::npos) return GuiElementType::Tab;
    return GuiElementType::Unknown;
}

GuiElementType ComGuiElement::get_classified_type() const {
    if (!element_type_cached_) {
        cached_element_type_ = classify_type(get_type());
        element_type_cached_ = true;
    }
    return cached_element_type_;
}

ComGuiElement::Rect ComGuiElement::get_rect() const {
    Rect rect;
    try {
        rect.x = get_int_property(L"ScreenLeft");
        rect.y = get_int_property(L"ScreenTop");
        rect.width = get_int_property(L"Width");
        rect.height = get_int_property(L"Height");
    } catch (const SapGuiException& e) {
        spdlog::debug("Could not retrieve element rect: SapGuiException: {}", e.what());
    } catch (const ComException& e) {
        spdlog::debug("Could not retrieve element rect: ComException: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::debug("Could not retrieve element rect: std::exception: {}", e.what());
    }
    return rect;
}

void ComGuiElement::press() {
    utils::TraceGuard trace("ComGuiElement::press");
    if (!dispatch_) throw ComException("Null element");

    try {
        _bstr_t method("Press");
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, method.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("get_dispid_via_typeinfo failed: 0x{:08X}", hr));
            throw ComException("Press method not found on element", hr);
        }

        DISPPARAMS params = {nullptr, nullptr, 0, 0};
        _variant_t result;
        hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                            &params, &result, nullptr, nullptr);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("Invoke failed: 0x{:08X}", hr));
            throw ComException("Failed to press element", hr);
        }

        std::string element_id = get_id();
        trace.mark_success();
        spdlog::debug("Element pressed: {}", element_id);
    } catch (const ComException&) {
        throw;
    } catch (const std::exception& e) {
        trace.mark_error(e.what());
        throw ComException(std::string("Exception in press: ") + e.what());
    }
}

void ComGuiElement::select(bool selected) {
    utils::TraceGuard trace("ComGuiElement::select");
    if (!dispatch_) throw ComException("Null element");

    try {
        GuiElementType elem_type = get_classified_type();

        // GuiTab elements use the Select() method (no parameters)
        if (elem_type == GuiElementType::Tab) {
            _bstr_t method("Select");
            DISPID dispid;
            HRESULT hr = get_dispid_via_typeinfo(dispatch_, method.GetBSTR(), &dispid);
            if (FAILED(hr)) {
                trace.mark_error(fmt::format("Select method not found: 0x{:08X}", hr));
                throw ComException("Select method not found on tab element", hr);
            }

            // Call Select() method with no parameters
            DISPPARAMS params = {nullptr, nullptr, 0, 0};
            _variant_t result;
            hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                                &params, &result, nullptr, nullptr);
            if (FAILED(hr)) {
                trace.mark_error(fmt::format("Failed to call Select(): 0x{:08X}", hr));
                throw ComException("Failed to select tab", hr);
            }

            trace.mark_success();
            spdlog::debug("Tab selected: {}", get_id());
            return;
        }

        // CheckBox and RadioButton use the Selected property
        if (elem_type != GuiElementType::CheckBox && elem_type != GuiElementType::RadioButton) {
            throw ComException("Select only works on CheckBox, RadioButton, or Tab elements");
        }

        _bstr_t prop("Selected");
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, prop.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("Selected property not found: 0x{:08X}", hr));
            throw ComException("Selected property not found", hr);
        }

        _variant_t var;
        var.vt = VT_BOOL;
        var.boolVal = selected ? VARIANT_TRUE : VARIANT_FALSE;

        DISPID propput_id = DISPID_PROPERTYPUT;
        DISPPARAMS params = {(VARIANT*)&var, &propput_id, 1, 1};
        hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT,
                            &params, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("Failed to set Selected: 0x{:08X}", hr));
            throw ComException("Failed to set selected state", hr);
        }

        trace.mark_success();
        spdlog::debug("Element selected: {} = {}", get_id(), (selected ? "true" : "false"));
    } catch (const ComException&) {
        throw;
    } catch (const std::exception& e) {
        trace.mark_error(e.what());
        throw ComException(std::string("Exception in select: ") + e.what());
    }
}

std::string ComGuiElement::get_label() const {
    if (!dispatch_) return "";

    try {
        // Try AccLabel property first (most reliable)
        return get_string_property(L"AccLabel");
    } catch (const ComException&) {
        // AccLabel not available, return empty
        return "";
    }
}

std::string ComGuiElement::get_tooltip() const {
    if (!dispatch_) return "";

    // Cascade: AccTooltip → DefaultTooltip → Tooltip
    try {
        std::string tooltip = get_string_property(L"AccTooltip");
        if (!tooltip.empty()) return tooltip;
    } catch (const ComException&) {}

    try {
        std::string tooltip = get_string_property(L"DefaultTooltip");
        if (!tooltip.empty()) return tooltip;
    } catch (const ComException&) {}

    try {
        return get_string_property(L"Tooltip");
    } catch (const ComException&) {
        return "";
    }
}

bool ComGuiElement::is_changeable() const {
    if (!dispatch_) return false;

    try {
        // Check Changeable property (available on most interactive elements)
        return get_bool_property(L"Changeable");
    } catch (const ComException&) {
        // If Changeable not available, assume not changeable
        return false;
    }
}

int ComGuiElement::get_child_count() const {
    if (!dispatch_) return 0;

    try {
        auto children = get_dispatch_property(L"Children");
        if (!children) {
            spdlog::debug("get_child_count: Children property is null for {}", get_id());
            return 0;
        }
        int count = ::fairyfly::sap::get_int_property(children, "Count");
        if (count > 0) {
            spdlog::debug("get_child_count: {} has {} children", get_id(), count);
        }
        return count;
    } catch (const ComException& e) {
        spdlog::debug("get_child_count: Exception for {}: {}", get_id(), e.what());
        return 0;
    }
}

ComGuiElementPtr ComGuiElement::get_child(int index) const {
    if (!dispatch_) return nullptr;

    try {
        auto children = get_dispatch_property(L"Children");
        if (!children) {
            spdlog::debug("get_child({}): Children property is null", index);
            return nullptr;
        }

        _variant_t idx(index);
        _variant_t result;
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(children, L"Item", &dispid);
        if (FAILED(hr)) {
            spdlog::debug("get_child({}): GetIDsOfNames for Item failed: 0x{:08X}", index, hr);
            return nullptr;
        }

        DISPPARAMS params = {(VARIANT*)&idx, nullptr, 1, 0};
        hr = children->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                             &params, &result, nullptr, nullptr);
        if (FAILED(hr)) {
            spdlog::debug("get_child({}): Invoke failed: 0x{:08X}", index, hr);
            return nullptr;
        }
        if (result.vt != VT_DISPATCH) {
            spdlog::debug("get_child({}): Result is not VT_DISPATCH, got vt={}", index, result.vt);
            return nullptr;
        }

        return ComGuiElement::create(result.pdispVal);
    } catch (const ComException& e) {
        spdlog::debug("get_child({}): Exception: {}", index, e.what());
        return nullptr;
    }
}

SapGuiCollection<ComGuiElement> ComGuiElement::children() const {
    if (!dispatch_) {
        return SapGuiCollection<ComGuiElement>(nullptr);
    }

    try {
        auto children_dispatch = get_dispatch_property(L"Children");
        if (!children_dispatch) {
            spdlog::debug("children(): Children property is null");
            return SapGuiCollection<ComGuiElement>(nullptr);
        }

        return SapGuiCollection<ComGuiElement>(children_dispatch);
    } catch (const ComException& e) {
        spdlog::debug("children(): Exception: {}", e.what());
        return SapGuiCollection<ComGuiElement>(nullptr);
    }
}

std::string ComGuiElement::get_container_type() const {
    if (!dispatch_) return "";

    std::string type = get_type();

    // Classify containers by SAP GUI type
    if (type == "GuiToolbar" || type == "GuiMenubar") return "toolbar";
    if (type == "GuiTabStrip") return "tabs";
    if (type == "GuiTableControl") return "table";
    if (type == "GuiGridView") return "grid";
    if (type == "GuiUserArea" || type == "GuiSimpleContainer") return "form";
    if (type == "GuiStatusPane") return "statusbar";
    if (type == "GuiTitlebar") return "titlebar";
    if (type == "GuiBox") return "group";

    // Check if element has children (generic container)
    if (get_child_count() > 0) return "container";

    return "";
}

int ComGuiElement::get_property_int(const std::wstring& property_name) const {
    try {
        return get_int_property(property_name.c_str());
    } catch (const std::exception& e) {
        std::string prop_name_utf8 = (const char*)_bstr_t(property_name.c_str());
        spdlog::debug("get_property_int failed for {}: {}", prop_name_utf8, e.what());
        return 0;
    }
}

bool ComGuiElement::get_property_bool(const std::wstring& property_name) const {
    try {
        return get_bool_property(property_name.c_str());
    } catch (const std::exception& e) {
        std::string prop_name_utf8 = (const char*)_bstr_t(property_name.c_str());
        spdlog::debug("get_property_bool failed for {}: {}", prop_name_utf8, e.what());
        return false;
    }
}

std::string ComGuiElement::get_property_string(const std::wstring& property_name) const {
    try {
        return get_string_property(property_name.c_str());
    } catch (const std::exception& e) {
        std::string prop_name_utf8 = (const char*)_bstr_t(property_name.c_str());
        spdlog::debug("get_property_string failed for {}: {}", prop_name_utf8, e.what());
        return "";
    }
}

std::string ComGuiElement::get_subtype() const {
    try {
        // SubType property is available on GuiShell elements
        return get_string_property(L"SubType");
    } catch (const std::exception& e) {
        spdlog::debug("get_subtype failed: {}", e.what());
        return "";
    }
}

std::vector<std::string> ComGuiElement::get_all_node_keys() const {
    std::vector<std::string> keys;

    try {
        // Call GetAllNodeKeys() method on the tree control
        // This returns a collection object
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"GetAllNodeKeys", &dispid);
        if (FAILED(hr)) {
            spdlog::debug("Could not get DISPID for GetAllNodeKeys");
            return keys;
        }

        DISPPARAMS params = {0};
        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = dispatch_->Invoke(
            dispid,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            DISPATCH_METHOD,
            &params,
            result_guard.get(),
            nullptr,
            nullptr
        );

        if (SUCCEEDED(hr) && result_var.vt == VT_DISPATCH && result_var.pdispVal) {
            // Result is a collection - enumerate it
            IDispatch* coll = result_var.pdispVal;

            // Get Count property
            DISPID count_dispid;
            hr = get_dispid_via_typeinfo(coll, L"Count", &count_dispid);
            if (FAILED(hr)) {
                return keys;
            }

            DISPPARAMS count_params = {0};
            VARIANT count_result_var;
            com::VariantGuard count_result_guard(&count_result_var);

            hr = coll->Invoke(
                count_dispid,
                IID_NULL,
                LOCALE_USER_DEFAULT,
                DISPATCH_PROPERTYGET,
                &count_params,
                count_result_guard.get(),
                nullptr,
                nullptr
            );

            if (SUCCEEDED(hr) && count_result_var.vt == VT_I4) {
                int count = count_result_var.lVal;

                // Get Item method dispid
                DISPID item_dispid;
                hr = get_dispid_via_typeinfo(coll, L"Item", &item_dispid);
                if (FAILED(hr)) {
                    return keys;
                }

                // Enumerate items (0-based indexing)
                for (int i = 0; i < count; ++i) {
                    VARIANT index_var;
                    com::VariantGuard index_guard(&index_var);
                    index_var.vt = VT_I4;
                    index_var.lVal = i;

                    DISPPARAMS item_params;
                    item_params.cArgs = 1;
                    item_params.rgvarg = &index_var;
                    item_params.cNamedArgs = 0;
                    item_params.rgdispidNamedArgs = nullptr;

                    VARIANT item_result_var;
                    com::VariantGuard item_result_guard(&item_result_var);

                    hr = coll->Invoke(
                        item_dispid,
                        IID_NULL,
                        LOCALE_USER_DEFAULT,
                        DISPATCH_METHOD,
                        &item_params,
                        item_result_guard.get(),
                        nullptr,
                        nullptr
                    );

                    if (SUCCEEDED(hr)) {
                        // Convert to string
                        if (item_result_var.vt == VT_BSTR) {
                            keys.push_back((const char*)_bstr_t(item_result_var.bstrVal));
                        } else if (item_result_var.vt == VT_I4) {
                            keys.push_back(std::to_string(item_result_var.lVal));
                        }
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        spdlog::debug("get_all_node_keys failed: {}", e.what());
    }

    return keys;
}

std::string ComGuiElement::get_node_text_by_key(const std::string& key) const {
    try {
        // Call GetNodeTextByKey(key) method
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"GetNodeTextByKey", &dispid);
        if (FAILED(hr)) {
            spdlog::debug("Could not get DISPID for GetNodeTextByKey");
            return "";
        }

        // Convert key to BSTR
        _bstr_t key_bstr(key.c_str());

        VARIANT arg_var;
        com::VariantGuard arg_guard(&arg_var);
        arg_var.vt = VT_BSTR;
        arg_var.bstrVal = key_bstr;

        DISPPARAMS params;
        params.cArgs = 1;
        params.rgvarg = &arg_var;
        params.cNamedArgs = 0;
        params.rgdispidNamedArgs = nullptr;

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = dispatch_->Invoke(
            dispid,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            DISPATCH_METHOD,
            &params,
            result_guard.get(),
            nullptr,
            nullptr
        );

        std::string text;
        if (SUCCEEDED(hr) && result_var.vt == VT_BSTR) {
            text = (const char*)_bstr_t(result_var.bstrVal);
        }

        return text;

    } catch (const std::exception& e) {
        spdlog::debug("get_node_text_by_key failed for key '{}': {}", key, e.what());
        return "";
    }
}

std::string ComGuiElement::get_node_path_by_key(const std::string& key) const {
    try {
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"GetNodePathByKey", &dispid);
        if (FAILED(hr)) {
            return "";
        }

        _bstr_t key_bstr(key.c_str());
        VARIANT arg_var;
        com::VariantGuard arg_guard(&arg_var);
        arg_var.vt = VT_BSTR;
        arg_var.bstrVal = key_bstr;

        DISPPARAMS params;
        params.cArgs = 1;
        params.rgvarg = &arg_var;
        params.cNamedArgs = 0;
        params.rgdispidNamedArgs = nullptr;

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                              &params, result_guard.get(), nullptr, nullptr);

        std::string path;
        if (SUCCEEDED(hr) && result_var.vt == VT_BSTR) {
            path = (const char*)_bstr_t(result_var.bstrVal);
        }

        return path;

    } catch (const std::exception& e) {
        spdlog::debug("get_node_path_by_key failed for key '{}': {}", key, e.what());
        return "";
    }
}

std::vector<std::string> ComGuiElement::get_column_order() const {
    std::vector<std::string> columns;

    try {
        // Get ColumnOrder property (returns a collection)
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"ColumnOrder", &dispid);
        if (FAILED(hr)) {
            return columns;
        }

        DISPPARAMS params = {0};
        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
                              &params, result_guard.get(), nullptr, nullptr);

        if (SUCCEEDED(hr) && result_var.vt == VT_DISPATCH && result_var.pdispVal) {
            IDispatch* coll = result_var.pdispVal;

            // Get Count property
            DISPID count_dispid;
            hr = get_dispid_via_typeinfo(coll, L"Count", &count_dispid);
            if (SUCCEEDED(hr)) {
                DISPPARAMS count_params = {0};
                VARIANT count_result_var;
                com::VariantGuard count_result_guard(&count_result_var);

                hr = coll->Invoke(count_dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                DISPATCH_PROPERTYGET, &count_params, count_result_guard.get(), nullptr, nullptr);

                if (SUCCEEDED(hr) && count_result_var.vt == VT_I4) {
                    int count = count_result_var.lVal;

                    // Get Item method dispid
                    DISPID item_dispid;
                    hr = get_dispid_via_typeinfo(coll, L"Item", &item_dispid);
                    if (SUCCEEDED(hr)) {
                        for (int i = 0; i < count; ++i) {
                            VARIANT index_var;
                            com::VariantGuard index_guard(&index_var);
                            index_var.vt = VT_I4;
                            index_var.lVal = i;

                            DISPPARAMS item_params;
                            item_params.cArgs = 1;
                            item_params.rgvarg = &index_var;
                            item_params.cNamedArgs = 0;
                            item_params.rgdispidNamedArgs = nullptr;

                            VARIANT item_result_var;
                            com::VariantGuard item_result_guard(&item_result_var);

                            hr = coll->Invoke(item_dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                            DISPATCH_METHOD, &item_params, item_result_guard.get(), nullptr, nullptr);

                            if (SUCCEEDED(hr) && item_result_var.vt == VT_BSTR) {
                                columns.push_back((const char*)_bstr_t(item_result_var.bstrVal));
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

std::string ComGuiElement::get_cell_value(int row, const std::string& column_name) const {
    try {
        // Call GetCellValue(row As Long, column As String) As String
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"GetCellValue", &dispid);
        if (FAILED(hr)) {
            return "";
        }

        // Prepare parameters: row (int) and column_name (string)
        // Parameters are passed in REVERSE order in DISPPARAMS
        VARIANT params[2];
        com::VariantGuard param_guards[2] = {
            com::VariantGuard(&params[0]),  // column_name (second parameter, first in array)
            com::VariantGuard(&params[1])  // row (first parameter, second in array)
        };

        params[0].vt = VT_BSTR;
        params[0].bstrVal = SysAllocString(std::wstring(column_name.begin(), column_name.end()).c_str());

        params[1].vt = VT_I4;
        params[1].lVal = row;

        DISPPARAMS disp_params;
        disp_params.cArgs = 2;
        disp_params.rgvarg = params;
        disp_params.cNamedArgs = 0;
        disp_params.rgdispidNamedArgs = nullptr;

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                              DISPATCH_METHOD, &disp_params, result_guard.get(), nullptr, nullptr);

        std::string cell_value;
        if (SUCCEEDED(hr) && result_var.vt == VT_BSTR) {
            cell_value = (const char*)_bstr_t(result_var.bstrVal);
        }

        // Cleanup happens automatically via RAII guards
        return cell_value;

    } catch (const std::exception& e) {
        spdlog::debug("get_cell_value failed: {}", e.what());
        return "";
    }
}

std::string ComGuiElement::get_item_text(const std::string& node_key, const std::string& column_name) const {
    try {
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"GetItemText", &dispid);
        if (FAILED(hr)) {
            return "";
        }

        // Two arguments: node_key and column_name (in reverse order for DISPPARAMS)
        _bstr_t key_bstr(node_key.c_str());
        _bstr_t col_bstr(column_name.c_str());

        VARIANT args[2];
        com::VariantGuard arg_guards[2] = {
            com::VariantGuard(&args[0]),
            com::VariantGuard(&args[1])
        };

        args[1].vt = VT_BSTR;  // First argument (node_key)
        args[1].bstrVal = key_bstr;

        args[0].vt = VT_BSTR;  // Second argument (column_name)
        args[0].bstrVal = col_bstr;

        DISPPARAMS params;
        params.cArgs = 2;
        params.rgvarg = args;
        params.cNamedArgs = 0;
        params.rgdispidNamedArgs = nullptr;

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                              &params, result_guard.get(), nullptr, nullptr);

        std::string text;
        if (SUCCEEDED(hr) && result_var.vt == VT_BSTR) {
            text = (const char*)_bstr_t(result_var.bstrVal);
        }

        return text;

    } catch (const std::exception& e) {
        spdlog::debug("get_item_text failed for key '{}', column '{}': {}", node_key, column_name, e.what());
        return "";
    }
}

} // namespace sap
} // namespace fairyfly

