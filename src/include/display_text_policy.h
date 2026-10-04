#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace fairyfly {
namespace sap {

/// Splits a table-control field leaf such as "txtVALUE[1,3]" into {"txtVALUE", "3"}.
/// Leaves without a numeric row come back unchanged with an empty row.
std::pair<std::string, std::string> split_field_index(const std::string& field);

/// The sibling leaves that can name a "...VALUE" field: base + NAME/KEY/FIELDNAME/HEADERNAME.
/// `row` is the table-control row of the value field, empty outside table controls.
struct SiblingQuery {
    std::array<std::string, 4> leaves;
    std::string row;
};

/// Receives the Text of one sibling (index into SiblingQuery::leaves). Returning false means
/// the sibling names a credential and the probe should stop.
using SiblingVisitor = std::function<bool(std::size_t index, const std::string& text)>;

/// Observes the sibling name fields of one VALUE field's row and feeds each text to `visit`.
/// Returns false when the siblings could not be observed (unverified); returns true after
/// everything was visited or the visitor asked to stop.
using SiblingNameProbe = std::function<bool(const SiblingQuery&, const SiblingVisitor&)>;

/// Everything resolve_display_text needs. The readers are lazy so a caller that talks to COM
/// keeps issuing only the reads (and in the order) the decision needs.
struct DisplayTextInputs {
    std::string type;
    std::function<std::string()> id;
    std::function<std::string()> label;
    /// nullopt = Changeable could not be read.
    std::function<std::optional<bool>()> changeable;
    /// nullopt = the DisplayedText property is not available on this element.
    std::function<std::optional<std::string>()> displayed_text;
    std::function<std::string()> text;
    SiblingNameProbe sibling_probe;
};

/// The credential-safe text of one element: a redaction marker for password fields,
/// credential-named fields and VALUE fields whose NAME sibling is a credential or cannot be
/// verified, otherwise DisplayedText (falling back to Text) with structured credential text
/// masked. Pure decision logic shared by the COM reader and any other element source.
std::string resolve_display_text(const DisplayTextInputs& in);

} // namespace sap
} // namespace fairyfly
