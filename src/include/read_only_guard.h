#pragma once

#include <string>
#include "core.h"

namespace fairyfly::sap {

/// Read-only guard classifiers (--read-only / FAIRYFLY_READ_ONLY=1).
///
/// Rules (case-insensitive, matched on whole words so "Step" never matches "stop"
/// and "Posting" never matches "post"; '&' accelerators and "(F8)" style hints are ignored):
///   word deny list  : save, delete, release, stop, post, activate, lock, unlock,
///                     create, change (except "change layout/view/variant/display")
///   phrase deny list: "cancel" + "job"        (cancel job / cancel active job)
///                     "execute" + "background" (Execute in background)
///                     "change" + "password", "reset" + "password"
///   id deny list    : ids containing &DELETE, &SAVE, &RELEASE, or the standard toolbar
///                     Save button tbar[0]/btn[11]
/// Never refused: Job log, Job details, Display, Details, Refresh, Back, Cancel (F12)
/// used as navigation, Spool, Step, Find, plain Execute.
/// Text and tooltip are ignored for data-holding element types (text fields, labels)
/// because their text is content, not an action label; the id rules always apply.
///
/// Returns the name of the first matching rule, or an empty string when allowed.
std::string matched_read_only_rule(const std::string& element_type, const std::string& text,
                                   const std::string& tooltip, const std::string& element_id);

/// True when the described control would change SAP state.
bool is_state_changing_action(const std::string& element_type, const std::string& text,
                              const std::string& tooltip, const std::string& element_id);

/// True for VKeys that save or delete (11 = F11 Save, 14 = Shift+F2 Delete).
bool is_state_changing_vkey(int vkey);

/// Structured READ_ONLY_REFUSED error result.
Result make_read_only_refusal(const std::string& element_id, const std::string& element_type,
                              const std::string& text, const std::string& tooltip,
                              const std::string& rule);

} // namespace fairyfly::sap
