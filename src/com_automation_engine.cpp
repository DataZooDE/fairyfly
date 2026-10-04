#include "include/com_automation_engine.h"
#include "include/element_errors.h"
#include "include/object_tree_diag.h"
#include "include/session_facts.h"
#include "include/action_status.h"
#include "include/field_fill_info.h"
#include "include/tab_guard.h"
#include "include/server_clock.h"
#include "include/collection_id_lookup.h"
#include "include/sensitive_data.h"
#include "include/html_viewer_reader.h"
#include "include/com/raii_helpers.h"
#include "include/com/wrapper_helpers.h"
#include "include/constants.h"
#include "include/trace.h"
#include "include/element_type_registry.h"
#include "include/screen_element_collector.h"
#include "include/table_data_extractor.h"
#include "include/cli_handler.h"
#include "include/string_utils.h"
#include "include/system/window_owner.h"
#include "include/base64.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <map>
#include <vector>
#include <fmt/format.h>
#ifdef _WIN32
#include <cstdio>
#include <errno.h>
#endif

// CImg for image processing
// Force Windows GDI display (not X11)
#ifdef _WIN32
    #ifndef cimg_display
        #define cimg_display 2  // CIMG_DISPLAY_GDI value
    #endif
#else
    #ifndef cimg_display
        #define cimg_display 0  // Disable display on non-Windows
    #endif
#endif
#include <CImg.h>

namespace fairyfly {
namespace sap {

using utils::TraceGuard;

static ComGuiElementPtr find_element_if_present(const ComGuiSessionPtr& session,
                                                const std::string& path) {
    try {
        return session->find_element_by_id(path);
    } catch (const ComException& error) {
        if (!is_missing_element_error(error.what())) throw;
        return nullptr;
    }
}

/// State of a tab page for the inactive-tab check: whether it exists, its caption and whether it is the strip's
/// SelectedTab. Any COM failure reads as "unknown" (not found), so the check never turns into a new error.
static TabPageLookup make_tab_lookup(const ComGuiSessionPtr& session) {
    return [session](const TabPageRef& ref) {
        TabPageState state;
        try {
            auto page = find_element_if_present(session, ref.page_id);
            if (!page) return state;
            state.found = true;
            try { state.text = page->get_string_property(L"Text"); } catch (const std::exception&) {}
            auto strip = find_element_if_present(session, ref.strip_id);
            if (!strip) { state.found = false; return state; }
            auto selected = strip->get_dispatch_property(L"SelectedTab");
            state.selected = selected && ComGuiElement::create(selected)->get_id() == ref.page_id;
        } catch (const std::exception&) {
            state = TabPageState{};
        }
        return state;
    };
}

/// ELEMENT_ON_INACTIVE_TAB when `path` is missing because a tab page above it is not selected.
static std::optional<Result> inactive_tab_error(const ComGuiSessionPtr& session, const std::string& path,
                                                bool offer_activate_tab) {
    try {
        return classify_element_on_inactive_tab(path, make_tab_lookup(session), offer_activate_tab);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

ComAutomationEngine::ComAutomationEngine() {
    try {
        // Initialize COM and get SAP GUI application
        app_ = ComGuiApplication::create();
        spdlog::debug("ComAutomationEngine initialized successfully");

        // Initialize connection launcher
        connection_launcher_ = std::make_unique<ConnectionLauncher>(app_);

        // Check if any connections exist
        if (app_->get_connection_count() > 0) {
            auto connection = app_->get_connection(0);
            if (connection && connection->get_session_count() > 0) {
                bind_session(connection, connection->get_session(0));
                spdlog::debug("Found existing connection and session");
            } else {
                current_connection_ = connection;
            }
        }
    } catch (const ComInitializationException&) {
        throw;
    } catch (const ComException& e) {
        spdlog::warn("SAP GUI not available: {}", e.what());
        // Non-fatal - can still be initialized, just not connected
    }
}

void ComAutomationEngine::initialize_services() {
    if (current_session_) {
        auto screenshot_handler = std::make_unique<ScreenshotHandler>(current_session_);
        auto screen_reader = std::make_unique<ScreenReader>(current_session_);
        screenshot_handler_ = std::move(screenshot_handler);
        screen_reader_ = std::move(screen_reader);
        spdlog::debug("Service classes initialized");
    }
}

void ComAutomationEngine::bind_session(ComGuiConnectionPtr connection, ComGuiSessionPtr session) {
    if (!connection || !session) {
        throw ComException("Cannot attach an unavailable SAP session");
    }
    auto screenshot_handler = std::make_unique<ScreenshotHandler>(session);
    auto screen_reader = std::make_unique<ScreenReader>(session);
    current_connection_ = std::move(connection);
    current_session_ = std::move(session);
    screenshot_handler_ = std::move(screenshot_handler);
    screen_reader_ = std::move(screen_reader);
}

ComGuiConnectionPtr ComAutomationEngine::ensure_connection() {
    if (!current_connection_) {
        if (!app_ || app_->get_connection_count() == 0) {
            throw ComException("No SAP connections available");
        }
        current_connection_ = app_->get_connection(0);
    }
    return current_connection_;
}

ComGuiSessionPtr ComAutomationEngine::ensure_session() {
    if (!current_session_) {
        auto conn = ensure_connection();
        if (conn->get_session_count() == 0) {
            throw ComException("No sessions available in connection");
        }
        auto session = conn->get_session(0);
        if (!owner_window_allowed(session))
            throw ComException("SAP GUI window is unavailable to this Windows logon");
        bind_session(conn, std::move(session));
    }
    if (!owner_window_allowed(current_session_))
        throw ComException("SAP GUI window is unavailable to this Windows logon");
    return current_session_;
}

bool ComAutomationEngine::owner_window_allowed(const ComGuiSessionPtr& session) const noexcept {
    if (!owner_window_guard_) return true;
    try {
        if (!session) return false;
        const auto window = session->get_active_window();
        if (!window) return false;
        // SAP's Handle is a signed COM Long; Windows sign-extends user handles.
        const auto raw = window->get_int_property(L"Handle");
        const auto handle = system::sap_com_long_to_window_handle(raw);
        return system::window_owned_by_current_logon(handle);
    } catch (...) { return false; }
}

Result ComAutomationEngine::attach_by_click(int timeout_seconds) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!app_) {
            app_ = ComGuiApplication::create();
        }

        spdlog::info("Starting window selection (timeout: {}s)", timeout_seconds);

        // Get user to click on SAP window
        HWND selected_hwnd = select_window_by_mouse_click(timeout_seconds);

        spdlog::info("Window selected, searching for matching SAP session...");

        // Find the connection/session that matches this HWND
        SAPGuiWindow window = app_->find_window_by_hwnd(selected_hwnd);

        if (!window.is_valid()) {
            result.status = Result::Status::Error;
            result.error["code"] = "WINDOW_NOT_FOUND";
            result.error["message"] = "Selected window is not an active SAP GUI session";
            result.error["suggestions"] = json::array({
                "Ensure the window you clicked is a SAP transaction window (not just SAP Logon)",
                "The window must have an active SAP session with a transaction open"
            });
            return result;
        }

        // Use found connection and session
        auto connection = app_->get_connection(window.connection_idx);
        bind_session(connection, connection->get_session(window.session_idx));

        result.status = Result::Status::Success;
        result.data["window_title"] = window.title;
        result.data["connection_id"] = current_connection_->get_id();
        result.data["session_id"] = current_session_->get_id();

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Attached to SAP session via window click (duration: {}ms)", result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "ATTACH_FAILED";
        result.error["message"] = e.what();
        spdlog::error("Window selection failed: {}", e.what());
    }

    return result;
}

Result ComAutomationEngine::attach_by_session_id(const std::string& session_id) {
    Result result;
    if (!select_session(session_id)) {
        result.status = Result::Status::Error;
        result.error = {{"code", "SESSION_NOT_FOUND"},
                        {"message", "The exact SAP GUI session ID is not accessible"},
                        {"session_id", session_id}};
        return result;
    }

    result.status = Result::Status::Success;
    result.data["connection_id"] = current_connection_->get_id();
    result.data["session_id"] = current_session_->get_id();
    try {
        auto window = current_session_->get_active_window();
        result.data["window_title"] = window ? window->get_title() : "";
    } catch (const std::exception&) {
        result.data["window_title"] = "";
    }
    return result;
}

Result ComAutomationEngine::launch_connection(const std::string& connection_name, bool allow_sapshcut) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;
    json trace = json::array();  // Diagnostic trace

    try {
        trace.push_back({{"step", "initialize"}, {"message", "Starting native SAP connection"}});

        spdlog::info("Launching SAP connection using native COM: {}", connection_name);
        trace.push_back({{"step", "launch_start"}, {"connection_name", connection_name}});

        // Step 1: Try native COM OpenConnection first (fast, in-process, avoids sapshcut security prompt)
        bool session_created = false;
        bool native_connection_opened = false;
        bool backend_scripting_disabled = false;
        bool used_sapshcut = false;
        std::string native_error;
        try {
            if (!app_) {
                app_ = ComGuiApplication::create();
            }
            if (app_) {
                spdlog::info("Attempting native COM OpenConnection('{}')", connection_name);
                auto conn = app_->open_connection(connection_name, true, false);
                native_connection_opened = conn != nullptr;
                if (conn) {
                    try {
                        backend_scripting_disabled = conn->get_bool_property(L"DisabledByServer");
                    } catch (const ComException&) {
                        // Older SAP GUI versions may not expose this property.
                    }
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    while (!backend_scripting_disabled && conn->get_session_count() == 0 &&
                           std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(constants::SESSION_WAIT_INTERVAL_MS));
                    }
                }
                if (conn && conn->get_session_count() > 0) {
                    bind_session(conn, conn->get_session(0));
                    session_created = true;
                    spdlog::info("Native COM OpenConnection established session: {}", current_session_->get_id());
                    trace.push_back({
                        {"step", "native_open_connection"},
                        {"success", true},
                        {"session_id", current_session_->get_id()}
                    });
                }
            }
        } catch (const std::exception& e) {
            native_error = e.what();
            spdlog::debug("Native COM OpenConnection attempt failed: {}", native_error);
        }

