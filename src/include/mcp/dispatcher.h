#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <set>
#include <string>
#include <vector>
#include "include/auth/authorize.h"
#include "include/cli_handler.h"
#include "include/mcp/policy.h"
#include "include/mcp/session_leases.h"
#include "include/mcp/session_policy_state.h"
#include "include/mcp/session_routing_state.h"
#include "include/mcp/session_worker_protocol.h"
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// ToolProvider that maps tool calls onto CLI argv via the catalog, checks policy, invokes the
/// command registry through `Invoker`, shapes the Result and emits an audit record.
///
/// Every call (including each item of gui_batch) runs the same pipeline: find spec -> check_call ->
/// rate limit -> build_argv -> invoke -> shape -> audit hook (exactly once) -> sticky connection.
class CommandDispatcher : public ToolProvider {
public:
    /// `specs` empty = all_tool_specs() (tests may inject a custom catalog).
    CommandDispatcher(Invoker invoker, Policy policy, AuditHook hook = nullptr,
                      std::vector<ToolSpec> specs = {});
    std::vector<ToolDef> list_tools() const override;
    /// Includes tools hidden by policy so a call to them gets the friendly policy refusal.
    bool has_tool(const std::string& name) const override { return find_spec(name) != nullptr; }
    ToolResult call_tool(const std::string& name, const json& args, const CallContext& ctx) override;
    void set_client_info(const json& client_info) override;
    /// Switches between read-only and write mode (the link tray's "Allow changes in SAP"). Call it between calls, on
    /// the thread that runs them; the caller enforces the hard read-only cap.
    void set_read_only(bool read_only) {
        policy_.read_only = read_only;
        policy_.allow_write = !read_only;
    }

