#include "include/config/client_config.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace fairyfly::config {

namespace {

using ordered = nlohmann::ordered_json;

bool looks_like_token(const std::string& s) {
    static const std::regex token(R"(ffy_[A-Za-z0-9]+_)");
    return std::regex_search(s, token);
}

std::string dollar(const std::string& env) { return "${" + env + "}"; }

std::string claude_code_command(const ClientConfigOptions& o) {
    return "claude mcp add --transport http " + o.name + " " + o.url + " --header \"Authorization: Bearer " +
           dollar(o.token_env) + "\"\n";
}

std::string claude_code_json(const ClientConfigOptions& o) {
    ordered server;
    server["type"] = "http";
    server["url"] = o.url;
    server["headers"] = ordered{{"Authorization", "Bearer " + dollar(o.token_env)}};
    ordered root;
    root["mcpServers"] = ordered::object();
    root["mcpServers"][o.name] = server;
    return root.dump(2) + "\n";
}

const char* kTrustNote =
    "Certificate created by `fairyfly mcp setup --self-signed`: export it with `fairyfly mcp cert export` and trust it\n"
    "on the client (curl --cacert, NODE_EXTRA_CA_CERTS, Windows certutil); see docs/MCP_SETUP.md.";

const char* kAuthHeaderVar = "FAIRYFLY_AUTH_HEADER";

std::string claude_desktop_json(const ClientConfigOptions& o) {
    ordered server;
    server["command"] = "npx";
    server["args"] = ordered::array({"-y", "mcp-remote", o.url, "--header", std::string("Authorization:") + dollar(kAuthHeaderVar)});
    server["env"] = ordered{{kAuthHeaderVar, "Bearer <paste-your-token-here>"}};
    ordered root;
    root["mcpServers"] = ordered::object();
    root["mcpServers"][o.name] = server;
    return root.dump(2) + "\n";
}

std::string mcp_remote_command(const ClientConfigOptions& o) {
    return std::string("export ") + kAuthHeaderVar + "=\"Bearer " + dollar(o.token_env) + "\"\n" +
           "npx -y mcp-remote " + o.url + " --header \"Authorization:" + dollar(kAuthHeaderVar) + "\"\n";
}

std::string curl_smoke(const ClientConfigOptions& o) {
    const std::string common = "curl -sS --cacert fairyfly.cer -X POST \"$URL\" \\\n"
                               "  -H \"Authorization: Bearer $" + o.token_env + "\" \\\n"
                               "  -H \"Content-Type: application/json\" \\\n"
                               "  -H \"Accept: application/json, text/event-stream\" \\\n";
    return "export " + o.token_env + "='<paste-your-token-here>'\n"
           "URL=" + o.url + "\n\n"
           "# 1. initialize\n" + common +
           "  -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\","
           "\"capabilities\":{},\"clientInfo\":{\"name\":\"curl\",\"version\":\"0\"}}}'\n\n"
           "# 2. tools/list\n" + common +
           "  -d '{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}'\n";
}

std::string stdio_local(const ClientConfigOptions& o) {
    ordered server;
    server["command"] = "fairyfly";
    server["args"] = ordered::array({"mcp"});
    ordered root;
    root["mcpServers"] = ordered::object();
    root["mcpServers"][o.name] = server;
    return "claude mcp add " + o.name + " -- fairyfly mcp\n\n" + root.dump(2) + "\n";
}

} // namespace

std::string validate_client_options(const ClientConfigOptions& o) {
    static const std::regex url_re(R"(^https?://[^\s'"`$\\@]+$)");
    static const std::regex env_re(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
    static const std::regex name_re(R"(^[A-Za-z0-9_-]{1,64}$)");
    if (!std::regex_match(o.url, url_re)) return "--url must be an http(s) URL without spaces, quotes, '$' or credentials";
    if (looks_like_token(o.url)) return "--url looks like it contains a token; tokens are never embedded";
    if (!std::regex_match(o.token_env, env_re)) return "--token-env must be the NAME of an environment variable";
    if (looks_like_token(o.token_env)) return "--token-env must be a variable name, not the token itself";
    if (!std::regex_match(o.name, name_re)) return "--name may only contain letters, digits, '-' and '_'";
    return {};
}

std::vector<ClientSnippet> build_client_configs(const ClientConfigOptions& o) {
    std::vector<ClientSnippet> out;
    const bool all = o.all();
    if (all || o.claude_code) {
        out.push_back({"claude-code", "Claude Code: claude mcp add",
                       "Set the token first: export " + o.token_env + "=<your token> (create one with: fairyfly mcp token create).\n"
                       "The shell expands the variable when you run the command, so the token ends up in your Claude config.\n"
                       "To keep it out, use the .mcp.json variant below: Claude Code expands ${VAR} when it loads the file.",
                       claude_code_command(o)});
        out.push_back({"claude-code-json", "Claude Code: .mcp.json (project scope)",
                       "Claude Code reads " + dollar(o.token_env) + " from the environment at startup.", claude_code_json(o)});
    }
    if (all || o.claude_desktop) {
        out.push_back({"claude-desktop", "Claude Desktop: claude_desktop_config.json via mcp-remote",
                       "Requires Node.js (npx). The header value lives in an environment variable because mcp-remote on\n"
                       "Windows mishandles arguments that contain spaces (\"Bearer <token>\").\n"
                       "Replace the placeholder in \"env\" with your token; do not commit this file.\n" +
                       std::string(kTrustNote) + "\nNode/mcp-remote: NODE_EXTRA_CA_CERTS=<exported .pem>.",
                       claude_desktop_json(o)});
    }
    if (all || o.mcp_remote) {
        out.push_back({"mcp-remote", "mcp-remote on the command line",
                       "Same env indirection as above (space-free --header argument).\n" + std::string(kTrustNote),
                       mcp_remote_command(o)});
    }
    if (all || o.curl) {
        out.push_back({"curl", "curl smoke test (Linux/macOS shell)",
                       "Replace the placeholder with your token. fairyfly.cer is the exported server certificate (never use -k).\n"
                       "Expected: an initialize result, then the tool list.\n" + std::string(kTrustNote),
                       curl_smoke(o)});
    }
    if (all || o.stdio) {
        out.push_back({"stdio", "Local stdio (no network, no token)",
                       "fairyfly must be on PATH (or use its full path). Add --allow-write to args for write mode.",
                       stdio_local(o)});
    }
    return out;
}

std::string render_client_configs_text(const std::vector<ClientSnippet>& snippets) {
    std::string out;
    for (const auto& s : snippets) {
        out += "=== " + s.title + " ===\n";
        if (!s.notes.empty()) out += s.notes + "\n";
        out += "\n" + s.body + "\n";
    }
    return out;
}

nlohmann::json render_client_configs_json(const std::vector<ClientSnippet>& snippets, const ClientConfigOptions& o) {
    nlohmann::json out;
    out["url"] = o.url;
    out["token_env"] = o.token_env;
    out["name"] = o.name;
    out["configs"] = nlohmann::json::array();
    for (const auto& s : snippets)
        out["configs"].push_back({{"id", s.id}, {"title", s.title}, {"notes", s.notes}, {"config", s.body}});
    return out;
}

} // namespace fairyfly::config
