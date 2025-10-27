#include "include/com_wrapper.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

namespace fairyfly {
namespace sap {

// ============================================================================
// Helper Functions for COM Property/Method Access
// ============================================================================

/// Get a string property from COM object
inline std::string get_string_property(IDispatch* obj, const char* prop_name) {
    if (!obj) throw ComException("Null object");

    try {
        _bstr_t prop(prop_name);
        DISPID dispid;
        // Use get_dispid_via_typeinfo for SAP GUI compatibility
        HRESULT hr = get_dispid_via_typeinfo(obj, prop.GetBSTR(), &dispid);
        if (FAILED(hr)) return "";

        _variant_t result;
        DISPPARAMS params = {nullptr, nullptr, 0, 0};
        hr = obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
                        &params, &result, nullptr, nullptr);
        if (FAILED(hr)) return "";

        return (const char*)_bstr_t(result);
    } catch (...) {
        return "";
    }
}

/// Get an integer property from COM object
inline int get_int_property(IDispatch* obj, const char* prop_name) {
    if (!obj) throw ComException("Null object");

    try {
        _bstr_t prop(prop_name);
        DISPID dispid;
        // CRITICAL FIX: Use get_dispid_via_typeinfo for SAP GUI compatibility
        HRESULT hr = get_dispid_via_typeinfo(obj, prop.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            spdlog::debug("get_int_property|get_dispid_via_typeinfo failed|prop={}|hr=0x{:08X}", prop_name, hr);
            return 0;
        }

        _variant_t result;
        DISPPARAMS params = {nullptr, nullptr, 0, 0};
        hr = obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
                        &params, &result, nullptr, nullptr);
        if (FAILED(hr)) {
            spdlog::debug("get_int_property|Invoke failed|prop={}|hr=0x{:08X}", prop_name, hr);
            return 0;
        }

        int value = (int)result;
        spdlog::debug("get_int_property|success|prop={}|value={}", prop_name, value);
        return value;
    } catch (const std::exception& e) {
        spdlog::warn("get_int_property|exception|prop={}|error={}", prop_name, e.what());
        return 0;
    } catch (...) {
        spdlog::warn("get_int_property|unknown exception|prop={}", prop_name);
        return 0;
    }
}

/// Get a boolean property from COM object
inline bool get_bool_property(IDispatch* obj, const char* prop_name) {
    if (!obj) throw ComException("Null object");

    try {
        _bstr_t prop(prop_name);
        DISPID dispid;
        // Use get_dispid_via_typeinfo for SAP GUI compatibility
        HRESULT hr = get_dispid_via_typeinfo(obj, prop.GetBSTR(), &dispid);
        if (FAILED(hr)) return false;

        _variant_t result;
        DISPPARAMS params = {nullptr, nullptr, 0, 0};
        hr = obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
                        &params, &result, nullptr, nullptr);
        if (FAILED(hr)) return false;

        return result.boolVal != VARIANT_FALSE;
    } catch (...) {
        return false;
    }
}

/// Get DISPID for a method/property name using ITypeInfo (works better than IDispatch::GetIDsOfNames)
/// This is the CRITICAL FIX for SAP GUI scripting - IDispatch::GetIDsOfNames fails but ITypeInfo works
HRESULT get_dispid_via_typeinfo(IDispatch* obj, const wchar_t* name, DISPID* dispid) {
    if (!obj || !name || !dispid) return E_POINTER;

    // Try ITypeInfo first (this works for SAP GUI)
    ITypeInfo* type_info = nullptr;
    HRESULT hr = obj->GetTypeInfo(0, LOCALE_USER_DEFAULT, &type_info);
    if (SUCCEEDED(hr) && type_info) {
        LPOLESTR names[1] = { const_cast<LPOLESTR>(name) };
        hr = type_info->GetIDsOfNames(names, 1, dispid);
        type_info->Release();
        if (SUCCEEDED(hr)) return S_OK;
    }

    // Fallback to IDispatch::GetIDsOfNames
    LPOLESTR name_ptr = const_cast<LPOLESTR>(name);
    return obj->GetIDsOfNames(IID_NULL, &name_ptr, 1, LOCALE_USER_DEFAULT, dispid);
}

