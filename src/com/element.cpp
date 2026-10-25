#include "include/text/utf8.h"
#include "include/com/invoke.h"
#include "include/string_utils.h"
#include "include/com/wrapper.h"
#include "include/com/wrapper_helpers.h"
#include "include/com/raii_helpers.h"
#include "include/com/utf8.h"
#include "include/sensitive_data.h"
#include "include/display_text_policy.h"
#include "include/element_metadata_builder.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>

namespace fairyfly {
namespace sap {

namespace {
/// A toolbar's by-position answer that is a string ("" when the call fails or answers something else).
std::string text_result(HRESULT hr, const _variant_t& result) {
    return SUCCEEDED(hr) && result.vt == VT_BSTR ? com::bstr_to_utf8(result.bstrVal) : std::string();
}
}  // namespace

void ComGuiElement::call(const wchar_t* method, std::initializer_list<_variant_t> args, const char* unavailable,
                         const char* failed) {
    DISPID dispid;
    HRESULT hr = get_dispid_via_typeinfo(dispatch_, method, &dispid);
    if (FAILED(hr)) throw ComException(unavailable, hr);
    hr = invoke(dispatch_, dispid, DISPATCH_METHOD, args);
    if (FAILED(hr)) throw ComException(failed, hr);
}


// ============================================================================
// ComGuiElement Implementation
// ============================================================================

ComGuiElement::ComGuiElement(IDispatchPtr elem) : SapGuiObject(elem) {
    if (!elem) throw ComException("Invalid element pointer");
}

ComGuiElementPtr ComGuiElement::create(IDispatchPtr elem) {
    return std::make_shared<ComGuiElement>(elem);
}

// get_id() and get_type() now inherited from SapGuiObject

std::string ComGuiElement::get_text_for_direct_read() const {
    if (get_type() != "GuiLabel") return get_text();
    const auto id = get_id();
    const auto label_pos = id.rfind("/lbl[");
    if (label_pos == std::string::npos) return get_text();

    // Screen reads mask all labels on a credential report row after collecting
    // them. Direct get has no such postpass, so inspect the same row before
    // returning one positioned label. Do this only for direct reads to avoid
    // repeated sibling traversal during a full screen extraction.
    try {
        const auto comma = id.find(',', label_pos + 5);
        const auto close = id.find(']', comma);
        if (comma == std::string::npos || close == std::string::npos)
            return redaction_marker(redaction_reason::unverified);
        const auto row = id.substr(comma + 1, close - comma - 1);
        auto parent_dispatch = get_dispatch_property(L"Parent");
        if (!parent_dispatch) return redaction_marker(redaction_reason::unverified);
        auto parent = ComGuiElement::create(parent_dispatch);
        const int count = parent->get_child_count();
        if (count <= 0 || count > 500) return redaction_marker(redaction_reason::unverified);

        bool found = false;
        for (int index = 0; index < count; ++index) {
            auto sibling = parent->get_child(index);
            if (!sibling) return redaction_marker(redaction_reason::unverified);
            const auto sibling_id = sibling->get_id();
            if (sibling_id.empty()) return redaction_marker(redaction_reason::unverified);
            const auto sibling_pos = sibling_id.rfind("/lbl[");
            if (sibling_pos == std::string::npos) continue;
            const auto sibling_comma = sibling_id.find(',', sibling_pos + 5);
            const auto sibling_close = sibling_id.find(']', sibling_comma);
            if (sibling_comma == std::string::npos || sibling_close == std::string::npos ||
                sibling_id.substr(sibling_comma + 1,
                                  sibling_close - sibling_comma - 1) != row) continue;
            if (sibling_id == id) found = true;
            if (contains_sensitive_data_name(sibling->get_string_property(L"Text")))
                return redaction_marker(redaction_reason::label_row);
        }
        if (!found) return redaction_marker(redaction_reason::unverified);
    } catch (const std::exception&) {
        return redaction_marker(redaction_reason::unverified);
    }
    return get_text();
}

std::string ComGuiElement::get_text() const {
    // The credential decisions live in resolve_display_text (shared with other element
    // sources); this adapter only supplies the lazy COM reads.
    DisplayTextInputs in;
    in.type = get_type();
    in.id = [&] { return get_id(); };
    in.label = [&] { return get_label(); };
    in.changeable = [&]() -> std::optional<bool> {
        try { return changeable_cached(); } catch (const std::exception&) { return std::nullopt; }
    };
    in.displayed_text = [&]() -> std::optional<std::string> {
        try {
            return get_string_property(L"DisplayedText");
        } catch (const ComException&) {
            // DisplayedText property not available on this element type
            return std::nullopt;
        }
    };
    in.text = [&] { return get_string_property(L"Text"); };
    in.sibling_probe = [&](const SiblingQuery& query, const SiblingVisitor& visit) {
        try {
            auto parent = get_dispatch_property(L"Parent");
            if (!parent) return false;
            bool needs_enumeration = !query.row.empty();
            if (query.row.empty()) {
                for (size_t index = 0; index < query.leaves.size(); ++index) {
                    auto sibling_dispatch = call_method_with_string(
                        parent, "FindById", query.leaves[index]);
                    if (!sibling_dispatch) {
                        needs_enumeration = true;
                        continue;
                    }
                    const auto name = ComGuiElement::create(sibling_dispatch)
                        ->get_string_property(L"Text");
                    if (!visit(index, name)) return true;
                }
            }
            if (needs_enumeration) {
                // A failed FindById call can mean either absence or a COM
                // observation error. Enumerate the bounded parent to prove
                // absence before allowing an ordinary VALUE field through.
                auto parent_element = ComGuiElement::create(parent);
                const int count = parent_element->get_child_count();
                if (count <= 0 || count > 200) return false;
                for (int index = 0; index < count; ++index) {
                    auto child = parent_element->get_child(index);
                    if (!child) return false;
                    const auto child_id = child->get_id();
                    if (child_id.empty()) return false;
                    const auto child_separator = child_id.find_last_of('/');
                    const auto child_leaf = child_id.substr(
                        child_separator == std::string::npos ? 0 : child_separator + 1);
                    const auto [child_field, child_row] = split_field_index(child_leaf);
                    if (child_row != query.row) continue;
                    const auto match = std::find(query.leaves.begin(), query.leaves.end(), child_field);
                    if (match == query.leaves.end()) continue;
                    const auto name = child->get_string_property(L"Text");
                    if (!visit(static_cast<size_t>(match - query.leaves.begin()), name)) return true;
                }
            }
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    return resolve_display_text(in);
}

bool ComGuiElement::explicitly_read_only() const {
    DISPID changeable_id;
    if (FAILED(resolve_dispid(L"Changeable", &changeable_id))) return false;
    _variant_t changeable;
    const HRESULT hr = invoke(dispatch_, changeable_id, DISPATCH_PROPERTYGET, {}, &changeable);
    return SUCCEEDED(hr) && changeable.vt == VT_BOOL && changeable.boolVal == VARIANT_FALSE;
}

std::optional<bool> ComGuiElement::parse_check_value(const std::string& value) {
    const std::string v = utils::lower_ascii(utils::trim(value));
    if (v == "true" || v == "1" || v == "yes" || v == "on" || v == "x") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off" || v.empty()) return false;
    return std::nullopt;
}

std::optional<bool> ComGuiElement::get_selected() const {
    const auto read = read_bool_property_strict(L"Selected");
    if (read.status != PropertyStatus::Ok) return std::nullopt;
    return read.value == "true";
}

std::optional<bool> ComGuiElement::toggle_selected() {
    if (get_classified_type() != GuiElementType::CheckBox) {
        throw ComException("toggle only works on check boxes");
    }
    const auto current = get_selected();
    if (!current) throw ComException("Cannot read the check box Selected state, so it is not toggled blindly");
    select(!*current);
    return get_selected();
}

ComGuiElement::FillOutcome ComGuiElement::fill_value(const std::string& value) {
    FillOutcome outcome;
    if (explicitly_read_only()) {
        outcome.status = FillOutcome::Status::ReadOnly;
        return outcome;
    }
    const std::string type = get_type();
    const auto invalid = [&](std::string message) {
        outcome.status = FillOutcome::Status::InvalidArgument;
        outcome.message = std::move(message);
        return outcome;
    };

    if (type == "GuiCheckBox" || type == "GuiRadioButton") {
        const bool radio = type == "GuiRadioButton";
        const auto wanted = parse_check_value(value);
        if (!wanted) {
            return invalid("Cannot interpret '" + value + "' as a " + (radio ? "radio button" : "check box") +
                           " value; accepted: true, false, 1, 0, yes, no, on, off, x or an empty value (case-insensitive)");
        }
        if (radio && !*wanted) {
            return invalid("A radio button cannot be cleared; select another option of the group instead");
        }
        select(*wanted);
        outcome.selected = get_selected();
        return outcome;
    }

    if (type == "GuiComboBox") {
        struct Entry { std::string key, value; };
        std::vector<Entry> entries;
        try {
            if (auto entries_dispatch = get_dispatch_property(L"Entries")) {
                SapGuiCollection<ComGuiElement> collection(entries_dispatch);
                const bool walked = collection.for_each([&](const std::shared_ptr<ComGuiElement>& entry) {
                    entries.push_back({entry->get_string_property(L"Key"), entry->get_string_property(L"Value")});
                });
                if (!walked) entries.clear();
            }
        } catch (const std::exception& e) {
            spdlog::debug("ComboBox entries unavailable: {}", e.what());
            entries.clear();
        }
        const bool entries_known = !entries.empty();

        std::string key_to_write = value;
        if (entries_known) {
            const std::string wanted = utils::trim(value);
            const std::string wanted_lower = utils::lower_ascii(wanted);
            const Entry* match = nullptr;
            for (const auto& entry : entries) {  // exact key first, then the displayed value
                if (utils::trim(entry.key) == wanted) { match = &entry; break; }
            }
            if (!match) {
                for (const auto& entry : entries) {
                    if (utils::lower_ascii(utils::trim(entry.value)) == wanted_lower) { match = &entry; break; }
                }
            }
            if (!match) {  // keys that differ only in case ("en" for "EN")
                for (const auto& entry : entries) {
                    if (utils::lower_ascii(utils::trim(entry.key)) == wanted_lower) { match = &entry; break; }
                }
            }
            if (!match) {
                constexpr size_t kMaxListed = 30;
                std::string list;
                for (size_t i = 0; i < entries.size() && i < kMaxListed; ++i) {
                    if (i) list += "; ";
                    list += entries[i].key + " = " + entries[i].value;
                }
                if (entries.size() > kMaxListed) {
                    list += "; ... (" + std::to_string(entries.size() - kMaxListed) + " more)";
                }
                return invalid("No combo box entry matches '" + value +
                               "' (key or displayed value). Available entries (key = value): " + list);
            }
            key_to_write = match->key;
        }

        try {
            set_string_property(L"Key", key_to_write);
        } catch (const std::exception& e) {
            if (entries_known) throw;
            throw ComException("Combo box entries are unavailable, so '" + value +
                               "' was written as a key and SAP rejected it: " + e.what());
        }
        try { outcome.key = get_string_property(L"Key"); } catch (const std::exception&) {}
        try { outcome.display_value = get_string_property(L"Value"); } catch (const std::exception&) {}
        return outcome;
    }

    set_string_property(L"Text", value);
    spdlog::debug("Set text on element type {}", type);
    return outcome;
}

bool ComGuiElement::set_text(const std::string& text) {
    if (explicitly_read_only()) return false;
    const std::string type = get_type();
    if (type == "GuiComboBox") {
        set_string_property(L"Key", text);
    } else {
        set_string_property(L"Text", text);
    }
    spdlog::debug("Set text on element type {}", type);
    return true;
}

bool ComGuiElement::is_enabled() const {
    if (!dispatch_) return false;

    // Try to get Enabled property via DISPID (per-type cache, including misses)
    DISPID dispid;
    HRESULT hr = resolve_dispid(L"Enabled", &dispid);

    if (FAILED(hr)) {
        // Property doesn't exist - most SAP GUI elements don't have "Enabled"
        // If the property doesn't exist, assume the element is enabled
        // (the Changeable property is the real indicator of editability)
        return true;
    }

    // Property exists, get its value
    _variant_t result;
    hr = invoke(dispatch_, dispid, DISPATCH_PROPERTYGET, {}, &result);
    if (FAILED(hr)) return true;  // If we can't read it, assume enabled

    // Verify variant type before accessing boolVal to avoid undefined behavior
    if (result.vt == VT_BOOL) {
        return result.boolVal != VARIANT_FALSE;
    }

    spdlog::debug("is_enabled|unexpected type|vt={}", (int)result.vt);
    return true;  // If unexpected type, assume enabled
}

bool ComGuiElement::is_visible() const {
    // "Visible" is not a GuiVComponent scripting property, so the DISPID lookup normally
    // misses (negative-cached per type, so it costs no COM round trips after the first).
    // SAP only returns elements that exist on the current screen, so a missing property
    // means "visible". When a Visible property does exist its actual value is returned.
    // Any other COM failure keeps the conservative answer (false) with a debug log.
    if (!dispatch_) return false;
    DISPID dispid;
    const HRESULT lookup = resolve_dispid(L"Visible", &dispid);
    if (lookup == DISP_E_UNKNOWNNAME || lookup == DISP_E_MEMBERNOTFOUND ||
        lookup == TYPE_E_ELEMENTNOTFOUND) {
        return true;
    }
    if (FAILED(lookup)) {
        spdlog::debug("is_visible|lookup failed|hr={:#010x}", (unsigned int)lookup);
        return false;
    }
    return get_bool_property(L"Visible");
}

// Static helper to classify element type
GuiElementType ComGuiElement::classify_type(const std::string& type_str) {
    if (type_str == "GuiRadioButton" || type_str.find("RadioButton") != std::string::npos) return GuiElementType::RadioButton;
    if (type_str == "GuiCheckBox" || type_str.find("CheckBox") != std::string::npos) return GuiElementType::CheckBox;
    if (type_str == "GuiButton" || type_str.find("Button") != std::string::npos) return GuiElementType::Button;
    if (type_str == "GuiTab" || (type_str.find("Tab") != std::string::npos && type_str.find("TabStrip") == std::string::npos)) return GuiElementType::Tab;
    if (type_str.find("TextField") != std::string::npos || type_str.find("CTextField") != std::string::npos) return GuiElementType::TextField;
    if (type_str.find("ComboBox") != std::string::npos) return GuiElementType::ComboBox;
    if (type_str.find("Label") != std::string::npos) return GuiElementType::Label;
    if (type_str.find("Table") != std::string::npos) return GuiElementType::Table;
    if (type_str.find("Tree") != std::string::npos) return GuiElementType::Tree;
    if (type_str.find("StatusBar") != std::string::npos) return GuiElementType::StatusBar;
    if (type_str.find("MenuBar") != std::string::npos) return GuiElementType::MenuBar;
    if (type_str.find("Toolbar") != std::string::npos) return GuiElementType::Toolbar;
    return GuiElementType::Unknown;
}

GuiElementType ComGuiElement::get_classified_type() const {
    if (!element_type_cached_) {
        cached_element_type_ = classify_type(get_type());
        element_type_cached_ = true;
    }
    return cached_element_type_;
}

ComGuiElement::Rect ComGuiElement::get_rect() const {
    Rect rect;
    try {
        rect.x = get_int_property(L"ScreenLeft");
        rect.y = get_int_property(L"ScreenTop");
        rect.width = get_int_property(L"Width");
        rect.height = get_int_property(L"Height");
    } catch (const SapGuiException& e) {
        spdlog::debug("Could not retrieve element rect: SapGuiException: {}", e.what());
    } catch (const ComException& e) {
        spdlog::debug("Could not retrieve element rect: ComException: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::debug("Could not retrieve element rect: std::exception: {}", e.what());
    }
    return rect;
}

void ComGuiElement::set_focus() {
    if (!dispatch_) throw ComException("Null element");
    call(L"SetFocus", {}, "SetFocus method not found on element", "Failed to focus element");
}

void ComGuiElement::press() {
    utils::TraceGuard trace("ComGuiElement::press");
    if (!dispatch_) throw ComException("Null element");

    try {
        std::string elem_type = get_type();
        if (elem_type == "GuiTab" || elem_type == "GuiMenu" ||
            elem_type == "GuiRadioButton") {
            select(true);
            return;
        }
        if (elem_type == "GuiCheckBox") {
            // A click flips a check box; assigning Selected=true would leave a checked box checked.
            toggle_selected();
            return;
        }

        _bstr_t method("Press");
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, method.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            // Fallback for elements like GuiTab or controls that use Select
            _bstr_t sel_method("Select");
            DISPID sel_dispid;
            if (SUCCEEDED(get_dispid_via_typeinfo(dispatch_, sel_method.GetBSTR(), &sel_dispid))) {
                select(true);
                return;
            }
            trace.mark_error(fmt::format("get_dispid_via_typeinfo failed: 0x{:08X}", hr));
            throw ComException("Press method not found on element", hr);
        }

        _variant_t result;
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {}, &result);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("Invoke failed: 0x{:08X}", hr));
            throw ComException("Failed to press element", hr);
        }

        std::string element_id = get_id();
        trace.mark_success();
        spdlog::debug("Element pressed: {}", element_id);
    } catch (const ComException&) {
        throw;
    } catch (const std::exception& e) {
        trace.mark_error(e.what());
        throw ComException(std::string("Exception in press: ") + e.what());
    }
}

void ComGuiElement::select(bool selected) {
    utils::TraceGuard trace("ComGuiElement::select");
    if (!dispatch_) throw ComException("Null element");

    try {
        GuiElementType elem_type = get_classified_type();

        // Tabs and menu items use the Select() method (no parameters).
        if (elem_type == GuiElementType::Tab || get_type() == "GuiMenu") {
            _bstr_t method("Select");
            DISPID dispid;
            HRESULT hr = get_dispid_via_typeinfo(dispatch_, method.GetBSTR(), &dispid);
            if (FAILED(hr)) {
                trace.mark_error(fmt::format("Select method not found: 0x{:08X}", hr));
                throw ComException("Select method not found on tab or menu element", hr);
            }

            // Call Select() method with no parameters
            _variant_t result;
            hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {}, &result);
            if (FAILED(hr)) {
                trace.mark_error(fmt::format("Failed to call Select(): 0x{:08X}", hr));
                throw ComException("Failed to select tab or menu element", hr);
            }

            trace.mark_success();
            spdlog::debug("Tab or menu selected: {}", get_id());
            return;
        }

        // CheckBox and RadioButton use the Selected property
        if (elem_type != GuiElementType::CheckBox && elem_type != GuiElementType::RadioButton) {
            throw ComException("Select only works on CheckBox, RadioButton, Tab, or Menu elements");
        }

        // For RadioButton, try Select() method first if available
        if (elem_type == GuiElementType::RadioButton && selected) {
            _bstr_t sel_method("Select");
            DISPID sel_dispid;
            if (SUCCEEDED(get_dispid_via_typeinfo(dispatch_, sel_method.GetBSTR(), &sel_dispid))) {
                _variant_t result;
                if (SUCCEEDED(invoke(dispatch_, sel_dispid, DISPATCH_METHOD, {}, &result))) {
                    trace.mark_success();
                    spdlog::debug("RadioButton selected via Select(): {}", get_id());
                    return;
                }
            }
        }

        _bstr_t prop("Selected");
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, prop.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("Selected property not found: 0x{:08X}", hr));
            throw ComException("Selected property not found", hr);
        }

        hr = invoke(dispatch_, dispid, DISPATCH_PROPERTYPUT, {_variant_t(selected)});   // VT_BOOL
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("Failed to set Selected: 0x{:08X}", hr));
            throw ComException("Failed to set selected state", hr);
        }

        trace.mark_success();
        spdlog::debug("Element selected: {} = {}", get_id(), (selected ? "true" : "false"));
    } catch (const ComException&) {
        throw;
    } catch (const std::exception& e) {
        trace.mark_error(e.what());
        throw ComException(std::string("Exception in select: ") + e.what());
    }
}

