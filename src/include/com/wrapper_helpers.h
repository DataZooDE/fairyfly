#pragma once

#include "include/com/wrapper.h"
#include "include/sap_gui_base.h"
#include "include/com/utf8.h"
#include <spdlog/spdlog.h>
#include <comdef.h>

namespace fairyfly {
namespace sap {

/// Read a COM collection count for session-close verification. Unlike the
/// ordinary screen-reading helper, a failed read must not mean zero items.
int get_collection_count_checked(IDispatch* collection);

/// Get a string property from COM object (free function helper)
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

        return result.vt == VT_BSTR ? com::bstr_to_utf8(result.bstrVal) : "";
    } catch (const SapGuiException& e) {
        spdlog::debug("get_string_property: SapGuiException for {}: {}", prop_name, e.what());
        return "";
    } catch (const ComException& e) {
        spdlog::debug("get_string_property: ComException for {}: {}", prop_name, e.what());
        return "";
    } catch (const std::exception& e) {
        spdlog::debug("get_string_property: std::exception for {}: {}", prop_name, e.what());
        return "";
    }
}

/// Get an integer property from COM object (free function helper)
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
    } catch (const SapGuiException& e) {
        spdlog::warn("get_int_property|SapGuiException|prop={}|error={}", prop_name, e.what());
        return 0;
    } catch (const ComException& e) {
        spdlog::warn("get_int_property|ComException|prop={}|error={}", prop_name, e.what());
        return 0;
    } catch (const std::exception& e) {
        spdlog::warn("get_int_property|std::exception|prop={}|error={}", prop_name, e.what());
        return 0;
    }
}

/// Get a boolean property from COM object (free function helper)
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

        // Verify variant type before accessing boolVal to avoid undefined behavior
        if (result.vt == VT_BOOL) {
            return result.boolVal != VARIANT_FALSE;
        }

        spdlog::debug("get_bool_property|unexpected type|prop={}|vt={}", prop_name, (int)result.vt);
        return false;
    } catch (const SapGuiException& e) {
        spdlog::debug("get_bool_property: SapGuiException for {}: {}", prop_name, e.what());
        return false;
    } catch (const ComException& e) {
        spdlog::debug("get_bool_property: ComException for {}: {}", prop_name, e.what());
        return false;
    } catch (const std::exception& e) {
        spdlog::debug("get_bool_property: std::exception for {}: {}", prop_name, e.what());
        return false;
    }
}

/// Get a dispatch property from COM object (free function helper)
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
    } catch (const SapGuiException& e) {
        spdlog::warn("get_dispatch_property|SapGuiException|prop={}|error={}", prop_name, e.what());
        return nullptr;
    } catch (const ComException& e) {
        spdlog::warn("get_dispatch_property|ComException|prop={}|error={}", prop_name, e.what());
        return nullptr;
    } catch (const std::exception& e) {
        spdlog::warn("get_dispatch_property|std::exception|prop={}|error={}", prop_name, e.what());
        return nullptr;
    }
}

/// Set a string property on COM object (free function helper)
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
    } catch (const SapGuiException& e) {
        throw ComException(std::string("SAP GUI error setting property: ") + e.what(), e.hresult());
    } catch (const std::runtime_error& e) {
        throw ComException(std::string("Runtime error setting property: ") + e.what());
    } catch (const std::exception& e) {
        throw ComException(std::string("Exception setting property: ") + e.what());
    }
}

/// Call a method on COM object with string parameter using an already-resolved DISPID
/// (avoids the per-call ITypeInfo lookup). Same null-on-failure contract as
/// call_method_with_string.
inline IDispatchPtr call_method_with_string_dispid(IDispatch* obj, DISPID dispid,
                                                    const std::string& param) {
    if (!obj) throw ComException("Null object");

    try {
        HRESULT hr;
        _variant_t param_var(param.c_str());
        DISPPARAMS disp_params = {(VARIANT*)&param_var, nullptr, 1, 0};
        _variant_t result;
        hr = obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                        &disp_params, &result, nullptr, nullptr);
        if (FAILED(hr) || result.vt != VT_DISPATCH) return nullptr;

        return IDispatchPtr(result.pdispVal);
    } catch (const SapGuiException& e) {
        spdlog::debug("call_method_with_string: SapGuiException: {}", e.what());
        return nullptr;
    } catch (const ComException& e) {
        spdlog::debug("call_method_with_string: ComException: {}", e.what());
        return nullptr;
    } catch (const std::exception& e) {
        spdlog::debug("call_method_with_string: std::exception: {}", e.what());
        return nullptr;
    }
}

/// Call a method on COM object with string parameter (free function helper)
inline IDispatchPtr call_method_with_string(IDispatch* obj, const char* method_name,
                                             const std::string& param) {
    if (!obj) throw ComException("Null object");

    try {
        _bstr_t method(method_name);
        DISPID dispid;
        // Use get_dispid_via_typeinfo for SAP GUI compatibility
        HRESULT hr = get_dispid_via_typeinfo(obj, method.GetBSTR(), &dispid);
        if (FAILED(hr)) return nullptr;
        return call_method_with_string_dispid(obj, dispid, param);
    } catch (const SapGuiException& e) {
        spdlog::debug("call_method_with_string: SapGuiException: {}", e.what());
        return nullptr;
    } catch (const ComException& e) {
        spdlog::debug("call_method_with_string: ComException: {}", e.what());
        return nullptr;
    } catch (const std::exception& e) {
        spdlog::debug("call_method_with_string: std::exception: {}", e.what());
        return nullptr;
    }
}

/// Safely convert VARIANT with BSTR to string, with robust NULL pointer and access violation handling
/// This prevents segfaults from NULL BSTR pointers or invalid memory access
/// \param var The VARIANT to convert (should have vt == VT_BSTR)
/// \param context Optional context string for diagnostic logging (e.g., "get_item_text key='Favo', col='Text'")
/// \return String value, or empty string if BSTR is NULL or conversion fails
std::string safe_bstr_to_string(const VARIANT& var, const char* context = nullptr);

/// Safely invoke an IDispatch method or property with Win32 SEH protection
/// Catches access violations (0xC0000005) inside native SAP GUI COM controls
HRESULT safe_invoke(IDispatch* obj, DISPID dispid, WORD flags, DISPPARAMS* params, VARIANT* result);

} // namespace sap
} // namespace fairyfly

