#include <catch2/catch_test_macros.hpp>
#include <spdlog/spdlog.h>
#include "include/com/wrapper.h"
#include "include/com/wrapper_helpers.h"
#include "include/collection_id_lookup.h"
#include "include/automation_engine.h"
#include "include/com_automation_engine.h"
#include "include/cli_entry.h"
#include "include/element_metadata_extractor.h"
#include "include/table_data_extractor.h"
#include "include/screen_reader.h"
#include <nlohmann/json.hpp>
#include <exception>
#include <iostream>
#include <sstream>
#include <thread>
#include <map>
#include <string>
#include <vector>
#include <cwchar>

using namespace fairyfly;
using namespace fairyfly::sap;

namespace {
class TextFieldDispatch final : public IDispatch {
public:
    TextFieldDispatch(const wchar_t* type, DISPID type_id,
                      const wchar_t* id = L"", const wchar_t* label = L"",
                      const wchar_t* subtype = L"", const wchar_t* value = L"secret-from-com")
        : type_(type), type_id_(type_id), id_(id), label_(label), subtype_(subtype), value_(value) {}

    int text_reads = 0;
    int cell_value_lookups = 0;
    int toolbar_id_lookups = 0;
    int toolbar_tooltip_lookups = 0;
    int tree_node_doubleclicks = 0;
    std::vector<std::wstring> selected_tree_nodes = {L"stale-node"};
    std::vector<std::string> tree_selection_calls;
    int tree_item_doubleclicks = 0;
    int editor_line_reads = 0;
    bool tree_node_has_text = false;
    bool count_fails = false;
    bool count_requires_property_get = false;
    int count_value = 1;
    bool selected = false;
    IDispatch* connections_dispatch = nullptr;
    IDispatch* left_label_dispatch = nullptr;
    IDispatch* parent_dispatch = nullptr;
    IDispatch* named_sibling_dispatch = nullptr;
    const wchar_t* named_sibling_id = L"txtIP_HEADER_NAME";
    IDispatch* children_dispatch = nullptr;
    std::vector<IDispatch*> child_items;

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
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count,
                                            LCID, DISPID* ids) override {
        if (!names || !ids || count != 1) return E_INVALIDARG;
        if (std::wcscmp(names[0], L"Type") == 0) *ids = type_id_;
        else if (std::wcscmp(names[0], L"DisplayedText") == 0) *ids = type_id_ + 1;
        else if (std::wcscmp(names[0], L"Text") == 0) *ids = type_id_ + 2;
        else if (std::wcscmp(names[0], L"Id") == 0) *ids = type_id_ + 3;
        else if (std::wcscmp(names[0], L"AccLabel") == 0) *ids = type_id_ + 4;
        else if (std::wcscmp(names[0], L"FirstVisibleColumn") == 0) *ids = type_id_ + 5;
        else if (std::wcscmp(names[0], L"FirstVisibleRow") == 0) *ids = type_id_ + 6;
        else if (std::wcscmp(names[0], L"CurrentCellColumn") == 0) *ids = type_id_ + 7;
        else if (std::wcscmp(names[0], L"CurrentCellRow") == 0) *ids = type_id_ + 8;
        else if (std::wcscmp(names[0], L"GetCellValue") == 0) {
            ++cell_value_lookups;
            *ids = type_id_ + 9;
        }
        else if (std::wcscmp(names[0], L"GetToolbarButtonId") == 0) {
            ++toolbar_id_lookups;
            *ids = type_id_ + 10;
        }
        else if (std::wcscmp(names[0], L"GetToolbarButtonTooltip") == 0) {
            ++toolbar_tooltip_lookups;
            *ids = type_id_ + 11;
        }
        else if (std::wcscmp(names[0], L"GetNodeTextByKey") == 0) *ids = type_id_ + 12;
        else if (std::wcscmp(names[0], L"GetColumnNames") == 0) *ids = type_id_ + 13;
        else if (std::wcscmp(names[0], L"GetItemText") == 0) *ids = type_id_ + 14;
        else if (std::wcscmp(names[0], L"Count") == 0) *ids = type_id_ + 15;
        else if (std::wcscmp(names[0], L"Item") == 0) *ids = type_id_ + 16;
        else if (std::wcscmp(names[0], L"DoubleClickNode") == 0) *ids = type_id_ + 17;
        else if (std::wcscmp(names[0], L"DoubleClickItem") == 0) *ids = type_id_ + 18;
        else if (std::wcscmp(names[0], L"GetLineCount") == 0) *ids = type_id_ + 19;
        else if (std::wcscmp(names[0], L"GetLineText") == 0) *ids = type_id_ + 20;
        else if (std::wcscmp(names[0], L"SubType") == 0) *ids = type_id_ + 21;
        else if (std::wcscmp(names[0], L"Connections") == 0) *ids = type_id_ + 22;
        else if (std::wcscmp(names[0], L"LeftLabel") == 0) *ids = type_id_ + 23;
        else if (std::wcscmp(names[0], L"Parent") == 0) *ids = type_id_ + 24;
        else if (std::wcscmp(names[0], L"FindById") == 0) *ids = type_id_ + 25;
        else if (std::wcscmp(names[0], L"Children") == 0) *ids = type_id_ + 26;
        else if (std::wcscmp(names[0], L"Selected") == 0) *ids = type_id_ + 27;
        else if (std::wcscmp(names[0], L"Visible") == 0) *ids = type_id_ + 28;
        else if (std::wcscmp(names[0], L"Changeable") == 0) *ids = type_id_ + 29;
        else if (std::wcscmp(names[0], L"UnselectAll") == 0) *ids = type_id_ + 30;
        else if (std::wcscmp(names[0], L"SelectNode") == 0) *ids = type_id_ + 31;
        else return DISP_E_UNKNOWNNAME;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD flags,
                                     DISPPARAMS* params, VARIANT* result, EXCEPINFO*, UINT*) override {
        if (id == type_id_ + 17 && (flags & DISPATCH_METHOD)) {
            ++tree_node_doubleclicks;
            return S_OK;
        }
        if (id == type_id_ + 18 && (flags & DISPATCH_METHOD)) {
            ++tree_item_doubleclicks;
            return S_OK;
        }
        if (id == type_id_ + 30 && (flags & DISPATCH_METHOD)) {
            selected_tree_nodes.clear();
            tree_selection_calls.push_back("UnselectAll");
            return S_OK;
        }
        if (id == type_id_ + 31 && (flags & DISPATCH_METHOD)) {
            if (!params || params->cArgs != 1 || params->rgvarg[0].vt != VT_BSTR)
                return DISP_E_BADPARAMCOUNT;
            selected_tree_nodes.emplace_back(params->rgvarg[0].bstrVal);
            tree_selection_calls.push_back("SelectNode");
            return S_OK;
        }
        if (!result) return DISP_E_MEMBERNOTFOUND;
        if (id == type_id_ + 22 && (flags & DISPATCH_PROPERTYGET) && connections_dispatch) {
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = connections_dispatch;
            connections_dispatch->AddRef();
            return S_OK;
        }
        if (id == type_id_ + 23 && (flags & DISPATCH_PROPERTYGET) && left_label_dispatch) {
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = left_label_dispatch;
            left_label_dispatch->AddRef();
            return S_OK;
        }
        if (id == type_id_ + 24 && (flags & DISPATCH_PROPERTYGET) && parent_dispatch) {
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = parent_dispatch;
            parent_dispatch->AddRef();
            return S_OK;
        }
        if (id == type_id_ + 25 && (flags & DISPATCH_METHOD) && named_sibling_dispatch) {
            if (!params || params->cArgs != 1 || params->rgvarg[0].vt != VT_BSTR ||
                std::wcscmp(params->rgvarg[0].bstrVal, named_sibling_id) != 0)
                return DISP_E_BADPARAMCOUNT;
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = named_sibling_dispatch;
            named_sibling_dispatch->AddRef();
            return S_OK;
        }
        if (id == type_id_ + 26 && (flags & DISPATCH_PROPERTYGET) && children_dispatch) {
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = children_dispatch;
            children_dispatch->AddRef();
            return S_OK;
        }
        if (id == type_id_ + 16 && (flags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET)) &&
            !child_items.empty()) {
            if (!params || params->cArgs != 1 ||
                (params->rgvarg[0].vt != VT_I4 && params->rgvarg[0].vt != VT_INT) ||
                params->rgvarg[0].lVal < 0 ||
                static_cast<size_t>(params->rgvarg[0].lVal) >= child_items.size())
                return DISP_E_BADINDEX;
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = child_items[params->rgvarg[0].lVal];
            result->pdispVal->AddRef();
            return S_OK;
        }
        if (id == type_id_ + 19 && (flags & DISPATCH_METHOD)) {
            VariantInit(result);
            result->vt = VT_I4;
            result->lVal = 3;
            return S_OK;
        }
        if (id == type_id_ + 20 && (flags & DISPATCH_METHOD)) {
            if (!params || params->cArgs != 1 || params->rgvarg[0].vt != VT_I4)
                return DISP_E_BADPARAMCOUNT;
            ++editor_line_reads;
            const int line = params->rgvarg[0].lVal;
            if (line < 1 || line > 3) return DISP_E_BADINDEX;
            VariantInit(result);
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(line == 1 ? L"FORM SET_SCREEN_0300." :
                                             line == 2 ? L"  lv_name = iv_name." : L"ENDFORM.");
            return result->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        if (id == type_id_ + 13 && (flags & DISPATCH_METHOD)) {
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = this;
            AddRef();
            return S_OK;
        }
        if (id == type_id_ + 15 && (flags & DISPATCH_PROPERTYGET)) {
            if (count_requires_property_get && flags != DISPATCH_PROPERTYGET) return DISP_E_MEMBERNOTFOUND;
            if (count_fails) return E_FAIL;
            VariantInit(result);
            result->vt = VT_I4;
            result->lVal = child_items.empty() ? count_value :
                static_cast<long>(child_items.size());
            return S_OK;
        }
        if (id >= type_id_ + 12 && id <= type_id_ + 16 && (flags & DISPATCH_METHOD)) {
            VariantInit(result);
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(id == type_id_ + 12 ?
                                             (tree_node_has_text ? L"Simple node" : L"") :
                                             id == type_id_ + 14 ? L"Users by Address Data" : L"TEXT");
            return result->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        if (id >= type_id_ + 9 && id <= type_id_ + 11 && (flags & DISPATCH_METHOD)) {
            VariantInit(result);
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(id == type_id_ + 9 ? L"grid-cell" :
                                             id == type_id_ + 10 ? L"BUTTON" : L"Tooltip");
            return result->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        if (!(flags & DISPATCH_PROPERTYGET)) return DISP_E_MEMBERNOTFOUND;
        VariantInit(result);
        if (id >= type_id_ + 27 && id <= type_id_ + 29) {
            result->vt = VT_BOOL;
            result->boolVal = id == type_id_ + 27 && !selected ? VARIANT_FALSE : VARIANT_TRUE;
            return S_OK;
        }
        if (id == type_id_ + 6 || id == type_id_ + 8) {
            result->vt = VT_I4;
            result->lVal = (id == type_id_ + 6) ? 3 : 40;
            return S_OK;
        }
        result->vt = VT_BSTR;
        if (id == type_id_) result->bstrVal = SysAllocString(type_);
        else if (id == type_id_ + 1 || id == type_id_ + 2) {
            ++text_reads;
            result->bstrVal = SysAllocString(value_);
        } else if (id == type_id_ + 3) result->bstrVal = SysAllocString(id_);
        else if (id == type_id_ + 4) result->bstrVal = SysAllocString(label_);
        else if (id == type_id_ + 5) result->bstrVal = SysAllocString(L"NAMESPACE");
        else if (id == type_id_ + 7) result->bstrVal = SysAllocString(L"PROCESS_MODE_TEXT");
        else if (id == type_id_ + 21) result->bstrVal = SysAllocString(subtype_);
        else return DISP_E_MEMBERNOTFOUND;
        return result->bstrVal ? S_OK : E_OUTOFMEMORY;
    }

private:
    ULONG references_ = 1;
    const wchar_t* type_;
    DISPID type_id_;
    const wchar_t* id_;
    const wchar_t* label_;
    const wchar_t* subtype_;
    const wchar_t* value_;
};

// Minimal read-only dispatch that serves string properties by name.
class StringPropertyDispatch final : public IDispatch {
public:
    explicit StringPropertyDispatch(std::map<std::wstring, std::wstring> values)
        : values_(std::move(values)) {
        for (const auto& entry : values_) names_.push_back(entry.first);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDispatch) return E_NOINTERFACE;
        *object = static_cast<IDispatch*>(this);
        AddRef();
        return S_OK;
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
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count,
                                            LCID, DISPID* ids) override {
        if (!names || !ids || count != 1) return E_INVALIDARG;
        for (size_t i = 0; i < names_.size(); ++i) {
            if (names_[i] == names[0]) {
                *ids = static_cast<DISPID>(7000 + i);
                return S_OK;
            }
        }
        return DISP_E_UNKNOWNNAME;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD flags,
                                     DISPPARAMS*, VARIANT* result, EXCEPINFO*, UINT*) override {
        if (!result || !(flags & DISPATCH_PROPERTYGET)) return DISP_E_MEMBERNOTFOUND;
        const auto index = static_cast<size_t>(id - 7000);
        if (index >= names_.size()) return DISP_E_MEMBERNOTFOUND;
        VariantInit(result);
        result->vt = VT_BSTR;
        result->bstrVal = SysAllocString(values_.at(names_[index]).c_str());
        return result->bstrVal ? S_OK : E_OUTOFMEMORY;
    }
private:
    ULONG references_ = 1;
    std::map<std::wstring, std::wstring> values_;
    std::vector<std::wstring> names_;
};

class ReadOnlyFieldDispatch final : public IDispatch {
public:
    explicit ReadOnlyFieldDispatch(bool changeable = false, bool has_changeable = true)
        : changeable_(changeable), has_changeable_(has_changeable) {}
    int text_writes = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDispatch) return E_NOINTERFACE;
        *object = static_cast<IDispatch*>(this);
        AddRef();
        return S_OK;
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
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count,
                                           LCID, DISPID* ids) override {
        if (!names || !ids || count != 1) return E_INVALIDARG;
        if (std::wcscmp(names[0], L"Type") == 0) *ids = 71;
        else if (std::wcscmp(names[0], L"Changeable") == 0 && has_changeable_) *ids = 72;
        else if (std::wcscmp(names[0], L"Text") == 0) *ids = 73;
        else return DISP_E_UNKNOWNNAME;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD flags,
                                     DISPPARAMS*, VARIANT* result, EXCEPINFO*, UINT*) override {
        if (id == 73 && (flags & DISPATCH_PROPERTYPUT)) {
            ++text_writes;
            return changeable_ ? S_OK : DISP_E_EXCEPTION;
        }
        if (!(flags & DISPATCH_PROPERTYGET) || !result) return DISP_E_MEMBERNOTFOUND;
        VariantInit(result);
        if (id == 72 && has_changeable_) {
            result->vt = VT_BOOL;
            result->boolVal = changeable_ ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;
        }
        if (id == 71) {
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(L"GuiTextField");
            return result->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        return DISP_E_MEMBERNOTFOUND;
    }
private:
    ULONG references_ = 1;
    bool changeable_;
    bool has_changeable_;
};

