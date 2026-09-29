#pragma once
// YAML configuration of `fairyfly mcp` (phase 4 of the remote-MCP work).
//
// Design: one key table (`key_specs()`) drives parsing, validation, precedence, `mcp config show`
// and the commented template, so they cannot drift apart. A McpConfig only holds the keys that are
// PRESENT in one layer (YAML, environment or command-line flags); `resolve()` stacks the layers
// (flag > env > YAML > default) and remembers where each effective value came from.
//
// The YAML never holds secrets: unknown keys and secret-like keys warn, secret-like values are
// refused (CONFIG_CONTAINS_SECRET). Everything here is pure (no file/registry/process access) except
// the two small file helpers at the bottom.

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "include/mcp/types.h"

namespace fairyfly::config {

using Value = std::variant<std::string, long long, bool, std::vector<std::string>>;

enum class ValueType { String, Int, Bool, StringList };
enum class Source { Default, Yaml, Env, Flag };

const char* source_name(Source source);

/// One documented key of the config file ("server.port").
struct KeySpec {
    std::string key;                    ///< "section.name"
    ValueType type = ValueType::String;
    std::optional<Value> def;           ///< default; nullopt = unset by default
    std::string doc;                    ///< one-line description (template comment)
    std::optional<long long> min;       ///< Int range
    std::optional<long long> max;
    std::vector<std::string> allowed;   ///< String enum (empty = any)
    std::string env;                    ///< environment variable overriding the key
};

const std::vector<KeySpec>& key_specs();
const KeySpec* find_key(const std::string& key);

/// Renders a value the way it appears in `config show` ("8383", "true", "[a, b]", "text").
std::string value_to_string(const Value& value);
/// Parses a flag/environment string for `spec`. Returns nullopt (and `error`) when it does not fit.
std::optional<Value> parse_value(const KeySpec& spec, const std::string& text, std::string* error = nullptr);

/// One layer of settings: only the keys that were actually given.
struct McpConfig {
    std::map<std::string, Value> values;
    std::map<std::string, int> lines;   ///< 1-based YAML line of each key (YAML layer only)

    bool has(const std::string& key) const { return values.count(key) != 0; }
    void set(const std::string& key, Value value) { values[key] = std::move(value); }
    std::optional<std::string> get_string(const std::string& key) const;
    std::optional<long long> get_int(const std::string& key) const;
    std::optional<bool> get_bool(const std::string& key) const;
    std::optional<std::vector<std::string>> get_list(const std::string& key) const;
};

enum class Severity { Warning, Error };

struct ConfigIssue {
    Severity severity = Severity::Error;
    std::string code;      ///< CONFIG_PARSE_ERROR, CONFIG_INVALID_VALUE, CONFIG_UNKNOWN_KEY, CONFIG_CONTAINS_SECRET, ...
    std::string message;   ///< never contains the offending value of a secret
    int line = 0;          ///< 1-based, 0 = unknown
    std::string key;       ///< dotted key when known
};

struct ParseResult {
    McpConfig config;
    std::vector<ConfigIssue> issues;
    bool ok() const;                    ///< no Error issue
    bool has_secret() const;            ///< an issue with code CONFIG_CONTAINS_SECRET
    std::string first_error_code() const;
};

/// Strict parse + validation of YAML text. Never throws.
ParseResult parse_yaml(const std::string& yaml_text);

// ---- Precedence ---------------------------------------------------------------------------------
using EnvLookup = std::function<std::string(const char*)>;

struct EffectiveValue {
    Value value;
    Source source = Source::Default;
    bool set = true;   ///< false: no default and no layer set it ("(unset)")
};
using Effective = std::map<std::string, EffectiveValue>;

/// Layers over the defaults: flag > env > yaml > default. `env_issues` collects unparsable
/// environment values (those are ignored, not fatal).
Effective resolve(const McpConfig& yaml, const McpConfig& flags, const EnvLookup& env,
                  std::vector<ConfigIssue>* env_issues = nullptr);

/// The environment layer alone (used by resolve and by tests).
McpConfig env_layer(const EnvLookup& env, std::vector<ConfigIssue>* issues = nullptr);

/// Flag layer over env layer over yaml layer, defaults NOT included (only what someone set).
McpConfig merged_layers(const McpConfig& yaml, const McpConfig& flags, const EnvLookup& env);

/// Copies the keys present in `layers` into `options`; keys that are absent leave the field alone.
/// Only fields ServeOptions has today are set: mode.*, limits.*, tools.families, default_connection,
/// format, server.transport, server.port. Server host/sse/allowed_hosts/cors_origins are read from
/// the McpConfig by the HTTP layer (see docs/MCP_TRAY.md).
void apply_config(const McpConfig& layers, mcp::ServeOptions& options);

// ---- Paths and text -----------------------------------------------------------------------------
/// Config file path: explicit (-c) > FAIRYFLY_MCP_CONFIG > %LOCALAPPDATA%\fairyfly\mcp.yaml.
struct ConfigPath {
    std::filesystem::path path;
    Source source = Source::Default;
    bool explicit_request = false;   ///< a missing file is an error only when requested explicitly
};
ConfigPath resolve_config_path(const std::string& flag_path, const EnvLookup& env);

/// The commented template written by `mcp config init`. Every active line equals the default.
std::string config_template();

/// Human-readable `config show` table and its JSON form.
std::string format_effective(const Effective& effective);
nlohmann::json effective_to_json(const Effective& effective);

/// "path:line: message" style rendering of one issue.
std::string format_issue(const ConfigIssue& issue, const std::string& file_label = {});

/// Environment lookup of the real process ("" when unset).
EnvLookup process_env();

// ---- File helpers (the only I/O here) -----------------------------------------------------------
std::optional<std::string> read_text_file(const std::filesystem::path& path);
bool write_text_file(const std::filesystem::path& path, const std::string& text);

} // namespace fairyfly::config
