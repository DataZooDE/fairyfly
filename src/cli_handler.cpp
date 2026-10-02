#include "include/cli_handler.h"
#include "include/string_utils.h"
#include "include/read_only_guard.h"
#include "include/sensitive_data.h"
#include "include/com_automation_engine.h"
#include "include/action_status.h"
#include "include/action_argument_checks.h"
#include "include/login_flow.h"
#include "include/credential_resolver.h"
#include "include/constants.h"
#include "include/grid_analyzer.h"
#include "include/formatters/grid_renderer.h"
#include "include/formatters/screen_markdown_formatter.h"
#include "include/formatters/tree_formatter.h"
#include "include/formatters/table_formatter.h"
#include "include/formatters/markdown_table_formatter.h"
#include "include/formatters/toon_encoder.h"
#include "include/formatters/compact_json.h"
#include "include/element_renderer_registry.h"
#include "include/semantic_classifier.h"
#include "include/table_data_extractor.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <map>
#include <set>
#include <unordered_set>
#include <climits>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winreg.h>
#endif

namespace fairyfly {
namespace cli {

void CommandHandler::describe_element(const std::string& element_id, std::string& type,
                                       std::string& text, std::string& tooltip)
{
    auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
    if (!com_engine) return;
    try {
        auto session = com_engine->get_session();
        if (!session) return;
        std::string full_path = element_id;
        if (full_path.rfind("@active", 0) == 0)
            full_path = engine_->get_active_window_id().id + full_path.substr(7);
        auto element = session->find_element_by_id(full_path);
        if (!element) return;
        try { type = element->get_type(); } catch (const std::exception&) {}
        // Never read the text of input elements (a GuiPasswordField holds a secret): the guard
        // ignores their text anyway and the refusal must not be able to echo it.
        const bool input_type = type == "GuiPasswordField" || type == "GuiTextField" ||
                                type == "GuiCTextField" || type == "GuiComboBox" ||
                                type == "GuiComboBoxControl" || type == "GuiTextedit" ||
                                sap::is_sensitive_input_field(type, element_id, "");
        if (!input_type) {
            try { text = element->get_text(); } catch (const std::exception&) {}
            try { tooltip = element->get_tooltip(); } catch (const std::exception&) {}
        }
    } catch (const std::exception& e) {
        spdlog::debug("Read-only guard could not inspect {}: {}", element_id, e.what());
    }
}

std::optional<Result> CommandHandler::guard_element(const std::string& element_id)
{
    if (!read_only_) return std::nullopt;
    std::string type, text, tooltip;
    std::string rule;
    const auto btn_pos = element_id.rfind("/btn_");
    if (element_id.find("/shell/btn_") != std::string::npos && btn_pos != std::string::npos) {
        // Synthetic toolbar button (.../shell/btn_XXX): not a COM element, so read its tooltip/text
        // through the shell's toolbar APIs before pressing. If that fails, the id rules decide.
        type = "GuiButton";
        try {
            auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
            auto session = com_engine ? com_engine->get_session() : nullptr;
            if (session) {
                std::string toolbar_path = element_id.substr(0, btn_pos);
                if (toolbar_path.rfind("@active", 0) == 0)
                    toolbar_path = engine_->get_active_window_id().id + toolbar_path.substr(7);
                auto shell = session->find_element_by_id(toolbar_path);
                if (shell) {
                    rule = sap::read_only_toolbar_button_rule(*shell, element_id.substr(btn_pos + 5),
                                                              element_id, text, tooltip);
                    if (rule.empty()) return std::nullopt;
                    return sap::make_read_only_refusal(element_id, type, text, tooltip, rule);
                }
            }
        } catch (const std::exception& e) {
            spdlog::debug("Read-only guard could not inspect toolbar button {}: {}", element_id, e.what());
        }
        type.clear();
    } else {
        describe_element(element_id, type, text, tooltip);
    }
    rule = sap::matched_read_only_rule(type, text, tooltip, element_id);
    if (rule.empty()) return std::nullopt;
    return sap::make_read_only_refusal(element_id, type, text, tooltip, rule);
}

CommandHandler::CommandHandler(std::unique_ptr<cred::CredentialStore> store)
    : engine_(AutomationEngine::create()),
      conn_mgr_(std::make_unique<ConnectionManager>()),
      credential_store_(store ? std::move(store) : cred::make_default_store())
{
    // Initialize command handler
    spdlog::debug("CommandHandler initialized");
}

std::vector<int> CommandHandler::prune_dead_entries()
{
    std::vector<int> removed;
    auto all = conn_mgr_->list_connections();
    if (all.size() < 2) return removed;

    const auto parts = partition_connections(all, [this](const Connection& conn) {
        try {
            return engine_->validate_session_for_cleanup(conn.session_id, conn.server_session_key);
        } catch (const std::exception& e) {
            spdlog::debug("Cannot confirm connection {} is gone, keeping: {}", conn.id, e.what());
            return true;
        }
    });
    for (const auto& conn : parts.stale) {
        if (conn_mgr_->delete_connection_if_unchanged(conn)) {
            spdlog::info("Removed stale connection {} ({})", conn.id, conn.session_id);
            removed.push_back(conn.id);
        }
    }
    return removed;
}

ResultT<Connection> CommandHandler::resolve_and_validate_connection(std::optional<int> explicit_conn_id)
{
    if (!explicit_conn_id.has_value()) {
        prune_dead_entries();
    }

    // Resolve connection (auto-detect or explicit)
    auto conn_result = conn_mgr_->resolve_connection(explicit_conn_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return conn_result;  // Return error as-is
    }

    Connection conn = conn_result.value;

    // Validate that the session still exists
    if (!engine_->select_session(conn.session_id, conn.server_session_key)) {
        spdlog::warn("Connection {} has invalid session {}, deleting", conn.id, conn.session_id);
        conn_mgr_->delete_connection_if_unchanged(conn);

        ResultT<Connection> result;
        result.status = ResultT<Connection>::Status::Error;
        result.error["code"] = "INVALID_CONNECTION";
        result.error["message"] = fmt::format("Connection {} session no longer exists", conn.id);
        result.error["session_id"] = conn.session_id;
        result.error["suggestions"] = json::array({
            "The SAP session was closed or connection lost",
            "Run 'fairyfly session attach' to create a new connection"
        });
        return result;
    }

    // Update last_validated timestamp
    if (conn.server_session_key.empty()) {
        const auto key = engine_->current_server_session_key();
        if (!key.empty()) {
            conn = conn_mgr_->set_session_key(conn, key);
        } else {
            conn_mgr_->touch_connection(conn);
        }
    } else {
        conn_mgr_->touch_connection(conn);
    }

    ResultT<Connection> result;
    result.status = ResultT<Connection>::Status::Success;
    result.value = conn;
    return result;
}

Result CommandHandler::handle_attach(int timeout_seconds, std::optional<std::string> session_id)
{
    if (session_id) {
        spdlog::info("Attaching to SAP GUI session {}", *session_id);
    } else {
        spdlog::info("Starting window attachment mode (timeout: {}s)", timeout_seconds);
    }
    auto result = session_id ? engine_->attach_by_session_id(*session_id)
                             : engine_->attach_by_click(timeout_seconds);

    if (result.status == Result::Status::Success) {
        spdlog::info("Successfully attached to SAP window");

        // Extract connection info from result
        std::string attached_session_id = result.data.value("session_id", "");
        std::string connection_id = result.data.value("connection_id", "");
        std::string window_title = result.data.value("window_title", "");

        // Get connection metadata if available
        auto app_info = engine_->get_application_info();
        std::string description = "";
        std::string connection_string = "";

        if (app_info.contains("connections") && app_info["connections"].is_array()) {
            for (const auto& candidate : app_info["connections"]) {
                if (candidate.value("id", "") == connection_id) {
                    description = candidate.value("description", "");
                    connection_string = candidate.value("connection_string", "");
                    break;
                }
            }
        }

        // Create or update connection file
        Connection conn = conn_mgr_->create_or_update_connection(
            attached_session_id,
            connection_id,
            description,
            connection_string,
            window_title,
            engine_->current_server_session_key()
        );

        result.data["connection_file_id"] = conn.id;
        result.data["connection_file"] = conn.get_file_path();
        result.data["pruned_stale"] = conn_mgr_->prune_other_entries_for_path(
            conn, engine_->current_server_session_key());  // live key re-read right before pruning
        result.data["message"] = fmt::format("Attached to SAP GUI session (connection: {})", conn.id);

        spdlog::info("Created/updated connection file: {}", conn.get_file_path());
    } else {
        result.error["suggestions"] = session_id
            ? json::array({"Run 'fairyfly session list' to find an accessible exact session ID"})
            : json::array({
                "Ensure you clicked on an active SAP transaction window (not just SAP Logon)",
                "The window must contain an active SAP session with a transaction loaded",
                "Try running 'fairyfly doctor' to check SAP GUI status"
            });
    }

    return result;
}

Result compose_launch_login_result(Result launch, const Result& login)
{
    if (login.status == Result::Status::Success) {
        json info = {{"transaction", login.data.value("transaction", "")},
                     {"credential_source", login.data.value("credential_source", "")}};
        if (login.data.contains("warnings")) info["warnings"] = login.data["warnings"];
        if (login.data.contains("multiple_logon")) info["multiple_logon"] = login.data["multiple_logon"];
        launch.data["login"] = std::move(info);
        return launch;
    }
    Result failed = login;
    if (!failed.error.is_object()) failed.error = json::object();
    if (!failed.error.contains("connection_open")) failed.error["connection_open"] = true;
    failed.error["launch"] = launch.data;
    return failed;
}

Result CommandHandler::handle_launch(const std::string& connection_name, bool allow_sapshcut,
                                     bool login, const std::string& credential_name,
                                     const std::string& multiple_logon)
{
    spdlog::info("Launching SAP connection: {}", connection_name);
    auto result = engine_->launch_connection(connection_name, allow_sapshcut);

    if (result.status == Result::Status::Success) {
        spdlog::info("Successfully launched SAP connection: {}", connection_name);

        // Extract connection info from result
        std::string session_id = result.data.value("session_id", "");
        std::string connection_id = result.data.value("connection_id", "");

        // Get connection metadata
        auto app_info = engine_->get_application_info();
        std::string description = connection_name;
        std::string connection_string = "";

        if (app_info.contains("connections") && app_info["connections"].is_array()) {
            for (const auto& candidate : app_info["connections"]) {
                if (candidate.value("id", "") == connection_id) {
                    description = candidate.value("description", connection_name);
                    connection_string = candidate.value("connection_string", "");
                    break;
                }
            }
        }

        // Create or update connection file
        Connection conn = conn_mgr_->create_or_update_connection(
            session_id,
            connection_id,
            description,
            connection_string,
            "",  // No window title for launch
            engine_->current_server_session_key()
        );

        result.data["connection_file_id"] = conn.id;
        result.data["connection_file"] = conn.get_file_path();
        result.data["message"] = fmt::format("Launched SAP connection '{}' (connection: {})", connection_name, conn.id);

        spdlog::info("Created/updated connection file: {}", conn.get_file_path());

        // --login: authenticate through the scripting API (never a sapshcut command line),
        // also after the --allow-sapshcut fallback. Allowed under --read-only on purpose:
        // logging on is authentication, not a change of business state. On failure the
        // connection stays open and the launch data is returned with the login error.
        if (login) {
            auto login_result = handle_login("", conn.id, false, credential_name, multiple_logon);
            return compose_launch_login_result(std::move(result), login_result);
        }
    }

    return result;
}

Result CommandHandler::handle_login(const std::string& credentials_file,
                                    std::optional<int> connection_id, bool from_stdin,
                                    const std::string& credential_name,
                                    const std::string& multiple_logon)
{
    Result result;

    // Refuse before touching SAP: unknown values, and `end` (ends the user's other logons)
    // under --read-only.
    if (const auto early = plan_multiple_logon(multiple_logon, true, read_only_); early.error_code) {
        result.status = Result::Status::Error;
        result.error = {{"code", *early.error_code},
                        {"message", *early.error_code == "READ_ONLY_REFUSED"
                            ? "--multiple-logon end ends the user's other logons and is refused under --read-only"
                            : "--multiple-logon must be one of fail, keep, end, terminate"},
                        {"rule", "login:multiple-logon-end"}};
        return result;
    }

    auto selected = resolve_and_validate_connection(connection_id);
    if (selected.status != ResultT<Connection>::Status::Success) {
        return result_from_error(selected);
    }

    cred::LoginSource source;
    if (!credentials_file.empty()) source.credentials_file = credentials_file;
    source.from_stdin = from_stdin;
    if (!credential_name.empty()) source.credential_name = credential_name;
    auto resolved = cred::resolve_login_credentials(
        source, selected.value.connection_description, credential_store(), std::cin,
        [](const std::string& path) -> std::unique_ptr<std::istream> {
            auto file = std::make_unique<std::ifstream>(path, std::ios::binary);
            if (!*file) return nullptr;
            return file;
        });
    if (resolved.status != ResultT<cred::ResolvedLogin>::Status::Success) {
        result.status = Result::Status::Error;
        result.error = resolved.error;
        return result;
    }
    LoginCredentials credentials = std::move(resolved.value.credentials);
    // Scrub the plaintext secrets on every exit path.
    struct CredentialScrubber {
        LoginCredentials& credentials;
        ~CredentialScrubber() { cred::scrub_login_credentials(credentials); }
    } scrubber{credentials};
    for (const auto& warning : resolved.value.warnings) spdlog::warn("{}", warning);

    auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
    if (!com_engine || !com_engine->get_session()) {
        result.status = Result::Status::Error;
        result.error = {{"code", "ENGINE_TYPE_MISMATCH"},
                        {"message", "SAP GUI logon requires the COM automation engine"}};
        return result;
    }
    const auto session = com_engine->get_session();
    const auto password_path = selected.value.session_id + "/wnd[0]/usr/pwdRSYST-BCODE";
    const auto snapshot = [&]() {
        json screen = {{"transaction", session->get_transaction_code()}, {"elements", json::array()}};
        try {
            session->find_element_by_id(password_path);
            screen["elements"].push_back({{"id", password_path}});
        } catch (const sap::ComException& error) {
            if (!sap::is_missing_element_error(error.what())) throw;
        }
        return screen;
    };
    if (!is_sap_logon_screen(snapshot())) {
        result.status = Result::Status::Error;
        result.error = {{"code", "NOT_LOGON_SCREEN"},
                        {"message", "Selected session is not on the SAP logon screen"}};
        return result;
    }

    for (const auto& field : {
             std::pair{"@active/usr/txtRSYST-MANDT", credentials.client},
             std::pair{"@active/usr/txtRSYST-BNAME", credentials.username},
             std::pair{"@active/usr/txtRSYST-LANGU", credentials.language},
             std::pair{"@active/usr/pwdRSYST-BCODE", credentials.password}}) {
        result = engine_->fill_field(ElementId(field.first), field.second);
        if (result.status != Result::Status::Success) {
            engine_->fill_field(ElementId("@active/usr/pwdRSYST-BCODE"), "");
            return result;
        }
    }

    result = engine_->click_element(ElementId("@active/tbar[0]/btn[0]"));
    if (result.status != Result::Status::Success) {
        engine_->fill_field(ElementId("@active/usr/pwdRSYST-BCODE"), "");
        return result;
    }

    // Multiple-logon dialog (user already logged on): handled before the password change check.
    const auto dialog_prefix = selected.value.session_id + "/wnd[1]/";
    const auto dialog_element_exists = [&](const std::string& suffix) {
        try {
            session->find_element_by_id(dialog_prefix + suffix);
            return true;
        } catch (const sap::ComException& error) {
            if (!sap::is_missing_element_error(error.what())) throw;
            return false;
        }
    };
    const bool multiple_logon_dialog = dialog_element_exists("usr/radMULTI_LOGON_OPT2") ||
                                       dialog_element_exists("usr/txtMULTI_LOGON_TEXT");
    json multiple_logon_info;
    if (multiple_logon_dialog) {
        const auto plan = plan_multiple_logon(multiple_logon, true, read_only_);
        using Action = MultipleLogonPlan::Action;
        if (plan.action == Action::Refuse) {
            result.status = Result::Status::Error;
            result.error = {{"code", plan.error_code.value_or("INVALID_ARGUMENT")},
                            {"message", "--multiple-logon end is refused under --read-only"}};
            return result;
        }
        if (plan.action == Action::Fail) {
            const auto read_text = [&](const std::string& suffix) {
                try {
                    return session->find_element_by_id(dialog_prefix + suffix)->get_text();
                } catch (const std::exception&) {
                    return std::string();
                }
            };
            result.status = Result::Status::Error;
            result.error = make_multiple_logon_fail_error(read_text("usr/txtMULTI_LOGON_TEXT"),
                                                          read_text("usr/txtMULTI_LOGON_TEXT2"));
            result.error["transaction"] = session->get_transaction_code();
            result.error["password_change_detected"] = false;
            return result;
        }
        if (plan.action == Action::Select) {
            result = engine_->click_element(ElementId("@active/" + plan.radio_suffix));
            if (result.status != Result::Status::Success) return result;
            result = engine_->click_element(ElementId("@active/tbar[0]/btn[0]"));
            if (result.status != Result::Status::Success) return result;
            if (plan.choice == "terminate") {
                result = {};
                result.status = Result::Status::Error;
                result.error = {{"code", "MULTIPLE_LOGON_TERMINATED"},
                                {"message", "The user is already logged on; this logon was terminated as requested "
                                            "(--multiple-logon terminate). SAP closed the session."},
                                {"connection_open", false}};
                return result;
            }
            multiple_logon_info = make_multiple_logon_annotation(plan.choice);
        }
    }

    const auto new_password_path = selected.value.session_id + "/wnd[1]/usr/pwdRSYST-NCODE";
    bool password_change_required = false;
    try {
        session->find_element_by_id(new_password_path);
        password_change_required = true;
    } catch (const sap::ComException& error) {
        if (!sap::is_missing_element_error(error.what())) throw;
    }
    if (password_change_required) {
        if (credentials.new_password.empty()) {
            result.status = Result::Status::Error;
            result.error = {{"code", "PASSWORD_CHANGE_REQUIRED"},
                            {"message", "SAP requires a new password; provide New Password in the credential input"}};
            return result;
        }
        for (const auto* field : {"@active/usr/pwdRSYST-NCODE", "@active/usr/pwdRSYST-NCOD2"}) {
            result = engine_->fill_field(ElementId(field), credentials.new_password);
            if (result.status != Result::Status::Success) return result;
        }
        result = engine_->click_element(ElementId("@active/tbar[0]/btn[0]"));
        if (result.status != Result::Status::Success) return result;
    }

    std::string authenticated_user = session->get_user();
    for (int attempt = 0; attempt < 10 &&
         !sap_user_matches(authenticated_user, credentials.username); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        authenticated_user = session->get_user();
    }
    if (!sap_user_matches(authenticated_user, credentials.username)) {
        engine_->fill_field(ElementId("@active/usr/pwdRSYST-BCODE"), "");
        result.status = Result::Status::Error;
        result.error = {{"code", "LOGON_NOT_COMPLETED"},
                        {"message", "SAP did not authenticate the requested user"},
                        {"transaction", session->get_transaction_code()},
                        {"password_change_detected", password_change_required}};
        return result;
    }

    result = {};
    result.status = Result::Status::Success;
    result.data = {{"connection_id", selected.value.id},
                   {"session_id", selected.value.session_id},
                   {"transaction", session->get_transaction_code()},
                   {"message", "SAP GUI logon completed"},
                   {"credential_source", resolved.value.source}};
    if (!resolved.value.warnings.empty()) result.data["warnings"] = resolved.value.warnings;
    if (!multiple_logon_info.is_null()) result.data["multiple_logon"] = multiple_logon_info;
    return result;
}

Result CommandHandler::handle_disconnect(std::optional<int> connection_id, bool close_session)
{
    Result result;

    // Resolve which connection to disconnect
    if (!connection_id.has_value()) {
        // No explicit ID - drop confirmed-dead entries, then check for a single one
        prune_dead_entries();
        auto connections = conn_mgr_->list_connections();

        if (connections.empty()) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_CONNECTIONS";
            result.error["message"] = "No connection files found";
            return result;
        }

        if (connections.size() > 1) {
            result.status = Result::Status::Error;
            result.error["code"] = "MULTIPLE_CONNECTIONS";
            result.error["message"] = fmt::format("Found {} connections, specify which one to disconnect", connections.size());
            result.error["suggestions"] = json::array({
                "Use --connection <id> to specify which connection to disconnect",
                "Run 'fairyfly connection list' to see all connections"
            });
            return result;
        }

        connection_id = connections[0].id;
    }