class DualDispatchObject final : public IUnknown {
public:
    class Facet final : public IDispatch {
    public:
        explicit Facet(DualDispatchObject& owner) : owner_(owner) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
            if (!object) return E_POINTER;
            *object = nullptr;
            if (iid == IID_IUnknown) *object = static_cast<IUnknown*>(&owner_);
            else if (iid == IID_IDispatch) *object = static_cast<IDispatch*>(this);
            else return E_NOINTERFACE;
            owner_.AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return owner_.AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return owner_.Release(); }
        HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
            if (!count) return E_POINTER;
            *count = 0;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
            return DISP_E_UNKNOWNNAME;
        }
        HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*,
                                         EXCEPINFO*, UINT*) override { return DISP_E_MEMBERNOTFOUND; }
    private:
        DualDispatchObject& owner_;
    };

    DualDispatchObject() : first_(*this), second_(*this) {}
    IDispatch* first() { return &first_; }
    IDispatch* second() { return &second_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        *object = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
private:
    ULONG references_ = 1;
    Facet first_;
    Facet second_;
};

class SessionIdentityDispatch final : public IDispatch {
public:
    explicit SessionIdentityDispatch(bool info) : info_(info) {
        if (!info_) child_ = new SessionIdentityDispatch(true);
    }
    ~SessionIdentityDispatch() {
        if (child_) child_->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDispatch) return E_NOINTERFACE;
        *object = static_cast<IDispatch*>(this);
        AddRef();
        return S_OK;
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
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count,
                                            LCID, DISPID* ids) override {
        if (!names || !ids || count != 1) return E_INVALIDARG;
        if (!info_ && std::wcscmp(names[0], L"Info") == 0) *ids = 1;
        else if (info_ && std::wcscmp(names[0], L"SystemSessionId") == 0) *ids = 2;
        else if (info_ && std::wcscmp(names[0], L"SessionNumber") == 0) *ids = 3;
        else return DISP_E_UNKNOWNNAME;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD flags,
                                     DISPPARAMS*, VARIANT* result, EXCEPINFO*, UINT*) override {
        if (!(flags & DISPATCH_PROPERTYGET) || !result) return DISP_E_MEMBERNOTFOUND;
        VariantInit(result);
        if (!info_ && id == 1) {
            child_->AddRef();
            result->vt = VT_DISPATCH;
            result->pdispVal = child_;
        } else if (info_ && id == 2) {
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(L"BACKEND-42");
        } else if (info_ && id == 3) {
            result->vt = VT_I4;
            result->lVal = 2;
        } else return DISP_E_MEMBERNOTFOUND;
        return S_OK;
    }