/// Get a dispatch property from COM object
inline IDispatchPtr get_dispatch_property(IDispatch* obj, const char* prop_name) {
    if (!obj) throw ComException("Null object");

    try {
        _bstr_t prop(prop_name);
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(obj, prop.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            spdlog::debug("get_dispatch_property|get_dispid_via_typeinfo failed|prop={}|hr=0x{:08X}", prop_name, hr);
            return nullptr;
        }

        _variant_t result;
        DISPPARAMS params = {nullptr, nullptr, 0, 0};
        hr = obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
                        &params, &result, nullptr, nullptr);
        if (FAILED(hr)) {
            spdlog::debug("get_dispatch_property|Invoke failed|prop={}|hr=0x{:08X}", prop_name, hr);
            return nullptr;
        }
        if (result.vt != VT_DISPATCH) {
            spdlog::debug("get_dispatch_property|wrong variant type|prop={}|vt={}|expected=VT_DISPATCH({})",
                         prop_name, (int)result.vt, (int)VT_DISPATCH);
            return nullptr;
        }

        spdlog::debug("get_dispatch_property|success|prop={}", prop_name);
        return IDispatchPtr(result.pdispVal);
    } catch (const std::exception& e) {
        spdlog::warn("get_dispatch_property|exception|prop={}|error={}", prop_name, e.what());
        return nullptr;
    } catch (...) {
        spdlog::warn("get_dispatch_property|unknown exception|prop={}", prop_name);
        return nullptr;
    }
}

/// Try multiple property names (first non-null wins)
/// Set a string property on COM object
inline void set_string_property(IDispatch* obj, const char* prop_name, const std::string& value) {
    if (!obj) throw ComException("Null object");

    try {
        _bstr_t prop(prop_name);
        DISPID dispid;
        // Use get_dispid_via_typeinfo for SAP GUI compatibility
        HRESULT hr = get_dispid_via_typeinfo(obj, prop.GetBSTR(), &dispid);
        if (FAILED(hr)) throw ComException(std::string("Property not found: ") + prop_name, hr);

        _variant_t var(value.c_str());
        DISPID propput_id = DISPID_PROPERTYPUT;
        DISPPARAMS params = {(VARIANT*)&var, &propput_id, 1, 1};
        hr = obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT,
                        &params, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            throw ComException(std::string("Failed to set property: ") + prop_name, hr);
        }
    } catch (const ComException&) {
        throw;
    } catch (...) {
        throw ComException("COM error setting property");
    }
}

/// Call a method on COM object with string parameter
inline IDispatchPtr call_method_with_string(IDispatch* obj, const char* method_name,
                                             const std::string& param) {
    if (!obj) throw ComException("Null object");

    try {
        _bstr_t method(method_name);
        DISPID dispid;
        // Use get_dispid_via_typeinfo for SAP GUI compatibility
        HRESULT hr = get_dispid_via_typeinfo(obj, method.GetBSTR(), &dispid);
        if (FAILED(hr)) return nullptr;

        _variant_t param_var(param.c_str());
        DISPPARAMS disp_params = {(VARIANT*)&param_var, nullptr, 1, 0};
        _variant_t result;
        hr = obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                        &disp_params, &result, nullptr, nullptr);
        if (FAILED(hr) || result.vt != VT_DISPATCH) return nullptr;

        return IDispatchPtr(result.pdispVal);
    } catch (...) {
        return nullptr;
    }
}

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
    return get_string_property(L"Text");
}

void ComGuiElement::set_text(const std::string& text) {
    set_string_property(L"Text", text);
    spdlog::debug("Set element text: {}", text);
}

bool ComGuiElement::is_enabled() const {
    return get_bool_property(L"Enabled");
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
    } catch (...) {
        spdlog::debug("Could not retrieve element rect");
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
    } catch (...) {
        return 0;
    }
}

bool ComGuiElement::get_property_bool(const std::wstring& property_name) const {
    try {
        return get_bool_property(property_name.c_str());
    } catch (...) {
        return false;
    }
}

// ============================================================================
// ComGuiWindow Implementation
// ============================================================================

ComGuiWindow::ComGuiWindow(IDispatchPtr wnd) : SapGuiObject(wnd) {
    if (!wnd) throw ComException("Invalid window pointer");
}

ComGuiWindowPtr ComGuiWindow::create(IDispatchPtr wnd) {
    return std::make_shared<ComGuiWindow>(wnd);
}

// get_id() now inherited from SapGuiObject

std::string ComGuiWindow::get_title() const {
    // Try Text property first (window titlebar text in SAP GUI)
    try {
        std::string text = get_string_property(L"Text");
        if (!text.empty()) {
            return text;
        }
    } catch (...) {
        // Text property not available, try Title
    }

    // Fall back to Title property
    try {
        return get_string_property(L"Title");
    } catch (...) {
        return "";
    }
}

int ComGuiWindow::get_child_count() const {
    auto children = get_dispatch_property(L"Children");
    if (!children) return 0;
    return ::fairyfly::sap::get_int_property(children, "Count");
}

