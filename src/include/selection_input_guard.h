#pragma once
// Handler-level LIVE validation for selection input (a read-only MCP token created with --allow-selection-input types
// into a selection field). The dispatcher decides by element ID and by the screen read BEFORE the call; this guard
// looks at the real control right before SetText: its type, whether it is changeable, whether its id / name / label /
// tooltip name a credential, and whether the session still shows the screen the token started on (closing the window
// between the pre-call check and the write). Pure logic, no COM: the engine fills the probe from the live element.

#include <algorithm>
#include <optional>
#include <string>

#include "include/sensitive_data.h"

namespace fairyfly::sap {

/// What the dispatcher allows for ONE fill call (set by the handler for that call only, then cleared).
struct SelectionInputPolicy {
    bool active = false;
    std::string program;        ///< Info.Program of the recorded initial screen
    std::string screen_number;  ///< Info.ScreenNumber of the recorded initial screen
};

/// The live facts about the target control and the screen at write time.
struct SelectionInputProbe {
    std::string type;           ///< SAP GUI type name (GuiTextField, ...)
    std::string id;
    std::string name;
    std::string label;          ///< live label text (may be empty)
    std::string tooltip;
    bool changeable = false;
    bool changeable_known = false;
    std::string program;        ///< live Info.Program read immediately before the write ("" = unknown)
    std::string screen_number;  ///< live Info.ScreenNumber ("" = unknown)
};

struct SelectionInputRefusal {
    std::string code;     ///< INPUT_TARGET_DENIED or INPUT_SCREEN_DENIED
    std::string message;
};

/// nullopt = the write may proceed. Fails closed: unknown changeability, unknown screen and every credential hint refuse.
inline std::optional<SelectionInputRefusal> evaluate_selection_input(const SelectionInputProbe& probe,
                                                                     const SelectionInputPolicy& policy) {
    if (!policy.active) return std::nullopt;
    const auto target = [](const std::string& why) {
        return SelectionInputRefusal{"INPUT_TARGET_DENIED", why};
    };
    if (probe.type != "GuiTextField" && probe.type != "GuiCTextField")
        return target("typing is only allowed into plain text fields (GuiTextField / GuiCTextField), not into " +
                      (probe.type.empty() ? std::string("an element of unknown type") : probe.type));
    if (!probe.changeable_known || !probe.changeable)
        return target("the field is not known to be changeable on the current screen");
    if (!sensitive_input_field_reason(probe.type, probe.id, probe.label).empty() ||
        !sensitive_input_field_reason(probe.type, probe.name, probe.label).empty() ||
        !sensitive_input_field_reason(probe.type, probe.id, probe.tooltip).empty() ||
        contains_sensitive_data_name(probe.id) || contains_sensitive_data_name(probe.name) ||
        contains_sensitive_data_name(probe.label) || contains_sensitive_data_name(probe.tooltip))
        return target("typing into password or credential fields is never allowed");
    if (probe.program.empty() || probe.screen_number.empty() || probe.program != policy.program ||
        probe.screen_number != policy.screen_number)
        return SelectionInputRefusal{"INPUT_SCREEN_DENIED",
                                     "the screen changed since the transaction was started; typing is only allowed on the initial "
                                     "screen of the transaction; start it again with gui_transaction_start"};
    return std::nullopt;
}

}  // namespace fairyfly::sap