private:
    ULONG references_ = 1;
    bool info_;
    SessionIdentityDispatch* child_ = nullptr;
};

struct DispatchCacheAccess : SapGuiObject {
    using SapGuiObject::clear_dispid_cache;
};

struct ScopedDispatchCacheReset {
    ScopedDispatchCacheReset() { DispatchCacheAccess::clear_dispid_cache(); }
    ~ScopedDispatchCacheReset() { DispatchCacheAccess::clear_dispid_cache(); }
};

struct ForcedCliResult {
    bool mta_initialized = false;
    int exit_code = -1;
    std::string output;
    std::exception_ptr failure;
};

ForcedCliResult run_cli_on_mta(std::vector<std::string> args) {
    ForcedCliResult result;
    std::thread worker([&] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) return;
        result.mta_initialized = true;
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        std::ostringstream captured;
        auto* previous = std::cout.rdbuf(captured.rdbuf());
        try {
            result.exit_code = cli::run_cli(static_cast<int>(argv.size()), argv.data());
        } catch (...) {
            result.failure = std::current_exception();
        }
        std::cout.rdbuf(previous);
        result.output = captured.str();
        CoUninitialize();
    });
    worker.join();
    return result;
}
} // namespace

TEST_CASE("COM wrappers compare controlling IUnknown identity", "[com][identity]") {
    auto* object = new DualDispatchObject();
    {
        SapGuiObject first(IDispatchPtr(object->first()));
        SapGuiObject second(IDispatchPtr(object->second()));
        REQUIRE(first.get_dispatch() != second.get_dispatch());
        REQUIRE(first == second);
    }
    object->Release();
}

TEST_CASE("SAP session key includes backend connection and session number", "[com][identity]") {
    ComGuiSession session(IDispatchPtr(new SessionIdentityDispatch(false), true));
    REQUIRE(session.get_server_session_key() == "BACKEND-42:2");
}

TEST_CASE("Screen metadata reports selected radio and checkbox state", "[com][metadata]") {
    for (const auto* type : {L"GuiRadioButton", L"GuiCheckBox"}) {
        auto* dispatch = new TextFieldDispatch(type, 9400, L"/app/con[0]/ses[0]/wnd[0]/usr/radTEST");
        auto element = ComGuiElement::create(IDispatchPtr(dispatch, true));
        dispatch->selected = true;
        const auto checked = ElementMetadataExtractor::extract(element);
        REQUIRE(checked.at("selected") == true);
        dispatch->selected = false;
        const auto unchecked = ElementMetadataExtractor::extract(element);
        REQUIRE(unchecked.at("selected") == false);
    }
}

TEST_CASE("SAP collection lookup uses object ID after connection indices become sparse", "[com][identity]") {
    const std::vector<std::string> live_ids = {"/app/con[1]"};
    const auto index = find_collection_index_by_id(
        static_cast<int>(live_ids.size()), "/app/con[1]",
        [&](int position) { return live_ids.at(position); });
    REQUIRE(index == 0);

    const std::vector<std::string> session_ids = {"/app/con[1]/ses[2]"};
    const auto session_index = find_collection_index_by_id(
        static_cast<int>(session_ids.size()), "/app/con[1]/ses[2]",
        [&](int position) { return session_ids.at(position); });
    REQUIRE(session_index == 0);

    const auto after_stale_item = find_collection_index_by_id(
        2, "/app/con[1]", [&](int position) -> std::string {
            if (position == 0) throw std::runtime_error("closed connection");
            return "/app/con[1]";
        });
    REQUIRE(after_stale_item == 1);
}

TEST_CASE("Close verification does not turn a failed COM count into zero sessions", "[com][lifecycle]") {
    auto* collection = new TextFieldDispatch(L"Collection", 9100);
    IDispatchPtr holder(collection, true);
    collection->count_requires_property_get = true;
    REQUIRE(get_collection_count_checked(holder) == 1);
    collection->count_fails = true;
    REQUIRE_THROWS_AS(get_collection_count_checked(holder), ComException);
}

TEST_CASE("Checked session observation distinguishes COM failure from absence", "[com][lifecycle]") {
    auto* root = new TextFieldDispatch(L"Application", 9200);
    auto* collection = new TextFieldDispatch(L"Collection", 9300);
    root->connections_dispatch = collection;
    IDispatchPtr root_holder(root, true);
    root->Release();
    auto app = std::make_shared<ComGuiApplication>(root_holder);
    collection->count_fails = true;
    REQUIRE_THROWS_AS(session_present_checked(app, "/app/con[0]/ses[0]", "key:0"), ComException);
    collection->count_fails = false;
    collection->count_value = 0;
    REQUIRE_FALSE(session_present_checked(app, "/app/con[0]/ses[0]", "key:0"));
    collection->Release();
}

TEST_CASE("ComException creation and properties", "[com][exception]") {
    SECTION("ComException stores message") {
        ComException ex("Test error");
        REQUIRE(std::string(ex.what()) == "Test error");
    }

    SECTION("ComException stores HRESULT") {
        HRESULT hr = 0x80004005;
        ComException ex("Test", hr);
        REQUIRE(ex.hresult() == hr);
    }

    SECTION("ComException can be caught as std::runtime_error") {
        REQUIRE_THROWS_AS(throw ComException("Test"), std::runtime_error);
    }
}

TEST_CASE("Password field redaction uses the correct COM type", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* password_dispatch = new TextFieldDispatch(L"GuiPasswordField", 11);
    auto password = ComGuiElement::create(password_dispatch);
    password_dispatch->Release();
    REQUIRE(password->get_text() == "[REDACTED]");
    REQUIRE(password_dispatch->text_reads == 0);

    auto* ordinary_dispatch = new TextFieldDispatch(L"GuiTextField", 21);
    auto ordinary = ComGuiElement::create(ordinary_dispatch);
    ordinary_dispatch->Release();
    REQUIRE(ordinary->get_text() == "secret-from-com");
    REQUIRE(ordinary_dispatch->text_reads == 1);
}

TEST_CASE("Named ordinary input fields never read secret text", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* named_dispatch = new TextFieldDispatch(
        L"GuiTextField", 41, L"wnd[0]/usr/txtS_API_KEY");
    auto named = ComGuiElement::create(named_dispatch);
    named_dispatch->Release();
    REQUIRE(named->get_text() == "[REDACTED]");
    REQUIRE(named_dispatch->text_reads == 0);

    auto* labeled_dispatch = new TextFieldDispatch(
        L"GuiTextField", 41, L"wnd[0]/usr/txtGENERIC", L"Client Secret");
    auto labeled = ComGuiElement::create(labeled_dispatch);
    labeled_dispatch->Release();
    REQUIRE(labeled->get_text() == "[REDACTED]");
    REQUIRE(labeled_dispatch->text_reads == 0);
}

TEST_CASE("SAP assigned left label redacts a neutral input field", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* caption_dispatch = new TextFieldDispatch(
        L"GuiTextField", 9400, L"wnd[0]/usr/txtGV_CAP", L"", L"", L"Client Secret");
    auto* field_dispatch = new TextFieldDispatch(
        L"GuiTextField", 9400, L"wnd[0]/usr/txtP_VALUE", L"", L"", L"synthetic-sensitive-marker");
    field_dispatch->left_label_dispatch = caption_dispatch;
    auto field = ComGuiElement::create(field_dispatch);
    field_dispatch->Release();

    REQUIRE(field->get_label() == "Client Secret");
    REQUIRE(field->get_text() == "[REDACTED]");
    REQUIRE(field_dispatch->text_reads == 0);
    caption_dispatch->Release();
}

