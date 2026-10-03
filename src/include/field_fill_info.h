#pragma once

#include "core.h"
#include <string>

namespace fairyfly::sap {

/// What the fill path knows about a text input after typing into it. Plain data so the shaping below is
/// unit-testable without a COM object.
struct FieldProbe {
    std::string type;          ///< GuiTextField, GuiCTextField, GuiComboBox, ...
    std::string id;            ///< full element id
    std::string name;          ///< control name (e.g. ctxtP_DATUM)
    std::string label;         ///< associated label text, may be empty
    std::string text_before;   ///< value shown before the fill (may be empty)
    std::string text_after;    ///< value read back from the control after the fill
    bool text_after_known = false;
    int max_length = 0;        ///< 0 = unknown
    bool numerical = false;
    bool required = false;
};

constexpr size_t kFillEchoMaxChars = 200;

/// "date", "time", "numeric" or "" (plain text / unknown). Heuristic: the scripting API exposes no DDIC type, so
/// the control name, label, MaxLength and the shape of the displayed value are used.
std::string infer_input_kind(const FieldProbe& probe);

/// SAP date formats by the shape of a displayed or typed date: "DD.MM.YYYY", "MM/DD/YYYY", "MM-DD-YYYY",
/// "YYYY.MM.DD", "YYYY/MM/DD", "YYYY-MM-DD". Empty when the text is not date-shaped. Time: "HH:MM:SS".
std::string date_format_of_text(const std::string& text);
std::string time_format_of_text(const std::string& text);

/// The `value` of a fill result: the typed value as read back from the control (truncated to 200 characters, with
/// an ellipsis marker) or "[REDACTED: reason]" for password/credential fields. `redacted` reports which one.
std::string fill_value_echo(const FieldProbe& probe, const std::string& typed, bool& redacted);

/// The `field` object of a fill result: {type, max_length?, input_kind?, format_hint?, format_hint_source?,
/// numerical?, required?, value_normalized?, format_warning?}. Never a hard validation: a mismatch with SAP's own
/// normalisation (format_hint_source "normalized_input") only adds
/// format_warning. For credential fields nothing but the type is reported.
json build_fill_field_info(const FieldProbe& probe, const std::string& typed);

/// Controls whose caption is often an icon: element get / screen find read their tooltip as `screen read` does.
bool type_shows_tooltip(const std::string& type);

/// Adds data["tooltip"] when non-empty (absent otherwise) and, when `text` is non-empty and differs from the
/// already reported data["value"] (get) / data["text"] (find), data["text"] as well. No-op for other types.
void attach_tooltip_fields(json& data, const std::string& type, const std::string& tooltip,
                           const std::string& text);

} // namespace fairyfly::sap
