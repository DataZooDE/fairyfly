#include "include/html_viewer_reader.h"
#include "include/com/utf8.h"

#include <UIAutomation.h>
#include <wrl/client.h>
#include <cstdint>
#include <exception>
#include <string>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>

namespace fairyfly::sap {

namespace {

using Microsoft::WRL::ComPtr;

struct ReadOnce {
    HtmlReadOutcome outcome = HtmlReadOutcome::NoAutomation;
    HtmlViewerText text;
};

// Cached IUIAutomation, owned by the thread that created it. Never released:
// process exit frees it, and a thread_local would be destroyed after CoUninitialize.
// Another thread gets an uncached instance (apartment rules).
ComPtr<IUIAutomation> acquire_automation() {
    static std::mutex mutex;
    static IUIAutomation* cached = nullptr;
    static std::thread::id owner;
    std::lock_guard<std::mutex> lock(mutex);
    const auto self = std::this_thread::get_id();
    if (cached && owner == self) return cached;
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&automation)))) return nullptr;
    if (!cached) {
        cached = automation.Get();
        cached->AddRef(); // intentionally leaked
        owner = self;
    }
    return automation;
}

ReadOnce read_once(HWND hwnd) {
    ReadOnce once;
    HtmlViewerText& result = once.text;

    auto automation = acquire_automation();
    if (!automation) return once;

    once.outcome = HtmlReadOutcome::NoRoot;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) return once;

    once.outcome = HtmlReadOutcome::NoDocument;

    VARIANT document_type;
    VariantInit(&document_type);
    document_type.vt = VT_I4;
    document_type.lVal = UIA_DocumentControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,
                                                   document_type, &condition))) return once;

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
                                                       has_text, &text_condition))) return once;
        document.Reset();
        if (FAILED(root->FindFirst(TreeScope_Subtree, text_condition.Get(), &document)) ||
            !document) return once;
    }

    once.outcome = HtmlReadOutcome::DocumentEmpty;
    ComPtr<IUnknown> pattern_unknown;
    if (FAILED(document->GetCurrentPattern(UIA_TextPatternId, &pattern_unknown)) ||
        !pattern_unknown) return once;
    ComPtr<IUIAutomationTextPattern> pattern;
    if (FAILED(pattern_unknown.As(&pattern)) || !pattern) return once;

    ComPtr<IUIAutomationTextRange> range;
    if (FAILED(pattern->get_DocumentRange(&range)) || !range) return once;

    constexpr int kMaxChars = 16384;
    BSTR body = nullptr;
    if (FAILED(range->GetText(kMaxChars + 1, &body)) || !body) {
        SysFreeString(body);
        return once;
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
    if (!result.text.empty()) once.outcome = HtmlReadOutcome::Text;
    return once;
}

} // namespace

HtmlViewerText read_html_viewer_text(int native_handle, HtmlReadBudget budget) {
    if (native_handle == 0) return {};
    const auto hwnd = reinterpret_cast<HWND>(
        static_cast<uintptr_t>(static_cast<uint32_t>(native_handle)));
    if (!IsWindow(hwnd)) return {};
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        auto once = read_once(hwnd);
        if (!once.text.text.empty()) return once.text;
        const auto elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
        if (!retry_decision(once.outcome, elapsed, budget)) return {};
        std::this_thread::sleep_for(std::chrono::milliseconds(budget.poll_ms));
    }
}

namespace {

bool is_browser_class(const wchar_t* name) {
    const std::wstring_view cls(name);
    return cls == L"Internet Explorer_Server" || cls == L"Shell DocObject View" ||
           cls == L"Shell Embedding" || cls.starts_with(L"Chrome_WidgetWin_") ||
           cls.find(L"WebView2") != std::wstring_view::npos;
}

BOOL CALLBACK find_browser_child(HWND hwnd, LPARAM found) {
    wchar_t name[256] = {};
    if (GetClassNameW(hwnd, name, 256) > 0 && is_browser_class(name)) {
        *reinterpret_cast<bool*>(found) = true;
        return FALSE;
    }
    return TRUE;
}

} // namespace

bool handle_hosts_browser(int native_handle) {
    if (native_handle == 0) return false;
    const auto hwnd = reinterpret_cast<HWND>(
        static_cast<uintptr_t>(static_cast<uint32_t>(native_handle)));
    if (!IsWindow(hwnd)) return false;
    wchar_t name[256] = {};
    if (GetClassNameW(hwnd, name, 256) > 0 && is_browser_class(name)) return true;
    bool found = false;
    EnumChildWindows(hwnd, find_browser_child, reinterpret_cast<LPARAM>(&found));
    return found;
}

} // namespace fairyfly::sap
