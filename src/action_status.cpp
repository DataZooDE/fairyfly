#include "include/action_status.h"
#include "include/sensitive_data.h"

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

namespace {

bool read_bar(const ComGuiSessionPtr& session, const std::string& id, ActionStatus& out) {
    try {
        auto bar = session->find_element_by_id(id);
        if (!bar) return false;
        // SAP status text is free text and can echo credentials ("Password: x rejected"): mask it at the source.
        out.text = redact_sensitive_response_text(bar->get_text());
        out.type = bar->get_property_string(L"MessageType");
        try { out.message_id = bar->get_property_string(L"MessageId"); } catch (const std::exception&) {}
        try { out.message_number = bar->get_property_string(L"MessageNumber"); } catch (const std::exception&) {}
        return true;
    } catch (const std::exception&) {
        // Some screens and modal dialogs do not expose a status bar.
        return false;
    }
}

} // namespace

ActionStatus read_action_status(const ComGuiSessionPtr& session) {
    // Prefer the active window's status bar (dialogs can have their own), then
    // fall back to the main window's.
    try {
        auto window = session->get_active_window();
        if (window) {
            const int index = WindowId(window->get_id()).get_index();
            if (index > 0) {
                ActionStatus dialog;
                if (read_bar(session, "wnd[" + std::to_string(index) + "]/sbar", dialog) &&
                    !dialog.text.empty())
                    return dialog;
            }
        }
    } catch (const std::exception&) {
    }
    ActionStatus main;
    read_bar(session, "wnd[0]/sbar", main);
    return main;
}

json status_bar_json(const ActionStatus& status, const ActionStatus* before) {
    if (status.text.empty()) return nullptr;
    json out;
    out["text"] = status.text;
    out["message_type"] = status.type;
    if (!status.message_id.empty()) out["message_id"] = status.message_id;
    if (!status.message_number.empty()) out["message_number"] = status.message_number;
    if (before) out["changed"] = status.text != before->text || status.type != before->type;
    return out;
}

void attach_status_message(json& data, const ActionStatus& before, const ActionStatus& after) {
    if (after.text.empty()) return;
    if (!data.is_object()) data = json::object();
    data["status_message"] = {{"type", after.type}, {"text", after.text}};
    // A repeated message may be left over from an earlier action: only a new warning is flagged.
    if (after.type == "W" && (after.text != before.text || after.type != before.type)) data["warning"] = true;
}

void attach_status_bar(Result& result, const ActionStatus& before, const ActionStatus& after) {
    if (after.text.empty()) return;
    json bar = status_bar_json(after, &before);
    if (result.status == Result::Status::Success) {
        if (!result.data.is_object()) result.data = json::object();
        attach_status_message(result.data, before, after);
        result.data["status_bar"] = bar;
    } else if (result.status == Result::Status::Error) {
        if (!result.error.is_object()) result.error = json::object();
        result.error["status_bar"] = bar;
    }
}

void attach_status_bar(Result& result, const ActionStatus& current) {
    if (current.text.empty()) return;
    json bar = status_bar_json(current);
    if (result.status == Result::Status::Success) {
        if (!result.data.is_object()) result.data = json::object();
        result.data["status_bar"] = bar;
    } else if (result.status == Result::Status::Error) {
        if (!result.error.is_object()) result.error = json::object();
        result.error["status_bar"] = bar;
    }
}

TransactionRequest normalize_transaction_request(const std::string& input) {
    TransactionRequest req;
    std::string s = input;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    req.command = s;
    req.expected_tcode = s;

    auto upper = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return v;
    };
    const std::string up = upper(s);
    auto reject = [&](const char* why) {
        req.valid = false;
        req.error_code = "UNSUPPORTED_OK_CODE";
        req.error_message = std::string("OK-code '") + input + "' is not supported: " + why;
        return req;
    };
    if (up == "/NEX" || up == "/NEND") return reject("it closes the SAP GUI session");
    if (up.rfind("/O", 0) == 0) return reject("it opens a new session");
    if (up.rfind("/I", 0) == 0) return reject("it closes the current session");
    if (up.rfind("/N", 0) == 0) {
        req.use_send_command = true;
        req.command = "/n" + s.substr(2);
        req.expected_tcode = s.substr(2);
        if (req.expected_tcode.empty()) req.expected_tcode = "SESSION_MANAGER";
    }
    return req;
}

// Known limitation: only window id, title, transaction and status bar text are compared. A click
// that changes nothing but field or grid contents (for example a "Next page" control) is reported
// as screen_changed:false after the full --timeout. Behavior is intentionally unchanged.
bool screen_snapshot_changed(const ScreenSnapshot& before, const ScreenSnapshot& after) {
    return before.window_id != after.window_id || before.title != after.title ||
           before.transaction != after.transaction ||
           before.statusbar_text != after.statusbar_text;
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
    // Type W/I/S messages that do not read like a rejection are NOT failures: a click that ran and left "No short
    // dumps match the selection criteria" (W) or "No data found" (I/S) in the status bar succeeded. They are
    // reported on the success result as status_message (and warning:true for W), see attach_status_message().
    // A warning that SAP wants confirmed cannot be told apart from an ordinary one, so the caller sees the text.
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
