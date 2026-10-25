#include "include/text/utf8.h"
#include "include/com/invoke.h"
#include "include/com/wrapper_helpers.h"
#include "include/sap_gui_base.h"
#include "include/com/utf8.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cwchar>

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
// Universal Type DISPID: every SAP GUI object is expected to expose Type under the same DISPID.
// Never trusted blindly: it is only used to read a value that is then checked against the
// set of type names already confirmed through ITypeInfo (a name is added when its own
// typeinfo DISPID equals the universal one). One disagreement disables the shortcut for the
// process, so a wrong DISPID cannot silently misread properties. All guarded by the mutex above.
static DISPID s_universal_type_dispid = DISPID_UNKNOWN;
static bool s_universal_type_known = false;
static bool s_universal_type_disabled = false;
static std::unordered_set<std::string> s_validated_type_names;

// GuiShell members that no shell subtype exposes (grid, tree, toolbar, HTML viewer, calendar, ...).
// Only these may be negatively cached for the shared "GuiShell" Type string.
static bool is_shell_member_always_absent(const wchar_t* name) {
    return std::wcscmp(name, L"DisplayedText") == 0;
}

static bool is_cacheable_dispid_miss(HRESULT hr) {
    return hr == DISP_E_UNKNOWNNAME || hr == DISP_E_MEMBERNOTFOUND ||
           hr == TYPE_E_ELEMENTNOTFOUND;
}

void SapGuiObject::clear_dispid_cache() {
    std::lock_guard<std::mutex> lock(s_dispid_cache_mutex);
    s_type_dispid_cache.clear();
    s_type_dispid_miss_cache.clear();
    s_universal_type_dispid = DISPID_UNKNOWN;
    s_universal_type_known = false;
    s_universal_type_disabled = false;
    s_validated_type_names.clear();
}

HRESULT SapGuiObject::resolve_dispid(const wchar_t* name, DISPID* dispid) const {
    return resolve_dispid_ex(name, dispid, nullptr);
}