TEST_CASE("Authorization object metadata remains readable", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* metadata_dispatch = new TextFieldDispatch(
        L"GuiCTextField", 9450, L"wnd[0]/usr/ctxtTSTCA-OBJCT",
        L"Authorization Object", L"", L"S_DEVELOP");
    auto metadata = ComGuiElement::create(metadata_dispatch);
    metadata_dispatch->Release();
    REQUIRE(metadata->get_text() == "S_DEVELOP");
    REQUIRE(metadata_dispatch->text_reads == 1);

    auto* credential_dispatch = new TextFieldDispatch(
        L"GuiTextField", 9460, L"wnd[0]/usr/txtVALUE",
        L"Authorization", L"", L"Bearer synthetic-secret");
    auto credential = ComGuiElement::create(credential_dispatch);
    credential_dispatch->Release();
    REQUIRE(credential->get_text() == "[REDACTED]");
    REQUIRE(credential_dispatch->text_reads == 0);
}

TEST_CASE("Authorization group metadata remains readable", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* group_dispatch = new TextFieldDispatch(
        L"GuiCTextField", 9470, L"wnd[0]/usr/ctxtTDDAT-CCLASS",
        L"Authorization Group", L"", L"&NC&");
    auto group = ComGuiElement::create(group_dispatch);
    group_dispatch->Release();
    REQUIRE(group->get_text() == "&NC&");

    auto* credential_dispatch = new TextFieldDispatch(
        L"GuiTextField", 9480, L"wnd[0]/usr/txtVALUE",
        L"Authorization", L"", L"Bearer synthetic-secret");
    auto credential = ComGuiElement::create(credential_dispatch);
    credential_dispatch->Release();
    REQUIRE(credential->get_text() == "[REDACTED]");
}

TEST_CASE("Gateway header value follows its sibling credential name", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    SECTION("credential name hides the value without reading it") {
        auto* parent_dispatch = new TextFieldDispatch(L"GuiUserArea", 9500);
        auto* name_dispatch = new TextFieldDispatch(
            L"GuiTextField", 9600, L"wnd[1]/usr/txtIP_HEADER_NAME", L"", L"", L"X-Session-Token");
        auto* value_dispatch = new TextFieldDispatch(
            L"GuiTextField", 9700, L"wnd[1]/usr/txtIP_HEADER_VALUE", L"", L"", L"synthetic-header-marker");
        parent_dispatch->named_sibling_dispatch = name_dispatch;
        value_dispatch->parent_dispatch = parent_dispatch;
        auto value = ComGuiElement::create(value_dispatch);
        value_dispatch->Release();

        REQUIRE(value->get_text() == "[REDACTED]");
        REQUIRE(value_dispatch->text_reads == 0);

        name_dispatch->Release();
        parent_dispatch->Release();
    }
    SECTION("ordinary header value remains readable") {
        auto* parent_dispatch = new TextFieldDispatch(L"GuiUserArea", 9800);
        auto* name_dispatch = new TextFieldDispatch(
            L"GuiTextField", 9900, L"wnd[1]/usr/txtIP_HEADER_NAME", L"", L"", L"Content-Type");
        auto* value_dispatch = new TextFieldDispatch(
            L"GuiTextField", 10000, L"wnd[1]/usr/txtIP_HEADER_VALUE", L"", L"", L"application/json");
        auto* children_dispatch = new TextFieldDispatch(L"GuiCollection", 10050);
        children_dispatch->child_items = {name_dispatch, value_dispatch};
        parent_dispatch->named_sibling_dispatch = name_dispatch;
        parent_dispatch->children_dispatch = children_dispatch;
        value_dispatch->parent_dispatch = parent_dispatch;
        auto value = ComGuiElement::create(value_dispatch);
        value_dispatch->Release();

        REQUIRE(value->get_text() == "application/json");
        REQUIRE(value_dispatch->text_reads == 1);

        name_dispatch->Release();
        children_dispatch->Release();
        parent_dispatch->Release();
    }
    SECTION("unavailable header name hides the value") {
        auto* value_dispatch = new TextFieldDispatch(
            L"GuiTextField", 10100, L"wnd[1]/usr/txtIP_HEADER_VALUE", L"", L"", L"synthetic-header-marker");
        auto value = ComGuiElement::create(value_dispatch);
        value_dispatch->Release();
        REQUIRE(value->get_text() == "[REDACTED]");
        REQUIRE(value_dispatch->text_reads == 0);
    }
}

TEST_CASE("Generic named value field hides a credential value", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* parent_dispatch = new TextFieldDispatch(L"GuiUserArea", 10500);
    auto* name_dispatch = new TextFieldDispatch(
        L"GuiTextField", 10600, L"wnd[1]/usr/txtNAME", L"", L"", L"Authorization");
    auto* value_dispatch = new TextFieldDispatch(
        L"GuiTextField", 10700, L"wnd[1]/usr/txtVALUE", L"", L"", L"Bearer synthetic-marker");
    parent_dispatch->named_sibling_dispatch = name_dispatch;
    parent_dispatch->named_sibling_id = L"txtNAME";
    value_dispatch->parent_dispatch = parent_dispatch;
    auto value = ComGuiElement::create(value_dispatch);
    value_dispatch->Release();

    REQUIRE(value->get_text() == "[REDACTED]");
    REQUIRE(value_dispatch->text_reads == 0);

    name_dispatch->Release();
    parent_dispatch->Release();
}

TEST_CASE("Indexed table value uses the header name in the same row", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* parent_dispatch = new TextFieldDispatch(L"GuiTableControl", 13200);
    auto* secret_name = new TextFieldDispatch(
        L"GuiTextField", 13300,
        L"wnd[0]/usr/tblPAIR/txtZPAIR-HEADERNAME[1,0]", L"", L"", L"Authorization");
    auto* ordinary_name = new TextFieldDispatch(
        L"GuiTextField", 13300,
        L"wnd[0]/usr/tblPAIR/txtZPAIR-HEADERNAME[1,1]", L"", L"", L"Content-Type");
    auto* field_name = new TextFieldDispatch(
        L"GuiTextField", 13300,
        L"wnd[0]/usr/tblPAIR/txtZPAIR-FIELDNAME[1,2]", L"", L"", L"Authorization");
    auto* secret_value = new TextFieldDispatch(
        L"GuiTextField", 13300,
        L"wnd[0]/usr/tblPAIR/txtZPAIR-VALUE[2,0]", L"", L"", L"synthetic-table-secret");
    auto* ordinary_value = new TextFieldDispatch(
        L"GuiTextField", 13300,
        L"wnd[0]/usr/tblPAIR/txtZPAIR-VALUE[2,1]", L"", L"", L"application/json");
    auto* field_value = new TextFieldDispatch(
        L"GuiTextField", 13300,
        L"wnd[0]/usr/tblPAIR/txtZPAIR-VALUE[2,2]", L"", L"", L"synthetic-field-secret");
    auto* children_dispatch = new TextFieldDispatch(L"GuiCollection", 13700);
    children_dispatch->child_items = {
        secret_name, ordinary_name, field_name,
        secret_value, ordinary_value, field_value};
    parent_dispatch->children_dispatch = children_dispatch;
    secret_value->parent_dispatch = parent_dispatch;
    ordinary_value->parent_dispatch = parent_dispatch;
    field_value->parent_dispatch = parent_dispatch;
    auto secret = ComGuiElement::create(secret_value);
    auto ordinary = ComGuiElement::create(ordinary_value);
    auto field = ComGuiElement::create(field_value);
    secret_value->Release();
    ordinary_value->Release();
    field_value->Release();

    REQUIRE(secret->get_text() == "[REDACTED]");
    CHECK(secret_value->text_reads == 0);
    REQUIRE(ordinary->get_text() == "application/json");
    CHECK(ordinary_value->text_reads == 1);
    REQUIRE(field->get_text() == "[REDACTED]");
    CHECK(field_value->text_reads == 0);

    secret_name->Release();
    ordinary_name->Release();
    field_name->Release();
    children_dispatch->Release();
    parent_dispatch->Release();
}

TEST_CASE("Generic keyed value field hides a credential value", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* parent_dispatch = new TextFieldDispatch(L"GuiUserArea", 12200);
    auto* key_dispatch = new TextFieldDispatch(
        L"GuiTextField", 12300, L"wnd[0]/usr/txtP_KEY", L"", L"", L"Authorization");
    auto* value_dispatch = new TextFieldDispatch(
        L"GuiTextField", 12400, L"wnd[0]/usr/txtP_VALUE", L"", L"", L"synthetic-key-marker");
    auto* children_dispatch = new TextFieldDispatch(L"GuiCollection", 12500);
    children_dispatch->child_items = {key_dispatch, value_dispatch};
    parent_dispatch->children_dispatch = children_dispatch;
    value_dispatch->parent_dispatch = parent_dispatch;
    auto value = ComGuiElement::create(value_dispatch);
    value_dispatch->Release();

    REQUIRE(value->get_text() == "[REDACTED]");
    REQUIRE(value_dispatch->text_reads == 0);

    key_dispatch->Release();
    children_dispatch->Release();
    parent_dispatch->Release();
}