ComGuiElementPtr ComGuiWindow::get_child(int index) const {
    auto children_col = children();
    return children_col.item(index);
}

SapGuiCollection<ComGuiElement> ComGuiWindow::children() const {
    auto children_dispatch = get_dispatch_property(L"Children");
    if (!children_dispatch) {
        // Return empty collection
        return SapGuiCollection<ComGuiElement>(nullptr);
    }
    return SapGuiCollection<ComGuiElement>(children_dispatch);
}

// ============================================================================
// ComGuiSession Implementation
// ============================================================================

ComGuiSession::ComGuiSession(IDispatchPtr sess) : SapGuiObject(sess) {
    if (!sess) throw ComException("Invalid session pointer");
}

ComGuiSessionPtr ComGuiSession::create(IDispatchPtr sess) {
    return std::make_shared<ComGuiSession>(sess);
}

// get_id(), get_name(), get_type(), get_type_as_number() now inherited from SapGuiObject

bool ComGuiSession::is_busy() const {
    return get_bool_property(L"Busy");
}

bool ComGuiSession::is_alive() const {
    try {
        std::string id = get_id();
        return !id.empty();
    } catch (...) {
        return false;
    }
}

ComGuiWindowPtr ComGuiSession::get_active_window() const {
    auto window = get_dispatch_property(L"ActiveWindow");
    if (!window) return nullptr;
    return ComGuiWindow::create(window);
}

ComGuiElementPtr ComGuiSession::find_element_by_id(const std::string& id) const {
    auto elem = ::fairyfly::sap::call_method_with_string(dispatch_, "FindById", id);
    if (!elem) {
        throw ComException("Element not found: " + id);
    }
    return ComGuiElement::create(elem);
}

void ComGuiSession::start_transaction(const std::string& tcode) {
    try {
        auto window = get_active_window();
        if (!window) throw ComException("No active window");

        auto tcode_elem = find_element_by_id("wnd[0]/tbar[0]/okcd");
        if (tcode_elem) {
            tcode_elem->set_text(tcode);
            spdlog::info("Started transaction: {}", tcode);
        }
    } catch (const ComException& e) {
        spdlog::error("Failed to start transaction '{}': {}", tcode, e.what());
        throw;
    }
}

void ComGuiSession::wait_for_completion(int timeout_ms) {
    auto start = std::chrono::high_resolution_clock::now();
    while (is_busy()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start
        );
        if (elapsed.count() > timeout_ms) {
            throw ComException("Session timeout waiting for completion");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void ComGuiSession::send_vkey(int vkey) {
    utils::TraceGuard trace("ComGuiSession::send_vkey");
    if (!dispatch_) throw ComException("Null session");

    try {
        _bstr_t method("SendVKey");
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, method.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("SendVKey method not found: 0x{:08X}", hr));
            throw ComException("SendVKey method not found", hr);
        }

        _variant_t vkey_var(vkey);
        DISPPARAMS params = {(VARIANT*)&vkey_var, nullptr, 1, 0};
        _variant_t result;
        hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                             &params, &result, nullptr, nullptr);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("SendVKey invoke failed: 0x{:08X}", hr));
            throw ComException("Failed to send virtual key", hr);
        }

        trace.mark_success();
        spdlog::debug("Sent virtual key: {}", vkey);

        // Brief wait for server to process key
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } catch (const ComException&) {
        throw;
    } catch (const std::exception& e) {
        trace.mark_error(e.what());
        throw ComException(std::string("Exception in send_vkey: ") + e.what());
    }
}

ComGuiElementPtr ComGuiSession::wait_for_element(const std::string& element_id, int timeout_ms) {
    utils::TraceGuard trace("ComGuiSession::wait_for_element");
    if (!dispatch_) throw ComException("Null session");

    auto start = std::chrono::high_resolution_clock::now();
    int attempts = 0;

    while (true) {
        try {
            auto elem = find_element_by_id(element_id);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start
            );
            spdlog::debug("wait_for_element|found|element={}|elapsed_ms={}|attempts={}",
                         element_id, elapsed.count(), attempts);
            trace.mark_success();
            return elem;
        } catch (const ComException&) {
            attempts++;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start
            );

            if (elapsed.count() > timeout_ms) {
                spdlog::warn("wait_for_element|timeout|element={}|elapsed_ms={}|attempts={}",
                           element_id, elapsed.count(), attempts);
                trace.mark_error("timeout");
                return nullptr;
            }

            spdlog::debug("wait_for_element|retry|element={}|attempt={}|elapsed_ms={}",
                         element_id, attempts, elapsed.count());

            // Progressive backoff: 100ms, 200ms, 300ms... up to 500ms
            int delay = (100 * attempts < 500) ? (100 * attempts) : 500;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
}