std::string ComGuiElement::get_label() const {
    if (!dispatch_) return "";
    if (label_cached_) return cached_label_;

    try {
        // Try AccLabel property first (most reliable)
        cached_label_ = get_string_property(L"AccLabel");
    } catch (const ComException&) {
        cached_label_.clear();
    }
    // Selection-screen comments assigned with FOR FIELD are exposed through
    // LeftLabel/RightLabel even when the field has no AccLabel.
    if (cached_label_.empty()) {
        for (const auto* side : {L"LeftLabel", L"RightLabel"}) {
            auto label_dispatch = get_dispatch_property(side);
            if (!label_dispatch) continue;
            auto label = ComGuiElement::create(label_dispatch);
            cached_label_ = label->get_string_property(L"Text");
            if (!cached_label_.empty()) break;
        }
    }
    label_cached_ = true;
    return cached_label_;
}

std::string ComGuiElement::get_tooltip() const {
    if (!dispatch_) return "";

    // Cascade: AccTooltip → DefaultTooltip → Tooltip
    try {
        std::string tooltip = get_string_property(L"AccTooltip");
        if (!tooltip.empty()) return tooltip;
    } catch (const ComException&) {}

    try {
        std::string tooltip = get_string_property(L"DefaultTooltip");
        if (!tooltip.empty()) return tooltip;
    } catch (const ComException&) {}

    try {
        return get_string_property(L"Tooltip");
    } catch (const ComException&) {
        return "";
    }
}

