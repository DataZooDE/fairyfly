#include "include/mcp/session_worker_main.h"

#include <iostream>
#include <memory>
#include <optional>
#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

#include "include/cli_handler.h"
#include "include/mcp/dispatcher.h"
#include "include/mcp/session_worker_protocol.h"
#include "include/mcp/screen_guard.h"

namespace fairyfly::mcp {
namespace {

Result worker_error(const char* code, const char* message) {
    Result result;
    result.status = Result::Status::Error;
    result.error = {{"code", code}, {"message", message}};
    return result;
}

bool hard_read_only() {
    std::string value;
#ifdef _WIN32
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, "FAIRYFLY_READ_ONLY") == 0 && buffer) {
        value = buffer;
        std::free(buffer);
    }
#else
    if (const char* env = std::getenv("FAIRYFLY_READ_ONLY")) value = env;
#endif
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

} // namespace

int run_session_worker_stdio() {
    // stdout is exclusively the versioned IPC stream. No CLI diagnostics or
    // spdlog output may share it, even when command execution fails.
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(
        "mcp-session-worker", std::make_shared<spdlog::sinks::stderr_sink_mt>()));
    std::unique_ptr<cli::CommandHandler> handler;
    struct BoundSession {
        int connection;
        std::string identity;
        std::string owner;
    };
    std::optional<BoundSession> bound;
    return run_worker_loop(std::cin, std::cout, [&](const WorkerCall& call) -> Result {
        if (call.enumerate) {
            if (bound) return worker_error("WORKER_SESSION_MISMATCH", "worker is bound to a saved connection");
            if (!handler) {
                handler = std::make_unique<cli::CommandHandler>();
                handler->set_batch_mode(true);
                handler->set_owner_window_guard(true);
            }
            Result result = handler->handle_list_all();
            if (result.status != Result::Status::Success || !result.data.is_object() ||
                !result.data.contains("connections") || !result.data["connections"].is_array())
                return worker_error("OWNER_IDENTITY_UNKNOWN", "SAP sessions could not be enumerated");
            for (auto& connection : result.data["connections"]) {
                if (!connection.is_object() || !connection.contains("sessions") ||
                    !connection["sessions"].is_array()) continue;
                for (auto& session : connection["sessions"]) {
                    if (!session.is_object() || !session.contains("id") || !session["id"].is_string()) continue;
                    try {
                        const auto target = handler->peek_session_target("", session["id"].get<std::string>(), std::nullopt);
                        if (target.ambiguous || target.facts.system.empty() || target.facts.client.empty() ||
                            target.facts.user.empty()) continue;
                        session["verified_target"] = {{"system", target.facts.system + "/" + target.facts.client},
                                                      {"user", target.facts.user},
                                                      {"connection_name", target.connection_name}};
                    } catch (...) {}
                }
            }
            return result;
        }
        if (call.probe) {
            if (bound && bound->connection != call.connection)
                return worker_error("WORKER_SESSION_MISMATCH", "worker is bound to another saved connection");
            if (!handler) {
                handler = std::make_unique<cli::CommandHandler>();
                handler->set_batch_mode(true);
                handler->set_owner_window_guard(true);
            }
            const std::optional<int> selected = call.connection < 0 ? std::nullopt : std::optional<int>(call.connection);
            const audit::SapFacts facts = handler->audit_facts_for_connection(selected);
            if (!facts.connection_id || (selected && facts.connection_id != selected) ||
                facts.system.empty() || facts.client.empty() ||
                facts.user.empty() || facts.session_identity.empty())
                return worker_error("OWNER_SESSION_UNAVAILABLE", "the requested SAP session is unavailable");
            const auto target = handler->peek_session_target("", "", facts.connection_id);
            Result result;
            result.status = Result::Status::Success;
            result.data = {{"connection_id", *facts.connection_id}, {"session_identity", facts.session_identity},
                           {"system", facts.system}, {"client", facts.client}, {"user", facts.user},
                           {"transaction", facts.transaction}, {"program", facts.program},
                           {"screen_number", facts.screen_number}, {"connection_name", target.connection_name}};
            return result;
        }
        if (bound) {
            if (bound->connection != call.connection || bound->identity != call.session_identity ||
                bound->owner != call.owner_identity)
                return worker_error("WORKER_SESSION_MISMATCH", "worker is bound to another SAP session");
        }
        if (hard_read_only() && call.selection_input)
            return worker_error("READ_ONLY", "selection input is disabled by FAIRYFLY_READ_ONLY");
        if (!handler) {
            handler = std::make_unique<cli::CommandHandler>();
            handler->set_batch_mode(true);
            handler->set_owner_window_guard(true);
        }
        const audit::SapFacts live = handler->audit_facts_for_connection(call.connection);
        const std::string live_owner = live.system + "/" + live.client + "/" + live.user;
        if (live.connection_id != call.connection || live.session_identity != call.session_identity ||
            live_owner != call.owner_identity || live.system.empty() || live.client.empty() || live.user.empty())
            return worker_error("OWNER_SESSION_UNAVAILABLE", "the requested SAP session is unavailable");
        if (!call.expected_screen_guard.empty() && screen_guard_for(live) != call.expected_screen_guard)
            return worker_error("SCREEN_CHANGED", "the SAP session or dynpro changed since the client's screen read");
        if (!bound) bound = BoundSession{call.connection, call.session_identity, call.owner_identity};
        handler->set_read_only(hard_read_only() || (call.read_only && !call.selection_input));
        handler->set_selection_input_only(call.selection_input, call.selection_program, call.selection_screen);
        handler->set_expected_mcp_session(call.session_identity, call.owner_identity);
        struct ClearGuard {
            cli::CommandHandler& handler;
            ~ClearGuard() {
                handler.set_selection_input_only(false);
                handler.set_expected_mcp_session("", "");
                handler.set_read_only(true);
            }
        } clear{*handler};
        Result result = make_registry_invoker([&]() -> cli::CommandHandler& { return *handler; })(call.argv);
        return result;
    });
}

} // namespace fairyfly::mcp
