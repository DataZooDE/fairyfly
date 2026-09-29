#pragma once

#include "core.h"
#include "com/wrapper.h"
#include <optional>
#include <string>

namespace fairyfly::sap {

struct ActionStatus {
    std::string text;
    std::string type;
};

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