bool ComGuiElement::changeable_cached() const {
    if (!cached_changeable_) cached_changeable_ = get_bool_property(L"Changeable");
    return *cached_changeable_;
}

bool ComGuiElement::is_changeable() const {
    if (!dispatch_) return false;

    try {
        // Check Changeable property (available on most interactive elements)
        return changeable_cached();
    } catch (const ComException&) {
        // If Changeable not available, assume not changeable
        return false;
    }
}

IDispatchPtr ComGuiElement::children_dispatch_cached() const {
    if (!children_fetched_) {
        // Throws on COM failure; nothing is cached in that case.
        children_dispatch_ = get_dispatch_property(L"Children");
        children_fetched_ = true;
    }
    return children_dispatch_;
}

int ComGuiElement::get_child_count() const {
    if (!dispatch_) return 0;
    if (children_count_ >= 0) return children_count_;

    try {
        auto children = children_dispatch_cached();
        if (!children) {
            spdlog::debug("get_child_count: Children property is null for {}", get_id());
            children_count_ = 0;
            return 0;
        }
        int count = ::fairyfly::sap::get_int_property(children, "Count");
        children_count_ = count;
        if (count > 0) {
            spdlog::debug("get_child_count: {} has {} children", get_id(), count);
        }
        return count;
    } catch (const ComException& e) {
        spdlog::debug("get_child_count: Exception for {}: {}", get_id(), e.what());
        return 0;
    }
}

