#pragma once
// Scriptable fakes of SAP GUI's COM objects for the unit tests: FakeNode (properties, FindById, collections, method
// calls), FakeEnumVariant (_NewEnum) and ScopedDispatchCacheReset (the process-wide DISPID cache). One name table for
// the whole test binary, so FakeNode DISPIDs agree across test files.
#include <windows.h>
#include <oaidl.h>
#include <comdef.h>

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "include/sap_gui_base.h"

namespace fairyfly::test {

using sap::SapGuiObject;

// Minimal IEnumVARIANT over a list of dispatch pointers (for _NewEnum fakes).
class FakeEnumVariant final : public IEnumVARIANT {
public:
    explicit FakeEnumVariant(std::vector<IDispatch*> items, size_t position = 0,
                             size_t fail_after = static_cast<size_t>(-1))
        : items_(std::move(items)), position_(position), fail_after_(fail_after) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IEnumVARIANT) {
            *object = static_cast<IEnumVARIANT*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, VARIANT* out, ULONG* fetched) override {
        ULONG got = 0;
        if (position_ >= fail_after_) {
            if (fetched) *fetched = 0;
            return E_FAIL;
        }
        while (got < count && position_ < items_.size()) {
            VariantInit(&out[got]);
            out[got].vt = VT_DISPATCH;
            out[got].pdispVal = items_[position_++];
            out[got].pdispVal->AddRef();
            ++got;
        }
        if (fetched) *fetched = got;
        return got == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        position_ += count;
        if (position_ > items_.size()) { position_ = items_.size(); return S_FALSE; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { position_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumVARIANT** out) override {
        *out = new FakeEnumVariant(items_, position_, fail_after_);
        return S_OK;
    }
private:
    ULONG references_ = 1;
    std::vector<IDispatch*> items_;
    size_t position_;
    size_t fail_after_;
};

struct DispatchCacheAccess : SapGuiObject {
    using SapGuiObject::clear_dispid_cache;
};

struct ScopedDispatchCacheReset {
    ScopedDispatchCacheReset() { DispatchCacheAccess::clear_dispid_cache(); }
    ~ScopedDispatchCacheReset() { DispatchCacheAccess::clear_dispid_cache(); }
};

// Generic scriptable SAP object for ScreenReader tests: string/bool/dispatch properties by
// name, FindById lookups, collection Item/Count, Select hook, and per-property read counters.
// DISPIDs come from one global name table so the type-level DISPID cache stays consistent
// across instances.
class FakeNode final : public IDispatch {
public:
    std::map<std::wstring, std::wstring> strings;
    std::map<std::wstring, bool> bools;
    std::map<std::wstring, long> ints;
    std::map<std::wstring, IDispatch*> dispatches;
    std::map<std::wstring, IDispatch*> find_by_id;
    std::vector<IDispatch*> items;
    std::function<void()> on_select;
    std::map<std::wstring, int> reads;
    int select_calls = 0;
    // Collection round-trip counters: _NewEnum calls (served from items when enumerable) and
    // indexed Item(i) calls; count_override (>= 0) replaces the reported Count.
    bool enumerable = false;
    int new_enum_calls = 0;
    int item_calls = 0;
    long count_override = -1;
    VARTYPE count_type = VT_I4;              ///< VT_I4 or VT_I2: how Count answers
    std::vector<VARTYPE> item_index_types;   ///< the VARTYPE of each Item(i) index
    std::vector<_variant_t> values;          ///< when not empty, Item(i) answers a copy of values[i] instead of items[i]
    // Scripted Busy: while busy_true_reads > 0 each Busy read returns true and decrements it,
    // then false. -1 leaves Busy to the bools map. StartTransaction records its call time.
    int busy_true_reads = -1;
    int start_transaction_calls = 0;
    std::chrono::steady_clock::time_point start_transaction_at{};
    std::wstring start_transaction_code;   ///< StartTransaction's argument
    // GuiSession.GetObjectTree(Id, [props]): records the call, answers object_tree_payload (a UTF-16
    // string) or fails with object_tree_hresult. Names in unknown_names do not resolve a DISPID.
    int object_tree_calls = 0;
    std::wstring object_tree_id;
    std::vector<std::wstring> object_tree_props;
    bool object_tree_props_passed = false;
    std::wstring object_tree_payload;
    HRESULT object_tree_hresult = S_OK;
    std::set<std::wstring> unknown_names;
    // Property writes: recorded in `puts` and stored into strings/bools; names in put_fails answer DISP_E_EXCEPTION.
    std::vector<std::pair<std::wstring, std::wstring>> puts;
    std::set<std::wstring> put_fails;
    // Generic methods: a name in `methods` is recorded in `calls` as {name, argument text} (no argument: "") and
    // answers its `dispatches` entry (AddRef'd) when there is one; names in put_fails answer DISP_E_EXCEPTION.
    std::set<std::wstring> methods;
    std::vector<std::pair<std::wstring, std::wstring>> calls;
    std::map<std::wstring, std::wstring> method_answers;   ///< "Name(arg,arg)" -> the BSTR a generic method answers
    std::set<std::wstring> failing_calls;                  ///< "Name(arg,arg)" answers DISP_E_EXCEPTION (still recorded)
    std::map<std::wstring, _variant_t> method_values;      ///< a generic method's answer by name (any VARIANT, copied)

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDispatch) {
            *object = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count, LCID,
                                            DISPID* ids) override {
        if (!names || !ids || count != 1) return E_INVALIDARG;
        if (unknown_names.count(names[0])) return DISP_E_UNKNOWNNAME;
        auto& table = name_table();
        for (size_t i = 0; i < table.size(); ++i) {
            if (table[i] == names[0]) { *ids = static_cast<DISPID>(1000 + i); return S_OK; }
        }
        table.emplace_back(names[0]);
        *ids = static_cast<DISPID>(1000 + table.size() - 1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD flags, DISPPARAMS* params,
                                     VARIANT* result, EXCEPINFO*, UINT*) override {
        if (id == DISPID_NEWENUM && enumerable && result) {
            ++new_enum_calls;
            VariantInit(result);
            result->vt = VT_UNKNOWN;
            result->punkVal = new FakeEnumVariant(items);
            return S_OK;
        }
        const auto& table = name_table();
        if (id < 1000 || static_cast<size_t>(id - 1000) >= table.size()) return DISP_E_MEMBERNOTFOUND;
        const std::wstring& name = table[id - 1000];
        if ((flags & DISPATCH_METHOD) && name == L"Select") {
            ++select_calls;
            if (on_select) on_select();
            return S_OK;
        }
        if ((flags & DISPATCH_METHOD) && name == L"StartTransaction") {
            ++start_transaction_calls;
            if (params && params->cArgs == 1 && params->rgvarg[0].vt == VT_BSTR) start_transaction_code = params->rgvarg[0].bstrVal;
            start_transaction_at = std::chrono::steady_clock::now();
            return S_OK;
        }
        if ((flags & DISPATCH_METHOD) && name == L"GetObjectTree") {
            ++object_tree_calls;
            if (!params || params->cArgs < 1 || params->cArgs > 2) return DISP_E_BADPARAMCOUNT;
            const VARIANT& id_arg = params->rgvarg[params->cArgs - 1];
            if (id_arg.vt != VT_BSTR) return DISP_E_TYPEMISMATCH;
            object_tree_id = id_arg.bstrVal;
            object_tree_props.clear();
            object_tree_props_passed = params->cArgs == 2;
            if (object_tree_props_passed) {
                const VARIANT& props_arg = params->rgvarg[0];
                if (props_arg.vt != (VT_ARRAY | VT_VARIANT)) return DISP_E_TYPEMISMATCH;
                SAFEARRAY* array = props_arg.parray;
                LONG lower = 0, upper = -1;
                SafeArrayGetLBound(array, 1, &lower);
                SafeArrayGetUBound(array, 1, &upper);
                for (LONG i = lower; i <= upper; ++i) {
                    VARIANT element;
                    VariantInit(&element);
                    SafeArrayGetElement(array, &i, &element);
                    if (element.vt != VT_BSTR) { VariantClear(&element); return DISP_E_TYPEMISMATCH; }
                    object_tree_props.emplace_back(element.bstrVal);
                    VariantClear(&element);
                }
            }
            if (FAILED(object_tree_hresult)) return object_tree_hresult;
            if (!result) return S_OK;
            VariantInit(result);
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(object_tree_payload.c_str());
            return S_OK;
        }
        if ((flags & DISPATCH_METHOD) && name == L"FindById") {
            if (!result || !params || params->cArgs != 1 || params->rgvarg[0].vt != VT_BSTR)
                return DISP_E_BADPARAMCOUNT;
            auto it = find_by_id.find(params->rgvarg[0].bstrVal);
            if (it == find_by_id.end()) return DISP_E_EXCEPTION;
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = it->second;
            it->second->AddRef();
            return S_OK;
        }
        if ((flags & DISPATCH_METHOD) && methods.count(name)) {
            if (put_fails.count(name)) return DISP_E_EXCEPTION;
            std::wstring text;   // the arguments in reading order (COM passes them last first), joined by ","
            for (UINT i = params ? params->cArgs : 0; i-- > 0;) {
                const VARIANT& arg = params->rgvarg[i];
                if (!text.empty()) text += L",";
                if (arg.vt == VT_BSTR) text += arg.bstrVal;
                else if (arg.vt == VT_I4 || arg.vt == VT_INT) text += std::to_wstring(arg.vt == VT_INT ? arg.intVal : arg.lVal);
                else if (arg.vt == VT_BOOL) text += arg.boolVal != VARIANT_FALSE ? L"true" : L"false";
                else return DISP_E_TYPEMISMATCH;
            }
            calls.emplace_back(name, text);
            if (failing_calls.count(name + L"(" + text + L")")) return DISP_E_EXCEPTION;
            auto returned = dispatches.find(name);
            if (result && returned != dispatches.end() && returned->second) {
                VariantInit(result);
                result->vt = VT_DISPATCH;
                result->pdispVal = returned->second;
                returned->second->AddRef();
            } else if (auto value = method_values.find(name); result && value != method_values.end()) {
                VariantInit(result);
                return VariantCopy(result, &value->second);
            } else if (auto answer = method_answers.find(name + L"(" + text + L")"); result && answer != method_answers.end()) {
                VariantInit(result);
                result->vt = VT_BSTR;
                result->bstrVal = SysAllocString(answer->second.c_str());
            }
            return S_OK;
        }
        if ((flags & DISPATCH_PROPERTYPUT) && params && params->cArgs == 1) {
            if (put_fails.count(name)) return DISP_E_EXCEPTION;
            if (!params->rgdispidNamedArgs || params->cNamedArgs != 1 || params->rgdispidNamedArgs[0] != DISPID_PROPERTYPUT)
                return DISP_E_PARAMNOTOPTIONAL;   // a property put names its value DISPID_PROPERTYPUT
            const VARIANT& arg = params->rgvarg[0];
            if (arg.vt == VT_I4 || arg.vt == VT_INT) {   // _variant_t(int) is VT_INT
                ints[name] = arg.vt == VT_INT ? arg.intVal : arg.lVal;
                puts.emplace_back(name, std::to_wstring(ints[name]));
                return S_OK;
            }
            if (arg.vt == VT_BSTR) {
                strings[name] = arg.bstrVal;
                puts.emplace_back(name, std::wstring(arg.bstrVal));
            } else if (arg.vt == VT_BOOL) {
                bools[name] = arg.boolVal != VARIANT_FALSE;
                puts.emplace_back(name, std::wstring(arg.boolVal != VARIANT_FALSE ? L"true" : L"false"));
            } else {
                return DISP_E_TYPEMISMATCH;
            }
            return S_OK;
        }
        if (!result) return DISP_E_MEMBERNOTFOUND;
        if (name == L"Item" && (flags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET))) {
            ++item_calls;
            if (params && params->cArgs == 1) item_index_types.push_back(params->rgvarg[0].vt);
            const size_t size = values.empty() ? items.size() : values.size();
            if (!params || params->cArgs != 1 || params->rgvarg[0].lVal < 0 ||
                static_cast<size_t>(params->rgvarg[0].lVal) >= size)
                return DISP_E_BADINDEX;
            VariantInit(result);
            if (!values.empty()) return VariantCopy(result, &values[params->rgvarg[0].lVal]);
            result->vt = VT_DISPATCH;
            result->pdispVal = items[params->rgvarg[0].lVal];
            result->pdispVal->AddRef();
            return S_OK;
        }
        if (!(flags & DISPATCH_PROPERTYGET)) return DISP_E_MEMBERNOTFOUND;
        ++reads[name];
        VariantInit(result);
        if (name == L"Count") {
            const long count = count_override >= 0 ? count_override
                                                   : static_cast<long>(values.empty() ? items.size() : values.size());
            result->vt = count_type;
            if (count_type == VT_I2) result->iVal = static_cast<SHORT>(count);
            else result->lVal = count;
            return S_OK;
        }
        if (name == L"Busy" && busy_true_reads >= 0) {
            result->vt = VT_BOOL;
            result->boolVal = busy_true_reads > 0 ? VARIANT_TRUE : VARIANT_FALSE;
            if (busy_true_reads > 0) --busy_true_reads;
            return S_OK;
        }
        if (auto d = dispatches.find(name); d != dispatches.end()) {
            result->vt = VT_DISPATCH;
            result->pdispVal = d->second;
            d->second->AddRef();
            return S_OK;
        }
        if (auto n = ints.find(name); n != ints.end()) {
            result->vt = VT_I4;
            result->lVal = n->second;
            return S_OK;
        }
        if (auto b = bools.find(name); b != bools.end()) {
            result->vt = VT_BOOL;
            result->boolVal = b->second ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;
        }
        if (auto v = strings.find(name); v != strings.end()) {
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(v->second.c_str());
            return S_OK;
        }
        return DISP_E_MEMBERNOTFOUND;
    }

private:
    static std::vector<std::wstring>& name_table() {
        static std::vector<std::wstring> table;
        return table;
    }
    ULONG references_ = 1;
};

}  // namespace fairyfly::test
