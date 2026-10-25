#pragma once

#include "include/com/wrapper.h"
#include "include/sap_gui_base.h"
#include "include/com/utf8.h"
#include <spdlog/spdlog.h>
#include <comdef.h>

#include "include/com/invoke.h"

namespace fairyfly {
namespace sap {

/// Read a COM collection count for session-close verification. Unlike the
/// ordinary screen-reading helper, a failed read must not mean zero items.
int get_collection_count_checked(IDispatch* collection);

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
        hr = invoke(obj, dispid, DISPATCH_PROPERTYGET, {}, &result);
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

/// Call a method on COM object with string parameter using an already-resolved DISPID
/// (avoids the per-call ITypeInfo lookup). Same null-on-failure contract as
/// call_method_with_string.
inline IDispatchPtr call_method_with_string_dispid(IDispatch* obj, DISPID dispid,
                                                    const std::string& param) {
    if (!obj) throw ComException("Null object");

    try {
        _variant_t result;   // the parameter as UTF-8 (it went through the ANSI code page before)
        const HRESULT hr = invoke(obj, dispid, DISPATCH_METHOD, {_variant_t(com::utf8_to_wide(param).c_str())}, &result);
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
/// safe_invoke with `args` in reading order (DispArgs).
inline HRESULT safe_invoke(IDispatch* obj, DISPID dispid, WORD flags, std::initializer_list<_variant_t> args, VARIANT* result) {
    DispArgs params(flags, args);
    return safe_invoke(obj, dispid, flags, params.get(), result);
}

} // namespace sap
} // namespace fairyfly