ComGuiElementPtr ComGuiElement::get_child(int index) const {
    if (!dispatch_ || index < 0) return nullptr;

    try {
        auto children_col = children();
        return children_col.item(index);
    } catch (const std::exception& e) {
        spdlog::debug("ComGuiElement::get_child({}): {}", index, e.what());
        return nullptr;
    }
}


SapGuiCollection<ComGuiElement> ComGuiElement::children() const {
    if (!dispatch_) {
        return SapGuiCollection<ComGuiElement>(nullptr);
    }

    try {
        auto children_dispatch = children_dispatch_cached();
        if (!children_dispatch) {
            spdlog::debug("children(): Children property is null");
            return SapGuiCollection<ComGuiElement>(nullptr);
        }

        return SapGuiCollection<ComGuiElement>(children_dispatch);
    } catch (const ComException& e) {
        spdlog::debug("children(): Exception: {}", e.what());
        return SapGuiCollection<ComGuiElement>(nullptr);
    }
}

std::string ComGuiElement::get_container_type() const {
    if (!dispatch_) return "";
    return classify_container_type(get_type(), [&] { return get_child_count(); });
}

int ComGuiElement::get_property_int(const std::wstring& property_name) const {
    try {
        return get_int_property(property_name.c_str());
    } catch (const std::exception& e) {
        std::string prop_name_utf8 = text::wide_to_utf8_lossy(property_name);
        spdlog::debug("get_property_int failed for {}: {}", prop_name_utf8, e.what());
        return 0;
    }
}

bool ComGuiElement::get_property_bool(const std::wstring& property_name) const {
    try {
        return get_bool_property(property_name.c_str());
    } catch (const std::exception& e) {
        std::string prop_name_utf8 = text::wide_to_utf8_lossy(property_name);
        spdlog::debug("get_property_bool failed for {}: {}", prop_name_utf8, e.what());
        return false;
    }
}