TEST_CASE("Generic keyed value field preserves ordinary content", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* parent_dispatch = new TextFieldDispatch(L"GuiUserArea", 12600);
    auto* key_dispatch = new TextFieldDispatch(
        L"GuiTextField", 12700, L"wnd[0]/usr/txtP_KEY", L"", L"", L"Flight");
    auto* value_dispatch = new TextFieldDispatch(
        L"GuiTextField", 12800, L"wnd[0]/usr/txtP_VALUE", L"", L"", L"LH");
    auto* children_dispatch = new TextFieldDispatch(L"GuiCollection", 12900);
    children_dispatch->child_items = {key_dispatch, value_dispatch};
    parent_dispatch->children_dispatch = children_dispatch;
    value_dispatch->parent_dispatch = parent_dispatch;
    auto value = ComGuiElement::create(value_dispatch);
    value_dispatch->Release();

    REQUIRE(value->get_text() == "LH");
    REQUIRE(value_dispatch->text_reads == 1);

    key_dispatch->Release();
    children_dispatch->Release();
    parent_dispatch->Release();
}

TEST_CASE("Generic value field preserves ordinary content", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    SECTION("ordinary sibling name") {
        auto* parent_dispatch = new TextFieldDispatch(L"GuiUserArea", 10800);
        auto* name_dispatch = new TextFieldDispatch(
            L"GuiTextField", 10900, L"wnd[1]/usr/txtNAME", L"", L"", L"Flight");
        auto* value_dispatch = new TextFieldDispatch(
            L"GuiTextField", 11000, L"wnd[1]/usr/txtVALUE", L"", L"", L"LH");
        auto* children_dispatch = new TextFieldDispatch(L"GuiCollection", 11050);
        children_dispatch->child_items = {name_dispatch, value_dispatch};
        parent_dispatch->named_sibling_dispatch = name_dispatch;
        parent_dispatch->named_sibling_id = L"txtNAME";
        parent_dispatch->children_dispatch = children_dispatch;
        value_dispatch->parent_dispatch = parent_dispatch;
        auto value = ComGuiElement::create(value_dispatch);
        value_dispatch->Release();

        REQUIRE(value->get_text() == "LH");
        REQUIRE(value_dispatch->text_reads == 1);

        name_dispatch->Release();
        children_dispatch->Release();
        parent_dispatch->Release();
    }
    SECTION("no paired name field") {
        auto* parent_dispatch = new TextFieldDispatch(L"GuiUserArea", 11200);
        auto* children_dispatch = new TextFieldDispatch(L"GuiCollection", 11300);
        auto* value_dispatch = new TextFieldDispatch(
            L"GuiTextField", 11100, L"wnd[0]/usr/txtVALUE", L"", L"", L"ordinary-value");
        children_dispatch->child_items = {value_dispatch};
        parent_dispatch->children_dispatch = children_dispatch;
        value_dispatch->parent_dispatch = parent_dispatch;
        auto value = ComGuiElement::create(value_dispatch);
        value_dispatch->Release();
        REQUIRE(value->get_text() == "ordinary-value");
        children_dispatch->Release();
        parent_dispatch->Release();
    }
}

TEST_CASE("Generic value field suppresses an unverified sibling", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    SECTION("parent unavailable") {
        auto* value_dispatch = new TextFieldDispatch(
            L"GuiTextField", 11400, L"wnd[1]/usr/txtVALUE", L"", L"", L"synthetic-marker");
        auto value = ComGuiElement::create(value_dispatch);
        value_dispatch->Release();
        REQUIRE(value->get_text() == "[REDACTED]");
        REQUIRE(value_dispatch->text_reads == 0);
    }
    SECTION("FindById fails but the sibling exists in Children") {
        auto* parent_dispatch = new TextFieldDispatch(L"GuiUserArea", 11500);
        auto* children_dispatch = new TextFieldDispatch(L"GuiCollection", 11600);
        auto* name_dispatch = new TextFieldDispatch(
            L"GuiTextField", 11700, L"wnd[1]/usr/txtNAME", L"", L"", L"Authorization");
        auto* value_dispatch = new TextFieldDispatch(
            L"GuiTextField", 11800, L"wnd[1]/usr/txtVALUE", L"", L"", L"synthetic-marker");
        children_dispatch->child_items = {name_dispatch, value_dispatch};
        parent_dispatch->children_dispatch = children_dispatch;
        value_dispatch->parent_dispatch = parent_dispatch;
        auto value = ComGuiElement::create(value_dispatch);
        value_dispatch->Release();

        REQUIRE(value->get_text() == "[REDACTED]");
        REQUIRE(value_dispatch->text_reads == 0);

        name_dispatch->Release();
        children_dispatch->Release();
        parent_dispatch->Release();
    }
}

TEST_CASE("Direct report label read masks a credential value on its row", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* parent_dispatch = new TextFieldDispatch(L"GuiUserArea", 10200);
    auto* children_dispatch = new TextFieldDispatch(L"GuiCollection", 10300);
    auto* name_dispatch = new TextFieldDispatch(
        L"GuiLabel", 10400, L"wnd[0]/usr/lbl[0,2]", L"", L"", L"Password:");
    auto* value_dispatch = new TextFieldDispatch(
        L"GuiLabel", 10400, L"wnd[0]/usr/lbl[29,2]", L"", L"", L"synthetic-report-marker");
    auto* ordinary_dispatch = new TextFieldDispatch(
        L"GuiLabel", 10400, L"wnd[0]/usr/lbl[0,3]", L"", L"", L"ordinary report text");
    children_dispatch->child_items = {name_dispatch, value_dispatch, ordinary_dispatch};
    parent_dispatch->children_dispatch = children_dispatch;
    value_dispatch->parent_dispatch = parent_dispatch;
    ordinary_dispatch->parent_dispatch = parent_dispatch;
    auto value = ComGuiElement::create(value_dispatch);
    auto ordinary = ComGuiElement::create(ordinary_dispatch);
    value_dispatch->Release();
    ordinary_dispatch->Release();

    auto parent = ComGuiElement::create(parent_dispatch);
    REQUIRE(parent->get_child_count() == 3);
    auto first_child = parent->get_child(0);
    REQUIRE(first_child);
    REQUIRE(parent->get_child(1));
    REQUIRE(parent->get_child(2));
    REQUIRE(parent->get_child(2)->get_id() == "wnd[0]/usr/lbl[0,3]");
    REQUIRE(value->get_text_for_direct_read() == "[REDACTED]");
    REQUIRE(ordinary->get_text_for_direct_read() == "ordinary report text");

    name_dispatch->Release();
    children_dispatch->Release();
    parent_dispatch->Release();
}

TEST_CASE("Direct report label read suppresses an unreadable sibling", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    auto* parent_dispatch = new TextFieldDispatch(L"GuiUserArea", 11900);
    auto* children_dispatch = new TextFieldDispatch(L"GuiCollection", 12000);
    auto* unreadable_dispatch = new TextFieldDispatch(
        L"GuiLabel", 12100, L"", L"", L"", L"Password:");
    auto* value_dispatch = new TextFieldDispatch(
        L"GuiLabel", 12100, L"wnd[0]/usr/lbl[29,2]", L"", L"", L"synthetic-report-marker");
    children_dispatch->child_items = {unreadable_dispatch, value_dispatch};
    parent_dispatch->children_dispatch = children_dispatch;
    value_dispatch->parent_dispatch = parent_dispatch;
    auto value = ComGuiElement::create(value_dispatch);
    value_dispatch->Release();

    REQUIRE(value->get_text_for_direct_read() == "[REDACTED]");
    REQUIRE(value_dispatch->text_reads == 0);

    unreadable_dispatch->Release();
    children_dispatch->Release();
    parent_dispatch->Release();
}

TEST_CASE("Grid viewport metadata exposes the scroll position", "[com][grid]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 151, L"wnd[0]/usr/grid");
    auto grid = ComGuiElement::create(dispatch);
    dispatch->Release();
    const auto viewport = extract_grid_viewport_metadata(grid);
    REQUIRE(viewport.at("first_visible_column") == "NAMESPACE");
    REQUIRE(viewport.at("first_visible_row") == 3);
    REQUIRE(viewport.at("current_cell_column") == "PROCESS_MODE_TEXT");
    REQUIRE(viewport.at("current_cell_row") == 40);
}

