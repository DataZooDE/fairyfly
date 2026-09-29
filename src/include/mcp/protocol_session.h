#pragma once
// ProtocolSession: per-client MCP protocol state (era, negotiated version, clientInfo, initialized flag)
// plus the era-neutral method handlers shared by the stdio engine and the HTTP endpoint.
// Everything here that touches the ToolProvider runs on the main (COM) thread.

#include <string>
#include <vector>

#include "include/mcp/types.h"

namespace fairyfly::mcp {

// ---- protocol constants -------------------------------------------------------------------------
inline constexpr int kHeaderMismatch = -32020;      ///< Mcp-Method / Mcp-Name header disagrees with the body
inline constexpr int kUnsupportedVersion = -32022;  ///< protocol version not supported (data.supported lists ours)
inline constexpr const char* kStatelessVersion = "2026-07-28";

/// One request/notification handed to a session.
struct Pending {
    json id;  ///< null for notifications
    std::string method;
    json params;
};

/// {"content":[{"type":"text","text":...}] [, "isError":true]}
json text_result(const std::string& text, bool is_error);
/// tools/list entry of one tool.
json tool_to_json(const ToolDef& tool);
/// initialize result; `version` is the negotiated protocol version.
json make_initialize_result(const ServerOptions& options, const std::string& version);
/// Picks the version to answer an initialize with: the client's when in `supported`, else `supported.front()`.
std::string negotiate_version(const std::vector<std::string>& supported, const std::string& requested);

/// Shared method handlers (return a full JSON-RPC message). Main thread.
json tools_list_message(ToolProvider& provider, const Pending& p, bool sort_by_name);
json set_level_message(const Pending& p);
/// Validates params, checks has_tool (unknown => -32602), runs the tool and maps the ToolResult
/// (exceptions become an isError result). `ctx.request_id` is filled from p.id when null.
json call_tool_message(ToolProvider& provider, const Pending& p, CallContext ctx);

/// The stdio protocol state machine: ONE implicit session, legacy era, initialize required.
class ProtocolSession {
public:
    ProtocolSession(ToolProvider& provider, const ServerOptions& options) : provider_(provider), options_(options) {}

    /// Handles one queued message on the main thread. Returns the JSON-RPC response, or null for
    /// notifications / unknown notifications. `ctx_base` seeds the CallContext of a tools/call
    /// (principal/era/http); `cancelled` polls the call's cancel flag.
    json process(const Pending& p, std::function<bool()> cancelled, const CallContext& ctx_base = CallContext{});

    bool initialized() const { return initialized_; }
    bool ready() const { return ready_; }
    const std::string& negotiated_version() const { return version_; }
    const json& client_info() const { return client_info_; }

private:
    json handle_initialize(const Pending& p);

    ToolProvider& provider_;
    const ServerOptions& options_;
    bool initialized_ = false;
    bool ready_ = false;
    std::string version_;
    json client_info_;
};

} // namespace fairyfly::mcp
