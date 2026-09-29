#include "include/sap_gui_base.h"
#include "include/com/utf8.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <mutex>

namespace fairyfly {
namespace sap {

using fairyfly::utils::TraceGuard;

// Static type-level DISPID cache: map<type_name, map<prop_name, DISPID>>
static std::unordered_map<std::string, std::unordered_map<std::wstring, DISPID>> s_type_dispid_cache;
// Negative cache: map<type_name, map<prop_name, failing HRESULT>>. Members that a type
// does not have (Visible, AccLabel, DisplayedText on non-text types, ...) miss on every
// element of that type; each miss costs ~3 COM round trips, so remember it.
static std::unordered_map<std::string, std::unordered_map<std::wstring, HRESULT>> s_type_dispid_miss_cache;
static std::mutex s_dispid_cache_mutex;

static bool is_cacheable_dispid_miss(HRESULT hr) {
    return hr == DISP_E_UNKNOWNNAME || hr == DISP_E_MEMBERNOTFOUND ||
           hr == TYPE_E_ELEMENTNOTFOUND;
}

void SapGuiObject::clear_dispid_cache() {
    std::lock_guard<std::mutex> lock(s_dispid_cache_mutex);
    s_type_dispid_cache.clear();
    s_type_dispid_miss_cache.clear();
}

HRESULT SapGuiObject::resolve_dispid(const wchar_t* name, DISPID* dispid) const {
    if (!dispatch_ || !name || !dispid) return E_POINTER;

    std::string type_name;
    {
        std::lock_guard<std::mutex> lock(s_dispid_cache_mutex);
        if (type_cached_) {
            type_name = cached_type_;
        }
        if (!type_name.empty()) {
            auto type_it = s_type_dispid_cache.find(type_name);
            if (type_it != s_type_dispid_cache.end()) {
                auto prop_it = type_it->second.find(name);
                if (prop_it != type_it->second.end()) {
                    *dispid = prop_it->second;
                    return S_OK;
                }
            }
            auto miss_type_it = s_type_dispid_miss_cache.find(type_name);
            if (miss_type_it != s_type_dispid_miss_cache.end()) {
                auto miss_it = miss_type_it->second.find(name);
                if (miss_it != miss_type_it->second.end()) {
                    return miss_it->second;
                }
            }
        }
    }

    // Slow path: resolve via ITypeInfo
    HRESULT hr = get_dispid_via_typeinfo(dispatch_, name, dispid);
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(s_dispid_cache_mutex);
        if (type_name.empty() && type_cached_) {
            type_name = cached_type_;
        }
        if (!type_name.empty()) {
            s_type_dispid_cache[type_name][name] = *dispid;
        }
    } else if (is_cacheable_dispid_miss(hr)) {
        std::lock_guard<std::mutex> lock(s_dispid_cache_mutex);
        if (type_name.empty() && type_cached_) {
            type_name = cached_type_;
        }
        if (!type_name.empty()) {
            s_type_dispid_miss_cache[type_name][name] = hr;
        }
    }
    return hr;
}

// ============================================================================
// SapGuiObject Implementation
// ============================================================================

SapGuiObject::SapGuiObject(IDispatchPtr dispatch)
    : dispatch_(dispatch) {
    TraceGuard trace("SapGuiObject::constructor");
    if (!dispatch_) {
        spdlog::warn("SapGuiObject constructed with null IDispatch");
    }
}

std::string SapGuiObject::get_string_property(const wchar_t* name) const {
    TraceGuard trace("SapGuiObject::get_string_property");

    if (!dispatch_) {
        spdlog::error("get_string_property|null dispatch|prop={}", (const char*)_bstr_t(name));
        return "";
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(name, &dispid);
    if (FAILED(hr)) {
        spdlog::debug("get_string_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                      (const char*)_bstr_t(name), (unsigned int)hr);
        return "";
    }

    // Invoke property getter
    DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
    _variant_t result;
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_PROPERTYGET | DISPATCH_METHOD,
        &no_params,
        &result,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::debug("get_string_property|Invoke failed|prop={}|hr={:#010x}",
                      (const char*)_bstr_t(name), (unsigned int)hr);
        return "";
    }

    // Convert to string
    if (result.vt == VT_BSTR && result.bstrVal) {
        std::string value = com::bstr_to_utf8(result.bstrVal);
        spdlog::debug("get_string_property|success|prop={}", (const char*)_bstr_t(name));
        return value;
    }

    spdlog::warn("get_string_property|unexpected type|prop={}|vt={}", (const char*)_bstr_t(name), (int)result.vt);
    return "";
}

int SapGuiObject::get_int_property(const wchar_t* name) const {
    TraceGuard trace("SapGuiObject::get_int_property");

    if (!dispatch_) {
        spdlog::error("get_int_property|null dispatch|prop={}", (const char*)_bstr_t(name));
        return 0;
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(name, &dispid);
    if (FAILED(hr)) {
        spdlog::debug("get_int_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                      (const char*)_bstr_t(name), (unsigned int)hr);
        return 0;
    }

    // Invoke property getter
    DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
    _variant_t result;
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_PROPERTYGET | DISPATCH_METHOD,
        &no_params,
        &result,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::debug("get_int_property|Invoke failed|prop={}|hr={:#010x}",
                      (const char*)_bstr_t(name), (unsigned int)hr);
        return 0;
    }

    // Convert to int
    if (result.vt == VT_I4 || result.vt == VT_I2) {
        int value = (int)result;
        spdlog::debug("get_int_property|success|prop={}|value={}", (const char*)_bstr_t(name), value);
        return value;
    }

    spdlog::debug("get_int_property|unexpected type|prop={}|vt={}", (const char*)_bstr_t(name), (int)result.vt);
    return 0;
}

bool SapGuiObject::get_bool_property(const wchar_t* name) const {
    TraceGuard trace("SapGuiObject::get_bool_property");

    if (!dispatch_) {
        spdlog::error("get_bool_property|null dispatch|prop={}", (const char*)_bstr_t(name));
        return false;
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(name, &dispid);
    if (FAILED(hr)) {
        spdlog::debug("get_bool_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                      (const char*)_bstr_t(name), (unsigned int)hr);
        return false;
    }

    // Invoke property getter
    DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
    _variant_t result;
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_PROPERTYGET | DISPATCH_METHOD,
        &no_params,
        &result,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::debug("get_bool_property|Invoke failed|prop={}|hr={:#010x}",
                      (const char*)_bstr_t(name), (unsigned int)hr);
        return false;
    }

    // Convert to bool
    if (result.vt == VT_BOOL) {
        bool value = (result.boolVal != VARIANT_FALSE);
        spdlog::debug("get_bool_property|success|prop={}|value={}", (const char*)_bstr_t(name), value);
        return value;
    }

    spdlog::debug("get_bool_property|unexpected type|prop={}|vt={}", (const char*)_bstr_t(name), (int)result.vt);
    return false;
}

IDispatchPtr SapGuiObject::get_dispatch_property(const wchar_t* name) const {
    TraceGuard trace("SapGuiObject::get_dispatch_property");

    if (!dispatch_) {
        spdlog::error("get_dispatch_property|null dispatch|prop={}", (const char*)_bstr_t(name));
        return nullptr;
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(name, &dispid);
    if (FAILED(hr)) {
        spdlog::debug("get_dispatch_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                      (const char*)_bstr_t(name), (unsigned int)hr);
        return nullptr;
    }

    // Invoke property getter
    DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
    _variant_t result;
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_PROPERTYGET | DISPATCH_METHOD,
        &no_params,
        &result,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::debug("get_dispatch_property|Invoke failed|prop={}|hr={:#010x}",
                      (const char*)_bstr_t(name), (unsigned int)hr);
        return nullptr;
    }

    // Convert to IDispatch
    if (result.vt == VT_DISPATCH && result.pdispVal) {
        spdlog::debug("get_dispatch_property|success|prop={}", (const char*)_bstr_t(name));
        return IDispatchPtr(result.pdispVal, true); // Attach without AddRef
    }

    spdlog::debug("get_dispatch_property|unexpected type|prop={}|vt={}", (const char*)_bstr_t(name), (int)result.vt);
    return nullptr;
}

void SapGuiObject::set_string_property(const wchar_t* name, const std::string& value) {
    TraceGuard trace("SapGuiObject::set_string_property");

    if (!dispatch_) {
        spdlog::error("set_string_property|null dispatch|prop={}", (const char*)_bstr_t(name));
        throw SapGuiException("Cannot set property on null object", E_POINTER, "SapGuiObject", "set_string_property");
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(name, &dispid);
    if (FAILED(hr)) {
        spdlog::error("set_string_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                     (const char*)_bstr_t(name), (unsigned int)hr);
        throw SapGuiException("Failed to get property DISPID", hr, "SapGuiObject", "set_string_property");
    }

    // Prepare parameters
    const auto wide_value = com::utf8_to_wide(value);
    _variant_t var_value(wide_value.c_str());
    DISPID dispid_named = DISPID_PROPERTYPUT;
    DISPPARAMS params;
    params.rgvarg = &var_value;
    params.rgdispidNamedArgs = &dispid_named;
    params.cArgs = 1;
    params.cNamedArgs = 1;

    // Invoke property setter
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_PROPERTYPUT,
        &params,
        nullptr,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::error("set_string_property|Invoke failed|prop={}|hr={:#010x}",
                     (const char*)_bstr_t(name), (unsigned int)hr);
        throw SapGuiException("Failed to set property", hr, "SapGuiObject", "set_string_property");
    }

    spdlog::debug("set_string_property|success|prop={}", (const char*)_bstr_t(name));
}

void SapGuiObject::set_int_property(const wchar_t* name, int value) {
    TraceGuard trace("SapGuiObject::set_int_property");

    if (!dispatch_) {
        spdlog::error("set_int_property|null dispatch|prop={}", (const char*)_bstr_t(name));
        throw SapGuiException("Cannot set property on null object", E_POINTER, "SapGuiObject", "set_int_property");
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(name, &dispid);
    if (FAILED(hr)) {
        spdlog::error("set_int_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                     (const char*)_bstr_t(name), (unsigned int)hr);
        throw SapGuiException("Failed to get property DISPID", hr, "SapGuiObject", "set_int_property");
    }

    // Prepare parameters
    _variant_t var_value(value);
    DISPID dispid_named = DISPID_PROPERTYPUT;
    DISPPARAMS params;
    params.rgvarg = &var_value;
    params.rgdispidNamedArgs = &dispid_named;
    params.cArgs = 1;
    params.cNamedArgs = 1;

    // Invoke property setter
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_PROPERTYPUT,
        &params,
        nullptr,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::error("set_int_property|Invoke failed|prop={}|value={}|hr={:#010x}",
                     (const char*)_bstr_t(name), value, (unsigned int)hr);
        throw SapGuiException("Failed to set property", hr, "SapGuiObject", "set_int_property");
    }

    spdlog::debug("set_int_property|success|prop={}|value={}", (const char*)_bstr_t(name), value);
}

void SapGuiObject::set_bool_property(const wchar_t* name, bool value) {
    TraceGuard trace("SapGuiObject::set_bool_property");

    if (!dispatch_) {
        spdlog::error("set_bool_property|null dispatch|prop={}", (const char*)_bstr_t(name));
        throw SapGuiException("Cannot set property on null object", E_POINTER, "SapGuiObject", "set_bool_property");
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(name, &dispid);
    if (FAILED(hr)) {
        spdlog::error("set_bool_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                     (const char*)_bstr_t(name), (unsigned int)hr);
        throw SapGuiException("Failed to get property DISPID", hr, "SapGuiObject", "set_bool_property");
    }

    // Prepare parameters
    _variant_t var_value(value);
    DISPID dispid_named = DISPID_PROPERTYPUT;
    DISPPARAMS params;
    params.rgvarg = &var_value;
    params.rgdispidNamedArgs = &dispid_named;
    params.cArgs = 1;
    params.cNamedArgs = 1;

    // Invoke property setter
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_PROPERTYPUT,
        &params,
        nullptr,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::error("set_bool_property|Invoke failed|prop={}|value={}|hr={:#010x}",
                     (const char*)_bstr_t(name), value, (unsigned int)hr);
        throw SapGuiException("Failed to set property", hr, "SapGuiObject", "set_bool_property");
    }

    spdlog::debug("set_bool_property|success|prop={}|value={}", (const char*)_bstr_t(name), value);
}

IDispatchPtr SapGuiObject::invoke_method_dispatch(const wchar_t* method_name) const {
    TraceGuard trace("SapGuiObject::invoke_method_dispatch");

    if (!dispatch_) {
        spdlog::error("invoke_method_dispatch|null dispatch|method={}", (const char*)_bstr_t(method_name));
        throw SapGuiException("Cannot invoke method on null object", E_POINTER, "SapGuiObject", "invoke_method_dispatch");
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(method_name, &dispid);
    if (FAILED(hr)) {
        spdlog::error("invoke_method_dispatch|GetIDsOfNames failed|method={}|hr={:#010x}",
                     (const char*)_bstr_t(method_name), (unsigned int)hr);
        throw SapGuiException("Failed to get method DISPID", hr, "SapGuiObject", "invoke_method_dispatch");
    }

    // Invoke method
    DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
    _variant_t result;
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_METHOD,
        &no_params,
        &result,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::error("invoke_method_dispatch|Invoke failed|method={}|hr={:#010x}",
                     (const char*)_bstr_t(method_name), (unsigned int)hr);
        throw SapGuiException("Failed to invoke method", hr, "SapGuiObject", "invoke_method_dispatch");
    }

    if (result.vt == VT_DISPATCH && result.pdispVal) {
        spdlog::debug("invoke_method_dispatch|success|method={}", (const char*)_bstr_t(method_name));
        return IDispatchPtr(result.pdispVal, true); // Attach without AddRef
    }

    spdlog::warn("invoke_method_dispatch|unexpected type|method={}|vt={}", (const char*)_bstr_t(method_name), (int)result.vt);
    return nullptr;
}

void SapGuiObject::invoke_method_with_string(const wchar_t* method_name, const std::string& param) {
    TraceGuard trace("SapGuiObject::invoke_method_with_string");

    if (!dispatch_) {
        spdlog::error("invoke_method_with_string|null dispatch|method={}", (const char*)_bstr_t(method_name));
        throw SapGuiException("Cannot invoke method on null object", E_POINTER, "SapGuiObject", "invoke_method_with_string");
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(method_name, &dispid);
    if (FAILED(hr)) {
        spdlog::error("invoke_method_with_string|GetIDsOfNames failed|method={}|hr={:#010x}",
                     (const char*)_bstr_t(method_name), (unsigned int)hr);
        throw SapGuiException("Failed to get method DISPID", hr, "SapGuiObject", "invoke_method_with_string");
    }

    // Prepare parameters
    const auto wide_param = com::utf8_to_wide(param);
    _variant_t var_param(wide_param.c_str());
    DISPPARAMS params;
    params.rgvarg = &var_param;
    params.rgdispidNamedArgs = nullptr;
    params.cArgs = 1;
    params.cNamedArgs = 0;

    // Invoke method
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_METHOD,
        &params,
        nullptr,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::error("invoke_method_with_string|Invoke failed|method={}|param={}|hr={:#010x}",
                     (const char*)_bstr_t(method_name), param, (unsigned int)hr);
        throw SapGuiException("Failed to invoke method", hr, "SapGuiObject", "invoke_method_with_string");
    }

    spdlog::debug("invoke_method_with_string|success|method={}|param={}", (const char*)_bstr_t(method_name), param);
}

void SapGuiObject::invoke_method_with_int(const wchar_t* method_name, int param) {
    TraceGuard trace("SapGuiObject::invoke_method_with_int");

    if (!dispatch_) {
        spdlog::error("invoke_method_with_int|null dispatch|method={}", (const char*)_bstr_t(method_name));
        throw SapGuiException("Cannot invoke method on null object", E_POINTER, "SapGuiObject", "invoke_method_with_int");
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(method_name, &dispid);
    if (FAILED(hr)) {
        spdlog::error("invoke_method_with_int|GetIDsOfNames failed|method={}|hr={:#010x}",
                     (const char*)_bstr_t(method_name), (unsigned int)hr);
        throw SapGuiException("Failed to get method DISPID", hr, "SapGuiObject", "invoke_method_with_int");
    }

    // Prepare parameters
    _variant_t var_param(param);
    DISPPARAMS params;
    params.rgvarg = &var_param;
    params.rgdispidNamedArgs = nullptr;
    params.cArgs = 1;
    params.cNamedArgs = 0;

    // Invoke method
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_METHOD,
        &params,
        nullptr,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::error("invoke_method_with_int|Invoke failed|method={}|param={}|hr={:#010x}",
                     (const char*)_bstr_t(method_name), param, (unsigned int)hr);
        throw SapGuiException("Failed to invoke method", hr, "SapGuiObject", "invoke_method_with_int");
    }

    spdlog::debug("invoke_method_with_int|success|method={}|param={}", (const char*)_bstr_t(method_name), param);
}

void SapGuiObject::invoke_method_void(const wchar_t* method_name) {
    TraceGuard trace("SapGuiObject::invoke_method_void");

    if (!dispatch_) {
        spdlog::error("invoke_method_void|null dispatch|method={}", (const char*)_bstr_t(method_name));
        throw SapGuiException("Cannot invoke method on null object", E_POINTER, "SapGuiObject", "invoke_method_void");
    }

    // Get DISPID using cached type lookup (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(method_name, &dispid);
    if (FAILED(hr)) {
        spdlog::error("invoke_method_void|GetIDsOfNames failed|method={}|hr={:#010x}",
                     (const char*)_bstr_t(method_name), (unsigned int)hr);
        throw SapGuiException("Failed to get method DISPID", hr, "SapGuiObject", "invoke_method_void");
    }

    // Invoke method
    DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
    hr = dispatch_->Invoke(
        dispid,
        IID_NULL,
        LOCALE_USER_DEFAULT,
        DISPATCH_METHOD,
        &no_params,
        nullptr,
        nullptr,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::error("invoke_method_void|Invoke failed|method={}|hr={:#010x}",
                     (const char*)_bstr_t(method_name), (unsigned int)hr);
        throw SapGuiException("Failed to invoke method", hr, "SapGuiObject", "invoke_method_void");
    }

    spdlog::debug("invoke_method_void|success|method={}", (const char*)_bstr_t(method_name));
}

std::string SapGuiObject::get_id() const {
    if (id_cached_) {
        return cached_id_;
    }

    cached_id_ = get_string_property(L"Id");
    id_cached_ = true;
    return cached_id_;
}

std::string SapGuiObject::get_type() const {
    if (type_cached_) {
        return cached_type_;
    }

    cached_type_ = get_string_property(L"Type");
    type_cached_ = true;
    return cached_type_;
}

int SapGuiObject::get_type_as_number() const {
    return get_int_property(L"TypeAsNumber");
}

std::string SapGuiObject::get_name() const {
    return get_string_property(L"Name");
}

bool SapGuiObject::operator==(const SapGuiObject& other) const {
    if (!dispatch_ || !other.dispatch_) return dispatch_ == other.dispatch_;

    IUnknown* this_identity = nullptr;
    IUnknown* other_identity = nullptr;
    const HRESULT this_hr = dispatch_->QueryInterface(IID_IUnknown,
                                                     reinterpret_cast<void**>(&this_identity));
    const HRESULT other_hr = other.dispatch_->QueryInterface(IID_IUnknown,
                                                            reinterpret_cast<void**>(&other_identity));
    const bool same = this_identity == other_identity;
    if (this_identity) this_identity->Release();
    if (other_identity) other_identity->Release();
    if (FAILED(this_hr) || FAILED(other_hr)) {
        throw SapGuiException("Could not determine COM object identity",
                              FAILED(this_hr) ? this_hr : other_hr);
    }
    return same;
}

} // namespace sap
} // namespace fairyfly
