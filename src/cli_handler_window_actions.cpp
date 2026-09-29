// CommandHandler entry points for key send, popup close and menu list/select.
#include "include/cli_handler.h"
#include "include/com_automation_engine.h"
#include "include/action_argument_checks.h"
#include "include/read_only_guard.h"
#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace fairyfly {
namespace cli {

namespace {
Result engine_mismatch(const char* what) {
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "ENGINE_TYPE_MISMATCH";
    result.error["message"] = std::string(what) + " requires the COM automation engine";
    return result;
}
} // namespace

Result CommandHandler::handle_send_key(const std::string& key, const std::string& window,
                                       std::optional<int> connection_id)
{
    if (auto invalid = sap::check_vkey_argument(key)) return *invalid;
    const auto vkey = sap::parse_vkey(key);
    // Read-only: allowlist. Enter (0) is judged below once the active window is known.
    if (read_only_ && *vkey != 0) {
        const auto rule = sap::read_only_vkey_rule(*vkey, 0);
        if (!rule.empty()) return sap::make_read_only_refusal("vkey:" + key, "Key", key, "", rule);
    }

    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }
    auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
    if (!com_engine) return engine_mismatch("key send");
    if (read_only_ && *vkey == 0) {
        // The target window decides: Enter on a popup may confirm a Save/Delete dialog.
        const std::string target = (window.empty() || window == "@active")
            ? com_engine->get_active_window_id().id : window;
        const auto rule = sap::read_only_vkey_rule(0, WindowId(target).get_index());
        if (!rule.empty()) return sap::make_read_only_refusal("vkey:" + key, "Key", key, "", rule);
    }

    spdlog::info("Sending VKey {} to {} on connection {}", *vkey, window, conn_result.value.id);
    Result result = com_engine->send_key(*vkey, window);
    if (result.status == Result::Status::Success) {
        result.data["key"] = key;
        result.data["connection_id"] = conn_result.value.id;
    }
    return result;
}

Result CommandHandler::handle_close(int vkey, std::optional<int> connection_id)
{
    if (read_only_) {
        // close always acts on a popup, so Enter would be a confirmation there.
        const auto rule = sap::read_only_vkey_rule(vkey, 1);
        if (!rule.empty())
            return sap::make_read_only_refusal("vkey:" + std::to_string(vkey), "Key", "", "", rule);
    }
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }
    auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
    if (!com_engine) return engine_mismatch("popup close");

    Result result = com_engine->close_popup(vkey);
    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }
    return result;
}

Result CommandHandler::handle_screen_menu(const std::string& select_path, const std::string& window,
                                          std::optional<int> connection_id)
{
    if (read_only_ && !select_path.empty()) {
        // Every path segment is a menu label; refuse when any of them names a state-changing action.
        size_t start = 0;
        while (start <= select_path.size()) {
            size_t end = select_path.find('/', start);
            if (end == std::string::npos) end = select_path.size();
            // Normalize like the menu matcher does ('&' accelerators stripped) so 'Sa&ve' cannot slip past.
            const std::string segment = sap::normalize_menu_segment(select_path.substr(start, end - start));
            const auto rule = sap::matched_read_only_rule("GuiMenu", segment, "", "");
            if (!rule.empty())
                return sap::make_read_only_refusal(select_path, "GuiMenu", segment, "", rule);
            start = end + 1;
        }
    }
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }
    auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
    if (!com_engine) return engine_mismatch("menu");

    // Enumeration never selects; selection only happens for an explicit --select path.
    Result result = select_path.empty() ? com_engine->read_menu(window)
                                        : com_engine->select_menu(select_path, window, read_only_);
    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }
    return result;
}

} // namespace cli
} // namespace fairyfly