TEST_CASE("Grid cell reads resolve the COM method once per grid type", "[com][grid][performance]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 151, L"wnd[0]/usr/grid");
    auto grid = ComGuiElement::create(dispatch);
    dispatch->Release();
    REQUIRE(grid->get_type() == "GuiShell");
    REQUIRE(grid->get_cell_value(0, "A") == "grid-cell");
    REQUIRE(grid->get_cell_value(0, "B") == "grid-cell");
    REQUIRE(dispatch->cell_value_lookups == 1);
}

TEST_CASE("Grid toolbar metadata resolves each COM method once", "[com][grid][performance]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 151, L"wnd[0]/usr/grid");
    auto grid = ComGuiElement::create(dispatch);
    dispatch->Release();
    REQUIRE(grid->get_type() == "GuiShell");
    REQUIRE(grid->get_grid_toolbar_button_id(0) == "BUTTON");
    REQUIRE(grid->get_grid_toolbar_button_id(1) == "BUTTON");
    REQUIRE(grid->get_grid_toolbar_button_tooltip(0) == "Tooltip");
    REQUIRE(grid->get_grid_toolbar_button_tooltip(1) == "Tooltip");
    REQUIRE(dispatch->toolbar_id_lookups == 1);
    REQUIRE(dispatch->toolbar_tooltip_lookups == 1);
}

TEST_CASE("ABAP editor reads bounded source lines through scripting methods", "[com][editor]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 351, L"wnd[0]/usr/editor");
    auto editor = ComGuiElement::create(dispatch);
    dispatch->Release();
    const auto content = editor->get_abap_editor_content(2);
    REQUIRE(content.total_lines == 3);
    REQUIRE(content.lines_read == 2);
    REQUIRE(content.truncated);
    REQUIRE(content.text == "FORM SET_SCREEN_0300.\n  lv_name = iv_name.");
    REQUIRE(dispatch->editor_line_reads == 2);
}

TEST_CASE("Direct element read returns bounded ABAP source instead of shell identifier", "[com][editor][get]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 451, L"wnd[0]/usr/editor", L"", L"AbapEditor");
    auto editor = ComGuiElement::create(dispatch);
    dispatch->Release();
    const auto data = read_element_value(editor);
    REQUIRE(data["value"] == "FORM SET_SCREEN_0300.\n  lv_name = iv_name.\nENDFORM.");
    REQUIRE(data["source_total_lines"] == 3);
    REQUIRE(data["source_lines_read"] == 3);
    REQUIRE(data["source_truncated"] == false);
    REQUIRE(dispatch->text_reads == 0);
}

TEST_CASE("Direct HTML viewer read does not expose its browser control name", "[com][html][get]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 452, L"wnd[0]/usr/html",
                                           L"", L"HTMLViewer", L"SAP.HTMLControl.1");
    auto viewer = ComGuiElement::create(dispatch);
    dispatch->Release();
    const auto data = read_element_value(viewer);
    REQUIRE(data.at("element_subtype") == "HTMLViewer");
    REQUIRE(data.at("content_available") == false);
    REQUIRE(data.at("value") == "");
}

TEST_CASE("ScreenReader extract_grid_data preserves GuiShell subtype", "[screen][grid]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 452, L"wnd[0]/usr/cntlGRID1/shellcont/shell",
                                           L"", L"GridView", L"");
    auto grid = ComGuiElement::create(dispatch);
    dispatch->Release();

    ScreenReader reader(nullptr);
    auto data = reader.extract_grid_data_immediately(grid, "wnd[0]/usr/cntlGRID1/shellcont/shell");
    REQUIRE(data.at("type") == "GuiShell");
    REQUIRE(data.at("subtype") == "GridView");
}

TEST_CASE("URI input field does not export a credential query parameter", "[com][privacy][get]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(
        L"GuiCTextField", 551, L"wnd[0]/usr/ctxtURI", L"Request URI", L"",
        L"/sap/opu/odata?access_token=FF_FIELD_QUERY_83&$top=1");
    auto field = ComGuiElement::create(dispatch);
    dispatch->Release();
    const auto text = field->get_text();
    REQUIRE(text.find("FF_FIELD_QUERY_83") == std::string::npos);
    REQUIRE(text.find("$top=1") != std::string::npos);
}

TEST_CASE("List-tree text item receives a real double-click", "[com][tree][action]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 151, L"wnd[0]/usr/tree");
    auto tree = ComGuiElement::create(dispatch);
    dispatch->Release();
    tree->doubleclick_node("NODE");
    REQUIRE(dispatch->tree_item_doubleclicks == 1);
    REQUIRE(dispatch->tree_node_doubleclicks == 0);

    auto* simple_dispatch = new TextFieldDispatch(L"GuiShell", 251, L"wnd[0]/usr/simple-tree");
    simple_dispatch->tree_node_has_text = true;
    auto simple_tree = ComGuiElement::create(simple_dispatch);
    simple_dispatch->Release();
    simple_tree->doubleclick_node("SIMPLE");
    REQUIRE(simple_dispatch->tree_node_doubleclicks == 1);
    REQUIRE(simple_dispatch->tree_item_doubleclicks == 0);
}

TEST_CASE("Selecting a tree node clears an earlier selection", "[com][tree][action]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 351, L"wnd[0]/usr/tree");
    auto tree = ComGuiElement::create(dispatch);
    dispatch->Release();

    tree->select_node("target-node");

    REQUIRE(dispatch->tree_selection_calls == std::vector<std::string>{"UnselectAll", "SelectNode"});
    REQUIRE(dispatch->selected_tree_nodes == std::vector<std::wstring>{L"target-node"});
}


TEST_CASE("COM apartment mismatch cannot create a synthetic automation engine", "[com][factory]") {
    bool mta_initialized = false;
    bool mismatch_propagated = false;
    std::thread worker([&] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) return;
        mta_initialized = true;
        try {
            auto engine = AutomationEngine::create();
        } catch (const ComInitializationException& e) {
            mismatch_propagated = e.hresult() == RPC_E_CHANGED_MODE;
        }
        CoUninitialize();
    });
    worker.join();
    REQUIRE(mta_initialized);
    REQUIRE(mismatch_propagated);
}