std::string ComGuiElement::get_property_string(const std::wstring& property_name) const {
    try {
        return get_string_property(property_name.c_str());
    } catch (const std::exception& e) {
        std::string prop_name_utf8 = text::wide_to_utf8_lossy(property_name);
        spdlog::debug("get_property_string failed for {}: {}", prop_name_utf8, e.what());
        return "";
    }
}

std::string ComGuiElement::get_subtype() const {
    try {
        // SubType property is available on GuiShell elements
        return get_string_property(L"SubType");
    } catch (const std::exception& e) {
        spdlog::debug("get_subtype failed: {}", e.what());
        return "";
    }
}

std::vector<std::string> ComGuiElement::get_all_node_keys() const {
    std::vector<std::string> keys;

    try {
        // Call GetAllNodeKeys() method on the tree control
        // This returns a collection object
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"GetAllNodeKeys", &dispid);
        if (FAILED(hr)) {
            spdlog::debug("Could not get DISPID for GetAllNodeKeys");
            return keys;
        }

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {}, result_guard.get());

        if (SUCCEEDED(hr) && result_var.vt == VT_DISPATCH && result_var.pdispVal) {
            // Result is a collection - enumerate it
            IDispatch* coll = result_var.pdispVal;

            // Get Count property
            DISPID count_dispid;
            hr = get_dispid_via_typeinfo(coll, L"Count", &count_dispid);
            if (FAILED(hr)) {
                return keys;
            }

            VARIANT count_result_var;
            com::VariantGuard count_result_guard(&count_result_var);

            hr = invoke(coll, count_dispid, DISPATCH_PROPERTYGET, {}, count_result_guard.get());

            if (SUCCEEDED(hr) && count_result_var.vt == VT_I4) {
                int count = count_result_var.lVal;

                // Get Item method dispid
                DISPID item_dispid;
                hr = get_dispid_via_typeinfo(coll, L"Item", &item_dispid);
                if (FAILED(hr)) {
                    return keys;
                }

                // Enumerate items (0-based indexing)
                for (int i = 0; i < count; ++i) {
                    VARIANT item_result_var;
                    com::VariantGuard item_result_guard(&item_result_var);

                    hr = invoke(coll, item_dispid, DISPATCH_METHOD, {_variant_t(static_cast<long>(i))}, item_result_guard.get());   // VT_I4, as SAP expects

                    if (SUCCEEDED(hr)) {
                        // CRITICAL FIX: Use safe_bstr_to_string() to handle NULL BSTR and access violations
                        if (item_result_var.vt == VT_BSTR) {
                            std::string context = fmt::format("get_all_node_keys item[{}]", i);
                            std::string key = safe_bstr_to_string(item_result_var, context.c_str());
                            if (!key.empty()) {
                                keys.push_back(key);
                            }
                        } else if (item_result_var.vt == VT_I4) {
                            keys.push_back(std::to_string(item_result_var.lVal));
                        }
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        spdlog::debug("get_all_node_keys failed: {}", e.what());
    }

    return keys;
}

std::string ComGuiElement::get_node_text_by_key(const std::string& key) const {
    try {
        if (!dispatch_) {
            spdlog::warn("get_node_text_by_key called with null dispatch pointer");
            return "";
        }

        // Call GetNodeTextByKey(key) method
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"GetNodeTextByKey", &dispid);
        if (FAILED(hr)) {
            spdlog::debug("Could not get DISPID for GetNodeTextByKey");
            return "";
        }

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(key)}, result_guard.get());

        // CRITICAL FIX: Use safe_bstr_to_string() to handle NULL BSTR and access violations
        std::string text;
        if (SUCCEEDED(hr)) {
            std::string context = fmt::format("get_node_text_by_key key='{}'", key);
            text = safe_bstr_to_string(result_var, context.c_str());
        }

        return text;

    } catch (const std::exception& e) {
        spdlog::debug("get_node_text_by_key|exception|key={}|error={}", key, e.what());
        return "";
    }
}

std::string ComGuiElement::get_node_path_by_key(const std::string& key) const {
    try {
        if (!dispatch_) {
            spdlog::warn("get_node_path_by_key called with null dispatch pointer");
            return "";
        }

        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"GetNodePathByKey", &dispid);
        if (FAILED(hr)) {
            return "";
        }

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(key)}, result_guard.get());

        // CRITICAL FIX: Use safe_bstr_to_string() to handle NULL BSTR and access violations
        std::string path;
        if (SUCCEEDED(hr)) {
            std::string context = fmt::format("get_node_path_by_key key='{}'", key);
            path = safe_bstr_to_string(result_var, context.c_str());
        }

        return path;

    } catch (const std::exception& e) {
        spdlog::debug("get_node_path_by_key|exception|key={}|error={}", key, e.what());
        return "";
    }
}

