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

std::vector<ToolDef> CommandDispatcher::list_tools() const {
    std::vector<ToolDef> defs;
    for (const auto& spec : specs_)
        if (tool_visible(spec, policy_)) defs.push_back(spec.def);
    return defs;
}

std::vector<ToolDef> CommandDispatcher::list_tools_for(const Principal& principal) const {
    std::vector<ToolDef> defs;
    for (const auto& spec : specs_)
        if (tool_visible(spec, policy_) && auth::tool_allowed_for(principal, spec)) defs.push_back(spec.def);
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
    else record.connection = policy_.default_connection ? policy_.default_connection : sticky_connection(ctx.principal.name);

    // 2. policy
    const PolicyDecision decision = check_call(*spec, args, policy_);
    if (!decision.allowed)
        return fail(decision.code.empty() ? "REFUSED" : decision.code,
                    decision.message.empty() ? "call refused by policy" : decision.message);

    // 2b. token authorization (scope, token read-only, SAP system, T-code); the stdio principal allows everything
    const Principal& principal = ctx.principal;
    {
        std::optional<std::string> current_system, current_tcode;
        if ((!principal.sap_systems.empty() || !principal.tcodes.empty()) && facts_provider_) {
            if (auto facts = facts_provider_(record.connection); facts && facts->any()) {
                if (!facts->system.empty()) current_system = facts->system + "/" + facts->client;
                if (!facts->transaction.empty()) current_tcode = facts->transaction;
            }
        }
        const PolicyDecision authz = auth::authorize_call(principal, *spec, spec->family, args, policy_, current_system,
                                                          current_tcode, [this](const std::string& n) { return find_spec(n); });
        if (!authz.allowed) return fail(authz.code.empty() ? "REFUSED" : authz.code, authz.message);
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
        if (target_resolver_) {
            try { target = target_resolver_(query); } catch (...) { target = {}; }
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
    const bool rate_ok = per_principal ? keyed_limiter_.allow(principal.name, budget, std::chrono::steady_clock::now())
                                       : (!rate_gate_ || rate_gate_(std::chrono::steady_clock::now()));
    if (!rate_ok)
        return fail("RATE_LIMITED",
                    "too many tool calls (limit " + std::to_string(per_principal ? budget : policy_.max_calls_per_minute) +
                        " per minute)",
                    "wait a few seconds and retry, or combine steps with gui_batch");
    // Optional per-family budget of the token (--rate-family), on top of the overall one. Every gui_batch item counts.
    if (const int family_limit = auth::rate_family_limit(principal, spec->family); family_limit > 0) {
        if (!keyed_limiter_.allow(principal.name + "|family:" + spec->family, family_limit, std::chrono::steady_clock::now()))
            return fail("RATE_LIMITED",
                        "too many '" + spec->family + "' tool calls (family limit " + std::to_string(family_limit) + " per minute)",
                        "wait a few seconds and retry");
    }

    // A read-only token narrows the server mode for this call only (restored afterwards).
    struct ReadOnlyScope {
        const ReadOnlyOverride& hook;
        bool server_value;
        bool active;
        ReadOnlyScope(const ReadOnlyOverride& h, bool narrow, bool server)
            : hook(h), server_value(server), active(narrow && static_cast<bool>(h)) {
            if (active) hook(true);
        }
        ~ReadOnlyScope() {
            if (active) { try { hook(server_value); } catch (...) {} }
        }
    } read_only_scope(read_only_override_, principal.read_only && !policy_.read_only, policy_.read_only);

    // Effective policy: call argument > policy default > sticky default (the argument wins in build_argv).
    Policy effective = policy_;
    if (!effective.default_connection) effective.default_connection = sticky_connection(ctx.principal.name);

    // gui_session_attach without session_id: resolve through `list`.
    if (name == "gui_session_attach" && args.is_object() && !args.contains("session_id")) {
        Result listed = invoke({"session", "list"});
        if (listed.status != Result::Status::Success) {
            code = result_error_code(listed);
            return shape_result(listed, *spec, policy_, "");
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
    Result result = invoke(argv);

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
                result = invoke(retry_argv);
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
    ToolResult shaped = shape_result(result, shaped_spec, policy_, header);
    if (result.status != Result::Status::Success) code = result_error_code(result);
    else if (result.data.is_object() && result.data.contains("connection_id")) {
        if (auto id = int_from_json(result.data["connection_id"])) record.connection = id;
    }

    // 8. sticky default connection
    if (result.status == Result::Status::Success && (name == "gui_session_attach" || name == "gui_session_launch") &&
        result.data.is_object() && result.data.contains("connection_file_id")) {
        if (auto id = int_from_json(result.data["connection_file_id"])) {
            set_sticky_connection(ctx.principal.name, *id);
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
    bool any_failed = false, stopped = false;
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