        if (!session_created && (native_connection_opened || !allow_sapshcut)) {
            result.status = Result::Status::Error;
            result.error["code"] = native_connection_opened ? "SESSION_NOT_READY" : "CONNECTION_OPEN_FAILED";
            result.error["message"] = native_connection_opened
                ? "SAP connection opened but no session is available"
                : "Native SAP connection did not open";
            result.error["connection_name"] = connection_name;
            if (backend_scripting_disabled) {
                result.error["detail"] = "SAP reports that backend GUI scripting is disabled for this connection";
            }
            if (!native_error.empty()) result.error["native_error"] = native_error;
            result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start);
            result.diagnostics["trace"] = trace;
            return result;
        }

        if (!session_created) {
            // Step 2: Explicit opt-in fallback when native COM did not open a connection.
            used_sapshcut = true;
            // A launched session must be new; an existing session on the same
            // SAP Logon entry is not evidence that this launch succeeded.
            std::vector<ConnectionLauncher::ExistingSession> existing_sessions;
            if (!app_) throw std::runtime_error("Cannot inspect SAP sessions before launch");
            connection_launcher_ = std::make_unique<ConnectionLauncher>(app_);
            for (int c = 0; c < app_->get_connection_count(); ++c) {
                auto existing_connection = app_->get_connection(c);
                if (!existing_connection) continue;
                for (int s = 0; s < existing_connection->get_session_count(); ++s) {
                    auto existing_session = existing_connection->get_session(s);
                    if (existing_session) {
                        existing_sessions.push_back({existing_session->get_id(),
                                                     existing_session->get_server_session_key(),
                                                     existing_session});
                    }
                }
            }

            bool launch_success = ConnectionLauncher::launch_sapshcut(connection_name);
            trace.push_back({
                {"step", "launch_sapshcut"},
                {"success", launch_success}
            });

            if (!launch_success) {
                throw std::runtime_error("Failed to launch sapshcut.exe");
            }

            // Step 3: Wait for session to be created
            auto [connection, session] = connection_launcher_->wait_for_session(
                connection_name, constants::SESSION_CREATION_TIMEOUT_SEC, existing_sessions);
            session_created = (connection != nullptr && session != nullptr);

            if (session_created) {
                bind_session(connection, session);
            }
            trace.push_back({
                {"step", "wait_for_session"},
                {"success", session_created},
                {"timeout_seconds", constants::SESSION_CREATION_TIMEOUT_SEC}
            });
        }

        if (!session_created) {
            result.status = Result::Status::Error;
            if (used_sapshcut && connection_launcher_ &&
                connection_launcher_->wait_failure() == ConnectionLauncher::WaitFailure::SecurityPrompt) {
                result.error["code"] = "SAP_GUI_SECURITY_PROMPT";
                result.error["message"] = "SAP GUI Security is waiting for a decision on the shortcut connection";
                result.error["suggestions"] = json::array({
                    "Review the SAP GUI Security dialog for this connection",
                    "Use native SAP GUI launch when available"
                });
            } else {
                result.error["code"] = "SESSION_TIMEOUT";
                result.error["message"] = "Session was not created within timeout period";
                result.error["suggestions"] = json::array({
                    "Verify the SAP Logon entry is configured and reachable",
                    "Check that SAP GUI scripting is enabled",
                    "Ensure SAP system is accessible and responding",
                    "Try increasing timeout or manual login first"
                });
            }
            result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start);
            result.diagnostics["trace"] = trace;
            return result;
        }

        // Step 4: Session found! Capture details
        // COM IDs already use the same zero-based indices as the application collection.
        std::string conn_id = current_connection_ ? current_connection_->get_id() : "";
        std::string sess_id = current_session_ ? current_session_->get_id() : "";

        trace.push_back({
            {"step", "session_attached"},
            {"connection_id", conn_id},
            {"session_id", sess_id},
            {"status", "success"}
        });

        auto end = std::chrono::high_resolution_clock::now();
        result.status = Result::Status::Success;
        result.data["connection_name"] = connection_name;
        result.data["connection_id"] = conn_id;
        result.data["session_id"] = sess_id;
        result.data["message"] = fmt::format("Launched SAP connection '{}' using {}", connection_name,
                                               used_sapshcut ? "sapshcut" : "native COM");

        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        result.diagnostics["trace"] = trace;
        result.diagnostics["timing_ms"] = result.duration.count();

        trace.push_back({
            {"step", "complete"},
            {"duration_ms", result.duration.count()}
        });

        spdlog::info("Successfully launched SAP connection '{}' (duration: {}ms)", connection_name, result.duration.count());

    } catch (const std::exception& e) {
        auto end = std::chrono::high_resolution_clock::now();

        trace.push_back({
            {"step", "exception"},
            {"type", "std::exception"},
            {"message", e.what()}
        });

        result.status = Result::Status::Error;
        result.error["code"] = "LAUNCH_FAILED";
        result.error["message"] = e.what();
        result.error["suggestions"] = json::array({
            "Verify the SAP Logon entry is configured and reachable",
            "Verify sapshcut.exe is installed (part of SAP GUI)",
            "Check SAP GUI scripting is enabled (Options → Accessibility & Scripting)",
            fmt::format("Verify connection '{}' is configured in SAP Logon", connection_name)
        });
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        result.diagnostics["trace"] = trace;
        result.diagnostics["timing_ms"] = result.duration.count();

        spdlog::error("Launch connection failed: {}", e.what());
    }

    return result;
}

