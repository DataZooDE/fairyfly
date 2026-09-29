// Window-level and grid/menu actions for ComAutomationEngine: doubleclick_grid_cell,
// send_key, close_popup, read_menu and select_menu.
#include "include/com_automation_engine.h"
#include "include/action_status.h"
#include "include/constants.h"
#include "include/menu_navigation.h"
#include "include/vkey.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>

namespace fairyfly {
namespace sap {

namespace {

using Clock = std::chrono::high_resolution_clock;

std::chrono::milliseconds elapsed_since(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
}

/// Single switch point for status-bar handling of the actions in this file.
/// Replace the body with the shared attach_status_bar helper once it exists.
/// Returns true when SAP rejected the action; `result` then holds the error.
bool apply_status_outcome(Result& result, const ActionStatus& before, const ActionStatus& after,
                          const std::string& element, bool submitting) {
    if (auto rejection = classify_action_status(before, after, element, submitting)) {
        result = *rejection;
        return true;
    }
    if ((after.text != before.text || after.type != before.type) &&
        !after.text.empty() && after.type == "W") {
        result.data["warning"] = after.text;
    }
    return false;
}

Result error_result(const char* code, const std::string& message) {
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = code;
    result.error["message"] = message;
    return result;
}

Result exception_result(const std::exception& e, bool com) {
    return error_result(com ? "COM_ERROR" : "EXCEPTION", e.what());
}

} // namespace

Result ComAutomationEngine::doubleclick_grid_cell(const ElementId& element, int row,
                                                  const std::string& column) {
    const auto start = Clock::now();
    try {
        ElementId resolved = resolve_element_path(element);
        auto session = ensure_session();
        auto grid = session->find_element_by_id(resolved.path);
        if (!grid) {
            auto result = error_result("ELEMENT_NOT_FOUND", "GridView not found: " + resolved.path);
            result.error["element"] = resolved.path;
            return result;
        }
        const std::string type = grid->get_type();
        if (type != "GuiGridView" &&
            (type != "GuiShell" || grid->get_string_property(L"SubType") != "GridView")) {
            return error_result("WRONG_ELEMENT_TYPE", "Element is not a GridView");
        }

        const auto before_status = read_action_status(session);
        grid->doubleclick_grid_cell(row, column);
        session->wait_for_completion(500);
        const auto after_status = read_action_status(session);

        Result result;
        if (apply_status_outcome(result, before_status, after_status, resolved.path, true)) {
            result.duration = elapsed_since(start);
            return result;
        }
        result.status = Result::Status::Success;
        result.data["action"] = "doubleclick_grid_cell";
        result.data["element"] = resolved.path;
        result.data["row"] = row;
        result.data["column"] = column;
        result.duration = elapsed_since(start);
        return result;
    } catch (const ComException& e) {
        return exception_result(e, true);
    } catch (const std::exception& e) {
        return exception_result(e, false);
    }
}

Result ComAutomationEngine::send_key(int vkey, const std::string& window) {
    const auto start = Clock::now();
    try {
        auto session = ensure_session();
        ComGuiWindowPtr target;
        if (window.empty() || window == "@active") {
            target = session->get_active_window();
        } else {
            auto element = session->find_element_by_id(window);
            if (element) target = ComGuiWindow::create(element->get_dispatch());
        }
        if (!target) {
            auto result = error_result("WINDOW_NOT_FOUND", "Window not found: " +
                                       (window.empty() ? std::string("@active") : window));
            result.error["window"] = window;
            return result;
        }
        const std::string window_before = target->get_id();
        const auto before_status = read_action_status(session);

        target->send_vkey(vkey);
        session->wait_for_completion(500);
        const auto after_status = read_action_status(session);

        Result result;
        // Enter, Execute (F8) and Save (Ctrl+S = 11) submit; other keys navigate.
        const bool submitting = vkey == 0 || vkey == 8 || vkey == 11;
        if (apply_status_outcome(result, before_status, after_status, window_before, submitting)) {
            result.duration = elapsed_since(start);
            return result;
        }
        result.status = Result::Status::Success;
        result.data["action"] = "send_key";
        result.data["vkey"] = vkey;
        result.data["window"] = window_before;
        result.data["window_after"] = get_active_window_id().id;
        result.duration = elapsed_since(start);
        return result;
    } catch (const ComException& e) {
        return exception_result(e, true);
    } catch (const std::exception& e) {
        return exception_result(e, false);
    }
}

Result ComAutomationEngine::close_popup(int vkey) {
    const auto start = Clock::now();
    try {
        auto session = ensure_session();
        auto window = session->get_active_window();
        if (!window) return error_result("NO_ACTIVE_WINDOW", "Could not get the active window");
        const std::string window_before = window->get_id();
        const int before_index = WindowId(window_before).get_index();
        if (classify_close_outcome(before_index, before_index) == CloseOutcome::NoPopup) {
            auto result = error_result("NO_POPUP",
                "The active window is the main window; there is no popup to close");
            result.error["window"] = window_before;
            return result;
        }

        const auto before_status = read_action_status(session);
        window->send_vkey(vkey);
        try { session->wait_for_completion(500); } catch (const ComException&) { /* poll below */ }

        int after_index = before_index;
        std::string window_after = window_before;
        for (int attempt = 0; attempt < 20; ++attempt) {
            window_after = get_active_window_id().id;
            after_index = WindowId(window_after).get_index();
            if (classify_close_outcome(before_index, after_index) == CloseOutcome::Closed) break;
            if (attempt < 19) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (classify_close_outcome(before_index, after_index) != CloseOutcome::Closed) {
            auto result = error_result("POPUP_STILL_OPEN",
                "The popup did not close after sending VKey " + std::to_string(vkey));
            result.error["window"] = window_before;
            result.error["vkey"] = vkey;
            result.duration = elapsed_since(start);
            return result;
        }

        Result result;
        const auto after_status = read_action_status(session);
        if (apply_status_outcome(result, before_status, after_status, window_before, false)) {
            result.duration = elapsed_since(start);
            return result;
        }
        result.status = Result::Status::Success;
        result.data["action"] = "close";
        result.data["vkey"] = vkey;
        result.data["window_before"] = window_before;
        result.data["window_after"] = window_after;
        result.data["closed"] = true;
        result.duration = elapsed_since(start);
        return result;
    } catch (const ComException& e) {
        return exception_result(e, true);
    } catch (const std::exception& e) {
        return exception_result(e, false);
    }
}

Result ComAutomationEngine::read_menu(const std::string& window) {
    const auto start = Clock::now();
    try {
        ElementId resolved = resolve_element_path(ElementId((window.empty() ? "wnd[0]" : window) + "/mbar"));
        auto session = ensure_session();
        auto menubar = session->find_element_by_id(resolved.path);
        if (!menubar) {
            auto result = error_result("MENU_NOT_FOUND", "Menu bar not found: " + resolved.path);
            result.error["element"] = resolved.path;
            return result;
        }
        Result result;
        result.status = Result::Status::Success;
        result.data["action"] = "read_menu";
        result.data["element"] = resolved.path;
        result.data["menu"] = read_menu_tree(menubar);
        result.duration = elapsed_since(start);
        return result;
    } catch (const ComException& e) {
        return exception_result(e, true);
    } catch (const std::exception& e) {
        return exception_result(e, false);
    }
}

Result ComAutomationEngine::select_menu(const std::string& menu_path, const std::string& window) {
    const auto start = Clock::now();
    try {
        const auto segments = split_menu_path(menu_path);
        if (segments.empty()) return error_result("INVALID_MENU_PATH", "Menu path is empty");

        ElementId resolved = resolve_element_path(ElementId((window.empty() ? "wnd[0]" : window) + "/mbar"));
        auto session = ensure_session();
        auto menubar = session->find_element_by_id(resolved.path);
        if (!menubar) {
            auto result = error_result("MENU_NOT_FOUND", "Menu bar not found: " + resolved.path);
            result.error["element"] = resolved.path;
            return result;
        }
        auto item = find_menu_by_path(menubar, segments);
        if (!item) {
            auto result = error_result("MENU_ITEM_NOT_FOUND", "No menu item matches: " + menu_path);
            result.error["menu_path"] = menu_path;
            result.error["suggestions"] = nlohmann::json::array({"Run 'screen menu' to list available menu items"});
            return result;
        }
        const std::string item_id = item->get_id();
        if (!item->is_enabled()) {
            auto result = error_result("ELEMENT_DISABLED", "Menu item is disabled: " + menu_path);
            result.error["element"] = item_id;
            return result;
        }

        const auto before_status = read_action_status(session);
        item->select(true);
        session->wait_for_completion(500);
        const auto after_status = read_action_status(session);

        Result result;
        if (apply_status_outcome(result, before_status, after_status, item_id, true)) {
            result.duration = elapsed_since(start);
            return result;
        }
        result.status = Result::Status::Success;
        result.data["action"] = "select_menu";
        result.data["menu_path"] = menu_path;
        result.data["element"] = item_id;
        result.data["window_after"] = get_active_window_id().id;
        result.duration = elapsed_since(start);
        return result;
    } catch (const ComException& e) {
        return exception_result(e, true);
    } catch (const std::exception& e) {
        return exception_result(e, false);
    }
}

} // namespace sap
} // namespace fairyfly
