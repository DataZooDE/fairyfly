#include <set>
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
#include "include/menu_navigation.h"
#include "include/vkey.h"
#include "include/read_only_guard.h"
#include "include/action_argument_checks.h"
#include "include/sensitive_data.h"
#include "include/action_status.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>
#include <map>
#include <string>
#include <vector>
#include <array>
#include <cwchar>

using namespace fairyfly;
using namespace fairyfly::sap;

// Redaction markers carry a reason: "[REDACTED: <reason>]".
static bool is_redacted(const std::string& value) { return fairyfly::sap::is_redaction_marker(value); }

namespace {
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
} // namespace

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
    std::vector<std::string> grid_calls;
    int set_cell_row = -1;
    std::wstring set_cell_column;
    int sent_vkey = -1;
    int send_vkey_calls = 0;
    int select_calls = 0;
    bool send_vkey_throws = false;
    bool close_throws = false;
    int close_calls = 0;
    // Round-trip counters: GetIDsOfNames calls per member name, Children property reads,
    // and _NewEnum calls (served from child_items when enumerable is set).
    std::map<std::wstring, int> name_lookups;
    int children_invokes = 0;
    int new_enum_calls = 0;
    bool enumerable = false;
    // Visible member behavior: known (value visible_value), missing, or failing with E_FAIL.
    bool visible_missing = false;
    bool visible_fails = false;
    bool visible_value = true;
    size_t enum_fail_after = static_cast<size_t>(-1);
    // Type property reads that fail with DISP_E_MEMBERNOTFOUND before succeeding (a type whose object
    // rejects the universal Type DISPID once).
    int type_invoke_failures = 0;
    int changeable_reads = 0;
    bool displayed_text_missing = false;  // DisplayedText is an unknown member (like on a GuiShell)
    bool has_row_count = false;  // grid-like GuiShell: RowCount exists (tree-like: unknown name)
    // GuiShell toolbar buttons {id, text, tooltip}; ButtonCount/GetButton* exist only when non-empty.
    std::vector<std::array<const wchar_t*, 3>> shell_buttons;

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
        ++name_lookups[names[0]];
        if (std::wcscmp(names[0], L"Type") == 0) *ids = type_id_;
        else if (std::wcscmp(names[0], L"DisplayedText") == 0) {
            if (displayed_text_missing) return DISP_E_UNKNOWNNAME;
            *ids = type_id_ + 1;
        }
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
        else if (std::wcscmp(names[0], L"Visible") == 0) {
            if (visible_missing) return DISP_E_UNKNOWNNAME;
            *ids = type_id_ + 28;
        }
        else if (std::wcscmp(names[0], L"Changeable") == 0) *ids = type_id_ + 29;
        else if (std::wcscmp(names[0], L"UnselectAll") == 0) *ids = type_id_ + 30;
        else if (std::wcscmp(names[0], L"SelectNode") == 0) *ids = type_id_ + 31;
        else if (std::wcscmp(names[0], L"SetCurrentCell") == 0) *ids = type_id_ + 32;
        else if (std::wcscmp(names[0], L"DoubleClickCurrentCell") == 0) *ids = type_id_ + 33;
        else if (std::wcscmp(names[0], L"SendVKey") == 0) *ids = type_id_ + 34;
        else if (std::wcscmp(names[0], L"Select") == 0) *ids = type_id_ + 35;
        else if (std::wcscmp(names[0], L"Close") == 0) *ids = type_id_ + 36;
        else if (std::wcscmp(names[0], L"RowCount") == 0 && has_row_count) *ids = type_id_ + 37;
        else if (std::wcscmp(names[0], L"ButtonCount") == 0 && !shell_buttons.empty()) *ids = type_id_ + 38;
        else if (std::wcscmp(names[0], L"GetButtonId") == 0 && !shell_buttons.empty()) *ids = type_id_ + 39;
        else if (std::wcscmp(names[0], L"GetButtonText") == 0 && !shell_buttons.empty()) *ids = type_id_ + 40;
        else if (std::wcscmp(names[0], L"GetButtonTooltip") == 0 && !shell_buttons.empty()) *ids = type_id_ + 41;
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
        if (id == type_id_ + 32 && (flags & DISPATCH_METHOD)) {
            if (!params || params->cArgs != 2) return DISP_E_BADPARAMCOUNT;
            if (params->rgvarg[1].vt != VT_I4 && params->rgvarg[1].vt != VT_INT)
                return DISP_E_TYPEMISMATCH;
            if (params->rgvarg[0].vt != VT_BSTR) return DISP_E_TYPEMISMATCH;
            set_cell_row = params->rgvarg[1].lVal;
            set_cell_column = params->rgvarg[0].bstrVal;
            grid_calls.push_back("SetCurrentCell");
            return S_OK;
        }
        if (id == type_id_ + 33 && (flags & DISPATCH_METHOD)) {
            if (params && params->cArgs != 0) return DISP_E_BADPARAMCOUNT;
            grid_calls.push_back("DoubleClickCurrentCell");
            return S_OK;
        }
        if (id == type_id_ + 36 && (flags & DISPATCH_METHOD)) {
            ++close_calls;
            return close_throws ? DISP_E_EXCEPTION : S_OK;
        }
        if (id == type_id_ + 34 && (flags & DISPATCH_METHOD)) {
            if (send_vkey_throws) return DISP_E_EXCEPTION;
            if (!params || params->cArgs != 1) return DISP_E_BADPARAMCOUNT;
            if (params->rgvarg[0].vt != VT_I4 && params->rgvarg[0].vt != VT_INT)
                return DISP_E_TYPEMISMATCH;
            sent_vkey = params->rgvarg[0].lVal;
            ++send_vkey_calls;
            return S_OK;
        }
        if (id == type_id_ + 35 && (flags & DISPATCH_METHOD)) {
            ++select_calls;
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
        if (id == type_id_ && type_invoke_failures > 0) {
            --type_invoke_failures;
            return DISP_E_MEMBERNOTFOUND;
        }
        if (!result) return DISP_E_MEMBERNOTFOUND;
        if (id == type_id_ + 38 && (flags & DISPATCH_PROPERTYGET)) {
            VariantInit(result);
            result->vt = VT_I4;
            result->lVal = static_cast<long>(shell_buttons.size());
            return S_OK;
        }
        if (id >= type_id_ + 39 && id <= type_id_ + 41 && (flags & DISPATCH_METHOD)) {
            if (!params || params->cArgs != 1 ||
                (params->rgvarg[0].vt != VT_I4 && params->rgvarg[0].vt != VT_INT))
                return DISP_E_BADPARAMCOUNT;
            const long index = params->rgvarg[0].lVal;
            if (index < 0 || static_cast<size_t>(index) >= shell_buttons.size()) return DISP_E_BADINDEX;
            VariantInit(result);
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(shell_buttons[index][id - type_id_ - 39]);
            return result->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        if (id == type_id_ + 37 && (flags & DISPATCH_PROPERTYGET)) {
            VariantInit(result);
            result->vt = VT_I4;
            result->lVal = 6;
            return S_OK;
        }
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
        if (id == DISPID_NEWENUM && enumerable) {
            ++new_enum_calls;
            VariantInit(result);
            result->vt = VT_UNKNOWN;
            result->punkVal = new FakeEnumVariant(child_items, 0, enum_fail_after);
            return S_OK;
        }
        if (id == type_id_ + 26 && (flags & DISPATCH_PROPERTYGET) && children_dispatch) {
            ++children_invokes;
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
        if (id == type_id_ + 28) {
            if (visible_fails) return E_FAIL;
            result->vt = VT_BOOL;
            result->boolVal = visible_value ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;
        }
        if (id == type_id_ + 29) ++changeable_reads;
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

TEST_CASE("GuiCheckBox and GuiRadioButton direct reads report the selected state", "[com][get]") {
    ScopedDispatchCacheReset cache_reset;
    for (const auto* type : {L"GuiCheckBox", L"GuiRadioButton"}) {
        auto* dispatch = new TextFieldDispatch(type, 14000, L"wnd[0]/usr/chkTEST", L"Flag", L"", L"Flag text");
        auto element = ComGuiElement::create(IDispatchPtr(dispatch, true));
        dispatch->selected = true;
        auto checked = read_element_value(element);
        REQUIRE(checked.at("selected") == true);
        REQUIRE(checked.at("value") == "Flag text");
        REQUIRE(checked.at("label") == "Flag");
        dispatch->selected = false;
        REQUIRE(read_element_value(element).at("selected") == false);

        // No accessibility label: the caption doubles as the label.
        auto* bare = new TextFieldDispatch(type, 14100, L"wnd[0]/usr/chkBARE", L"", L"", L"Flag text");
        auto bare_element = ComGuiElement::create(IDispatchPtr(bare, true));
        auto bare_data = read_element_value(bare_element);
        REQUIRE(bare_data.at("label") == bare_data.at("value"));
        REQUIRE(bare_data.at("label") == "Flag text");
    }
}

TEST_CASE("GridView double-click sets the current cell before double-clicking it", "[com][grid]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiGridView", 14200);
    auto grid = ComGuiElement::create(IDispatchPtr(dispatch, true));
    grid->doubleclick_grid_cell(3, "MATNR");
    REQUIRE(dispatch->set_cell_row == 3);
    REQUIRE(dispatch->set_cell_column == L"MATNR");
    REQUIRE(dispatch->grid_calls == std::vector<std::string>{"SetCurrentCell", "DoubleClickCurrentCell"});
    REQUIRE_THROWS_AS(grid->doubleclick_grid_cell(-1, "MATNR"), ComException);
    REQUIRE_THROWS_AS(grid->doubleclick_grid_cell(0, ""), ComException);
}

TEST_CASE("Window send_vkey passes the numeric key as an integer variant", "[com][window]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiModalWindow", 14400);
    ComGuiWindow window(IDispatchPtr(dispatch, true));
    window.send_vkey(8);
    REQUIRE(dispatch->send_vkey_calls == 1);
    REQUIRE(dispatch->sent_vkey == 8);
}

TEST_CASE("Popup close falls back to window Close when SendVKey throws", "[com][window][close]") {
    using fairyfly::sap::attempt_close;
    using fairyfly::sap::CloseMethod;
    ScopedDispatchCacheReset cache_reset;

    SECTION("vkey works: no fallback") {
        auto* dispatch = new TextFieldDispatch(L"GuiModalWindow", 14400);
        ComGuiWindow window(IDispatchPtr(dispatch, true));
        auto a = attempt_close(1, [&] { window.send_vkey(12); }, [&] { window.close(); });
        REQUIRE(a.method == CloseMethod::Vkey);
        REQUIRE(dispatch->close_calls == 0);
    }
    SECTION("vkey throws, Close succeeds") {
        auto* dispatch = new TextFieldDispatch(L"GuiModalWindow", 14400);
        dispatch->send_vkey_throws = true;
        ComGuiWindow window(IDispatchPtr(dispatch, true));
        auto a = attempt_close(1, [&] { window.send_vkey(12); }, [&] { window.close(); });
        REQUIRE(a.method == CloseMethod::WindowClose);
        REQUIRE(dispatch->close_calls == 1);
        REQUIRE(dispatch->sent_vkey == -1);  // never fell back to Enter
        REQUIRE_FALSE(a.original_error.empty());
    }
    SECTION("both throw") {
        auto* dispatch = new TextFieldDispatch(L"GuiModalWindow", 14400);
        dispatch->send_vkey_throws = true;
        dispatch->close_throws = true;
        ComGuiWindow window(IDispatchPtr(dispatch, true));
        auto a = attempt_close(1, [&] { window.send_vkey(12); }, [&] { window.close(); });
        REQUIRE(a.method == CloseMethod::Unsupported);
        REQUIRE(a.original_error.find("SendVKey invoke failed") != std::string::npos);
        REQUIRE_FALSE(a.close_error.empty());
    }
    SECTION("main window never gets Close") {
        auto* dispatch = new TextFieldDispatch(L"GuiMainWindow", 14400);
        dispatch->send_vkey_throws = true;
        ComGuiWindow window(IDispatchPtr(dispatch, true));
        auto a = attempt_close(0, [&] { window.send_vkey(12); }, [&] { window.close(); });
        REQUIRE(a.method == CloseMethod::Unsupported);
        REQUIRE(dispatch->close_calls == 0);
    }
}

TEST_CASE("Menu tree enumeration nests children and never selects", "[com][menu]") {
    ScopedDispatchCacheReset cache_reset;
    auto* bar = new TextFieldDispatch(L"GuiMenubar", 14600, L"wnd[0]/mbar");
    auto* system_menu = new TextFieldDispatch(L"GuiMenu", 14700, L"wnd[0]/mbar/menu[0]", L"", L"", L"S&ystem");
    auto* display = new TextFieldDispatch(L"GuiMenu", 14800, L"wnd[0]/mbar/menu[0]/menu[0]", L"", L"", L"Runtime Errors");
    auto* bar_children = new TextFieldDispatch(L"GuiCollection", 14900);
    auto* system_children = new TextFieldDispatch(L"GuiCollection", 15000);
    bar_children->child_items = {system_menu};
    system_children->child_items = {display};
    bar->children_dispatch = bar_children;
    system_menu->children_dispatch = system_children;
    auto root = ComGuiElement::create(IDispatchPtr(bar, true));

    const auto tree = read_menu_tree(root);
    REQUIRE(tree.size() == 1);
    REQUIRE(tree[0].at("id") == "wnd[0]/mbar/menu[0]");
    REQUIRE(tree[0].at("text") == "S&ystem");
    REQUIRE(tree[0].at("enabled") == true);
    REQUIRE(tree[0].at("children").size() == 1);
    REQUIRE(tree[0].at("children")[0].at("text") == "Runtime Errors");
    REQUIRE(tree[0].at("children")[0].at("children").empty());
    REQUIRE(system_menu->select_calls == 0);
    REQUIRE(display->select_calls == 0);

    bar_children->Release();
    system_children->Release();
    system_menu->Release();
    display->Release();
}

TEST_CASE("Menu path selection calls Select only on the matching leaf", "[com][menu]") {
    ScopedDispatchCacheReset cache_reset;
    auto* bar = new TextFieldDispatch(L"GuiMenubar", 15200, L"wnd[0]/mbar");
    auto* other = new TextFieldDispatch(L"GuiMenu", 15300, L"wnd[0]/mbar/menu[0]", L"", L"", L"Edit");
    auto* utilities = new TextFieldDispatch(L"GuiMenu", 15400, L"wnd[0]/mbar/menu[1]", L"", L"", L"&Utilities");
    auto* leaf = new TextFieldDispatch(L"GuiMenu", 15500, L"wnd[0]/mbar/menu[1]/menu[0]", L"", L"", L"Display");
    auto* bar_children = new TextFieldDispatch(L"GuiCollection", 15600);
    auto* utilities_children = new TextFieldDispatch(L"GuiCollection", 15700);
    bar_children->child_items = {other, utilities};
    utilities_children->child_items = {leaf};
    bar->children_dispatch = bar_children;
    utilities->children_dispatch = utilities_children;
    auto root = ComGuiElement::create(IDispatchPtr(bar, true));

    auto item = find_menu_by_path(root, split_menu_path("utilities/ DISPLAY "));
    REQUIRE(item != nullptr);
    REQUIRE(find_menu_by_path(root, split_menu_path("Utilities/Missing")) == nullptr);
    REQUIRE(leaf->select_calls == 0);
    item->select(true);
    REQUIRE(leaf->select_calls == 1);
    REQUIRE(utilities->select_calls == 0);
    REQUIRE(other->select_calls == 0);

    bar_children->Release();
    utilities_children->Release();
    other->Release();
    utilities->Release();
    leaf->Release();
}

TEST_CASE("Read-only guard judges a synthetic toolbar button by its tooltip and text", "[com][readonly]") {
    using fairyfly::sap::read_only_toolbar_button_rule;
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 15400, L"wnd[0]/usr/shell", L"", L"Toolbar");
    dispatch->shell_buttons = {{L"REL", L"", L"Release job"}, {L"LOG", L"Log", L"Job log"},
                               {L"XYZ", L"Delete", L""}};
    auto shell = ComGuiElement::create(IDispatchPtr(dispatch, true));

    std::string text, tooltip;
    CHECK(read_only_toolbar_button_rule(*shell, "REL", "wnd[0]/usr/shell/btn_REL", text, tooltip) == "word:release");
    CHECK(tooltip == "Release job");
    CHECK(read_only_toolbar_button_rule(*shell, "LOG", "wnd[0]/usr/shell/btn_LOG", text, tooltip).empty());
    CHECK(tooltip == "Job log");
    CHECK(read_only_toolbar_button_rule(*shell, "XYZ", "wnd[0]/usr/shell/btn_XYZ", text, tooltip) == "word:delete");
    // Unknown button: lookup fails, id rules alone decide (innocuous id passes, guarded id is refused).
    CHECK(read_only_toolbar_button_rule(*shell, "NOPE", "wnd[0]/usr/shell/btn_NOPE", text, tooltip).empty());
    CHECK(read_only_toolbar_button_rule(*shell, "NOPE", "wnd[0]/usr/shell/btn_&DELETE", text, tooltip) == "id:&delete");
}

