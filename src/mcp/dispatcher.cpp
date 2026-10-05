#include "include/mcp/dispatcher.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <set>

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <comdef.h>
#endif

#include "include/auth/authorize.h"
#include "include/command_table.h"
#include "include/commands/cli_app.h"
#include "include/commands/command_registry.h"
#include "include/element_renderers.h"
#include "include/exceptions.h"
#include "include/login_flow.h"
#include "include/mcp/mcp_audit.h"
#include "include/mcp/policy.h"
#include "include/mcp/result_shaper.h"
#include "include/mcp/screen_guard.h"
#include "include/mcp/session_lease_policy.h"
#include "include/mcp/session_route_plan.h"
#include "include/mcp/session_worker_process.h"
#include "include/mcp/tool_catalog.h"

namespace fairyfly::mcp {

namespace {

const std::set<std::string> kScreenDataTools = {"gui_screen_read", "gui_screen_find", "gui_element_get", "gui_menu_list"};

std::string dump_compact(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

ToolResult error_result(const std::string& code, const std::string& message, const std::string& hint = "") {
    ToolResult r;
    r.is_error = true;
    std::string text = "ERROR " + code + ": " + message;
    if (!hint.empty()) text += "\nhint: " + hint;
    text += "\n" + dump_compact(json{{"code", code}, {"message", message}});
    r.content.push_back(json{{"type", "text"}, {"text", text}});
    return r;
}

Result error_from(const std::string& code, const std::string& message) {
    Result r;
    r.status = Result::Status::Error;
    r.error = {{"code", code}, {"message", message}};
    return r;
}

std::string result_error_code(const Result& result) {
    const json full = result.to_json();
    if (full.contains("error") && full["error"].is_object() && full["error"].contains("code") &&
        full["error"]["code"].is_string())
        return full["error"]["code"].get<std::string>();
    return "ERROR";
}

std::optional<int> int_from_json(const json& v) {
    try {
        if (v.is_number_integer()) return v.get<int>();
        if (v.is_string() && !v.get<std::string>().empty()) return std::stoi(v.get<std::string>());
    } catch (const std::exception&) {}
    return std::nullopt;
}

bool login_identity_transition_allowed(const std::string& before, const std::string& after,
                                       bool require_complete = false) {
    const auto first = before.find('|');
    const auto second = before.find('|', first == std::string::npos ? 0 : first + 1);
    const auto next_first = after.find('|');
    const auto next_second = after.find('|', next_first == std::string::npos ? 0 : next_first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        next_first == std::string::npos || next_second == std::string::npos ||
        first != next_first || before.substr(0, first) != after.substr(0, next_first)) return false;
    const bool old_key_empty = second == first + 1;
    const bool new_key_present = next_second > next_first + 1;
    const bool new_generation_present = next_second + 1 < after.size();
    if (before == after) return !require_complete || (new_key_present && new_generation_present);
    return old_key_empty && new_key_present && new_generation_present;
}

std::string session_policy_key(const Principal& principal, const std::string& identity,
                               std::optional<int> connection) {
    const std::string& owner = principal_key(principal);
    if (!identity.empty()) return owner + "\x1fsession:" + policy_session_identity(identity);
    if (connection) return owner + "\x1f" "connection:" + std::to_string(*connection);
    return owner;  // legacy/unknown target: preserve the conservative principal-wide state
}

/// Audit command of an argv: the CLI path from the command table ("element click").
std::string command_of(const std::vector<std::string>& argv) { return command_table::command_of_argv(argv); }

std::string text_of(const ToolResult& r) {
    for (const auto& block : r.content)
        if (block.is_object() && block.value("type", "") == "text" && block.contains("text") && block["text"].is_string())
            return block["text"].get<std::string>();
    return {};
}

std::string scale_text(double v) {
    if (std::floor(v) == v) return std::to_string(static_cast<long long>(v));
    std::string s = std::to_string(v);
    while (!s.empty() && s.back() == '0') s.pop_back();
    return s;
}

} // namespace

CommandDispatcher::CommandDispatcher(Invoker invoker, Policy policy, AuditHook hook, std::vector<ToolSpec> specs)
    : invoker_(std::move(invoker)), policy_(std::move(policy)), hook_(std::move(hook)),
      specs_(specs.empty() ? all_tool_specs() : std::move(specs)), limiter_(policy_.max_calls_per_minute) {
    rate_gate_ = [this](std::chrono::steady_clock::time_point now) { return limiter_.allow(now); };
}

std::set<std::string> CommandDispatcher::visible_tool_names(const Principal* principal) const {
    std::set<std::string> names;
    for (const auto& spec : specs_)
        if ((principal && auth::selection_input_tool_visible(*principal, spec)) ||
            (tool_visible(spec, policy_) && (!principal || auth::tool_allowed_for(*principal, spec))))
            names.insert(spec.def.name);
    return names;
}

std::vector<ToolDef> CommandDispatcher::list_tools() const {
    const auto visible = visible_tool_names(nullptr);
    std::vector<ToolDef> defs;
    for (const auto& spec : specs_)
        if (visible.count(spec.def.name)) {
            defs.push_back(spec.def);
            defs.back().description = describe_for(spec, visible);
        }
    return defs;
}

std::vector<ToolDef> CommandDispatcher::list_tools_for(const Principal& principal) const {
    const auto visible = visible_tool_names(&principal);
    std::vector<ToolDef> defs;
    for (const auto& spec : specs_)
        if (visible.count(spec.def.name)) {
            defs.push_back(spec.def);
            defs.back().description = describe_for(spec, visible);
            // A read-only token with allow_selection_input sees the narrowed fill tool: no cells, initial screen only.
            if (auth::selection_input_path(principal, spec.def.name, policy_) && auth::selection_input_tool_visible(principal, spec)) {
                defs.back().title = "Type into SAP selection field";
                defs.back().description =
                    "Types a value into a plain input (selection) field of the SAP screen, or clears it. This token is read-only: typing "
                    "is allowed ONLY on the initial screen of the transaction you opened with gui_transaction_start (before any "
                    "navigation; start the transaction again to get back to it) and ONLY into selection fields. Table or grid cells "
                    "(row/column), checkboxes, the command field and password fields are refused, and nothing is saved or posted. "
                    "Enter and F8 EXECUTE the selection (gui_key_send): the operator allowed this token only transactions whose "
                    "execution is read-only. The result echoes the value read back from the control, except for credential "
                    "fields; audit logs never contain the value. Give exactly one of value or clear.";
                if (defs.back().input_schema.is_object() && defs.back().input_schema.contains("properties"))
                    for (const char* key : {"row", "column", "checkbox", "commit"}) defs.back().input_schema["properties"].erase(key);
            }
        }
    return defs;
}

void CommandDispatcher::set_client_info(const json& client_info) {
    client_info_ = client_info;
    std::string name, version;
    if (client_info.is_object()) {
        if (client_info.contains("name") && client_info["name"].is_string()) name = client_info["name"].get<std::string>();
        if (client_info.contains("version") && client_info["version"].is_string())
            version = client_info["version"].get<std::string>();
    }
    client_ = version.empty() ? name : name + "/" + version;
    if (client_.size() > 128) client_.resize(128);
}

void CommandDispatcher::set_rate_gate(std::function<bool(std::chrono::steady_clock::time_point)> gate) {
    rate_gate_ = std::move(gate);
}

const ToolSpec* CommandDispatcher::find_spec(const std::string& name) const {
    for (const auto& spec : specs_)
        if (spec.def.name == name) return &spec;
    return nullptr;
}

std::string format_slow_step_message(const std::string& step, const std::string& tool, const std::string& principal,
                                     long long elapsed_ms) {
    return "slow MCP step: step=" + step + " tool=" + tool + " principal=" + principal + " elapsed_ms=" + std::to_string(elapsed_ms);
}

void CommandDispatcher::note_step(const char* step, const std::string& tool, const std::string& principal,
                                  std::chrono::steady_clock::time_point started, long long& out_ms) const {
    const auto elapsed = std::chrono::steady_clock::now() - started;
    out_ms += std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    if (elapsed < slow_step_threshold_) return;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    try {
        if (slow_step_reporter_) slow_step_reporter_(step, tool, principal, ms);
        else spdlog::warn("{}", format_slow_step_message(step, tool, principal, ms));
    } catch (...) {}
}

Result CommandDispatcher::invoke(const std::vector<std::string>& argv) {
    try {
        return invoker_(argv);
    } catch (const std::exception& e) {
        return error_from("INTERNAL_ERROR", e.what());
    } catch (...) {
        return error_from("INTERNAL_ERROR", "Unknown failure while executing command");
    }
}

ToolResult CommandDispatcher::audited(const std::string& tool,
                                      const std::function<ToolResult(McpCallRecord&, std::string&)>& fn,
                                      const CallContext& ctx, std::string* code_out) {
    const auto started = std::chrono::steady_clock::now();
    McpCallRecord record;
    record.tool = tool;
    record.client = client_;
    record.read_only = policy_.read_only || ctx.principal.read_only;
    record.principal = ctx.principal.name;
    record.token_id = ctx.principal.id;
    record.remote_addr = ctx.principal.remote_addr;
    record.transport = transport_label(ctx.http);
    record.era = ctx.http ? (ctx.era == ProtocolEra::Stateless ? "stateless" : "legacy") : "";
    record.request_id = ctx.request_id.is_null() ? std::string() : dump_compact(ctx.request_id);
    if (record.request_id.size() > 64) record.request_id.resize(64);

    std::string code;
    ToolResult result;
    const bool audit_blocked = policy_.audit_required && (!hook_ || mcp_audit_failed());
    if (audit_blocked) {
        code = "AUDIT_UNAVAILABLE";
        result = error_result(code, "The required audit trail is unavailable; no action was run. Retry after the audit trail recovers.");
    } else {
        try {
            result = fn(record, code);
        } catch (const std::exception& e) {
            result = error_result("INTERNAL_ERROR", e.what());
            code = "INTERNAL_ERROR";
        } catch (...) {
            result = error_result("INTERNAL_ERROR", "unexpected failure");
            code = "INTERNAL_ERROR";
        }
    }
    if (result.is_error && code.empty()) code = "ERROR";
    record.status = result.is_error ? "error" : "success";
    if (result.is_error) {
        if (record.error_code.empty()) record.error_code = code;
    } else {
        record.error_code.clear();
    }
    record.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started).count();
    if (hook_) {
        try { hook_(record); } catch (...) {}
        // Required-audit mode: the action already ran, but the model and user must learn that it
        // could not be recorded (mirrors the CLI's exit-code rule for --audit-required).
        if (policy_.audit_required && !audit_blocked && code != "AUDIT_UNAVAILABLE" && mcp_audit_failed()) {
            result = error_result("AUDIT_UNAVAILABLE",
                                  "The action was carried out but could not be recorded in the audit trail "
                                  "(audit is required). Tell the user and do not repeat the action.");
            code = "AUDIT_UNAVAILABLE";
        }
    }
    if (code_out) *code_out = result.is_error ? code : std::string();
    return result;
}

ToolResult CommandDispatcher::run_single(const std::string& name, const json& args, const CallContext& ctx,
                                         std::string* code_out) {
    return audited(name, [&](McpCallRecord& record, std::string& code) { return execute_call(name, args, record, code, ctx); },
                   ctx, code_out);
}

ToolResult CommandDispatcher::execute_call(const std::string& name, const json& raw_args, McpCallRecord& record,
                                           std::string& code, const CallContext& ctx) {
    auto fail = [&](const std::string& c, const std::string& message, const std::string& hint = "") {
        if (ctx.http && !policy_.owner_sap_identities.empty() &&
            (c == "OWNER_IDENTITY_UNKNOWN" || c == "OWNER_IDENTITY_DENIED" || c == "SESSION_TARGET_UNKNOWN" ||
             c == "SYSTEM_UNKNOWN" || c == "SYSTEM_DENIED" || c == "CONNECTION_DENIED" ||
             c == "SESSION_TARGET_CHANGED")) {
            record.error_code = c;  // the audit sink may distinguish boundary probes; the caller may not
            code = "OWNER_SESSION_UNAVAILABLE";
            return error_result(code, "the requested SAP session is unavailable");
        }
        code = c;
        return error_result(c, message, hint);
    };

    // 1. spec
    const ToolSpec* spec = find_spec(name);
    if (!spec) return fail("TOOL_NOT_FOUND", "unknown tool '" + name + "'", "use tools/list to see the available tools");
    json args = raw_args.is_null() ? json::object() : raw_args;
    const bool accepts_connection = spec->def.input_schema.is_object() &&
        spec->def.input_schema.value("properties", json::object()).contains("connection");
    const bool implicit_sticky = accepts_connection && !args.contains("connection") && !policy_.default_connection;
    const auto sticky_target = implicit_sticky ? routing_state_->sticky_target(principal_key(ctx.principal))
                                               : std::nullopt;
    if (args.is_object() && args.contains("connection")) record.connection = int_from_json(args["connection"]);
    else record.connection = accepts_connection
        ? (policy_.default_connection ? policy_.default_connection : sticky_connection(principal_key(ctx.principal)))
        : std::nullopt;

    // 2. policy
    // gui_element_fill of a principal with allow_selection_input is decided by auth::authorize_call (INPUT_* rules) instead of
    // the blanket read-only refusal; every other tool keeps check_call.
    const bool selection_input = auth::selection_input_path(ctx.principal, name, policy_);
    const PolicyDecision decision = selection_input ? PolicyDecision{} : check_call(*spec, args, policy_);
    if (!decision.allowed)
        return fail(decision.code.empty() ? "REFUSED" : decision.code,
                    decision.message.empty() ? "call refused by policy" : decision.message);

    // 2b. token authorization (scope, token read-only, SAP system, T-code); the stdio principal allows everything
    const Principal& principal = ctx.principal;
    if (!auth::scope_allows(principal, spec->family, name))
        return fail("SCOPE_DENIED", "token lacks the required scope for this tool");
    if (ctx.http && !policy_.owner_sap_identities.empty() && !owner_token_bound(policy_, principal))
        return fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP session is unavailable");
    // Refuse exhausted tokens before doing a potentially slow SAP COM lookup.
    const bool per_principal = principal.rate_per_minute > 0 || principal.name != "stdio";
    const int budget = principal.rate_per_minute > 0 ? principal.rate_per_minute : policy_.max_calls_per_minute;
    const bool rate_ok = per_principal ? keyed_limiter_->allow(principal_key(principal), budget, std::chrono::steady_clock::now())
                                       : (!rate_gate_ || rate_gate_(std::chrono::steady_clock::now()));
    if (!rate_ok)
        return fail("RATE_LIMITED",
                    "too many tool calls (limit " + std::to_string(per_principal ? budget : policy_.max_calls_per_minute) +
                        " per minute)",
                    "wait a few seconds and retry, or combine steps with gui_batch");
    if (const int family_limit = auth::rate_family_limit(principal, spec->family); family_limit > 0) {
        if (!keyed_limiter_->allow(principal_key(principal) + "|family:" + spec->family, family_limit, std::chrono::steady_clock::now()))
            return fail("RATE_LIMITED",
                        "too many '" + spec->family + "' tool calls (family limit " + std::to_string(family_limit) + " per minute)",
                        "wait a few seconds and retry");
    }
    // These discovery responses contain other sessions/credentials and cannot be vouched for by the currently
    // selected session. Connection and session listings are verified row by row after execution.
    if (ctx.http && !policy_.owner_sap_identities.empty() &&
        (name == "gui_credentials_list" ||
         (name == "gui_session_attach" && args.is_object() && !args.contains("session_id"))))
        return fail("OWNER_IDENTITY_UNKNOWN", "the SAP identity for this call cannot be verified before execution");
    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_connection_list" &&
        args.is_object() && args.value("cleanup", false))
        return fail("OWNER_IDENTITY_UNKNOWN", "connection cleanup is unavailable for owner-mode HTTP");
    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_launch") {
        if (policy_.owner_sap_identities.size() > 1 && !routing_state_->prelogin_capacity_available(principal.id))
            return fail("OWNER_SESSION_UNAVAILABLE", "the endpoint has too many unfinished SAP logon windows");
        const std::string requested = args.is_object() && args.contains("name") && args["name"].is_string()
            ? args["name"].get<std::string>() : std::string();
        const bool vouched = !requested.empty() && std::any_of(principal.connections.begin(), principal.connections.end(),
            [&](const std::string& pattern) { return auth::glob_match(pattern, requested); });
        if (!vouched) return fail("OWNER_SESSION_UNAVAILABLE", "the SAP Logon entry is unavailable to this token");
        if (args.is_object() && args.value("allow_sapshcut", false))
            return fail("SESSION_CONTROL_UNAVAILABLE", "shortcut launch is unavailable for owner-mode HTTP");
    }
    std::unique_lock<std::shared_timed_mutex> control_probe_lock;
    const auto acquire_control_probe_gate = [&]() {
        if (!control_probe_gate_)
            return false;
        control_probe_lock = std::unique_lock<std::shared_timed_mutex>(*control_probe_gate_, std::defer_lock);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!control_probe_lock.try_lock_for(std::chrono::milliseconds(50))) {
            if ((ctx.cancelled && ctx.cancelled()) || std::chrono::steady_clock::now() >= deadline)
                return false;
        }
        return true;
    };
    // Launch changes the GUI application. Login only needs the global probe gate
    // after its own window lane is free; holding it while queued would stall all
    // unrelated windows' admission probes.
    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_launch" &&
        !acquire_control_probe_gate()) {
        return fail("SESSION_ROUTE_UNAVAILABLE", "SAP admission probes are busy");
    }
    // Another launch may have filled the table while this call waited for the
    // process-wide launch gate. Check again while holding that gate.
    if (ctx.http && policy_.owner_sap_identities.size() > 1 && name == "gui_session_launch" &&
        !routing_state_->prelogin_capacity_available(principal.id))
        return fail("OWNER_SESSION_UNAVAILABLE", "the endpoint has too many unfinished SAP logon windows");
    std::string policy_state_key = session_policy_key(principal, "", record.connection);
    std::string policy_fallback_key = policy_state_key;
    std::optional<audit::SapFacts> owner_pre_facts;
    std::optional<audit::SapFacts> login_pre_facts;
    std::optional<audit::SapFacts> prelogin_close_facts;
    std::shared_ptr<void> login_lane_reservation;
    auth::InitialScreen selection_initial;  // the recorded initial screen an authorized selection-input fill may write on
    {
        std::optional<std::string> current_system, current_tcode;
        auth::SelectionInputContext input_context;
        std::optional<audit::SapFacts> live_facts;
        const bool owner_needs_facts = ctx.http && !policy_.owner_sap_identities.empty() &&
                                       name != "gui_session_list" && name != "gui_connection_list" &&
                                       name != "gui_doctor" &&
                                       name != "gui_session_attach" &&
                                       name != "gui_session_launch";
        if ((!principal.sap_systems.empty() || !principal.tcodes.empty() || owner_needs_facts ||
             (sticky_target && !sticky_target->session_identity.empty())) && facts_provider_) {
            const auto started = std::chrono::steady_clock::now();
            try { live_facts = facts_provider_(record.connection); } catch (...) {}
            note_step("facts_pre", name, principal.name, started, record.facts_pre_ms);
            if (live_facts && live_facts->any()) {
                record.sap = *live_facts;
                if (record.connection && live_facts->connection_id && *record.connection != *live_facts->connection_id)
                    return fail("SESSION_TARGET_UNKNOWN", "live SAP facts refer to another saved connection");
                if (!live_facts->system.empty()) current_system = live_facts->system + "/" + live_facts->client;
                if (!live_facts->transaction.empty()) current_tcode = live_facts->transaction;
                input_context.program = live_facts->program;
                input_context.screen_number = live_facts->screen_number;
                input_context.session_identity = live_facts->session_identity;
                // The connection this call acts on: explicit/sticky/default, else the id the facts lookup resolved
                // (automatic single-connection resolution). Two different answers = unknown.
                if (!record.connection)
                    input_context.connection = live_facts->connection_id;
                policy_state_key = session_policy_key(principal, live_facts->session_identity,
                                                      live_facts->connection_id ? live_facts->connection_id : record.connection);
                policy_fallback_key = session_policy_key(principal, "",
                                                         live_facts->connection_id ? live_facts->connection_id : record.connection);
            }
        }
        if (sticky_target && !sticky_target->session_identity.empty() &&
            (!live_facts || live_facts->connection_id != sticky_target->connection ||
             live_facts->session_identity != sticky_target->session_identity))
            return fail("SESSION_TARGET_CHANGED", "the saved default connection now identifies another SAP session");
        if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_login") {
            const bool known = live_facts && live_facts->connection_id && !live_facts->system.empty() &&
                !live_facts->session_identity.empty() && live_facts->user.empty() &&
                (!record.connection || *record.connection == *live_facts->connection_id);
            if (!known) return fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP logon session is unavailable");
            if (policy_.owner_sap_identities.size() > 1 &&
                !routing_state_->owns_prelogin(principal.id, *live_facts->connection_id, live_facts->session_identity))
                return fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP logon session is unavailable");
            record.connection = live_facts->connection_id;
            login_pre_facts = live_facts;
            const std::string lane_key = window_lane_key(live_facts->session_identity);
            if (lane_key.empty() || !login_lane_reserver_)
                return fail("OWNER_SESSION_UNAVAILABLE", "the SAP logon session cannot be reserved");
            try { login_lane_reservation = login_lane_reserver_(lane_key, ctx.cancelled); } catch (...) {}
            if (!login_lane_reservation || (ctx.cancelled && ctx.cancelled()))
                return fail("SESSION_ROUTE_UNAVAILABLE", "the SAP logon session is busy or unavailable");
            if (!acquire_control_probe_gate())
                return fail("SESSION_ROUTE_UNAVAILABLE", "SAP admission probes are busy");
            bool authorized_after_wait = false;
            try { authorized_after_wait = ctx.reauthorize && ctx.reauthorize(); } catch (...) {}
            if (!authorized_after_wait)
                return fail("TOKEN_CHANGED", "authorization changed while waiting for the SAP logon session");
            std::optional<audit::SapFacts> reserved_facts;
            try { reserved_facts = facts_provider_(*record.connection); } catch (...) {}
            if (!reserved_facts || reserved_facts->connection_id != login_pre_facts->connection_id ||
                reserved_facts->session_identity != login_pre_facts->session_identity ||
                reserved_facts->system != login_pre_facts->system || !reserved_facts->user.empty())
                return fail("OWNER_SESSION_UNAVAILABLE", "the SAP logon session changed while waiting");
            if (policy_.owner_sap_identities.size() > 1 &&
                !routing_state_->owns_prelogin(principal.id, *reserved_facts->connection_id,
                                               reserved_facts->session_identity))
                return fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP logon session is unavailable");
        } else if (ctx.http && policy_.owner_sap_identities.size() > 1 &&
            name == "gui_session_disconnect" && args.is_object() && args.contains("close_session") &&
            args["close_session"].is_boolean() && args["close_session"].get<bool>() &&
            record.connection && routing_state_->has_prelogin_claim(principal.id, *record.connection)) {
            if (!live_facts || live_facts->connection_id != record.connection ||
                live_facts->system.empty() || !live_facts->user.empty() ||
                live_facts->session_identity.empty() ||
                !routing_state_->owns_prelogin(principal.id, *record.connection, live_facts->session_identity))
                return fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP session is unavailable");
            auth::SessionTarget target;
            target.ambiguous = true;
            try { if (target_resolver_) target = target_resolver_(TargetQuery{"", "", record.connection}); }
            catch (...) {}
            const bool connection_allowed = !target.ambiguous && !target.connection_name.empty() &&
                std::any_of(principal.connections.begin(), principal.connections.end(), [&](const std::string& pattern) {
                    return auth::glob_match(pattern, target.connection_name);
                });
            std::string granted_system_client;
            for (const auto& identity : policy_.owner_sap_identities) {
                if (!owner_identity_allowed(policy_, principal, identity) ||
                    identity.rfind(live_facts->system + "/", 0) != 0) continue;
                const auto second = identity.find('/', live_facts->system.size() + 1);
                if (second == std::string::npos) continue;
                const std::string candidate = identity.substr(0, second);
                if (!principal.sap_systems.empty() && !auth::system_allowed(principal.sap_systems, candidate)) continue;
                granted_system_client = candidate;
                break;
            }
            if (!connection_allowed || granted_system_client.empty())
                return fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP session is unavailable");
            current_system = granted_system_client;
            prelogin_close_facts = live_facts;
        } else if (ctx.http && !policy_.owner_sap_identities.empty() && name != "gui_session_attach" &&
            name != "gui_session_list" && name != "gui_connection_list" &&
            name != "gui_doctor" && name != "gui_session_launch") {
            const bool known = live_facts && !live_facts->system.empty() && !live_facts->client.empty() &&
                               !live_facts->user.empty() && !live_facts->session_identity.empty() &&
                               (!record.connection || (live_facts->connection_id && *record.connection == *live_facts->connection_id));
            if (!known) return fail("OWNER_IDENTITY_UNKNOWN", "the target session's SAP identity could not be verified");
            const std::string identity = live_facts->system + "/" + live_facts->client + "/" + live_facts->user;
            const bool allowed = owner_identity_allowed(policy_, principal, identity);
            if (!allowed) return fail("OWNER_IDENTITY_DENIED", "the target session's SAP identity is not allowed for this endpoint");
            // Implicit resolution must use the same saved connection that was checked above.
            if (!record.connection) record.connection = live_facts->connection_id;
            owner_pre_facts = live_facts;
        }
        if (principal.allow_selection_input) {
            input_context.initial = initial_screen(policy_state_key);
            if (record.connection) input_context.connection = record.connection;
            // Any call that observes another (or an unknown) screen than the recorded one ends the typing window for good:
            // coming back to the initial screen later needs a new gui_transaction_start.
            if (input_context.initial && facts_provider_ && name != "gui_transaction_start" &&
                (!current_tcode || auth::normalize_tcode(*current_tcode) != input_context.initial->transaction ||
                 input_context.program != input_context.initial->program ||
                 input_context.screen_number != input_context.initial->screen_number)) {
                set_initial_screen(policy_state_key, std::nullopt);
                input_context.initial.reset();
            }
        }
        Principal scoped_doctor_principal = principal;
        if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_doctor")
            scoped_doctor_principal.connections.clear(); // output is filtered by the original token below
        const PolicyDecision authz = auth::authorize_call(scoped_doctor_principal, *spec, spec->family, args, policy_, current_system,
                                                          current_tcode, [this](const std::string& n) { return find_spec(n); },
                                                          &input_context);
        if (!authz.allowed) return fail(authz.code.empty() ? "REFUSED" : authz.code, authz.message);
        if (selection_input && input_context.initial) selection_initial = *input_context.initial;
        // Atomicity: call_tool runs only on the executor (main) thread, one call at a time (ToolProvider contract, CallExecutor
        // FIFO), so this check -> invoke -> set_tcode_blocked sequence cannot interleave with another call of the same token.
        // A token that ended its previous call outside its T-code allowlist stays locked out of screen-acting tools
        // until gui_transaction_start succeeds with an allowed code (nothing navigates back automatically).
        if (!principal.tcodes.empty() && auth::acts_on_screen(spec->family) &&
            (tcode_blocked(policy_state_key) || tcode_blocked(policy_fallback_key)))
            return fail("TCODE_DENIED", "an earlier call of token '" + principal.name + "' left its allowed transactions; screen "
                                        "tools stay blocked until gui_transaction_start opens an allowed transaction");
    }

    // 2c. session/connection targets: the SAP-system and saved-connection allowlists also bind the calls that launch,
    // log on, attach, disconnect (or act through an explicit `connection`). The target is resolved read-only, without
    // contacting SAP; when it cannot be determined the call is refused (fail closed).
    const auto check_target = [&](const json& call_args) -> std::optional<PolicyDecision> {
        if (prelogin_close_facts) return std::nullopt; // exact live claim and name were checked above
        const bool owner_target = ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_attach";
        if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_login")
            return std::nullopt; // the handler checks the stored credential and live connection before typing
        if (!owner_target && !auth::needs_session_target(principal, name, call_args)) return std::nullopt;
        TargetQuery query;
        if (name == "gui_session_launch") {
            if (call_args.is_object() && call_args.contains("name") && call_args["name"].is_string())
                query.logon_name = call_args["name"].get<std::string>();
        } else if (name == "gui_session_attach") {
            if (call_args.is_object() && call_args.contains("session_id") && call_args["session_id"].is_string())
                query.session_id = call_args["session_id"].get<std::string>();
        } else {
            query.connection = record.connection;
        }
        auth::SessionTarget target;
        target.ambiguous = true;  // no resolver / resolver failure: a launch cannot be checked against open sessions -> denied
        if (target_resolver_) {
            try { target = target_resolver_(query); } catch (...) { target = {}; target.ambiguous = true; }
        }
        if (owner_target) {
            if (target.system.empty() || target.user.empty() || target.ambiguous ||
                target.system.find('/') == std::string::npos)
                return PolicyDecision{false, "OWNER_IDENTITY_UNKNOWN", "the target session's SAP identity could not be verified"};
            const std::string identity = target.system + "/" + target.user;
            if (!owner_identity_allowed(policy_, principal, identity))
                return PolicyDecision{false, "OWNER_IDENTITY_DENIED", "the target session's SAP identity is not allowed for this endpoint"};
        }
        if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_launch") {
            // A SAP Logon entry can already have windows authenticated as other users.
            // Its live system/client may help constrain launch, but that user's identity
            // says nothing about the new window. The launch/login guards check the new
            // window and its authenticated SAP user before exposing its saved ID.
            target.user.clear();
        }
        auto decision = auth::authorize_session_target(principal, name, call_args, target);
        if (decision.allowed) return std::nullopt;
        return decision;
    };
    const bool attach_needs_resolution = name == "gui_session_attach" && args.is_object() && !args.contains("session_id");
    if (!attach_needs_resolution)
        if (auto refused = check_target(args)) return fail(refused->code.empty() ? "REFUSED" : refused->code, refused->message);

    // Broker-native lease control runs only after the same scope, owner, SAP-system and
    // saved-connection checks as ordinary calls. Its secret never enters CLI argv or audit.
    if (name == "gui_session_lease") {
        try { (void)spec->build_argv(args, policy_); }
        catch (const std::exception& e) { return fail("INVALID_ARGUMENT", e.what()); }
        record.command = "session lease";
        if (!ctx.http || policy_.owner_sap_identities.empty() || !owner_pre_facts ||
            principal.id.empty() || !session_leases_)
            return fail("LEASE_UNAVAILABLE", "session leases require authenticated owner-mode HTTP");
        const std::string action = args.at("action").get<std::string>();
        const std::string id = args.value("lease_id", std::string());
        const std::string& key = owner_pre_facts->session_identity;
        const auto now = SessionLeases::Clock::now();
        if (action == "release") {
            if (!session_leases_->release(key, principal.id, id))
                return fail("LEASE_NOT_HELD", "the lease is unavailable or no longer held by this token");
            ToolResult out;
            out.structured = json{{"status", "released"}};
            out.content.push_back(json{{"type", "text"}, {"text", out.structured->dump()}});
            return out;
        }
        const LeaseResult lease = action == "acquire" ? session_leases_->acquire(key, principal.id, now)
            : session_leases_->renew(key, principal.id, id, now);
        if (lease.status != LeaseStatus::Granted)
            return fail(lease.status == LeaseStatus::HeldByOther ? "LEASE_HELD" : "LEASE_NOT_HELD",
                        "the requested SAP session lease is unavailable");
        std::optional<audit::SapFacts> current;
        const auto post_started = std::chrono::steady_clock::now();
        try { current = facts_provider_(record.connection); } catch (...) {}
        note_step("facts_post", name, principal.name, post_started, record.facts_post_ms);
        if (!current || current->connection_id != owner_pre_facts->connection_id ||
            current->session_identity != key || current->system != owner_pre_facts->system ||
            current->client != owner_pre_facts->client || current->user != owner_pre_facts->user) {
            session_leases_->retire_session(key);
            return fail("OWNER_IDENTITY_UNKNOWN", "the SAP session changed while its lease was being acquired");
        }
        ToolResult out;
        out.structured = json{{"status", "granted"}, {"lease_id", lease.lease_id},
                              {"expires_in_seconds", std::max<long long>(0,
                                  std::chrono::duration_cast<std::chrono::seconds>(lease.expires_at - now).count())}};
        out.content.push_back(json{{"type", "text"}, {"text", out.structured->dump()}});
        return out;
    }

    // A read-only token narrows the server mode for this call only (restored afterwards). An authorized selection-input
    // fill does the opposite for THAT call only: it lifts the handler's read-only guard (so the fill is not refused) and
    // restores the server value afterwards, also when the invocation throws. No other tool ever runs with the guard lifted.
    struct ReadOnlyScope {
        const ReadOnlyOverride& hook;
        bool server_value;
        bool active;
        ReadOnlyScope(const ReadOnlyOverride& h, bool apply, bool value, bool server)
            : hook(h), server_value(server), active(apply && static_cast<bool>(h)) {
            if (active) hook(value);
        }
        ~ReadOnlyScope() {
            if (active) { try { hook(server_value); } catch (...) {} }
        }
    } read_only_scope(read_only_override_,
                      selection_input || (principal.read_only && !policy_.read_only),
                      selection_input ? false : true, policy_.read_only);

    // The same call also switches the handler into selection-input mode: the handler validates the LIVE control (plain
    // changeable text field, no credential id/name/label) and the live screen right before the write. Restored on every
    // path (the destructor runs for exceptions and for CALL_TIMEOUT unwinding as well). Without a hook the lifted guard
    // would be unchecked, so the call is refused.
    struct SelectionScope {
        const SelectionInputOverride& hook;
        bool active;
        SelectionScope(const SelectionInputOverride& h, bool apply, const std::string& program, const std::string& screen)
            : hook(h), active(apply && static_cast<bool>(h)) {
            if (active) hook(true, program, screen);
        }
        ~SelectionScope() {
            if (active) { try { hook(false, std::string(), std::string()); } catch (...) {} }
        }
    } selection_scope(selection_input_override_, selection_input, selection_initial.program, selection_initial.screen_number);
    if (selection_input && !selection_input_override_)
        return fail("INPUT_TARGET_DENIED", "the server cannot validate the field before typing (fail closed)");

    // Effective policy: call argument > policy default > sticky default (the argument wins in build_argv).
    Policy effective = policy_;
    if (record.connection) effective.default_connection = record.connection;
    else if (!effective.default_connection) effective.default_connection = sticky_connection(principal_key(ctx.principal));

    // gui_session_attach without session_id: resolve through `list`.
    if (name == "gui_session_attach" && args.is_object() && !args.contains("session_id")) {
        Result listed = invoke({"session", "list"});
        // A token limited to named connections only ever sees (and can only be offered) its own sessions.
        if (!auth::filter_listing_for_connections(principal, "gui_session_list", listed))
            return fail("RESULT_FILTER_FAILED", "the session list could not be limited to the connections of token '" +
                                                 principal.name + "'");
        if (listed.status != Result::Status::Success) {
            code = result_error_code(listed);
            return shape_result(listed, *spec, policy_, "", nullptr);
        }
        json sessions = json::array();
        const json& conns = listed.data.is_object() && listed.data.contains("connections") ? listed.data["connections"] : json::array();
        if (conns.is_array())
            for (const auto& c : conns) {
                if (!c.is_object() || !c.contains("sessions") || !c["sessions"].is_array()) continue;
                for (const auto& s : c["sessions"])
                    sessions.push_back({{"session_id", s.value("id", "")},
                                        {"connection", c.value("description", "")},
                                        {"window_title", s.contains("active_window_title") && s["active_window_title"].is_string()
                                                             ? s["active_window_title"] : json("")}});
            }
        if (sessions.empty())
            return fail("NO_SESSIONS", "no SAP GUI session is open", "start SAP GUI / gui_session_launch, then retry");
        if (sessions.size() > 1)
            return fail("MULTIPLE_SESSIONS",
                        std::to_string(sessions.size()) + " SAP GUI sessions are open; call gui_session_attach again with session_id. "
                        "Sessions: " + dump_compact(sessions));
        args["session_id"] = sessions[0]["session_id"];
        // The session is known now: the token's system/connection allowlists apply to it like to an explicit id.
        if (auto refused = check_target(args)) return fail(refused->code.empty() ? "REFUSED" : refused->code, refused->message);
    }

    // 4. build argv
    std::vector<std::string> argv;
    try {
        argv = spec->build_argv(args, effective);
    } catch (const std::invalid_argument& e) {
        return fail("INVALID_ARGUMENT", e.what());
    } catch (const std::exception& e) {
        return fail("INVALID_ARGUMENT", std::string("invalid arguments: ") + e.what());
    }
    record.argv = argv;
    record.command = command_of(argv);
    for (std::size_t i = 0; i + 1 < argv.size(); ++i)
        if (argv[i] == "--connection") record.connection = int_from_json(json(argv[i + 1]));

    struct LeaseWriteScope {
        std::shared_ptr<SessionLeases> leases;
        std::string session, token, id;
        bool active = false;
        ~LeaseWriteScope() {
            if (active) leases->end_write(session, token, id, SessionLeases::Clock::now());
        }
    } lease_scope;
    if (ctx.http && !policy_.owner_sap_identities.empty() && !prelogin_close_facts &&
        lease_required_for_call(name, args)) {
        if (!owner_pre_facts || principal.id.empty() || !session_leases_)
            return fail("LEASE_UNAVAILABLE", "the session lease cannot be verified");
        if (!args.is_object() || !args.contains("lease_id") || !args["lease_id"].is_string())
            return fail("LEASE_REQUIRED", "acquire a session lease and pass its lease_id");
        const std::string id = args["lease_id"].get<std::string>();
        if (ctx.batch_lease) {
            const auto& held = *ctx.batch_lease;
            if (held.session_identity != owner_pre_facts->session_identity || held.token_id != principal.id ||
                held.lease_id != id || !record.connection || held.connection != *record.connection)
                return fail("LEASE_REQUIRED", "the batch lease does not match this SAP session");
        } else {
            lease_scope.leases = session_leases_;
            lease_scope.session = owner_pre_facts->session_identity;
            lease_scope.token = principal.id;
            lease_scope.id = id;
            lease_scope.active = session_leases_->begin_write(lease_scope.session, lease_scope.token, lease_scope.id,
                                                             SessionLeases::Clock::now());
            if (!lease_scope.active) return fail("LEASE_REQUIRED", "the session lease is missing or expired");
        }
    }

    std::string expected_screen_guard;
    if (args.is_object() && args.contains("expected_screen_guard")) {
        if (!ctx.http || policy_.owner_sap_identities.empty() || !owner_pre_facts)
            return fail("INVALID_ARGUMENT", "a screen guard requires an owner-routed SAP session");
        expected_screen_guard = args["expected_screen_guard"].get<std::string>();
        if (expected_screen_guard.size() != 64)
            return fail("INVALID_ARGUMENT", "expected_screen_guard must be a 64-character digest");
        if (screen_guard_for(*owner_pre_facts) != expected_screen_guard)
            return fail("SCREEN_CHANGED", "the SAP session or dynpro changed since the client's screen read");
    }

    if ((owner_pre_facts || prelogin_close_facts) && !owner_session_override_)
        return fail("OWNER_IDENTITY_UNKNOWN", "the server cannot bind this call to its checked SAP session");
    if (prelogin_close_facts) {
        bool current = false;
        try { current = ctx.reauthorize && ctx.reauthorize(); } catch (...) {}
        if (!current || !routing_state_->owns_prelogin(principal.id, *prelogin_close_facts->connection_id,
                                                        prelogin_close_facts->session_identity))
            return fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP session is unavailable");
    }
    struct OwnerSessionScope {
        const OwnerSessionOverride& hook;
        bool active;
        OwnerSessionScope(const OwnerSessionOverride& h, const std::optional<audit::SapFacts>& facts)
            : hook(h), active(static_cast<bool>(facts) && static_cast<bool>(h)) {
            if (active) hook(facts->session_identity, facts->system + "/" + facts->client + "/" + facts->user);
        }
        ~OwnerSessionScope() {
            if (active) { try { hook("", ""); } catch (...) {} }
        }
    } owner_session_scope(owner_session_override_, owner_pre_facts ? owner_pre_facts : prelogin_close_facts);

    // 5. invoke
    if (selection_input) record.input_allowed = true;  // authorized above; the typed value is never recorded
    const auto invoke_started = std::chrono::steady_clock::now();
    const bool worker_mode = (session_invoker_ || session_invoker_with_gate_) && owner_pre_facts;
    const bool worker_bound = worker_mode && worker_command_allowed(argv);
    if (worker_mode && !worker_bound)
        return fail("WORKER_UNSUPPORTED", "the session command is not supported by the private worker");
    if (worker_bound && !record.connection)
        return fail("OWNER_SESSION_UNAVAILABLE", "the checked SAP session has no saved connection");
    const auto invoke_selected = [&](const std::vector<std::string>& selected_argv) -> Result {
        if (!worker_bound) return invoke(selected_argv);
        if (!worker_command_allowed(selected_argv))
            return error_from("WORKER_UNSUPPORTED", "the session retry cannot be routed to its private worker");
        WorkerCall worker_call;
        worker_call.connection = *record.connection;
        worker_call.session_identity = owner_pre_facts->session_identity;
        worker_call.owner_identity = owner_pre_facts->system + "/" + owner_pre_facts->client + "/" + owner_pre_facts->user;
        worker_call.argv = selected_argv;
        worker_call.read_only = policy_.read_only || principal.read_only;
        worker_call.expected_screen_guard = expected_screen_guard;
        worker_call.selection_input = selection_input;
        if (selection_input) {
            worker_call.selection_program = selection_initial.program;
            worker_call.selection_screen = selection_initial.screen_number;
        }
        const auto before_send = [&] {
            if (policy_.audit_required && mcp_audit_failed())
                throw WorkerTransportError("AUDIT_UNAVAILABLE", "the required audit trail failed before the worker action");
            if (ctx.cancelled && ctx.cancelled())
                throw WorkerTransportError("CANCELLED", "session call was cancelled before the worker action");
            if (ctx.http) {
                if (!ctx.reauthorize)
                    throw WorkerTransportError("AUTH_UNAVAILABLE", "worker authorization check is unavailable");
                bool current = false;
                try { current = ctx.reauthorize(); } catch (...) {}
                if (!current)
                    throw WorkerTransportError("TOKEN_CHANGED", "authorization changed before the worker action");
            }
            if (ctx.http && !policy_.owner_sap_identities.empty() && lease_required_for_call(name, args)) {
                const std::string id = args.value("lease_id", std::string());
                if (!session_leases_ || !session_leases_->permits_write(owner_pre_facts->session_identity,
                        principal.id, id, SessionLeases::Clock::now()))
                    throw WorkerTransportError("LEASE_REQUIRED", "session lease changed before the worker action");
            }
        };
        try {
            before_send();
            return session_invoker_with_gate_ ? session_invoker_with_gate_(worker_call, before_send)
                                              : session_invoker_(worker_call);
        }
        catch (const WorkerTransportError& e) { return error_from(e.code(), e.what()); }
        catch (...) { return error_from("OUTCOME_UNKNOWN", "session worker command outcome is unknown"); }
    };
    if (ctx.http && !policy_.owner_sap_identities.empty() &&
        (name == "gui_session_attach" || name == "gui_session_login" || name == "gui_session_launch")) {
        if (ctx.cancelled && ctx.cancelled())
            return fail("CANCELLED", "session control was cancelled before selecting the SAP session");
        bool current = false;
        try { current = ctx.reauthorize && ctx.reauthorize(); } catch (...) {}
        if (!current)
            return fail("TOKEN_CHANGED", "authorization changed before selecting the SAP session");
        if (name == "gui_session_attach")
            if (auto refused = check_target(args))
                return fail(refused->code.empty() ? "REFUSED" : refused->code, refused->message);
    }
    struct AttachGuardScope {
        const AttachTargetGuardOverride& hook;
        bool active;
        AttachGuardScope(const AttachTargetGuardOverride& override, AttachTargetGuard guard)
            : hook(override), active(static_cast<bool>(override) && static_cast<bool>(guard)) {
            if (active) hook(std::move(guard));
        }
        ~AttachGuardScope() { if (active) { try { hook({}); } catch (...) {} } }
    } attach_guard_scope(attach_target_guard_override_,
        ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_attach"
            ? AttachTargetGuard([principal, args](const audit::SapFacts& facts, const std::string& connection_name) {
                if (facts.system.empty() || facts.client.empty() || facts.user.empty()) return false;
                auth::SessionTarget target;
                target.system = facts.system + "/" + facts.client;
                target.user = facts.user;
                target.connection_name = connection_name;
                return auth::authorize_session_target(principal, "gui_session_attach", args, target).allowed;
            }) : AttachTargetGuard{});
    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_attach" &&
        (!attach_target_guard_override_ || !attach_finalize_override_))
        return fail("OWNER_SESSION_UNAVAILABLE", "the attach target cannot be checked before changing the saved connection");
    std::optional<audit::SapFacts> checked_launch_facts;
    const LaunchTargetGuard owner_launch_guard = [this, &principal, &args, &ctx, &checked_launch_facts]
        (const audit::SapFacts& facts, const std::string& connection_name) {
        if (facts.system.empty() || connection_name.empty() || (ctx.cancelled && ctx.cancelled())) return false;
        bool authorized = false;
        try { authorized = ctx.reauthorize && ctx.reauthorize(); } catch (...) {}
        if (!authorized) return false;
        const std::string requested = args.at("name").get<std::string>();
        const auto name_allowed = [&](const std::string& name) {
            return std::any_of(principal.connections.begin(), principal.connections.end(),
                [&](const std::string& pattern) { return auth::glob_match(pattern, name); });
        };
        if (!name_allowed(requested) || !name_allowed(connection_name)) return false;
        std::string allowed_system_client;
        for (const std::string& owner : policy_.owner_sap_identities) {
            if (!owner_identity_allowed(policy_, principal, owner)) continue;
            const auto first = owner.find('/');
            const auto second = first == std::string::npos ? first : owner.find('/', first + 1);
            if (first == std::string::npos || second == std::string::npos) continue;
            const std::string system = owner.substr(0, first);
            const std::string client = owner.substr(first + 1, second - first - 1);
            const std::string user = owner.substr(second + 1);
            const bool sap_logon_screen = facts.user.empty() && facts.client == "000" &&
                                          facts.transaction == "S000";
            if (facts.system != system || (!facts.client.empty() && facts.client != client && !sap_logon_screen) ||
                (!facts.user.empty() && facts.user != user) ||
                (facts.user.empty() && !facts.client.empty() && facts.client != client && !sap_logon_screen)) continue;
            const std::string candidate = system + "/" + client;
            if (!principal.sap_systems.empty() && !auth::system_allowed(principal.sap_systems, candidate)) continue;
            allowed_system_client = candidate;
            break;
        }
        if (allowed_system_client.empty() || (!facts.user.empty() && facts.client.empty())) return false;
        auth::SessionTarget target;
        target.system = allowed_system_client;
        target.user = facts.user;
        target.connection_name = connection_name;
        if (!auth::authorize_session_target(principal, "gui_session_launch", args, target).allowed) return false;
        checked_launch_facts = facts;
        return true;
    };
    struct LaunchGuardScope {
        const LaunchTargetGuardOverride& hook;
        bool active;
        LaunchGuardScope(const LaunchTargetGuardOverride& override, LaunchTargetGuard guard)
            : hook(override), active(static_cast<bool>(override) && static_cast<bool>(guard)) {
            if (active) hook(std::move(guard));
        }
        ~LaunchGuardScope() { if (active) { try { hook({}); } catch (...) {} } }
    } launch_guard_scope(launch_target_guard_override_,
        ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_launch"
            ? owner_launch_guard : LaunchTargetGuard{});
    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_launch" &&
        (!launch_target_guard_override_ || !launch_rollback_override_))
        return fail("OWNER_SESSION_UNAVAILABLE", "the launched SAP window cannot be verified");
    std::optional<audit::SapFacts> anticipated_login;
    struct LoginGuardScope {
        const LoginTargetGuardOverride& hook;
        bool active;
        LoginGuardScope(const LoginTargetGuardOverride& override, LoginTargetGuard guard)
            : hook(override), active(static_cast<bool>(override) && static_cast<bool>(guard)) {
            if (active) hook(std::move(guard));
        }
        ~LoginGuardScope() { if (active) { try { hook({}); } catch (...) {} } }
    } login_guard_scope(login_target_guard_override_,
        ctx.http && !policy_.owner_sap_identities.empty() &&
            (name == "gui_session_login" || (name == "gui_session_launch" && args.is_object() && args.value("login", false)))
            ? LoginTargetGuard([this, &principal, &args, &ctx, &name, &login_pre_facts,
                                &checked_launch_facts, &anticipated_login]
                (const audit::SapFacts& facts, const std::string& connection_name) {
                if (facts.client.empty() || facts.user.empty() || !facts.connection_id)
                    return false;
                if (name == "gui_session_login" && (!login_pre_facts ||
                    facts.connection_id != login_pre_facts->connection_id ||
                    !login_identity_transition_allowed(login_pre_facts->session_identity, facts.session_identity) ||
                    facts.system != login_pre_facts->system))
                    return false;
                if (name == "gui_session_login" && policy_.owner_sap_identities.size() > 1 &&
                    !routing_state_->owns_prelogin(principal.id, *login_pre_facts->connection_id,
                                                   login_pre_facts->session_identity)) return false;
                if (name == "gui_session_launch" && (!checked_launch_facts ||
                    facts.system != checked_launch_facts->system)) return false;
                if (ctx.cancelled && ctx.cancelled()) return false;
                bool authorized = false;
                try { authorized = ctx.reauthorize && ctx.reauthorize(); } catch (...) {}
                if (!authorized) return false;
                const std::string owner = facts.system + "/" + facts.client + "/" + facts.user;
                if (!owner_identity_allowed(policy_, principal, owner)) return false;
                auth::SessionTarget target;
                target.system = facts.system + "/" + facts.client;
                target.user = facts.user;
                target.connection_name = connection_name;
                if (!auth::authorize_session_target(principal, "gui_session_login", args, target).allowed)
                    return false;
                anticipated_login = facts;
                return true;
            }) : LoginTargetGuard{});
    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_login" &&
        (!login_pre_facts || !login_target_guard_override_))
        return fail("OWNER_SESSION_UNAVAILABLE", "the SAP login target cannot be verified");
    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_launch" &&
        args.is_object() && args.value("login", false) && !login_target_guard_override_)
        return fail("OWNER_SESSION_UNAVAILABLE", "the launched SAP login cannot be verified");
    if (policy_.audit_required && mcp_audit_failed())
        return fail("AUDIT_UNAVAILABLE", "the required audit trail failed before the action");
    const bool owner_connection_list = ctx.http && !policy_.owner_sap_identities.empty() &&
                                       (name == "gui_connection_list" || name == "gui_doctor");
    const bool owner_session_list = ctx.http && !policy_.owner_sap_identities.empty() &&
                                    name == "gui_session_list";
    const auto listing_deadline = std::chrono::steady_clock::now() + owner_listing_budget_;
    if (owner_connection_list && !connection_snapshot_provider_)
        return fail("OWNER_IDENTITY_UNKNOWN", "the connection list cannot be verified");
    Result result;
    if (owner_connection_list) {
        try { result = connection_snapshot_provider_(); }
        catch (...) { return fail("OWNER_IDENTITY_UNKNOWN", "the connection list cannot be verified"); }
    } else if (owner_session_list && owner_session_listing_provider_) {
        try { result = owner_session_listing_provider_(owner_listing_budget_, ctx.cancelled); }
        catch (...) { return fail("OWNER_IDENTITY_UNKNOWN", "the session list cannot be verified"); }
    } else {
        result = invoke_selected(argv);
    }
    note_step("invoke", name, principal.name, invoke_started, record.invoke_ms);
    if (name == "gui_session_disconnect" && result.status == Result::Status::Success && owner_pre_facts) {
        // The handler may have closed/detached the old GUI window while a new
        // cache generation took its numeric ID. Only clear the ID fallback if
        // deletion of the bound saved record actually succeeded.
        const bool file_deleted = result.data.is_object() && result.data.value("file_deleted", false);
        if (session_leases_) session_leases_->retire_session(owner_pre_facts->session_identity);
        if (routing_state_) routing_state_->retire_session(owner_pre_facts->session_identity);
        if (session_policy_state_ && owner_pre_facts->connection_id) {
            if (args.is_object() && args.value("close_session", false))
                session_policy_state_->retire_session(owner_pre_facts->session_identity, *owner_pre_facts->connection_id,
                                                      file_deleted);
            else if (file_deleted)
                session_policy_state_->retire_connection(*owner_pre_facts->connection_id);
        }
    }
    if (name == "gui_session_disconnect" && result.status == Result::Status::Success && prelogin_close_facts)
        routing_state_->finish_prelogin(principal.id, *prelogin_close_facts->connection_id,
                                        prelogin_close_facts->session_identity);

    // A session can close or be replaced after the owner precheck. Reuse this check after
    // every invocation whose result might be returned, including a screenshot retry.
    const auto owner_unchanged = [&]() {
        if (!owner_pre_facts || name == "gui_session_disconnect") return true;
        std::optional<audit::SapFacts> current;
        try { current = facts_provider_(record.connection); } catch (...) {}
        if (current && current->connection_id == owner_pre_facts->connection_id &&
            current->session_identity == owner_pre_facts->session_identity &&
            current->system == owner_pre_facts->system && current->client == owner_pre_facts->client &&
            current->user == owner_pre_facts->user) record.sap = *current;
        return current && current->connection_id == record.connection &&
            current->session_identity == owner_pre_facts->session_identity &&
            current->system == owner_pre_facts->system && current->client == owner_pre_facts->client &&
            current->user == owner_pre_facts->user;
    };
    if (!owner_unchanged())
        return fail("OUTCOME_UNKNOWN", "the SAP session changed while the call was running; its result is withheld");

    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_list") {
        const auto listing_interrupted = [&] {
            return std::chrono::steady_clock::now() >= listing_deadline || (ctx.cancelled && ctx.cancelled());
        };
        if (listing_interrupted())
            return fail("OWNER_IDENTITY_UNKNOWN", "the session list could not be verified in time");
        if (result.status != Result::Status::Success || !result.data.is_object() ||
            !result.data.contains("connections") || !result.data["connections"].is_array() ||
            (!owner_session_listing_provider_ && !target_resolver_))
            return fail("OWNER_IDENTITY_UNKNOWN", "the session list could not be verified against the endpoint owner's SAP identity");
        json kept = json::array();
        long long total_sessions = 0;
        for (const auto& connection : result.data["connections"]) {
            if (listing_interrupted())
                return fail("OWNER_IDENTITY_UNKNOWN", "the session list could not be verified in time");
            if (!connection.is_object() || !connection.contains("sessions") || !connection["sessions"].is_array())
                return fail("OWNER_IDENTITY_UNKNOWN", "the session list could not be verified against the endpoint owner's SAP identity");
            std::map<std::string, json> groups;
            for (const auto& session : connection["sessions"]) {
                if (listing_interrupted())
                    return fail("OWNER_IDENTITY_UNKNOWN", "the session list could not be verified in time");
                if (!session.is_object() || !session.contains("id") || !session["id"].is_string()) continue;
                TargetQuery query;
                query.session_id = session["id"].get<std::string>();
                auth::SessionTarget target;
                if (owner_session_listing_provider_) {
                    if (!session.contains("verified_target") || !session["verified_target"].is_object()) continue;
                    const auto& verified = session["verified_target"];
                    if (!verified.contains("system") || !verified["system"].is_string() ||
                        !verified.contains("user") || !verified["user"].is_string() ||
                        !verified.contains("connection_name") || !verified["connection_name"].is_string()) continue;
                    target.system = verified["system"].get<std::string>();
                    target.user = verified["user"].get<std::string>();
                    target.connection_name = verified["connection_name"].get<std::string>();
                } else {
                    try { target = target_resolver_(query); } catch (...) { target.ambiguous = true; }
                }
                if (listing_interrupted())
                    return fail("OWNER_IDENTITY_UNKNOWN", "the session list could not be verified in time");
                if (target.ambiguous || target.system.empty() || target.user.empty()) continue;
                const std::string identity = target.system + "/" + target.user;
                if (!owner_identity_allowed(policy_, principal, identity)) continue;
                if (!principal.sap_systems.empty() && !auth::system_allowed(principal.sap_systems, target.system)) continue;
                // Enumeration metadata may describe an earlier occupant of a reused SAP session ID.
                // Only return the ID that was resolved and metadata from that live resolution.
                if (!groups.count(target.connection_name)) groups[target.connection_name] = json::array();
                groups[target.connection_name].push_back({{"id", query.session_id}});
                ++total_sessions;
            }
            for (auto& [connection_name, sessions] : groups) {
                json visible = {{"sessions", std::move(sessions)}};
                visible["session_count"] = visible["sessions"].size();
                if (!connection_name.empty()) visible["description"] = connection_name;
                kept.push_back(std::move(visible));
            }
        }
        if (listing_interrupted())
            return fail("OWNER_IDENTITY_UNKNOWN", "the session list could not be verified in time");
        const auto total_connections = kept.size();
        result.data = {{"connections", std::move(kept)}, {"total_connections", total_connections},
                       {"total_sessions", total_sessions}};
        result.diagnostics = json();
    }

    if (owner_connection_list) {
        if (std::chrono::steady_clock::now() >= listing_deadline || (ctx.cancelled && ctx.cancelled()))
            return fail("OWNER_IDENTITY_UNKNOWN", "the connection list could not be verified in time");
        if (result.status != Result::Status::Success || !result.data.is_object() ||
            !result.data.contains("connections") || !result.data["connections"].is_array() ||
            !facts_provider_ || !target_resolver_)
            return fail("OWNER_IDENTITY_UNKNOWN", "the connection list could not be verified against the endpoint owner's SAP identity");
        // A busy SAP window can hold a facts probe for five seconds. The snapshot
        // itself performs no COM validation; this budget covers the full listing.
        json kept = json::array();
        for (const auto& row : result.data["connections"]) {
            if (std::chrono::steady_clock::now() >= listing_deadline || (ctx.cancelled && ctx.cancelled()))
                return fail("OWNER_IDENTITY_UNKNOWN", "the connection list could not be verified in time");
            if (!row.is_object())
                return fail("OWNER_IDENTITY_UNKNOWN", "the connection list could not be verified against the endpoint owner's SAP identity");
            const auto id = row.contains("id") ? int_from_json(row["id"]) : std::nullopt;
            if (!id || *id < 0 ||
                !row.contains("session_id") || !row["session_id"].is_string() ||
                !row.contains("server_session_key") || !row["server_session_key"].is_string() ||
                !row.contains("cache_generation") || !row["cache_generation"].is_string()) continue;
            const std::string session_id = row["session_id"].get<std::string>();
            const std::string key = row["server_session_key"].get<std::string>();
            const std::string generation = row["cache_generation"].get<std::string>();
            if (session_id.empty() || key.empty() || generation.empty()) continue;
            std::optional<audit::SapFacts> facts;
            auth::SessionTarget target;
            target.ambiguous = true;
            try {
                facts = facts_provider_(*id);
                if (std::chrono::steady_clock::now() < listing_deadline && !(ctx.cancelled && ctx.cancelled()))
                    target = target_resolver_(TargetQuery{"", "", *id});
            } catch (...) { continue; }
            if (std::chrono::steady_clock::now() >= listing_deadline || (ctx.cancelled && ctx.cancelled()))
                return fail("OWNER_IDENTITY_UNKNOWN", "the connection list could not be verified in time");
            if (!facts || facts->connection_id != id ||
                facts->session_identity != session_id + "|" + key + "|" + generation ||
                facts->system.empty() || facts->client.empty() || facts->user.empty() ||
                target.ambiguous || target.system != facts->system + "/" + facts->client ||
                target.user != facts->user || target.connection_name.empty()) continue;
            const std::string identity = target.system + "/" + target.user;
            if (!owner_identity_allowed(policy_, principal, identity)) continue;
            if (!principal.sap_systems.empty() && !auth::system_allowed(principal.sap_systems, target.system)) continue;
            kept.push_back({{"id", *id}, {"session_id", session_id},
                            {"description", target.connection_name}, {"valid", true}});
        }
        const auto count = kept.size();
        result.data = {{"connections", std::move(kept)}, {"count", count}};
        result.diagnostics = json();
        if (name == "gui_doctor") {
            if (!auth::filter_listing_for_connections(principal, "gui_connection_list", result))
                return fail("OWNER_IDENTITY_UNKNOWN", "the owner diagnostic could not be filtered");
            const auto visible_count = result.data["count"].get<size_t>();
            const bool ready = visible_count > 0;
            result.data = {{"overall_health", ready ? "ok" : "warning"},
                           {"checks", json::array({{{"name", "owner_connections"},
                                                    {"status", ready ? "pass" : "warn"},
                                                    {"message", ready ? "An allowed SAP connection is ready" :
                                                                        "No allowed SAP connection is ready"},
                                                    {"count", visible_count}}})}};
        }
    }

    // Attaching changes the selected connection. Re-read the saved connection created by the attach and withhold
    // the result if its live session or SAP user differs from the pre-call target. Do this before shaping or sticky state.
    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_login" &&
        result.status == Result::Status::Success) {
        std::optional<audit::SapFacts> authenticated;
        try { authenticated = facts_provider_(record.connection); } catch (...) {}
        auth::SessionTarget target;
        target.ambiguous = true;
        try { target = target_resolver_(TargetQuery{"", "", record.connection}); } catch (...) {}
        if (!anticipated_login || !authenticated || !authenticated->connection_id ||
            authenticated->connection_id != login_pre_facts->connection_id ||
            !login_identity_transition_allowed(login_pre_facts->session_identity, authenticated->session_identity, true) ||
            authenticated->system != anticipated_login->system ||
            authenticated->client != anticipated_login->client ||
            !sap_user_matches(authenticated->user, anticipated_login->user) ||
            target.ambiguous || target.system != authenticated->system + "/" + authenticated->client ||
            target.user != authenticated->user ||
            !auth::authorize_session_target(principal, name, args, target).allowed)
            return fail("OUTCOME_UNKNOWN", "SAP login may have completed, but the authenticated session could not be verified; inspect the session or attach it again before retrying");
        record.sap = *authenticated;
        if (policy_.owner_sap_identities.size() > 1)
            routing_state_->finish_prelogin(principal.id, *login_pre_facts->connection_id,
                                            login_pre_facts->session_identity);
    }

    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_launch") {
        const json& launch_data = result.status == Result::Status::Success ? result.data
            : result.error.is_object() && result.error.contains("launch") ? result.error["launch"] : result.data;
        const auto id = launch_data.is_object() && launch_data.contains("connection_file_id")
            ? int_from_json(launch_data["connection_file_id"]) : std::nullopt;
        const auto rollback = [&] {
            if (id && launch_rollback_override_) {
                try {
                    if (!launch_rollback_override_(*id))
                        spdlog::error("owner launch rollback did not remove saved connection {}", *id);
                }
                catch (const std::exception& e) {
                    spdlog::error("owner launch rollback failed for saved connection {}: {}", *id, e.what());
                }
                catch (...) { spdlog::error("owner launch rollback failed for saved connection {}", *id); }
            }
        };
        if (result.status != Result::Status::Success && !id)
            return fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP connection is unavailable");
        const std::string session = launch_data.is_object() && launch_data.contains("session_id") &&
            launch_data["session_id"].is_string() ? launch_data["session_id"].get<std::string>() : std::string();
        if (!id || session.empty() || !checked_launch_facts || !facts_provider_ || !target_resolver_) {
            rollback();
            return fail("OUTCOME_UNKNOWN", "the launched SAP window could not be verified; inspect it before retrying");
        }
        record.connection = id;
        std::optional<audit::SapFacts> launched;
        try { launched = facts_provider_(*id); } catch (...) {}
        auth::SessionTarget target;
        target.ambiguous = true;
        try { target = target_resolver_(TargetQuery{"", "", *id}); } catch (...) {}
        const audit::SapFacts initial = *checked_launch_facts;
        if (!launched || launched->connection_id != id ||
            launched->session_identity.rfind(session + "|", 0) != 0 ||
            launched->system != initial.system ||
            (!initial.client.empty() && launched->client != initial.client &&
             !(initial.user.empty() && initial.client == "000" && initial.transaction == "S000")) ||
            (!initial.user.empty() && launched->user != initial.user) ||
            target.ambiguous || !owner_launch_guard(*launched, target.connection_name)) {
            rollback();
            return fail("OUTCOME_UNKNOWN", "the launched SAP window could not be verified; inspect it before retrying");
        }
        if (policy_.owner_sap_identities.size() > 1) {
            if (launched->user.empty()) {
                if (!routing_state_->bind_prelogin(principal.id, *id, launched->session_identity)) {
                    rollback();
                    return fail("OWNER_SESSION_UNAVAILABLE", "the SAP logon window could not be reserved for this token");
                }
            } else if (result.status != Result::Status::Success) {
                rollback();
                return fail("OUTCOME_UNKNOWN", "the launched SAP login outcome is unknown; inspect it before retrying");
            }
        }
        record.sap = *launched;
        if (result.status != Result::Status::Success) {
            const std::string prior_code = result.error.is_object() && result.error.contains("code") &&
                result.error["code"].is_string() ? result.error["code"].get<std::string>() : std::string();
            const bool unknown = prior_code == "OUTCOME_UNKNOWN";
            result.error = {{"code", unknown ? "OUTCOME_UNKNOWN" : "LOGIN_NOT_COMPLETED"},
                            {"message", unknown
                                ? "SAP login may have completed; inspect the saved connection before retrying"
                                : "SAP login did not complete; use gui_session_login on the saved connection"},
                            {"connection_file_id", *id}};
            result.data = json::object();
            result.diagnostics = json::object();
        }
    }

    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_attach" &&
        result.status != Result::Status::Success) {
        try { (void)attach_finalize_override_(false); } catch (...) {}
    }
    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_session_attach" &&
        result.status == Result::Status::Success) {
        const auto attach_fail = [&](const char* code, const char* message) {
            bool rolled_back = false;
            try { rolled_back = attach_finalize_override_(false); } catch (...) {}
            if (!rolled_back)
                return fail("OUTCOME_UNKNOWN", "the saved connection could not be rolled back after attach verification failed");
            return fail(code, message);
        };
        const auto id = result.data.is_object() && result.data.contains("connection_file_id")
                            ? int_from_json(result.data["connection_file_id"]) : std::nullopt;
        const std::string requested = args.contains("session_id") && args["session_id"].is_string()
                                          ? args["session_id"].get<std::string>() : std::string();
        if (!id || requested.empty() || !result.data.contains("session_id") ||
            !result.data["session_id"].is_string() || result.data["session_id"].get<std::string>() != requested ||
            !facts_provider_)
            return attach_fail("OWNER_IDENTITY_UNKNOWN", "the attached session's SAP identity could not be verified");
        std::optional<audit::SapFacts> attached;
        try { attached = facts_provider_(*id); } catch (...) {}
        if (!attached || attached->system.empty() || attached->client.empty() || attached->user.empty() ||
            attached->connection_id != id || attached->session_identity.rfind(requested + "|", 0) != 0)
            return attach_fail("OWNER_IDENTITY_UNKNOWN", "the attached session's SAP identity could not be verified");
        const std::string identity = attached->system + "/" + attached->client + "/" + attached->user;
        if (!owner_identity_allowed(policy_, principal, identity))
            return attach_fail("OWNER_IDENTITY_DENIED", "the attached session's SAP identity is not allowed for this endpoint");
        auth::SessionTarget current_target;
        current_target.ambiguous = true;
        try { current_target = target_resolver_(TargetQuery{"", "", *id}); } catch (...) {}
        if (current_target.ambiguous || current_target.system != attached->system + "/" + attached->client ||
            current_target.user != attached->user ||
            !auth::authorize_session_target(principal, name, args, current_target).allowed)
            return attach_fail("OWNER_SESSION_UNAVAILABLE", "the attached session is unavailable to this token");
        record.sap = *attached;
        bool committed = false;
        try { committed = attach_finalize_override_(true); } catch (...) {}
        if (!committed)
            return fail("OUTCOME_UNKNOWN", "the saved connection could not be committed after attach verification");
    }

    // 5a. tokens limited to named connections only see those in the three listings: filter the structured result
    // before anything is shaped, rendered or audited; an unexpected shape is an error, never unfiltered data.
    if (auth::listing_needs_filter(principal, name) && !auth::filter_listing_for_connections(principal, name, result))
        return fail("RESULT_FILTER_FAILED", "the listing could not be limited to the connections of token '" + principal.name +
                                             "' and is withheld");

    // 5b. post-call re-check of the transaction (same read-only facts provider as before the call). Only for tokens
    // with a T-code allowlist and only for calls that act on the screen or start a transaction. Unknown facts do not
    // flag (the next call fails closed on them anyway); a known transaction outside the allowlist does.
    // Serialised with the blocked-state check above (single executor thread), see docs/MCP_REMOTE.md.
    bool tcode_left = false;
    if (!principal.tcodes.empty() && facts_provider_ && (auth::acts_on_screen(spec->family) || name == "gui_transaction_start")) {
        std::string after;
        std::string after_program, after_screen, after_identity;
        std::optional<int> after_connection;
        // The connection the call ACTUALLY used (its result carries it), not the one resolved before the call: an automatic
        // single-connection resolution leaves record.connection empty.
        std::optional<int> used_connection = record.connection;
        if (result.status == Result::Status::Success && result.data.is_object() && result.data.contains("connection_id"))
            if (auto id = int_from_json(result.data["connection_id"])) used_connection = id;
        const auto facts_started = std::chrono::steady_clock::now();
        try {
            if (auto facts = facts_provider_(used_connection)) {
                after = auth::normalize_tcode(facts->transaction);
                after_program = facts->program;
                after_screen = facts->screen_number;
                after_identity = facts->session_identity;
                after_connection = facts->connection_id;
            }
        } catch (...) {}
        // A racing or misrouted facts lookup must not update another session's policy state.
        // Treat the facts as unknown while retaining the call's connection for conservative
        // initial-screen invalidation on that session.
        if (used_connection && after_connection && *used_connection != *after_connection) {
            after.clear();
            after_program.clear();
            after_screen.clear();
            after_identity.clear();
            after_connection.reset();
        } else if (!used_connection) used_connection = after_connection;
        const std::string post_state_key = !after_identity.empty() || used_connection
            ? session_policy_key(principal, after_identity, used_connection) : policy_state_key;
        note_step("facts_post", name, principal.name, facts_started, record.facts_post_ms);
        if (!after.empty()) {
            const bool allowed = std::any_of(principal.tcodes.begin(), principal.tcodes.end(),
                                             [&](const std::string& pat) { return auth::glob_match(pat, after); });
            if (!allowed) {
                tcode_left = true;
                set_tcode_blocked(post_state_key, true);
                if (used_connection) set_tcode_blocked(session_policy_key(principal, "", used_connection), true);
                record.tcode_left_allowlist = true;
                if (principal.allow_selection_input) set_initial_screen(post_state_key, std::nullopt);
            } else if (name == "gui_transaction_start" && result.status == Result::Status::Success) {
                set_tcode_blocked(post_state_key, false);
                if (used_connection) set_tcode_blocked(session_policy_key(principal, "", used_connection), false);
                // The screen the start ended on is the only one typing is allowed on (selection-input rule). Unknown
                // program/screen records nothing, so typing stays denied until a start with known facts succeeds.
                if (principal.allow_selection_input) {
                    auth::InitialScreen screen{after, after_program, after_screen, used_connection, after_identity};
                    set_initial_screen(post_state_key, screen.known() ? std::optional<auth::InitialScreen>(screen) : std::nullopt);
                }
            }
        }
        // Typing is only possible right after gui_transaction_start until the first navigation: any later call whose
        // post-call facts are uncertain (empty) or show another transaction/program/screen clears the record, so returning
        // to the initial screen later does NOT re-enable typing until gui_transaction_start runs again.
        if (principal.allow_selection_input && name != "gui_transaction_start") {
            const auto invalid = [&](const auth::InitialScreen& rec) {
                return after.empty() || after_program.empty() || after_screen.empty() ||
                    after != rec.transaction || after_program != rec.program || after_screen != rec.screen_number;
            };
            if (auto rec = initial_screen(post_state_key); rec && invalid(*rec))
                set_initial_screen(post_state_key, std::nullopt);
            // A timed-out or misrouted post-call lookup can fall back to a connection key.
            // Invalidate the identity-keyed record checked before the call as well.
            if (post_state_key != policy_state_key)
                if (auto rec = initial_screen(policy_state_key); rec && invalid(*rec))
                    set_initial_screen(policy_state_key, std::nullopt);
        }
    }

    // gui_screen_capture: enforce the image cap with one retry at half scale.
    if (spec->output == ToolOutput::Image && result.status == Result::Status::Success &&
        image_payload_bytes(result) > policy_.max_image_bytes) {
        double current = 1.0;
        if (args.contains("scale") && args["scale"].is_number()) current = args["scale"].get<double>();
        const double halved = current / 2;
        if (halved >= 0.01) {
            json retry_args = args;
            retry_args["scale"] = halved;
            try {
                auto retry_argv = spec->build_argv(retry_args, effective);
                record.argv = retry_argv;
                const auto retry_started = std::chrono::steady_clock::now();
                result = invoke_selected(retry_argv);
                note_step("invoke", name, principal.name, retry_started, record.invoke_ms);
                if (!owner_unchanged())
                    return fail("OUTCOME_UNKNOWN", "the SAP session changed while the call was running; its result is withheld");
            } catch (const std::exception&) {}
        }
        if (result.status == Result::Status::Success && image_payload_bytes(result) > policy_.max_image_bytes)
            return fail("IMAGE_TOO_LARGE",
                        "screenshot is " + std::to_string(image_payload_bytes(result)) + " bytes, above the " +
                            std::to_string(policy_.max_image_bytes) + " byte limit",
                        "pass a smaller `scale` (e.g. " + scale_text(std::max(halved / 2, 0.05)) +
                            ") or crop with x, y, width, height");
    }

    // 6. shape
    ToolSpec shaped_spec = *spec;
    if (spec->output == ToolOutput::Markdown) {
        std::string format = policy_.default_format;
        if (args.contains("format") && args["format"].is_string()) format = args["format"].get<std::string>();
        if (format == "json") shaped_spec.output = ToolOutput::Json;
    }
    std::optional<int> connection = record.connection;
    const std::string header = kScreenDataTools.count(name) ? make_untrusted_header(result, connection) : std::string();
    const std::set<std::string> visible_now = visible_tool_names(&principal);
    ToolResult shaped = shape_result(result, shaped_spec, policy_, header, &visible_now);
    if (ctx.http && !policy_.owner_sap_identities.empty() && name == "gui_screen_read" &&
        result.status == Result::Status::Success && owner_pre_facts && facts_provider_) {
        std::optional<audit::SapFacts> current;
        try { current = facts_provider_(record.connection); } catch (...) {}
        if (current && current->connection_id == owner_pre_facts->connection_id &&
            current->session_identity == owner_pre_facts->session_identity) {
            const std::string guard = screen_guard_for(*current);
            if (!guard.empty() && guard == screen_guard_for(*owner_pre_facts)) {
                if (!shaped.structured || !shaped.structured->is_object()) {
                    // A text (Markdown) result has no structured part. Clients such as Claude Code hand
                    // structuredContent to the model when it is present, so a guard-only object would hide the
                    // screen: carry the text along.
                    std::string text;
                    for (const auto& item : shaped.content)
                        if (item.is_object() && item.value("type", "") == "text") text += item.value("text", "");
                    shaped.structured = json::object();
                    (*shaped.structured)["text"] = text;
                }
                (*shaped.structured)["screen_guard"] = guard;
            }
        }
    }
    if (result.status != Result::Status::Success) code = result_error_code(result);
    else if (result.data.is_object() && result.data.contains("connection_id")) {
        if (auto id = int_from_json(result.data["connection_id"])) record.connection = id;
    }

    if (tcode_left) {
        const std::string warning = "WARNING tcode_left_allowlist: this call ended outside the token's allowed transactions; "
                                    "further screen calls are denied until gui_transaction_start opens an allowed transaction.";
        if (!shaped.content.empty() && shaped.content[0].is_object() && shaped.content[0].value("type", "") == "text")
            shaped.content[0]["text"] = shaped.content[0]["text"].get<std::string>() + "\n" + warning;
        else
            shaped.content.push_back(json{{"type", "text"}, {"text", warning}});
        if (!shaped.structured || !shaped.structured->is_object()) shaped.structured = json::object();
        (*shaped.structured)["tcode_left_allowlist"] = true;
    }

    // 8. sticky default connection
    if (result.status == Result::Status::Success && (name == "gui_session_attach" || name == "gui_session_launch") &&
        result.data.is_object() && result.data.contains("connection_file_id")) {
        if (auto id = int_from_json(result.data["connection_file_id"])) {
            std::string identity;
            if (facts_provider_) {
                try {
                    const auto facts = facts_provider_(*id);
                    if (facts && facts->connection_id == *id) identity = facts->session_identity;
                } catch (...) {}
            }
            const bool stored = !ctx.http || policy_.owner_sap_identities.empty() || !identity.empty();
            if (stored)
                set_sticky_connection(principal_key(ctx.principal), *id, std::move(identity));
            record.connection = id;
            if (stored) {
                const std::string note = "connection " + std::to_string(*id) +
                                         " is now the default for later calls that omit `connection`.";
                if (!shaped.content.empty() && shaped.content[0].is_object() && shaped.content[0].value("type", "") == "text")
                    shaped.content[0]["text"] = shaped.content[0]["text"].get<std::string>() + "\n" + note;
            }
        }
    }
    return shaped;
}