    // Load the connection
    auto conn = conn_mgr_->load_connection(connection_id.value());
    if (!conn.has_value()) {
        result.status = Result::Status::Error;
        result.error["code"] = "CONNECTION_NOT_FOUND";
        result.error["message"] = fmt::format("Connection {} not found", connection_id.value());
        return result;
    }

    if (close_session) {
        auto selected = resolve_and_validate_connection(connection_id);
        if (selected.status != ResultT<Connection>::Status::Success) {
            return result_from_error(selected);
        }
        auto closed = engine_->close_current_session();
        if (closed.status != Result::Status::Success) {
            return closed;
        }
        conn = selected.value;
    }

    // A replacement cache entry must survive even if the close action completed.
    bool deleted = conn_mgr_->delete_connection_if_unchanged(*conn);

    result.status = Result::Status::Success;
    result.data["connection_id"] = connection_id.value();
    result.data["session_id"] = conn.value().session_id;
    result.data["file_deleted"] = deleted;
    result.data["session_closed"] = close_session;
    result.data["message"] = deleted
        ? (close_session
            ? fmt::format("Closed SAP GUI session and removed saved connection {}", connection_id.value())
            : fmt::format("Removed saved connection {}; SAP GUI session remains open", connection_id.value()))
        : (close_session
            ? fmt::format("Closed SAP GUI session; saved connection {} changed and was retained", connection_id.value())
            : fmt::format("Saved connection {} changed and was retained; SAP GUI session remains open", connection_id.value()));