std::string ComGuiSession::get_transaction_code() const {
    try {
        // Get the Info object from the session
        auto info = get_dispatch_property(L"Info");
        if (!info) {
            return "";
        }

        // Get the Transaction property from the Info object
        // We need to access a string property on the info IDispatch object
        _bstr_t prop_name("Transaction");
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(info, prop_name.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            return "";  // Transaction property not found
        }

        DISPPARAMS params = {nullptr, nullptr, 0, 0};
        _variant_t result;
        hr = info->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
                         &params, &result, nullptr, nullptr);
        if (FAILED(hr)) {
            return "";  // Failed to get Transaction property
        }

        if (result.vt == VT_BSTR) {
            return std::string(_bstr_t(result.bstrVal));
        }
        return "";
    } catch (const ComException&) {
        // Transaction property not available or error accessing it
        return "";
    } catch (const std::exception&) {
        return "";
    }
}

// ============================================================================
// ComGuiConnection Implementation
// ============================================================================

ComGuiConnection::ComGuiConnection(IDispatchPtr conn) : SapGuiObject(conn) {
    if (!conn) throw ComException("Invalid connection pointer");
}

ComGuiConnectionPtr ComGuiConnection::create(IDispatchPtr conn) {
    return std::make_shared<ComGuiConnection>(conn);
}

// get_id(), get_type(), get_type_as_number() now inherited from SapGuiObject

std::string ComGuiConnection::get_description() const {
    return get_string_property(L"Description");
}

std::string ComGuiConnection::get_connection_string() const {
    return get_string_property(L"ConnectionString");
}

int ComGuiConnection::get_session_count() const {
    auto sessions = get_dispatch_property(L"Children");
    if (!sessions) {
        spdlog::debug("GuiConnection.Children not available, falling back to Sessions");
        sessions = get_dispatch_property(L"Sessions");
    }

    if (!sessions) {
        spdlog::warn("GuiConnection has no Children/Sessions collection - no active sessions visible");
        return 0;
    }

    int count = ::fairyfly::sap::get_int_property(sessions, "Count");
    spdlog::debug("GuiConnection session collection count: {}", count);
    return count;
}

ComGuiSessionPtr ComGuiConnection::get_session(int index) const {
    auto sessions_col = sessions();
    return sessions_col.item(index);
}

SapGuiCollection<ComGuiSession> ComGuiConnection::sessions() const {
    auto sessions_dispatch = get_dispatch_property(L"Children");
    if (!sessions_dispatch) {
        spdlog::debug("GuiConnection.Children not available, trying Sessions");
        sessions_dispatch = get_dispatch_property(L"Sessions");
    }

    if (!sessions_dispatch) {
        spdlog::warn("GuiConnection has no Children/Sessions collection");
        return SapGuiCollection<ComGuiSession>(nullptr);
    }

    return SapGuiCollection<ComGuiSession>(sessions_dispatch);
}

// ============================================================================
// ComGuiApplication Implementation
// ============================================================================

ComGuiApplication::ComGuiApplication(IDispatchPtr sap_gui_app) : SapGuiObject(sap_gui_app) {
    if (!sap_gui_app) {
        throw ComException("Invalid SAP GUI application pointer");
    }
    spdlog::info("ComGuiApplication initialized");
}