ToolResult CommandDispatcher::run_batch(const json& raw_args, const CallContext& ctx) {
    const ToolSpec* spec = find_spec("gui_batch");
    json args = raw_args.is_null() ? json::object() : raw_args;

    // Reject the whole batch while a required audit sink is unavailable. If a
    // denial append repairs the sink, a later request may run; this batch may not.
    if (policy_.audit_required && (!hook_ || mcp_audit_failed()))
        return audited("gui_batch", [&](McpCallRecord&, std::string& code) {
            code = "AUDIT_UNAVAILABLE";
            return error_result(code, "The required audit trail is unavailable; no batch action was run.");
        }, ctx, nullptr);

    // Validate the whole batch up front (one audit record when it is rejected).
    std::string problem;
    try {
        validate_tool_arguments(args, spec->def.input_schema);
        for (const auto& item : args["items"])
            if (item["tool"] == "gui_batch") throw std::invalid_argument("gui_batch cannot be nested inside gui_batch");
    } catch (const std::exception& e) {
        problem = e.what();
    }
    if (!problem.empty())
        return audited("gui_batch", [&](McpCallRecord&, std::string& code) {
            code = "INVALID_ARGUMENT";
            return error_result("INVALID_ARGUMENT", problem);
        }, ctx, nullptr);

    // Token authorization of the whole batch up front (scope "batch" + the static rules of every item; the
    // server's own read-only refusals and the SAP-system rule are applied per item at execution time).
    {
        Policy static_policy = policy_;
        static_policy.read_only = false;
        const PolicyDecision authz = auth::authorize_call(ctx.principal, *spec, spec->family, args, static_policy, std::nullopt,
                                                          std::nullopt, [this](const std::string& n) { return find_spec(n); });
        if (!authz.allowed)
            return audited("gui_batch", [&](McpCallRecord&, std::string& code) {
                code = authz.code.empty() ? "REFUSED" : authz.code;
                return error_result(code, authz.message);
            }, ctx, nullptr);
    }

    auto batch_fail = [&](const std::string& reason, const std::string& message) {
        return audited("gui_batch", [&](McpCallRecord&, std::string& code) {
            code = reason;
            return error_result(reason, message);
        }, ctx, nullptr);
    };
    json items = args["items"];
    struct BatchPin {
        std::shared_ptr<SessionLeases> leases;
        std::string session, token, id;
        bool active = false;
        ~BatchPin() {
            if (active) leases->end_write(session, token, id, SessionLeases::Clock::now());
        }
    } pin;
    CallContext item_ctx = ctx;
    if (ctx.http && !policy_.owner_sap_identities.empty()) {
        if (!args.contains("connection"))
            return batch_fail("BATCH_SESSION_REQUIRED", "owner-mode batches need an explicit saved connection");
        const int connection = args["connection"].get<int>();
        const std::string lease_id = args.value("lease_id", std::string());
        bool needs_lease = false;
        for (auto& item : items) {
            const std::string name = item["tool"].get<std::string>();
            const ToolSpec* item_spec = find_spec(name);
            if (!item_spec || (item_spec->family != "screen" && item_spec->family != "element" &&
                               item_spec->family != "menu" && item_spec->family != "key" &&
                               item_spec->family != "popup" && item_spec->family != "transaction"))
                return batch_fail("BATCH_SESSION_REQUIRED", "owner-mode batches may contain only calls to one established SAP session");
            json arguments = item.value("arguments", json::object());
            if (arguments.contains("connection") && arguments["connection"] != connection)
                return batch_fail("BATCH_MIXED_SESSIONS", "batch items target different saved connections");
            arguments["connection"] = connection;
            if (arguments.contains("lease_id") && arguments["lease_id"] != lease_id)
                return batch_fail("BATCH_MIXED_LEASES", "batch items present different session leases");
            if (lease_required_for_call(name, arguments)) {
                needs_lease = true;
                if (lease_id.empty()) return batch_fail("LEASE_REQUIRED", "acquire a session lease for this batch");
                arguments["lease_id"] = lease_id;
            }
            item["arguments"] = std::move(arguments);
        }
        std::optional<audit::SapFacts> facts;
        if (facts_provider_) {
            try { facts = facts_provider_(connection); } catch (...) {}
        }
        if (!facts || facts->connection_id != connection || facts->system.empty() || facts->client.empty() ||
            facts->user.empty() || facts->session_identity.empty() ||
            !owner_identity_allowed(policy_, ctx.principal,
                facts->system + "/" + facts->client + "/" + facts->user))
            return batch_fail("OWNER_SESSION_UNAVAILABLE", "the requested SAP session is unavailable");
        if (needs_lease) {
            if (!session_leases_ || ctx.principal.id.empty())
                return batch_fail("LEASE_UNAVAILABLE", "the session lease cannot be verified");
            pin.leases = session_leases_;
            pin.session = facts->session_identity;
            pin.token = ctx.principal.id;
            pin.id = lease_id;
            pin.active = pin.leases->begin_write(pin.session, pin.token, pin.id, SessionLeases::Clock::now());
            if (!pin.active) return batch_fail("LEASE_REQUIRED", "the session lease is missing or expired");
            item_ctx.batch_lease = CallContext::BatchLease{pin.session, pin.token, pin.id, connection};
        }
    }

    const bool stop_on_error = args.value("stop_on_error", true);
    json summary = json::array();
    std::string body;
    json images = json::array();
    bool any_failed = false, stopped = false, left_allowlist = false;
    const std::size_t total = items.size();
    std::size_t index = 0;
    for (const auto& item : items) {
        ++index;
        const std::string tool = item["tool"].get<std::string>();
        if (stopped || (ctx.cancelled && ctx.cancelled())) {
            if (!stopped) any_failed = true;
            stopped = true;
            summary.push_back({{"tool", tool}, {"ok", false}, {"skipped", true}});
            if (ctx.report_progress) {
                try { ctx.report_progress(static_cast<double>(index), static_cast<double>(total), "batch item skipped"); }
                catch (...) {}
            }
            continue;
        }
        std::string code;
        ToolResult r = run_single(tool, item.contains("arguments") ? item["arguments"] : json::object(), item_ctx, &code);
        json entry = {{"tool", tool}, {"ok", !r.is_error}};
        if (r.structured && r.structured->is_object() && r.structured->contains("tcode_left_allowlist")) {
            entry["tcode_left_allowlist"] = true;
            left_allowlist = true;
        }
        if (r.is_error) {
            entry["error_code"] = code;
            any_failed = true;
            if (stop_on_error) stopped = true;
        }
        summary.push_back(entry);
        body += "\n--- [" + std::to_string(index) + "/" + std::to_string(total) + "] " + tool + " ---\n" + text_of(r);
        for (const auto& block : r.content)
            if (block.is_object() && block.value("type", "") == "image") images.push_back(block);
        if (ctx.report_progress) {
            try { ctx.report_progress(static_cast<double>(index), static_cast<double>(total), "batch item completed"); }
            catch (...) {}
        }
    }

    ToolResult out;
    out.is_error = any_failed;
    out.content.push_back(json{{"type", "text"}, {"text", dump_compact(summary) + body}});
    for (const auto& image : images) out.content.push_back(image);
    if (left_allowlist) out.structured = json{{"tcode_left_allowlist", true}};
    return out;
}

