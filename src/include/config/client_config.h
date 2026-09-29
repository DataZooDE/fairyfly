#pragma once
// `fairyfly mcp client-config`: ready-to-paste client configurations for the remote MCP server.
// Pure text generation. Only PLACEHOLDERS are ever printed, never a real token.

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fairyfly::config {

struct ClientConfigOptions {
    std::string url = "https://vm:8443/mcp";
    std::string token_env = "FAIRYFLY_TOKEN";   ///< NAME of the environment variable that holds the token
    std::string name = "fairyfly";              ///< server name in the client
    bool claude_code = false;
    bool claude_desktop = false;
    bool mcp_remote = false;
    bool curl = false;
    bool stdio = false;                         ///< local stdio variant (`fairyfly mcp`)
    /// No selector = everything.
    bool all() const { return !claude_code && !claude_desktop && !mcp_remote && !curl && !stdio; }
};

struct ClientSnippet {
    std::string id;      ///< claude-code, claude-code-json, claude-desktop, mcp-remote, curl, stdio
    std::string title;
    std::string notes;   ///< plain-text hints (one per line)
    std::string body;    ///< the paste-ready text
};

/// Validation: returns an error message (empty = ok). Rejects token-looking values, bad names and URLs.
std::string validate_client_options(const ClientConfigOptions& options);

std::vector<ClientSnippet> build_client_configs(const ClientConfigOptions& options);

std::string render_client_configs_text(const std::vector<ClientSnippet>& snippets);
nlohmann::json render_client_configs_json(const std::vector<ClientSnippet>& snippets, const ClientConfigOptions& options);

} // namespace fairyfly::config
