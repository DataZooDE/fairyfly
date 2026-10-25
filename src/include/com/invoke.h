#pragma once
// One IDispatch::Invoke with its DISPPARAMS built in one place (arguments in reading order, the named put argument).
#include <windows.h>
#include <comdef.h>

#include <algorithm>
#include <initializer_list>
#include <vector>

namespace fairyfly {
namespace sap {

/// The DISPPARAMS of one call: `args` in reading order (COM wants them last first); a DISPATCH_PROPERTYPUT names its
/// value DISPID_PROPERTYPUT. Owns the argument copies for as long as it lives.
class DispArgs {
public:
    DispArgs(WORD flags, std::initializer_list<_variant_t> args) : reversed_(args.begin(), args.end()) {
        std::reverse(reversed_.begin(), reversed_.end());
        const bool put = flags == DISPATCH_PROPERTYPUT;
        params_ = {reversed_.empty() ? nullptr : reinterpret_cast<VARIANT*>(reversed_.data()), put ? &put_name_ : nullptr,
                   static_cast<UINT>(reversed_.size()), put ? 1u : 0u};
    }
    DispArgs(const DispArgs&) = delete;
    DispArgs& operator=(const DispArgs&) = delete;
    DISPPARAMS* get() { return &params_; }

private:
    std::vector<_variant_t> reversed_;
    DISPID put_name_ = DISPID_PROPERTYPUT;
    DISPPARAMS params_{};
};

/// IDispatch::Invoke of `dispid` with `args` in reading order. `result` (optional) receives the answer. E_POINTER
/// without an object. Never throws.
inline HRESULT invoke(IDispatch* obj, DISPID dispid, WORD flags, std::initializer_list<_variant_t> args = {},
                      VARIANT* result = nullptr) {
    if (!obj) return E_POINTER;
    DispArgs params(flags, args);
    return obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, params.get(), result, nullptr, nullptr);
}

} // namespace sap
} // namespace fairyfly
