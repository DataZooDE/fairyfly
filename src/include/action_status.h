#pragma once

#include "core.h"
#include "com/wrapper.h"
#include <optional>
#include <string>

namespace fairyfly::sap {

struct ActionStatus {
    std::string text;
    std::string type;
    std::string message_id;
    std::string message_number;
};

/// JSON view of a status bar message: {text, message_type, changed}.
/// Returns null when there is no message text. `changed` is only emitted when
/// a previous status is supplied.
json status_bar_json(const ActionStatus& status, const ActionStatus* before = nullptr);

/// Success-side view of a status message: data["status_message"] = {type, text}, plus data["warning"] = true when a
/// NEW type-W message appeared. Benign W/I/S messages after an action that otherwise worked are not errors
/// (classify_action_status ignores them); this is how the caller still sees them. No-op for empty text.
void attach_status_message(json& data, const ActionStatus& before, const ActionStatus& after);

/// Attach the status bar message to result.data["status_bar"] on success and
/// result.error["status_bar"] on error. No-op when the message is empty.
void attach_status_bar(Result& result, const ActionStatus& before, const ActionStatus& after);

/// Like attach_status_bar, but only when the action itself produced a message: an unchanged status bar (same text and
/// type as before) is left over from an earlier action and is omitted. For actions that do not talk to the server
/// (typing into a field), where a re-read right after the call would otherwise echo the previous message.
void attach_fresh_status_bar(Result& result, const ActionStatus& before, const ActionStatus& after);

/// Single-snapshot variant (no change detection) for read-only commands.
void attach_status_bar(Result& result, const ActionStatus& current);

struct TransactionRequest {
    bool valid = true;
    std::string command;         // what to send / start
    std::string expected_tcode;  // transaction code expected afterwards (may be empty)
    bool use_send_command = false;
    std::string error_code;      // set when !valid
    std::string error_message;
};

/// Normalises a user supplied transaction string. "/nSE38" is sent via
/// SendCommand (StartTransaction would prepend "/n" again). "/o", "/i" and
/// "/nex" are rejected because they change or close sessions.
TransactionRequest normalize_transaction_request(const std::string& input);

struct ScreenSnapshot {
    std::string window_id;
    std::string title;
    std::string transaction;
    std::string statusbar_text;
};

bool screen_snapshot_changed(const ScreenSnapshot& before, const ScreenSnapshot& after);

std::optional<Result> classify_list_label_outcome(bool same_label,
                                                  bool same_window,
                                                  bool same_title,
                                                  const ActionStatus& before,
                                                  const ActionStatus& after,
                                                  const std::string& element);

bool is_missing_element_error(const std::string& message);

bool should_activate_positioned_label(const std::string& type,
                                      const std::string& path,
                                      int window_index,
                                      bool is_hotspot,
                                      bool is_list_element);

std::optional<Result> classify_transaction_modal(const std::string& window_id,
                                                 std::string message,
                                                 const std::string& tcode);

std::optional<Result> classify_f4_outcome(bool dialog_opened,
                                         const ActionStatus& status,
                                         const std::string& element);

std::optional<Result> classify_f4_target_window(const std::string& active_window,
                                               const std::string& element);

std::string f4_dialog_path(int active_window_index);

ActionStatus read_action_status(const ComGuiSessionPtr& session);

std::optional<Result> classify_action_status(const ActionStatus& before,
                                             const ActionStatus& after,
                                             const std::string& element,
                                             bool submitting_action = false);

} // namespace fairyfly::sap