TEST_CASE("CLI serializes COM initialization failure and exits nonzero", "[com][cli]") {
    const auto result = run_cli_on_mta({"fairyfly", "list"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    const auto response = nlohmann::json::parse(result.output);
    REQUIRE(response.at("status") == "error");
    REQUIRE(response.at("error").at("code") == "INTERNAL_ERROR");
    REQUIRE(spdlog::get_level() == spdlog::level::err);
}

TEST_CASE("Fill accepts an explicit clear option without a value", "[com][cli][fill]") {
    const auto result = run_cli_on_mta({"fairyfly", "fill", "wnd[0]/usr/txtFIELD", "--clear"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("INTERNAL_ERROR") != std::string::npos);
}

TEST_CASE("Disconnect accepts an explicit close-session option", "[com][cli][disconnect]") {
    const auto result = run_cli_on_mta({"fairyfly", "disconnect", "--connection", "0", "--close-session"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("INTERNAL_ERROR") != std::string::npos);
}

TEST_CASE("Attach accepts an exact SAP GUI session ID", "[com][cli][attach]") {
    const auto result = run_cli_on_mta({"fairyfly", "attach", "--session-id", "/app/con[0]/ses[1]"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("INTERNAL_ERROR") != std::string::npos);
}

TEST_CASE("CLI exception honors global TOON output", "[com][cli][format]") {
    const auto result = run_cli_on_mta({"fairyfly", "--output", "toon", "list"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("error:") != std::string::npos);
    REQUIRE(result.output.find("INTERNAL_ERROR") != std::string::npos);
    REQUIRE(result.output.find('{') == std::string::npos);
}

TEST_CASE("CLI exception honors screen Markdown output", "[com][cli][format]") {
    const auto result = run_cli_on_mta({"fairyfly", "screen", "read", "--output", "markdown", "--no-tabs"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("# ") != std::string::npos);
    REQUIRE(result.output.find("INTERNAL_ERROR") != std::string::npos);
    REQUIRE(result.output.find('{') == std::string::npos);
}

TEST_CASE("SAP application wrapper balances COM initialization", "[com][lifecycle]") {
    HRESULT apartment_after = S_OK;
    std::thread worker([&] {
        try {
            auto app = ComGuiApplication::create();
            app.reset();
        } catch (const ComException&) {
            // SAP GUI may be absent; failure must also balance CoInitializeEx.
        }
        APTTYPE type;
        APTTYPEQUALIFIER qualifier;
        apartment_after = CoGetApartmentType(&type, &qualifier);
    });
    worker.join();
    REQUIRE(apartment_after == CO_E_NOTINITIALIZED);
}

TEST_CASE("Read-only SAP field rejects fill before Text property write", "[com][fill][readonly]") {
    auto* dispatch = new ReadOnlyFieldDispatch();
    auto field = ComGuiElement::create(dispatch);
    dispatch->Release();

    REQUIRE_FALSE(field->set_text("new value"));
    REQUIRE(dispatch->text_writes == 0);
}

TEST_CASE("Writable SAP field can still receive Text property write", "[com][fill]") {
    auto* dispatch = new ReadOnlyFieldDispatch(true);
    auto field = ComGuiElement::create(dispatch);
    dispatch->Release();

    REQUIRE(field->set_text("new value"));
    REQUIRE(dispatch->text_writes == 1);
}

TEST_CASE("Field without Changeable property preserves prior Text write behavior", "[com][fill]") {
    auto* dispatch = new ReadOnlyFieldDispatch(true, false);
    auto field = ComGuiElement::create(dispatch);
    dispatch->Release();

    REQUIRE(field->set_text("new value"));
    REQUIRE(dispatch->text_writes == 1);
}

TEST_CASE("ComGuiElement classify_type tests", "[com][element][classify]") {
    SECTION("Correctly identifies RadioButton and does not shadow with Button") {
        REQUIRE(ComGuiElement::classify_type("GuiRadioButton") == GuiElementType::RadioButton);
    }

    SECTION("Correctly identifies CheckBox and does not shadow with Button") {
        REQUIRE(ComGuiElement::classify_type("GuiCheckBox") == GuiElementType::CheckBox);
    }

    SECTION("Correctly identifies Button") {
        REQUIRE(ComGuiElement::classify_type("GuiButton") == GuiElementType::Button);
    }

    SECTION("Correctly distinguishes Tab from TabStrip") {
        REQUIRE(ComGuiElement::classify_type("GuiTab") == GuiElementType::Tab);
        REQUIRE(ComGuiElement::classify_type("GuiTabStrip") != GuiElementType::Tab);
    }

    SECTION("Correctly identifies text fields") {
        REQUIRE(ComGuiElement::classify_type("GuiTextField") == GuiElementType::TextField);
        REQUIRE(ComGuiElement::classify_type("GuiCTextField") == GuiElementType::TextField);
    }
}

TEST_CASE("ComGuiApplication initialization", "[com][gui_application]") {
    SECTION("ComGuiApplication::create() returns valid pointer or throws") {
        try {
            auto app = ComGuiApplication::create();
            REQUIRE(app != nullptr);
            spdlog::info("SAP GUI found - initialization successful");
            SUCCEED();
        } catch (const ComException& e) {
            spdlog::warn("SAP GUI not available: {}", e.what());
            SUCCEED("ComException thrown as expected when SAP not available");
        }
    }
}

TEST_CASE("ComGuiApplication with SAP GUI present", "[com][gui_application][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available: " + std::string(e.what()));
    }

    REQUIRE(app != nullptr);

    SECTION("Can get app object") {
        REQUIRE(app->get_app_object() != nullptr);
    }

    SECTION("Can check connection count") {
        int count = app->get_connection_count();
        REQUIRE(count >= 0);
        CAPTURE(count);
    }

    SECTION("has_connections matches get_connection_count") {
        bool has = app->has_connections();
        int count = app->get_connection_count();
        REQUIRE(has == (count > 0));
    }

}

TEST_CASE("ComGuiConnection properties", "[com][gui_connection][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available");
    }

    int conn_count = app->get_connection_count();
    if (conn_count == 0) {
        SKIP("No SAP connections available");
    }

    auto conn = app->get_connection(0);
    REQUIRE(conn != nullptr);

    SECTION("Connection has valid ID") {
        std::string id = conn->get_id();
        REQUIRE(!id.empty());
        CAPTURE(id);
    }

    SECTION("Connection has description") {
        std::string desc = conn->get_description();
        REQUIRE(!desc.empty());
        CAPTURE(desc);
    }

    SECTION("Session count is valid") {
        int sess_count = conn->get_session_count();
        REQUIRE(sess_count >= 0);
        REQUIRE(sess_count <= 6);  // SAP hard limit
        CAPTURE(sess_count);
    }
}

TEST_CASE("ComGuiSession basic operations", "[com][gui_session][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available");
    }

    if (app->get_connection_count() == 0) {
        SKIP("No SAP connections");
    }

    auto conn = app->get_connection(0);
    if (conn->get_session_count() == 0) {
        SKIP("No active sessions");
    }

    auto session = conn->get_session(0);
    REQUIRE(session != nullptr);

    SECTION("Session has valid ID") {
        std::string id = session->get_id();
        REQUIRE(!id.empty());
        CAPTURE(id);
    }

    SECTION("Can check if session is alive") {
        bool alive = session->is_alive();
        REQUIRE(alive);
    }

    SECTION("Can check busy status") {
        bool busy = session->is_busy();
        REQUIRE(std::is_same_v<decltype(busy), bool>);
        CAPTURE(busy);
    }

    SECTION("Can get active window") {
        auto window = session->get_active_window();
        REQUIRE(window != nullptr);
    }
}

TEST_CASE("ComGuiWindow operations", "[com][gui_window][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available");
    }

    if (app->get_connection_count() == 0) {
        SKIP("No SAP connections");
    }

    auto conn = app->get_connection(0);
    if (conn->get_session_count() == 0) {
        SKIP("No active sessions");
    }

    auto session = conn->get_session(0);
    auto window = session->get_active_window();
    REQUIRE(window != nullptr);

    SECTION("Window has ID") {
        std::string id = window->get_id();
        REQUIRE(!id.empty());
        CAPTURE(id);
    }

    SECTION("Window may have title") {
        std::string title = window->get_title();
        // Title might be empty, but operation should not crash
        CAPTURE(title);
        SUCCEED();
    }

    SECTION("Window child count is non-negative") {
        int count = window->get_child_count();
        REQUIRE(count >= 0);
        CAPTURE(count);
    }

    SECTION("Can traverse children") {
        int count = window->get_child_count();
        if (count > 0) {
            auto child = window->get_child(0);
            REQUIRE(child != nullptr);
        }
    }
}

