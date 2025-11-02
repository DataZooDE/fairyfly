#include "include/com/wrapper_helpers.h"
#include "include/sap_gui_base.h"

namespace fairyfly {
namespace sap {

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

} // namespace sap
} // namespace fairyfly