std::vector<std::string> ComGuiElement::get_column_order() const {
    std::vector<std::string> columns;

    try {
        // Get ColumnOrder property (returns a collection)
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"ColumnOrder", &dispid);
        if (FAILED(hr)) {
            return columns;
        }

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);

        hr = invoke(dispatch_, dispid, DISPATCH_PROPERTYGET, {}, result_guard.get());

        if (SUCCEEDED(hr) && result_var.vt == VT_DISPATCH && result_var.pdispVal) {
            IDispatch* coll = result_var.pdispVal;

            // Get Count property
            DISPID count_dispid;
            hr = get_dispid_via_typeinfo(coll, L"Count", &count_dispid);
            if (SUCCEEDED(hr)) {
                VARIANT count_result_var;
                com::VariantGuard count_result_guard(&count_result_var);

                hr = invoke(coll, count_dispid, DISPATCH_PROPERTYGET, {}, count_result_guard.get());

                if (SUCCEEDED(hr) && count_result_var.vt == VT_I4) {
                    int count = count_result_var.lVal;

                    // Get Item method dispid
                    DISPID item_dispid;
                    hr = get_dispid_via_typeinfo(coll, L"Item", &item_dispid);
                    if (SUCCEEDED(hr)) {
                        for (int i = 0; i < count; ++i) {
                            VARIANT item_result_var;
                            com::VariantGuard item_result_guard(&item_result_var);

                            hr = invoke(coll, item_dispid, DISPATCH_METHOD, {_variant_t(static_cast<long>(i))}, item_result_guard.get());   // VT_I4, as SAP expects

                            // CRITICAL FIX: Use safe_bstr_to_string() to handle NULL BSTR and access violations
                            if (SUCCEEDED(hr)) {
                                std::string context = fmt::format("get_column_order item[{}]", i);
                                std::string col = safe_bstr_to_string(item_result_var, context.c_str());
                                if (!col.empty()) {
                                    columns.push_back(col);
                                }
                            }
                        }
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        spdlog::debug("get_column_order failed: {}", e.what());
    }

    return columns;
}

std::string ComGuiElement::get_cell_value(int row, const std::string& column_name) const {
    try {
        if (!dispatch_) {
            spdlog::warn("get_cell_value called with null dispatch pointer");
            return "";
        }

        // Call GetCellValue(row As Long, column As String) As String
        DISPID dispid;
        HRESULT hr = resolve_dispid(L"GetCellValue", &dispid);
        if (FAILED(hr)) {
            return "";
        }

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {_variant_t(static_cast<long>(row)), bstr_arg(column_name)},
                    result_guard.get());

        // CRITICAL FIX: Use safe_bstr_to_string() to handle NULL BSTR and access violations
        std::string cell_value;
        if (SUCCEEDED(hr)) {
            std::string context = fmt::format("get_cell_value row={} col='{}'", row, column_name);
            cell_value = safe_bstr_to_string(result_var, context.c_str());
        }

        // Cleanup happens automatically via RAII guards
        return cell_value;

    } catch (const std::exception& e) {
        spdlog::debug("get_cell_value|exception|row={}|column={}|error={}", row, column_name, e.what());
        return "";
    }
}

AbapEditorContent ComGuiElement::get_abap_editor_content(int max_lines) const {
    if (!dispatch_ || max_lines < 1)
        throw ComException("ABAP editor line limit must be positive");

    DISPID count_id;
    HRESULT hr = resolve_dispid(L"GetLineCount", &count_id);
    if (FAILED(hr)) throw ComException("GetLineCount method not found", hr);
    VARIANT count_result;
    com::VariantGuard count_guard(&count_result);
    hr = safe_invoke(dispatch_, count_id, DISPATCH_METHOD, {}, count_guard.get());
    if (FAILED(hr) || count_result.vt != VT_I4)
        throw ComException("GetLineCount failed", hr);

    AbapEditorContent content;
    content.total_lines = std::max(0L, count_result.lVal);
    const int limit = std::min(content.total_lines, max_lines);
    if (limit == 0) return content;

    DISPID line_id;
    hr = resolve_dispid(L"GetLineText", &line_id);
    if (FAILED(hr)) throw ComException("GetLineText method not found", hr);
    constexpr size_t max_chars = 1024 * 1024;
    for (int line = 0; line < limit; ++line) {
        VARIANT line_result;
        com::VariantGuard line_guard(&line_result);
        // SAP ABAP editor lines are one-based
        hr = safe_invoke(dispatch_, line_id, DISPATCH_METHOD, {_variant_t(static_cast<long>(line + 1))}, line_guard.get());
        if (FAILED(hr) || line_result.vt != VT_BSTR)
            throw ComException(fmt::format("GetLineText failed (hr=0x{:08X}, type={})",
                                         static_cast<unsigned int>(hr), line_result.vt), hr);
        const std::string value = com::bstr_to_utf8(line_result.bstrVal);
        if (content.text.size() + value.size() + 1 > max_chars) break;
        if (content.lines_read) content.text += '\n';
        content.text += value;
        ++content.lines_read;
    }
    content.truncated = content.lines_read < content.total_lines;
    return content;
}

std::vector<std::string> ComGuiElement::get_tree_column_names() const {
    std::vector<std::string> names;
    if (!dispatch_) return names;
    try {
        DISPID method_id;
        if (FAILED(get_dispid_via_typeinfo(dispatch_, L"GetColumnNames", &method_id))) return names;
        VARIANT result;
        com::VariantGuard result_guard(&result);
        if (FAILED(invoke(dispatch_, method_id, DISPATCH_METHOD, {}, result_guard.get())) ||
            result.vt != VT_DISPATCH || !result.pdispVal) return names;
        IDispatch* collection = result.pdispVal;
        DISPID count_id, item_id;
        if (FAILED(get_dispid_via_typeinfo(collection, L"Count", &count_id)) ||
            FAILED(get_dispid_via_typeinfo(collection, L"Item", &item_id))) return names;
        VARIANT count;
        com::VariantGuard count_guard(&count);
        if (FAILED(invoke(collection, count_id, DISPATCH_PROPERTYGET, {}, count_guard.get())) ||
            count.vt != VT_I4) return names;
        for (int i = 0; i < count.lVal && i < 100; ++i) {
            _variant_t item;
            if (SUCCEEDED(invoke(collection, item_id, DISPATCH_METHOD, {_variant_t(i)}, &item)) &&
                item.vt == VT_BSTR)
                names.push_back(safe_bstr_to_string(item, "get_tree_column_names item"));
        }
    } catch (const std::exception& e) {
        spdlog::debug("get_tree_column_names failed: {}", e.what());
    }
    return names;
}

void ComGuiElement::modify_grid_cell(int row, const std::string& column_name,
                                     const std::string& value, bool checkbox, bool commit) {
    if (!dispatch_) throw ComException("Null GridView element");
    if (row < 0 || column_name.empty()) throw ComException("Invalid GridView cell coordinates");

    const wchar_t* method_name = checkbox ? L"ModifyCheckBox" : L"ModifyCell";
    _variant_t cell_value;
    if (checkbox) {
        bool checked;
        if (value == "X" || value == "x" || value == "1" || value == "true" || value == "True") {
            checked = true;
        } else if (value.empty() || value == "0" || value == "false" || value == "False") {
            checked = false;
        } else {
            throw ComException("Checkbox value must be X, 1, true, 0, or false");
        }
        cell_value = _variant_t(checked);
    } else {
        cell_value = _variant_t(com::utf8_to_wide(value).c_str());
    }
    call(method_name, {_variant_t(row), _variant_t(com::utf8_to_wide(column_name).c_str()), cell_value},
         "GridView cell modification is unavailable", "Failed to modify GridView cell");
    if (commit) call(L"TriggerModified", {}, "GridView TriggerModified method not found", "Failed to commit GridView changes");
}

void ComGuiElement::select_grid_row(int row, const std::string& column_name) {
    if (!dispatch_) throw ComException("Null GridView element");
    if (row < 0 || column_name.empty()) throw ComException("Invalid GridView row or column");

    const auto invoke_cell = [&](const wchar_t* method_name) {
        call(method_name, {_variant_t(row), _variant_t(com::utf8_to_wide(column_name).c_str())},
             "GridView cell action is unavailable", "Failed to activate GridView cell");
    };

    // Click refreshes some detail panes, but read-only grids can reject it.
    // SelectedRows is the operation required for row-targeted toolbar actions.
    try {
        invoke_cell(L"SetCurrentCell");
        try { invoke_cell(L"Click"); }
        catch (const ComException&) { /* Read-only cells can reject Click. */ }
        set_string_property(L"SelectedRows", std::to_string(row));
    } catch (...) {
        try { set_string_property(L"SelectedRows", ""); }
        catch (...) { /* Preserve the original selection error. */ }
        throw;
    }
}

void ComGuiElement::doubleclick_grid_cell(int row, const std::string& column_name) {
    if (!dispatch_) throw ComException("Null GridView element");
    if (row < 0 || column_name.empty()) throw ComException("Invalid GridView row or column");

    call(L"SetCurrentCell", {_variant_t(row), _variant_t(com::utf8_to_wide(column_name).c_str())},
         "GridView SetCurrentCell method not found", "Failed to set GridView current cell");
    call(L"DoubleClickCurrentCell", {}, "GridView DoubleClickCurrentCell method not found",
         "Failed to double-click GridView cell");
}

int ComGuiElement::get_grid_toolbar_button_count() const {
    try { return get_int_property(L"ToolbarButtonCount"); }
    catch (const std::exception&) { return 0; }
}

std::string ComGuiElement::get_grid_toolbar_button_id(int position) const {
    try {
        _variant_t result;
        return text_result(call_method(L"GetToolbarButtonId", {_variant_t(position)}, result, Lookup::Cached), result);
    } catch (const std::exception&) { return ""; }
}

std::string ComGuiElement::get_grid_toolbar_button_tooltip(int position) const {
    try {
        _variant_t result;
        return text_result(call_method(L"GetToolbarButtonTooltip", {_variant_t(position)}, result, Lookup::Cached), result);
    } catch (const std::exception&) { return ""; }
}

std::string ComGuiElement::get_item_text(const std::string& node_key, const std::string& column_name) const {
    try {
        // Validate dispatch pointer before attempting COM calls
        if (!dispatch_) {
            spdlog::warn("get_item_text called with null dispatch pointer");
            return "";
        }

        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"GetItemText", &dispid);
        if (FAILED(hr)) {
            if (hr == E_FAIL) {
                spdlog::error("get_item_text|stale_com_pointer|key={}|column={}", node_key, column_name);
            }
            return "";
        }

        VARIANT result_var;
        com::VariantGuard result_guard(&result_var);
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(node_key), bstr_arg(column_name)}, result_guard.get());

        // CRITICAL FIX: Use safe_bstr_to_string() to handle NULL BSTR and access violations
        std::string text;
        if (SUCCEEDED(hr)) {
            std::string context = fmt::format("get_item_text key='{}' col='{}'", node_key, column_name);
            text = safe_bstr_to_string(result_var, context.c_str());
        } else {
            spdlog::debug("get_item_text|invoke_failed|key={}|column={}|hr=0x{:08X}",
                         node_key, column_name, hr);
        }

        // Cleanup happens automatically via VariantGuard RAII
        return text;

    } catch (const std::exception& e) {
        spdlog::debug("get_item_text|exception|key={}|column={}|error={}", node_key, column_name, e.what());
        return "";
    }
}

