#pragma once
// FROZEN shared contract of the MCP server (Phase 0). Three modules are implemented in parallel
// against this header; change it only through the orchestrator.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/core.h"  // fairyfly::Result

namespace fairyfly::mcp {

using json = nlohmann::json;
using fairyfly::Result;

// ---- JSON-RPC 2.0 / MCP error codes ---------------------------------------------------------
inline constexpr int kParseError = -32700;           ///< line is not valid JSON
inline constexpr int kInvalidRequest = -32600;       ///< valid JSON but not a valid JSON-RPC message
inline constexpr int kMethodNotFound = -32601;       ///< unknown method (also used for server/discover)
inline constexpr int kInvalidParams = -32602;        ///< bad params (unknown tool, malformed arguments)
inline constexpr int kInternalError = -32603;        ///< unexpected server failure
inline constexpr int kServerNotInitialized = -32002; ///< request before initialize/initialized
inline constexpr int kServerBusy = -32000;           ///< call queue full (max_queue) or timed out

// ---- Transport ------------------------------------------------------------------------------
/// One newline-delimited JSON message per line. Implementations: StdioTransport, StringTransport.
class Transport {
public:
    virtual ~Transport() = default;
    /// Reads one line (without the trailing '\n' / '\r\n'). Returns false on EOF.
    virtual bool read_line(std::string& line) = 0;
    /// Thread-safe: writes `line` + '\n' atomically w.r.t. other writers and flushes.
    virtual void write_line(std::string_view line) = 0;
};

// ---- Tool model -----------------------------------------------------------------------------
/// What tools/list advertises for one tool.
struct ToolDef {
    std::string name;         ///< e.g. "sap_screen_read" (all tools are prefixed sap_)
    std::string title;        ///< human readable title
    std::string description;  ///< model-facing description
    json input_schema;        ///< JSON Schema object for the arguments
    json annotations;         ///< MCP annotations (readOnlyHint, destructiveHint, ...); null = none
    json meta;                ///< optional _meta object; null = none
};

/// What a tool call returns (mapped to the MCP tools/call result).
struct ToolResult {
    json content = json::array();   ///< MCP content blocks ({"type":"text","text":...}, image, ...)
    std::optional<json> structured; ///< structuredContent, when present
    bool is_error = false;          ///< tool-level failure (NOT a JSON-RPC error)
};

/// Per-call context handed to the provider by the server.
struct CallContext {
    json request_id;                       ///< JSON-RPC id of the tools/call request
    std::optional<json> progress_token;    ///< params._meta.progressToken when supplied
    std::function<bool()> cancelled;       ///< polled by long calls; true after notifications/cancelled
};

/// Backend of the server: exposes tools and runs them. Called ONLY from the main (COM) thread,
/// one call at a time.
class ToolProvider {
public:
    virtual ~ToolProvider() = default;
    virtual std::vector<ToolDef> list_tools() const = 0;
    virtual ToolResult call_tool(const std::string& name, const json& args, const CallContext& ctx) = 0;
    /// Receives initialize.params.clientInfo (for auditing). Default: ignore.
    virtual void set_client_info(const json& client_info) { (void)client_info; }
};

/// Protocol-level server settings.
struct ServerOptions {
    std::string name = "fairyfly";
    std::string version;       ///< server version reported in initialize
    std::string instructions;  ///< initialize.instructions
    /// Newest first; the server echoes the client's version when supported, else its newest.
    std::vector<std::string> protocol_versions = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
    std::size_t max_queue = 16;      ///< max queued tools/call requests (beyond: kServerBusy)
    int call_timeout_ms = 120000;    ///< soft per-call timeout
};

/// Effective runtime policy used by the dispatcher/policy/shaper (after env cap and flags).
struct Policy {
    bool read_only = true;                   ///< write tools hidden and refused
    bool allow_write = false;                ///< explicit opt-in that turned read_only off
    std::optional<int> default_connection;   ///< used when a tool call omits "connection"
    std::string default_format = "markdown"; ///< "markdown" | "json" for text-y tools
    std::size_t max_result_chars = 60000;    ///< truncation cap for text results
    std::size_t max_image_bytes = 2 * 1024 * 1024; ///< cap for base64-decoded image payloads
    int max_calls_per_minute = 120;          ///< RateLimiter budget
};

/// Parsed options of `fairyfly serve` (raw CLI values, before the env cap is applied).
struct ServeOptions {
    bool read_only = false;
    bool allow_write = false;
    std::optional<int> default_connection;
    std::string format = "markdown";
    std::size_t max_result_chars = 60000;
    std::size_t max_image_bytes = 2097152;
    int max_calls_per_minute = 120;
    int call_timeout_ms = 120000;
    std::string transport = "stdio"; ///< only "stdio" is implemented
    int port = 0;                    ///< reserved for a future http transport
};

/// How a tool renders its result.
enum class ToolOutput { Text, Markdown, Json, Image };

/// Catalog entry: tool definition + how to turn arguments into a CLI invocation.
struct ToolSpec {
    ToolDef def;
    bool write_tool = false;            ///< state-changing: hidden and refused in read-only mode
    ToolOutput output = ToolOutput::Json;
    /// Returns the FULL CLI argv WITHOUT the leading program name, e.g. {"screen","read","--tab","t"}.
    /// Throws std::invalid_argument with a model-readable message for bad arguments.
    std::function<std::vector<std::string>(const json& args, const Policy&)> build_argv;
};

/// Outcome of a policy check for one call.
struct PolicyDecision {
    bool allowed = true;
    std::string code;     ///< machine code when refused (e.g. READ_ONLY, RATE_LIMITED)
    std::string message;  ///< model-readable reason when refused
};

/// One audited MCP tool call (mapped onto audit::AuditRecord by mcp_audit).
struct McpCallRecord {
    std::string tool;                  ///< MCP tool name
    std::string command;               ///< CLI subcommand it ran ("" if refused before running)
    std::vector<std::string> argv;     ///< argv without program name (redacted downstream)
    std::optional<int> connection;
    std::string status;                ///< "success" | "error" | "refused"
    std::string error_code;            ///< machine code only, never messages
    std::string client;                ///< "name/version" of the MCP client
    std::string request_id;            ///< JSON-RPC id rendered as text
    long long duration_ms = 0;
    bool read_only = true;
};

/// Receives one record per tools/call. Never throws.
using AuditHook = std::function<void(const McpCallRecord&)>;

/// Runs one CLI invocation (argv WITHOUT program name) through the command registry and returns
/// its Result. Main thread only.
using Invoker = std::function<Result(const std::vector<std::string>& argv)>;

} // namespace fairyfly::mcp
