#include "include/html_viewer_reader.h"
#include "include/com/utf8.h"

#include <UIAutomation.h>
#include <wrl/client.h>
#include <cstdint>
#include <exception>
#include <chrono>
#include <thread>

namespace fairyfly::sap {

namespace {

HtmlViewerText read_once(HWND hwnd) {
    HtmlViewerText result;

    using Microsoft::WRL::ComPtr;
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&automation)))) return result;

    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) return result;

    VARIANT document_type;
    VariantInit(&document_type);
    document_type.vt = VT_I4;
    document_type.lVal = UIA_DocumentControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,
                                                   document_type, &condition))) return result;

    ComPtr<IUIAutomationElement> document;
    if (FAILED(root->FindFirst(TreeScope_Descendants, condition.Get(), &document)) ||
        !document) {
        // No Document control: accept any element that exposes a text pattern.
        VARIANT has_text;
        VariantInit(&has_text);
        has_text.vt = VT_BOOL;
        has_text.boolVal = VARIANT_TRUE;
        ComPtr<IUIAutomationCondition> text_condition;
        if (FAILED(automation->CreatePropertyCondition(UIA_IsTextPatternAvailablePropertyId,
                                                       has_text, &text_condition))) return result;
        document.Reset();
        if (FAILED(root->FindFirst(TreeScope_Subtree, text_condition.Get(), &document)) ||
            !document) return result;
    }

    ComPtr<IUnknown> pattern_unknown;
    if (FAILED(document->GetCurrentPattern(UIA_TextPatternId, &pattern_unknown)) ||
        !pattern_unknown) return result;
    ComPtr<IUIAutomationTextPattern> pattern;
    if (FAILED(pattern_unknown.As(&pattern)) || !pattern) return result;

    ComPtr<IUIAutomationTextRange> range;
    if (FAILED(pattern->get_DocumentRange(&range)) || !range) return result;

    constexpr int kMaxChars = 16384;
    BSTR body = nullptr;
    if (FAILED(range->GetText(kMaxChars + 1, &body)) || !body) {
        SysFreeString(body);
        return result;
    }
    const auto length = SysStringLen(body);
    result.truncated = length > kMaxChars;
    unsigned int safe_length = result.truncated ? kMaxChars : length;
    if (safe_length > 0 && safe_length < length &&
        body[safe_length - 1] >= 0xD800 && body[safe_length - 1] <= 0xDBFF) {
        --safe_length;
    }
    try {
        result.text = com::wide_to_utf8(std::wstring_view(body, safe_length));
    } catch (const std::exception&) {
        result = {};
    }
    SysFreeString(body);
    return result;
}

} // namespace

HtmlViewerText read_html_viewer_text(int native_handle, int max_attempts) {
    if (native_handle == 0) return {};
    const auto hwnd = reinterpret_cast<HWND>(
        static_cast<uintptr_t>(static_cast<uint32_t>(native_handle)));
    if (!IsWindow(hwnd)) return {};
    if (max_attempts < 1) max_attempts = 1;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        auto result = read_once(hwnd);
        if (!result.text.empty()) return result;
        if (attempt != max_attempts - 1) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return {};
}

} // namespace fairyfly::sap
