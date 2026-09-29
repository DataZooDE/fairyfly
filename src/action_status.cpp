#include "include/action_status.h"

#include <algorithm>
#include <cctype>

namespace fairyfly::sap {

std::optional<Result> classify_list_label_outcome(bool same_label,
                                                  bool same_window,
                                                  bool same_title,
                                                  const ActionStatus& before,
                                                  const ActionStatus& after,
                                                  const std::string& element) {
    if (!same_label || !same_window || !same_title ||
        before.text != after.text || before.type != after.type)
        return std::nullopt;
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "ACTION_OUTCOME_UNVERIFIED";
    result.error["message"] = "List label activation left the screen and status unchanged";
    result.error["element"] = element;
    return result;
}

bool is_missing_element_error(const std::string& message) {
    return message.rfind("Element not found: ", 0) == 0;
}

bool should_activate_positioned_label(const std::string& type,
                                      const std::string& path,
                                      int window_index,
                                      bool is_hotspot,
                                      bool is_list_element) {
    return type == "GuiLabel" && path.find("/usr/") != std::string::npos &&
           path.find("/lbl[") != std::string::npos &&
           (window_index > 0 || is_hotspot || is_list_element);
}

std::optional<Result> classify_transaction_modal(const std::string& window_id,
                                                 std::string message,
                                                 const std::string& tcode) {
    if (window_id.find("wnd[1]") == std::string::npos) return std::nullopt;
    while (!message.empty() && std::isspace(static_cast<unsigned char>(message.back())))
        message.pop_back();
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::string expected = "cannot start transaction " + tcode;
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower != expected) return std::nullopt;

    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "TRANSACTION_FAILED";
    result.error["message"] = message;
    result.error["tcode"] = tcode;
    return result;
}

std::optional<Result> classify_f4_outcome(bool dialog_opened,
                                         const ActionStatus& status,
                                         const std::string& element) {
    if (dialog_opened) return std::nullopt;
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "F4_DIALOG_NOT_OPENED";
    result.error["message"] = status.text.empty()
        ? "F4 search help did not open a dialog" : status.text;
    result.error["element"] = element;
    result.error["f4_dialog_opened"] = false;
    if (!status.type.empty()) result.error["message_type"] = status.type;
    return result;
}

std::optional<Result> classify_f4_target_window(const std::string& active_window,
                                                const std::string& element) {
    if (WindowId(active_window).get_index() == WindowId(element).get_index())
        return std::nullopt;
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "WINDOW_MISMATCH";
    result.error["message"] = "F4 field is not in the active window";
    result.error["element"] = element;
    result.error["active_window"] = active_window;
    return result;
}

std::string f4_dialog_path(int active_window_index) {
    return "wnd[" + std::to_string(active_window_index + 1) + "]";
}

ActionStatus read_action_status(const ComGuiSessionPtr& session) {
    try {
        auto bar = session->find_element_by_id("wnd[0]/sbar");
        if (bar) return {bar->get_text(), bar->get_property_string(L"MessageType")};
    } catch (const std::exception&) {
        // Some screens and modal dialogs do not expose the main status bar.
    }
    return {};
}

std::optional<Result> classify_action_status(const ActionStatus& before,
                                             const ActionStatus& after,
                                             const std::string& element,
                                             bool submitting_action) {
    if (after.text.empty())
        return std::nullopt;

    const bool repeated = after.text == before.text && after.type == before.type;
    std::string lower = after.text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool rejection_text = lower.find("does not exist") != std::string::npos ||
                                lower.find("not authorized") != std::string::npos ||
                                lower.find("cannot be selected") != std::string::npos ||
                                lower.find("not possible in read-only mode") != std::string::npos;
    if (after.type == "W" && submitting_action && !rejection_text) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "ACTION_OUTCOME_UNVERIFIED";
        result.error["message"] = after.text;
        result.error["message_type"] = after.type;
        result.error["element"] = element;
        result.error["reason"] = repeated
            ? "The same SAP warning was present before and after the action"
            : "SAP warning may require confirmation before the action completes";
        return result;
    }
    if (after.type != "E" && after.type != "A" && !rejection_text)
        return std::nullopt;

    if (repeated && !submitting_action) return std::nullopt;

    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = repeated ? "ACTION_OUTCOME_UNVERIFIED" :
                            after.type == "A" ? "ACTION_ABORTED" : "ACTION_FAILED";
    result.error["message"] = after.text;
    result.error["message_type"] = after.type;
    result.error["element"] = element;
    if (repeated) result.error["reason"] = "The same rejection was present before and after the action";
    return result;
}

} // namespace fairyfly::sap