TEST_CASE("ComGuiElement properties", "[com][gui_element][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available");
    }

    if (app->get_connection_count() == 0) {
        SKIP("No SAP connections");
    }

    auto conn = app->get_connection(0);
    if (conn->get_session_count() == 0) {
        SKIP("No active sessions");
    }

    auto session = conn->get_session(0);
    auto window = session->get_active_window();

    int child_count = window->get_child_count();
    if (child_count == 0) {
        SKIP("No child elements");
    }

    auto elem = window->get_child(0);
    REQUIRE(elem != nullptr);

    SECTION("Element has ID") {
        std::string id = elem->get_id();
        REQUIRE(!id.empty());
        CAPTURE(id);
    }

    SECTION("Element has type") {
        std::string type = elem->get_type();
        REQUIRE(!type.empty());
        CAPTURE(type);
    }

    SECTION("Element text can be retrieved") {
        std::string text = elem->get_text();
        // Text might be empty, but operation should not crash
        CAPTURE(text);
        SUCCEED();
    }

    SECTION("Element visibility can be checked") {
        bool visible = elem->is_visible();
        CAPTURE(visible);
        SUCCEED();
    }

    SECTION("Element enabled status can be checked") {
        bool enabled = elem->is_enabled();
        CAPTURE(enabled);
        SUCCEED();
    }

    SECTION("Container element get_child navigation") {
        bool tested_container = false;
        for (int i = 0; i < child_count; ++i) {
            auto child = window->get_child(i);
            if (child && child->get_child_count() > 0) {
                int sub_count = child->get_child_count();
                CAPTURE(child->get_id(), child->get_type(), sub_count);
                auto grand_child = child->get_child(0);
                REQUIRE(grand_child != nullptr);
                CHECK(!grand_child->get_id().empty());
                tested_container = true;
                break;
            }
        }
        REQUIRE(tested_container);
    }

    SECTION("TabStrip children navigation via get_child") {
        try {
            auto tabstrip = session->find_element_by_id("wnd[0]/usr/tabsTAB_STRIP");
            if (tabstrip) {
                int tab_count = tabstrip->get_child_count();
                CHECK(tab_count == 6);
                for (int i = 0; i < tab_count; ++i) {
                    auto tab = tabstrip->get_child(i);
                    REQUIRE(tab != nullptr);
                    CHECK(tab->get_type() == "GuiTab");
                    CHECK(!tab->get_id().empty());
                    int sub_child_count = tab->get_child_count();
                    WARN("Tab " << tab->get_id() << " has " << sub_child_count << " children");
                    if (sub_child_count > 0) {
                        auto tab_sub = tab->get_child(0);
                        if (tab_sub) {
                            WARN("Tab child: " << tab_sub->get_id() << " (type: " << tab_sub->get_type() << ", child_count: " << tab_sub->get_child_count() << ")");
                            if (tab_sub->get_child_count() > 0) {
                                auto sub_child = tab_sub->get_child(0);
                                if (sub_child) {
                                    WARN("Tab sub-child: " << sub_child->get_id() << " (type: " << sub_child->get_type() << ", child_count: " << sub_child->get_child_count() << ")");
                                }
                            }
                        }
                    }
                }



            }
        } catch (const ComException&) {
            // Screen might not have tabsTAB_STRIP currently
        }
    }

    SECTION("Inspect GuiTableControl on Fields tab") {
        try {
            try {
                auto tab_def = session->find_element_by_id("wnd[0]/usr/tabsTAB_STRIP/tabpDEF");
                if (tab_def) {
                    tab_def->select();
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
            } catch (...) {}

            auto table = session->find_element_by_id("wnd[0]/usr/tabsTAB_STRIP/tabpDEF/ssubTS_SCREEN:SAPLSD41:2201/tblSAPLSD41TC0");
            REQUIRE(table != nullptr);
            WARN("Found table: " << table->get_id() << " type: " << table->get_type());
            int row_count = 0, vis_rows = 0;
            try { row_count = table->get_property_int(L"RowCount"); } catch (...) {}
            try { vis_rows = table->get_property_int(L"VisibleRowCount"); } catch (...) {}
            WARN("Table RowCount=" << row_count << ", VisibleRowCount=" << vis_rows);

            // Check Columns property
            try {
                auto cols = table->get_dispatch_property(L"Columns");
                if (cols) {
                    _variant_t count_val;
                    DISPID count_dispid;
                    if (SUCCEEDED(get_dispid_via_typeinfo(cols, L"Count", &count_dispid))) {
                        DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
                        if (SUCCEEDED(cols->Invoke(count_dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &no_params, &count_val, nullptr, nullptr))) {
                            int num_cols = count_val.intVal;
                            WARN("Columns Count = " << num_cols);
                            SapGuiCollection<ComGuiElement> col_coll(cols);
                            for (int c = 0; c < num_cols; ++c) {
                                auto col_elem = col_coll.item(c);
                                if (col_elem) {
                                    std::string col_name, col_title;
                                    try { col_name = col_elem->get_property_string(L"Name"); } catch (...) {}
                                    try { col_title = col_elem->get_property_string(L"Title"); } catch (...) {}
                                    WARN("Col " << c << " name='" << col_name << "' title='" << col_title << "'");
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                WARN("Columns error: " << e.what());
            }


            // Check GetCell method
            try {
                DISPID cell_dispid;
                if (SUCCEEDED(get_dispid_via_typeinfo(table->get_dispatch(), L"GetCell", &cell_dispid))) {
                    _variant_t row(0);
                    _variant_t col(0);
                    VARIANT args[2] = {col, row}; // reverse order for DISPPARAMS: col=arg[0], row=arg[1] in VARIANT array (SAP GUI COM convention is (row, col))
                    DISPPARAMS params = {args, nullptr, 2, 0};
                    _variant_t cell_result;
                    HRESULT hr = table->get_dispatch()->Invoke(cell_dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, &cell_result, nullptr, nullptr);
                    if (SUCCEEDED(hr) && cell_result.vt == VT_DISPATCH) {
                        auto cell_elem = ComGuiElement::create(cell_result.pdispVal);
                        WARN("GetCell(0,0) succeeded: ID=" << cell_elem->get_id() << " text=" << cell_elem->get_text() << " type=" << cell_elem->get_type());
                    } else {
                        WARN("GetCell Invoke hr = 0x" << std::hex << hr);
                    }
                } else {
                    WARN("GetCell dispid not found");
                }
            } catch (const std::exception& e) {
                WARN("GetCell error: " << e.what());
            }
        } catch (const std::exception& e) {
            WARN("Table not found: " << e.what());
        }
    }

    SECTION("ElementMetadataExtractor extracts columns and rows for GuiTableControl") {
        try {
            auto table = session->find_element_by_id("/app/con[0]/ses[0]/wnd[0]/usr/tabsTAB_STRIP/tabpDEF/ssubTS_SCREEN:SAPLSD41:2201/tblSAPLSD41TC0");
            REQUIRE(table != nullptr);
            auto metadata = ElementMetadataExtractor::extract(table);
            REQUIRE(metadata.contains("table_data"));
            REQUIRE(metadata["table_data"].contains("columns"));
            REQUIRE(metadata["table_data"]["columns"].size() == 10);
            REQUIRE(metadata["table_data"].contains("rows"));
            REQUIRE(metadata["table_data"]["rows"].size() > 0);
        } catch (const std::exception& e) {
            WARN("Table metadata test error: " << e.what());
        }
    }

    SECTION("Inspect usr children on current screen") {
        auto usr = session->find_element_by_id("/app/con[0]/ses[0]/wnd[0]/usr");
        REQUIRE(usr != nullptr);
        int count = usr->get_child_count();
        WARN("usr child_count=" << count);
        auto coll = usr->children();
        WARN("usr children collection count=" << coll.count());
        for (int i = 0; i < coll.count(); ++i) {
            auto ch = coll.item(i);
            if (ch) {
                WARN("Child " << i << ": ID=" << ch->get_id() << " Type=" << ch->get_type() << " Text=" << ch->get_text());
            } else {
                WARN("Child " << i << " is null");
            }
        }
    }
}



TEST_CASE("ComGuiElement find_element_by_id", "[com][gui_element][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available");
    }

    if (app->get_connection_count() == 0) {
        SKIP("No SAP connections");
    }

    auto conn = app->get_connection(0);
    if (conn->get_session_count() == 0) {
        SKIP("No active sessions");
    }

    auto session = conn->get_session(0);

    SECTION("Can find element by standard window ID") {
        try {
            auto elem = session->find_element_by_id("wnd[0]");
            REQUIRE(elem != nullptr);
        } catch (const ComException& e) {
            SKIP("Element not found (expected in some SAP screens)");
        }
    }

    SECTION("Non-existent element throws") {
        REQUIRE_THROWS_AS(session->find_element_by_id("wnd[999]/usr/invalid"),
                         ComException);
    }
}

TEST_CASE("Memory safety of COM wrappers", "[com][memory]") {
    SECTION("Multiple wrapper creations don't cause issues") {
        try {
            auto app1 = ComGuiApplication::create();
            {
                auto app2 = ComGuiApplication::create();
                REQUIRE(app2 != nullptr);
            }
            // app2 destructed, app1 should still be valid
            REQUIRE(app1 != nullptr);
            SUCCEED();
        } catch (const ComException& e) {
            SKIP("SAP GUI not available");
        }
    }

    SECTION("Null shared_ptr handling") {
        ComGuiElementPtr null_elem = nullptr;
        REQUIRE(null_elem == nullptr);
    }
}

TEST_CASE("COM wrapper exception safety", "[com][exceptions]") {
    SECTION("Invalid COM operations throw appropriate exceptions") {
        try {
            // This should raise an error - trying to create with null
            ComGuiElement::create(nullptr);
            FAIL("Should have thrown ComException");
        } catch (const ComException& e) {
            SUCCEED("ComException thrown as expected");
        }
    }
}


TEST_CASE("read_action_status returns status bar text and message type", "[com][status]") {
    ScopedDispatchCacheReset cache_reset;
    auto* bar = new StringPropertyDispatch({
        {L"Type", L"GuiStatusbar"}, {L"Text", L"Job log displayed"},
        {L"DisplayedText", L"Job log displayed"},
        {L"MessageType", L"S"}, {L"MessageId", L"BL"}, {L"MessageNumber", L"001"}});
    auto* session_dispatch = new TextFieldDispatch(L"GuiSession", 9600);
    session_dispatch->named_sibling_dispatch = bar;
    session_dispatch->named_sibling_id = L"wnd[0]/sbar";
    auto session = ComGuiSession::create(session_dispatch);
    session_dispatch->Release();

    const auto status = read_action_status(session);
    REQUIRE(status.text == "Job log displayed");
    REQUIRE(status.type == "S");
    REQUIRE(status.message_id == "BL");
    REQUIRE(status.message_number == "001");
    bar->Release();
}
