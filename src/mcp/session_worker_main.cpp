#include "include/mcp/session_worker_main.h"
#include "include/sap_identity.h"
#include "include/config/environment.h"

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

std::optional<Result> worker_binding_refusal(const WorkerCall& call, const std::optional<WorkerBinding>& bound,
                                             bool read_only_cap) {
    if (bound && (bound->connection != call.connection || bound->identity != call.session_identity ||
                  bound->owner != call.owner_identity))
        return Result::fail("WORKER_SESSION_MISMATCH", "worker is bound to another SAP session");
    if (read_only_cap && call.selection_input)
        return Result::fail("READ_ONLY", "selection input is disabled by FAIRYFLY_READ_ONLY");
    return std::nullopt;
}

std::optional<Result> worker_live_refusal(const WorkerCall& call, const audit::SapFacts& live) {
    const std::string live_owner = sap_identity(live);
    if (live.connection_id != call.connection || live.session_identity != call.session_identity ||
        live_owner != call.owner_identity || live.system.empty() || live.client.empty() || live.user.empty())
        return Result::fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP session is unavailable");
    if (!call.expected_screen_guard.empty() && screen_guard_for(live) != call.expected_screen_guard)
        return Result::fail("SCREEN_CHANGED", "the SAP session or dynpro changed since the client's screen read");
    return std::nullopt;
}

WorkerCallFlags::WorkerCallFlags(cli::CommandHandler& handler, const WorkerCall& call, bool read_only_cap) : handler_(handler) {
    handler_.set_read_only(read_only_cap || (call.read_only && !call.selection_input));
    handler_.set_selection_input_only(call.selection_input, call.selection_program, call.selection_screen);
    handler_.set_expected_mcp_session(call.session_identity, call.owner_identity);
}

WorkerCallFlags::~WorkerCallFlags() {
    handler_.set_selection_input_only(false);
    handler_.set_expected_mcp_session("", "");
    handler_.set_read_only(true);
}

WorkerInvoker make_session_worker_invoker(std::function<std::unique_ptr<cli::CommandHandler>()> make_handler) {
    struct State {
        std::unique_ptr<cli::CommandHandler> handler;
        std::optional<WorkerBinding> bound;
    };
    auto state = std::make_shared<State>();
    return [state, make_handler = std::move(make_handler)](const WorkerCall& call) -> Result {
        // the handler (and with it SAP) is created on the first call that needs it
        const auto ensure_handler = [&]() -> cli::CommandHandler& {
            if (!state->handler) {
                state->handler = make_handler();
                state->handler->set_batch_mode(true);
                state->handler->set_owner_window_guard(true);
            }
            return *state->handler;
        };
        if (call.enumerate) {
            if (state->bound) return Result::fail("WORKER_SESSION_MISMATCH", "worker is bound to a saved connection");
            cli::CommandHandler& handler = ensure_handler();
            Result result = handler.handle_list_all();
            if (result.status != Result::Status::Success || !result.data.is_object() ||
                !result.data.contains("connections") || !result.data["connections"].is_array())
                return Result::fail("OWNER_IDENTITY_UNKNOWN", "SAP sessions could not be enumerated");
            for (auto& connection : result.data["connections"]) {
                if (!connection.is_object() || !connection.contains("sessions") ||
                    !connection["sessions"].is_array()) continue;
                for (auto& session : connection["sessions"]) {
                    if (!session.is_object() || !session.contains("id") || !session["id"].is_string()) continue;
                    try {
                        const auto target = handler.peek_session_target("", session["id"].get<std::string>(), std::nullopt);
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
            if (state->bound && state->bound->connection != call.connection)
                return Result::fail("WORKER_SESSION_MISMATCH", "worker is bound to another saved connection");
            cli::CommandHandler& handler = ensure_handler();
            const std::optional<int> selected = call.connection < 0 ? std::nullopt : std::optional<int>(call.connection);
            const audit::SapFacts facts = handler.audit_facts_for_connection(selected);
            if (!facts.connection_id || (selected && facts.connection_id != selected) ||
                facts.system.empty() || facts.client.empty() ||
                facts.user.empty() || facts.session_identity.empty())
                return Result::fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP session is unavailable");
            const auto target = handler.peek_session_target("", "", facts.connection_id);
            Result result;
            result.status = Result::Status::Success;
            result.data = {{"connection_id", *facts.connection_id}, {"session_identity", facts.session_identity},
                           {"system", facts.system}, {"client", facts.client}, {"user", facts.user},
                           {"transaction", facts.transaction}, {"program", facts.program},
                           {"screen_number", facts.screen_number}, {"connection_name", target.connection_name}};
            return result;
        }
        // a mismatch and the read-only cap are refused before the handler (SAP) is touched
        if (auto refused = worker_binding_refusal(call, state->bound, config::read_only_cap())) return *refused;
        cli::CommandHandler& handler = ensure_handler();
        const audit::SapFacts live = handler.audit_facts_for_connection(call.connection);
        if (auto refused = worker_live_refusal(call, live)) return *refused;
        if (!state->bound) state->bound = WorkerBinding{call.connection, call.session_identity, call.owner_identity};
        const WorkerCallFlags flags(handler, call, config::read_only_cap());
        Result result = make_registry_invoker([&]() -> cli::CommandHandler& { return handler; })(call.argv);
        return result;
    };
}

int run_session_worker_stdio() {
    // stdout is exclusively the versioned IPC stream. No CLI diagnostics or
    // spdlog output may share it, even when command execution fails.
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(
        "mcp-session-worker", std::make_shared<spdlog::sinks::stderr_sink_mt>()));
    return run_worker_loop(std::cin, std::cout, make_session_worker_invoker([] { return std::make_unique<cli::CommandHandler>(); }));
}

} // namespace fairyfly::mcp