// ============================================================================
// GuiShell Tree Navigation Methods
// ============================================================================

void ComGuiElement::select_node(const std::string& node_key) {
    try {
        // SelectNode may add to an existing tree selection. A subsequent SAP toolbar
        // action can then operate on the stale node instead of the requested one.
        DISPID unselect_dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"UnselectAll", &unselect_dispid);
        if (FAILED(hr)) {
            throw ComException("UnselectAll method not found - element may not be a tree control", hr);
        }
        hr = invoke(dispatch_, unselect_dispid, DISPATCH_METHOD);
        if (FAILED(hr)) {
            throw ComException("Failed to clear earlier tree selections", hr);
        }

        DISPID dispid;
        hr = get_dispid_via_typeinfo(dispatch_, L"SelectNode", &dispid);
        if (FAILED(hr)) {
            throw ComException("SelectNode method not found - element may not be a tree control", hr);
        }

        // the key as UTF-8 (it went through the ANSI code page before)
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(node_key)});

        if (FAILED(hr)) {
            throw ComException("Failed to select tree node '" + node_key + "'", hr);
        }

        spdlog::debug("Successfully selected tree node: {}", node_key);

    } catch (const std::exception& e) {
        spdlog::error("select_node failed for key '{}': {}", node_key, e.what());
        throw;
    }
}

void ComGuiElement::expand_node(const std::string& node_key) {
    try {
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"ExpandNode", &dispid);
        if (FAILED(hr)) {
            throw ComException("ExpandNode method not found - element may not be a tree control", hr);
        }

        // the key as UTF-8 (it went through the ANSI code page before)
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(node_key)});

        if (FAILED(hr)) {
            throw ComException("Failed to expand tree node '" + node_key + "'", hr);
        }

        spdlog::debug("Successfully expanded tree node: {}", node_key);

    } catch (const std::exception& e) {
        spdlog::error("expand_node failed for key '{}': {}", node_key, e.what());
        throw;
    }
}

void ComGuiElement::collapse_node(const std::string& node_key) {
    try {
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"CollapseNode", &dispid);
        if (FAILED(hr)) {
            throw ComException("CollapseNode method not found - element may not be a tree control", hr);
        }

        // the key as UTF-8 (it went through the ANSI code page before)
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(node_key)});

        if (FAILED(hr)) {
            throw ComException("Failed to collapse tree node '" + node_key + "'", hr);
        }

        spdlog::debug("Successfully collapsed tree node: {}", node_key);

    } catch (const std::exception& e) {
        spdlog::error("collapse_node failed for key '{}': {}", node_key, e.what());
        throw;
    }
}

