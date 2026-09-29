#include "include/com/wrapper.h"
#include "include/com/wrapper_helpers.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>

namespace fairyfly {
namespace sap {

// ============================================================================
// ComGuiWindow Implementation
// ============================================================================

ComGuiWindow::ComGuiWindow(IDispatchPtr wnd) : SapGuiObject(wnd) {
    if (!wnd) throw ComException("Invalid window pointer");
}

ComGuiWindowPtr ComGuiWindow::create(IDispatchPtr wnd) {
    return std::make_shared<ComGuiWindow>(wnd);
}

// get_id() now inherited from SapGuiObject

std::string ComGuiWindow::get_title() const {
    // Try Text property first (window titlebar text in SAP GUI)
    try {
        std::string text = get_string_property(L"Text");
        if (!text.empty()) {
            return text;
        }
    } catch (const std::exception&) {
        // Text property not available, try Title
    }

    // Fall back to Title property
    try {
        return get_string_property(L"Title");
    } catch (const std::exception&) {
        return "";
    }
}

int ComGuiWindow::get_child_count() const {
    auto children = get_dispatch_property(L"Children");
    if (!children) return 0;
    return ::fairyfly::sap::get_int_property(children, "Count");
}

ComGuiElementPtr ComGuiWindow::get_child(int index) const {
    auto children_col = children();
    return children_col.item(index);
}

SapGuiCollection<ComGuiElement> ComGuiWindow::children() const {
    auto children_dispatch = get_dispatch_property(L"Children");
    if (!children_dispatch) {
        // Return empty collection
        return SapGuiCollection<ComGuiElement>(nullptr);
    }
    return SapGuiCollection<ComGuiElement>(children_dispatch);
}

void ComGuiWindow::send_vkey(int vkey) {
    if (!dispatch_) throw ComException("Null window");

    try {
        _bstr_t method("SendVKey");
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, method.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            throw ComException(fmt::format("SendVKey method not found: 0x{:08X}", hr), hr);
        }

        _variant_t vkey_var(vkey);
        DISPPARAMS params = {(VARIANT*)&vkey_var, nullptr, 1, 0};
        _variant_t result;
        hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                             &params, &result, nullptr, nullptr);
        if (FAILED(hr)) {
            throw ComException(fmt::format("SendVKey invoke failed: 0x{:08X}", hr), hr);
        }

        spdlog::debug("Sent virtual key {} to window", vkey);
    } catch (const _com_error& e) {
        _bstr_t error_msg(e.ErrorMessage());
        throw ComException(fmt::format("COM error sending VKey {}: {}", vkey,
                          std::string(static_cast<const char*>(error_msg))), e.Error());
    } catch (const std::exception& e) {
        throw ComException(fmt::format("Exception sending VKey {}: {}", vkey, e.what()));
    }
}

} // namespace sap
} // namespace fairyfly

