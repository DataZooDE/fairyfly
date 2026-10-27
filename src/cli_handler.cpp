#include "include/com/control_views.h"
#include "include/com/tree_view.h"
#include "include/cli_handler.h"
#include "include/sap_identity.h"
#include "include/string_utils.h"
#include "include/read_only_guard.h"
#include "include/sensitive_data.h"
#include "include/connection_launcher.h"
#include "include/sap_gui_scripting.h"
#include "include/com/wrapper.h"
#include "include/action_status.h"
#include "include/server_clock.h"
#include "include/action_argument_checks.h"
#include "include/login_flow.h"
#include "include/credential_resolver.h"
#include "include/table_data_extractor.h"
#include "include/constants.h"
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

bool owner_bound_session_matches(const Connection& saved, const audit::SapFacts& live,
                                 const std::string& expected_session, const std::string& expected_owner) {
    if (!live.connection_id || *live.connection_id != saved.id || expected_session.empty() ||
        live.session_identity != expected_session || live.system.empty() || live.client.empty() ||
        expected_owner != sap_identity(live)) return false;
    if (!saved.server_session_key.empty()) return true;
    if (!live.user.empty() || live.transaction != "S000" || saved.cache_generation.empty()) return false;
    const std::string prefix = saved.session_id + "||" + saved.cache_generation + "|hwnd:";
    if (live.session_identity.rfind(prefix, 0) != 0) return false;
    const std::string handle = live.session_identity.substr(prefix.size());
    return !handle.empty() && handle != "0" &&
        std::all_of(handle.begin(), handle.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

std::string CommandHandler::expand_active(const std::string& path) const {
    if (path.rfind("@active", 0) != 0) return path;  // asks SAP for the active window only when needed
    return sap::expand_active_window_prefix(path, engine_->get_active_window_id().id);
}

void CommandHandler::describe_element(const std::string& element_id, std::string& type,
                                       std::string& text, std::string& tooltip)
{
    auto* com_engine = engine_->scripting();
    if (!com_engine) return;
    try {
        auto session = com_engine->unchecked_session();
        if (!session) return;
        const std::string full_path = expand_active(element_id);
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
            auto* com_engine = engine_->scripting();
            auto session = com_engine ? com_engine->unchecked_session() : nullptr;
            if (session) {
                const std::string toolbar_path = expand_active(element_id.substr(0, btn_pos));
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
    std::string judged_id = element_id;
    try {
        judged_id = expand_active(element_id);
    } catch (const std::exception& e) {
        spdlog::debug("Read-only guard could not resolve the active window for {}: {}", element_id, e.what());
    }
    rule = sap::matched_read_only_rule(type, text, tooltip, judged_id);
    if (rule.empty()) return std::nullopt;
    return sap::make_read_only_refusal(element_id, type, text, tooltip, rule);
}

CommandHandler::CommandHandler(std::unique_ptr<cred::CredentialStore> store)
    : CommandHandler(std::move(store), AutomationEngine::create(), std::make_unique<ConnectionManager>()) {}

CommandHandler::CommandHandler(std::unique_ptr<cred::CredentialStore> store, std::unique_ptr<AutomationEngine> engine,
                               std::unique_ptr<ConnectionManager> connections)
    : engine_(std::move(engine)),
      conn_mgr_(std::move(connections)),
      credential_store_(store ? std::move(store) : cred::make_default_store())
{
    spdlog::debug("CommandHandler initialized");
}

void CommandHandler::set_owner_window_guard(bool enabled) {
    if (auto* com = engine_->scripting())
        com->set_owner_window_guard(enabled);
}

void CommandHandler::set_attach_owner_identities(std::vector<std::string> identities) {
    attach_owner_identities_ = std::move(identities);
    set_owner_window_guard(!attach_owner_identities_.empty());
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

ResultT<Connection> CommandHandler::resolve_and_validate_connection(std::optional<int> explicit_conn_id,
                                                                    bool update_cache)
{
    if (update_cache && !explicit_conn_id.has_value()) {
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
        if (update_cache) conn_mgr_->delete_connection_if_unchanged(conn);

        ResultT<Connection> result;
        result.status = ResultT<Connection>::Status::Error;
        result.error["code"] = "INVALID_CONNECTION";
        result.error["message"] = fmt::format("Connection {} session no longer exists", conn.id);
        result.error["session_id"] = conn.session_id;
        // what the record says it was attached to: the MCP dispatcher re-attaches an implicit default connection
        // only to the one open session of the same system, client and user
        result.error["stale_connection"] = {{"id", conn.id},
                                            {"session_id", conn.session_id},
                                            {"system", conn.identity.system},
                                            {"client", conn.identity.client},
                                            {"user", conn.identity.user}};
        result.error["suggestions"] = json::array({
            "The SAP session was closed or connection lost",
            "Run 'fairyfly session attach' to create a new connection"
        });
        return result;
    }

    // The dispatcher checked this exact saved connection before invoking the command. Recheck
    // after selecting it and before any SAP action or cache touch; a closed/reused GUI index or
    // changed cache generation must not silently redirect a queued MCP call.
    if (!expected_mcp_session_.empty()) {
        const audit::SapFacts live = audit_facts_for_connection(conn.id);
        if (!owner_bound_session_matches(conn, live, expected_mcp_session_, expected_mcp_owner_)) {
            ResultT<Connection> denied;
            denied.status = ResultT<Connection>::Status::Error;
            denied.error = {{"code", "OWNER_SESSION_UNAVAILABLE"}, {"message", "the requested SAP session is unavailable"}};
            return denied;
        }
    }

    // Update last_validated timestamp
    if (!update_cache) {
        // Owner-mode login must authorize the credential identity before altering saved state.
    } else if (conn.server_session_key.empty()) {
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
    pending_owner_attach_.reset();
    try {
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
        std::string checked_server_key;
        audit::SapFacts checked_facts;
        auto owner_attach_unavailable = [&]() {
            // attach_by_session_id already selected the target in the engine. Drop that
            // local binding so a denied foreign session cannot appear in later audit facts.
            // disconnect() here does not close the SAP GUI window.
            if (auto* com = engine_->scripting())
                (void)engine_->disconnect();
            result.status = Result::Status::Error;
            result.data = json::object();
            result.error = {{"code", "OWNER_SESSION_UNAVAILABLE"}, {"message", "the requested SAP session is unavailable"}};
            return result;
        };

        // The MCP endpoint's owner rule must run before create_or_update_connection.
        // The later dispatcher postcheck conditionally removes this generation if it changes.
        if (!attach_owner_identities_.empty()) {
            auto* com = engine_->scripting();
            checked_server_key = com ? engine_->current_server_session_key() : std::string();
            const audit::SapFacts facts = com && !attached_session_id.empty() && !checked_server_key.empty()
                ? com->peek_session_facts(attached_session_id, checked_server_key) : audit::SapFacts{};
            const std::string identity = sap_identity(facts);
            const bool allowed = !facts.system.empty() && !facts.client.empty() && !facts.user.empty() &&
                (!session_id || *session_id == attached_session_id) &&
                identity_listed(attach_owner_identities_, identity);
            if (!allowed) return owner_attach_unavailable();
        }

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

        if (!attach_owner_identities_.empty()) {
            auto* com = engine_->scripting();
            checked_facts = com && engine_->current_server_session_key() == checked_server_key
                ? com->peek_session_facts(attached_session_id, checked_server_key) : audit::SapFacts{};
            const std::string identity = sap_identity(checked_facts);
            if (checked_facts.system.empty() || checked_facts.client.empty() || checked_facts.user.empty() ||
                !identity_listed(attach_owner_identities_, identity))
                return owner_attach_unavailable();
            bool token_allowed = false;
            try { token_allowed = attach_target_guard_ && attach_target_guard_(checked_facts, description); }
            catch (...) {}
            if (!token_allowed) return owner_attach_unavailable();
        }

        // The SAP identity the connection belongs to: a stale default connection is re-attached later only to a
        // session of the same system, client and user (mcp dispatcher, auto_attach).
        const audit::SapFacts attached_facts = attach_owner_identities_.empty() ? audit_facts() : checked_facts;
        const ConnectionIdentity identity{attached_facts.system, attached_facts.client, attached_facts.user};

        // Create or update connection file
        Connection conn = conn_mgr_->create_or_update_connection(
            attached_session_id,
            connection_id,
            description,
            connection_string,
            window_title,
            attach_owner_identities_.empty() ? engine_->current_server_session_key() : checked_server_key,
            identity
        );
        if (!attach_owner_identities_.empty()) pending_owner_attach_ = conn;
        if (connection_changed_hook_) {
            try { connection_changed_hook_(conn.id); }
            catch (const std::exception& e) { spdlog::error("saved connection notification failed: {}", e.what()); }
            catch (...) { spdlog::error("saved connection notification failed"); }
        }

        result.data["connection_file_id"] = conn.id;
        result.data["connection_file"] = conn.get_file_path();
        result.data["pruned_stale"] = attach_owner_identities_.empty()
            ? conn_mgr_->prune_other_entries_for_path(conn, engine_->current_server_session_key())
            : 0;  // do not prune other users' entries during an owner-mode attach race
        result.data["message"] = fmt::format("Attached to SAP GUI session (connection: {})", conn.id);
        {
            const json clock = sap::server_time_fields();
            for (const auto& [key, value] : clock.items()) result.data[key] = value;
            result.data["server_time_summary"] = sap::server_time_summary(clock);
        }

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
    } catch (...) {
        if (!attach_owner_identities_.empty()) {
            if (pending_owner_attach_) {
                try { (void)finalize_owner_attach(false); }
                catch (...) { try { (void)engine_->disconnect(); } catch (...) {} }
            } else {
                try { (void)engine_->disconnect(); } catch (...) {}
            }
        }
        throw;
    }
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
    pending_owner_launch_.reset();
    spdlog::info("Launching SAP connection: {}", connection_name);
    auto result = engine_->launch_connection(connection_name, allow_sapshcut);

    if (result.status == Result::Status::Success) {
        spdlog::info("Successfully launched SAP connection: {}", connection_name);

        // Extract connection info from result
        std::string session_id = result.data.value("session_id", "");
        std::string connection_id = result.data.value("connection_id", "");

        // Owner-mode HTTP launches may open a prelogin window with no SAP user yet.
        // Check the exact new live window before writing any saved connection.
        auto owner_launch_unavailable = [&]() {
            if (auto* com = engine_->scripting())
                (void)engine_->disconnect();  // detach Fairyfly; leave the SAP window for operator inspection
            result.status = Result::Status::Error;
            result.data = json::object();
            result.diagnostics = json::object();
            result.error = {{"code", "OWNER_SESSION_UNAVAILABLE"},
                            {"message", "the requested SAP connection is unavailable"}};
            return result;
        };
        std::string checked_server_key;
        std::string checked_description;
        if (!attach_owner_identities_.empty()) {
            auto* com = engine_->scripting();
            checked_server_key = com ? engine_->current_server_session_key() : std::string();
            const auto selected = com ? com->unchecked_session() : nullptr;
            bool selected_matches = false;
            try { selected_matches = selected && selected->is_alive() && selected->get_id() == session_id; }
            catch (...) {}
            const audit::SapFacts live = com && selected_matches
                ? com->peek_session_facts(session_id, checked_server_key) : audit::SapFacts{};
            checked_description = com ? com->peek_session_connection_description(session_id) : std::string();
            bool allowed = false;
            try {
                allowed = !session_id.empty() && !connection_id.empty() && !live.system.empty() &&
                    sap::ConnectionLauncher::matches_connection_name(connection_name, checked_description) &&
                    launch_target_guard_ && launch_target_guard_(live, checked_description);
            } catch (...) {}
            if (!allowed) return owner_launch_unavailable();
        }

        // Get connection metadata
        std::string description = connection_name;
        std::string connection_string = "";
        if (!attach_owner_identities_.empty()) {
            description = checked_description;
        } else {
            auto app_info = engine_->get_application_info();
            if (app_info.contains("connections") && app_info["connections"].is_array()) {
                for (const auto& candidate : app_info["connections"]) {
                    if (candidate.value("id", "") == connection_id) {
                        description = candidate.value("description", connection_name);
                        connection_string = candidate.value("connection_string", "");
                        break;
                    }
                }
            }
        }

        // Create or update connection file
        Connection conn = attach_owner_identities_.empty() ? conn_mgr_->create_or_update_connection(
            session_id,
            connection_id,
            description,
            connection_string,
            "",  // No window title for launch
            attach_owner_identities_.empty() ? engine_->current_server_session_key() : checked_server_key
        ) : conn_mgr_->create_new_connection(
            session_id, connection_id, description, connection_string, "", checked_server_key);
        if (!attach_owner_identities_.empty()) pending_owner_launch_ = conn;
        if (connection_changed_hook_) {
            try { connection_changed_hook_(conn.id); }
            catch (const std::exception& e) { spdlog::error("saved connection notification failed: {}", e.what()); }
            catch (...) { spdlog::error("saved connection notification failed"); }
        }

        result.data["connection_file_id"] = conn.id;
        result.data["connection_file"] = conn.get_file_path();
        result.data["message"] = fmt::format("Launched SAP connection '{}' (connection: {})", connection_name, conn.id);

        spdlog::info("Created/updated connection file: {}", conn.get_file_path());

        // --login: authenticate through the scripting API (never a sapshcut command line),
        // also after the --allow-sapshcut fallback. Allowed under --read-only on purpose:
        // logging on is authentication, not a change of business state. On failure the
        // connection stays open and the launch data is returned with the login error.
        if (login) {
            Result login_result;
            try { login_result = handle_login("", conn.id, false, credential_name, multiple_logon); }
            catch (const std::exception& e) {
                spdlog::error("SAP login after launch failed unexpectedly: {}", e.what());
                login_result.set_error("OUTCOME_UNKNOWN", "SAP login outcome is unknown; inspect the launched session before retrying");
            } catch (...) {
                spdlog::error("SAP login after launch failed unexpectedly");
                login_result.set_error("OUTCOME_UNKNOWN", "SAP login outcome is unknown; inspect the launched session before retrying");
            }
            return compose_launch_login_result(std::move(result), login_result);
        }
    }

    return result;
}

bool CommandHandler::finalize_owner_attach(bool accepted) {
    if (!pending_owner_attach_) return false;
    const Connection written = *pending_owner_attach_;
    pending_owner_attach_.reset();
    if (accepted) {
        bool unchanged = false;
        try { unchanged = conn_mgr_->matches_connection_generation(written); } catch (...) {}
        if (unchanged) return true;
        try { (void)engine_->disconnect(); } catch (...) {}
        return false;
    }
    // The postcheck withheld this attach. The old cache generation, if any,
    // was replaced by the attach; deleting only our generation is safer than
    // leaving a session that failed verification addressable by its saved ID.
    try { (void)engine_->disconnect(); }
    catch (const std::exception& e) { spdlog::error("owner attach detach failed: {}", e.what()); }
    catch (...) { spdlog::error("owner attach detach failed"); }
    const bool removed = conn_mgr_->delete_connection_if_unchanged(written);
    if (removed && connection_changed_hook_) {
        try { connection_changed_hook_(written.id); }
        catch (const std::exception& e) { spdlog::error("saved connection notification failed: {}", e.what()); }
        catch (...) { spdlog::error("saved connection notification failed"); }
    }
    return removed;
}

bool CommandHandler::rollback_owner_launch(int connection_id) {
    if (!pending_owner_launch_ || pending_owner_launch_->id != connection_id) return false;
    const Connection written = *pending_owner_launch_;
    pending_owner_launch_.reset();
    // The launch may have selected a window that failed the owner's postcheck.
    // Drop the tray's COM binding even if the conditional cache delete fails.
    try { (void)engine_->disconnect(); }
    catch (const std::exception& e) { spdlog::error("owner launch detach failed: {}", e.what()); }
    catch (...) { spdlog::error("owner launch detach failed"); }
    const bool removed = conn_mgr_->delete_connection_if_unchanged(written);
    if (removed && connection_changed_hook_) {
        try { connection_changed_hook_(connection_id); }
        catch (const std::exception& e) { spdlog::error("saved connection notification failed: {}", e.what()); }
        catch (...) { spdlog::error("saved connection notification failed"); }
    }
    return removed;
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

    auto selected = resolve_and_validate_connection(connection_id, !login_target_guard_);
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
        result.error = login_target_guard_
            ? json{{"code", "OWNER_SESSION_UNAVAILABLE"},
                   {"message", "the requested SAP logon is unavailable"}}
            : resolved.error;
        return result;
    }
    LoginCredentials credentials = std::move(resolved.value.credentials);
    // Scrub the plaintext secrets on every exit path.
    struct CredentialScrubber {
        LoginCredentials& credentials;
        ~CredentialScrubber() { cred::scrub_login_credentials(credentials); }
    } scrubber{credentials};
    for (const auto& warning : resolved.value.warnings) spdlog::warn("{}", warning);

    const auto owner_login_allowed = [&]() {
        if (!login_target_guard_) return true;
        const audit::SapFacts live = audit_facts_for_connection(selected.value.id);
        const std::string expected_identity = selected.value.session_id + "|" +
            selected.value.server_session_key + "|" + selected.value.cache_generation;
        const std::string learned_identity = selected.value.session_id + "|" +
            engine_->current_server_session_key() + "|" + selected.value.cache_generation;
        const bool identity_matches = live.session_identity == expected_identity ||
            (selected.value.server_session_key.empty() &&
             !engine_->current_server_session_key().empty() &&
             live.session_identity == learned_identity);
        const auto target = peek_session_target("", "", selected.value.id);
        audit::SapFacts anticipated = live;
        anticipated.client = credentials.client;
        anticipated.user = credentials.username;
        bool allowed = false;
        try {
            allowed = live.connection_id == selected.value.id && identity_matches &&
                !live.system.empty() &&
                (live.user.empty() || sap_user_matches(live.user, credentials.username)) &&
                (live.client.empty() || live.client == credentials.client ||
                 (live.user.empty() && live.client == "000" && live.transaction == "S000")) &&
                !target.ambiguous &&
                !target.connection_name.empty() &&
                login_target_guard_(anticipated, target.connection_name);
        } catch (...) {}
        return allowed;
    };
    bool password_typed = false;
    const auto deny_owner_login = [&]() {
        // A revocation can arrive after the password field was filled.
        if (password_typed) {
            try { engine_->fill_field(ElementId("@active/usr/pwdRSYST-BCODE"), ""); } catch (...) {}
        }
        result.set_error("OWNER_SESSION_UNAVAILABLE", "the requested SAP logon is unavailable");
        return result;
    };
    if (login_target_guard_) {
        const audit::SapFacts live = audit_facts_for_connection(selected.value.id);
        if (!live.user.empty() || !owner_login_allowed()) {
            return deny_owner_login();
        }
    }

    auto* com_engine = engine_->scripting();
    if (!com_engine || !com_engine->unchecked_session()) {
        result.set_error("ENGINE_TYPE_MISMATCH", "SAP GUI logon requires the COM automation engine");
        return result;
    }
    const auto session = com_engine->unchecked_session();
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
        result.set_error("NOT_LOGON_SCREEN", "Selected session is not on the SAP logon screen");
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
    password_typed = true;

    if (!owner_login_allowed()) return deny_owner_login();
    result = engine_->click_element(ElementId("@active/tbar[0]/btn[0]"));
    if (result.status != Result::Status::Success) {
        engine_->fill_field(ElementId("@active/usr/pwdRSYST-BCODE"), "");
        return result;
    }
    if (connection_changed_hook_) {
        try { connection_changed_hook_(selected.value.id); }
        catch (const std::exception& e) { spdlog::error("saved connection notification failed: {}", e.what()); }
        catch (...) { spdlog::error("saved connection notification failed"); }
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
            result.set_error(plan.error_code.value_or("INVALID_ARGUMENT"), "--multiple-logon end is refused under --read-only");
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
            if (!owner_login_allowed()) return deny_owner_login();
            result = engine_->click_element(ElementId("@active/" + plan.radio_suffix));
            if (result.status != Result::Status::Success) return result;
            if (!owner_login_allowed()) return deny_owner_login();
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
            result.set_error("PASSWORD_CHANGE_REQUIRED", "SAP requires a new password; provide New Password in the credential input");
            return result;
        }
        for (const auto* field : {"@active/usr/pwdRSYST-NCODE", "@active/usr/pwdRSYST-NCOD2"}) {
            result = engine_->fill_field(ElementId(field), credentials.new_password);
            if (result.status != Result::Status::Success) return result;
        }
        if (!owner_login_allowed()) return deny_owner_login();
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

    if (login_target_guard_) {
        const audit::SapFacts authenticated = audit_facts_for_connection(selected.value.id);
        if (authenticated.system.empty() || authenticated.client != credentials.client ||
            !sap_user_matches(authenticated.user, credentials.username) || !owner_login_allowed()) {
            result.set_error("OUTCOME_UNKNOWN", "SAP logon may have completed but its owner cannot be verified; inspect or attach before retry");
            return result;
        }
        const std::string key = engine_->current_server_session_key();
        if (key.empty() || (!selected.value.server_session_key.empty() &&
                            selected.value.server_session_key != key)) {
            result.set_error("OUTCOME_UNKNOWN", "SAP logon completed but its saved session identity changed; attach it again");
            return result;
        }
        if (selected.value.server_session_key.empty()) {
            try {
                const Connection updated = conn_mgr_->set_session_key(selected.value, key);
                if (pending_owner_launch_ && pending_owner_launch_->id == selected.value.id &&
                    pending_owner_launch_->cache_generation == selected.value.cache_generation)
                    pending_owner_launch_ = updated;
            }
            catch (...) {
                result.set_error("OUTCOME_UNKNOWN", "SAP logon completed but its saved session could not be updated");
                return result;
            }
            if (connection_changed_hook_) {
                try { connection_changed_hook_(selected.value.id); }
                catch (const std::exception& e) { spdlog::error("saved connection notification failed: {}", e.what()); }
                catch (...) { spdlog::error("saved connection notification failed"); }
            }
        }
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
            result.set_error("NO_CONNECTIONS", "No connection files found");
            return result;
        }

        if (connections.size() > 1) {
            result.set_error("MULTIPLE_CONNECTIONS", fmt::format("Found {} connections, specify which one to disconnect", connections.size()), json::array({
                "Use --connection <id> to specify which connection to disconnect",
                "Run 'fairyfly connection list' to see all connections"
            }));
            return result;
        }

        connection_id = connections[0].id;
    }

    // Load the connection
    auto conn = conn_mgr_->load_connection(connection_id.value());
    if (!conn.has_value()) {
        result.set_error("CONNECTION_NOT_FOUND", fmt::format("Connection {} not found", connection_id.value()));
        return result;
    }

    if (close_session || !expected_mcp_session_.empty()) {
        auto selected = resolve_and_validate_connection(connection_id);
        if (selected.status != ResultT<Connection>::Status::Success) {
            return result_from_error(selected);
        }
        if (close_session) {
            auto closed = engine_->close_current_session();
            if (closed.status != Result::Status::Success) {
                return closed;
            }
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

Result CommandHandler::handle_connections_list(bool cleanup, bool validate)
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
        bool valid = validate && engine_->validate_session(conn.session_id, conn.server_session_key);

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

/// element click --node-key: select, expand, collapse, double-click or a context-menu item of a tree node.
Result CommandHandler::click_tree_node(const ElementId& elem, const std::string& element_id, const std::string& node_key,
                                       const std::string& tree_action, const std::string& menu_item, int connection_id) {
    Result result;
    if (tree_action == "contextmenu" && menu_item.empty()) {
        result.set_error("MENU_ITEM_REQUIRED", "--menu-item is required for tree context-menu actions");
        return result;
    }
    spdlog::info("Tree operation requested: action='{}', node_key='{}'", tree_action, node_key);

    try {
        const Stopwatch timer;

        // Get the ComAutomationEngine to access session
        auto* com_engine = engine_->scripting();
        if (!com_engine) {
            result.set_error("ENGINE_TYPE_MISMATCH", "Tree operations require COM automation engine (stub mode active)");
            return result;
        }

        auto session = com_engine->unchecked_session();
        if (!session) {
            result.set_error("NO_SESSION", "No active session");
            return result;
        }

        const std::string full_path = expand_active(elem.path);

        // Find element by ID
        auto tree_elem = session->find_element_by_id(full_path);
        if (!tree_elem) {
            result.set_error("ELEMENT_NOT_FOUND", fmt::format("Element not found: {}", full_path));
            return result;
        }

        // Check if element is a tree (optional - will fail gracefully if not)
        std::string elem_type = tree_elem->get_type();
        const auto before_status = sap::read_action_status(session);

        // Execute the tree action
        if (tree_action == "select") {
            as_tree(*tree_elem).select_node(node_key);
        } else if (tree_action == "expand") {
            as_tree(*tree_elem).expand_node(node_key);
        } else if (tree_action == "collapse") {
            as_tree(*tree_elem).collapse_node(node_key);
        } else if (tree_action == "contextmenu") {
            as_tree(*tree_elem).select_node_context_item(node_key, menu_item);
        } else { // doubleclick (default)
            as_tree(*tree_elem).doubleclick_node(node_key);
        }
        session->wait_for_completion(500);
        const auto after_status = sap::read_action_status(session);
        if (auto rejection = sap::classify_action_status(before_status,
                after_status, full_path,
                tree_action == "contextmenu" || tree_action == "doubleclick")) {
            sap::attach_status_bar(*rejection, before_status, after_status);
            rejection->error["node_key"] = node_key;
            rejection->error["tree_action"] = tree_action;
            rejection->duration = timer.elapsed();
            return *rejection;
        }

        const auto duration = timer.elapsed();

        result.status = Result::Status::Success;
        result.data["action"] = tree_action + "_node";
        result.data["node_key"] = node_key;
        if (tree_action == "contextmenu") result.data["menu_item"] = menu_item;
        result.data["element"] = full_path;
        result.data["element_type"] = elem_type;
        result.data["connection_id"] = connection_id;
        sap::attach_status_bar(result, before_status, after_status);
        result.duration = duration;

        spdlog::info("Executed tree action '{}' on node '{}' (duration: {}ms)",
                    tree_action, node_key, duration.count());

        return result;

    } catch (const std::exception& e) {
        result.set_error("TREE_ACTION_FAILED", fmt::format("Tree action '{}' failed: {}", tree_action, e.what()));
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

/// After a click with --wait-for-window: wait until the window OR the screen content changes (SAP often updates in
/// place, same window id), at most `timeout_ms`; what changed goes into result.data and the wait into its duration.
void CommandHandler::wait_for_screen_change(Result& result, const WindowId& window_before,
                                            const sap::ScreenSnapshot& snapshot_before, SapGuiScripting* snapshot_engine,
                                            int timeout_ms) {
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
        return Result::fail("INVALID_ELEMENT", fmt::format("Invalid element ID: {}", element_id), json::array({
            "Element IDs should start with 'wnd' or '@active' (e.g., wnd[0]/usr/btn[3] or @active/usr/btn[3])"
        }));
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
            if (auto* com_engine = engine_->scripting()) {
                if (auto session = com_engine->unchecked_session()) {
                    const std::string full_path = expand_active(elem.path);
                    if (auto target = session->find_element_by_id(full_path))
                        target_text = node_key.empty() ? as_grid(*target).get_cell_value(*row, column)
                                                       : as_tree(*target).get_node_text_by_key(node_key);
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
    auto* snapshot_engine = engine_->scripting();
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
            result.set_error("GRID_ROW_OPTIONS_REQUIRED", "GridView selection requires --row >= 0 and --column on a grid element");
            return result;
        }
        auto* com_engine = engine_->scripting();
        if (!com_engine) {
            result.set_error("ENGINE_TYPE_MISMATCH", "GridView row selection requires the COM automation engine");
            return result;
        }
        result = doubleclick ? com_engine->doubleclick_grid_cell(elem, *row, column)
                             : com_engine->select_grid_row(elem, *row, column);
        if (result.status == Result::Status::Success)
            result.data["connection_id"] = conn_result.value.id;
        return result;
    }

    // Check if node_key is provided - if so, this is a tree operation
    if (!node_key.empty()) return click_tree_node(elem, element_id, node_key, tree_action, menu_item, conn_result.value.id);

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
        if (wait_for_window) wait_for_screen_change(result, window_before, snapshot_before, snapshot_engine, timeout_ms);
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
        return Result::fail("INVALID_ELEMENT", fmt::format("Invalid element ID: {}", element_id), json::array({
            "Element IDs should start with 'wnd' or '@active' (e.g., wnd[0]/usr/ctxtFIELD or @active/usr/ctxtFIELD)"
        }));
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
        return Result::fail("INVALID_ELEMENT", fmt::format("Invalid element ID: {}", element_id));
    }

    const bool grid_requested = row.has_value() || !column.empty() || checkbox || commit;
    Result result;
    // Selection-input mode: only a plain text field, validated live by the engine right before the write. The policy
    // is cleared again on every exit path (RAII), so it can never leak into a later fill.
    struct SelectionPolicyScope {
        AutomationEngine& engine;
        bool active = false;
        ~SelectionPolicyScope() { if (active) { try { engine.set_selection_input_policy({}); } catch (...) {} } }
    } selection_scope{*engine_};
    if (selection_input_only_) {
        if (grid_requested) {
            result.set_error("INPUT_TARGET_DENIED", "typing is only allowed into plain selection fields, not into table or grid cells");
            return result;
        }
        sap::SelectionInputPolicy policy;
        policy.active = true;
        policy.program = selection_program_;
        policy.screen_number = selection_screen_;
        selection_scope.active = true;
        if (!engine_->set_selection_input_policy(policy)) {
            result.set_error("INPUT_TARGET_DENIED", "the automation engine cannot validate the field before typing (fail closed)");
            return result;
        }
    }
    if (grid_requested) {
        if (!row.has_value() || *row < 0 || column.empty()) {
            result.set_error("GRID_CELL_OPTIONS_REQUIRED", "GridView fill requires --row >= 0 and --column");
            return result;
        }
        auto* com_engine = engine_->scripting();
        if (!com_engine) {
            result.set_error("ENGINE_TYPE_MISMATCH", "GridView editing requires the COM automation engine");
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

Result CommandHandler::handle_fill_fields(const std::vector<sap::FillField>& fields, const std::string& then_key,
                                          bool stop_on_error, std::optional<int> connection_id, bool allow_fill)
{
    Result result;
    std::optional<int> then_vkey;
    if (!then_key.empty()) {
        then_vkey = sap::fill_then_vkey(then_key);
        if (!then_vkey) {
            result.set_error("INVALID_ARGUMENT", "then_key must be enter or f8; save, back and cancel are separate calls");
            return result;
        }
    }
    if (fields.empty()) {
        result.set_error("INVALID_ARGUMENT", "at least one field is required");
        return result;
    }

    // Every field takes exactly the single-field path (read-only guard, selection-input rules, grid cells, read-back).
    json entries = json::array();
    std::size_t filled = 0;
    std::optional<std::size_t> failed_at;
    json first_error;
    json last_ok;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        if (failed_at && stop_on_error) {
            entries.push_back({{"element", f.element}, {"ok", false}, {"skipped", true}});
            continue;
        }
        Result one = handle_fill(f.element, f.clear ? std::string() : f.value, connection_id, f.row, f.column, f.checkbox,
                                 f.commit, allow_fill);
        json entry = {{"element", f.element}, {"ok", one.status == Result::Status::Success}};
        if (one.status == Result::Status::Success) {
            ++filled;
            for (const char* key : {"value", "value_redacted", "selected", "key", "display_value"})
                if (one.data.contains(key)) entry[key] = one.data[key];
            last_ok = one.data;
        } else {
            entry["error_code"] = one.error.value("code", std::string("ERROR"));
            entry["message"] = one.error.value("message", std::string());
            if (!failed_at) {
                failed_at = i;
                first_error = one.error;
            }
        }
        entries.push_back(std::move(entry));
    }

    result.data["action"] = "fill_fields";
    result.data["fields"] = entries;
    result.data["filled"] = filled;
    result.data["total"] = fields.size();
    if (last_ok.contains("connection_id")) result.data["connection_id"] = last_ok["connection_id"];
    if (last_ok.contains("window")) result.data["window"] = last_ok["window"];

    if (failed_at) {
        result.status = Result::Status::Error;
        result.error = first_error;
        result.error["message"] = fmt::format("field {} of {} ({}) failed: {}{}", *failed_at + 1, fields.size(),
                                              fields[*failed_at].element, first_error.value("message", std::string()),
                                              then_vkey ? "; the key was not pressed" : "");
        result.error["failed_at"] = *failed_at;
        result.error["filled"] = filled;
        result.error["fields"] = entries;
        return result;
    }
    if (!then_vkey) {
        result.status = Result::Status::Success;
        if (last_ok.contains("statusbar")) result.data["statusbar"] = last_ok["statusbar"];
        return result;
    }

    // The key goes to the main window only: on a popup, Enter could confirm a save or delete dialog.
    auto* com_engine = engine_->scripting();
    const std::string active = com_engine ? engine_->get_active_window_id().id : std::string();
    if (!com_engine || WindowId(active).get_index() != 0) {
        result.set_error("THEN_KEY_NOT_ON_MAIN_WINDOW", fmt::format("all {} fields were filled, but a popup ({}) is open, so {} was not pressed",
                                              fields.size(), active.empty() ? "unknown window" : active, then_key));
        result.error["hint"] = "read the popup, then answer it with gui_popup_close or gui_key_send";
        result.error["filled"] = filled;
        result.error["fields"] = entries;
        return result;
    }
    Result key = handle_send_key(then_key, "wnd[0]", connection_id);
    if (key.status != Result::Status::Success) {
        result.status = Result::Status::Error;
        result.error = key.error;
        result.error["message"] = fmt::format("all {} fields were filled, but {} failed: {}", fields.size(), then_key,
                                              key.error.value("message", std::string()));
        result.error["filled"] = filled;
        result.error["fields"] = entries;
        return result;
    }
    result.status = Result::Status::Success;
    result.data["then_key"] = then_key;
    for (auto it = key.data.begin(); it != key.data.end(); ++it)
        if (it.key() != "key" && it.key() != "connection_id" && !result.data.contains(it.key())) result.data[it.key()] = it.value();
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
        return Result::fail("INVALID_ELEMENT", fmt::format("Invalid element ID: {}", element_id));
    }

    // Handle tree node listing
    if (list_nodes) {
        spdlog::info("Listing tree nodes: {} on connection {}", element_id, conn_result.value.id);

        try {
            // Get the actual element COM object
            auto* com_engine = engine_->scripting();
            if (!com_engine) {
                return Result::fail("ENGINE_TYPE_MISMATCH", "Tree operations require COM automation engine (stub mode active)");
            }

            // Check for modal dialog blocking access to main window
            auto dialog_info = com_engine->detect_modal_dialog();
            if (!dialog_info.empty()) {
                Result result;
                result.set_error("MODAL_DIALOG_ACTIVE", "Cannot access element - modal dialog is active");
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

            auto session = com_engine->unchecked_session();
            if (!session) {
                return Result::fail("NO_SESSION", "No active session");
            }

            const std::string full_path = expand_active(elem.path);

            // Find element by ID
            auto tree_elem = session->find_element_by_id(full_path);
            if (!tree_elem) {
                return Result::fail("ELEMENT_NOT_FOUND", fmt::format("Element not found: {}", full_path));
            }

            // Check if element is a tree
            std::string elem_type = tree_elem->get_type();
            if (elem_type != "GuiShell" && elem_type != "GuiTree") {
                Result result;
                result.set_error("NOT_A_TREE", fmt::format("Element is not a tree control (type: {})", elem_type));
                result.error["element"] = full_path;
                result.error["element_type"] = elem_type;
                return result;
            }

            // Get all node keys
            const Stopwatch timer;
            auto node_keys = as_tree(*tree_elem).get_all_node_keys();

            // Build nodes array with keys and text
            json nodes = json::array();
            sap::TreeNodeTexts node_texts(*tree_elem);
            for (const auto& key : node_keys) {
                json node;
                node["key"] = key;
                node["text"] = node_texts.text(key);
                nodes.push_back(node);
            }

            const auto duration = timer.elapsed();

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
            return Result::fail("TREE_LIST_FAILED", fmt::format("Failed to list tree nodes: {}", e.what()));
        }
    }

    // Regular field reading
    spdlog::info("Reading field: {} on connection {}", element_id, conn_result.value.id);
    auto* com_read_engine = engine_->scripting();
    auto result = (activate_tab && com_read_engine) ? com_read_engine->read_field(elem, true)
                                                    : engine_->read_field(elem);

    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
    }

    return result;
}

Result CommandHandler::handle_screen_read(bool include_children, std::optional<int> connection_id,
                                           bool expand_tabs, const ScreenFilterOptions& filters,
                                           bool skip_trees, bool compact, int max_rows,
                                           const std::string& only_tab, bool probe_all, int row_offset,
                                           const std::string& tree_reader)
{
    // Resolve and validate connection
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }

    spdlog::info("Reading screen structure (include_children={}, expand_tabs={}, skip_trees={}) on connection {}",
                 include_children, expand_tabs, skip_trees, conn_result.value.id);

    engine_->set_probe_all(probe_all);
    engine_->set_tree_reader(tree_reader);
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

Result CommandHandler::handle_object_tree_dump(std::optional<int> connection_id, const std::string& id,
                                               const std::vector<std::string>& props)
{
    auto conn_result = resolve_and_validate_connection(connection_id);
    if (conn_result.status != ResultT<Connection>::Status::Success) {
        return result_from_error(conn_result);
    }
    auto result = engine_->dump_object_tree(id, props);
    if (result.status == Result::Status::Success) {
        result.data["connection_id"] = conn_result.value.id;
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
            result.set_error("ENUMERATION_FAILED", info["error"]);
            return result;
        }
        
        // Log summary
        int total_conns = info["total_connections"].get<int>();
        int total_sess = info["total_sessions"].get<int>();
        spdlog::info("Found {} connection(s) with {} total session(s)", total_conns, total_sess);
        
    } catch (const std::exception& e) {
        result.set_error("ENUMERATION_FAILED", e.what());
        spdlog::error("Failed to enumerate SAP GUI: {}", e.what());
    }
    
    return result;
}

}  // namespace cli
}  // namespace fairyfly