    spdlog::info("Disconnected connection {} (saved file removed: {})", connection_id.value(), deleted);

    return result;
}

Result CommandHandler::handle_connections_list(bool cleanup)
{
    Result result;

    auto connections = conn_mgr_->list_connections();

    if (cleanup) {
        // Cleanup invalid connections
        int deleted = conn_mgr_->cleanup_invalid_connections(
            [this](const Connection& conn) {
                return engine_->validate_session_for_cleanup(conn.session_id, conn.server_session_key);
            }
        );

        result.data["cleaned_up"] = deleted;
        spdlog::info("Cleaned up {} invalid connection(s)", deleted);

        // Reload connections after cleanup
        connections = conn_mgr_->list_connections();
    }

    // Build connection list with validation status
    json conn_list = json::array();
    for (const auto& conn : connections) {
        bool valid = engine_->validate_session(conn.session_id, conn.server_session_key);

        conn_list.push_back({
            {"id", conn.id},
            {"file", conn.get_file_path()},
            {"session_id", conn.session_id},
            {"connection_id", conn.connection_id},
            {"description", conn.connection_description},
            {"connection_string", conn.connection_string},
            {"window_title", conn.window_title},
            {"created_at", conn.created_at},
            {"last_validated", conn.last_validated},
            {"server_session_key", conn.server_session_key},
            {"cache_generation", conn.cache_generation},
            {"valid", valid}
        });
    }

    result.status = Result::Status::Success;
    result.data["connections"] = conn_list;
    result.data["count"] = connections.size();

    return result;
}

Result CommandHandler::handle_transaction(const std::string& tcode, std::optional<int> connection_id)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    spdlog::info("Executing transaction: {} on connection {}", tcode, conn_result.value.id);

    auto result = engine_->execute_transaction(tcode);

    if (result.status == Result::Status::Success) {
        result.data["transaction"] = tcode;
        result.data["connection_id"] = conn_result.value.id;
    }

    return result;
}

Result CommandHandler::handle_click(const std::string& element_id, std::optional<int> connection_id,
                                     bool wait_for_window, int timeout_ms,
                                     const std::string& node_key_raw, const std::string& tree_action,
                                     const std::string& menu_item_raw, std::optional<int> row,
                                     const std::string& column, bool doubleclick)
{
    // Tool-call transports may deliver "PROG&lt;SYST&gt;" for the key "PROG<SYST>".
    const std::string node_key = utils::unescape_html_entities(node_key_raw);
    const std::string menu_item = utils::unescape_html_entities(menu_item_raw);
    if (auto invalid = sap::check_doubleclick_options(doubleclick, row, column)) return *invalid;
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    ElementId elem(element_id);
    if (!elem.is_valid()) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "INVALID_ELEMENT";
        result.error["message"] = fmt::format("Invalid element ID: {}", element_id);
        result.error["suggestions"] = json::array({
            "Element IDs should start with 'wnd' or '@active' (e.g., wnd[0]/usr/btn[3] or @active/usr/btn[3])"
        });
        return result;
    }

    // Check if this is a synthetic toolbar button (pattern: .../shell/btn_XXXX)
    std::string element_path = elem.path;
    bool is_synthetic_button = element_path.find("/shell/btn_") != std::string::npos;

    // Read-only guard. Grid row select / double-click and tree select/expand/collapse/doubleclick stay allowed.
    if (read_only_ && !row.has_value() && column.empty()) {
        if (node_key.empty()) {
            if (auto refusal = guard_element(element_id)) return *refusal;
        } else if (tree_action == "contextmenu") {
            const auto rule = sap::matched_read_only_rule("GuiMenu", menu_item, "", element_id);
            if (!rule.empty())
                return sap::make_read_only_refusal(element_id, "GuiMenu", menu_item, "", rule);
        }
    }

    // Read-only: double-click stays allowed (it opens ST22 dumps) but its target is inspected: the
    // grid/tree element plus the addressed cell/node text go through the word rules. Residual risk:
    // a double-click on a hotspot can still trigger an application action the guard cannot see.
    if (read_only_ && !is_synthetic_button &&
        ((doubleclick && row.has_value() && !column.empty()) ||
         (!node_key.empty() && tree_action == "doubleclick"))) {
        std::string type, text, tooltip, target_text;
        describe_element(element_id, type, text, tooltip);
        try {
            if (auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get())) {
                if (auto session = com_engine->get_session()) {
                    std::string full_path = elem.path;
                    if (full_path.rfind("@active", 0) == 0)
                        full_path = engine_->get_active_window_id().id + full_path.substr(7);
                    if (auto target = session->find_element_by_id(full_path))
                        target_text = node_key.empty() ? target->get_cell_value(*row, column)
                                                       : target->get_node_text_by_key(node_key);
                }
            }
        } catch (const std::exception& e) {
            spdlog::debug("Read-only guard could not read the double-click target: {}", e.what());
        }
        const auto rule = sap::read_only_doubleclick_rule(type, text, tooltip, element_id, target_text);
        if (!rule.empty()) return sap::make_read_only_refusal(element_id, type, target_text, tooltip, rule);
    }

    // Get current active window before click (if monitoring for new windows)
    WindowId window_before;
    sap::ScreenSnapshot snapshot_before;
    auto* snapshot_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
    if (wait_for_window) {
        if (snapshot_engine) snapshot_before = snapshot_engine->capture_screen_snapshot();
        window_before = engine_->get_active_window_id();
        spdlog::info("Clicking element: {} on connection {} (monitoring for new window, current={})",
                     element_id, conn_result.value.id, window_before.id);
    } else {
        spdlog::info("Clicking element: {} on connection {}", element_id, conn_result.value.id);
    }

    Result result;

    if (row.has_value() || !column.empty()) {
        if (!row.has_value() || *row < 0 || column.empty() || !node_key.empty() || is_synthetic_button) {
            result.status = Result::Status::Error;
            result.error["code"] = "GRID_ROW_OPTIONS_REQUIRED";
            result.error["message"] = "GridView selection requires --row >= 0 and --column on a grid element";
            return result;
        }
        auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
        if (!com_engine) {
            result.status = Result::Status::Error;
            result.error["code"] = "ENGINE_TYPE_MISMATCH";
            result.error["message"] = "GridView row selection requires the COM automation engine";
            return result;
        }
        result = doubleclick ? com_engine->doubleclick_grid_cell(elem, *row, column)
                             : com_engine->select_grid_row(elem, *row, column);
        if (result.status == Result::Status::Success)
            result.data["connection_id"] = conn_result.value.id;
        return result;
    }

    // Check if node_key is provided - if so, this is a tree operation
    if (!node_key.empty()) {
        if (tree_action == "contextmenu" && menu_item.empty()) {
            result.status = Result::Status::Error;
            result.error["code"] = "MENU_ITEM_REQUIRED";
            result.error["message"] = "--menu-item is required for tree context-menu actions";
            return result;
        }
        spdlog::info("Tree operation requested: action='{}', node_key='{}'", tree_action, node_key);

        try {
            auto start = std::chrono::high_resolution_clock::now();

            // Get the ComAutomationEngine to access session
            auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
            if (!com_engine) {
                result.status = Result::Status::Error;
                result.error["code"] = "ENGINE_TYPE_MISMATCH";
                result.error["message"] = "Tree operations require COM automation engine (stub mode active)";
                return result;
            }

            auto session = com_engine->get_session();
            if (!session) {
                result.status = Result::Status::Error;
                result.error["code"] = "NO_SESSION";
                result.error["message"] = "No active session";
                return result;
            }

            // Resolve element path to full path
            std::string full_path = elem.path;
            if (elem.path.rfind("@active", 0) == 0) {  // Starts with "@active"
                WindowId active_window = engine_->get_active_window_id();
                // Replace @active with actual window ID
                full_path = active_window.id + elem.path.substr(7);  // Skip "@active"
            }

            // Find element by ID
            auto tree_elem = session->find_element_by_id(full_path);
            if (!tree_elem) {
                result.status = Result::Status::Error;
                result.error["code"] = "ELEMENT_NOT_FOUND";
                result.error["message"] = fmt::format("Element not found: {}", full_path);
                return result;
            }

            // Check if element is a tree (optional - will fail gracefully if not)
            std::string elem_type = tree_elem->get_type();
            const auto before_status = sap::read_action_status(session);

            // Execute the tree action
            if (tree_action == "select") {
                tree_elem->select_node(node_key);
            } else if (tree_action == "expand") {
                tree_elem->expand_node(node_key);
            } else if (tree_action == "collapse") {
                tree_elem->collapse_node(node_key);
            } else if (tree_action == "contextmenu") {
                tree_elem->select_node_context_item(node_key, menu_item);
            } else { // doubleclick (default)
                tree_elem->doubleclick_node(node_key);
            }
            session->wait_for_completion(500);
            const auto after_status = sap::read_action_status(session);
            if (auto rejection = sap::classify_action_status(before_status,
                    after_status, full_path,
                    tree_action == "contextmenu" || tree_action == "doubleclick")) {
                sap::attach_status_bar(*rejection, before_status, after_status);
                rejection->error["node_key"] = node_key;
                rejection->error["tree_action"] = tree_action;
                rejection->duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - start);
                return *rejection;
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            result.status = Result::Status::Success;
            result.data["action"] = tree_action + "_node";
            result.data["node_key"] = node_key;
            if (tree_action == "contextmenu") result.data["menu_item"] = menu_item;
            result.data["element"] = full_path;
            result.data["element_type"] = elem_type;
            result.data["connection_id"] = conn_result.value.id;
            sap::attach_status_bar(result, before_status, after_status);
            result.duration = duration;

            spdlog::info("Executed tree action '{}' on node '{}' (duration: {}ms)",
                        tree_action, node_key, duration.count());

            return result;

        } catch (const std::exception& e) {
            result.status = Result::Status::Error;
            result.error["code"] = "TREE_ACTION_FAILED";
            result.error["message"] = fmt::format("Tree action '{}' failed: {}", tree_action, e.what());
            result.error["node_key"] = node_key;
            result.error["tree_action"] = tree_action;
            result.error["suggestions"] = json::array({
                fmt::format("List nodes: fairyfly element get {} --list-nodes", element_id),
                "Ensure element is a tree control (GuiShell or GuiTree)",
                "Verify node key exists in tree"
            });
            return result;
        }
    }

    // Handle synthetic toolbar buttons differently
    if (is_synthetic_button) {
        // Extract toolbar path and button ID from synthetic button path
        size_t btn_pos = element_path.rfind("/btn_");
        std::string toolbar_path = element_path.substr(0, btn_pos);
        std::string button_id = element_path.substr(btn_pos + 5); // Skip "/btn_"

        spdlog::debug("Detected synthetic button - toolbar: {}, button_id: {}", toolbar_path, button_id);

        // Press button using toolbar's PressButton method
        result = engine_->press_toolbar_button(ElementId(toolbar_path), button_id);
    } else {
        // Regular click for real COM elements
        result = engine_->click_element(elem);
    }

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;

        // If requested, wait until the window OR the screen content changes.
        // SAP often updates in place (same window id), so a window-only check
        // would always wait for the full timeout.
        if (wait_for_window) {
            const auto start = std::chrono::steady_clock::now();
            bool window_changed = false;
            bool screen_changed = false;
            WindowId new_window = window_before;

            while (true) {
                new_window = engine_->get_active_window_id();
                window_changed = new_window.id != window_before.id;
                if (snapshot_engine) {
                    screen_changed = sap::screen_snapshot_changed(
                        snapshot_before, snapshot_engine->capture_screen_snapshot());
                } else {
                    screen_changed = window_changed;
                }
                if (window_changed || screen_changed) break;

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                if (elapsed.count() >= timeout_ms) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            result.data["waited_ms"] = waited.count();
            result.data["window_changed"] = window_changed;
            result.data["screen_changed"] = screen_changed;
            result.duration += waited;

            if (window_changed) {
                result.data["window_before"] = window_before.id;
                result.data["window_after"] = new_window.id;
                result.data["new_window"] = new_window.id;  // For easy access
                spdlog::info("New window detected after click: {} -> {}", window_before.id, new_window.id);
            } else if (screen_changed) {
                spdlog::info("Screen changed in place after click (waited {}ms)", waited.count());
            } else {
                spdlog::debug("No screen change detected after click (waited {}ms)", waited.count());
            }
        }
    }

    return result;
}