Result ComAutomationEngine::disconnect() {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        screenshot_handler_.reset();
        screen_reader_.reset();
        current_session_ = nullptr;
        current_connection_ = nullptr;
        // Note: app_ is kept alive for potential reconnection

        result.status = Result::Status::Success;
        result.data["message"] = "Disconnected from SAP";

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Disconnected from SAP");

    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

bool session_present_checked(const ComGuiApplicationPtr& app,
                             const std::string& session_id,
                             const std::string& server_key) {
    if (!app) throw ComException("SAP GUI application is unavailable");
    const auto separator = session_id.find("/ses[");
    if (separator == std::string::npos)
        throw ComException("Selected SAP GUI session ID is invalid");
    const std::string connection_id = session_id.substr(0, separator);

    auto connections = app->get_dispatch_property(L"Connections");
    if (!connections) connections = app->get_dispatch_property(L"Children");
    if (!connections) throw ComException("SAP GUI connections could not be enumerated");
    const int connection_count = get_collection_count_checked(connections);
    SapGuiCollection<ComGuiConnection> connection_items(connections);
    for (int index = 0; index < connection_count; ++index) {
        auto connection = connection_items.item(index);
        if (!connection) throw ComException("A SAP GUI connection could not be inspected");
        const std::string live_connection_id = connection->get_id();
        if (live_connection_id.empty()) throw ComException("A SAP GUI connection ID is unavailable");
        if (live_connection_id != connection_id) continue;
        if (connection->get_bool_property(L"DisabledByServer"))
            throw ComException("SAP GUI scripting is disabled for the selected connection");

        auto sessions = connection->get_dispatch_property(L"Children");
        if (!sessions) sessions = connection->get_dispatch_property(L"Sessions");
        if (!sessions) throw ComException("SAP GUI sessions could not be enumerated");
        const int session_count = get_collection_count_checked(sessions);
        SapGuiCollection<ComGuiSession> session_items(sessions);
        for (int child = 0; child < session_count; ++child) {
            auto session = session_items.item(child);
            if (!session) throw ComException("A SAP GUI session could not be inspected");
            const std::string live_session_id = session->get_id();
            if (live_session_id.empty()) throw ComException("A SAP GUI session ID is unavailable");
            if (live_session_id != session_id) continue;
            if (server_key.empty()) return true;
            const std::string live_key = session->get_server_session_key();
            if (live_key.empty()) throw ComException("SAP GUI server session key is unavailable");
            return live_key == server_key;
        }
        return false;
    }
    return false;
}

Result ComAutomationEngine::close_current_session() {
    Result result;
    try {
        auto session = ensure_session();
        auto connection = ensure_connection();
        const std::string session_id = session->get_id();
        const std::string server_key = session->get_server_session_key();
        connection->invoke_method_with_string(L"CloseSession", session_id);

        bool uncertain = false;
        std::string observation_error;
        for (int attempt = 0; attempt < 20; ++attempt) {
            try {
                if (!session_present_checked(app_, session_id, server_key)) {
                    disconnect();
                    result.status = Result::Status::Success;
                    result.data["session_id"] = session_id;
                    return result;
                }
            } catch (const std::exception& e) {
                uncertain = true;
                observation_error = e.what();
            }
            if (attempt < 19) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        result.status = Result::Status::Error;
        result.error = uncertain
            ? json{{"code", "SESSION_CLOSE_UNVERIFIED"},
                   {"message", "Could not verify that SAP GUI closed the selected session: " + observation_error}}
            : json{{"code", "SESSION_STILL_OPEN"},
                   {"message", "SAP GUI did not close the selected session"}};
        return result;
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error = {{"code", "SESSION_CLOSE_FAILED"}, {"message", e.what()}};
        return result;
    }
}

bool ComAutomationEngine::is_connected() const {
    return current_session_ != nullptr && current_session_->is_alive();
}

std::pair<ComGuiConnectionPtr, ComGuiSessionPtr> ComAutomationEngine::find_session_by_id(
    const std::string& session_id) const {
    if (!app_) return {};
    static const std::regex pattern(R"(^/app/con\[(\d+)\]/ses\[(\d+)\]$)");
    std::smatch match;
    if (!std::regex_match(session_id, match, pattern)) return {};
    const std::string connection_id = "/app/con[" + match[1].str() + "]";
    const auto connection_index = find_collection_index_by_id(
        app_->get_connection_count(), connection_id, [this](int index) {
            auto connection = app_->get_connection(index);
            return connection ? connection->get_id() : std::string{};
        });
    if (!connection_index) return {};
    auto conn = app_->get_connection(*connection_index);
    const auto session_index = find_collection_index_by_id(
        conn->get_session_count(), session_id, [&conn](int index) {
            auto session = conn->get_session(index);
            return session ? session->get_id() : std::string{};
        });
    if (!session_index) return {};
    auto sess = conn->get_session(*session_index);
    if (!sess || sess->get_id() != session_id || !sess->is_alive()) return {};
    return {conn, sess};
}

audit::SapFacts ComAutomationEngine::peek_session_facts(const std::string& session_id,
                                                        const std::string& server_session_key) const noexcept {
    audit::SapFacts facts;
    try {
        auto sess = find_session_by_id(session_id).second;
        if (!sess) return facts;
        if (!owner_window_allowed(sess)) return facts;
        if (!server_session_key.empty() && sess->get_server_session_key() != server_session_key) return facts;
        bool timed_out = false;
        facts = read_facts_bounded(
            [&] { return sess->is_busy(); },
            [&] {
                audit::SapFacts read;
                read.system = sess->get_system_name();
                read.client = sess->get_client();
                read.user = sess->get_user();
                read.transaction = sess->get_transaction_code();
                read.program = sess->get_program();
                read.screen_number = sess->get_screen_number();
                return read;
            },
            FactsBudget{}, FactsClock{}, &timed_out);
        if (timed_out)
            spdlog::warn("peek_session_facts: session {} stayed busy for 5 s, facts unknown", session_id);
        if (!owner_window_allowed(sess)) return audit::SapFacts{};
    } catch (...) {
        return audit::SapFacts{};
    }
    return facts;
}

std::string ComAutomationEngine::peek_session_connection_description(const std::string& session_id) const noexcept {
    try {
        auto found = find_session_by_id(session_id);
        if (!found.first || !found.second) return {};
        if (!owner_window_allowed(found.second)) return {};
        return found.first->get_description();
    } catch (...) {
        return {};
    }
}

std::uintptr_t ComAutomationEngine::peek_session_window_handle(const std::string& session_id) const noexcept {
    try {
        auto sess = find_session_by_id(session_id).second;
        if (!sess || !owner_window_allowed(sess)) return 0;
        auto window = sess->get_active_window();
        if (!window) return 0;
        const auto handle = system::sap_com_long_to_window_handle(window->get_int_property(L"Handle"));
        return system::window_owned_by_current_logon(handle) ? handle : 0;
    } catch (...) { return 0; }
}

bool ComAutomationEngine::validate_session(const std::string& session_id,
                                           const std::string& server_session_key) const {
    try {
        auto session = find_session_by_id(session_id).second;
        return session && owner_window_allowed(session) && (server_session_key.empty() ||
                           session->get_server_session_key() == server_session_key);
    } catch (const std::exception& e) {
        spdlog::debug("Cannot validate session {}: {}", session_id, e.what());
        return false;
    }
}

bool ComAutomationEngine::validate_session_for_cleanup(
    const std::string& session_id, const std::string& server_session_key) const {
    return session_present_checked(app_, session_id, server_session_key);
}

bool ComAutomationEngine::select_session(const std::string& session_id,
                                         const std::string& server_session_key) {
    try {
        auto [conn, sess] = find_session_by_id(session_id);
        if (!sess) return false;
        if (!owner_window_allowed(sess)) return false;
        if (!server_session_key.empty() &&
            sess->get_server_session_key() != server_session_key) return false;
        bind_session(std::move(conn), std::move(sess));
        return true;
    } catch (const std::exception& e) {
        spdlog::debug("Cannot select session {}: {}", session_id, e.what());
        return false;
    }
}

std::string ComAutomationEngine::current_server_session_key() const {
    return current_session_ ? current_session_->get_server_session_key() : "";
}

Result ComAutomationEngine::execute_transaction(const std::string& tcode) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        auto session = ensure_session();
        const auto request = normalize_transaction_request(tcode);
        if (!request.valid) {
            result.status = Result::Status::Error;
            result.error["code"] = request.error_code;
            result.error["message"] = request.error_message;
            result.error["tcode"] = tcode;
            result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start);
            return result;
        }
        const std::string& expected_tcode = request.expected_tcode;

        // 1. Snapshot previous statusbar state to guard against stale messages
        const ActionStatus prev_status = read_action_status(session);
        const std::string prev_sbar_text = prev_status.text;
        const std::string prev_sbar_type = prev_status.type;

        if (request.use_send_command) {
            // StartTransaction prepends "/n" itself, so "/nXYZ" must go via SendCommand.
            session->invoke_method_with_string(L"SendCommand", request.command);
        } else {
            session->start_transaction(request.command);
        }

        // Wait briefly for transaction to start
        session->wait_for_completion(500);

        // SAP can set Transaction to the requested code while reporting that
        // startup failed in an information dialog instead of the status bar.
        // The active window is fetched once here and reused for the post-action status read.
        std::string active_window_id;
        bool window_known = false;
        try {
            auto window = session->get_active_window();
            if (window) {
                active_window_id = window->get_id();
                window_known = true;
            }
            if (window && active_window_id.find("wnd[1]") != std::string::npos) {
                auto message = session->find_element_by_id("wnd[1]/usr/txtMESSTXT1");
                if (message) {
                    if (auto rejection = classify_transaction_modal(active_window_id,
                                                                     message->get_text(), expected_tcode)) {
                        rejection->duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::high_resolution_clock::now() - start);
                        return *rejection;
                    }
                }
            }
        } catch (const ComException&) {
            // Other transaction dialogs do not necessarily have a message field.
        }

        // 2. Check for error or abort messages on statusbar
        const ActionStatus post_status = window_known ? read_action_status(session, active_window_id)
                                                      : read_action_status(session);
        const std::string post_sbar_type = post_status.type;
        const std::string post_sbar_text = post_status.text;
        if (!post_sbar_text.empty()) {
            const bool is_new_message = (post_sbar_text != prev_sbar_text || post_sbar_type != prev_sbar_type);
            if (post_sbar_type == "E" || post_sbar_type == "A") {
                if (is_new_message) {
                    result.status = Result::Status::Error;
                    result.error["code"] = (post_sbar_type == "A") ? "TRANSACTION_ABORTED" : "TRANSACTION_FAILED";
                    result.error["message"] = post_sbar_text;
                    result.error["tcode"] = tcode;
                    result.error["message_type"] = post_sbar_type;
                    attach_status_bar(result, prev_status, post_status);

                    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::high_resolution_clock::now() - start);
                    spdlog::warn("Transaction {} failed with statusbar error [{}]: {}", tcode, post_sbar_type, post_sbar_text);
                    return result;
                }
            } else if (is_new_message && post_sbar_type == "W") {
                attach_status_message(result.data, prev_status, post_status);
            } else if (is_new_message) {
                result.data["statusbar"] = post_sbar_text;
            }
        }

        // 3. Record actual active transaction code
        std::string actual_tcode = session->get_transaction_code();
        auto same_tcode = [](std::string left, std::string right) {
            std::transform(left.begin(), left.end(), left.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            std::transform(right.begin(), right.end(), right.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            return left == right;
        };
        // Bare "/n" returns to the Easy Access menu; the reported code varies.
        const bool bare_n = request.use_send_command && request.command == "/n";
        if (!bare_n && !same_tcode(actual_tcode, expected_tcode)) {
            // A slow screen can briefly report the old or an empty transaction
            // after COM returns. Wait for the requested transaction itself.
            for (int attempt = 0; attempt < 10; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                actual_tcode = session->get_transaction_code();
                if (same_tcode(actual_tcode, expected_tcode)) break;
            }
            if (!same_tcode(actual_tcode, expected_tcode)) {
                result.status = Result::Status::Error;
                result.error["code"] = "TRANSACTION_NOT_STARTED";
                result.error["message"] = "Requested transaction did not become active";
                attach_status_bar(result, prev_status, post_status);
                if (!post_sbar_text.empty() && (post_sbar_type == "E" || post_sbar_type == "A")) {
                    // The same message may be a repeated rejection or a stale
                    // rejection from an earlier command. Preserve it without
                    // claiming that this attempt produced a new SAP error.
                    result.error["statusbar_message"] = post_sbar_text;
                    result.error["message_type"] = post_sbar_type;
                    result.error["statusbar_unchanged"] =
                        post_sbar_text == prev_sbar_text && post_sbar_type == prev_sbar_type;
                }
                result.error["tcode"] = tcode;
                result.error["actual_tcode"] = actual_tcode;
                result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - start);
                return result;
            }
        }
        result.data["actual_tcode"] = actual_tcode;

        result.status = Result::Status::Success;
        result.data["tcode"] = tcode;
        result.data["message"] = "Transaction started";
        attach_status_bar(result, prev_status, post_status);

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Executed transaction {} (duration: {}ms)", tcode, result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "TRANSACTION_FAILED";
        result.error["message"] = e.what();
        result.error["tcode"] = tcode;
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start);
        spdlog::error("Transaction {} failed: {}", tcode, e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start);
    }

    return result;
}

ScreenSnapshot ComAutomationEngine::capture_screen_snapshot() const {
    ScreenSnapshot snapshot;
    try {
        auto session = current_session_;
        if (!session) return snapshot;
        bool window_known = false;
        if (auto window = session->get_active_window()) {
            snapshot.window_id = window->get_id();
            window_known = true;
            try { snapshot.title = window->get_title(); } catch (const std::exception&) {}
        }
        try { snapshot.transaction = session->get_transaction_code(); } catch (const std::exception&) {}
        snapshot.statusbar_text = (window_known ? read_action_status(session, snapshot.window_id)
                                                : read_action_status(session)).text;
    } catch (const std::exception&) {
        // A partially readable snapshot is still useful for change detection.
    }
    return snapshot;
}

