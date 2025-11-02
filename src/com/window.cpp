#include "include/com/wrapper.h"
#include "include/com/wrapper_helpers.h"
#include <spdlog/spdlog.h>

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

} // namespace sap
} // namespace fairyfly