ComGuiApplicationPtr ComGuiApplication::create() {
    // Initialize COM library for STA (Single-Threaded Apartment) mode
    // This is required for SAP GUI scripting API
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != S_FALSE) {  // S_FALSE means already initialized
        throw ComException("Failed to initialize COM library", hr);
    }

    spdlog::debug("COM library initialized");

    try {
        IDispatch* sap_gui = nullptr;

        // Method 1: Try to connect to running SAP GUI instance using GetObject (Preferred)
        // According to SAP GUI Scripting API documentation:
        // "Set rotEntry = GetObject("SAPGUI")"
        // "Set application = rotEntry.GetScriptingEngine"
        // We must call GetScriptingEngine property on the ROT entry!
        spdlog::debug("Attempting to get SAPGUI ROT entry...");
        hr = CoGetObject(L"SAPGUI", nullptr, IID_IDispatch, (void**)&sap_gui);

        if (SUCCEEDED(hr) && sap_gui) {
            spdlog::info("CoGetObject('SAPGUI') succeeded - SAP Logon is running");

            // CoGetObject("SAPGUI") returns SapGuiAuto (ROT entry). It exposes GetScriptingEngine.
            // Some builds expose it as a PROPERTYGET, others as a METHOD. Try both, PROPERTYGET first.
            _bstr_t ge_name(L"GetScriptingEngine");
            DISPID ge_id{};
            HRESULT ge_hr = get_dispid_via_typeinfo(sap_gui, ge_name, &ge_id);

            if (SUCCEEDED(ge_hr)) {
                DISPPARAMS noargs{nullptr, nullptr, 0, 0};
                _variant_t vr;

                // Try PROPERTYGET
                HRESULT hr_prop = sap_gui->Invoke(ge_id, IID_NULL, LOCALE_USER_DEFAULT,
                                                  DISPATCH_PROPERTYGET, &noargs, &vr, nullptr, nullptr);
                if (SUCCEEDED(hr_prop) && vr.vt == VT_DISPATCH && vr.pdispVal) {
                    spdlog::info("Got GuiApplication via ROT GetScriptingEngine (PROPERTYGET)");
                    sap_gui->Release();
                    return std::make_shared<ComGuiApplication>(IDispatchPtr(vr.pdispVal));
                }

                // Try METHOD
                VariantClear(&vr);
                HRESULT hr_meth = sap_gui->Invoke(ge_id, IID_NULL, LOCALE_USER_DEFAULT,
                                                  DISPATCH_METHOD, &noargs, &vr, nullptr, nullptr);
                if (SUCCEEDED(hr_meth) && vr.vt == VT_DISPATCH && vr.pdispVal) {
                    spdlog::info("Got GuiApplication via ROT GetScriptingEngine (METHOD)");
                    sap_gui->Release();
                    return std::make_shared<ComGuiApplication>(IDispatchPtr(vr.pdispVal));
                }

                spdlog::debug("GetScriptingEngine failed: prop_hr=0x{:08X}, meth_hr=0x{:08X}", hr_prop, hr_meth);
            } else {
                spdlog::debug("GetScriptingEngine name resolution failed (hr=0x{:08X})", ge_hr);
            }

            sap_gui->Release();
            sap_gui = nullptr;
        }

        spdlog::debug("Method 1 (GetObject + GetScriptingEngine) failed, trying CreateObject as fallback");

        // Method 2: Try CreateObject as fallback
        // This creates a new SAP GUI instance rather than connecting to running one
        // Only use this if Method 1 fails
        CLSID clsid;
        hr = CLSIDFromProgID(L"SapGui.ScriptingCtrl.1", &clsid);

        if (SUCCEEDED(hr)) {
            spdlog::debug("CLSID found for SapGui.ScriptingCtrl.1, attempting CoCreateInstance...");
            // Try CLSCTX_ALL to allow in-process, local, and remote servers
            hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IDispatch,
                                 (void**)&sap_gui);

            if (SUCCEEDED(hr) && sap_gui) {
                spdlog::info("Created SAP GUI COM object via CoCreateInstance");

                // CRITICAL FIX: SAP GUI uses interface hierarchies. We need to do the IUnknown roundtrip
                // to get the most derived IDispatch interface that exposes all methods.
                // See: https://stackoverflow.com/questions/25965753/idispatch-returns-disp-e-unknownname
                IUnknown* unknown = nullptr;
                HRESULT qi_hr = sap_gui->QueryInterface(IID_IUnknown, (void**)&unknown);
                spdlog::debug("QueryInterface for IID_IUnknown: hr=0x{:08X}", qi_hr);

                if (SUCCEEDED(qi_hr) && unknown) {
                    IDispatch* dispatch_derived = nullptr;
                    qi_hr = unknown->QueryInterface(IID_IDispatch, (void**)&dispatch_derived);
                    spdlog::debug("QueryInterface IUnknown->IDispatch (most derived): hr=0x{:08X}", qi_hr);

                    if (SUCCEEDED(qi_hr) && dispatch_derived) {
                        spdlog::info("Got most derived IDispatch interface via IUnknown roundtrip");
                        sap_gui->Release();
                        sap_gui = dispatch_derived;
                    }
                    unknown->Release();
                }

                // Try alternative approach: Use ITypeInfo instead of GetIDsOfNames directly
                // According to research, GetIDsOfNames may not work but ITypeInfo::GetIDsOfNames can
                ITypeInfo* type_info = nullptr;
                HRESULT ti_hr = sap_gui->GetTypeInfo(0, LOCALE_USER_DEFAULT, &type_info);
                spdlog::debug("GetTypeInfo: hr=0x{:08X}", ti_hr);

                DISPID test_dispid = -1;
                HRESULT test_hr = E_FAIL;

                if (SUCCEEDED(ti_hr) && type_info) {
                    // Try using ITypeInfo::GetIDsOfNames instead of IDispatch::GetIDsOfNames
                    LPOLESTR method_names[1] = { const_cast<LPOLESTR>(L"OpenConnection") };
                    test_hr = type_info->GetIDsOfNames(method_names, 1, &test_dispid);
                    spdlog::debug("ITypeInfo::GetIDsOfNames for 'OpenConnection': hr=0x{:08X}, dispid={}", test_hr, test_dispid);
                    type_info->Release();

                    if (SUCCEEDED(test_hr)) {
                        spdlog::info("SUCCESS: OpenConnection found via ITypeInfo! GuiApplication is ready");
                        return std::make_shared<ComGuiApplication>(IDispatchPtr(sap_gui));
                    }
                }

                // Fallback: try IDispatch::GetIDsOfNames anyway
                if (FAILED(test_hr)) {
                    _bstr_t test_method("OpenConnection");
                    test_hr = sap_gui->GetIDsOfNames(IID_NULL, (LPOLESTR*)&test_method, 1, LOCALE_USER_DEFAULT, &test_dispid);
                    spdlog::debug("IDispatch::GetIDsOfNames for 'OpenConnection': hr=0x{:08X}, dispid={}", test_hr, test_dispid);

                    if (SUCCEEDED(test_hr)) {
                        spdlog::info("SUCCESS: OpenConnection found! GuiApplication is ready");
                        return std::make_shared<ComGuiApplication>(IDispatchPtr(sap_gui));
                    }
                }

                spdlog::error("FAILED: OpenConnection not found via ITypeInfo OR IDispatch");
                spdlog::error("This indicates sapfewse.ocx may not be properly registered");
                spdlog::error("Try: regsvr32 \"C:\\Program Files (x86)\\SAP\\FrontEnd\\SAPgui\\sapfewse.ocx\"");
                spdlog::error("Or check: SAP GUI Options → Accessibility & Scripting → Enable scripting");
            }

            spdlog::debug("CoCreateInstance failed (hr=0x{:08X})", hr);
        }

        // All methods failed
        std::string error_msg = fmt::format(
            "SAP GUI Scripting API not available (final hr=0x{:08X}). "
            "Possible causes: (1) SAP GUI not running, (2) SAP GUI scripting not enabled in SAP GUI options, "
            "(3) COM registration issue. Ensure SAP GUI is running and has an active transaction window open.",
            hr
        );
        throw ComException(error_msg, hr);

    } catch (const ComException&) {
        CoUninitialize();
        throw;
    } catch (const std::exception& e) {
        CoUninitialize();
        throw ComException(std::string("Failed to create ComGuiApplication: ") + e.what());
    }
}