Result CommandHandler::handle_press_f4(const std::string& element_id, std::optional<int> connection_id)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    ElementId elem(element_id);
    if (!elem.is_valid()) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "INVALID_ELEMENT";
        result.error["message"] = fmt::format("Invalid element ID: {}", element_id);
        result.error["suggestions"] = json::array({
            "Element IDs should start with 'wnd' or '@active' (e.g., wnd[0]/usr/ctxtFIELD or @active/usr/ctxtFIELD)"
        });
        return result;
    }

    spdlog::info("Pressing F4 for element: {} on connection {}", element_id, conn_result.value.id);

    // Call the automation engine's press_f4 method
    Result result = engine_->press_f4(elem);

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
        result.data["element_id"] = element_id;
        result.data["message"] = "F4 search help opened successfully";
    }

    return result;
}

Result CommandHandler::handle_fill(const std::string& element_id, const std::string& value,
                                   std::optional<int> connection_id, std::optional<int> row,
                                   const std::string& column, bool checkbox, bool commit, bool allow_fill)
{
    if (read_only_ && !allow_fill) {
        // The value is deliberately not echoed (it may be a password).
        std::string type, text, tooltip;
        describe_element(element_id, type, text, tooltip);
        return sap::make_read_only_refusal(element_id, type, text, tooltip, "fill:disabled-in-read-only");
    }
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    ElementId elem(element_id);
    if (!elem.is_valid()) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "INVALID_ELEMENT";
        result.error["message"] = fmt::format("Invalid element ID: {}", element_id);
        return result;
    }

    const bool grid_requested = row.has_value() || !column.empty() || checkbox || commit;
    Result result;
    if (grid_requested) {
        if (!row.has_value() || *row < 0 || column.empty()) {
            result.status = Result::Status::Error;
            result.error["code"] = "GRID_CELL_OPTIONS_REQUIRED";
            result.error["message"] = "GridView fill requires --row >= 0 and --column";
            return result;
        }
        auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
        if (!com_engine) {
            result.status = Result::Status::Error;
            result.error["code"] = "ENGINE_TYPE_MISMATCH";
            result.error["message"] = "GridView editing requires the COM automation engine";
            return result;
        }
        result = com_engine->fill_grid_cell(elem, *row, column, value, checkbox, commit);
    } else {
        spdlog::info("Filling element: {} on connection {}", element_id, conn_result.value.id);
        result = engine_->fill_field(elem, value);
    }

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }

    return result;
}

Result CommandHandler::handle_read_field(const std::string& element_id, std::optional<int> connection_id, bool list_nodes,
                                         bool activate_tab)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    ElementId elem(element_id);
    if (!elem.is_valid()) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "INVALID_ELEMENT";
        result.error["message"] = fmt::format("Invalid element ID: {}", element_id);
        return result;
    }

    // Handle tree node listing
    if (list_nodes) {
        spdlog::info("Listing tree nodes: {} on connection {}", element_id, conn_result.value.id);

        try {
            // Get the actual element COM object
            auto* com_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
            if (!com_engine) {
                Result result;
                result.status = Result::Status::Error;
                result.error["code"] = "ENGINE_TYPE_MISMATCH";
                result.error["message"] = "Tree operations require COM automation engine (stub mode active)";
                return result;
            }

            // Check for modal dialog blocking access to main window
            auto dialog_info = com_engine->detect_modal_dialog();
            if (!dialog_info.empty()) {
                Result result;
                result.status = Result::Status::Error;
                result.error["code"] = "MODAL_DIALOG_ACTIVE";
                result.error["message"] = "Cannot access element - modal dialog is active";
                result.error["dialog"] = dialog_info;
                result.error["suggestions"] = json::array({
                    fmt::format("Close the dialog first: fairyfly element click '@active/tbar[0]/btn[0]'"),
                    fmt::format("Or use main window explicitly: fairyfly element get '@main{}' --list-nodes", elem.path.substr(7))
                });
                result.error["requested_element"] = element_id;
                result.error["active_window"] = dialog_info["window_id"];
                result.error["main_window"] = "wnd[0]";
                spdlog::warn("Modal dialog blocking access: {} ({})",
                            dialog_info.value("window_id", "unknown"),
                            dialog_info.value("title", "Unknown"));
                return result;
            }

            auto session = com_engine->get_session();
            if (!session) {
                Result result;
                result.status = Result::Status::Error;
                result.error["code"] = "NO_SESSION";
                result.error["message"] = "No active session";
                return result;
            }

            // Resolve element path to full path
            std::string full_path = elem.path;
            if (elem.path.rfind("@active", 0) == 0) {  // Starts with "@active"
                WindowId active_window = engine_->get_active_window_id();
                // Replace @active with actual window ID
                full_path = active_window.id + elem.path.substr(7);  // Skip "@active"
            }

            // Find element by ID
            auto tree_elem = session->find_element_by_id(full_path);
            if (!tree_elem) {
                Result result;
                result.status = Result::Status::Error;
                result.error["code"] = "ELEMENT_NOT_FOUND";
                result.error["message"] = fmt::format("Element not found: {}", full_path);
                return result;
            }

            // Check if element is a tree
            std::string elem_type = tree_elem->get_type();
            if (elem_type != "GuiShell" && elem_type != "GuiTree") {
                Result result;
                result.status = Result::Status::Error;
                result.error["code"] = "NOT_A_TREE";
                result.error["message"] = fmt::format("Element is not a tree control (type: {})", elem_type);
                result.error["element"] = full_path;
                result.error["element_type"] = elem_type;
                return result;
            }

            // Get all node keys
            auto start = std::chrono::high_resolution_clock::now();
            auto node_keys = tree_elem->get_all_node_keys();

            // Build nodes array with keys and text
            json nodes = json::array();
            std::vector<std::string> tree_column_names;
            bool tree_column_names_loaded = false;
            for (const auto& key : node_keys) {
                json node;
                node["key"] = key;
                std::string node_text = tree_elem->get_node_text_by_key(key);
                if (node_text.empty() && !tree_column_names_loaded) {
                    tree_column_names = tree_elem->get_tree_column_names();
                    tree_column_names_loaded = true;
                }
                node["text"] = sap::recover_tree_node_text(
                    node_text, tree_column_names,
                    [&](const std::string& name) {
                        return tree_elem->get_item_text(key, name);
                    });
                nodes.push_back(node);
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            Result result;
            result.status = Result::Status::Success;
            result.data["element"] = full_path;
            result.data["element_type"] = elem_type;
            result.data["nodes"] = nodes;
            result.data["node_count"] = nodes.size();
            result.data["connection_id"] = conn_result.value.id;
            result.duration = duration;

            spdlog::info("Listed {} tree nodes (duration: {}ms)", nodes.size(), duration.count());

            return result;

        } catch (const std::exception& e) {
            Result result;
            result.status = Result::Status::Error;
            result.error["code"] = "TREE_LIST_FAILED";
            result.error["message"] = fmt::format("Failed to list tree nodes: {}", e.what());
            return result;
        }
    }

    // Regular field reading
    spdlog::info("Reading field: {} on connection {}", element_id, conn_result.value.id);
    auto* com_read_engine = dynamic_cast<sap::ComAutomationEngine*>(engine_.get());
    auto result = (activate_tab && com_read_engine) ? com_read_engine->read_field(elem, true)
                                                    : engine_->read_field(elem);

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }

    return result;
}

static std::string lowercase_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

/// True when any string value inside `value` contains the (lowercased) needle.
static bool json_strings_contain(const json& value, const std::string& needle, int depth = 0)
{
    if (depth > 64) return false;
    if (value.is_string()) {
        return lowercase_ascii(value.get<std::string>()).find(needle) != std::string::npos;
    }
    if (value.is_array() || value.is_object()) {
        for (const auto& item : value) {
            if (json_strings_contain(item, needle, depth + 1)) return true;
        }
    }
    return false;
}