Result ComAutomationEngine::click_element(const ElementId& element) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!element.is_valid()) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid element ID format";
            result.error["element"] = element.path;
            return result;
        }

        // Resolve @active to actual window ID
        ElementId resolved_element = resolve_element_path(element);
        auto session = ensure_session();
        auto elem = find_element_if_present(session, resolved_element.path);

        if (!elem) {
            if (auto inactive = inactive_tab_error(session, resolved_element.path, false)) return *inactive;
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "Element not found at path: " + resolved_element.path;
            result.error["element"] = resolved_element.path;

            // Check if window mismatch (element path specifies different window than active)
            WindowId requested_window = resolved_element.get_window();
            WindowId active_window = get_active_window_id();
            bool window_mismatch = !window_ids_match(requested_window, active_window);

            json suggestions = json::array({
                "Verify element path is correct",
                "Run 'screen read' to see available elements",
                "Check if screen is fully loaded"
            });

            if (window_mismatch) {
                result.error["window_mismatch"] = true;
                result.error["requested_window"] = requested_window.id;
                result.error["active_window"] = active_window.id;
                suggestions.push_back(
                    "Active window is " + active_window.id + ", not " + requested_window.id + " - did a popup open?"
                );
                suggestions.push_back(
                    "Try: fairyfly element click " + active_window.id + "/" + resolved_element.get_element_path()
                );
                suggestions.push_back(
                    "Or use: fairyfly element click @active/" + resolved_element.get_element_path()
                );
            }

            result.error["suggestions"] = suggestions;
            spdlog::warn("Element not found: {}|window_mismatch={}", resolved_element.path, window_mismatch);
            return result;
        }

        if (!elem->is_enabled()) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_DISABLED";
            result.error["message"] = "Element is disabled and cannot be clicked";
            result.error["element"] = resolved_element.path;
            return result;
        }

        const auto before_status = read_action_status(session);

        // ABAP list hotspots and modal list-picker rows expose only SetFocus,
        // not Press. Focusing the label and sending F2 activates the row.
        const int target_window_index = WindowId(resolved_element.path).get_index();
        const std::string elem_type = elem->get_type();
        const bool is_hotspot = elem_type == "GuiLabel" &&
            elem->get_property_bool(L"IsHotspot");
        const bool is_list_element = elem_type == "GuiLabel" &&
            elem->get_property_bool(L"IsListElement");
        const bool activate_list_label = should_activate_positioned_label(
            elem_type, resolved_element.path, target_window_index, is_hotspot, is_list_element);
        std::string list_label_text_before;
        std::string list_window_before;
        std::string list_title_before;
        if (activate_list_label) {
            auto window = session->get_active_window();
            if (!window) throw ComException("No active window for list selection");
            if (WindowId(window->get_id()).get_index() != target_window_index) {
                result.status = Result::Status::Error;
                result.error["code"] = "WINDOW_MISMATCH";
                result.error["message"] = "List row is not in the active popup";
                result.error["element"] = resolved_element.path;
                return result;
            }
            list_label_text_before = elem->get_text();
            list_window_before = window->get_id();
            list_title_before = window->get_title();
            elem->set_focus();
            window->send_vkey(2);
        } else if (elem_type == "GuiLabel") {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_CLICKABLE";
            result.error["message"] = "Label is not an interactive hotspot";
            result.error["element"] = resolved_element.path;
            return result;
        } else {
            elem->press();
        }
        session->wait_for_completion(500);

        const auto after_status = read_action_status(session);
        if (auto rejection = classify_action_status(before_status, after_status, resolved_element.path,
                elem_type == "GuiButton" || activate_list_label)) {
            attach_status_bar(*rejection, before_status, after_status);
            rejection->duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start);
            return *rejection;
        }
        if (activate_list_label) {
            bool same_label = true;
            bool same_window = true;
            bool same_title = true;
            try {
                auto refreshed = session->find_element_by_id(resolved_element.path);
                same_label = refreshed && refreshed->get_text() == list_label_text_before;
            } catch (const std::exception&) {
                // If the screen cannot be checked, do not claim a verified action.
            }
            try {
                auto current_window = session->get_active_window();
                same_window = current_window && current_window->get_id() == list_window_before;
                same_title = current_window && current_window->get_title() == list_title_before;
            } catch (const std::exception&) {
                // Treat an unreadable post-action window as unverified.
            }
            if (auto unchanged = classify_list_label_outcome(
                    same_label, same_window, same_title, before_status, after_status,
                    resolved_element.path)) {
                attach_status_bar(*unchanged, before_status, after_status);
                unchanged->duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - start);
                return *unchanged;
            }
        }
        result.status = Result::Status::Success;
        result.data["element"] = resolved_element.path;
        if (element.path != resolved_element.path) {
            result.data["element_requested"] = element.path;  // Show original @active path
        }
        result.data["action"] = "click";
        result.data["element_type"] = elem->get_type();
        if (elem_type == "GuiCheckBox" || elem_type == "GuiRadioButton") {
            if (const auto selected = elem->get_selected()) result.data["selected"] = *selected;
        }
        result.data["window"] = resolved_element.get_window().id;
        attach_status_bar(result, before_status, after_status);

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Clicked element {} (duration: {}ms)", resolved_element.path, result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        result.error["element"] = element.path;
        spdlog::error("Click failed for {}: {}", element.path, e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

Result ComAutomationEngine::press_f4(const ElementId& element) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!element.is_valid()) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid element ID format";
            result.error["element"] = element.path;
            return result;
        }

        // Resolve @active to actual window ID
        ElementId resolved_element = resolve_element_path(element);
        auto session = ensure_session();
        auto elem = find_element_if_present(session, resolved_element.path);

        if (!elem) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "Element not found at path: " + resolved_element.path;
            result.error["element"] = resolved_element.path;
            result.error["suggestions"] = json::array({
                "Verify element path is correct",
                "Run 'screen read' to see available elements",
                "Check if screen is fully loaded"
            });
            spdlog::warn("Element not found: {}", resolved_element.path);
            return result;
        }

        // Check if element type supports F4 (GuiCTextField)
        std::string elem_type = elem->get_type();
        if (elem_type != "GuiCTextField") {
            result.status = Result::Status::Error;
            result.error["code"] = "F4_NOT_SUPPORTED";
            result.error["message"] = "Element does not support F4 search help";
            result.error["element"] = resolved_element.path;
            result.error["element_type"] = elem_type;
            result.error["suggestions"] = json::array({
                "F4 search help is only available for GuiCTextField elements",
                "This element is of type " + elem_type,
                "Run 'screen read' to find fields with F4 help (marked with (F4 Search))"
            });
            spdlog::warn("F4 not supported for element type: {}", elem_type);
            return result;
        }

        if (!elem->is_enabled()) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_DISABLED";
            result.error["message"] = "Element is disabled and F4 cannot be pressed";
            result.error["element"] = resolved_element.path;
            return result;
        }

        auto window = session->get_active_window();
        if (!window) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_ACTIVE_WINDOW";
            result.error["message"] = "Could not get active window for sending F4";
            return result;
        }
        if (auto mismatch = classify_f4_target_window(window->get_id(), resolved_element.path)) {
            return *mismatch;
        }
        const int active_window_index = WindowId(window->get_id()).get_index();

        // Step 1: Set focus on the element
        elem->press_f4();  // Now sets focus instead of calling PressF4

        // Step 2: Get the active window and send F4 virtual key (VKey 4)
        window->send_vkey(4);  // F4 = VKey 4

        // Step 3: Wait for a new modal after the active window, not an existing modal.
        auto f4_dialog = session->wait_for_element(f4_dialog_path(active_window_index), 2000);

        if (!f4_dialog) {
            auto outcome = classify_f4_outcome(false, read_action_status(session), resolved_element.path);
            result = *outcome;
            auto end = std::chrono::high_resolution_clock::now();
            result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            spdlog::warn("F4 dialog did not open for {}: {}", resolved_element.path,
                         result.error.value("message", ""));
            return result;
        }

        result.status = Result::Status::Success;
        result.data["element"] = resolved_element.path;
        if (element.path != resolved_element.path) {
            result.data["element_requested"] = element.path;  // Show original @active path
        }
        result.data["action"] = "press_f4";
        result.data["element_type"] = elem_type;
        result.data["window"] = resolved_element.get_window().id;
        result.data["f4_dialog_opened"] = (f4_dialog != nullptr);

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Pressed F4 for element {} (duration: {}ms)", resolved_element.path, result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        result.error["element"] = element.path;
        spdlog::error("Press F4 failed for {}: {}", element.path, e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

Result ComAutomationEngine::press_toolbar_button(const ElementId& toolbar_element, const std::string& button_id) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!toolbar_element.is_valid()) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid toolbar element ID format";
            result.error["element"] = toolbar_element.path;
            return result;
        }

        // Resolve @active to actual window ID
        ElementId resolved_element = resolve_element_path(toolbar_element);
        auto session = ensure_session();
        auto elem = session->find_element_by_id(resolved_element.path);

        if (!elem) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "Toolbar element not found at path: " + resolved_element.path;
            result.error["element"] = resolved_element.path;
            result.error["suggestions"] = json::array({
                "Verify toolbar path is correct",
                "Run 'screen read' to see available elements",
                "Toolbar must be a GuiShell element with SubType='Toolbar'"
            });
            spdlog::warn("Toolbar element not found: {}", resolved_element.path);
            return result;
        }

        // Verify element is a GuiShell toolbar
        std::string elem_type = elem->get_type();
        if (elem_type != "GuiShell") {
            result.status = Result::Status::Error;
            result.error["code"] = "WRONG_ELEMENT_TYPE";
            result.error["message"] = "Element is not a GuiShell toolbar (type: " + elem_type + ")";
            result.error["element"] = resolved_element.path;
            result.error["element_type"] = elem_type;
            return result;
        }

        const auto before_status = read_action_status(session);

        // Press the toolbar button
        elem->press_button(button_id);
        session->wait_for_completion(500);
        const auto after_status = read_action_status(session);
        if (auto rejection = classify_action_status(before_status, after_status,
                                             resolved_element.path + "/btn_" + button_id, true)) {
            attach_status_bar(*rejection, before_status, after_status);
            rejection->duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start);
            return *rejection;
        }

        result.status = Result::Status::Success;
        result.data["toolbar"] = resolved_element.path;
        result.data["button_id"] = button_id;
        result.data["action"] = "press_button";
        if (toolbar_element.path != resolved_element.path) {
            result.data["toolbar_requested"] = toolbar_element.path;  // Show original @active path
        }
        attach_status_bar(result, before_status, after_status);

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Pressed toolbar button {} on {} (duration: {}ms)",
                    button_id, resolved_element.path, result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        result.error["toolbar"] = toolbar_element.path;
        result.error["button_id"] = button_id;
        spdlog::error("Press toolbar button failed for {} / {}: {}", toolbar_element.path, button_id, e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

Result ComAutomationEngine::fill_field(const ElementId& element, const std::string& value) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!element.is_valid()) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid element ID format";
            return result;
        }

        // Resolve @active to actual window ID
        ElementId resolved_element = resolve_element_path(element);
        auto session = ensure_session();
        auto elem = find_element_if_present(session, resolved_element.path);

        if (!elem) {
            if (auto inactive = inactive_tab_error(session, resolved_element.path, false)) return *inactive;
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "Element not found at path: " + resolved_element.path;
            result.error["element"] = resolved_element.path;

            // Check if window mismatch
            WindowId requested_window = resolved_element.get_window();
            WindowId active_window = get_active_window_id();
            bool window_mismatch = !window_ids_match(requested_window, active_window);

            json suggestions = json::array({
                "Verify element path is correct",
                "Run 'screen read' to see available elements",
                "Check if screen is fully loaded"
            });

            if (window_mismatch) {
                result.error["window_mismatch"] = true;
                result.error["requested_window"] = requested_window.id;
                result.error["active_window"] = active_window.id;
                suggestions.push_back(
                    "Active window is " + active_window.id + ", not " + requested_window.id + " - did a popup open?"
                );
                suggestions.push_back(
                    "Try: fairyfly element fill " + active_window.id + "/" + resolved_element.get_element_path() + " \"<value>\""
                );
                suggestions.push_back(
                    "Or use: fairyfly element fill @active/" + resolved_element.get_element_path() + " \"<value>\""
                );
            }

            result.error["suggestions"] = suggestions;
            spdlog::warn("Element not found: {}|window_mismatch={}", resolved_element.path, window_mismatch);
            return result;
        }

        if (!elem->is_enabled()) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_DISABLED";
            result.error["message"] = "Element is disabled";
            result.error["element"] = resolved_element.path;
            return result;
        }

        // Selection-input mode: validate the LIVE control and the live screen right before the write (the dispatcher's
        // decision used the element id and the screen read before the call). Nothing is read from the field first.
        if (selection_input_policy_.active) {
            sap::SelectionInputProbe live;
            live.id = resolved_element.path;
            // Strict reads: a failed read of Type/Id/Name/Changeable/label/tooltip refuses (it must not look like an empty value).
            const ControlPropertyReader reader = [&](const std::string& property) -> PropertyRead {
                const auto dot = property.find('.');
                const std::wstring first = std::wstring(property.begin(), property.begin() + (dot == std::string::npos ? property.size() : dot));
                if (dot != std::string::npos) {
                    const std::wstring second(property.begin() + dot + 1, property.end());
                    return elem->read_object_text_strict(first.c_str(), second.c_str());
                }
                if (property == "Changeable") return elem->read_bool_property_strict(first.c_str());
                return elem->read_string_property_strict(first.c_str());
            };
            if (auto refusal = collect_selection_probe(reader, live)) {
                result.status = Result::Status::Error;
                result.error["code"] = refusal->code;
                result.error["message"] = refusal->message;
                result.error["element"] = resolved_element.path;
                return result;
            }
            const auto screen = read_facts_bounded(
                [&] { return session->is_busy(); },
                [&] {
                    audit::SapFacts read;
                    read.program = session->get_program();
                    read.screen_number = session->get_screen_number();
                    return read;
                },
                FactsBudget{std::chrono::milliseconds(2000), std::chrono::milliseconds(100)});
            live.program = screen.program;
            live.screen_number = screen.screen_number;
            if (auto refusal = evaluate_selection_input(live, selection_input_policy_)) {
                result.status = Result::Status::Error;
                result.error["code"] = refusal->code;
                result.error["message"] = refusal->message;
                result.error["element"] = resolved_element.path;
                return result;
            }
        }

        // Probe the field first: the credential decision needs type, id and label before any text is read.
        FieldProbe probe;
        probe.type = elem->get_type();
        probe.id = resolved_element.path;
        try { probe.name = elem->get_name(); } catch (const std::exception&) {}
        try { probe.label = elem->get_label(); } catch (const std::exception&) {}
        const bool credential_field =
            !sensitive_input_field_reason(probe.type, probe.id, probe.label).empty();
        if (!credential_field) {
            try { probe.text_before = elem->get_text(); } catch (const std::exception&) {}
            try { probe.max_length = elem->get_property_int(L"MaxLength"); } catch (const std::exception&) {}
            try { probe.numerical = elem->get_property_bool(L"Numerical"); } catch (const std::exception&) {}
            try { probe.required = elem->get_property_bool(L"Required"); } catch (const std::exception&) {}
        }

        // Set the text. SetText is local to the GUI front end (no server round trip until Enter), so a status bar
        // read right after it still shows the previous action's message: report it only when it changed.
        const auto before_status = read_action_status(session);
        const auto outcome = elem->fill_value(value);
        const auto after_status = read_action_status(session);
        if (outcome.status == ComGuiElement::FillOutcome::Status::InvalidArgument) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ARGUMENT";
            result.error["message"] = outcome.message;
            result.error["element"] = resolved_element.path;
            return result;
        }
        if (outcome.status == ComGuiElement::FillOutcome::Status::ReadOnly) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_READ_ONLY";
            result.error["message"] = "Element is not changeable on the current SAP screen";
            result.error["element"] = resolved_element.path;
            attach_fresh_status_bar(result, before_status, after_status);
            return result;
        }
        if (!credential_field) {
            try {
                probe.text_after = elem->get_text();
                probe.text_after_known = true;
            } catch (const std::exception&) { /* echo the typed value instead */ }
        }

        result.status = Result::Status::Success;
        result.data["element"] = resolved_element.path;
        if (element.path != resolved_element.path) {
            result.data["element_requested"] = element.path;  // Show original @active path
        }
        bool value_redacted = false;
        result.data["value"] = fill_value_echo(probe, value, value_redacted);
        if (value_redacted) result.data["value_redacted"] = true;
        result.data["action"] = "fill";
        result.data["window"] = resolved_element.get_window().id;
        result.data["field"] = build_fill_field_info(probe, value);
        if (outcome.selected) result.data["selected"] = *outcome.selected;
        if (outcome.key) result.data["key"] = *outcome.key;
        if (outcome.display_value) result.data["display_value"] = *outcome.display_value;
        attach_fresh_status_bar(result, before_status, after_status);

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Filled field {} (duration: {}ms)", resolved_element.path, result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        spdlog::error("Fill failed for {}: {}", element.path, e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

