#include "include/mcp/dispatcher.h"

#include <algorithm>
#include <cmath>
#include <memory>
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
#include "include/mcp/mcp_audit.h"
#include "include/mcp/policy.h"
#include "include/mcp/result_shaper.h"
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
                    "Execute the selection with gui_key_send (F8 or Enter). Values are echoed nowhere: results and audit logs do not "
                    "contain them. Give exactly one of value or clear.";
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
    record.remote_addr = ctx.principal.remote_addr;
    record.transport = transport_label(ctx.http);
    record.era = ctx.http ? (ctx.era == ProtocolEra::Stateless ? "stateless" : "legacy") : "";
    record.request_id = ctx.request_id.is_null() ? std::string() : dump_compact(ctx.request_id);
    if (record.request_id.size() > 64) record.request_id.resize(64);

    std::string code;
    ToolResult result;
    try {
        result = fn(record, code);
    } catch (const std::exception& e) {
        result = error_result("INTERNAL_ERROR", e.what());
        code = "INTERNAL_ERROR";
    } catch (...) {
        result = error_result("INTERNAL_ERROR", "unexpected failure");
        code = "INTERNAL_ERROR";
    }
    if (result.is_error && code.empty()) code = "ERROR";
    record.status = result.is_error ? "error" : "success";
    record.error_code = result.is_error ? code : std::string();
    record.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started).count();
    if (hook_) {
        try { hook_(record); } catch (...) {}
        // Required-audit mode: the action already ran, but the model and user must learn that it
        // could not be recorded (mirrors the CLI's exit-code rule for --audit-required).
        if (policy_.audit_required && mcp_audit_failed()) {
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
        code = c;
        return error_result(c, message, hint);
    };

    // 1. spec
    const ToolSpec* spec = find_spec(name);
    if (!spec) return fail("TOOL_NOT_FOUND", "unknown tool '" + name + "'", "use tools/list to see the available tools");
    json args = raw_args.is_null() ? json::object() : raw_args;
    if (args.is_object() && args.contains("connection")) record.connection = int_from_json(args["connection"]);
    else record.connection = policy_.default_connection ? policy_.default_connection : sticky_connection(principal_key(ctx.principal));

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
    {
        std::optional<std::string> current_system, current_tcode;
        auth::SelectionInputContext input_context;
        if ((!principal.sap_systems.empty() || !principal.tcodes.empty()) && facts_provider_) {
            const auto started = std::chrono::steady_clock::now();
            std::optional<audit::SapFacts> facts;
            try { facts = facts_provider_(record.connection); } catch (...) {}
            note_step("facts_pre", name, principal.name, started, record.facts_pre_ms);
            if (facts && facts->any()) {
                if (!facts->system.empty()) current_system = facts->system + "/" + facts->client;
                if (!facts->transaction.empty()) current_tcode = facts->transaction;
                input_context.program = facts->program;
                input_context.screen_number = facts->screen_number;
            }
        }
        if (principal.allow_selection_input) {
            input_context.initial = initial_screen(principal_key(principal));
            input_context.connection = record.connection;
            // Any call that observes another (or an unknown) screen than the recorded one ends the typing window for good:
            // coming back to the initial screen later needs a new gui_transaction_start.
            if (input_context.initial && facts_provider_ && name != "gui_transaction_start" &&
                (!current_tcode || auth::normalize_tcode(*current_tcode) != input_context.initial->transaction ||
                 input_context.program != input_context.initial->program ||
                 input_context.screen_number != input_context.initial->screen_number)) {
                set_initial_screen(principal_key(principal), std::nullopt);
                input_context.initial.reset();
            }
        }
        const PolicyDecision authz = auth::authorize_call(principal, *spec, spec->family, args, policy_, current_system,
                                                          current_tcode, [this](const std::string& n) { return find_spec(n); },
                                                          &input_context);
        if (!authz.allowed) return fail(authz.code.empty() ? "REFUSED" : authz.code, authz.message);
        // Atomicity: call_tool runs only on the executor (main) thread, one call at a time (ToolProvider contract, CallExecutor
        // FIFO), so this check -> invoke -> set_tcode_blocked sequence cannot interleave with another call of the same token.
        // A token that ended its previous call outside its T-code allowlist stays locked out of screen-acting tools
        // until gui_transaction_start succeeds with an allowed code (nothing navigates back automatically).
        if (!principal.tcodes.empty() && auth::acts_on_screen(spec->family) && tcode_blocked(principal_key(principal)))
            return fail("TCODE_DENIED", "an earlier call of token '" + principal.name + "' left its allowed transactions; screen "
                                        "tools stay blocked until gui_transaction_start opens an allowed transaction");
    }

    // 2c. session/connection targets: the SAP-system and saved-connection allowlists also bind the calls that launch,
    // log on, attach, disconnect (or act through an explicit `connection`). The target is resolved read-only, without
    // contacting SAP; when it cannot be determined the call is refused (fail closed).
    const auto check_target = [&](const json& call_args) -> std::optional<PolicyDecision> {
        if (!auth::needs_session_target(principal, name, call_args)) return std::nullopt;
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
        auto decision = auth::authorize_session_target(principal, name, call_args, target);
        if (decision.allowed) return std::nullopt;
        return decision;
    };
    const bool attach_needs_resolution = name == "gui_session_attach" && args.is_object() && !args.contains("session_id");
    if (!attach_needs_resolution)
        if (auto refused = check_target(args)) return fail(refused->code.empty() ? "REFUSED" : refused->code, refused->message);

    // 3. rate limit (per principal for tokens; the server-wide gate for the local stdio principal)
    const bool per_principal = principal.rate_per_minute > 0 || principal.name != "stdio";
    const int budget = principal.rate_per_minute > 0 ? principal.rate_per_minute : policy_.max_calls_per_minute;
    const bool rate_ok = per_principal ? keyed_limiter_.allow(principal_key(principal), budget, std::chrono::steady_clock::now())
                                       : (!rate_gate_ || rate_gate_(std::chrono::steady_clock::now()));
    if (!rate_ok)
        return fail("RATE_LIMITED",
                    "too many tool calls (limit " + std::to_string(per_principal ? budget : policy_.max_calls_per_minute) +
                        " per minute)",
                    "wait a few seconds and retry, or combine steps with gui_batch");
    // Optional per-family budget of the token (--rate-family), on top of the overall one. Every gui_batch item counts.
    if (const int family_limit = auth::rate_family_limit(principal, spec->family); family_limit > 0) {
        if (!keyed_limiter_.allow(principal_key(principal) + "|family:" + spec->family, family_limit, std::chrono::steady_clock::now()))
            return fail("RATE_LIMITED",
                        "too many '" + spec->family + "' tool calls (family limit " + std::to_string(family_limit) + " per minute)",
                        "wait a few seconds and retry");
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

    // Effective policy: call argument > policy default > sticky default (the argument wins in build_argv).
    Policy effective = policy_;
    if (!effective.default_connection) effective.default_connection = sticky_connection(principal_key(ctx.principal));

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

    // 5. invoke
    if (selection_input) record.input_allowed = true;  // authorized above; the typed value is never recorded
    const auto invoke_started = std::chrono::steady_clock::now();
    Result result = invoke(argv);
    note_step("invoke", name, principal.name, invoke_started, record.invoke_ms);

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
        std::string after_program, after_screen;
        const auto facts_started = std::chrono::steady_clock::now();
        try {
            if (auto facts = facts_provider_(record.connection)) {
                after = auth::normalize_tcode(facts->transaction);
                after_program = facts->program;
                after_screen = facts->screen_number;
            }
        } catch (...) {}
        note_step("facts_post", name, principal.name, facts_started, record.facts_post_ms);
        if (!after.empty()) {
            const bool allowed = std::any_of(principal.tcodes.begin(), principal.tcodes.end(),
                                             [&](const std::string& pat) { return auth::glob_match(pat, after); });
            if (!allowed) {
                tcode_left = true;
                set_tcode_blocked(principal_key(principal), true);
                record.tcode_left_allowlist = true;
                if (principal.allow_selection_input) set_initial_screen(principal_key(principal), std::nullopt);
            } else if (name == "gui_transaction_start" && result.status == Result::Status::Success) {
                set_tcode_blocked(principal_key(principal), false);
                // The screen the start ended on is the only one typing is allowed on (selection-input rule). Unknown
                // program/screen records nothing, so typing stays denied until a start with known facts succeeds.
                if (principal.allow_selection_input) {
                    auth::InitialScreen screen{after, after_program, after_screen, record.connection};
                    set_initial_screen(principal_key(principal), screen.known() ? std::optional<auth::InitialScreen>(screen) : std::nullopt);
                }
            }
        }
        // Typing is only possible right after gui_transaction_start until the first navigation: any later call whose
        // post-call facts are uncertain (empty) or show another transaction/program/screen clears the record, so returning
        // to the initial screen later does NOT re-enable typing until gui_transaction_start runs again.
        if (principal.allow_selection_input && name != "gui_transaction_start") {
            if (auto rec = initial_screen(principal_key(principal)); rec) {
                if (after.empty() || after_program.empty() || after_screen.empty() || after != rec->transaction ||
                    after_program != rec->program || after_screen != rec->screen_number)
                    set_initial_screen(principal_key(principal), std::nullopt);
            }
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
                result = invoke(retry_argv);
                note_step("invoke", name, principal.name, retry_started, record.invoke_ms);
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
            set_sticky_connection(principal_key(ctx.principal), *id);
            record.connection = id;
            const std::string note = "connection " + std::to_string(*id) +
                                     " is now the default for later calls that omit `connection`.";
            if (!shaped.content.empty() && shaped.content[0].is_object() && shaped.content[0].value("type", "") == "text")
                shaped.content[0]["text"] = shaped.content[0]["text"].get<std::string>() + "\n" + note;
        }
    }
    return shaped;
}