int ComGuiApplication::get_connection_count() const {
    spdlog::info("Enumerating GuiApplication.Connections for connections");

    // Try Connections first (matches VBScript behavior app.Connections)
    IDispatchPtr connections = get_dispatch_property(L"Connections");

    if (!connections) {
        spdlog::warn("GuiApplication.Connections returned null, trying Children");

        // Fallback to Children
        IDispatchPtr children = get_dispatch_property(L"Children");
        if (!children) {
            spdlog::error("Both Connections and Children returned null - SAP GUI scripting may be disabled");
            return 0;
        }

        int count = ::fairyfly::sap::get_int_property(children, "Count");
        spdlog::info("Using Children collection: {} items", count);
        return count;
    }
    
    // Get count from Connections
    int count = ::fairyfly::sap::get_int_property(connections, "Count");
    spdlog::info("GuiApplication.Connections reports {} connection(s)", count);

    // Log detailed diagnostics
    if (count > 0) {
        spdlog::debug("Connections collection has {} items", count);
    } else {
        spdlog::warn("Connections collection exists but reports 0 items - no SAP connections currently open");
        spdlog::warn("To see connections: Open SAP Logon, create a connection, or open a transaction");
    }

    return count;
}

ComGuiConnectionPtr ComGuiApplication::get_connection(int index) const {
    auto connections_col = connections();
    return connections_col.item(index);
}

SapGuiCollection<ComGuiConnection> ComGuiApplication::connections() const {
    auto connections_dispatch = get_dispatch_property(L"Connections");
    if (!connections_dispatch) {
        spdlog::info("GuiApplication.Connections not available, trying Children");
        connections_dispatch = get_dispatch_property(L"Children");
    }

    if (!connections_dispatch) {
        spdlog::warn("GuiApplication has no Connections/Children collection");
        return SapGuiCollection<ComGuiConnection>(nullptr);
    }

    return SapGuiCollection<ComGuiConnection>(connections_dispatch);
}

// Global state for mouse hook
static HWND g_clicked_hwnd = nullptr;
static HANDLE g_click_event = nullptr;
static HWND g_last_hwnd = nullptr;
static HCURSOR g_crosshair_cursor = nullptr;