    /// Replaces the rate-limit decision (default: RateLimiter(policy.max_calls_per_minute)).
    /// Test seam; the gate returns false to refuse the call with RATE_LIMITED.
    void set_rate_gate(std::function<bool(std::chrono::steady_clock::time_point)> gate);
    /// Share principal and family budgets across the global control executor
    /// and all session lanes of one HTTP endpoint.
    void set_shared_rate_limiter(std::shared_ptr<KeyedRateLimiter> limiter) {
        if (limiter) keyed_limiter_ = std::move(limiter);
    }
    /// SAP system/client/transaction the call will run against (for token allowlists); nullopt = unknown.
    /// Without a provider every call of a token that has a system allowlist is denied SYSTEM_UNKNOWN.
    using SapFactsProvider = std::function<std::optional<audit::SapFacts>(std::optional<int> connection)>;
    void set_sap_facts_provider(SapFactsProvider provider) { facts_provider_ = std::move(provider); }
    using ConnectionSnapshotProvider = std::function<Result()>;
    void set_connection_snapshot_provider(ConnectionSnapshotProvider provider) { connection_snapshot_provider_ = std::move(provider); }
    using OwnerSessionListingProvider = std::function<Result(std::chrono::milliseconds,
                                                              const std::function<bool()>&)>;
    void set_owner_session_listing_provider(OwnerSessionListingProvider provider) {
        owner_session_listing_provider_ = std::move(provider);
    }
    /// Selector of a session/connection lookup (exactly one is set): a SAP Logon entry name (launch), a session id
    /// (attach) or a saved connection id (everything else).
    struct TargetQuery {
        std::string logon_name;
        std::string session_id;
        std::optional<int> connection;
    };
    /// Resolves the SAP system and connection name a call targets, without contacting SAP (token allowlists).
    /// Without a resolver every call of a token with sap_systems/connections that needs one is denied (fail closed).
    using SessionTargetResolver = std::function<auth::SessionTarget(const TargetQuery&)>;
    void set_session_target_resolver(SessionTargetResolver resolver) { target_resolver_ = std::move(resolver); }
    /// Per-call read-only override: called with true before a call of a read-only token while the server
    /// is in write mode, and with the server value afterwards. Wire it to CommandHandler::set_read_only.
    using ReadOnlyOverride = std::function<void(bool read_only)>;
    void set_read_only_override(ReadOnlyOverride hook) { read_only_override_ = std::move(hook); }
    /// Selection-input mode of the handler for ONE authorized gui_element_fill call: (true, program, screen) before the call,
    /// (false, "", "") afterwards on every path. Set together with the lifted read-only guard.
    using SelectionInputOverride = std::function<void(bool on, const std::string& program, const std::string& screen_number)>;
    void set_selection_input_override(SelectionInputOverride hook) { selection_input_override_ = std::move(hook); }
    /// Bind a checked owner and saved-session generation to one handler invocation. Empty values clear it.
    using OwnerSessionOverride = std::function<void(const std::string& session_identity, const std::string& sap_identity)>;
    void set_owner_session_override(OwnerSessionOverride hook) { owner_session_override_ = std::move(hook); }
    using AttachTargetGuard = std::function<bool(const audit::SapFacts&, const std::string& connection_name)>;
    using AttachTargetGuardOverride = std::function<void(AttachTargetGuard)>;
    void set_attach_target_guard_override(AttachTargetGuardOverride hook) {
        attach_target_guard_override_ = std::move(hook);
    }
    using LoginTargetGuard = std::function<bool(const audit::SapFacts&, const std::string& connection_name)>;
    using LoginTargetGuardOverride = std::function<void(LoginTargetGuard)>;
    void set_login_target_guard_override(LoginTargetGuardOverride hook) {
        login_target_guard_override_ = std::move(hook);
    }
    using LaunchTargetGuard = std::function<bool(const audit::SapFacts&, const std::string& connection_name)>;
    using LaunchTargetGuardOverride = std::function<void(LaunchTargetGuard)>;
    void set_launch_target_guard_override(LaunchTargetGuardOverride hook) {
        launch_target_guard_override_ = std::move(hook);
    }
    using LaunchRollbackOverride = std::function<bool(int connection_id)>;
    void set_launch_rollback_override(LaunchRollbackOverride hook) {
        launch_rollback_override_ = std::move(hook);
    }
    using AttachFinalizeOverride = std::function<bool(bool accepted)>;
    void set_attach_finalize_override(AttachFinalizeOverride hook) {
        attach_finalize_override_ = std::move(hook);
    }
    using LoginLaneReserver = std::function<std::shared_ptr<void>(const std::string& lane_key,
                                                                  const std::function<bool()>& cancelled)>;
    void set_login_lane_reserver(LoginLaneReserver hook) { login_lane_reserver_ = std::move(hook); }
    void set_control_probe_gate(std::shared_ptr<std::shared_timed_mutex> gate) {
        control_probe_gate_ = std::move(gate);
    }
    /// Execute an already authorized, owner-bound established-session command
    /// in its session-owned worker. Control-plane commands stay on the tray.
    using SessionInvoker = std::function<Result(const WorkerCall&)>;
    void set_session_invoker(SessionInvoker hook) { session_invoker_ = std::move(hook); }
    using SessionInvokerWithGate = std::function<Result(const WorkerCall&, const std::function<void()>&)>;
    void set_session_invoker_with_gate(SessionInvokerWithGate hook) { session_invoker_with_gate_ = std::move(hook); }
    void set_session_leases(std::shared_ptr<SessionLeases> leases) { session_leases_ = std::move(leases); }
    void set_routing_state(std::shared_ptr<SessionRoutingState> routing) { routing_state_ = std::move(routing); }
    void set_session_policy_state(std::shared_ptr<SessionPolicyState> state) { session_policy_state_ = std::move(state); }
    /// Reports a step (facts_pre, invoke, facts_post) that took longer than the slow-step threshold (default 2000 ms). The
    /// default logs one spdlog warning; tests replace it.
    using SlowStepReporter = std::function<void(const std::string& step, const std::string& tool,
                                                const std::string& principal, long long elapsed_ms)>;
    void set_slow_step_reporter(SlowStepReporter reporter) { slow_step_reporter_ = std::move(reporter); }
    void set_slow_step_threshold(std::chrono::milliseconds threshold) { slow_step_threshold_ = threshold; }
    /// Maximum cooperative verification time for owner-scoped discovery.
    void set_owner_listing_budget(std::chrono::milliseconds budget) {
        if (budget > std::chrono::milliseconds::zero()) owner_listing_budget_ = budget;
    }
    /// tools/list for one principal: hides tools outside its scopes and, for read-only tokens, write tools.
    std::vector<ToolDef> list_tools_for(const Principal& principal) const override;
    /// Connection remembered from the last successful gui_session_attach / gui_session_launch OF THIS PRINCIPAL
    /// (HTTP tokens by id, see principal_key(); the local stdio principal is "stdio"). One principal attaching never retargets
    /// another; only the targeting is separate, the SAP GUI session and its screen state are shared.
    std::optional<int> sticky_connection(const std::string& principal_name = "stdio") const {
        return routing_state_->sticky_connection(principal_name);
    }

private:
    /// Owner mode: an HTTP call on an endpoint that is limited to exact SAP identities (--owner-identity).
    bool owner_mode(const CallContext& ctx) const { return ctx.http && !policy_.owner_sap_identities.empty(); }
    /// After a call of a token with a T-code allowlist: the transaction the session ended in. Outside the allowlist the
    /// token's screen tools are blocked until gui_transaction_start opens an allowed one; true then. Also keeps or
    /// clears the selection-input initial screen.
    bool recheck_transaction(const Principal& principal, const ToolSpec& spec, const std::string& name, const Result& result,
                             McpCallRecord& record, const std::string& policy_state_key);
    /// A row of an owner-mode listing is shown: its identity is the endpoint's and the token's, its system allowed.
    bool owner_target_visible(const Principal& principal, const auth::SessionTarget& target) const;
    /// A refusal before anything runs: the code, the message and a hint for the assistant.
    struct CallRefusal {
        std::string code;
        std::string message;
        std::string hint = {};
    };
    /// Admission of a call, before any SAP lookup: policy, token scope, owner binding, rate limits, owner-mode
    /// discovery and launch rules. Charges the rate limiters. nullopt: admitted.
    /// Owner-mode listings, verified row by row (dispatcher.cpp); a refusal when they cannot be verified in time.
    std::optional<CallRefusal> filter_owner_session_listing(Result& result, const Principal& principal,
                                                            const std::function<bool()>& listing_interrupted);
    std::optional<CallRefusal> filter_owner_connection_listing(Result& result, const std::string& name, const Principal& principal,
                                                               const std::function<bool()>& listing_interrupted);
    void auto_attach_and_retry(Result& result, const std::string& name, const Principal& principal, McpCallRecord& record,
                               const std::function<Result()>& retry);
    // execute_call's shaping steps (dispatcher.cpp)
    void hint_single_fills(Result& result);
    void attach_screen_guard(ToolResult& shaped, const McpCallRecord& record, const audit::SapFacts& owner_pre_facts);
    static void mark_tcode_left(ToolResult& shaped);
    void remember_sticky_connection(ToolResult& shaped, const Result& result, McpCallRecord& record, const CallContext& ctx);
    /// execute_call's refusal: maps the owner-mode boundary codes and records the code.
    using RefuseCall = std::function<ToolResult(const std::string& code, const std::string& message, const std::string& hint)>;
    ToolResult run_session_lease(const ToolSpec& spec, const json& args, const Principal& principal, McpCallRecord& record,
                                 const std::optional<audit::SapFacts>& owner_pre_facts, const CallContext& ctx,
                                 const RefuseCall& fail);
    std::optional<CallRefusal> verify_owner_login_outcome(const Principal& principal, const json& args, McpCallRecord& record,
                                                          const std::optional<audit::SapFacts>& login_pre_facts,
                                                          const std::optional<audit::SapFacts>& anticipated_login);
    std::optional<CallRefusal> verify_owner_launch_outcome(Result& result, const Principal& principal, McpCallRecord& record,
                                                           const std::optional<audit::SapFacts>& checked_launch_facts,
                                                           const LaunchTargetGuard& owner_launch_guard);
    std::optional<CallRefusal> prepare_owner_batch(const json& args, const CallContext& ctx, json& items, int& connection,
                                                   std::string& lease_id, bool& needs_lease,
                                                   std::optional<audit::SapFacts>& facts);
    std::optional<int> reattach_only_session_for(const ToolSpec& spec, const std::string& name, const Principal& principal,
                                                 const std::string& recorded, const std::string& stale_id,
                                                 std::string& reattach_note, bool& reattach_refused);
    std::optional<PolicyDecision> session_target_refusal(const std::string& name, const json& call_args,
                                                         const CallContext& ctx, const Principal& principal,
                                                         std::optional<int> connection, bool prelogin_close);
    std::optional<CallRefusal> admit_owner_identity(const Principal& principal, const std::optional<audit::SapFacts>& live_facts,
                                                    McpCallRecord& record);
    void track_initial_screen(const std::string& name, const std::string& policy_state_key, std::optional<int> connection,
                              const std::optional<std::string>& current_tcode, auth::SelectionInputContext& input_context);
    std::optional<CallRefusal> authorization_refusal(const ToolSpec& spec, const std::string& name, const json& args,
                                                     const CallContext& ctx, const Principal& principal,
                                                     const std::optional<std::string>& current_system,
                                                     const std::optional<std::string>& current_tcode,
                                                     auth::SelectionInputContext& input_context,
                                                     const std::string& policy_state_key,
                                                     const std::string& policy_fallback_key);
    Result invoke_on_worker(const std::vector<std::string>& selected_argv, const std::string& name, const json& args,
                            const CallContext& ctx, const Principal& principal, std::optional<int> connection,
                            const audit::SapFacts& owner_pre_facts, const std::string& expected_screen_guard,
                            bool selection_input, const auth::InitialScreen& selection_initial);
    void retire_disconnected_session(const Result& result, const json& args, const audit::SapFacts& owner_pre_facts);
    bool owner_session_unchanged(const audit::SapFacts& owner_pre_facts, McpCallRecord& record);
    struct CallState;   // dispatcher.cpp
    bool acquire_probe_gate(CallState& state, const CallContext& ctx);
    std::optional<ToolResult> admit_live_session(const std::string& name, McpCallRecord& record, const CallContext& ctx,
                                                 CallState& state, const RefuseCall& refuse);
    std::optional<CallRefusal> verify_owner_attach_outcome(const Result& result, const std::string& name, const json& args,
                                                           const Principal& principal, McpCallRecord& record);
    std::optional<CallRefusal> enforce_image_cap(Result& result, const std::string& name, const json& args,
                                                 const ToolSpec& spec, const Policy& effective, const Principal& principal,
                                                 McpCallRecord& record,
                                                 const std::function<Result(const std::vector<std::string>&)>& invoke_selected,
                                                 const std::function<bool()>& owner_unchanged);
    ToolResult shape_call(const std::string& name, Result& result, McpCallRecord& record, std::string& code,
                          const CallContext& ctx, const CallState& state, bool tcode_left);
    bool owner_launch_target_allowed(const audit::SapFacts& facts, const std::string& connection_name,
                                     const Principal& principal, const json& args, const CallContext& ctx,
                                     std::optional<audit::SapFacts>& checked_launch_facts);
    bool owner_login_target_allowed(const audit::SapFacts& facts, const std::string& connection_name, const std::string& name,
                                    const Principal& principal, const json& args, const CallContext& ctx,
                                    const std::optional<audit::SapFacts>& login_pre_facts,
                                    const std::optional<audit::SapFacts>& checked_launch_facts,
                                    std::optional<audit::SapFacts>& anticipated_login);
    struct LeaseWriteScope;   // dispatcher.cpp
    std::optional<ToolResult> resolve_attach_session(const ToolSpec& spec, json& args, std::string& code,
                                                     const Principal& principal,
                                                     const std::function<std::optional<PolicyDecision>(const json&)>& check_target,
                                                     const RefuseCall& refuse);
    std::optional<CallRefusal> begin_owner_lease(const json& args, const CallContext& ctx, const Principal& principal,
                                                 const std::optional<audit::SapFacts>& owner_pre_facts,
                                                 const McpCallRecord& record, LeaseWriteScope& lease_scope);
    std::optional<CallRefusal> check_screen_guard(const json& args, const CallContext& ctx,
                                                  const std::optional<audit::SapFacts>& owner_pre_facts,
                                                  std::string& expected_screen_guard);
    void retry_after_reattach(Result& result, const ToolSpec& spec, const json& args, const Policy& effective,
                              const std::vector<std::string>& argv, McpCallRecord& record,
                              const std::function<std::optional<int>(const std::string&, const std::string&)>& reattach,
                              const std::function<Result(const std::vector<std::string>&)>& invoke_selected);
    std::optional<ToolResult> admit_call(const std::string& name, const json& raw_args, McpCallRecord& record,
                                         const CallContext& ctx, CallState& state, const RefuseCall& refuse);
    // owner-mode branches of execute_call's pre-call checks (dispatcher.cpp)
    std::optional<CallRefusal> reserve_owner_login(const Principal& principal, const std::optional<audit::SapFacts>& live_facts,
                                                   McpCallRecord& record, const CallContext& ctx,
                                                   std::optional<audit::SapFacts>& login_pre_facts,
                                                   std::shared_ptr<void>& login_lane_reservation,
                                                   const std::function<bool()>& acquire_control_probe_gate);
    std::optional<CallRefusal> admit_prelogin_close(const Principal& principal, const std::optional<audit::SapFacts>& live_facts,
                                                    const McpCallRecord& record, std::optional<std::string>& current_system);
    std::optional<CallRefusal> admission_refusal(const ToolSpec* spec, const std::string& name, const json& args,
                                                 const CallContext& ctx, bool selection_input);
    /// steady_clock ms of the last successful single-field gui_element_fill (0 = none): a second one soon after gets a
    /// hint to use `fields`.
    std::atomic<long long> last_single_fill_ms_{0};