/// The element's own text (labels, tooltip, text content), not its table or tree data.
static bool element_own_text_matches(const json& elem, const std::string& needle)
{
    for (const char* key : {"text", "tooltip", "label", "text_content"}) {
        if (elem.contains(key) && json_strings_contain(elem[key], needle)) return true;
    }
    return false;
}

/// Grid / table-control data object that carries a `rows` array, or nullptr.
static json* table_rows_owner(json& elem)
{
    for (const char* key : {"table_data", "grid_data"}) {
        if (elem.contains(key) && elem[key].is_object() && elem[key].contains("rows") &&
            elem[key]["rows"].is_array())
            return &elem[key];
    }
    return nullptr;
}

static size_t count_matching_rows(const json& rows, const std::string& needle)
{
    size_t matched = 0;
    for (const auto& row : rows) {
        if (json_strings_contain(row, needle)) ++matched;
    }
    return matched;
}

/// Text search across labels, tooltips, table/grid cells, tree nodes and text content (IMP-005).
static bool element_text_matches(const json& elem, const std::string& needle)
{
    if (element_own_text_matches(elem, needle)) return true;
    for (const char* key : {"table_data", "grid_data", "tree_data", "tree_nodes"}) {
        if (elem.contains(key) && json_strings_contain(elem[key], needle)) return true;
    }
    return false;
}

/// `--text-contains` on a grid or table control keeps only the rows with a matching cell and reports
/// `rows_matched` / `rows_total` (rows read, before filtering). When the element matched through its
/// own text or a column title instead of a cell, its rows stay untouched.
static void narrow_table_rows(json& elem, const std::string& needle)
{
    if (!elem.is_object() || element_own_text_matches(elem, needle)) return;
    json* table = table_rows_owner(elem);
    if (!table) return;
    const json& rows = (*table)["rows"];
    if (count_matching_rows(rows, needle) == 0) return;
    json kept = json::array();
    for (const auto& row : rows) {
        if (json_strings_contain(row, needle)) kept.push_back(row);
    }
    (*table)["rows_total"] = rows.size();
    (*table)["rows_matched"] = kept.size();
    (*table)["rows"] = std::move(kept);
}

/// Grid, table control or positioned-label table (anything rendered as rows and columns).
static bool element_is_table(const json& elem)
{
    if (!elem.is_object()) return false;
    const std::string type = elem.value("type", "");
    if (type == "GuiGridView" || type == "GuiTableControl") return true;
    if (elem.value("subtype", "") == "GridView") return true;
    for (const char* key : {"table_data", "grid_data"}) {
        if (elem.contains(key) && elem[key].is_object()) return true;
    }
    return false;
}

static bool element_is_tree(const json& elem)
{
    if (!elem.is_object()) return false;
    const std::string subtype = elem.value("subtype", "");
    return elem.value("type", "") == "GuiTree" || subtype == "Tree" || subtype == "TableTreeControl" ||
           elem.contains("tree_nodes") || elem.contains("tree_data");
}

/// Helper to check if an element matches filter criteria
static bool element_matches_filter(const json& elem, const ScreenFilterOptions& filters)
{
    if (!elem.is_object()) {
        return false;
    }

    bool matches = true;

    // Type filters
    std::string type = elem.value("type", "");

    if (filters.only_buttons) {
        matches = matches && (type == "GuiButton");
    }

    if (filters.only_fields) {
        matches = matches && (type == "GuiTextField" || type == "GuiCTextField" ||
                             type == "GuiPasswordField") && elem.value("changeable", false);
    }

    if (filters.only_editable) {
        matches = matches && elem.value("changeable", false);
    }

    if (filters.only_tables) {
        matches = matches && element_is_table(elem);
    }

    if (filters.only_f4_fields) {
        matches = matches && elem.value("has_f4_help", false);
    }

    // Text search (case-insensitive)
    if (filters.text_contains.has_value() && matches) {
        matches = element_text_matches(elem, lowercase_ascii(filters.text_contains.value()));
    }

    // ID search
    if (filters.id_contains.has_value() && matches) {
        std::string id = elem.value("id", "");
        matches = id.find(filters.id_contains.value()) != std::string::npos;
    }

    // Exact type filter
    if (filters.type_filter.has_value() && matches) {
        matches = (type == filters.type_filter.value());
    }

    return matches;
}

/// Recursively filter elements based on ScreenFilterOptions
/// Returns flat array of all matching elements (including nested children)
static std::pair<json, json> filter_elements(const json& elements, const ScreenFilterOptions& filters)
{
    json filter_stats;

    // If no filters active, return all elements
    bool any_filter = filters.only_buttons || filters.only_fields || filters.only_editable ||
                      filters.only_f4_fields || filters.only_tables || filters.text_contains.has_value() ||
                      filters.id_contains.has_value() || filters.type_filter.has_value() ||
                      filters.first_match_only;

    if (!any_filter) {
        filter_stats["filter_applied"] = false;
        return {elements, filter_stats};
    }

    json filtered = json::array();
    int total_checked = 0;
    bool found_first = false;

    // Recursive lambda to collect all matching elements
    std::function<void(const json&)> collect_matches = [&](const json& elem_array) {
        if (!elem_array.is_array()) return;

        for (const auto& elem : elem_array) {
            // Child arrays may mix ID references with fully expanded objects.
            if (!elem.is_object()) continue;
            total_checked++;

            // Check if this element matches
            if (element_matches_filter(elem, filters)) {
                filtered.push_back(elem);
                if (filters.text_contains.has_value())
                    narrow_table_rows(filtered.back(), lowercase_ascii(filters.text_contains.value()));
                found_first = true;

                // If first_match_only, stop immediately
                if (filters.first_match_only) {
                    return;
                }
            }

            // Recursively check all child objects, even after string ID references.
            if (elem.is_object() && elem.contains("children") && elem["children"].is_array() &&
                (!filters.first_match_only || !found_first)) {
                collect_matches(elem["children"]);
                if (filters.first_match_only && found_first) {
                    return;
                }
            }

            // Also check toolbar_buttons array (synthetic buttons)
            if (elem.is_object() && elem.contains("toolbar_buttons") &&
                elem["toolbar_buttons"].is_array() && (!filters.first_match_only || !found_first)) {
                collect_matches(elem["toolbar_buttons"]);
                if (filters.first_match_only && found_first) {
                    return;
                }
            }
            if (filters.first_match_only && found_first) return;
        }
    };

    collect_matches(elements);

    // Build filter stats
    filter_stats["filter_applied"] = true;
    filter_stats["total_elements_checked"] = total_checked;
    filter_stats["filtered_element_count"] = static_cast<int>(filtered.size());

    return {filtered, filter_stats};
}

/// Name of the `only` selector in effect (MCP argument spelling), empty when none.
static std::string only_selector_name(const ScreenFilterOptions& filters)
{
    if (filters.only_buttons) return "buttons";
    if (filters.only_fields) return "fields";
    if (filters.only_editable) return "editable";
    if (filters.only_f4_fields) return "f4_fields";
    if (filters.only_tables) return "tables";
    return "";
}

/// Grids/trees that an `only` selector dropped: they would otherwise vanish without a trace.
static void collect_suppressed(const json& elements, const std::unordered_set<std::string>& kept,
                               json& suppressed, int depth = 0)
{
    if (!elements.is_array() || depth > 64) return;
    for (const auto& elem : elements) {
        if (!elem.is_object()) continue;
        const std::string id = elem.value("id", "");
        if (kept.count(id) == 0) {
            const char* ids_key = element_is_table(elem) ? "grid_ids" : element_is_tree(elem) ? "tree_ids" : nullptr;
            if (ids_key) {
                auto& ids = suppressed[ids_key];
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            }
        }
        if (elem.contains("children")) collect_suppressed(elem["children"], kept, suppressed, depth + 1);
        if (elem.contains("toolbar_buttons"))
            collect_suppressed(elem["toolbar_buttons"], kept, suppressed, depth + 1);
    }
}

bool screen_filters_need_grid_rows(const ScreenFilterOptions& filters)
{
    return !(filters.only_buttons || filters.only_fields || filters.only_editable || filters.only_f4_fields);
}

static void apply_screen_filters_impl(json& screen_data, const ScreenFilterOptions& filters);

/// Display name of a tab: its caption, else the id's last segment without the "tabp" prefix.
static std::string tab_display_name(const std::string& id, const std::string& text)
{
    if (!text.empty()) return text;
    const auto slash = id.rfind('/');
    std::string last = slash == std::string::npos ? id : id.substr(slash + 1);
    if (last.rfind("tabp", 0) == 0 && last.size() > 4) last = last.substr(4);
    return last;
}

static void collect_tab_headers(const json& node, std::map<std::string, std::string>& tabs, int depth = 0)
{
    if (depth > 64) return;
    if (node.is_array()) {
        for (const auto& item : node) collect_tab_headers(item, tabs, depth + 1);
        return;
    }
    if (!node.is_object()) return;
    if (node.value("type", "") == "GuiTab") {
        const std::string id = node.value("id", "");
        if (!id.empty()) {
            auto& text = tabs[id];
            if (text.empty()) text = node.value("text", "");
        }
    }
    for (const char* key : {"children", "toolbar_buttons"}) {
        if (node.contains(key)) collect_tab_headers(node[key], tabs, depth + 1);
    }
}