ToolResult CommandDispatcher::call_tool(const std::string& name, const json& args, const CallContext& ctx) {
    if (name == "gui_batch" && find_spec("gui_batch")) return run_batch(args, ctx);
    return run_single(name, args, ctx);
}

// ---------------------------------------------------------------------------------------------
// Registry invoker
// ---------------------------------------------------------------------------------------------
Invoker make_registry_invoker(const std::function<cli::CommandHandler&()>& get_handler) {
    return [get_handler](const std::vector<std::string>& tool_argv) -> Result {
        CLI::App app{"fairyfly"};
        commands::build_command_tree(app);  // registry and CLI tree belong to this worker thread

        static std::once_flag renderers_registered;
        std::call_once(renderers_registered, [] { sap::renderers::register_all_renderers(); });

        std::vector<std::string> full{"fairyfly"};
        full.insert(full.end(), tool_argv.begin(), tool_argv.end());
        std::vector<const char*> ptrs;
        ptrs.reserve(full.size());
        for (const auto& a : full) ptrs.push_back(a.c_str());

        try {
            app.parse(static_cast<int>(ptrs.size()), ptrs.data());  // never app.exit(): it prints
        } catch (const CLI::ParseError& e) {
            return error_from("INVALID_ARGUMENT", e.what());
        }

        try {
            return commands::CommandRegistry::instance().execute_active_command(get_handler());
        } catch (const UserError& e) {
            return error_from("USER_ERROR", e.what());
        } catch (const SystemError& e) {
            return error_from("SYSTEM_ERROR", e.what());
#ifdef _WIN32
        } catch (const _com_error& e) {
            return error_from("COM_ERROR", "COM operation failed (HRESULT " + std::to_string(static_cast<unsigned long>(e.Error())) + ")");
#endif
        } catch (const std::exception& e) {
            return error_from("INTERNAL_ERROR", e.what());
        } catch (...) {
            return error_from("INTERNAL_ERROR", "Unknown failure while executing command");
        }
    };
}

} // namespace fairyfly::mcp
