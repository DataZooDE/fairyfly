#pragma once

#include <string>
#include "core.h"

namespace fairyfly::sap {

class ComGuiElement;

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
/// Decision (2026-10-03, after a Codex hunt): a click on a check box or radio button of a selection screen is
/// ALLOWED under --read-only. It only changes input (like choosing a search filter), not SAP data; `element fill`
/// stays refused. Answer buttons of POPUPS are the opposite: a Yes/OK confirms whatever the dialog asks, so only
/// dismissing buttons (No, Cancel, Close, Back, Help, Details, standard Cancel) are allowed there.
///
/// Residual risk: double-click and Enter on the main window are allowed and can trigger
/// application actions the guard cannot classify; --read-only is a safety net, not a sandbox.
///
/// Returns the name of the first matching rule, or an empty string when allowed.
std::string matched_read_only_rule(const std::string& element_type, const std::string& text,
                                   const std::string& tooltip, const std::string& element_id);

/// Replaces a leading "@active" of an element id by the id of the active window so the guard sees the
/// real window index (an answer button of a popup must not slip through as @active/usr/btn...).
/// Unchanged when the id has no such prefix or the active window id is unknown (empty).
std::string expand_active_window_prefix(const std::string& element_id, const std::string& active_window_id);

/// True when the described control would change SAP state.
bool is_state_changing_action(const std::string& element_type, const std::string& text,
                              const std::string& tooltip, const std::string& element_id);

/// send-key under --read-only uses an ALLOWLIST. Allowed: F1 (1), F3 (3), F4 (4), F7 (7), F8 (8),
/// F12 (12), Shift+F3 (15), page keys (raw 80-83), and Enter (0) only when the active window is
/// the main window (index 0). Enter on a popup (index > 0) may confirm a Save/Delete dialog and
/// is refused. Returns "" when allowed, otherwise the rule "vkey:<n>". Pure function.
std::string read_only_vkey_rule(int vkey, int active_window_index);

/// True when the key is refused even on the main window (Enter counts as allowed here).
bool is_state_changing_vkey(int vkey);

/// Double-click stays allowed under --read-only (it opens ST22 dumps) but the target is inspected:
/// the grid/tree element and the addressed cell/node text are matched with the word rules.
/// Residual risk: a double-click can still trigger an application action whose label the
/// guard cannot see (for example a hotspot cell that saves).
std::string read_only_doubleclick_rule(const std::string& element_type, const std::string& element_text,
                                       const std::string& element_tooltip, const std::string& element_id,
                                       const std::string& target_text);

/// Synthetic toolbar button (<shell>/btn_<id>): read the button's text and tooltip through the
/// toolbar APIs and apply the word rules to them plus the id rules. When the lookup fails the id
/// rules alone decide (fallback). Returns the matched rule, or "". text/tooltip receive what was read.
std::string read_only_toolbar_button_rule(const ComGuiElement& shell, const std::string& button_id,
                                          const std::string& element_id, std::string& text,
                                          std::string& tooltip);

/// Lowercase, strip '&' accelerators and collapse whitespace of a menu path segment
/// (the same normalization the menu matcher applies).
std::string normalize_menu_segment(const std::string& segment);

/// Structured READ_ONLY_REFUSED error result.
Result make_read_only_refusal(const std::string& element_id, const std::string& element_type,
                              const std::string& text, const std::string& tooltip,
                              const std::string& rule);

} // namespace fairyfly::sap