json describe_tab_coverage(const json& screen_data)
{
    std::map<std::string, std::string> headers;  // id -> caption, ordered by id (= strip order for tabp ids)
    std::vector<std::string> order;
    const auto remember_order = [&](const json& list) {
        if (!list.is_array()) return;
        for (const auto& item : list)
            if (item.is_object() && item.value("type", "") == "GuiTab") {
                const std::string id = item.value("id", "");
                if (!id.empty() && std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
            }
    };
    if (screen_data.contains("elements")) collect_tab_headers(screen_data["elements"], headers);
    if (screen_data.contains("hierarchy")) {
        const auto& hierarchy = screen_data["hierarchy"];
        collect_tab_headers(hierarchy, headers);
        if (hierarchy.is_object())
            for (const auto& [category, list] : hierarchy.items()) remember_order(list);
    }
    if (screen_data.contains("elements")) remember_order(screen_data["elements"]);
    // Strips list their pages by id only in some reads.
    if (screen_data.contains("hierarchy") && screen_data["hierarchy"].is_object() &&
        screen_data["hierarchy"].contains("tabs") && screen_data["hierarchy"]["tabs"].is_array()) {
        for (const auto& entry : screen_data["hierarchy"]["tabs"]) {
            if (!entry.is_object() || entry.value("type", "") != "GuiTabStrip" || !entry.contains("children") ||
                !entry["children"].is_array())
                continue;
            for (const auto& child : entry["children"])
                if (child.is_string()) {
                    headers.emplace(child.get<std::string>(), "");
                    if (std::find(order.begin(), order.end(), child.get<std::string>()) == order.end())
                        order.push_back(child.get<std::string>());
                }
        }
    }
    json searched = json::array();
    std::set<std::string> handled;
    if (screen_data.contains("tabs_content") && screen_data["tabs_content"].is_array()) {
        for (const auto& tab : screen_data["tabs_content"]) {
            if (!tab.is_object()) continue;
            const std::string id = tab.value("tab_id", "");
            searched.push_back(tab_display_name(id, tab.value("tab_name", "")));
            handled.insert(id);
            headers.emplace(id, tab.value("tab_name", ""));
            if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
        }
    }
    json skipped = json::array();
    if (screen_data.contains("tabs_failed") && screen_data["tabs_failed"].is_array()) {
        for (const auto& tab : screen_data["tabs_failed"]) {
            if (!tab.is_object()) continue;
            const std::string id = tab.value("tab_id", "");
            skipped.push_back({{"tab_id", id}, {"tab_name", tab_display_name(id, tab.value("tab_name", ""))},
                               {"reason", tab.value("reason", "error")}});
            handled.insert(id);
        }
    }
    const bool expanded = screen_data.value("tabs_expanded", false);
    // With --tab only the requested tab is read; without it nothing was expanded (--no-tabs).
    const bool single_tab = expanded && screen_data.contains("tabs_content") && screen_data["tabs_content"].is_array() &&
                            screen_data["tabs_content"].size() < order.size();
    for (const auto& id : order) {
        if (handled.count(id)) continue;
        skipped.push_back({{"tab_id", id}, {"tab_name", tab_display_name(id, headers[id])},
                           {"reason", expanded && single_tab ? "not_requested" : "not_expanded"}});
    }
    if (order.empty() && searched.empty() && skipped.empty()) return json::object();
    return {{"searched", searched}, {"skipped", skipped}};
}

static std::string join_names(const json& names)
{
    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) out += ", ";
        out += name.get<std::string>();
    }
    return out;
}

void annotate_text_filter_coverage(json& screen_data, const json& coverage, size_t matches)
{
    if (!coverage.is_object() || coverage.empty()) return;
    const json& searched = coverage["searched"];
    const json& skipped = coverage["skipped"];
    screen_data["tabs_searched"] = searched;
    if (!skipped.empty()) screen_data["tabs_skipped"] = skipped;

    json not_expanded = json::array();
    json failed = json::array();
    for (const auto& tab : skipped) {
        const std::string reason = tab.value("reason", "");
        (reason == "not_expanded" || reason == "not_requested" ? not_expanded : failed)
            .push_back(tab.value("tab_name", ""));
    }
    if (matches > 0 && skipped.empty()) return;
    std::string note;
    if (matches == 0)
        note = searched.empty() ? "0 matches on the visible screen" : "0 matches in tabs [" + join_names(searched) + "]";
    else
        note = searched.empty() ? "Searched the visible screen" : "Searched tabs [" + join_names(searched) + "]";
    if (!not_expanded.empty()) note += "; tabs not expanded: [" + join_names(not_expanded) + "] (use tab=...)";
    if (!failed.empty()) note += "; tabs that could not be read: [" + join_names(failed) + "]";
    screen_data["text_filter_note"] = note;
}

void apply_screen_filters(json& screen_data, const ScreenFilterOptions& filters)
{
    const json coverage = filters.text_contains.has_value() ? describe_tab_coverage(screen_data) : json::object();
    apply_screen_filters_impl(screen_data, filters);
    if (coverage.empty()) return;
    size_t matches = screen_data.value("element_count", static_cast<size_t>(0));
    if (screen_data.contains("tabs_content") && screen_data["tabs_content"].is_array())
        for (const auto& tab : screen_data["tabs_content"])
            if (tab.is_object()) matches += tab.value("element_count", static_cast<size_t>(0));
    annotate_text_filter_coverage(screen_data, coverage, matches);
}

static void apply_screen_filters_impl(json& screen_data, const ScreenFilterOptions& filters)
{
    json all_elements = json::array();
    if (screen_data.contains("elements") && screen_data["elements"].is_array()) {
        all_elements = screen_data["elements"];
    } else if (screen_data.contains("hierarchy")) {
        const json& hierarchy = screen_data["hierarchy"];
        if (hierarchy.is_array()) {
            all_elements = hierarchy;
        } else if (hierarchy.is_object()) {
            for (const auto& [category, elements] : hierarchy.items()) {
                if (!elements.is_array()) continue;
                for (const auto& element : elements) all_elements.push_back(element);
            }
        }
    }

    auto [filtered, filter_stats] = filter_elements(all_elements, filters);
    if (!filter_stats.value("filter_applied", false)) return;

    const std::string text_needle = filters.text_contains.has_value()
        ? lowercase_ascii(filters.text_contains.value()) : std::string();
    json categorized = json::object();
    std::unordered_set<std::string> wanted_ids;
    std::unordered_set<std::string> assigned_ids;
    for (const auto& element : filtered) {
        if (element.is_object()) wanted_ids.insert(element.value("id", ""));
    }
    if (screen_data.contains("hierarchy") && screen_data["hierarchy"].is_object()) {
        for (const auto& [category, elements] : screen_data["hierarchy"].items()) {
            if (!elements.is_array()) continue;
            for (const auto& element : elements) {
                if (!element.is_object()) continue;
                const std::string id = element.value("id", "");
                if (wanted_ids.count(id) && assigned_ids.insert(id).second) {
                    categorized[category].push_back(element);
                    if (!text_needle.empty()) narrow_table_rows(categorized[category].back(), text_needle);
                }
            }
        }
    }
    // Some synthetic or nested elements have no top-level hierarchy category.
    for (const auto& element : filtered) {
        if (!element.is_object()) continue;
        const std::string id = element.value("id", "");
        if (!assigned_ids.insert(id).second) continue;
        const std::string type = element.value("type", "");
        const char* category = type == "GuiButton" ? "buttons" :
                               type == "GuiTextField" || type == "GuiCTextField" ||
                               type == "GuiPasswordField" || type == "GuiCheckBox" ||
                               type == "GuiComboBox" || type == "GuiRadioButton" ? "form_fields" :
                               type == "GuiTableControl" || type == "GuiGridView" ? "tables" :
                               type == "GuiToolbar" ? "toolbar" : "other";
        categorized[category].push_back(element);
        if (!text_needle.empty()) narrow_table_rows(categorized[category].back(), text_needle);
    }
    screen_data["elements"] = filtered;
    screen_data["element_count"] = filtered.size();
    screen_data["hierarchy"] = categorized;
    screen_data["filter_stats"] = filter_stats;

    const std::string only_name = only_selector_name(filters);
    json suppressed = {{"by", only_name}, {"grid_ids", json::array()}, {"tree_ids", json::array()}};
    if (!only_name.empty()) collect_suppressed(all_elements, wanted_ids, suppressed);

    if (screen_data.contains("tabs_content") && screen_data["tabs_content"].is_array()) {
        for (auto& tab_data : screen_data["tabs_content"]) {
            if (!tab_data.is_object()) continue;
            apply_screen_filters_impl(tab_data, filters);
            if (!tab_data.contains("suppressed")) continue;
            for (const char* key : {"grid_ids", "tree_ids"}) {
                for (const auto& id : tab_data["suppressed"].value(key, json::array())) {
                    auto& ids = suppressed[key];
                    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
                }
            }
        }
    }
    if (!suppressed["grid_ids"].empty() || !suppressed["tree_ids"].empty()) {
        suppressed["grids"] = suppressed["grid_ids"].size();
        suppressed["trees"] = suppressed["tree_ids"].size();
        std::string note;
        const auto describe = [&](const char* key, const char* noun) {
            const size_t count = suppressed[key].size();
            if (count == 0) return;
            if (!note.empty()) note += "; ";
            note += std::to_string(count) + " " + noun + "(s)";
        };
        describe("grid_ids", "grid");
        describe("tree_ids", "tree");
        note += " not included with only=" + only_name +
                (suppressed["grid_ids"].empty() ? "; use no filter" : "; use only=tables or no filter");
        suppressed["note"] = note;
        screen_data["suppressed"] = suppressed;
    }
}

Result CommandHandler::handle_screen_read(bool include_children, std::optional<int> connection_id,
                                           bool expand_tabs, const ScreenFilterOptions& filters,
                                           bool skip_trees, bool compact, int max_rows,
                                           const std::string& only_tab, bool probe_all, int row_offset)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    spdlog::info("Reading screen structure (include_children={}, expand_tabs={}, skip_trees={}) on connection {}",
                 include_children, expand_tabs, skip_trees, conn_result.value.id);

    engine_->set_probe_all(probe_all);
    engine_->set_row_offset(row_offset);
    engine_->set_grid_rows_needed(screen_filters_need_grid_rows(filters));
    Result result;
    if (expand_tabs) {
        result = engine_->read_screen_with_tabs(skip_trees, max_rows, only_tab);
    } else {
        result = engine_->read_screen(include_children, skip_trees, max_rows);
    }

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
        result.data["compact"] = compact;

        apply_screen_filters(result.data, filters);
        if (result.data.contains("filter_stats")) {
            const auto& stats = result.data["filter_stats"];
            spdlog::info("Applied filters: {} checked -> {} elements found",
                         stats.value("total_elements_checked", 0),
                         stats.value("filtered_element_count", 0));
        }
    }

    return result;
}

Result CommandHandler::handle_screen_find(const sap::ScreenFindOptions& query,
                                          std::optional<int> connection_id)
{
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }
    auto result = engine_->find_screen(query);
    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }
    return result;
}

Result CommandHandler::handle_screenshot(std::optional<int> connection_id, const ScreenshotOptions& options)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    spdlog::info("Capturing screenshot on connection {}", conn_result.value.id);
    auto result = engine_->capture_screenshot(options);

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }

    return result;
}

Result CommandHandler::handle_list_all()
{
    spdlog::info("Listing all SAP GUI connections, sessions, and windows");
    
    Result result;
    result.status = Result::Status::Success;
    
    try {
        // Get application info
        json info = engine_->get_application_info();
        result.data = info;
        if (info.contains("error")) {
            result.status = Result::Status::Error;
            result.error["code"] = "ENUMERATION_FAILED";
            result.error["message"] = info["error"];
            return result;
        }
        
        // Log summary
        int total_conns = info["total_connections"].get<int>();
        int total_sess = info["total_sessions"].get<int>();
        spdlog::info("Found {} connection(s) with {} total session(s)", total_conns, total_sess);
        
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "ENUMERATION_FAILED";
        result.error["message"] = e.what();
        spdlog::error("Failed to enumerate SAP GUI: {}", e.what());
    }
    
    return result;
}