Result ComAutomationEngine::fill_grid_cell(const ElementId& element, int row,
                                            const std::string& column, const std::string& value,
                                            bool checkbox, bool commit) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;
    try {
        ElementId resolved = resolve_element_path(element);
        auto session = ensure_session();
        auto grid = session->find_element_by_id(resolved.path);
        if (!grid) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "GridView not found: " + resolved.path;
            return result;
        }
        if (grid->get_type() != "GuiShell" && grid->get_type() != "GuiGridView") {
            result.status = Result::Status::Error;
            result.error["code"] = "WRONG_ELEMENT_TYPE";
            result.error["message"] = "Element is not a GridView";
            return result;
        }
        const auto before_status = read_action_status(session);
        grid->modify_grid_cell(row, column, value, checkbox, commit);
        const auto after_status = read_action_status(session);
        result.status = Result::Status::Success;
        result.data["action"] = "modify_grid_cell";
        result.data["element"] = resolved.path;
        result.data["row"] = row;
        result.data["column"] = column;
        result.data["value"] = "[REDACTED]";
        result.data["checkbox"] = checkbox;
        result.data["committed"] = commit;
        attach_status_bar(result, before_status, after_status);
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start);
    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }
    return result;
}

Result ComAutomationEngine::select_grid_row(const ElementId& element, int row,
                                             const std::string& column) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;
    try {
        ElementId resolved = resolve_element_path(element);
        auto session = ensure_session();
        auto grid = session->find_element_by_id(resolved.path);
        if (!grid) {
            result.status = Result::Status::Error;
            result.error["code"] = "ELEMENT_NOT_FOUND";
            result.error["message"] = "GridView not found: " + resolved.path;
            return result;
        }
        const std::string type = grid->get_type();
        if (type != "GuiGridView" &&
            (type != "GuiShell" || grid->get_string_property(L"SubType") != "GridView")) {
            result.status = Result::Status::Error;
            result.error["code"] = "WRONG_ELEMENT_TYPE";
            result.error["message"] = "Element is not a GridView";
            return result;
        }
        const auto before_status = read_action_status(session);
        grid->select_grid_row(row, column);
        const auto after_status = read_action_status(session);
        result.status = Result::Status::Success;
        result.data["action"] = "select_grid_row";
        result.data["element"] = resolved.path;
        result.data["row"] = row;
        result.data["column"] = column;
        attach_status_bar(result, before_status, after_status);
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start);
    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }
    return result;
}