// Debug logging function for hook callback (console won't work in hook context)
static void hook_debug(const char* msg) {
    static FILE* debug_file = nullptr;
    if (!debug_file) {
        fopen_s(&debug_file, "hook_debug.log", "a");
    }
    if (debug_file) {
        fprintf(debug_file, "%s\n", msg);
        fflush(debug_file);
    }
}

// Helper function to check if window is SAP GUI
static bool is_sap_gui_window(HWND hwnd) {
    if (!hwnd) return false;
    
    wchar_t class_name[256];
    GetClassNameW(hwnd, class_name, sizeof(class_name) / sizeof(class_name[0]));
    std::wstring wclass(class_name);
    
    // SAP GUI windows typically have class names containing "SAP"
    return (wclass.find(L"SAP") != std::wstring::npos) ||
           (wclass.find(L"sap") != std::wstring::npos);
}

// Low-level mouse hook callback
LRESULT CALLBACK MouseHookCallback(int nCode, WPARAM wParam, LPARAM lParam) {
    char debug_msg[256];
    
    if (nCode >= 0) {
        MSLLHOOKSTRUCT* mouse_data = (MSLLHOOKSTRUCT*)lParam;
        POINT pt = mouse_data->pt;
        HWND current_hwnd = WindowFromPoint(pt);
        
        if (wParam == WM_MOUSEMOVE) {
            sprintf_s(debug_msg, "MouseMove: hwnd=0x%p", reinterpret_cast<void*>(current_hwnd));
            hook_debug(debug_msg);
            
            // Change cursor to crosshair on mouse move
            if (g_crosshair_cursor) {
                SetCursor(g_crosshair_cursor);
            }
            
            // Only update console if window changed
            if (current_hwnd != g_last_hwnd) {
                g_last_hwnd = current_hwnd;
                
                // Check if this is a SAP window
                bool is_sap = is_sap_gui_window(current_hwnd);
                
                sprintf_s(debug_msg, "Window changed: is_sap=%d", is_sap ? 1 : 0);
                hook_debug(debug_msg);
                
                // Print status to console
                std::string status = is_sap ? "[SAP WINDOW DETECTED]" : "[Not a SAP window]";
                std::cout << "\r                                                                 ";  // Clear line
                std::cout << "\r" << status << std::flush;
            }
        }
        else if (wParam == WM_LBUTTONDOWN) {
            sprintf_s(debug_msg, "MouseClick detected!");
            hook_debug(debug_msg);
            
            if (g_click_event) {
                g_clicked_hwnd = current_hwnd;
                hook_debug("Setting click event...");
                std::cout << "\r[CLICK DETECTED!]" << std::endl;
                SetEvent(g_click_event);
                // Note: We don't consume the event - let it propagate normally
            }
        }
    }
    // Always call next hook - don't consume the event
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

HWND select_window_by_mouse_click(int timeout_seconds) {
    if (timeout_seconds <= 0) timeout_seconds = 10;

    // Load crosshair cursor
    g_crosshair_cursor = LoadCursor(nullptr, IDC_CROSS);
    
    // Create unnamed event for click notification
    g_click_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_click_event) {
        g_crosshair_cursor = nullptr;
        throw ComException("Failed to create click event");
    }

    g_clicked_hwnd = nullptr;
    g_last_hwnd = nullptr;

    // Install global mouse hook
    HHOOK mouse_hook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookCallback, nullptr, 0);
    if (!mouse_hook) {
        CloseHandle(g_click_event);
        g_click_event = nullptr;
        g_crosshair_cursor = nullptr;
        throw ComException("Failed to install mouse hook");
    }
    
    // Log hook installation
    hook_debug("Mouse hook installed successfully");
    spdlog::debug("Mouse hook installed");

    // Show prompt to user
    spdlog::info("Click on SAP GUI window within {} seconds...", timeout_seconds);
    std::cout << "\n>>> Click on SAP GUI window within " << timeout_seconds << " seconds..." << std::endl;
    std::cout << ">>> Hover over SAP windows to see detection status" << std::endl;

    // Process messages to allow hook to work - critical for WH_MOUSE_LL hooks!
    DWORD wait_result = WAIT_TIMEOUT;
    auto start = std::chrono::high_resolution_clock::now();
    
    MSG msg;
    while (true) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start
        ).count();
        
        if (elapsed_ms >= static_cast<long long>(timeout_seconds) * 1000) {
            wait_result = WAIT_TIMEOUT;
            break;
        }
        
        // Check if event was signaled (click detected)
        DWORD check_result = WaitForSingleObject(g_click_event, 10);  // Small timeout
        if (check_result == WAIT_OBJECT_0) {
            wait_result = WAIT_OBJECT_0;
            break;
        }
        
        // Process messages to allow hook to work
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // Unhook
    UnhookWindowsHookEx(mouse_hook);
    CloseHandle(g_click_event);
    g_click_event = nullptr;
    g_crosshair_cursor = nullptr;
    
    // Clear the status line
    std::cout << "\r                                 " << std::endl;

    if (wait_result == WAIT_TIMEOUT) {
        throw ComException("Window selection timeout - no click detected within " +
                          std::to_string(timeout_seconds) + " seconds");
    }

    if (wait_result != WAIT_OBJECT_0) {
        throw ComException("Window selection failed");
    }

    if (!g_clicked_hwnd) {
        throw ComException("No window selected");
    }

    // Verify it's a valid window
    wchar_t class_name[256];
    GetClassNameW(g_clicked_hwnd, class_name, sizeof(class_name) / sizeof(class_name[0]));

    // Convert wide string to regular string for logging
    std::wstring wclass(class_name);
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wclass.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string aclass(size_needed - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wclass.c_str(), -1, &aclass[0], size_needed, nullptr, nullptr);

    spdlog::debug("Selected window HWND=0x{:08X}, class={}",
                 reinterpret_cast<uintptr_t>(g_clicked_hwnd),
                 aclass);

    return g_clicked_hwnd;
}