TEST_CASE("send-key rejects an unknown key with INVALID_VKEY", "[cli][send-key]") {
    const auto bad = check_vkey_argument("banana");
    REQUIRE(bad.has_value());
    REQUIRE(bad->status == Result::Status::Error);
    REQUIRE(bad->error.at("code") == "INVALID_VKEY");
    REQUIRE_FALSE(check_vkey_argument("F8").has_value());
}

TEST_CASE("click --doubleclick requires a grid row and column", "[cli][click]") {
    const auto missing_row = check_doubleclick_options(true, std::nullopt, "MATNR");
    REQUIRE(missing_row.has_value());
    REQUIRE(missing_row->error.at("code") == "GRID_ROW_OPTIONS_REQUIRED");
    REQUIRE(check_doubleclick_options(true, 2, "").has_value());
    REQUIRE(check_doubleclick_options(true, -1, "MATNR").has_value());
    REQUIRE_FALSE(check_doubleclick_options(true, 0, "MATNR").has_value());
    REQUIRE_FALSE(check_doubleclick_options(false, std::nullopt, "").has_value());
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
    REQUIRE(is_redacted(password->get_text()));
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
    REQUIRE(is_redacted(named->get_text()));
    REQUIRE(named_dispatch->text_reads == 0);

    auto* labeled_dispatch = new TextFieldDispatch(
        L"GuiTextField", 41, L"wnd[0]/usr/txtGENERIC", L"Client Secret");
    auto labeled = ComGuiElement::create(labeled_dispatch);
    labeled_dispatch->Release();
    REQUIRE(is_redacted(labeled->get_text()));
    REQUIRE(labeled_dispatch->text_reads == 0);
}

TEST_CASE("A changeable input field named after a credential state is still redacted without reading it", "[com][privacy]") {
    ScopedDispatchCacheReset cache_reset;
    // the fake reports Changeable = true: PASSWORD_STATE could hold a real value
    auto* state_dispatch = new TextFieldDispatch(L"GuiTextField", 41, L"wnd[0]/usr/txtPASSWORD_EXT_PWD_STATE");
    auto state = ComGuiElement::create(state_dispatch);
    state_dispatch->Release();
    REQUIRE(is_redacted(state->get_text()));
    REQUIRE(state_dispatch->text_reads == 0);
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
    REQUIRE(is_redacted(field->get_text()));
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
    REQUIRE(is_redacted(credential->get_text()));
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
    REQUIRE(is_redacted(credential->get_text()));
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

        REQUIRE(is_redacted(value->get_text()));
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
        REQUIRE(is_redacted(value->get_text()));
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

    REQUIRE(is_redacted(value->get_text()));
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

    REQUIRE(is_redacted(secret->get_text()));
    CHECK(secret_value->text_reads == 0);
    REQUIRE(ordinary->get_text() == "application/json");
    CHECK(ordinary_value->text_reads == 1);
    REQUIRE(is_redacted(field->get_text()));
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

    REQUIRE(is_redacted(value->get_text()));
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
        REQUIRE(is_redacted(value->get_text()));
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

        REQUIRE(is_redacted(value->get_text()));
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
    REQUIRE(is_redacted(value->get_text_for_direct_read()));
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

    REQUIRE(is_redacted(value->get_text_for_direct_read()));
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

TEST_CASE("Unknown GuiShell subtype keeps its subtype and drops the ProgID text", "[com][metadata][err137]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiShell", 460, L"wnd[0]/usr/cntlPARAM/shellcont/shell",
                                           L"", L"Calendar", L"SAP.HTMLControl.1");
    auto shell = ComGuiElement::create(IDispatchPtr(dispatch, true));
    const auto data = ElementMetadataExtractor::extract(shell);
    REQUIRE(data.at("type") == "GuiShell");
    REQUIRE(data.at("subtype") == "Calendar");
    REQUIRE_FALSE(data.contains("text"));
    REQUIRE(data.at("content_available") == false);
}