Result CommandHandler::handle_doctor()
{
    spdlog::info("Running preflight environment and scripting diagnostics");

    Result result;
    result.status = Result::Status::Success;

    json checks = json::array();
    bool all_passed = true;
    bool has_warnings = false;

    auto add_check = [&](const std::string& name, const std::string& status,
                         const std::string& message, const std::string& details = "",
                         const std::string& remediation = "") {
        json c;
        c["name"] = name;
        c["status"] = status;
        c["message"] = message;
        if (!details.empty()) c["details"] = details;
        if (!remediation.empty()) c["remediation"] = remediation;
        checks.push_back(c);

        if (status == "fail") all_passed = false;
        if (status == "warn") has_warnings = true;
    };

#ifdef _WIN32
    // 1. Desktop & Window Station Context
    {
        char station_name[256] = {0};
        char desktop_name[256] = {0};
        DWORD len = 0;

        HWINSTA hwinsta = GetProcessWindowStation();
        if (hwinsta && GetUserObjectInformationA(hwinsta, UOI_NAME, station_name, sizeof(station_name), &len)) {
            // station retrieved
        } else {
            strncpy_s(station_name, sizeof(station_name), "Unknown", _TRUNCATE);
        }

        HDESK hdesk = GetThreadDesktop(GetCurrentThreadId());
        if (hdesk && GetUserObjectInformationA(hdesk, UOI_NAME, desktop_name, sizeof(desktop_name), &len)) {
            // desktop retrieved
        } else {
            strncpy_s(desktop_name, sizeof(desktop_name), "Unknown", _TRUNCATE);
        }

        std::string context_str = fmt::format("{}\\{}", station_name, desktop_name);
        if (_stricmp(station_name, "WinSta0") == 0 && _stricmp(desktop_name, "Default") == 0) {
            add_check("desktop_context", "pass", "Running on interactive desktop (" + context_str + ")");
        } else {
            add_check("desktop_context", "warn",
                      "Running on non-default desktop: " + context_str,
                      "SAP GUI COM scripting requires access to the interactive desktop.",
                      "Ensure the process is running in the active user console session (WinSta0\\Default).");
        }
    }

    // 2. SAP GUI Processes
    {
        bool found_logon = false;
        bool found_gui = false;
        bool found_pad = false;
        std::vector<DWORD> pids;
        DWORD toolhelp_error = ERROR_SUCCESS;
        DWORD fallback_error = ERROR_SUCCESS;
        size_t toolhelp_count = 0;
        size_t fallback_count = 0;
        bool fallback_attempted = false;

        auto record_process = [&](const wchar_t* name, DWORD pid) {
            bool matched = false;
            if (_wcsicmp(name, L"saplogon.exe") == 0) {
                found_logon = true;
                matched = true;
            } else if (_wcsicmp(name, L"sapgui.exe") == 0) {
                found_gui = true;
                matched = true;
            } else if (_wcsicmp(name, L"saplgpad.exe") == 0) {
                found_pad = true;
                matched = true;
            }
            if (matched && std::find(pids.begin(), pids.end(), pid) == pids.end()) {
                pids.push_back(pid);
            }
        };

        HANDLE snapshot = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 3; ++attempt) {
            snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot != INVALID_HANDLE_VALUE) break;
            toolhelp_error = GetLastError();
            if (toolhelp_error != ERROR_BAD_LENGTH) break;
        }
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snapshot, &pe)) {
                do {
                    ++toolhelp_count;
                    record_process(pe.szExeFile, pe.th32ProcessID);
                } while (Process32NextW(snapshot, &pe));
                DWORD walk_error = GetLastError();
                if (walk_error != ERROR_NO_MORE_FILES) toolhelp_error = walk_error;
            } else {
                toolhelp_error = GetLastError();
            }
            CloseHandle(snapshot);
        }

        // Toolhelp may expose only a restricted process snapshot. Fall back to PSAPI.
        if (pids.empty()) {
            fallback_attempted = true;
            std::vector<DWORD> process_ids(1024);
            DWORD bytes_returned = 0;
            bool enumerated = false;
            while (process_ids.size() <= 16384) {
                if (!K32EnumProcesses(process_ids.data(),
                                      static_cast<DWORD>(process_ids.size() * sizeof(DWORD)),
                                      &bytes_returned)) {
                    fallback_error = GetLastError();
                    break;
                }
                if (bytes_returned < process_ids.size() * sizeof(DWORD)) {
                    enumerated = true;
                    break;
                }
                process_ids.resize(process_ids.size() * 2);
            }
            if (enumerated) {
                fallback_count = bytes_returned / sizeof(DWORD);
                for (size_t i = 0; i < fallback_count; ++i) {
                    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_ids[i]);
                    if (!process) continue;
                    wchar_t image_path[MAX_PATH] = {};
                    DWORD path_length = MAX_PATH;
                    if (QueryFullProcessImageNameW(process, 0, image_path, &path_length)) {
                        const wchar_t* filename = wcsrchr(image_path, L'\\');
                        record_process(filename ? filename + 1 : image_path, process_ids[i]);
                    }
                    CloseHandle(process);
                }
            }
        }

        if (!pids.empty()) {
            std::string proc_desc;
            if (found_logon && found_gui) proc_desc = "saplogon.exe and sapgui.exe";
            else if (found_logon) proc_desc = "saplogon.exe";
            else if (found_gui) proc_desc = "sapgui.exe";
            else proc_desc = "saplgpad.exe";

            add_check("sapgui_processes", "pass",
                      fmt::format("SAP GUI process is running ({})", proc_desc),
                      fmt::format("{} process(es) detected; Toolhelp saw {} entries; {}",
                                  pids.size(), toolhelp_count,
                                  fallback_attempted
                                      ? fmt::format("PSAPI saw {} entries", fallback_count)
                                      : "PSAPI not run (Toolhelp found SAP GUI)"));
        } else {
            spdlog::debug("SAP process scan: Toolhelp saw {} entries (error {}), PSAPI saw {} (error {})",
                          toolhelp_count, toolhelp_error, fallback_count, fallback_error);
            add_check("sapgui_processes", "warn",
                      "SAP GUI process is not visible to this process",
                      fmt::format("Toolhelp saw {} processes (error {}); PSAPI saw {} (error {}).",
                                  toolhelp_count, toolhelp_error, fallback_count, fallback_error),
                      "Check SAP Logon manually. If it is running, process enumeration may be restricted.");
        }
    }

    // 3. Client Registry Security Settings
    {
        HKEY hKey;
        LPCWSTR subkey = L"Software\\SAP\\SAPGUI Front\\SAP Frontend Server\\Security";
        LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &hKey);
        if (status == ERROR_SUCCESS) {
            DWORD user_scripting = 1;
            DWORD warn_attach = 0;
            DWORD warn_conn = 0;
            DWORD size = sizeof(DWORD);
            DWORD type = REG_DWORD;

            bool has_user_scripting = (RegQueryValueExW(hKey, L"UserScripting", nullptr, &type, (LPBYTE)&user_scripting, &size) == ERROR_SUCCESS);
            size = sizeof(DWORD);
            bool has_warn_attach = (RegQueryValueExW(hKey, L"WarnOnAttach", nullptr, &type, (LPBYTE)&warn_attach, &size) == ERROR_SUCCESS);
            size = sizeof(DWORD);
            bool has_warn_conn = (RegQueryValueExW(hKey, L"WarnOnConnection", nullptr, &type, (LPBYTE)&warn_conn, &size) == ERROR_SUCCESS);

            RegCloseKey(hKey);

            if (has_user_scripting && user_scripting == 0) {
                add_check("client_registry", "fail",
                          "Client scripting is disabled in SAP GUI Security settings",
                          "HKCU\\Software\\SAP\\SAPGUI Front\\SAP Frontend Server\\Security\\UserScripting is 0",
                          "Open SAP GUI Options -> Accessibility & Scripting -> Scripting -> Check 'Enable scripting'.");
            } else if ((has_warn_attach && warn_attach != 0) || (has_warn_conn && warn_conn != 0)) {
                add_check("client_registry", "warn",
                          "Client scripting is enabled, but modal security warnings are active",
                          fmt::format("WarnOnAttach={}, WarnOnConnection={}", warn_attach, warn_conn),
                          "Uncheck 'Notify when a script attaches to SAP GUI' and 'Notify when a script opens a connection' in SAP GUI Options to avoid blocking modal dialogs.");
            } else {
                add_check("client_registry", "pass",
                          "Client scripting security configuration is optimal (enabled, modal warnings suppressed)");
            }
        } else {
            add_check("client_registry", "pass",
                      "SAP GUI client security registry key using standard defaults");
        }
    }
#else
    add_check("desktop_context", "info", "Non-Windows environment, desktop check skipped");
    add_check("sapgui_processes", "info", "Non-Windows environment, process check skipped");
    add_check("client_registry", "info", "Non-Windows environment, registry check skipped");
