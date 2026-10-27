#include "include/com/hresult.h"
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

namespace {
/// The padding a checkbox or combo input may carry: space, tab, CR, LF (not form feed or vertical tab).
constexpr std::string_view kInputBlanks = " \t\r\n";
}  // namespace

std::optional<bool> ComGuiElement::parse_check_value(const std::string& value) {
    const std::string v = utils::lower_ascii(utils::trim(value, kInputBlanks));
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
            const std::string wanted = utils::trim(value, kInputBlanks);
            const std::string wanted_lower = utils::lower_ascii(wanted);
            const Entry* match = nullptr;
            for (const auto& entry : entries) {  // exact key first, then the displayed value
                if (utils::trim(entry.key, kInputBlanks) == wanted) { match = &entry; break; }
            }
            if (!match) {
                for (const auto& entry : entries) {
                    if (utils::lower_ascii(utils::trim(entry.value, kInputBlanks)) == wanted_lower) { match = &entry; break; }
                }
            }
            if (!match) {  // keys that differ only in case ("en" for "EN")
                for (const auto& entry : entries) {
                    if (utils::lower_ascii(utils::trim(entry.key, kInputBlanks)) == wanted_lower) { match = &entry; break; }
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
            trace.mark_error(fmt::format("get_dispid_via_typeinfo failed: {}", hresult_hex(hr)));
            throw ComException("Press method not found on element", hr);
        }

        _variant_t result;
        hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {}, &result);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("Invoke failed: {}", hresult_hex(hr)));
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
                trace.mark_error(fmt::format("Select method not found: {}", hresult_hex(hr)));
                throw ComException("Select method not found on tab or menu element", hr);
            }

            // Call Select() method with no parameters
            _variant_t result;
            hr = invoke(dispatch_, dispid, DISPATCH_METHOD, {}, &result);
            if (FAILED(hr)) {
                trace.mark_error(fmt::format("Failed to call Select(): {}", hresult_hex(hr)));
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
            trace.mark_error(fmt::format("Selected property not found: {}", hresult_hex(hr)));
            throw ComException("Selected property not found", hr);
        }

        hr = invoke(dispatch_, dispid, DISPATCH_PROPERTYPUT, {_variant_t(selected)});   // VT_BOOL
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("Failed to set Selected: {}", hresult_hex(hr)));
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
            throw ComException(fmt::format("GetLineText failed (hr={}, type={})",
                                         hresult_hex(hr), line_result.vt), hr);
        const std::string value = com::bstr_to_utf8(line_result.bstrVal);
        if (content.text.size() + value.size() + 1 > max_chars) break;
        if (content.lines_read) content.text += '\n';
        content.text += value;
        ++content.lines_read;
    }
    content.truncated = content.lines_read < content.total_lines;
    return content;
}









// ============================================================================
// GuiShell Tree Navigation Methods
// ============================================================================






// ============================================================================
// GuiShell Toolbar Button Methods
// ============================================================================







void ComGuiElement::press_f4() {
    try {
        // F4 is sent as a virtual key event to the element (focus first; a failure is ignored)
        _variant_t ignored;
        (void)call_method(L"SetFocus", {}, ignored, Lookup::TypeInfo, Protection::Plain);
        _variant_t result;
        if (SUCCEEDED(call_method(L"SendVKey", {_variant_t(4)}, result, Lookup::TypeInfo, Protection::Plain)))   // index 4 is F4
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

