#pragma once

#include "include/config/environment.h"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fairyfly::audit {

/// Non-secret facts about the SAP session a command ran against.
struct SapFacts {
    std::string system;
    std::string client;
    std::string user;
    std::string transaction;
    std::string program;        ///< Info.Program of the current screen (empty when unknown); used for the initial-screen rule, not audited
    std::string screen_number;  ///< Info.ScreenNumber as text (empty when unknown)
    /// Connection file id the lookup ACTUALLY resolved (also through automatic single-connection resolution); not audited.
    std::optional<int> connection_id;
    /// Identity of the live session behind that connection (session path + server session key + cache generation; empty
    /// when unknown); used to bind the selection-input record to one session, never audited or shown.
    std::string session_identity;
    std::string window_title; ///< active SAP window title, only for screen-scoped checks; never audited

    bool any() const {
        return !system.empty() || !client.empty() || !user.empty() || !transaction.empty();
    }
};

/// One audit entry. Never holds error messages, results, screen content or cell values.
struct AuditRecord {
    std::chrono::system_clock::time_point ts{};
    int pid = 0;                       ///< 0 = current process id
    std::string command;               ///< invoked subcommand name ("" when none)
    std::vector<std::string> argv;     ///< arguments without argv[0]; redacted by format_record
    std::optional<int> connection;
    std::optional<SapFacts> sap;
    bool read_only = false;
    std::optional<std::size_t> batch_line;
    std::string status = "success";    ///< "success" | "error"
    std::string error_code;            ///< machine code only (never the message)
    int exit_code = 0;
    long long duration_ms = 0;
    std::string source = "cli";        ///< "cli" | "mcp" (emitted as audit_source)
    std::string tool;                  ///< MCP tool name (emitted only when non-empty)
    std::string client;                ///< MCP client "name/version" (emitted only when non-empty)
    std::string request_id;            ///< JSON-RPC id as text (emitted only when non-empty)
    std::string principal;             ///< remote MCP: token name (emitted only when non-empty; never a secret)
    std::string token_id;              ///< remote MCP: non-secret token issuance ID (empty for stdio/insecure)
    std::string remote_addr;           ///< remote MCP: client address (emitted only when non-empty)
    std::string transport;             ///< MCP: "stdio" | "http" (emitted only when non-empty)
    std::string era;                   ///< MCP: "legacy" | "stateless" (emitted only when non-empty)
    bool tcode_left_allowlist = false; ///< MCP: the transaction was outside the token's T-code allowlist after the call (emitted only when true)
    bool input_allowed = false;        ///< MCP: a read-only token typed into a selection field (--allow-selection-input); emitted only when true, never the value
    long long facts_pre_ms = 0;        ///< MCP: duration of the pre-call SAP facts lookup (emitted only when > 0)
    long long invoke_ms = 0;           ///< MCP: duration of the invocation itself (emitted only when > 0)
    long long facts_post_ms = 0;       ///< MCP: duration of the post-call facts re-check (emitted only when > 0)
    std::optional<long long> retention_days;  ///< audit.pruned: the retention that was applied (emitted only when set)
    std::vector<std::string> pruned;          ///< audit.pruned: deleted trail file names, never content (emitted when non-empty)
    std::vector<std::string> prune_failed;    ///< audit.pruned: file names whose delete was refused (emitted when non-empty)
    std::vector<std::string> prune_candidates;  ///< audit.prune_intent: the files about to be deleted (emitted when non-empty)
};

enum class Mode { Enabled, Disabled, Required };

struct AuditConfig {
    Mode mode = Mode::Enabled;
    std::filesystem::path file;
};

using GetEnvFn = config::EnvLookup;

/// "YYYY-MM.jsonl" for the UTC month of `now`.
std::string monthly_file_name(std::chrono::system_clock::time_point now);

/// ISO-8601 UTC with milliseconds, e.g. 2026-09-29T12:34:56.789Z.
std::string utc_timestamp_ms(std::chrono::system_clock::time_point now);

/// The audit settings of mcp.yaml (`audit.enabled`, `audit.required`, `audit.file`); unset = not configured.
struct AuditFileSettings {
    std::optional<bool> enabled;
    std::optional<bool> required;
    std::optional<std::string> file;
    std::optional<long long> retention_days;   ///< audit.retention_days (unset or 0 = keep forever; audit_retention.h)
};

/// Resolve mode and file, flag > environment > mcp.yaml > default.
/// File: FAIRYFLY_AUDIT_FILE > audit.file > %USERPROFILE%\.fairyfly\audit\YYYY-MM.jsonl (none without a profile: fail closed).
/// Mode: audit.enabled/required, then FAIRYFLY_AUDIT (0|false|no|off -> Disabled, required -> Required,
/// 1|true|yes|on -> Enabled), then --no-audit; --audit-required wins over every disable (fail safe).
AuditConfig resolve_config(const GetEnvFn& getenv_fn, bool cli_no_audit, bool cli_required,
                           std::chrono::system_clock::time_point now, const AuditFileSettings& file_settings = {});

/// Pure redaction of an argv (without argv[0]); applies element and array caps.
std::vector<std::string> redact_argv(const std::vector<std::string>& argv);

/// Single JSON line terminated by '\n', at most 16 KiB.
std::string format_record(const AuditRecord& record);

/// Required-mode pre-flight decision: the error code to return before running a command,
/// or nullopt to proceed.
std::optional<std::string> preflight_error(Mode mode, bool probe_ok);

/// Append-only sink. Never opens the file with write (overwrite) access.
class AuditSink {
public:
    explicit AuditSink(AuditConfig config) : config_(std::move(config)) {}

    Mode mode() const { return config_.mode; }
    bool enabled() const { return config_.mode != Mode::Disabled; }
    const std::filesystem::path& file() const { return config_.file; }
    /// New settings once mcp.yaml is read (before the server starts; not thread-safe against append).
    void reconfigure(AuditConfig config) { config_ = std::move(config); }

    /// Create directory/file if needed; true when appending is possible.
    bool probe() noexcept;
    /// One WriteFile per record. Swallows every failure (warns once per process).
    bool append(const AuditRecord& record) noexcept;

private:
    AuditConfig config_;
};

} // namespace fairyfly::audit