SAPGuiWindow ComGuiApplication::find_window_by_hwnd(HWND target_hwnd) const {
    spdlog::debug("Searching for HWND=0x{:08X} in SAP connections", 
                 reinterpret_cast<uintptr_t>(target_hwnd));
    
    // Get window title and class for the clicked window
    wchar_t window_title[512];
    wchar_t class_name[256];
    GetWindowTextW(target_hwnd, window_title, sizeof(window_title) / sizeof(window_title[0]));
    GetClassNameW(target_hwnd, class_name, sizeof(class_name) / sizeof(class_name[0]));
    
    std::wstring wtitle(window_title);
    std::wstring wclass(class_name);
    
    int title_size = WideCharToMultiByte(CP_UTF8, 0, wtitle.c_str(), -1, nullptr, 0, nullptr, nullptr);
    int class_size = WideCharToMultiByte(CP_UTF8, 0, wclass.c_str(), -1, nullptr, 0, nullptr, nullptr);
    
    std::string title(title_size - 1, 0);
    std::string aclass(class_size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wtitle.c_str(), -1, &title[0], title_size, nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, wclass.c_str(), -1, &aclass[0], class_size, nullptr, nullptr);
    
    spdlog::debug("Clicked window title: '{}', class: '{}'", title, aclass);
    
    int conn_count = get_connection_count();
    spdlog::info("Found {} SAP connections to check", conn_count);
    
    if (conn_count == 0) {
        spdlog::warn("No SAP connections available");
        return SAPGuiWindow();
    }

    // Since SAP COM doesn't expose native HWND directly, we match by window title
    // This is the best we can do without direct HWND access from SAP COM
    for (int c = 0; c < conn_count; ++c) {
        try {
            auto connection = get_connection(c);
            if (!connection) {
                spdlog::warn("Connection {} returned null", c);
                continue;
            }
            
            int sess_count = connection->get_session_count();
            spdlog::info("Connection {} has {} sessions", c, sess_count);
            
            if (sess_count == 0) {
                spdlog::debug("Connection {} has no sessions", c);
                continue;
            }

            for (int s = 0; s < sess_count; ++s) {
                try {
                    spdlog::debug("Checking session {}/{}", c, s);
                    auto session = connection->get_session(s);
                    if (!session) {
                        spdlog::warn("Session {}/{} returned null", c, s);
                        continue;
                    }
                    
                    auto active_window = session->get_active_window();
                    if (!active_window) {
                        spdlog::debug("Session {}/{} has no active window", c, s);
                        continue;
                    }
                    
                    std::string active_title = active_window->get_title();
                    spdlog::info("Session {}/{} active window title: '{}'", c, s, active_title);
                    spdlog::info("Clicked window title: '{}'", title);
                    spdlog::info("Titles match or using best match: conn={}, sess={}", c, s);
                    
                    // Return the first matching session
                    return SAPGuiWindow(active_title, target_hwnd, c, s);
                } catch (const std::exception& e) {
                    spdlog::error("Exception checking session {}/{}: {}", c, s, e.what());
                    continue;
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("Exception checking connection {}: {}", c, e.what());
            continue;
        }
    }

    spdlog::warn("No matching SAP session found for clicked window");
    
    // Not found - return invalid window
    return SAPGuiWindow();
}

} // namespace sap
} // namespace fairyfly