HRESULT SapGuiObject::resolve_dispid_ex(const wchar_t* name, DISPID* dispid, bool* from_cache) const {
    if (from_cache) *from_cache = false;
    if (!dispatch_ || !name || !dispid) return E_POINTER;

    // An unprimed wrapper reads its Type first (cheap with the universal DISPID) so the
    // per-type cache below applies to the very first property read. "Type" itself skips
    // this: get_type() resolves it through the slow path and must not recurse.
    if (!type_cached_ && dispatch_ && std::wcscmp(name, L"Type") != 0) {
        get_type();
    }

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
                    if (from_cache) *from_cache = true;
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
        // SAP reports every shell (grid, tree, toolbar, HTML viewer, calendar, ...) as Type
        // "GuiShell" while the members differ by SubType (RowCount/GetCellValue exist on
        // GridView only). A miss on one shell must not poison the others, so shells are never
        // negatively cached. Other types (GuiTextField, GuiButton, GuiCustomControl,
        // GuiContainerShell, ...) have a fixed interface per Type string and stay cached.
        // Exception: an explicit allowlist of members no shell subtype has (DisplayedText is a
        // text-field member; get_text probes it on every element).
        if (!type_name.empty() && (type_name != "GuiShell" || is_shell_member_always_absent(name))) {
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

HRESULT SapGuiObject::invoke_property_get(const wchar_t* name, _variant_t& result, bool* lookup_failed) const {
    if (lookup_failed) *lookup_failed = false;
    DISPID dispid;
    bool from_cache = false;
    HRESULT hr = resolve_dispid_ex(name, &dispid, &from_cache);
    if (FAILED(hr)) {
        if (lookup_failed) *lookup_failed = true;
        return hr;
    }

    DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
    hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET | DISPATCH_METHOD,
                           &no_params, &result, nullptr, nullptr);

    // A cached DISPID that the object now rejects is stale: drop it, re-resolve through ITypeInfo
    // and retry exactly once.
    if (from_cache && (hr == DISP_E_MEMBERNOTFOUND || hr == DISP_E_UNKNOWNNAME)) {
        std::string type_name;
        {
            std::lock_guard<std::mutex> lock(s_dispid_cache_mutex);
            if (type_cached_) type_name = cached_type_;
            auto type_it = s_type_dispid_cache.find(type_name);
            if (type_it != s_type_dispid_cache.end()) type_it->second.erase(name);
        }
        DISPID fresh;
        if (SUCCEEDED(get_dispid_via_typeinfo(dispatch_, name, &fresh))) {
            if (!type_name.empty()) {
                std::lock_guard<std::mutex> lock(s_dispid_cache_mutex);
                s_type_dispid_cache[type_name][name] = fresh;
            }
            hr = dispatch_->Invoke(fresh, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET | DISPATCH_METHOD,
                                   &no_params, &result, nullptr, nullptr);
        }
    }
    return hr;
}

std::string SapGuiObject::get_string_property(const wchar_t* name) const {
    TraceGuard trace("SapGuiObject::get_string_property");

    if (!dispatch_) {
        spdlog::error("get_string_property|null dispatch|prop={}", text::wide_to_utf8_lossy(name));
        return "";
    }

    // Resolve the DISPID (cached type lookup, falling back to ITypeInfo) and invoke the getter
    _variant_t result;
    bool lookup_failed = false;
    HRESULT hr = invoke_property_get(name, result, &lookup_failed);
    if (lookup_failed) {
        spdlog::debug("get_string_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                      text::wide_to_utf8_lossy(name), (unsigned int)hr);
        return "";
    }
    if (FAILED(hr)) {
        spdlog::debug("get_string_property|Invoke failed|prop={}|hr={:#010x}",
                      text::wide_to_utf8_lossy(name), (unsigned int)hr);
        return "";
    }

    // Convert to string
    if (result.vt == VT_BSTR && result.bstrVal) {
        std::string value = com::bstr_to_utf8(result.bstrVal);
        spdlog::debug("get_string_property|success|prop={}", text::wide_to_utf8_lossy(name));
        return value;
    }

    spdlog::warn("get_string_property|unexpected type|prop={}|vt={}", text::wide_to_utf8_lossy(name), (int)result.vt);
    return "";
}

namespace {
/// Gets `name` from `object` without swallowing anything: Ok + variant, NotSupported (unknown member), or Failed.
PropertyStatus strict_property_get(const SapGuiObject& object, IDispatch* dispatch, const wchar_t* name, _variant_t& result) {
    if (!dispatch) return PropertyStatus::Failed;
    DISPID dispid;
    const HRESULT lookup = object.resolve_dispid(name, &dispid);
    if (lookup == DISP_E_UNKNOWNNAME || lookup == DISP_E_MEMBERNOTFOUND || lookup == TYPE_E_ELEMENTNOTFOUND)
        return PropertyStatus::NotSupported;
    if (FAILED(lookup)) return PropertyStatus::Failed;
    DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
    const HRESULT hr = dispatch->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET | DISPATCH_METHOD,
                                        &no_params, &result, nullptr, nullptr);
    return FAILED(hr) ? PropertyStatus::Failed : PropertyStatus::Ok;
}
}  // namespace

PropertyRead SapGuiObject::read_string_property_strict(const wchar_t* name) const {
    PropertyRead out;
    try {
        _variant_t result;
        out.status = strict_property_get(*this, dispatch_, name, result);
        if (out.status != PropertyStatus::Ok) return out;
        if (result.vt == VT_BSTR) {
            if (result.bstrVal) out.value = com::bstr_to_utf8(result.bstrVal);  // a null BSTR is the empty string
            return out;
        }
        out.status = PropertyStatus::Failed;  // unexpected variant type
    } catch (...) {
        out = PropertyRead{};
    }
    return out;
}

PropertyRead SapGuiObject::read_bool_property_strict(const wchar_t* name) const {
    PropertyRead out;
    try {
        _variant_t result;
        out.status = strict_property_get(*this, dispatch_, name, result);
        if (out.status != PropertyStatus::Ok) return out;
        if (result.vt == VT_BOOL) {
            out.value = result.boolVal != VARIANT_FALSE ? "true" : "false";
            return out;
        }
        out.status = PropertyStatus::Failed;
    } catch (...) {
        out = PropertyRead{};
    }
    return out;
}

PropertyRead SapGuiObject::read_object_text_strict(const wchar_t* object_property, const wchar_t* text_property) const {
    PropertyRead out;
    try {
        _variant_t result;
        out.status = strict_property_get(*this, dispatch_, object_property, result);
        if (out.status != PropertyStatus::Ok) return out;
        if ((result.vt == VT_DISPATCH && !result.pdispVal) || result.vt == VT_EMPTY || result.vt == VT_NULL) {
            out.status = PropertyStatus::NotSupported;  // Nothing: the control has no such object (no label)
            return out;
        }
        if (result.vt != VT_DISPATCH) {
            out.status = PropertyStatus::Failed;
            return out;
        }
        SapGuiObject inner{IDispatchPtr(result.pdispVal)};
        return inner.read_string_property_strict(text_property);
    } catch (...) {
        out = PropertyRead{};
    }
    return out;
}

int SapGuiObject::get_int_property(const wchar_t* name) const {
    TraceGuard trace("SapGuiObject::get_int_property");

    if (!dispatch_) {
        spdlog::error("get_int_property|null dispatch|prop={}", text::wide_to_utf8_lossy(name));
        return 0;
    }

    // Resolve the DISPID (cached type lookup, falling back to ITypeInfo) and invoke the getter
    _variant_t result;
    bool lookup_failed = false;
    HRESULT hr = invoke_property_get(name, result, &lookup_failed);
    if (lookup_failed) {
        spdlog::debug("get_int_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                      text::wide_to_utf8_lossy(name), (unsigned int)hr);
        return 0;
    }
    if (FAILED(hr)) {
        spdlog::debug("get_int_property|Invoke failed|prop={}|hr={:#010x}",
                      text::wide_to_utf8_lossy(name), (unsigned int)hr);
        return 0;
    }

    // Convert to int
    if (result.vt == VT_I4 || result.vt == VT_I2) {
        int value = (int)result;
        spdlog::debug("get_int_property|success|prop={}|value={}", text::wide_to_utf8_lossy(name), value);
        return value;
    }

    spdlog::debug("get_int_property|unexpected type|prop={}|vt={}", text::wide_to_utf8_lossy(name), (int)result.vt);
    return 0;
}

bool SapGuiObject::get_bool_property(const wchar_t* name) const {
    TraceGuard trace("SapGuiObject::get_bool_property");

    if (!dispatch_) {
        spdlog::error("get_bool_property|null dispatch|prop={}", text::wide_to_utf8_lossy(name));
        return false;
    }

    // Resolve the DISPID (cached type lookup, falling back to ITypeInfo) and invoke the getter
    _variant_t result;
    bool lookup_failed = false;
    HRESULT hr = invoke_property_get(name, result, &lookup_failed);
    if (lookup_failed) {
        spdlog::debug("get_bool_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                      text::wide_to_utf8_lossy(name), (unsigned int)hr);
        return false;
    }
    if (FAILED(hr)) {
        spdlog::debug("get_bool_property|Invoke failed|prop={}|hr={:#010x}",
                      text::wide_to_utf8_lossy(name), (unsigned int)hr);
        return false;
    }

    // Convert to bool
    if (result.vt == VT_BOOL) {
        bool value = (result.boolVal != VARIANT_FALSE);
        spdlog::debug("get_bool_property|success|prop={}|value={}", text::wide_to_utf8_lossy(name), value);
        return value;
    }

    spdlog::debug("get_bool_property|unexpected type|prop={}|vt={}", text::wide_to_utf8_lossy(name), (int)result.vt);
    return false;
}

IDispatchPtr SapGuiObject::get_dispatch_property(const wchar_t* name) const {
    TraceGuard trace("SapGuiObject::get_dispatch_property");

    if (!dispatch_) {
        spdlog::error("get_dispatch_property|null dispatch|prop={}", text::wide_to_utf8_lossy(name));
        return nullptr;
    }

    // Resolve the DISPID (cached type lookup, falling back to ITypeInfo) and invoke the getter
    _variant_t result;
    bool lookup_failed = false;
    HRESULT hr = invoke_property_get(name, result, &lookup_failed);
    if (lookup_failed) {
        spdlog::debug("get_dispatch_property|GetIDsOfNames failed|prop={}|hr={:#010x}",
                      text::wide_to_utf8_lossy(name), (unsigned int)hr);
        return nullptr;
    }
    if (FAILED(hr)) {
        spdlog::debug("get_dispatch_property|Invoke failed|prop={}|hr={:#010x}",
                      text::wide_to_utf8_lossy(name), (unsigned int)hr);
        return nullptr;
    }

    // Convert to IDispatch
    if (result.vt == VT_DISPATCH && result.pdispVal) {
        spdlog::debug("get_dispatch_property|success|prop={}", text::wide_to_utf8_lossy(name));
        return IDispatchPtr(result.pdispVal, true); // Attach without AddRef
    }

    spdlog::debug("get_dispatch_property|unexpected type|prop={}|vt={}", text::wide_to_utf8_lossy(name), (int)result.vt);
    return nullptr;
}

_variant_t SapGuiObject::invoke_checked(const char* operation, const wchar_t* name, WORD flags, VARIANT* argument,
                                        const std::string& log_value) const {
    const bool property = flags == DISPATCH_PROPERTYPUT;
    const char* kind = property ? "prop" : "method";
    const std::string member = text::wide_to_utf8_lossy(name);
    if (!dispatch_) {
        spdlog::error("{}|null dispatch|{}={}", operation, kind, member);
        throw SapGuiException(property ? "Cannot set property on null object" : "Cannot invoke method on null object",
                              E_POINTER, "SapGuiObject", operation);
    }
    // DISPID through the per-type cache (falling back to ITypeInfo)
    DISPID dispid;
    HRESULT hr = resolve_dispid(name, &dispid);
    if (FAILED(hr)) {
        spdlog::error("{}|GetIDsOfNames failed|{}={}|hr={:#010x}", operation, kind, member, (unsigned int)hr);
        throw SapGuiException(property ? "Failed to get property DISPID" : "Failed to get method DISPID", hr, "SapGuiObject",
                              operation);
    }
    _variant_t result;
    hr = argument ? invoke(dispatch_, dispid, flags, {_variant_t(*argument)}, property ? nullptr : &result)
                  : invoke(dispatch_, dispid, flags, {}, &result);
    if (FAILED(hr)) {
        spdlog::error("{}|Invoke failed|{}={}{}|hr={:#010x}", operation, kind, member, log_value, (unsigned int)hr);
        throw SapGuiException(property ? "Failed to set property" : "Failed to invoke method", hr, "SapGuiObject", operation);
    }
    spdlog::debug("{}|success|{}={}{}", operation, kind, member, log_value);
    return result;
}

HRESULT SapGuiObject::call_method(const wchar_t* name, std::initializer_list<_variant_t> args, _variant_t& result) const {
    if (!dispatch_) return E_POINTER;
    DISPID dispid;
    const HRESULT hr = get_dispid_via_typeinfo(dispatch_, name, &dispid);
    if (FAILED(hr)) return hr;
    return safe_invoke(dispatch_, dispid, DISPATCH_METHOD, args, &result);
}

void SapGuiObject::set_string_property(const wchar_t* name, const std::string& value) {
    TraceGuard trace("SapGuiObject::set_string_property");
    _variant_t argument(com::utf8_to_wide(value).c_str());
    invoke_checked("set_string_property", name, DISPATCH_PROPERTYPUT, &argument);   // the value is not logged
}

void SapGuiObject::set_int_property(const wchar_t* name, int value) {
    TraceGuard trace("SapGuiObject::set_int_property");
    _variant_t argument(value);
    invoke_checked("set_int_property", name, DISPATCH_PROPERTYPUT, &argument, "|value=" + std::to_string(value));
}

void SapGuiObject::set_bool_property(const wchar_t* name, bool value) {
    TraceGuard trace("SapGuiObject::set_bool_property");
    _variant_t argument(value);
    invoke_checked("set_bool_property", name, DISPATCH_PROPERTYPUT, &argument, value ? "|value=true" : "|value=false");
}

IDispatchPtr SapGuiObject::invoke_method_dispatch(const wchar_t* method_name) const {
    TraceGuard trace("SapGuiObject::invoke_method_dispatch");
    _variant_t result = invoke_checked("invoke_method_dispatch", method_name, DISPATCH_METHOD, nullptr);
    if (result.vt == VT_DISPATCH && result.pdispVal) return IDispatchPtr(result.pdispVal, true);   // AddRef: result owns its own
    spdlog::warn("invoke_method_dispatch|unexpected type|method={}|vt={}", text::wide_to_utf8_lossy(method_name), (int)result.vt);
    return nullptr;
}

void SapGuiObject::invoke_method_with_string(const wchar_t* method_name, const std::string& param) {
    TraceGuard trace("SapGuiObject::invoke_method_with_string");
    _variant_t argument(com::utf8_to_wide(param).c_str());
    invoke_checked("invoke_method_with_string", method_name, DISPATCH_METHOD, &argument, "|param=" + param);
}

void SapGuiObject::invoke_method_with_int(const wchar_t* method_name, int param) {
    TraceGuard trace("SapGuiObject::invoke_method_with_int");
    _variant_t argument(param);
    invoke_checked("invoke_method_with_int", method_name, DISPATCH_METHOD, &argument, "|param=" + std::to_string(param));
}

void SapGuiObject::invoke_method_void(const wchar_t* method_name) {
    TraceGuard trace("SapGuiObject::invoke_method_void");
    invoke_checked("invoke_method_void", method_name, DISPATCH_METHOD, nullptr);
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

    // Fast path: invoke the universal Type DISPID and accept the answer only when it is a
    // BSTR naming a type already validated through ITypeInfo. Anything else (member not
    // found, wrong variant type, unknown name) falls through to the typeinfo path below.
    if (dispatch_) {
        DISPID universal = DISPID_UNKNOWN;
        {
            std::lock_guard<std::mutex> lock(s_dispid_cache_mutex);
            if (s_universal_type_known && !s_universal_type_disabled) universal = s_universal_type_dispid;
        }
        if (universal != DISPID_UNKNOWN) {
            DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
            _variant_t result;
            const HRESULT hr = dispatch_->Invoke(universal, IID_NULL, LOCALE_USER_DEFAULT,
                                                 DISPATCH_PROPERTYGET | DISPATCH_METHOD, &no_params, &result,
                                                 nullptr, nullptr);
            if (SUCCEEDED(hr) && result.vt == VT_BSTR && result.bstrVal) {
                std::string value = com::bstr_to_utf8(result.bstrVal);
                std::lock_guard<std::mutex> lock(s_dispid_cache_mutex);
                if (!s_universal_type_disabled && s_validated_type_names.count(value)) {
                    cached_type_ = std::move(value);
                    type_cached_ = true;
                    return cached_type_;
                }
            }
        }
    }

    // Slow path: Type resolved through ITypeInfo (resolve_dispid skips the type-first step
    // for "Type"), then used to validate the universal DISPID for this type name.
    DISPID type_dispid = DISPID_UNKNOWN;
    const bool have_dispid = dispatch_ && SUCCEEDED(resolve_dispid(L"Type", &type_dispid));
    std::string value;
    bool read_ok = false;
    if (have_dispid) {
        DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
        _variant_t result;
        const HRESULT hr = dispatch_->Invoke(type_dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                             DISPATCH_PROPERTYGET | DISPATCH_METHOD, &no_params, &result,
                                             nullptr, nullptr);
        if (SUCCEEDED(hr) && result.vt == VT_BSTR && result.bstrVal) {
            value = com::bstr_to_utf8(result.bstrVal);
            read_ok = true;
        }
    }
    if (read_ok && !value.empty()) {
        std::lock_guard<std::mutex> lock(s_dispid_cache_mutex);
        if (!s_universal_type_disabled) {
            if (!s_universal_type_known) {
                s_universal_type_dispid = type_dispid;
                s_universal_type_known = true;
                s_validated_type_names.insert(value);
            } else if (s_universal_type_dispid == type_dispid) {
                s_validated_type_names.insert(value);
            } else {
                s_universal_type_disabled = true;
                s_validated_type_names.clear();
                spdlog::warn("get_type|Type DISPID differs between types ({} vs {} for {}); "
                             "universal Type DISPID disabled", s_universal_type_dispid, type_dispid, value);
            }
        }
    }

    cached_type_ = value;
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