void ComGuiElement::doubleclick_node(const std::string& node_key) {
    try {
        // Some list trees (SUIM) expose the visible label as a TEXT item while
        // GetNodeTextByKey returns an empty string. DoubleClickNode succeeds but
        // does not activate that text item on those controls.
        if (get_node_text_by_key(node_key).empty()) {
            const auto item_columns = get_tree_column_names();
            if (std::find(item_columns.begin(), item_columns.end(), "TEXT") != item_columns.end() &&
                !get_item_text(node_key, "TEXT").empty()) {
                DISPID item_dispid;
                if (SUCCEEDED(get_dispid_via_typeinfo(dispatch_, L"DoubleClickItem", &item_dispid))) {
                    // DoubleClickItem(node key, item name "TEXT")
                    HRESULT hr = invoke(dispatch_, item_dispid, DISPATCH_METHOD, {bstr_arg(node_key), bstr_arg("TEXT")});
                    if (FAILED(hr)) throw ComException("Failed to double-click tree text item", hr);
                    return;
                }
            }
        }

        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, L"DoubleClickNode", &dispid);
        if (FAILED(hr)) {
            throw ComException("DoubleClickNode method not found - element may not be a tree control", hr);
        }

        // the key as UTF-8 (it went through the ANSI code page before)
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {bstr_arg(node_key)});

        if (FAILED(hr)) {
            throw ComException("Failed to double-click tree node '" + node_key + "'", hr);
        }

        spdlog::debug("Successfully double-clicked tree node: {}", node_key);

    } catch (const std::exception& e) {
        spdlog::error("doubleclick_node failed for key '{}': {}", node_key, e.what());
        throw;
    }
}

void ComGuiElement::select_node_context_item(const std::string& node_key,
                                             const std::string& item_text) {
    if (!dispatch_) throw ComException("Null tree element");
    if (item_text.empty()) throw ComException("Context-menu item text is empty");

    // UTF-8 key and item text (they went through the ANSI code page before)
    call(L"NodeContextMenu", {bstr_arg(node_key)}, "Tree context-menu method not found", "Tree context-menu action failed");
    call(L"SelectContextMenuItemByText", {bstr_arg(item_text)}, "Tree context-menu method not found",
         "Tree context-menu action failed");
}

// ============================================================================
// GuiShell Toolbar Button Methods
// ============================================================================

int ComGuiElement::get_button_count() const {
    return get_int_property(L"ButtonCount");
}

std::string ComGuiElement::button_string(const wchar_t* method, int position) const {
    try {
        _variant_t result;
        const HRESULT hr = call_method(method, {_variant_t(position)}, result);
        if (FAILED(hr))
            spdlog::debug("{} failed for position {}: hr=0x{:08X}", text::wide_to_utf8_lossy(method), position, (unsigned)hr);
        return text_result(hr, result);
    } catch (const std::exception& e) {
        spdlog::debug("{} failed for position {}: {}", text::wide_to_utf8_lossy(method), position, e.what());
        return "";
    }
}

std::string ComGuiElement::get_button_id(int position) const { return button_string(L"GetButtonId", position); }
std::string ComGuiElement::get_button_text(int position) const { return button_string(L"GetButtonText", position); }
std::string ComGuiElement::get_button_tooltip(int position) const { return button_string(L"GetButtonTooltip", position); }
std::string ComGuiElement::get_button_type(int position) const { return button_string(L"GetButtonType", position); }

bool ComGuiElement::get_button_enabled(int position) const {
    try {
        _variant_t result;
        if (FAILED(call_method(L"GetButtonEnabled", {_variant_t(position)}, result))) return false;
        return (result.vt == VT_UI1 || result.vt == VT_BOOL) && result.boolVal != 0;   // a byte or a boolean
    } catch (const std::exception& e) {
        spdlog::debug("get_button_enabled failed for position {}: {}", position, e.what());
        return false;
    }
}

bool ComGuiElement::find_toolbar_button_labels(const std::string& button_id, std::string& text,
                                               std::string& tooltip) const {
    try {
        int count = 0;
        try { count = get_button_count(); } catch (const std::exception&) {}
        for (int i = 0; i < count; ++i) {
            if (get_button_id(i) != button_id) continue;
            text = get_button_text(i);
            tooltip = get_button_tooltip(i);
            return true;
        }
        const int grid_count = get_grid_toolbar_button_count();
        for (int i = 0; i < grid_count; ++i) {
            if (get_grid_toolbar_button_id(i) != button_id) continue;
            text.clear();
            tooltip = get_grid_toolbar_button_tooltip(i);
            return true;
        }
    } catch (const std::exception& e) {
        spdlog::debug("find_toolbar_button_labels failed for '{}': {}", button_id, e.what());
    }
    return false;
}

void ComGuiElement::press_button(const std::string& button_id) {
    try {
        const std::wstring wide_id = com::utf8_to_wide(button_id);
        HRESULT last_error = DISP_E_MEMBERNOTFOUND;
        // GuiShell Toolbar uses PressButton; GuiGridView's built-in toolbar
        // uses PressToolbarButton. Both are exposed as synthetic CLI buttons.
        for (const wchar_t* method_name : {L"PressButton", L"PressToolbarButton"}) {
            _variant_t result;
            const HRESULT hr = call_method(method_name, {_variant_t(wide_id.c_str())}, result);
            if (SUCCEEDED(hr)) {
                spdlog::debug("Pressed toolbar button: {}", button_id);
                return;
            }
            last_error = hr;
        }
        throw ComException("Failed to press button '" + button_id + "'", last_error);
    } catch (const std::exception& e) {
        spdlog::error("press_button failed for button_id '{}': {}", button_id, e.what());
        throw;
    }
}

void ComGuiElement::press_f4() {
    try {
        // F4 is sent as a virtual key event to the element (focus first; a failure is ignored)
        _variant_t ignored;
        (void)call_method(L"SetFocus", {}, ignored);
        _variant_t result;
        if (SUCCEEDED(call_method(L"SendVKey", {_variant_t(4)}, result)))   // SAP's index 4 is F4
            spdlog::debug("Sent F4 key directly to element");
        else
            spdlog::debug("Element does not take SendVKey directly; focus set for window send_vkey");
    } catch (const std::exception& e) {
        spdlog::error("press_f4 failed: {}", e.what());
        throw;
    }
}

} // namespace sap
} // namespace fairyfly

