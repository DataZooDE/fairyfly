// CommandHandler entry points for send-key, close and screen menu.
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
    if (read_only_ && sap::is_state_changing_vkey(*vkey)) {
        return sap::make_read_only_refusal("vkey:" + key, "Key", key, "", "vkey:" + std::to_string(*vkey));
    }

    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }
    auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
    if (!com_engine) return engine_mismatch("send-key");

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
    if (read_only_ && sap::is_state_changing_vkey(vkey)) {
        return sap::make_read_only_refusal("vkey:" + std::to_string(vkey), "Key", "", "", "vkey:" + std::to_string(vkey));
    }
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }
    auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
    if (!com_engine) return engine_mismatch("close");

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
            const std::string segment = select_path.substr(start, end - start);
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
    if (!com_engine) return engine_mismatch("screen menu");

    // Enumeration never selects; selection only happens for an explicit --select path.
    Result result = select_path.empty() ? com_engine->read_menu(window)
                                        : com_engine->select_menu(select_path, window);
    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }
    return result;
}

} // namespace cli
} // namespace fairyfly