json read_element_value(const ComGuiElementPtr& element) {
    json data = json::object();
    const auto type = element->get_type();
    if (type == "GuiShell" && element->get_subtype() == "AbapEditor") {
        const auto content = element->get_abap_editor_content(constants::MAX_REQUESTED_TABLE_ROWS);
        data["value"] = redact_sensitive_abap_source(content.text);
        data["element_subtype"] = "AbapEditor";
        data["source_total_lines"] = content.total_lines;
        data["source_lines_read"] = content.lines_read;
        data["source_truncated"] = content.truncated;
    } else if (type == "GuiShell" && element->get_subtype() == "HTMLViewer") {
        const auto page = read_html_viewer_text(element->get_property_int(L"Handle"));
        data["value"] = redact_sensitive_response_text(page.text);
        data["element_subtype"] = "HTMLViewer";
        data["content_available"] = !page.text.empty();
        if (page.truncated) data["content_truncated"] = true;
    } else {
        data["value"] = element->get_text_for_direct_read();
        if (type == "GuiCheckBox" || type == "GuiRadioButton") {
            try { data["selected"] = element->get_property_bool(L"Selected"); }
            catch (const std::exception&) { /* State unavailable; keep value only. */ }
            try {
                const auto label = element->get_label();
                if (!label.empty()) data["label"] = label;
                else if (data.contains("value") && data["value"].is_string() &&
                         !data["value"].get<std::string>().empty()) {
                    data["label"] = data["value"];  // caption is the label
                }
            } catch (const std::exception&) { /* Optional label. */ }
        }
    }
    return data;
}

Result ComAutomationEngine::read_field(const ElementId& element) {
    return read_field(element, false);
}

Result ComAutomationEngine::read_field(const ElementId& element, bool activate_tab) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    // Tabs activated for the read are put back on EVERY exit path (success, error result, exception): the guard records the
    // previous tab before each selection and restores in its destructor if the code below unwinds.
    ComGuiSessionPtr tab_session;
    TabActivationGuard tab_guard([&tab_session](const std::string& page_id) {
        if (!tab_session) return false;
        auto page = find_element_if_present(tab_session, page_id);
        if (!page) return false;
        page->select();
        tab_session->wait_for_completion(2000);
        return true;
    });

    try {
        if (!element.is_valid()) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid element ID format";
            return result;
        }

        // Resolve @active to actual window ID
        ElementId resolved_element = resolve_element_path(element);
        auto session = ensure_session();
        tab_session = session;
        auto elem = find_element_if_present(session, resolved_element.path);

        // The content of an inactive tab page does not exist for the scripting API: tell that apart from a bad id.
        if (!elem) {
            if (!activate_tab) {
                if (auto inactive = inactive_tab_error(session, resolved_element.path, true)) return *inactive;
            } else {
                const auto lookup = make_tab_lookup(session);
                for (int round = 0; round < 4; ++round) {  // nested tab strips: outermost page first
                    auto inactive = find_inactive_tab_page(resolved_element.path, lookup);
                    if (!inactive) break;
                    ActivatedTab entry{inactive->first.page_id, inactive->second.text, ""};
                    try {
                        auto strip = find_element_if_present(session, inactive->first.strip_id);
                        if (strip) {
                            auto selected = strip->get_dispatch_property(L"SelectedTab");
                            if (selected) entry.previous_id = ComGuiElement::create(selected)->get_id();
                        }
                    } catch (const std::exception&) {}
                    auto page = find_element_if_present(session, inactive->first.page_id);
                    if (!page) break;
                    tab_guard.record(std::move(entry));  // recorded BEFORE selecting: a failing select is restored too
                    page->select();
                    session->wait_for_completion(2000);
                }
                elem = find_element_if_present(session, resolved_element.path);
            }
        }
        if (!elem) throw ComException("Element not found: " + resolved_element.path);

        const std::string element_type = elem->get_type();
        result.data = read_element_value(elem);
        result.status = Result::Status::Success;
        result.data["element"] = resolved_element.path;
        if (element.get_window().is_active_selector()) {
            result.data["element_requested"] = element.path;  // Show original @active path
        }
        result.data["element_type"] = element_type;
        result.data["window"] = resolved_element.get_window().id;
        if (type_shows_tooltip(element_type)) {
            std::string tooltip, text;
            try { tooltip = elem->get_tooltip(); } catch (const std::exception&) {}
            if (!tooltip.empty()) {
                try { text = elem->get_text(); } catch (const std::exception&) {}
                attach_tooltip_fields(result.data, element_type, tooltip, text);
            }
        }
        attach_status_bar(result, read_action_status(session));

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Read field {} (duration: {}ms)", resolved_element.path, result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = error_code_for_com_message(e.what());
        result.error["message"] = e.what();
        spdlog::error("Read failed for {}: {}", element.path, e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    // A read is observational: put the tabs back as they were (innermost first), whatever the read returned.
    if (!tab_guard.entries().empty()) {
        tab_guard.restore();
        json tabs = json::array();
        for (const auto& entry : tab_guard.entries()) tabs.push_back({{"tab_id", entry.tab_id}, {"tab_text", entry.tab_text}});
        json& target = result.status == Result::Status::Success ? result.data : result.error;
        target["tabs_activated"] = tabs;
        target["tabs_restored"] = tab_guard.restored();
        if (!tab_guard.restored()) target["restore_error"] = tab_guard.restore_error();
    }

    return result;
}

// Note: Element type checking logic moved to ElementTypeRegistry
// This function remains as a thin wrapper during migration
// Cache for tree/grid extraction results to avoid duplicate extractions
// Key: element ID, Value: extracted table/tree data JSON
static std::map<std::string, json> tree_grid_cache;

