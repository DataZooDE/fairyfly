#pragma once
// One IDispatch::Invoke with its DISPPARAMS built in one place (arguments in reading order, the named put argument).
#include <windows.h>
#include <comdef.h>

#include <algorithm>
#include <initializer_list>
#include <vector>

namespace fairyfly {
namespace sap {

/// IDispatch::Invoke of `dispid` with `args` in reading order (COM wants them last first); a DISPATCH_PROPERTYPUT names
/// its value DISPID_PROPERTYPUT. `result` (optional) receives the answer. E_POINTER without an object. Never throws.
inline HRESULT invoke(IDispatch* obj, DISPID dispid, WORD flags, std::initializer_list<_variant_t> args = {},
                      VARIANT* result = nullptr) {
    if (!obj) return E_POINTER;
    std::vector<_variant_t> reversed(args.begin(), args.end());
    std::reverse(reversed.begin(), reversed.end());
    DISPID put_name = DISPID_PROPERTYPUT;
    const bool put = flags == DISPATCH_PROPERTYPUT;
    DISPPARAMS params = {reversed.empty() ? nullptr : reinterpret_cast<VARIANT*>(reversed.data()), put ? &put_name : nullptr,
                         static_cast<UINT>(reversed.size()), put ? 1u : 0u};
    return obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result, nullptr, nullptr);
}

} // namespace sap
} // namespace fairyfly