#endif

    // 4. COM Automation Engine & Scripting Engine
    bool com_ok = false;
    try {
        if (engine_) {
            com_ok = true;
            add_check("com_engine", "pass", "SAP GUI COM scripting engine initialized successfully");
        } else {
            add_check("com_engine", "fail",
                      "Failed to initialize SAP GUI COM scripting engine",
                      "AutomationEngine returned null",
                      "Verify SAP GUI Scripting component is installed via SAP GUI setup.");
        }
    } catch (const std::exception& e) {
        add_check("com_engine", "fail",
                  fmt::format("COM scripting engine error: {}", e.what()),
                  "",
                  "Ensure SAP GUI is installed with Scripting Support enabled.");
    }

    // 5. Active Connections and Backend Scripting Health
    if (com_ok) {
        try {
            json info = engine_->get_application_info();
            int total_conns = info.value("total_connections", 0);
            int total_sess = info.value("total_sessions", 0);

            if (info.contains("error")) {
                add_check("active_sessions", "fail",
                          "Could not enumerate SAP GUI sessions",
                          info["error"].get<std::string>(),
                          "Check SAP Logon and retry the diagnostic.");
            } else if (info.value("backend_scripting_disabled", false) && total_sess == 0) {
                add_check("active_sessions", "fail",
                          "Backend scripting is disabled on an SAP connection",
                          fmt::format("{} connection(s), {} accessible session(s)", total_conns, total_sess),
                          "Enable sapgui/user_scripting on the backend and reconnect.");
            } else if (total_conns == 0) {
                add_check("active_sessions", "warn",
                          "No active SAP connections found",
                          "SAP GUI scripting engine is active, but no connections are currently open.",
                          "Connect to an SAP system in SAP Logon (e.g. Bigfox / A4H) or run 'fairyfly session launch <connection>'.");
            } else if (info.value("connection_enumeration_errors", 0) > 0) {
                add_check("active_sessions", "warn",
                          "Some SAP connections could not be inspected",
                          fmt::format("{} accessible session(s), {} connection error(s)",
                                      total_sess, info.value("connection_enumeration_errors", 0)),
                          "Inspect the connection error entries from 'fairyfly session list'.");
            } else if (info.value("session_enumeration_errors", 0) > 0) {
                add_check("active_sessions", "warn",
                          "Some SAP sessions could not be inspected",
                          fmt::format("{} accessible session(s), {} enumeration error(s)",
                                      total_sess, info.value("session_enumeration_errors", 0)),
                          "Inspect the session_errors entries from 'fairyfly session list'.");
            } else if (info.value("backend_scripting_disabled", false)) {
                add_check("active_sessions", "warn",
                          "An older SAP connection still has backend scripting disabled",
                          fmt::format("{} connection(s), {} accessible session(s)", total_conns, total_sess),
                          "Reconnect or close the disabled connection; accessible sessions can still be used.");
            } else if (total_sess == 0) {
                add_check("active_sessions", "warn",
                          "SAP connections are open, but no sessions are accessible",
                          fmt::format("{} connection(s), zero accessible sessions", total_conns),
                          "Finish SAP login and verify backend scripting is enabled.");
            } else {
                add_check("active_sessions", "pass",
                          fmt::format("Active session verified with backend scripting enabled ({} connections, {} sessions)",
                                      total_conns, total_sess));
            }
        } catch (const std::exception& e) {
            std::string err = e.what();
            if (err.find("disabled") != std::string::npos || err.find("Scripting") != std::string::npos) {
                add_check("active_sessions", "fail",
                          "Backend scripting is disabled by server administrator",
                          err,
                          "Enable sapgui/user_scripting = TRUE in transaction RZ11.");
            } else {
                add_check("active_sessions", "warn",
                          fmt::format("Could not query active sessions: {}", err));
            }
        }
    }

    std::string overall = "ok";
    if (!all_passed) {
        overall = "error";
    } else if (has_warnings) {
        overall = "warning";
    }

    result.data["overall_health"] = overall;
    result.data["checks"] = checks;

    return result;
}

// format_screen_markdown and all helper functions moved to ScreenMarkdownFormatter class
// Grid layout functions moved to GridAnalyzer and GridRenderer classes

// `screen read --compact` with json/toon output: replace duplicated hierarchy objects by ids.
static void apply_compact_screen_output(json& output)
{
    if (!output.is_object() || output.value("status", "") != "success") return;
    auto it = output.find("data");
    if (it == output.end() || !it->is_object()) return;
    if (it->contains("screen_id") && it->contains("hierarchy") && it->value("compact", false) == true) {
        *it = formatters::compact_screen_json(*it);
    }
}

std::string format_output(const Result& result, OutputFormat format, bool verbose_errors)
{
    switch (format) {
        case OutputFormat::Json: {
            json output = result.to_json();
            apply_compact_screen_output(output);

            // In compact error mode, remove suggestions array
            if (!verbose_errors && output.contains("error") && output["error"].contains("suggestions")) {
                output["error"].erase("suggestions");
            }

            return output.dump(2);
        }
        case OutputFormat::Markdown: {
            std::ostringstream oss;
            const auto& response = result.to_json();

            if (response["status"] == "success" && response.contains("data") &&
                response["data"].contains("scanned_count") &&
                response["data"].contains("elements") &&
                response["data"]["elements"].is_array()) {
                const auto& data = response["data"];
                const auto escape = [](const std::string& value) {
                    std::string safe;
                    for (char ch : value) {
                        switch (ch) {
                            case '&': safe += "&amp;"; break;
                            case '<': safe += "&lt;"; break;
                            case '>': safe += "&gt;"; break;
                            case '\n': case '\r': safe += ' '; break;
                            case '\\': case '`': case '*': case '_': case '[':
                            case ']': case '|': safe += '\\'; safe += ch; break;
                            default: safe += ch; break;
                        }
                    }
                    return safe;
                };
                oss << "# Screen matches\n\n";
                oss << "**Screen:** " << escape(data.value("title", "")) << "\n\n";
                oss << "**Scanned:** " << data.value("scanned_count", 0)
                    << " controls\n\n";
                if (data.value("scan_limit_reached", false))
                    oss << "_Scan stopped at the 500-control limit._\n\n";
                if (data["elements"].empty()) oss << "_No matches._\n";
                for (const auto& match : data["elements"]) {
                    if (!match.is_object()) continue;
                    oss << "- **" << escape(match.value("type", "Control")) << "**: "
                        << escape(match.value("id", "")) << "\n";
                    if (!match.value("name", "").empty())
                        oss << "  - Name: " << escape(match.value("name", "")) << "\n";
                    if (match.contains("text") && match["text"].is_string())
                        oss << "  - Text: " << escape(match["text"].get<std::string>()) << "\n";
                    if (match.contains("tooltip") && match["tooltip"].is_string())
                        oss << "  - Tooltip: " << escape(match["tooltip"].get<std::string>()) << "\n";
                }
                return oss.str();
            }

            // Check if this is screen data (special formatting)
            if (response["status"] == "success" &&
                response.contains("data") &&
                response["data"].contains("screen_id") &&
                response["data"].contains("hierarchy")) {
                // Check if compact mode is enabled
                bool compact = false;
                try {
                    compact = response["data"].value("compact", false);
                } catch (const json::exception& e) {
                    spdlog::error("Failed to get 'compact' field: {}", e.what());
                    spdlog::error("response[\"data\"] type: {}", response["data"].type_name());
                    if (response["data"].contains("compact")) {
                        spdlog::error("compact field type: {}", response["data"]["compact"].type_name());
                    }
                    throw;  // Re-throw to see full stack trace
                }
                // Use special screen markdown formatter
                try {
                    oss << ScreenMarkdownFormatter::format(response["data"], compact);
                } catch (const json::exception& e) {
                    spdlog::error("ScreenMarkdownFormatter::format() failed: {}", e.what());
                    throw;  // Re-throw to see full stack trace
                }

                // Add metadata footer
                if (response.contains("metadata") && response["metadata"].is_object()) {
                    const auto& meta = response["metadata"];
                    if (meta.contains("duration_ms")) {
                        oss << "\n_Read time: " << meta["duration_ms"].get<int>() << "ms_\n";
                    }
                }

                return oss.str();
            }

            // Check if this is doctor diagnostic data
            if (response["status"] == "success" &&
                response.contains("data") &&
                response["data"].contains("overall_health") &&
                response["data"].contains("checks")) {
                std::string health = response["data"]["overall_health"];
                if (health == "ok") {
                    oss << "# ✅ Fairyfly Environment Diagnostics: Healthy\n\n";
                } else if (health == "warning") {
                    oss << "# ⚠️ Fairyfly Environment Diagnostics: Warnings Detected\n\n";
                } else {
                    oss << "# ❌ Fairyfly Environment Diagnostics: Issues Detected\n\n";
                }

                oss << "| Status | Check | Details | Remediation |\n";
                oss << "| :--- | :--- | :--- | :--- |\n";
                for (const auto& check : response["data"]["checks"]) {
                    std::string st = check.value("status", "info");
                    std::string icon = (st == "pass") ? "✅ PASS" : (st == "warn" ? "⚠️ WARN" : (st == "fail" ? "❌ FAIL" : "ℹ️ INFO"));
                    std::string msg = check.value("message", "");
                    std::string det = check.value("details", "-");
                    std::string rem = check.value("remediation", "-");
                    if (det.empty()) det = "-";
                    if (rem.empty()) rem = "-";
                    oss << "| " << icon << " | " << msg << " | " << det << " | " << rem << " |\n";
                }
                return oss.str();
            }

            // Standard markdown formatting for non-screen results
            // Title
            if (response["status"] == "success") {
                oss << "# ✅ Success\n\n";
            } else {
                oss << "# ❌ Error\n\n";
            }

            // Error details
            if (response.contains("error")) {
                const auto& error = response["error"];
                if (error.contains("code")) {
                    oss << "**Error Code:** `" << error["code"].get<std::string>() << "`\n\n";
                }
                if (error.contains("message")) {
                    oss << "**Message:** " << error["message"].get<std::string>() << "\n\n";
                }
                // Only show suggestions if verbose_errors is enabled
                if (verbose_errors && error.contains("suggestions") && error["suggestions"].is_array()) {
                    oss << "**Suggestions:**\n";
                    for (const auto& suggestion : error["suggestions"]) {
                        oss << "- " << suggestion.get<std::string>() << "\n";
                    }
                    oss << "\n";
                }
            }

            // Data
            if (response.contains("data") && !response["data"].empty()) {
                oss << "## Data\n\n```json\n";
                oss << response["data"].dump(2) << "\n```\n";
            }

            // Metadata
            if (response.contains("metadata") && response["metadata"].is_object()) {
                const auto& meta = response["metadata"];
                if (!meta.empty()) {
                    oss << "## Metadata\n\n";
                    if (meta.contains("duration_ms")) {
                        oss << "- **Duration:** " << meta["duration_ms"].get<int>() << "ms\n";
                    }
                    if (meta.contains("timestamp")) {
                        oss << "- **Timestamp:** " << meta["timestamp"].get<std::string>() << "\n";
                    }
                }
            }

            return oss.str();
        }
        case OutputFormat::PlainText: {
            std::ostringstream oss;
            const auto& response = result.to_json();

            if (response["status"] == "success") {
                oss << "SUCCESS\n";
            } else {
                oss << "ERROR\n";
                if (response.contains("error")) {
                    const auto& error = response["error"];
                    if (error.contains("message")) {
                        oss << error["message"].get<std::string>() << "\n";
                    }
                }
            }

            return oss.str();
        }
        case OutputFormat::Toon: {
            formatters::ToonEncoder encoder;
            formatters::ToonOptions opts;
            opts.indent = 2;
            opts.delimiter = ',';
            opts.length_marker = false;

            json output = result.to_json();
            apply_compact_screen_output(output);

            // In compact error mode, remove suggestions array
            if (!verbose_errors && output.contains("error") && output["error"].contains("suggestions")) {
                output["error"].erase("suggestions");
            }

            // Encode the full result structure (status, data, error, metadata)
            return encoder.encode(output, opts);
        }
    }

    return result.to_json().dump(2);
}

}  // namespace cli
}  // namespace fairyfly