ToolResult CommandDispatcher::run_batch(const json& raw_args, const CallContext& ctx) {
    const ToolSpec* spec = find_spec("gui_batch");
    json args = raw_args.is_null() ? json::object() : raw_args;

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

    const bool stop_on_error = args.value("stop_on_error", true);
    json summary = json::array();
    std::string body;
    json images = json::array();
    bool any_failed = false, stopped = false, left_allowlist = false;
    const std::size_t total = args["items"].size();
    std::size_t index = 0;
    for (const auto& item : args["items"]) {
        ++index;
        const std::string tool = item["tool"].get<std::string>();
        if (stopped || (ctx.cancelled && ctx.cancelled())) {
            if (!stopped) any_failed = true;
            stopped = true;
            summary.push_back({{"tool", tool}, {"ok", false}, {"skipped", true}});
            continue;
        }
        std::string code;
        ToolResult r = run_single(tool, item.contains("arguments") ? item["arguments"] : json::object(), ctx, &code);
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
        // The command registry is a process-wide singleton: one invocation at a time.
        static std::mutex registry_mutex;
        std::lock_guard<std::mutex> lock(registry_mutex);

        CLI::App app{"fairyfly"};
        commands::build_command_tree(app);  // register_all_commands() destroys the previous invocation's command objects

        static bool renderers_registered = false;
        if (!renderers_registered) {
            sap::renderers::register_all_renderers();
            renderers_registered = true;
        }

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