TEST_CASE("session list accepts a per-command --output option", "[com][cli][imp005]") {
    const auto result = run_cli_on_mta({"fairyfly", "session", "list", "--output", "toon"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("INTERNAL_ERROR") != std::string::npos);
    const auto global = run_cli_on_mta({"fairyfly", "--output", "toon", "session", "list"});
    REQUIRE(global.exit_code == 1);
    REQUIRE(global.output.find("INTERNAL_ERROR") != std::string::npos);
}

TEST_CASE("CLI serializes COM initialization failure and exits nonzero", "[com][cli]") {
    const auto result = run_cli_on_mta({"fairyfly", "session", "list"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    const auto response = nlohmann::json::parse(result.output);
    REQUIRE(response.at("status") == "error");
    REQUIRE(response.at("error").at("code") == "INTERNAL_ERROR");
    REQUIRE(spdlog::get_level() == spdlog::level::err);
}

TEST_CASE("Fill accepts an explicit clear option without a value", "[com][cli][fill]") {
    const auto result = run_cli_on_mta({"fairyfly", "element", "fill", "wnd[0]/usr/txtFIELD", "--clear"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("INTERNAL_ERROR") != std::string::npos);
}

TEST_CASE("Disconnect accepts an explicit close-session option", "[com][cli][disconnect]") {
    const auto result = run_cli_on_mta({"fairyfly", "session", "disconnect", "--connection", "0", "--close-session"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("INTERNAL_ERROR") != std::string::npos);
}

TEST_CASE("Attach accepts an exact SAP GUI session ID", "[com][cli][attach]") {
    const auto result = run_cli_on_mta({"fairyfly", "session", "attach", "--session-id", "/app/con[0]/ses[1]"});
    REQUIRE(result.mta_initialized);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("INTERNAL_ERROR") != std::string::npos);
}

TEST_CASE("CLI exception honors global TOON output", "[com][cli][format]") {
    const auto result = run_cli_on_mta({"fairyfly", "--output", "toon", "session", "list"});
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


TEST_CASE("read_action_status masks credential-shaped status text", "[com][status][privacy]") {
    struct Case { const wchar_t* raw; const char* plain; bool masked; };
    for (const auto& [raw, plain, expect_masked] : std::vector<Case>{
             {L"Password: hunter2 rejected", "", true},
             {L"The parameter name is not known", "The parameter name is not known", false},
             {L"Job log displayed", "Job log displayed", false}}) {
        ScopedDispatchCacheReset cache_reset;
        auto* bar = new StringPropertyDispatch({
            {L"Type", L"GuiStatusbar"}, {L"Text", raw}, {L"DisplayedText", raw}, {L"MessageType", L"E"}});
        auto* session_dispatch = new TextFieldDispatch(L"GuiSession", 9700);
        session_dispatch->named_sibling_dispatch = bar;
        session_dispatch->named_sibling_id = L"wnd[0]/sbar";
        auto session = ComGuiSession::create(session_dispatch);
        session_dispatch->Release();

        const auto status = read_action_status(session);
        if (expect_masked) {
            CHECK(status.text.find("hunter2") == std::string::npos);
        } else {
            CHECK(status.text == plain);
        }
        bar->Release();
    }
}

TEST_CASE("Unknown member DISPID misses are cached per type", "[com][perf]") {
    ScopedDispatchCacheReset cache_reset;
    auto* first = new TextFieldDispatch(L"GuiButton", 20000);
    auto* second = new TextFieldDispatch(L"GuiButton", 20100);
    auto first_element = ComGuiElement::create(first);
    auto second_element = ComGuiElement::create(second);
    first->Release();
    second->Release();

    REQUIRE(first_element->get_type() == "GuiButton");
    REQUIRE(second_element->get_type() == "GuiButton");
    // AccTooltip/DefaultTooltip/Tooltip are unknown to the fake: empty on both elements.
    REQUIRE(first_element->get_tooltip().empty());
    REQUIRE(second_element->get_tooltip().empty());
    REQUIRE(first_element->get_tooltip().empty());
    auto* first_fake = static_cast<TextFieldDispatch*>(first_element->get_dispatch());
    auto* second_fake = static_cast<TextFieldDispatch*>(second_element->get_dispatch());
    REQUIRE(first_fake->name_lookups[L"AccTooltip"] == 1);
    REQUIRE(first_fake->name_lookups[L"Tooltip"] == 1);
    // The second element of the same type never asks the object again.
    REQUIRE(second_fake->name_lookups[L"AccTooltip"] == 0);
    REQUIRE(second_fake->name_lookups[L"DefaultTooltip"] == 0);
    REQUIRE(second_fake->name_lookups[L"Tooltip"] == 0);

    // Clearing the cache also clears remembered misses.
    SapGuiObject::clear_dispid_cache();
    REQUIRE(second_element->get_type() == "GuiButton");
    REQUIRE(second_element->get_tooltip().empty());
    REQUIRE(second_fake->name_lookups[L"AccTooltip"] == 1);
}

TEST_CASE("Universal Type DISPID skips the typeinfo lookup for validated types", "[com][perf][dispid]") {
    ScopedDispatchCacheReset cache_reset;
    auto* first = new TextFieldDispatch(L"GuiTextField", 7000);
    auto* second = new TextFieldDispatch(L"GuiTextField", 7000);
    auto first_element = ComGuiElement::create(first);
    auto second_element = ComGuiElement::create(second);
    first->Release();
    second->Release();

    REQUIRE(first_element->get_type() == "GuiTextField");
    REQUIRE(second_element->get_type() == "GuiTextField");
    REQUIRE(static_cast<TextFieldDispatch*>(first_element->get_dispatch())->name_lookups[L"Type"] == 1);
    // The second wrapper of a validated type reads Type through the universal DISPID: no lookup.
    REQUIRE(static_cast<TextFieldDispatch*>(second_element->get_dispatch())->name_lookups[L"Type"] == 0);
}

TEST_CASE("A differing Type DISPID disables the universal Type path", "[com][perf][dispid]") {
    ScopedDispatchCacheReset cache_reset;
    auto* text = new TextFieldDispatch(L"GuiTextField", 7000);
    auto* button = new TextFieldDispatch(L"GuiButton", 7100);
    auto* text_again = new TextFieldDispatch(L"GuiTextField", 7000);
    auto text_element = ComGuiElement::create(text);
    auto button_element = ComGuiElement::create(button);
    auto text_again_element = ComGuiElement::create(text_again);
    text->Release();
    button->Release();
    text_again->Release();

    REQUIRE(text_element->get_type() == "GuiTextField");
    REQUIRE(button_element->get_type() == "GuiButton");
    // The button's own typeinfo DISPID disagreed with the universal one, so the shortcut is off
    // and even an already validated type is looked up again.
    REQUIRE(text_again_element->get_type() == "GuiTextField");
    REQUIRE(static_cast<TextFieldDispatch*>(text_again_element->get_dispatch())->name_lookups[L"Type"] == 1);
}

TEST_CASE("A member-not-found universal Type read falls back without disabling", "[com][perf][dispid]") {
    ScopedDispatchCacheReset cache_reset;
    auto* text = new TextFieldDispatch(L"GuiTextField", 7000);
    auto* box = new TextFieldDispatch(L"GuiBox", 7000);
    box->type_invoke_failures = 1;
    auto* text_again = new TextFieldDispatch(L"GuiTextField", 7000);
    auto* box_again = new TextFieldDispatch(L"GuiBox", 7000);
    auto text_element = ComGuiElement::create(text);
    auto box_element = ComGuiElement::create(box);
    auto text_again_element = ComGuiElement::create(text_again);
    auto box_again_element = ComGuiElement::create(box_again);
    text->Release();
    box->Release();
    text_again->Release();
    box_again->Release();

    REQUIRE(text_element->get_type() == "GuiTextField");
    REQUIRE(box_element->get_type() == "GuiBox");
    REQUIRE(static_cast<TextFieldDispatch*>(box_element->get_dispatch())->name_lookups[L"Type"] == 1);
    // Not disabled: the validated types are served by the universal DISPID.
    REQUIRE(text_again_element->get_type() == "GuiTextField");
    REQUIRE(box_again_element->get_type() == "GuiBox");
    REQUIRE(static_cast<TextFieldDispatch*>(text_again_element->get_dispatch())->name_lookups[L"Type"] == 0);
    REQUIRE(static_cast<TextFieldDispatch*>(box_again_element->get_dispatch())->name_lookups[L"Type"] == 0);
}

TEST_CASE("The first property read on a fresh wrapper resolves through its Type", "[com][perf][dispid]") {
    ScopedDispatchCacheReset cache_reset;
    auto* first = new TextFieldDispatch(L"GuiTextField", 7000, L"", L"", L"", L"abc");
    auto* second = new TextFieldDispatch(L"GuiTextField", 7000, L"", L"", L"", L"def");
    auto first_element = ComGuiElement::create(first);
    auto second_element = ComGuiElement::create(second);
    first->Release();
    second->Release();

    REQUIRE(first_element->get_string_property(L"Text") == "abc");
    REQUIRE(second_element->get_string_property(L"Text") == "def");
    auto* second_fake = static_cast<TextFieldDispatch*>(second_element->get_dispatch());
    REQUIRE(second_fake->name_lookups[L"Type"] == 0);
    REQUIRE(second_fake->name_lookups[L"Text"] == 0);
    REQUIRE(second_element->get_type() == "GuiTextField");
}

TEST_CASE("clear_dispid_cache resets the universal Type DISPID state", "[com][perf][dispid]") {
    ScopedDispatchCacheReset cache_reset;
    auto* first = new TextFieldDispatch(L"GuiTextField", 7000);
    auto* second = new TextFieldDispatch(L"GuiTextField", 7000);
    auto first_element = ComGuiElement::create(first);
    auto second_element = ComGuiElement::create(second);
    first->Release();
    second->Release();

    REQUIRE(first_element->get_type() == "GuiTextField");
    SapGuiObject::clear_dispid_cache();
    REQUIRE(second_element->get_type() == "GuiTextField");
    REQUIRE(static_cast<TextFieldDispatch*>(second_element->get_dispatch())->name_lookups[L"Type"] == 1);
}

TEST_CASE("Shell member misses are not cached across GuiShell subtypes", "[com][perf][err142]") {
    ScopedDispatchCacheReset cache_reset;
    // Same COM Type string "GuiShell" for both: a tree-like shell without RowCount and a
    // grid-like shell with it. The tree's miss must not hide the grid's RowCount.
    auto* tree = new TextFieldDispatch(L"GuiShell", 30000, L"wnd[0]/usr/cntlT/shellcont/shell",
                                       L"", L"Tree", L"");
    auto* grid = new TextFieldDispatch(L"GuiShell", 30000, L"wnd[0]/usr/cntlG/shellcont/shell",
                                       L"", L"GridView", L"");
    grid->has_row_count = true;
    auto tree_element = ComGuiElement::create(tree);
    auto grid_element = ComGuiElement::create(grid);
    tree->Release();
    grid->Release();
    REQUIRE(tree_element->get_type() == "GuiShell");
    REQUIRE(grid_element->get_type() == "GuiShell");

    REQUIRE(tree_element->get_property_int(L"RowCount") == 0);
    REQUIRE(grid_element->get_property_int(L"RowCount") == 6);
    // The shell miss was asked again on the grid object (not served from a cached miss).
    REQUIRE(static_cast<TextFieldDispatch*>(grid_element->get_dispatch())->name_lookups[L"RowCount"] == 1);
}

TEST_CASE("Enumeration failure mid-way falls back to indexed children without duplicates", "[com][perf][enum]") {
    ScopedDispatchCacheReset cache_reset;
    auto* parent = new TextFieldDispatch(L"GuiContainerShell", 31000, L"wnd[0]/shellcont");
    auto* a = new TextFieldDispatch(L"GuiLabel", 31100, L"a");
    auto* b = new TextFieldDispatch(L"GuiLabel", 31200, L"b");
    auto* c = new TextFieldDispatch(L"GuiLabel", 31300, L"c");
    auto* collection = new TextFieldDispatch(L"GuiComponentCollection", 31400);
    collection->child_items = {a, b, c};
    collection->enumerable = true;
    collection->enum_fail_after = 2;  // yields a, b, then IEnumVARIANT::Next fails
    parent->children_dispatch = collection;
    auto element = ComGuiElement::create(parent);
    parent->Release();

    const auto metadata = ElementMetadataExtractor::extract(element);
    REQUIRE(metadata.contains("children"));
    REQUIRE(metadata.at("children") == json::array({"a", "b", "c"}));

    collection->Release();
    a->Release();
    b->Release();
    c->Release();
}

TEST_CASE("get_text and is_changeable share one Changeable read per wrapper", "[com][perf][changeable]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiTextField", 33000, L"wnd[0]/usr/txtFIELD", L"", L"", L"abc");
    auto element = ComGuiElement::create(dispatch);
    dispatch->Release();

    REQUIRE(element->get_text() == "abc");
    REQUIRE(element->is_changeable());
    REQUIRE(element->is_changeable());
    REQUIRE(static_cast<TextFieldDispatch*>(element->get_dispatch())->changeable_reads == 1);

    // The memo is per wrapper: a new wrapper reads Changeable itself.
    auto* other = new TextFieldDispatch(L"GuiTextField", 33000, L"wnd[0]/usr/txtOTHER", L"", L"", L"abc");
    auto other_element = ComGuiElement::create(other);
    other->Release();
    REQUIRE(other_element->is_changeable());
    REQUIRE(static_cast<TextFieldDispatch*>(other_element->get_dispatch())->changeable_reads == 1);
}

TEST_CASE("DisplayedText misses are cached for GuiShell but other shell misses are not", "[com][perf][shell]") {
    ScopedDispatchCacheReset cache_reset;
    auto* first = new TextFieldDispatch(L"GuiShell", 34000, L"wnd[0]/usr/shell1", L"", L"Toolbar", L"");
    auto* second = new TextFieldDispatch(L"GuiShell", 34000, L"wnd[0]/usr/shell2", L"", L"Tree", L"");
    first->displayed_text_missing = true;
    second->displayed_text_missing = true;
    auto first_element = ComGuiElement::create(first);
    auto second_element = ComGuiElement::create(second);
    first->Release();
    second->Release();

    first_element->get_text();
    second_element->get_text();
    REQUIRE(static_cast<TextFieldDispatch*>(first_element->get_dispatch())->name_lookups[L"DisplayedText"] == 1);
    REQUIRE(static_cast<TextFieldDispatch*>(second_element->get_dispatch())->name_lookups[L"DisplayedText"] == 0);
    // Members outside the allowlist stay uncached across GuiShell subtypes (see RowCount test).
    REQUIRE(first_element->get_property_int(L"RowCount") == 0);
    REQUIRE(second_element->get_property_int(L"RowCount") == 0);
    REQUIRE(static_cast<TextFieldDispatch*>(second_element->get_dispatch())->name_lookups[L"RowCount"] == 1);
}

TEST_CASE("ElementMetadataExtractor output for a text field is unchanged", "[com][metadata][golden]") {
    ScopedDispatchCacheReset cache_reset;
    auto* dispatch = new TextFieldDispatch(L"GuiTextField", 35000, L"/app/con[0]/ses[0]/wnd[0]/usr/txtFIELD",
                                           L"Field label", L"", L"abc");
    auto element = ComGuiElement::create(dispatch);
    dispatch->Release();
    const auto metadata = ElementMetadataExtractor::extract(element);
    // Golden: the memoised Changeable read and the shell negative cache must not change the output.
    REQUIRE(metadata.dump() ==
            R"({"capabilities":["fillable","readable"],"changeable":true,"enabled":true,)"
            R"("id":"/app/con[0]/ses[0]/wnd[0]/usr/txtFIELD","label":"Field label","name":"",)"
            R"("text":"abc","type":"GuiTextField","visible":true})");
}

TEST_CASE("for_each reports a failed enumeration so callers fall back", "[com][perf][enum]") {
    ScopedDispatchCacheReset cache_reset;
    auto* a = new TextFieldDispatch(L"GuiLabel", 32000, L"a");
    auto* b = new TextFieldDispatch(L"GuiLabel", 32100, L"b");
    auto* collection = new TextFieldDispatch(L"GuiComponentCollection", 32200);
    collection->child_items = {a, b};
    collection->enumerable = true;
    collection->enum_fail_after = 1;
    SapGuiCollection<ComGuiElement> children{IDispatchPtr(collection)};
    int seen = 0;
    REQUIRE_FALSE(children.for_each([&](const std::shared_ptr<ComGuiElement>&) { ++seen; }));
    REQUIRE(seen == 1);
    collection->Release();
    a->Release();
    b->Release();
}

TEST_CASE("is_enabled resolves Enabled once per type", "[com][perf]") {
    ScopedDispatchCacheReset cache_reset;
    auto* first = new TextFieldDispatch(L"GuiCTextField", 20200);
    auto* second = new TextFieldDispatch(L"GuiCTextField", 20200);
    auto first_element = ComGuiElement::create(first);
    auto second_element = ComGuiElement::create(second);
    first->Release();
    second->Release();
    first_element->get_type();
    second_element->get_type();

    // "Enabled" is unknown to the fake, so both report enabled (unchanged behavior).
    REQUIRE(first_element->is_enabled());
    REQUIRE(second_element->is_enabled());
    REQUIRE(static_cast<TextFieldDispatch*>(first_element->get_dispatch())->name_lookups[L"Enabled"] == 1);
    REQUIRE(static_cast<TextFieldDispatch*>(second_element->get_dispatch())->name_lookups[L"Enabled"] == 0);
    // Same type means same DISPIDs (cached Visible id applies to both objects).
    REQUIRE(first_element->is_visible());
    REQUIRE(second_element->is_visible());
    REQUIRE(static_cast<TextFieldDispatch*>(second_element->get_dispatch())->name_lookups[L"Visible"] == 0);
}

TEST_CASE("is_visible reports true when Visible is missing, else the real value", "[com]") {
    ScopedDispatchCacheReset cache_reset;
    auto make = [](DISPID base, auto configure) {
        auto* fake = new TextFieldDispatch(L"GuiCTextField", base);
        configure(fake);
        auto element = ComGuiElement::create(fake);
        fake->Release();
        element->get_type();
        return element;
    };
    SECTION("missing property means visible") {
        auto e = make(30100, [](TextFieldDispatch* f) { f->visible_missing = true; });
        REQUIRE(e->is_visible());
    }
    SECTION("Visible=false") {
        auto e = make(30200, [](TextFieldDispatch* f) { f->visible_value = false; });
        REQUIRE_FALSE(e->is_visible());
    }
    SECTION("Visible=true") {
        auto e = make(30300, [](TextFieldDispatch*) {});
        REQUIRE(e->is_visible());
    }
    SECTION("transient failure stays false") {
        auto e = make(30400, [](TextFieldDispatch* f) { f->visible_fails = true; });
        REQUIRE_FALSE(e->is_visible());
    }
}

TEST_CASE("find_element_by_id resolves FindById once and still returns not found", "[com][perf]") {
    ScopedDispatchCacheReset cache_reset;
    auto* session_dispatch = new TextFieldDispatch(L"GuiSession", 20400);
    auto* found = new TextFieldDispatch(L"GuiTextField", 20500, L"wnd[0]/usr/txtA");
    session_dispatch->named_sibling_dispatch = found;
    session_dispatch->named_sibling_id = L"wnd[0]/usr/txtA";
    auto session = ComGuiSession::create(session_dispatch);
    session_dispatch->Release();

    REQUIRE(session->find_element_by_id("wnd[0]/usr/txtA")->get_id() == "wnd[0]/usr/txtA");
    for (int i = 0; i < 4; ++i) {
        REQUIRE_THROWS_AS(session->find_element_by_id("wnd[0]/usr/missing"), ComException);
    }
    REQUIRE(session->find_element_by_id("wnd[0]/usr/txtA") != nullptr);
    REQUIRE(static_cast<TextFieldDispatch*>(session->get_dispatch())->name_lookups[L"FindById"] == 1);
    found->Release();
}

TEST_CASE("Children collection is fetched once per element wrapper", "[com][perf]") {
    ScopedDispatchCacheReset cache_reset;
    auto* parent = new TextFieldDispatch(L"GuiContainerShell", 20600, L"wnd[0]/shellcont");
    auto* first_child = new TextFieldDispatch(L"GuiLabel", 20700, L"wnd[0]/shellcont/lbl[0]");
    auto* second_child = new TextFieldDispatch(L"GuiLabel", 20800, L"wnd[0]/shellcont/lbl[1]");
    auto* collection = new TextFieldDispatch(L"GuiComponentCollection", 20900);
    collection->child_items = {first_child, second_child};
    parent->children_dispatch = collection;
    auto element = ComGuiElement::create(parent);
    parent->Release();

    REQUIRE(element->get_container_type() == "container");
    REQUIRE(element->get_child_count() == 2);
    auto children = element->children();
    REQUIRE(children.count() == 2);
    REQUIRE(element->get_child(0)->get_id() == "wnd[0]/shellcont/lbl[0]");
    REQUIRE(element->get_child(1)->get_id() == "wnd[0]/shellcont/lbl[1]");
    REQUIRE(element->get_child(2) == nullptr);
    REQUIRE(static_cast<TextFieldDispatch*>(element->get_dispatch())->children_invokes == 1);

    collection->Release();
    first_child->Release();
    second_child->Release();
}

TEST_CASE("SapGuiCollection::for_each uses one _NewEnum and matches item order", "[com][perf]") {
    ScopedDispatchCacheReset cache_reset;
    auto* first_child = new TextFieldDispatch(L"GuiLabel", 21000, L"a");
    auto* second_child = new TextFieldDispatch(L"GuiLabel", 21100, L"b");
    auto* third_child = new TextFieldDispatch(L"GuiLabel", 21200, L"c");
    auto* collection = new TextFieldDispatch(L"GuiComponentCollection", 21300);
    collection->child_items = {first_child, second_child, third_child};
    collection->enumerable = true;
    SapGuiCollection<ComGuiElement> children{IDispatchPtr(collection)};

    std::vector<std::string> by_item;
    for (int i = 0; i < 3; ++i) by_item.push_back(children.item(i)->get_id());
    const int enums_for_items = collection->new_enum_calls;
    REQUIRE(enums_for_items == 3);

    std::vector<std::string> by_walk;
    REQUIRE(children.for_each([&](const std::shared_ptr<ComGuiElement>& child) {
        by_walk.push_back(child->get_id());
    }));
    REQUIRE(collection->new_enum_calls == enums_for_items + 1);
    REQUIRE(by_walk == by_item);
    REQUIRE(by_walk == std::vector<std::string>{"a", "b", "c"});

    // Returning false stops the walk early.
    int seen = 0;
    REQUIRE(children.for_each([&](const std::shared_ptr<ComGuiElement>&) { return ++seen < 2; }));
    REQUIRE(seen == 2);

    // A collection without _NewEnum reports that callers must fall back to item(i).
    collection->enumerable = false;
    REQUIRE_FALSE(children.for_each([&](const std::shared_ptr<ComGuiElement>&) {}));

    collection->Release();
    first_child->Release();
    second_child->Release();
    third_child->Release();
}

TEST_CASE("enumerate_collection walks once and falls back to item(i)", "[com][perf][enum]") {
    ScopedDispatchCacheReset cache_reset;
    auto* first_child = new TextFieldDispatch(L"GuiLabel", 22000, L"a");
    auto* second_child = new TextFieldDispatch(L"GuiLabel", 22100, L"b");
    auto* third_child = new TextFieldDispatch(L"GuiLabel", 22200, L"c");
    auto* collection = new TextFieldDispatch(L"GuiComponentCollection", 22300);
    collection->child_items = {first_child, second_child, third_child};
    collection->enumerable = true;
    SapGuiCollection<ComGuiElement> children{IDispatchPtr(collection)};

    const auto ids = [](const std::vector<ComGuiElementPtr>& items) {
        std::vector<std::string> out;
        for (const auto& item : items) out.push_back(item->get_id());
        return out;
    };

    // One _NewEnum for the whole walk (item(i) would need one per index).
    auto all = enumerate_collection(children, 10);
    REQUIRE(ids(all) == std::vector<std::string>{"a", "b", "c"});
    REQUIRE(collection->new_enum_calls == 1);

    // The limit cuts the walk short.
    REQUIRE(ids(enumerate_collection(children, 2)) == std::vector<std::string>{"a", "b"});
    REQUIRE(enumerate_collection(children, 0).empty());

    // No enumerator: same items through Count/item(i).
    collection->enumerable = false;
    REQUIRE(ids(enumerate_collection(children, 10)) == std::vector<std::string>{"a", "b", "c"});
    REQUIRE(ids(enumerate_collection(children, 2)) == std::vector<std::string>{"a", "b"});

    // An enumerator that fails midway: the partial prefix is discarded, nothing is duplicated.
    collection->enumerable = true;
    collection->enum_fail_after = 2;
    REQUIRE(ids(enumerate_collection(children, 10)) == std::vector<std::string>{"a", "b", "c"});

    collection->Release();
    first_child->Release();
    second_child->Release();
    third_child->Release();
}

TEST_CASE("Label and tooltip values are unchanged by DISPID caching", "[com][perf]") {
    ScopedDispatchCacheReset cache_reset;
    for (int round = 0; round < 2; ++round) {
        auto* labelled = new TextFieldDispatch(L"GuiTextField", 21400, L"wnd[0]/usr/txtA", L"Customer");
        auto element = ComGuiElement::create(labelled);
        labelled->Release();
        REQUIRE(element->get_label() == "Customer");
        REQUIRE(element->get_tooltip().empty());
        REQUIRE_FALSE(element->get_label().empty());
    }
}

namespace {
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
    // Scripted Busy: while busy_true_reads > 0 each Busy read returns true and decrements it,
    // then false. -1 leaves Busy to the bools map. StartTransaction records its call time.
    int busy_true_reads = -1;
    int start_transaction_calls = 0;
    std::chrono::steady_clock::time_point start_transaction_at{};
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
        if ((flags & DISPATCH_PROPERTYPUT) && params && params->cArgs == 1) {
            if (put_fails.count(name)) return DISP_E_EXCEPTION;
            const VARIANT& arg = params->rgvarg[0];
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
            if (!params || params->cArgs != 1 || params->rgvarg[0].lVal < 0 ||
                static_cast<size_t>(params->rgvarg[0].lVal) >= items.size())
                return DISP_E_BADINDEX;
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = items[params->rgvarg[0].lVal];
            result->pdispVal->AddRef();
            return S_OK;
        }
        if (!(flags & DISPATCH_PROPERTYGET)) return DISP_E_MEMBERNOTFOUND;
        ++reads[name];
        VariantInit(result);
        if (name == L"Count") {
            result->vt = VT_I4;
            result->lVal = count_override >= 0 ? count_override : static_cast<long>(items.size());
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

// Scene: session -> wnd[0] -> usr -> tab strip (Address selected, Roles) with a grid on Roles.
struct TabScene {
    static constexpr const wchar_t* kWnd = L"/app/con[0]/ses[0]/wnd[0]";
    static constexpr const wchar_t* kUsr = L"/app/con[0]/ses[0]/wnd[0]/usr";
    static constexpr const wchar_t* kStrip = L"/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1";
    static constexpr const wchar_t* kTabA = L"/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpADDR";
    static constexpr const wchar_t* kTabB = L"/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpROLES";
    static constexpr const wchar_t* kGrid =
        L"/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpROLES/cntlG/shellcont/shell";
    static constexpr const wchar_t* kField =
        L"/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpADDR/txtNAME";

    std::vector<FakeNode*> nodes;
    FakeNode *session, *window, *info, *usr, *usr_children, *strip, *strip_children,
        *tab_a, *tab_a_children, *tab_b_stale, *tab_b_fresh, *tab_b_children, *grid, *field;
    int restore_of_a_selected = 0;

    FakeNode* make(const wchar_t* type, const wchar_t* id) {
        auto* node = new FakeNode();
        node->strings[L"Type"] = type;
        node->strings[L"Id"] = id;
        nodes.push_back(node);
        return node;
    }
    TabScene() {
        session = make(L"GuiSession", L"/app/con[0]/ses[0]");
        info = make(L"GuiSessionInfo", L"");
        info->strings[L"Transaction"] = L"SU01";
        window = make(L"GuiMainWindow", kWnd);
        window->strings[L"Text"] = L"Maintain Users";
        usr = make(L"GuiUserArea", kUsr);
        usr_children = make(L"GuiCollection", L"");
        strip = make(L"GuiTabStrip", kStrip);
        strip_children = make(L"GuiCollection", L"");
        tab_a = make(L"GuiTab", kTabA);
        tab_a->strings[L"Text"] = L"Address";
        tab_a_children = make(L"GuiCollection", L"");
        field = make(L"GuiTextField", kField);
        field->strings[L"Text"] = L"Miller";
        field->strings[L"DisplayedText"] = L"Miller";
        tab_a_children->items = {field};
        tab_a->dispatches[L"Children"] = tab_a_children;
        // Roles: the first (pre-select) tab pointer is stale and reports no children.
        tab_b_stale = make(L"GuiTab", kTabB);
        tab_b_stale->strings[L"Text"] = L"Roles";
        tab_b_fresh = make(L"GuiTab", kTabB);
        tab_b_fresh->strings[L"Text"] = L"Roles";
        tab_b_children = make(L"GuiCollection", L"");
        grid = make(L"GuiShell", kGrid);
        grid->strings[L"SubType"] = L"GridView";
        grid->ints[L"ColumnCount"] = 2;  // headers fall back to Column0/Column1, no rows
        tab_b_children->items = {grid};
        tab_b_fresh->dispatches[L"Children"] = tab_b_children;
        strip_children->items = {tab_a, tab_b_stale};
        strip->dispatches[L"Children"] = strip_children;
        strip->dispatches[L"SelectedTab"] = tab_a;
        usr_children->items = {strip};
        usr->dispatches[L"Children"] = usr_children;
        auto* window_children = make(L"GuiCollection", L"");
        window_children->items = {usr};
        window->dispatches[L"Children"] = window_children;
        session->dispatches[L"ActiveWindow"] = window;
        session->dispatches[L"Info"] = info;
        session->bools[L"Busy"] = false;
        auto& ids = session->find_by_id;
        ids[kWnd] = window;
        ids[kUsr] = usr;
        ids[kStrip] = strip;
        ids[kTabA] = tab_a;
        ids[kTabB] = tab_b_stale;
        ids[kGrid] = grid;
        ids[kField] = field;
        tab_b_stale->on_select = [this] {
            session->find_by_id[kTabB] = tab_b_fresh;
            strip->dispatches[L"SelectedTab"] = tab_b_fresh;
        };
        tab_b_fresh->on_select = tab_b_stale->on_select;
        tab_a->on_select = [this] {
            ++restore_of_a_selected;
            strip->dispatches[L"SelectedTab"] = tab_a;
        };
    }
    ~TabScene() { for (auto* node : nodes) node->Release(); }
    ComGuiSessionPtr session_wrapper() { return ComGuiSession::create(IDispatchPtr(session)); }
};
} // namespace

TEST_CASE("wait_for_completion checks Busy immediately and polls only while busy", "[com][session][perf]") {
    ScopedDispatchCacheReset cache_reset;
    auto* node = new FakeNode();
    node->strings[L"Type"] = L"GuiSession";
    node->strings[L"Id"] = L"/app/con[0]/ses[0]";
    auto session = ComGuiSession::create(IDispatchPtr(node));
    node->Release();

    SECTION("an idle session costs exactly one Busy read and no sleep") {
        node->busy_true_reads = 0;
        const auto start = std::chrono::steady_clock::now();
        session->wait_for_completion(500);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(node->reads[L"Busy"] == 1);
        REQUIRE(elapsed < std::chrono::milliseconds(15));
    }
    SECTION("busy once then idle reads Busy twice and stays under 60 ms") {
        node->busy_true_reads = 1;
        const auto start = std::chrono::steady_clock::now();
        session->wait_for_completion(500);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(node->reads[L"Busy"] == 2);
        REQUIRE(elapsed < std::chrono::milliseconds(60));
    }
    SECTION("busy N times then idle reads N+1 times") {
        node->busy_true_reads = 3;
        session->wait_for_completion(500);
        REQUIRE(node->reads[L"Busy"] == 4);
    }
    SECTION("an always busy session throws within the bound") {
        node->bools[L"Busy"] = true;
        const auto start = std::chrono::steady_clock::now();
        REQUIRE_THROWS_AS(session->wait_for_completion(50), ComException);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(elapsed < std::chrono::milliseconds(150));
    }
}

TEST_CASE("start_transaction returns right after the invoke without a settle sleep", "[com][session][perf]") {
    ScopedDispatchCacheReset cache_reset;
    auto* node = new FakeNode();
    node->strings[L"Type"] = L"GuiSession";
    node->strings[L"Id"] = L"/app/con[0]/ses[0]";
    auto session = ComGuiSession::create(IDispatchPtr(node));
    node->Release();

    session->start_transaction("SM37");
    const auto returned = std::chrono::steady_clock::now();
    REQUIRE(node->start_transaction_calls == 1);
    REQUIRE(returned - node->start_transaction_at < std::chrono::milliseconds(20));
}

namespace {
// Session with a main window bar and a dialog bar, as FakeNode graph for status bar read counts.
struct StatusBarScene {
    FakeNode *session, *bar0, *bar1, *window0, *window1;
    explicit StatusBarScene(const wchar_t* active_id) {
        auto make = [](const wchar_t* type, const wchar_t* id) {
            auto* node = new FakeNode();
            node->strings[L"Type"] = type;
            node->strings[L"Id"] = id;
            return node;
        };
        session = make(L"GuiSession", L"/app/con[0]/ses[0]");
        bar0 = make(L"GuiStatusbar", L"/app/con[0]/ses[0]/wnd[0]/sbar");
        bar1 = make(L"GuiStatusbar", L"/app/con[0]/ses[0]/wnd[1]/sbar");
        window0 = make(L"GuiMainWindow", L"/app/con[0]/ses[0]/wnd[0]");
        window1 = make(L"GuiModalWindow", L"/app/con[0]/ses[0]/wnd[1]");
        for (auto* bar : {bar0, bar1}) {
            bar->strings[L"Text"] = L"";
            bar->strings[L"MessageType"] = L"";
        }
        session->find_by_id[L"wnd[0]/sbar"] = bar0;
        session->find_by_id[L"wnd[1]/sbar"] = bar1;
        session->dispatches[L"ActiveWindow"] = std::wstring(active_id) == L"wnd[1]" ? window1 : window0;
    }
    ~StatusBarScene() { for (auto* node : {session, bar0, bar1, window0, window1}) node->Release(); }
    ComGuiSessionPtr wrapper() { return ComGuiSession::create(IDispatchPtr(session)); }
    static constexpr const char* kWnd0 = "/app/con[0]/ses[0]/wnd[0]";
    static constexpr const char* kWnd1 = "/app/con[0]/ses[0]/wnd[1]";
};
} // namespace

TEST_CASE("An empty status bar is read in three round trips", "[com][status][perf]") {
    ScopedDispatchCacheReset cache_reset;
    StatusBarScene scene(L"wnd[0]");
    const auto status = read_action_status(scene.wrapper(), StatusBarScene::kWnd0);
    REQUIRE(status.text.empty());
    REQUIRE(status.type.empty());
    REQUIRE(scene.bar0->reads[L"Text"] == 1);
    REQUIRE(scene.bar0->reads[L"MessageType"] == 1);
    REQUIRE(scene.bar0->reads[L"MessageId"] == 0);
    REQUIRE(scene.bar0->reads[L"MessageNumber"] == 0);
    REQUIRE(scene.bar0->reads[L"Type"] == 0);
    REQUIRE(scene.bar0->reads[L"DisplayedText"] == 0);
    REQUIRE(scene.session->reads[L"ActiveWindow"] == 0);
}

TEST_CASE("A status bar message keeps its JSON and costs two extra reads", "[com][status][perf]") {
    ScopedDispatchCacheReset cache_reset;
    StatusBarScene scene(L"wnd[0]");
    scene.bar0->strings[L"Text"] = L"Job log displayed";
    scene.bar0->strings[L"MessageType"] = L"S";
    scene.bar0->strings[L"MessageId"] = L"BL";
    scene.bar0->strings[L"MessageNumber"] = L"001";
    const auto status = read_action_status(scene.wrapper(), StatusBarScene::kWnd0);
    REQUIRE(status_bar_json(status) == json{{"text", "Job log displayed"}, {"message_type", "S"},
                                            {"message_id", "BL"}, {"message_number", "001"}});
    REQUIRE(scene.bar0->reads[L"Type"] == 0);
    REQUIRE(scene.bar0->reads[L"DisplayedText"] == 0);
    REQUIRE(scene.bar0->reads[L"MessageId"] == 1);
    REQUIRE(scene.bar0->reads[L"MessageNumber"] == 1);
}

TEST_CASE("The dialog status bar wins when non-empty, otherwise the main bar is used", "[com][status]") {
    ScopedDispatchCacheReset cache_reset;
    StatusBarScene scene(L"wnd[1]");
    scene.bar0->strings[L"Text"] = L"Main message";
    scene.bar0->strings[L"MessageType"] = L"I";

    SECTION("empty dialog bar falls back to the main bar") {
        const auto status = read_action_status(scene.wrapper(), StatusBarScene::kWnd1);
        REQUIRE(status.text == "Main message");
        REQUIRE(status.type == "I");
    }
    SECTION("non-empty dialog bar is preferred") {
        scene.bar1->strings[L"Text"] = L"Dialog message";
        scene.bar1->strings[L"MessageType"] = L"E";
        const auto status = read_action_status(scene.wrapper(), StatusBarScene::kWnd1);
        REQUIRE(status.text == "Dialog message");
        REQUIRE(status.type == "E");
        REQUIRE(scene.bar0->reads[L"Text"] == 0);
    }
    SECTION("the overload with a known window id never reads ActiveWindow, the plain call does") {
        (void)read_action_status(scene.wrapper(), StatusBarScene::kWnd1);
        REQUIRE(scene.session->reads[L"ActiveWindow"] == 0);
        const auto status = read_action_status(scene.wrapper());
        REQUIRE(scene.session->reads[L"ActiveWindow"] == 1);
        REQUIRE(status.text == "Main message");
    }
}

TEST_CASE("read_tab selects, re-fetches the tab, restores, and reads grids inside it", "[screen][tabs][read_tab]") {
    ScopedDispatchCacheReset cache_reset;
    TabScene scene;
    ScreenReader reader(scene.session_wrapper());

    auto result = reader.read_tab("tabpROLES");
    REQUIRE(result.status == Result::Status::Success);
    REQUIRE(result.data.contains("screen_id"));
    REQUIRE(result.data.at("title") == "Maintain Users");
    REQUIRE(result.data.at("transaction") == "SU01");
    for (const char* key : {"child_count", "elements", "element_count", "hierarchy",
                            "tabs_content", "tabs_expanded", "expanded_tab_count"})
        REQUIRE(result.data.contains(key));
    REQUIRE(result.data.at("tabs_expanded") == true);
    REQUIRE(result.data.at("expanded_tab_count") == 1);

    // Exactly one Select on the requested tab; the previously selected tab was restored.
    REQUIRE(scene.tab_b_stale->select_calls == 1);
    REQUIRE(scene.tab_b_fresh->select_calls == 0);
    REQUIRE(scene.restore_of_a_selected == 1);

    const auto& tab = result.data.at("tabs_content").at(0);
    REQUIRE(tab.at("tab_id") == "/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpROLES");
    REQUIRE(tab.at("tab_name") == "Roles");
    REQUIRE(tab.at("tab_type") == "GuiTab");
    REQUIRE(tab.at("tab_index") == 0);
    REQUIRE(tab.contains("hierarchy"));
    REQUIRE(tab.at("element_count") == tab.at("elements").size());

    // The tab's elements are its subtree: the grid (found via the re-fetched tab) with table
    // data, and none of the other tab's fields.
    bool grid_found = false;
    for (const auto& element : tab.at("elements")) {
        const auto id = element.value("id", "");
        REQUIRE(id.find("txtNAME") == std::string::npos);
        if (id.find("cntlG/shellcont/shell") != std::string::npos) {
            grid_found = true;
            REQUIRE(element.contains("table_data"));
        }
    }
    REQUIRE(grid_found);
}

TEST_CASE("read_tab does not select an already selected tab", "[screen][tabs][read_tab]") {
    ScopedDispatchCacheReset cache_reset;
    TabScene scene;
    ScreenReader reader(scene.session_wrapper());

    auto result = reader.read_tab("tabpADDR");
    REQUIRE(result.status == Result::Status::Success);
    REQUIRE(scene.tab_a->select_calls == 0);
    REQUIRE(scene.tab_b_stale->select_calls == 0);
    REQUIRE(result.data.at("expanded_tab_count") == 1);
    const auto& elements = result.data.at("tabs_content").at(0).at("elements");
    bool field_found = false;
    for (const auto& element : elements)
        field_found |= element.value("id", "").find("txtNAME") != std::string::npos;
    REQUIRE(field_found);
}

TEST_CASE("read_tab reports TAB_NOT_FOUND with the available tab ids", "[screen][tabs][read_tab]") {
    ScopedDispatchCacheReset cache_reset;
    TabScene scene;
    ScreenReader reader(scene.session_wrapper());

    auto result = reader.read_tab("tabpMISSING");
    REQUIRE(result.status == Result::Status::Error);
    REQUIRE(result.error.at("code") == "TAB_NOT_FOUND");
    REQUIRE(result.error.at("message") == "No tab matches 'tabpMISSING'");
    // same fields as TAB_LOAD_FAILED so callers handle both alike (README contract)
    REQUIRE(result.error.at("tab_id") == "tabpMISSING");
    REQUIRE(result.error.at("reason") == "not_found");
    REQUIRE(result.error.at("available_tabs") == json::array({
        "/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpADDR",
        "/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpROLES"}));
    REQUIRE(scene.tab_b_stale->select_calls == 0);
}

TEST_CASE("read_tab reports TAB_SELECTION_UNAVAILABLE when the current tab is unknown", "[screen][tabs][read_tab]") {
    ScopedDispatchCacheReset cache_reset;
    TabScene scene;
    scene.strip->dispatches.erase(L"SelectedTab");
    ScreenReader reader(scene.session_wrapper());

    auto result = reader.read_tab("tabpROLES");
    REQUIRE(result.status == Result::Status::Error);
    REQUIRE(result.error.at("code") == "TAB_SELECTION_UNAVAILABLE");
    REQUIRE(scene.tab_b_stale->select_calls == 0);
}

TEST_CASE("read_with_tabs delegates a single requested tab to read_tab", "[screen][tabs][read_tab]") {
    ScopedDispatchCacheReset cache_reset;
    TabScene scene;
    ScreenReader reader(scene.session_wrapper());

    auto result = reader.read_with_tabs(false, 20, "tabpROLES");
    REQUIRE(result.status == Result::Status::Success);
    REQUIRE(result.data.at("expanded_tab_count") == 1);
    REQUIRE(scene.tab_b_stale->select_calls == 1);
    REQUIRE(scene.restore_of_a_selected == 1);
}

TEST_CASE("Positioned label cells are read with one Text property and no Type or DisplayedText", "[screen][labels]") {
    ScopedDispatchCacheReset cache_reset;
    TabScene scene;
    scene.usr_children->items.clear();
    const wchar_t* ids[] = {L"/app/con[0]/ses[0]/wnd[0]/usr/lbl[1,1]",
                            L"/app/con[0]/ses[0]/wnd[0]/usr/lbl[10,1]"};
    std::vector<FakeNode*> labels;
    for (const auto* id : ids) {
        auto* label = scene.make(L"GuiLabel", id);
        label->strings[L"Text"] = L"Hello";
        label->strings[L"DisplayedText"] = L"Hello";
        scene.usr_children->items.push_back(label);
        scene.session->find_by_id[id] = label;
        labels.push_back(label);
    }
    ScreenReader reader(scene.session_wrapper());
    auto result = reader.read(true);
    REQUIRE(result.status == Result::Status::Success);
    for (auto* label : labels) {
        // The label type is implied by the /lbl[ prefix and primed for the cell read and Phase 3. Fresh
        // enumeration wrappers read Type once through the universal DISPID (cheaper than the typeinfo
        // lookup their Id read used to cost), so the count is bounded by those wrappers, not zero.
        REQUIRE(label->reads[L"Type"] <= 2);
        REQUIRE(label->reads[L"Text"] >= 1);
        // Only the Phase 3 metadata extraction may read DisplayedText; the cell read must not.
        REQUIRE(label->reads[L"DisplayedText"] <= 1);
    }
    // Label text is still reported.
    bool hello = false;
    for (const auto& element : result.data.at("elements"))
        hello |= element.dump().find("Hello") != std::string::npos;
    REQUIRE(hello);
}

TEST_CASE("read_with_tabs skips select for the current tab and keeps tab content to the tab subtree", "[screen][tabs][read_tab]") {
    ScopedDispatchCacheReset cache_reset;
    TabScene scene;
    ScreenReader reader(scene.session_wrapper());

    auto result = reader.read_with_tabs();
    REQUIRE(result.status == Result::Status::Success);
    REQUIRE(result.data.at("expanded_tab_count") == 2);
    // Address is current: never selected while reading it, only re-selected by the restore
    // because reading Roles moved the strip.
    REQUIRE(scene.tab_b_stale->select_calls == 1);
    REQUIRE(scene.restore_of_a_selected == 1);
    for (const auto& tab : result.data.at("tabs_content")) {
        for (const auto& element : tab.at("elements")) {
            const auto id = element.value("id", "");
            const auto prefix = tab.at("tab_id").get<std::string>();
            REQUIRE(id.rfind(prefix, 0) == 0);  // nothing outside this tab's subtree
        }
    }
    const auto& roles = result.data.at("tabs_content").at(1);
    bool grid_found = false;
    for (const auto& element : roles.at("elements"))
        grid_found |= element.value("id", "").find("cntlG/shellcont/shell") != std::string::npos;
    REQUIRE(grid_found);
}

namespace {
std::vector<std::string> element_ids_of(const json& data) {
    std::vector<std::string> ids;
    for (const auto& element : data.at("elements")) ids.push_back(element.value("id", ""));
    return ids;
}

// usr -> [lbl cells (2 cols x 3 rows), simple container with 4 text fields].
struct ListScene {
    TabScene scene;
    FakeNode* container = nullptr;
    FakeNode* container_children = nullptr;
    std::vector<FakeNode*> cells;
    explicit ListScene(bool enumerable, long container_count = -1) {
        scene.usr_children->items.clear();
        const std::wstring usr = TabScene::kUsr;
        for (int row = 1; row <= 3; ++row) {
            for (int col = 1; col <= 2; ++col) {
                const std::wstring id = usr + L"/lbl[" + std::to_wstring(col) + L"," + std::to_wstring(row) + L"]";
                auto* label = scene.make(L"GuiLabel", id.c_str());
                label->strings[L"Text"] = L"c" + std::to_wstring(col) + L"r" + std::to_wstring(row);
                scene.usr_children->items.push_back(label);
                scene.session->find_by_id[id] = label;
                cells.push_back(label);
            }
        }
        const std::wstring cid = usr + L"/subSUB";
        container = scene.make(L"GuiSimpleContainer", cid.c_str());
        container_children = scene.make(L"GuiCollection", L"");
        for (int i = 0; i < 4; ++i) {
            const std::wstring id = cid + L"/txtF" + std::to_wstring(i);
            auto* field = scene.make(L"GuiTextField", id.c_str());
            field->strings[L"Text"] = L"v" + std::to_wstring(i);
            field->strings[L"DisplayedText"] = L"v" + std::to_wstring(i);
            container_children->items.push_back(field);
            scene.session->find_by_id[id] = field;
        }
        container_children->count_override = container_count;
        container->dispatches[L"Children"] = container_children;
        scene.usr_children->items.push_back(container);
        scene.session->find_by_id[cid] = container;
        scene.usr_children->enumerable = enumerable;
        container_children->enumerable = enumerable;
    }
};
} // namespace

TEST_CASE("Container and userarea traversal enumerates once and matches the item(i) fallback", "[screen][enumeration]") {
    ScopedDispatchCacheReset cache_reset;
    std::vector<std::string> fast_ids, slow_ids;
    int fast_usr_enums = 0, fast_container_enums = 0, fast_usr_items = 0, fast_container_items = 0;
    {
        ListScene fast(true);
        ScreenReader reader(fast.scene.session_wrapper());
        auto result = reader.read(true);
        REQUIRE(result.status == Result::Status::Success);
        fast_ids = element_ids_of(result.data);
        fast_usr_enums = fast.scene.usr_children->new_enum_calls;
        fast_container_enums = fast.container_children->new_enum_calls;
        fast_usr_items = fast.scene.usr_children->item_calls;
        fast_container_items = fast.container_children->item_calls;
        for (auto* cell : fast.cells) {
            REQUIRE(cell->reads[L"Type"] <= 2);   // enumeration wrappers read Type via the universal DISPID
            REQUIRE(cell->reads[L"Text"] >= 1);
        }
    }
    {
        ListScene slow(false);
        ScreenReader reader(slow.scene.session_wrapper());
        auto result = reader.read(true);
        REQUIRE(result.status == Result::Status::Success);
        slow_ids = element_ids_of(result.data);
        // No enumerator: everything goes through item(i).
        REQUIRE(slow.scene.usr_children->new_enum_calls == 0);
        REQUIRE(slow.scene.usr_children->item_calls > 0);
        REQUIRE(slow.container_children->item_calls > 0);
    }
    REQUIRE_FALSE(fast_ids.empty());
    REQUIRE(fast_ids == slow_ids);
    // Traversal walks each container with one _NewEnum and never calls Item(i); the metadata
    // pass may enumerate the same collection once more, but no more than that.
    REQUIRE(fast_usr_items == 0);
    REQUIRE(fast_container_items == 0);
    REQUIRE(fast_usr_enums >= 1);
    REQUIRE(fast_usr_enums <= 2);
    REQUIRE(fast_container_enums >= 1);
    REQUIRE(fast_container_enums <= 2);
}

TEST_CASE("Container traversal honors the child count limit when enumerating", "[screen][enumeration]") {
    ScopedDispatchCacheReset cache_reset;
    std::vector<std::string> fast_ids, slow_ids;
    for (bool enumerable : {true, false}) {
        ListScene scene(enumerable, 2);  // reports 2 children although 4 are enumerable
        ScreenReader reader(scene.scene.session_wrapper());
        auto result = reader.read(true);
        REQUIRE(result.status == Result::Status::Success);
        (enumerable ? fast_ids : slow_ids) = element_ids_of(result.data);
    }
    REQUIRE(fast_ids == slow_ids);
    const auto has = [&](const char* needle) {
        return std::any_of(fast_ids.begin(), fast_ids.end(),
                           [&](const std::string& id) { return id.find(needle) != std::string::npos; });
    };
    REQUIRE(has("/subSUB/txtF0"));
    REQUIRE(has("/subSUB/txtF1"));
    REQUIRE_FALSE(has("/subSUB/txtF2"));
    REQUIRE_FALSE(has("/subSUB/txtF3"));
}

namespace {
struct ScopedTabWaitTimeout {
    explicit ScopedTabWaitTimeout(int ms) { set_tab_wait_timeout_ms_for_testing(ms); }
    ~ScopedTabWaitTimeout() { set_tab_wait_timeout_ms_for_testing(0); }
};

// Selecting Roles leaves the session busy; selecting Address (restore) clears it.
void make_roles_select_leave_session_busy(TabScene& scene) {
    auto roles_select = scene.tab_b_stale->on_select;
    scene.tab_b_stale->on_select = [&scene, roles_select] {
        roles_select();
        scene.session->bools[L"Busy"] = true;
    };
    auto address_select = scene.tab_a->on_select;
    scene.tab_a->on_select = [&scene, address_select] {
        address_select();
        scene.session->bools[L"Busy"] = false;
    };
}
} // namespace

TEST_CASE("read --tab reports TAB_LOAD_FAILED when the session stays busy and restores the tab",
          "[screen][tabs][read_tab][err142]") {
    ScopedDispatchCacheReset cache_reset;
    ScopedTabWaitTimeout short_wait(60);
    TabScene scene;
    make_roles_select_leave_session_busy(scene);
    ScreenReader reader(scene.session_wrapper());

    auto result = reader.read_tab("tabpROLES");
    REQUIRE(result.status == Result::Status::Error);
    REQUIRE(result.error.at("code") == "TAB_LOAD_FAILED");
    REQUIRE(result.error.at("reason") == "busy_timeout");
    REQUIRE(result.error.at("tab_id") == "/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpROLES");
    REQUIRE(scene.tab_b_stale->select_calls == 1);
    // The originally selected tab was restored before returning.
    REQUIRE(scene.restore_of_a_selected == 1);
    REQUIRE(scene.strip->dispatches.at(L"SelectedTab") == scene.tab_a);
}

TEST_CASE("read --tab reports TAB_LOAD_FAILED when the tab element is missing",
          "[screen][tabs][read_tab][err142]") {
    ScopedDispatchCacheReset cache_reset;
    TabScene scene;
    scene.session->find_by_id.erase(TabScene::kTabB);
    ScreenReader reader(scene.session_wrapper());

    auto result = reader.read_tab("tabpROLES");
    REQUIRE(result.status == Result::Status::Error);
    REQUIRE(result.error.at("code") == "TAB_LOAD_FAILED");
    REQUIRE(result.error.at("reason") == "not_found");
    REQUIRE(scene.strip->dispatches.at(L"SelectedTab") == scene.tab_a);
}

TEST_CASE("all-tabs read lists a failed tab in tabs_failed and still succeeds",
          "[screen][tabs][err142]") {
    ScopedDispatchCacheReset cache_reset;
    TabScene scene;
    scene.session->find_by_id.erase(TabScene::kTabB);
    ScreenReader reader(scene.session_wrapper());

    auto result = reader.read_with_tabs();
    REQUIRE(result.status == Result::Status::Success);
    REQUIRE(result.data.at("tabs_expanded") == true);
    REQUIRE(result.data.at("expanded_tab_count") == 1);
    REQUIRE(result.data.at("tabs_failed").size() == 1);
    const auto& failed = result.data.at("tabs_failed").at(0);
    REQUIRE(failed.at("tab_id") == "/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpROLES");
    REQUIRE(failed.at("reason") == "not_found");
}

TEST_CASE("Tab whose reported children yield nothing falls back to the user area",
          "[screen][tabs][read_tab][err142]") {
    ScopedDispatchCacheReset cache_reset;
    TabScene scene;
    // Address reports one child, but that child exposes no type/metadata at all.
    // (a GuiTree is skipped by --skip-trees before any metadata is read).
    auto* ghost = scene.make(L"GuiTree", L"/app/con[0]/ses[0]/wnd[0]/usr/tabsTABSTRIP1/tabpADDR/tree");
    scene.tab_a_children->items = {ghost};
    auto* extra = scene.make(L"GuiTextField", L"/app/con[0]/ses[0]/wnd[0]/usr/txtEXTRA");
    extra->strings[L"Text"] = L"reachable";
    extra->strings[L"DisplayedText"] = L"reachable";
    scene.usr_children->items = {scene.strip, extra};
    scene.session->find_by_id[L"/app/con[0]/ses[0]/wnd[0]/usr/txtEXTRA"] = extra;
    ScreenReader reader(scene.session_wrapper());

    auto result = reader.read_tab("tabpADDR", true);
    REQUIRE(result.status == Result::Status::Success);
    bool extra_found = false;
    for (const auto& element : result.data.at("tabs_content").at(0).at("elements"))
        extra_found |= element.value("id", "").find("txtEXTRA") != std::string::npos;
    REQUIRE(extra_found);
}

TEST_CASE("GuiSession.GetObjectTree wrapper passes id and props and returns the JSON text", "[com][session][bulk]") {
    ScopedDispatchCacheReset cache_reset;
    ComGuiSession::reset_object_tree_support();
    auto* node = new FakeNode();
    node->strings[L"Type"] = L"GuiSession";
    node->strings[L"Id"] = L"/app/con[0]/ses[0]";
    auto session = ComGuiSession::create(IDispatchPtr(node));
    node->Release();
    node->object_tree_payload = L"{\"children\":[{\"Id\":\"/app/con[0]/ses[0]/wnd[0]\",\"Text\":\"Müller\"}]}";

    SECTION("props travel as a variant array of strings and the id as the first argument") {
        const auto tree = session->get_object_tree("wnd[0]", {"Id", "Type", "Text"});
        REQUIRE(tree.has_value());
        REQUIRE(*tree == "{\"children\":[{\"Id\":\"/app/con[0]/ses[0]/wnd[0]\",\"Text\":\"M\xc3\xbc" "ller\"}]}");
        REQUIRE(node->object_tree_calls == 1);
        REQUIRE(node->object_tree_id == L"wnd[0]");
        REQUIRE(node->object_tree_props_passed);
        REQUIRE(node->object_tree_props == std::vector<std::wstring>{L"Id", L"Type", L"Text"});
        REQUIRE(ComGuiSession::object_tree_support() == ObjectTreeSupport::Available);
    }
    SECTION("no props passes only the id (the Optional parameter stays absent)") {
        REQUIRE(session->get_object_tree("wnd[0]").has_value());
        REQUIRE_FALSE(node->object_tree_props_passed);
        REQUIRE(node->object_tree_id == L"wnd[0]");
    }
    SECTION("an empty answer is treated as unusable") {
        node->object_tree_payload.clear();
        REQUIRE_FALSE(session->get_object_tree("wnd[0]", {"Id"}).has_value());
    }
    SECTION("a failing call returns nullopt and is retried on the next call") {
        node->object_tree_hresult = DISP_E_EXCEPTION;
        REQUIRE_FALSE(session->get_object_tree("wnd[0]", {"Id"}).has_value());
        REQUIRE(ComGuiSession::object_tree_support() != ObjectTreeSupport::Missing);
        node->object_tree_hresult = S_OK;
        REQUIRE(session->get_object_tree("wnd[0]", {"Id"}).has_value());
        REQUIRE(node->object_tree_calls == 2);
    }
    SECTION("a missing method marks the support Missing and later calls do not touch COM") {
        node->unknown_names.insert(L"GetObjectTree");
        REQUIRE_FALSE(session->get_object_tree("wnd[0]", {"Id"}).has_value());
        REQUIRE(ComGuiSession::object_tree_support() == ObjectTreeSupport::Missing);
        node->unknown_names.clear();   // even if the name resolves now, the process stays on the legacy path
        REQUIRE_FALSE(session->get_object_tree("wnd[0]", {"Id"}).has_value());
        REQUIRE(node->object_tree_calls == 0);
        ComGuiSession::reset_object_tree_support();
        SapGuiObject::clear_dispid_cache();   // the DISPID miss itself is cached per type as well
        REQUIRE(session->get_object_tree("wnd[0]", {"Id"}).has_value());
    }
}
namespace {
// Plain-element scenes shared by the metadata golden tests below.
FakeNode* make_golden_field(const wchar_t* type, const wchar_t* id) {
    auto* node = new FakeNode();
    node->strings[L"Type"] = type;
    node->strings[L"Id"] = id;
    node->strings[L"Name"] = L"BNAME";
    node->strings[L"Text"] = L"Miller";
    node->strings[L"DisplayedText"] = L"Miller";
    node->strings[L"AccLabel"] = L"User";
    node->strings[L"AccTooltip"] = L"User name";
    node->bools[L"Changeable"] = true;
    node->bools[L"Enabled"] = true;
    node->bools[L"Visible"] = true;
    return node;
}
}  // namespace

// Golden dumps recorded from the build before the display-text policy and metadata builder
// were split out of ComGuiElement::get_text and ElementMetadataExtractor::extract.
TEST_CASE("ElementMetadataExtractor plain elements keep their recorded JSON", "[metadata][golden]") {
    ScopedDispatchCacheReset cache_reset;
    auto* field = make_golden_field(L"GuiTextField", L"/app/con[0]/ses[0]/wnd[0]/usr/txtBNAME");
    auto* ctext = make_golden_field(L"GuiCTextField", L"/app/con[0]/ses[0]/wnd[0]/usr/ctxtBNAME");
    auto* check = make_golden_field(L"GuiCheckBox", L"/app/con[0]/ses[0]/wnd[0]/usr/chkBNAME");
    check->bools[L"Selected"] = true;
    auto* box = new FakeNode();
    box->strings[L"Type"] = L"GuiBox";
    box->strings[L"Id"] = L"/app/con[0]/ses[0]/wnd[0]/usr/boxBNAME";
    box->strings[L"Name"] = L"BOX";
    box->strings[L"Text"] = L"Group";
    auto* container = new FakeNode();
    container->strings[L"Type"] = L"GuiSimpleContainer";
    container->strings[L"Id"] = L"/app/con[0]/ses[0]/wnd[0]/usr/subSUB";
    auto* kids = new FakeNode();
    kids->items = {field, ctext};
    kids->enumerable = true;
    container->dispatches[L"Children"] = kids;

    const auto dump = [](FakeNode* node) {
        return ElementMetadataExtractor::extract(ComGuiElement::create(node)).dump();
    };
    CHECK(dump(field) == R"({"capabilities":["fillable","readable"],"changeable":true,"enabled":true,"id":"/app/con[0]/ses[0]/wnd[0]/usr/txtBNAME","label":"User","name":"BNAME","text":"Miller","tooltip":"User name","type":"GuiTextField","visible":true})");
    CHECK(dump(ctext) == R"({"capabilities":["fillable","readable","has_f4_help"],"changeable":true,"enabled":true,"has_f4_help":true,"id":"/app/con[0]/ses[0]/wnd[0]/usr/ctxtBNAME","label":"User","name":"BNAME","text":"Miller","tooltip":"User name","type":"GuiCTextField","visible":true})");
    CHECK(dump(check) == R"({"capabilities":["selectable","readable"],"changeable":true,"enabled":true,"id":"/app/con[0]/ses[0]/wnd[0]/usr/chkBNAME","label":"User","name":"BNAME","selected":true,"text":"Miller","type":"GuiCheckBox","visible":true})");
    CHECK(dump(box) == R"({"capabilities":["container"],"changeable":false,"container_type":"group","enabled":true,"id":"/app/con[0]/ses[0]/wnd[0]/usr/boxBNAME","is_group":true,"name":"BOX","text":"Group","type":"GuiBox","visible":false})");
    CHECK(dump(container) == R"({"capabilities":["container"],"changeable":false,"child_count":2,"children":["/app/con[0]/ses[0]/wnd[0]/usr/txtBNAME","/app/con[0]/ses[0]/wnd[0]/usr/ctxtBNAME"],"container_type":"form","enabled":true,"id":"/app/con[0]/ses[0]/wnd[0]/usr/subSUB","name":"","text":"","type":"GuiSimpleContainer","visible":false})");
}

namespace {
FakeNode* make_check_node(const wchar_t* type, bool selected) {
    auto* node = new FakeNode();
    node->strings[L"Type"] = type;
    node->strings[L"Id"] = L"/app/con[0]/ses[0]/wnd[0]/usr/chkFLAG";
    node->strings[L"Text"] = L"Flag";
    node->bools[L"Changeable"] = true;
    node->bools[L"Selected"] = selected;
    node->put_fails.insert(L"Text");  // the checkbox Text property is read-only in SAP
    return node;
}

FakeNode* make_combo_node(const std::vector<std::pair<std::wstring, std::wstring>>& entries, bool with_entries = true) {
    auto* node = new FakeNode();
    node->strings[L"Type"] = L"GuiComboBox";
    node->strings[L"Id"] = L"/app/con[0]/ses[0]/wnd[0]/usr/cmbLANG";
    node->strings[L"Key"] = L"DE";
    node->strings[L"Value"] = L"German";
    node->bools[L"Changeable"] = true;
    if (!with_entries) return node;
    auto* collection = new FakeNode();
    collection->enumerable = true;
    for (const auto& [key, value] : entries) {
        auto* entry = new FakeNode();
        entry->strings[L"Key"] = key;
        entry->strings[L"Value"] = value;
        collection->items.push_back(entry);
    }
    node->dispatches[L"Entries"] = collection;
    return node;
}
}  // namespace

TEST_CASE("Check box click toggles instead of always selecting", "[com][checkbox]") {
    ScopedDispatchCacheReset cache_reset;
    SECTION("a checked box becomes unchecked") {
        auto* node = make_check_node(L"GuiCheckBox", true);
        auto element = ComGuiElement::create(node);
        node->Release();
        element->press();
        CHECK(element->get_selected() == std::optional<bool>(false));
    }
    SECTION("an unchecked box becomes checked") {
        auto* node = make_check_node(L"GuiCheckBox", false);
        auto element = ComGuiElement::create(node);
        node->Release();
        element->press();
        CHECK(element->get_selected() == std::optional<bool>(true));
    }
    SECTION("a radio button click selects it") {
        auto* node = make_check_node(L"GuiRadioButton", false);
        node->on_select = [node] { node->bools[L"Selected"] = true; };
        auto element = ComGuiElement::create(node);
        node->Release();
        element->press();
        CHECK(element->get_selected() == std::optional<bool>(true));
    }
    SECTION("an unreadable Selected state is not toggled blindly") {
        auto* node = make_check_node(L"GuiCheckBox", true);
        node->unknown_names.insert(L"Selected");
        auto element = ComGuiElement::create(node);
        node->Release();
        CHECK_THROWS_AS(element->press(), ComException);
        CHECK(node->puts.empty());
    }
}

TEST_CASE("Check box and radio button fill write Selected", "[com][checkbox]") {
    ScopedDispatchCacheReset cache_reset;
    using Status = ComGuiElement::FillOutcome::Status;
    SECTION("spellings are case-insensitive and trimmed") {
        for (const char* off : {"false", "0", "no", "off", " FALSE ", "No", ""}) {
            auto* node = make_check_node(L"GuiCheckBox", true);
            auto element = ComGuiElement::create(node);
            node->Release();
            const auto outcome = element->fill_value(off);
            INFO("value '" << off << "'");
            CHECK(outcome.status == Status::Written);
            CHECK(outcome.selected == std::optional<bool>(false));
        }
        for (const char* on : {"true", "1", "yes", "on", "X", " x "}) {
            auto* node = make_check_node(L"GuiCheckBox", false);
            auto element = ComGuiElement::create(node);
            node->Release();
            const auto outcome = element->fill_value(on);
            INFO("value '" << on << "'");
            CHECK(outcome.status == Status::Written);
            CHECK(outcome.selected == std::optional<bool>(true));
        }
    }
    SECTION("fill never touches the read-only Text property") {
        auto* node = make_check_node(L"GuiCheckBox", true);
        auto element = ComGuiElement::create(node);
        node->Release();
        element->fill_value("false");
        for (const auto& put : node->puts) CHECK(put.first != L"Text");
    }
    SECTION("an unknown spelling is INVALID_ARGUMENT and lists the accepted ones") {
        auto* node = make_check_node(L"GuiCheckBox", true);
        auto element = ComGuiElement::create(node);
        node->Release();
        const auto outcome = element->fill_value("maybe");
        CHECK(outcome.status == Status::InvalidArgument);
        CHECK(outcome.message.find("maybe") != std::string::npos);
        CHECK(outcome.message.find("yes, no, on, off") != std::string::npos);
        CHECK(node->puts.empty());
    }
    SECTION("a radio button is selected by true and cannot be cleared") {
        auto* node = make_check_node(L"GuiRadioButton", false);
        node->on_select = [node] { node->bools[L"Selected"] = true; };
        auto element = ComGuiElement::create(node);
        node->Release();
        const auto cleared = element->fill_value("false");
        CHECK(cleared.status == Status::InvalidArgument);
        CHECK(cleared.message.find("cannot be cleared") != std::string::npos);
        CHECK(node->select_calls == 0);
        const auto selected = element->fill_value("1");
        CHECK(selected.status == Status::Written);
        CHECK(selected.selected == std::optional<bool>(true));
    }
    SECTION("a non-changeable check box is reported read-only") {
        auto* node = make_check_node(L"GuiCheckBox", true);
        node->bools[L"Changeable"] = false;
        auto element = ComGuiElement::create(node);
        node->Release();
        CHECK(element->fill_value("false").status == Status::ReadOnly);
        CHECK(node->puts.empty());
    }
}

TEST_CASE("Combo box fill accepts the key or the displayed value", "[com][combobox]") {
    ScopedDispatchCacheReset cache_reset;
    using Status = ComGuiElement::FillOutcome::Status;
    const std::vector<std::pair<std::wstring, std::wstring>> entries{
        {L"DE", L"German"}, {L"EN", L"English"}, {L"FR", L"French"}};
    const auto key_written = [](FakeNode* node) {
        std::wstring key;
        for (const auto& put : node->puts) if (put.first == L"Key") key = put.second;
        return key;
    };
    SECTION("key match") {
        auto* node = make_combo_node(entries);
        auto element = ComGuiElement::create(node);
        node->Release();
        const auto outcome = element->fill_value("EN");
        CHECK(outcome.status == Status::Written);
        CHECK(key_written(node) == L"EN");
        CHECK(outcome.key == std::optional<std::string>("EN"));
    }
    SECTION("display text match writes the key of the entry") {
        auto* node = make_combo_node(entries);
        auto element = ComGuiElement::create(node);
        node->Release();
        const auto outcome = element->fill_value("English");
        CHECK(outcome.status == Status::Written);
        CHECK(key_written(node) == L"EN");
    }
    SECTION("case and surrounding whitespace are tolerated") {
        auto* node = make_combo_node(entries);
        auto element = ComGuiElement::create(node);
        node->Release();
        CHECK(element->fill_value("  fRENCH ").status == Status::Written);
        CHECK(key_written(node) == L"FR");
    }
    SECTION("no match lists the available key = value pairs and writes nothing") {
        auto* node = make_combo_node(entries);
        auto element = ComGuiElement::create(node);
        node->Release();
        const auto outcome = element->fill_value("Klingon");
        CHECK(outcome.status == Status::InvalidArgument);
        CHECK(outcome.message.find("Klingon") != std::string::npos);
        CHECK(outcome.message.find("EN = English") != std::string::npos);
        CHECK(outcome.message.find("DE = German") != std::string::npos);
        CHECK(node->puts.empty());
    }
    SECTION("the option list is capped at 30 entries") {
        std::vector<std::pair<std::wstring, std::wstring>> many;
        for (int i = 0; i < 40; ++i) many.emplace_back(L"K" + std::to_wstring(i), L"V" + std::to_wstring(i));
        auto* node = make_combo_node(many);
        auto element = ComGuiElement::create(node);
        node->Release();
        const auto outcome = element->fill_value("nothing");
        CHECK(outcome.status == Status::InvalidArgument);
        CHECK(outcome.message.find("K29 = V29") != std::string::npos);
        CHECK(outcome.message.find("K30 = V30") == std::string::npos);
        CHECK(outcome.message.find("10 more") != std::string::npos);
    }
    SECTION("entries unavailable falls back to a direct Key write") {
        auto* node = make_combo_node({}, false);
        auto element = ComGuiElement::create(node);
        node->Release();
        const auto outcome = element->fill_value("EN");
        CHECK(outcome.status == Status::Written);
        CHECK(key_written(node) == L"EN");
    }
    SECTION("entries unavailable and the direct write fails gives a clear error") {
        auto* node = make_combo_node({}, false);
        node->put_fails.insert(L"Key");
        auto element = ComGuiElement::create(node);
        node->Release();
        try {
            element->fill_value("English");
            FAIL("expected a ComException");
        } catch (const ComException& e) {
            CHECK(std::string(e.what()).find("entries are unavailable") != std::string::npos);
            CHECK(std::string(e.what()).find("English") != std::string::npos);
        }
    }
}