    ToolResult run_single(const std::string& name, const json& args, const CallContext& ctx,
                          std::string* code_out = nullptr);
    ToolResult execute_call(const std::string& name, const json& args, McpCallRecord& record, std::string& code,
                            const CallContext& ctx);
    ToolResult run_batch(const json& args, const CallContext& ctx);
    ToolResult audited(const std::string& tool, const std::function<ToolResult(McpCallRecord&, std::string&)>& fn,
                       const CallContext& ctx, std::string* code_out);
    Result invoke(const std::vector<std::string>& argv);
    const ToolSpec* find_spec(const std::string& name) const;
    /// Names of the tools a caller can call (server policy and, when given, the principal's scopes / read-only flag).
    std::set<std::string> visible_tool_names(const Principal* principal) const;

    Invoker invoker_;
    Policy policy_;
    AuditHook hook_;
    std::vector<ToolSpec> specs_;
    json client_info_;
    std::string client_;
    RateLimiter limiter_;
    std::function<bool(std::chrono::steady_clock::time_point)> rate_gate_;
    void set_sticky_connection(const std::string& principal_name, int id, std::string session_identity,
                               std::string sap_identity = "") {
        routing_state_->set_sticky_connection(principal_name, id, std::move(session_identity), std::move(sap_identity));
    }
    std::shared_ptr<SessionRoutingState> routing_state_ = std::make_shared<SessionRoutingState>();
    /// Principals whose last screen-acting call ended outside their T-code allowlist: their next screen-acting
    /// call is denied until an allowed gui_transaction_start succeeds.
    bool tcode_blocked(const std::string& principal_name) const {
        return session_policy_state_->tcode_blocked(principal_name);
    }
    void set_tcode_blocked(const std::string& principal_name, bool blocked) {
        session_policy_state_->set_tcode_blocked(principal_name, blocked);
    }
    /// Per principal (principal_key): the screen reached by its last successful gui_transaction_start that ended in an
    /// allowlisted transaction. Only recorded for principals with allow_selection_input (selection-input rule).
    std::optional<auth::InitialScreen> initial_screen(const std::string& key) const {
        return session_policy_state_->initial_screen(key);
    }
    void set_initial_screen(const std::string& key, const std::optional<auth::InitialScreen>& screen) {
        session_policy_state_->set_initial_screen(key, screen);
    }
    std::shared_ptr<SessionPolicyState> session_policy_state_ = std::make_shared<SessionPolicyState>();
    std::shared_ptr<KeyedRateLimiter> keyed_limiter_ = std::make_shared<KeyedRateLimiter>();
    SapFactsProvider facts_provider_;
    ConnectionSnapshotProvider connection_snapshot_provider_;
    OwnerSessionListingProvider owner_session_listing_provider_;
    SessionTargetResolver target_resolver_;
    ReadOnlyOverride read_only_override_;
    SelectionInputOverride selection_input_override_;
    OwnerSessionOverride owner_session_override_;
    AttachTargetGuardOverride attach_target_guard_override_;
    LoginTargetGuardOverride login_target_guard_override_;
    LaunchTargetGuardOverride launch_target_guard_override_;
    LaunchRollbackOverride launch_rollback_override_;
    AttachFinalizeOverride attach_finalize_override_;
    LoginLaneReserver login_lane_reserver_;
    std::shared_ptr<std::shared_timed_mutex> control_probe_gate_;
    SessionInvoker session_invoker_;
    SessionInvokerWithGate session_invoker_with_gate_;
    std::shared_ptr<SessionLeases> session_leases_;
    SlowStepReporter slow_step_reporter_;
    std::chrono::milliseconds slow_step_threshold_{2000};
    std::chrono::milliseconds owner_listing_budget_{8000};
    /// Times one step of a call: stores the elapsed ms in `out_ms` and reports it when it exceeds the threshold.
    void note_step(const char* step, const std::string& tool, const std::string& principal,
                   std::chrono::steady_clock::time_point started, long long& out_ms) const;
};

/// "slow MCP step: step=<step> tool=<tool> principal=<token name> elapsed_ms=<n>": the warning logged for a step over the
/// threshold. The principal is the token NAME, never the secret.
std::string format_slow_step_message(const std::string& step, const std::string& tool, const std::string& principal,
                                     long long elapsed_ms);

/// Real Invoker: builds a private CLI::App, register_all_commands(), setup_all_commands, parses the
/// argv (prefixed with a program name, no app.exit), then execute_active_command(get_handler()).
/// The command registry is thread-local. Callers must give each worker its own CommandHandler and
/// serialize calls to the same SAP session. Never writes to stdout/stderr.
Invoker make_registry_invoker(const std::function<cli::CommandHandler&()>& get_handler);

} // namespace fairyfly::mcp
