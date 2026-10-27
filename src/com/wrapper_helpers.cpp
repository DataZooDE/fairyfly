#include "include/text/utf8.h"
#include "include/com/wrapper_helpers.h"
#include "include/com/utf8.h"
#include "include/sap_gui_base.h"
#include <spdlog/spdlog.h>
#include <comdef.h>

namespace fairyfly {
namespace sap {

int get_collection_count_checked(IDispatch* collection) {
    if (!collection) throw ComException("SAP GUI collection is unavailable");
    DISPID dispid;
    const HRESULT lookup = get_dispid_via_typeinfo(collection, L"Count", &dispid);
    if (FAILED(lookup)) throw ComException("Cannot resolve SAP GUI collection count", lookup);
    _variant_t result;
    const HRESULT invoked = invoke(collection, dispid, DISPATCH_PROPERTYGET, {}, &result);
    if (FAILED(invoked)) throw ComException("Cannot read SAP GUI collection count", invoked);
    if (result.vt != VT_I4 && result.vt != VT_I2)
        throw ComException("SAP GUI collection count has an unexpected type");
    const int count = static_cast<int>(result);
    if (count < 0) throw ComException("SAP GUI collection count is negative");
    return count;
}

std::vector<_variant_t> collection_member_items(IDispatch* owner, const wchar_t* member, WORD flags, int limit) {
    std::vector<_variant_t> items;
    if (!owner) return items;
    DISPID member_id;
    if (FAILED(get_dispid_via_typeinfo(owner, member, &member_id))) return items;
    _variant_t answer;
    if (FAILED(invoke(owner, member_id, flags, {}, &answer)) || answer.vt != VT_DISPATCH || !answer.pdispVal)
        return items;
    IDispatch* collection = answer.pdispVal;   // `answer` keeps it alive
    DISPID count_id, item_id;
    if (FAILED(get_dispid_via_typeinfo(collection, L"Count", &count_id))) return items;
    _variant_t count;
    if (FAILED(invoke(collection, count_id, DISPATCH_PROPERTYGET, {}, &count)) || count.vt != VT_I4) return items;
    if (FAILED(get_dispid_via_typeinfo(collection, L"Item", &item_id))) return items;
    for (long i = 0; i < count.lVal && i < limit; ++i) {
        _variant_t item;
        if (SUCCEEDED(invoke(collection, item_id, DISPATCH_METHOD, {_variant_t(i)}, &item)))
            items.push_back(std::move(item));
    }
    return items;
}

// Helper function for AddRef validation with SEH (no C++ objects allowed)
static ULONG safe_addref_check(IDispatch* obj) {
    __try {
        return obj->AddRef();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;  // Access violation means stale pointer
    }
}

// Helper function for GetTypeInfo with SEH (no C++ objects allowed)
static HRESULT safe_get_typeinfo(IDispatch* obj, ITypeInfo** type_info) {
    __try {
        return obj->GetTypeInfo(0, LOCALE_USER_DEFAULT, type_info);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;  // Access violation
    }
}

// Helper function for GetIDsOfNames with SEH (no C++ objects allowed)
static HRESULT safe_get_ids_of_names(IDispatch* obj, LPOLESTR* name, DISPID* dispid) {
    __try {
        return obj->GetIDsOfNames(IID_NULL, name, 1, LOCALE_USER_DEFAULT, dispid);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;  // Access violation
    }
}

/// Get DISPID for a method/property name using ITypeInfo (works better than IDispatch::GetIDsOfNames)
/// This is the CRITICAL FIX for SAP GUI scripting - IDispatch::GetIDsOfNames fails but ITypeInfo works
/// ENHANCED: Now includes SEH to catch access violations from stale COM pointers
HRESULT get_dispid_via_typeinfo(IDispatch* obj, const wchar_t* name, DISPID* dispid) {
    if (!obj || !name || !dispid) return E_POINTER;

    // Validate COM pointer using AddRef/Release before dereferencing
    // This detects destroyed objects (AddRef returns 0 for freed objects)
    ULONG ref_count = safe_addref_check(obj);
    if (ref_count == 0) {
        spdlog::error("get_dispid_via_typeinfo: Access violation during AddRef - stale COM pointer for '{}'",
                     text::wide_to_utf8_lossy(name));
        return E_FAIL;
    }
    obj->Release();  // Restore ref count

    // Try ITypeInfo first (this works for SAP GUI)
    // Wrapped in SEH to catch access violations from stale pointers
    ITypeInfo* type_info = nullptr;
    HRESULT hr = safe_get_typeinfo(obj, &type_info);
    if (hr == E_FAIL) {
        spdlog::error("get_dispid_via_typeinfo: Access violation calling GetTypeInfo for '{}' - stale COM pointer detected",
                     text::wide_to_utf8_lossy(name));
        return E_FAIL;
    }

    if (SUCCEEDED(hr) && type_info) {
        LPOLESTR names[1] = { const_cast<LPOLESTR>(name) };
        hr = type_info->GetIDsOfNames(names, 1, dispid);
        type_info->Release();
        if (SUCCEEDED(hr)) return S_OK;
    }

    // Fallback to IDispatch::GetIDsOfNames
    LPOLESTR name_ptr = const_cast<LPOLESTR>(name);
    hr = safe_get_ids_of_names(obj, &name_ptr, dispid);
    if (hr == E_FAIL) {
        spdlog::error("get_dispid_via_typeinfo: Access violation calling GetIDsOfNames for '{}' - stale COM pointer detected",
                     text::wide_to_utf8_lossy(name));
        return E_FAIL;
    }

    return hr;
}

// Helper function to validate BSTR pointer with SEH (no C++ objects allowed in __try)
// Returns 1 if BSTR is valid and can be accessed, 0 if access violation occurs
static int validate_bstr_seh(BSTR bstr, wchar_t* first_char_out) {
    __try {
        if (bstr == nullptr) {
            return 0;
        }
        // Try to read the first character to validate the pointer
        *first_char_out = bstr[0];
        return 1;  // Success
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Access violation during memory access
        return 0;
    }
}

/// Safely convert VARIANT with BSTR to string, with robust NULL pointer and access violation handling
/// This prevents segfaults from NULL BSTR pointers or invalid memory access
std::string safe_bstr_to_string(const VARIANT& var, const char* context) {
    // Validate VARIANT type
    if (var.vt != VT_BSTR) {
        if (context) {
            spdlog::debug("safe_bstr_to_string|wrong_variant_type|context={}|vt={}|expected=VT_BSTR({})",
                         context, (int)var.vt, (int)VT_BSTR);
        }
        return "";
    }

    // Check for NULL BSTR (this is valid for empty strings in some COM implementations)
    if (var.bstrVal == nullptr) {
        if (context) {
            spdlog::debug("safe_bstr_to_string|null_bstr|context={}|returning_empty_string", context);
        }
        return "";
    }

    // Validate BSTR pointer using SEH before attempting conversion
    wchar_t first_char = 0;
    int is_valid = validate_bstr_seh(var.bstrVal, &first_char);

    if (!is_valid) {
        // Access violation when trying to read BSTR - it's a stale/invalid pointer
        spdlog::error("safe_bstr_to_string|access_violation|context={}|bstr_ptr={}",
                     context ? context : "unknown",
                     (void*)var.bstrVal);
        return "";
    }

    // BSTR is valid, now safely convert it to string using _bstr_t
    try {
        return com::bstr_to_utf8(var.bstrVal);
    } catch (const std::exception& e) {
        spdlog::error("safe_bstr_to_string|conversion_exception|context={}|error={}",
                     context ? context : "unknown", e.what());
        return "";
    }
}

HRESULT safe_invoke(IDispatch* obj, DISPID dispid, WORD flags, DISPPARAMS* params, VARIANT* result) {
    if (!obj) return E_POINTER;
    __try {
        return obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, params, result, nullptr, nullptr);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

} // namespace sap
} // namespace fairyfly