// Helper: Extract rich metadata from element
static json extract_element_metadata(ComGuiElementPtr elem, int depth = 0) {
    if (!elem) return nullptr;

    try {
        json metadata;
        std::string elem_id = elem->get_id();
        metadata["id"] = elem_id;
        std::string type = elem->get_type();
        metadata["type"] = type;
        metadata["name"] = elem->get_name();

        // Text content (available on most elements)
        std::string text = type == "GuiPasswordField"
            ? redaction_marker(redaction_reason::password_field) : elem->get_text();
        metadata["text"] = text;

        // Interactive states - query for all elements (is_enabled handles missing property gracefully)
        bool enabled = elem->is_enabled();
        bool visible = elem->is_visible();
        bool changeable = elem->is_changeable();

        metadata["enabled"] = enabled;
        metadata["visible"] = visible;
        metadata["changeable"] = changeable;

        // Accessibility labels and tooltips - not available on non-visual containers
        if (!is_non_visual_container(type)) {
            std::string label = elem->get_label();
            std::string tooltip = elem->get_tooltip();

            if (!label.empty()) metadata["label"] = label;
            if (!tooltip.empty()) metadata["tooltip"] = tooltip;
        }

        // Container classification
        std::string container_type = elem->get_container_type();
        if (!container_type.empty()) {
            metadata["container_type"] = container_type;
        }

        // Parse grid coordinates for grid-positioned labels (pattern: lbl[row,col])
        if (type == "GuiLabel" && elem_id.find("/lbl[") != std::string::npos) {
            size_t bracket_pos = elem_id.find("/lbl[");
            size_t comma_pos = elem_id.find(",", bracket_pos);
            size_t close_bracket = elem_id.find("]", comma_pos);

            if (comma_pos != std::string::npos && close_bracket != std::string::npos) {
                try {
                    std::string row_str = elem_id.substr(bracket_pos + 5, comma_pos - bracket_pos - 5);
                    std::string col_str = elem_id.substr(comma_pos + 1, close_bracket - comma_pos - 1);

                    metadata["grid_row"] = std::stoi(row_str);
                    metadata["grid_col"] = std::stoi(col_str);
                } catch (const std::invalid_argument&) {
                    // Failed to parse coordinates, skip
                } catch (const std::out_of_range&) {
                    // Number out of range, skip
                }
            }
        }

        // SubType for GuiShell elements (GridView, Tree, Toolbar, etc.)
        if (type == "GuiShell") {
            std::string subtype = elem->get_subtype();
            if (!subtype.empty()) {
                metadata["subtype"] = subtype;
            } else {
                metadata["subtype"] = "N/A";
            }

            // Special handling for GuiShell Toolbar - enumerate buttons via ButtonCount API
            if (subtype == "Toolbar") {
                try {
                    int button_count = elem->get_button_count();
                    spdlog::debug("GuiShell toolbar has {} buttons", button_count);

                    json button_children = json::array();
                    for (int i = 0; i < button_count; ++i) {
                        try {
                            std::string btn_id = elem->get_button_id(i);
                            std::string btn_text = elem->get_button_text(i);
                            std::string btn_tooltip = elem->get_button_tooltip(i);
                            std::string btn_type = elem->get_button_type(i);
                            bool btn_enabled = elem->get_button_enabled(i);

                            // Skip separators
                            if (btn_type == "Separator") continue;

                            // Create synthetic button element
                            json btn_metadata;
                            btn_metadata["id"] = btn_id;
                            btn_metadata["type"] = "GuiButton";
                            btn_metadata["name"] = btn_id;
                            btn_metadata["text"] = btn_text;
                            btn_metadata["tooltip"] = btn_tooltip;
                            btn_metadata["enabled"] = btn_enabled;
                            btn_metadata["button_type"] = btn_type;
                            btn_metadata["changeable"] = true;
                            btn_metadata["visible"] = true;
                            btn_metadata["capabilities"] = json::array({"clickable"});

                            button_children.push_back(btn_metadata);
                        } catch (const std::exception& e) {
                            spdlog::warn("Failed to extract button {} from GuiShell toolbar: {}", i, e.what());
                        }
                    }

                    if (!button_children.empty()) {
                        metadata["children"] = button_children;
                        metadata["child_count"] = button_children.size();
                    }
                } catch (const std::exception& e) {
                    spdlog::debug("GuiShell toolbar button enumeration failed: {}", e.what());
                }
            }
        }

        // Special handling for GuiBox (grouping container)
        if (type == "GuiBox") {
            metadata["is_group"] = true;
        }

        // Derive capabilities
        metadata["capabilities"] = derive_capabilities(type, enabled, changeable);

        // Extract table/tree data for specialized controls
        // Also check if GuiShell contains a tree control based on text (ActiveX ProgID)
        bool is_tree_control = (type == "GuiTree" ||
                               (type == "GuiShell" && text.find("TableTreeControl") != std::string::npos) ||
                               (type == "GuiShell" && text.find("TreeControl") != std::string::npos));

        // Check if GuiShell has SubType=GridView
        std::string subtype = (type == "GuiShell") ? metadata.value("subtype", "") : "";
        bool is_gridview = (type == "GuiGridView" || (type == "GuiShell" && subtype == "GridView"));

        if (is_gridview || type == "GuiTableControl" || is_tree_control) {
            // Check cache first to avoid duplicate expensive extractions
            auto cache_it = tree_grid_cache.find(elem_id);
            if (cache_it != tree_grid_cache.end()) {
                // Reuse cached extraction results
                if (cache_it->second.contains("table_data")) {
                    metadata["table_data"] = cache_it->second["table_data"];
                }
                if (cache_it->second.contains("tree_data")) {
                    metadata["tree_data"] = cache_it->second["tree_data"];
                }
                spdlog::debug("Reused cached extraction data for element: {}", elem_id);
            } else {
                // Extract and cache the results
                try {
                    TableExtractionOptions options;
                    options.max_rows = constants::MAX_TABLE_ROWS;
                    options.max_tree_depth = constants::MAX_TREE_DEPTH;
                    options.include_headers = true;

                    TableDataExtractor extractor(options);
                    json cached_data;

                    if (is_gridview) {
                        auto grid_data = extractor.extract_grid_data(elem);
                        if (!grid_data.rows.empty() || !grid_data.columns.empty()) {
                            json table_json;
                            table_json["columns"] = grid_data.columns;
                            table_json["rows"] = grid_data.rows;
                            table_json["total_row_count"] = grid_data.total_row_count;
                            table_json["visible_row_count"] = grid_data.visible_row_count;
                            redact_sensitive_header_rows(table_json);
                            metadata["table_data"] = table_json;
                            cached_data["table_data"] = table_json;
                            spdlog::debug("Extracted grid data: {} rows × {} columns",
                                         grid_data.rows.size(), grid_data.columns.size());
                        }
                    } else if (type == "GuiTableControl") {
                        auto table_data = extractor.extract_table_data(elem);
                        if (!table_data.rows.empty() || !table_data.columns.empty() || table_data.total_row_count > 0) {
                            json table_json;
                            table_json["columns"] = table_data.columns;
                            table_json["rows"] = table_data.rows;
                            table_json["total_row_count"] = table_data.total_row_count;
                            table_json["visible_row_count"] = table_data.visible_row_count;
                            redact_sensitive_header_rows(table_json);
                            metadata["table_data"] = table_json;
                            cached_data["table_data"] = table_json;
                            spdlog::debug("Extracted table control data: {} rows × {} columns",
                                         table_data.rows.size(), table_data.columns.size());
                        }
                    } else if (is_tree_control) {
                        auto tree_data = extractor.extract_tree_data(elem);
                        if (!tree_data.nodes.empty()) {
                            json tree_json;
                            tree_json["columns"] = tree_data.columns;

                            // Convert tree nodes to JSON with depth limit to prevent stack overflow
                            json nodes_array = json::array();
                            const int MAX_TREE_DEPTH = 50;  // Prevent stack overflow on deeply nested trees
                            std::function<void(const TreeNode&, json&, int)> convert_node;
                            convert_node = [&](const TreeNode& node, json& node_json, int depth) {
                                if (depth > MAX_TREE_DEPTH) {
                                    spdlog::warn("Tree node conversion reached max depth {}, stopping", MAX_TREE_DEPTH);
                                    return;
                                }

                                node_json["text"] = node.text;
                                node_json["key"] = node.key;
                                node_json["level"] = node.level;
                                node_json["expanded"] = node.expanded;
                                if (!node.column_values.empty()) {
                                    node_json["column_values"] = node.column_values;
                                }
                                if (!node.children.empty()) {
                                    json children_array = json::array();
                                    for (const auto& child : node.children) {
                                        json child_json;
                                        convert_node(child, child_json, depth + 1);
                                        children_array.push_back(child_json);
                                    }
                                    node_json["children"] = children_array;
                                }
                            };

                            for (const auto& node : tree_data.nodes) {
                                json node_json;
                                convert_node(node, node_json, 0);  // Start at depth 0
                                nodes_array.push_back(node_json);
                            }

                            tree_json["nodes"] = nodes_array;
                            metadata["tree_data"] = tree_json;
                            cached_data["tree_data"] = tree_json;
                            spdlog::info("Extracted tree data: {} top-level nodes, {} columns",
                                        tree_data.nodes.size(), tree_data.columns.size());
                        }
                    }

                    // Cache the extraction results
                    if (!cached_data.empty()) {
                        tree_grid_cache[elem_id] = cached_data;
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to extract table/tree data for {}: {}", type, e.what());
                }
            }
        }

        // Extract text content from GuiShell with AbapEditor subtype (ABAP source code editor)
        if (type == "GuiShell" && subtype == "AbapEditor") {
            try {
                const auto content = elem->get_abap_editor_content(constants::MAX_REQUESTED_TABLE_ROWS);
                metadata["source_total_lines"] = content.total_lines;
                metadata["source_lines_read"] = content.lines_read;
                metadata["source_truncated"] = content.truncated;
                if (!content.text.empty()) {
                    metadata["text_content"] = redact_sensitive_abap_source(content.text);
                    spdlog::debug("Extracted AbapEditor text content ({} chars)", content.text.length());
                } else {
                    metadata["text_content"] = nullptr;  // Empty editor
                    spdlog::debug("AbapEditor is empty");
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to extract AbapEditor text: {}", e.what());
                metadata["text_content"] = nullptr;
            }
        }

        // Recursively process children (limit depth to MAX_ELEMENT_DEPTH levels to capture deeply nested tree controls)
        // SM59 tree structure: Window -> UserArea -> CustomControl -> ContainerShell -> SplitterShell -> ContainerShell[n] -> Tree
        // SEGW tree structure: Window -> UserArea -> ContainerShell -> SplitterShell -> ContainerShell -> ContainerShell -> SplitterShell -> ContainerShell[n] -> Tree
        // SEGW GridView is at depth 12: usr -> shellcont -> shell -> shellcont[1] -> shell -> shellcont[0] -> shell -> shellcont[0] -> shellcont -> shellcont -> shell -> shellcont[1] -> shell (GridView)
        if (depth < constants::MAX_ELEMENT_DEPTH) {
            int child_count = elem->get_child_count();

            // Special handling for GuiContainerShell - always try to enumerate children
            // even if get_child_count() returns 0, because SAP GUI sometimes reports 0
            // for containers that actually have shell controls
            bool force_enumerate = (type == "GuiContainerShell" || type == "GuiCustomControl" ||
                                   type == "GuiSplitterContainer" || type == "GuiContainerCtrl" ||
                                   type == "GuiSplitterShell" || type == "GuiDockShell");

            if (child_count > 0 || force_enumerate) {
                spdlog::debug("extract_element_metadata: Processing {} children of {} at depth {} (force={})",
                             child_count, elem->get_id(), depth, force_enumerate);
                json children = json::array();

                try {
                    // Use the .item(index) method which works with SAP GUI collections
                    auto children_collection = elem->children();

                    // For forced enumeration, try up to MAX_CHILDREN_TO_PROCESS items even if child_count is 0
                    int max_children = force_enumerate ?
                        (child_count > 0 ? (std::min)(child_count, constants::MAX_CHILDREN_TO_PROCESS) : constants::MAX_CHILDREN_TO_PROCESS) :
                        (std::min)(child_count, constants::MAX_CHILDREN_TO_PROCESS);

                    for (int i = 0; i < max_children; ++i) {
                        try {
                            ComGuiElementPtr child_ptr = children_collection.item(i);
                            if (!child_ptr) {
                                // If we're force-enumerating and hit null, stop trying
                                if (force_enumerate && child_count == 0) {
                                    spdlog::debug("Force enumerate: Reached end at index {}", i);
                                    break;
                                }
                                spdlog::debug("extract_element_metadata: child {} returned nullptr", i);
                                continue;
                            }

                            json child_data = extract_element_metadata(child_ptr, depth + 1);
                            if (!child_data.is_null()) {
                                children.push_back(child_data);
                            }
                        } catch (const SapGuiException& e) {
                            // Catch most specific exception first
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: SapGuiException at index {}, stopping: {}", i, e.what());
                                break;
                            }
                            spdlog::debug("extract_element_metadata: SapGuiException processing child {}: {}", i, e.what());
                        } catch (const ComException& e) {
                            // Catch COM-specific exception second
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: ComException at index {}, stopping: {}", i, e.what());
                                break;
                            }
                            spdlog::debug("extract_element_metadata: ComException processing child {}: {}", i, e.what());
                        } catch (const std::exception& e) {
                            // Catch general exception last (base class)
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: Exception at index {}, stopping: {}", i, e.what());
                                break;
                            }
                            spdlog::debug("extract_element_metadata: Exception processing child {}: {}", i, e.what());
                        }
                    }
                } catch (const std::exception& e) {
                    spdlog::debug("extract_element_metadata: Exception getting children collection: {}", e.what());
                }

                if (!children.empty()) {
                    metadata["children"] = children;
                    metadata["child_count"] = children.size();
                } else if (child_count > 0) {
                    spdlog::debug("extract_element_metadata: No children extracted for {} (had {} children)",
                                 elem->get_id(), child_count);
                }
            }
        }

        return metadata;
    } catch (const SapGuiException& e) {
        spdlog::debug("extract_element_metadata: SapGuiException extracting metadata: {}", e.what());
        return nullptr;
    } catch (const ComException& e) {
        spdlog::debug("extract_element_metadata: ComException extracting metadata: {}", e.what());
        return nullptr;
    } catch (const std::exception& e) {
        spdlog::debug("extract_element_metadata: std::exception extracting metadata: {}", e.what());
        return nullptr;
    }
}

Result ComAutomationEngine::read_screen(bool include_structure, bool skip_trees, int max_rows) {
        auto session = ensure_session();
    if (!screen_reader_) {
        initialize_services();
    }
    if (!screen_reader_) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "NO_SESSION";
        result.error["message"] = "Unable to initialize screen reader - no session";
    return result;
}
    screen_reader_->set_probe_all(probe_all_);
    screen_reader_->set_tree_reader_mode(parse_tree_reader_mode(tree_reader_));
    screen_reader_->set_row_offset(row_offset_);
    screen_reader_->set_grid_rows_needed(grid_rows_needed_);
    return screen_reader_->read(include_structure, skip_trees, max_rows);
        }

Result ComAutomationEngine::read_screen_with_tabs(bool skip_trees, int max_rows,
                                                   const std::string& only_tab) {
    if (!screen_reader_) {
        auto session = ensure_session();
        initialize_services();
    }
    if (!screen_reader_) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "NO_SESSION";
        result.error["message"] = "Unable to initialize screen reader - no session";
    return result;
}
    screen_reader_->set_probe_all(probe_all_);
    screen_reader_->set_tree_reader_mode(parse_tree_reader_mode(tree_reader_));
    screen_reader_->set_row_offset(row_offset_);
    screen_reader_->set_grid_rows_needed(grid_rows_needed_);
    return screen_reader_->read_with_tabs(skip_trees, max_rows, only_tab);
}

Result ComAutomationEngine::find_screen(const ScreenFindOptions& query) {
    if (!screen_reader_) {
        auto session = ensure_session();
        initialize_services();
    }
    if (!screen_reader_) {
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "NO_SESSION";
        result.error["message"] = "Unable to initialize screen reader - no session";
        return result;
    }
    screen_reader_->set_probe_all(probe_all_ || query.probe_all);
    return screen_reader_->find(query);
}

Result ComAutomationEngine::dump_object_tree(const std::string& id, const std::vector<std::string>& props) {
    Result result;
    try {
        auto session = ensure_session();
        std::string target = id;
        if (target.empty()) {
            auto window = session->get_active_window();
            if (!window) throw ComException("No active window");
            target = window->get_id();
        }
        result.data = diag::build_object_tree_diagnostic(
            target, props, [&session](const std::string& tree_id, const std::vector<std::string>& tree_props) {
                return session->get_object_tree(tree_id, tree_props);
            });
        result.status = Result::Status::Success;
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
    }
    return result;
}

Result ComAutomationEngine::capture_screenshot(const cli::ScreenshotOptions& options) {
    if (!screenshot_handler_) {
        auto session = ensure_session();
        initialize_services();
    }
    if (!screenshot_handler_) {
    Result result;
            result.status = Result::Status::Error;
            result.error["code"] = "NO_SESSION";
        result.error["message"] = "Unable to initialize screenshot handler - no session";
            return result;
        }
    return screenshot_handler_->capture(options);
}

nlohmann::json ComAutomationEngine::get_application_info() const
{
    json info;
    info["connections"] = json::array();
    info["total_connections"] = 0;
    info["total_sessions"] = 0;
    info["backend_scripting_disabled"] = false;
    info["connection_enumeration_errors"] = 0;
    info["session_enumeration_errors"] = 0;
    
    if (!app_) {
        info["error"] = "No SAP GUI application available. Is SAP Logon running?";
        return info;
    }
    
    try {
        int conn_count = app_->get_connection_count();
        info["total_connections"] = conn_count;
        
        json connections_array = json::array();
        
        for (int c = 0; c < conn_count; ++c) {
            json conn_obj;
            conn_obj["index"] = c;
            try {
                auto connection = app_->get_connection(c);
                if (!connection) throw std::runtime_error("Connection object unavailable");

                conn_obj["id"] = connection->get_id();
                conn_obj["description"] = connection->get_description();
                conn_obj["type"] = connection->get_type();
                conn_obj["type_as_number"] = connection->get_type_as_number();
                conn_obj["connection_string"] = connection->get_connection_string();
                try {
                    bool disabled = connection->get_bool_property(L"DisabledByServer");
                    conn_obj["backend_scripting_disabled"] = disabled;
                    if (disabled) info["backend_scripting_disabled"] = true;
                } catch (const std::exception&) {
                    conn_obj["backend_scripting_disabled"] = nullptr;
                }

                int sess_count = connection->get_session_count();
                json sessions_array = json::array();
                conn_obj["reported_session_count"] = sess_count;
                conn_obj["session_errors"] = json::array();

                for (int s = 0; s < sess_count; ++s) {
                    try {
                        auto session = connection->get_session(s);
                        if (!session) throw std::runtime_error("Session object unavailable");

                        json sess_obj;
                        sess_obj["index"] = s;
                        sess_obj["id"] = session->get_id();
                        sess_obj["name"] = session->get_name();
                        sess_obj["type"] = session->get_type();
                        sess_obj["type_as_number"] = session->get_type_as_number();
                        sess_obj["busy"] = session->is_busy();
                        sess_obj["alive"] = session->is_alive();
                        sess_obj["server_session_key_available"] =
                            !session->get_server_session_key().empty();
                        // The scripting API has no server clock; see server_clock.h for the top-level fallback.
                        sess_obj["server_time"] = nullptr;
                        sess_obj["server_time_source"] = "unavailable";

                        try {
                            auto active_window = session->get_active_window();
                            if (active_window) sess_obj["active_window_title"] = active_window->get_title();
                        } catch (const std::exception&) {
                            sess_obj["active_window_title"] = nullptr;
                        }

                        sessions_array.push_back(sess_obj);
                    } catch (const std::exception& e) {
                        conn_obj["session_errors"].push_back({{"index", s}, {"message", e.what()}});
                        info["session_enumeration_errors"] = info["session_enumeration_errors"].get<int>() + 1;
                    }
                }

                conn_obj["sessions"] = sessions_array;
                conn_obj["session_count"] = sessions_array.size();
                connections_array.push_back(conn_obj);
                info["total_sessions"] = info["total_sessions"].get<int>() + static_cast<int>(sessions_array.size());
            } catch (const std::exception& e) {
                conn_obj["error"] = e.what();
                conn_obj["sessions"] = json::array();
                conn_obj["session_count"] = 0;
                connections_array.push_back(conn_obj);
                info["connection_enumeration_errors"] = info["connection_enumeration_errors"].get<int>() + 1;
            }
        }
        
        info["connections"] = connections_array;
        // keep the json alive: iterating server_time_fields().items() would dangle (the temporary dies after the range-init)
        const json server_clock = server_time_fields();
        for (const auto& [key, value] : server_clock.items()) info[key] = value;
        info["server_time_summary"] = server_time_summary(info);

    } catch (const std::exception& e) {
        info["error"] = e.what();
    }

    return info;
}

WindowId ComAutomationEngine::get_active_window_id() const
{
    if (!current_session_) {
        return WindowId("wnd[0]");  // Default to main window if no session
    }

    try {
        auto active_window = current_session_->get_active_window();
        if (!active_window) {
            return WindowId("wnd[0]");
        }

        std::string window_id = active_window->get_id();
        spdlog::debug("get_active_window_id|window_id={}", window_id);
        return WindowId(window_id);
    } catch (const std::exception& e) {
        spdlog::warn("get_active_window_id|failed|error={}|defaulting_to_wnd[0]", e.what());
        return WindowId("wnd[0]");
    }
}

ElementId ComAutomationEngine::resolve_element_path(const ElementId& element) const
{
    if (!element.is_valid()) {
        return element;  // Return as-is if invalid
    }

    WindowId window = element.get_window();

    // Check if it's a special selector that needs resolution
    if (window.is_main_selector()) {
        // @main always resolves to wnd[0] (main window), regardless of active window
        WindowId main_window("wnd[0]");
        ElementId resolved = element.with_window(main_window);
        spdlog::debug("resolve_element_path|original={}|resolved={}|selector=@main",
                      element.path, resolved.path);
        return resolved;
    }

    if (window.is_active_selector()) {
        // @active resolves to current active window (may be modal dialog)
        WindowId active_window = get_active_window_id();
        ElementId resolved = element.with_window(active_window);
        spdlog::debug("resolve_element_path|original={}|resolved={}|selector=@active",
                      element.path, resolved.path);
        return resolved;
    }

    // Already has explicit window, no resolution needed
    return element;
}

nlohmann::json ComAutomationEngine::detect_modal_dialog() const
{
    nlohmann::json dialog_info;

    try {
        WindowId active_window = get_active_window_id();

        // If active window path contains wnd[0], no modal dialog is present
        // Handles both "wnd[0]" and "/app/con[X]/ses[0]/wnd[0]" formats
        if (active_window.id.find("wnd[0]") != std::string::npos) {
            return dialog_info;  // Empty json = no dialog
        }

        // Modal dialog detected - extract metadata
        dialog_info["window_id"] = active_window.id;
        dialog_info["is_modal_dialog"] = true;

        // Get the actual window object to extract details
        if (!current_session_) {
            return dialog_info;  // Return basic info if no session
        }

        auto window = current_session_->get_active_window();
        if (!window) {
            return dialog_info;
        }

        // Extract window properties
        try {
            dialog_info["title"] = window->get_title();  // Window title
        } catch (const std::exception& e) {
            spdlog::debug("Could not get dialog title: {}", e.what());
            dialog_info["title"] = "Unknown";
        }

        try {
            std::string window_type = window->get_type();
            dialog_info["type"] = window_type;
        } catch (const std::exception& e) {
            spdlog::debug("Could not get dialog type: {}", e.what());
            dialog_info["type"] = "Unknown";
        }

        spdlog::debug("Modal dialog detected: {} ({})",
                     active_window.id,
                     dialog_info.value("title", "Unknown"));

    } catch (const std::exception& e) {
        spdlog::warn("detect_modal_dialog|failed|error={}", e.what());
    }

    return dialog_info;
}


} // namespace sap
} // namespace fairyfly
