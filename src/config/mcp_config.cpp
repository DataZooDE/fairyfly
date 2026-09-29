#include "include/config/mcp_config.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "include/command_table.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fairyfly::config {

namespace {

using Vec = std::vector<std::string>;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string env_name_for(const std::string& key) {
    std::string out = "FAIRYFLY_MCP_";
    for (char c : key) out += (c == '.') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

KeySpec make(const char* key, ValueType type, std::optional<Value> def, const char* doc) {
    KeySpec spec;
    spec.key = key;
    spec.type = type;
    spec.def = std::move(def);
    spec.doc = doc;
    spec.env = env_name_for(key);
    return spec;
}

std::vector<KeySpec> build_specs() {
    std::vector<KeySpec> s;
    auto add = [&](KeySpec spec) { s.push_back(std::move(spec)); };

    KeySpec k = make("default_connection", ValueType::Int, std::nullopt,
                     "Connection index used when a tool call omits 'connection' (unset = none)");
    k.min = 0; k.max = 1000000;
    add(k);
    k = make("format", ValueType::String, Value{std::string("markdown")}, "Text format of tool results: markdown or json");
    k.allowed = {"markdown", "json"};
    add(k);

    k = make("server.host", ValueType::String, Value{std::string("127.0.0.1")},
             "Listen address of the HTTP transport. Keep 127.0.0.1 and put IIS (TLS) in front");
    add(k);
    k = make("server.port", ValueType::Int, Value{8383LL}, "Listen port of the HTTP transport");
    k.min = 1; k.max = 65535;
    add(k);
    k = make("server.transport", ValueType::String, Value{std::string("stdio")}, "stdio (default) or http");
    k.allowed = {"stdio", "http"};
    add(k);
    add(make("server.sse", ValueType::Bool, Value{false}, "Allow Server-Sent Events streaming (progress, keep-alive) on tools/call"));
    add(make("server.allowed_hosts", ValueType::StringList, Value{Vec{}},
             "Extra accepted Host header values (loopback names are always accepted)"));
    add(make("server.cors_origins", ValueType::StringList, Value{Vec{}}, "Browser origins allowed to call the server (empty = none)"));

    add(make("mode.read_only", ValueType::Bool, Value{false}, "Force the read-only guard (the guard is also the default without allow_write)"));
    add(make("mode.allow_write", ValueType::Bool, Value{false}, "Write mode: expose write tools and turn the guard off. FAIRYFLY_READ_ONLY=1 still wins"));

    k = make("limits.max_result_chars", ValueType::Int, Value{60000LL}, "Truncate text results beyond this many characters");
    k.min = 100; k.max = 100000000;
    add(k);
    k = make("limits.max_image_bytes", ValueType::Int, Value{2097152LL}, "Refuse screenshots larger than this many bytes");
    k.min = 1024; k.max = 1073741824LL;
    add(k);
    k = make("limits.max_calls_per_minute", ValueType::Int, Value{120LL}, "Rate limit for tool calls");
    k.min = 1; k.max = 1000000;
    add(k);
    k = make("limits.call_timeout_ms", ValueType::Int, Value{120000LL}, "Soft timeout per tool call in milliseconds");
    k.min = 1000; k.max = 3600000;
    add(k);

    add(make("tools.families", ValueType::StringList, Value{Vec{}}, "Only expose these tool families (empty = all)"));

    add(make("tray.enabled", ValueType::Bool, Value{false}, "Run with the system tray icon (same as --tray)"));
    add(make("tray.start_minimized_notice", ValueType::Bool, Value{true}, "Show a balloon when the tray starts"));
    add(make("tray.autostart", ValueType::Bool, Value{false}, "Documentation of intent only; use 'mcp --tray --install-autostart' to register"));

    add(make("audit.enabled", ValueType::Bool, Value{true}, "Write the audit trail (--no-audit / FAIRYFLY_AUDIT override)"));
    add(make("audit.required", ValueType::Bool, Value{false}, "Fail calls when the audit trail cannot be written"));
    add(make("audit.file", ValueType::String, std::nullopt, "Audit file (unset = %LOCALAPPDATA%\\fairyfly\\audit\\YYYY-MM.jsonl)"));

    k = make("auth.token_prefix", ValueType::String, Value{std::string("ffy")}, "Prefix of generated bearer tokens (2-8 lower-case letters/digits)");
    add(k);
    k = make("auth.proxy_secret_source", ValueType::String, Value{std::string("credential-manager")},
             "Where the IIS proxy secret is read from. The secret itself never goes into this file");
    k.allowed = {"credential-manager"};
    add(k);
    return s;
}

bool valid_token_prefix(const std::string& s) {
    if (s.size() < 2 || s.size() > 8) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c); });
}

/// Range/enum/format checks shared by YAML, environment and flags.
bool validate_value(const KeySpec& spec, const Value& value, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (spec.type == ValueType::Int) {
        const long long n = std::get<long long>(value);
        if ((spec.min && n < *spec.min) || (spec.max && n > *spec.max))
            return fail("must be between " + std::to_string(spec.min.value_or(0)) + " and " +
                        std::to_string(spec.max.value_or(0)));
    } else if (spec.type == ValueType::String) {
        const auto& text = std::get<std::string>(value);
        if (!spec.allowed.empty() && std::find(spec.allowed.begin(), spec.allowed.end(), text) == spec.allowed.end()) {
            std::string list;
            for (const auto& a : spec.allowed) list += (list.empty() ? "" : ", ") + a;
            return fail("must be one of: " + list);
        }
        if (spec.key == "server.host" && (text.empty() || text.find_first_of(" \t/") != std::string::npos))
            return fail("must be a host name or IP address without spaces or slashes");
        if (spec.key == "auth.token_prefix" && !valid_token_prefix(text))
            return fail("must be 2-8 lower-case letters or digits");
        if (spec.key == "audit.file" && text.empty()) return fail("must not be empty (remove the key for the default)");
    } else if (spec.type == ValueType::StringList && spec.key == "tools.families") {
        const auto& list = std::get<Vec>(value);
        const auto unknown = command_table::unknown_families(list);
        if (!unknown.empty()) {
            std::string names;
            for (const auto& u : unknown) names += (names.empty() ? "" : ", ") + u;
            return fail("unknown tool family: " + names);
        }
    }
    return true;
}

bool looks_secret_key(const std::string& key) {
    const std::string k = lower(key);
    return k.find("password") != std::string::npos || k.find("passwd") != std::string::npos ||
           k.find("secret") != std::string::npos || k.rfind("token", 0) == 0 || k.find("_token") != std::string::npos ||
           k.find("api_key") != std::string::npos || k.find("apikey") != std::string::npos;
}

bool looks_secret_value(const std::string& value) {
    const std::string l = lower(trim(value));
    if (l.rfind("bearer ", 0) == 0 || l.rfind("-----begin", 0) == 0) return true;
    static const std::regex token(R"(ffy_[A-Za-z0-9]+_[A-Za-z0-9_\-]{8,})");
    return std::regex_search(value, token);
}

struct Walker {
    ParseResult& out;

    void issue(Severity sev, std::string code, std::string message, int line, std::string key = {}) {
        out.issues.push_back({sev, std::move(code), std::move(message), line, std::move(key)});
    }

    /// Refuses secret-looking scalar VALUES anywhere in the document (message never echoes them).
    void scan_values(const YAML::Node& node, const std::string& path) {
        if (node.IsScalar()) {
            if (looks_secret_value(node.Scalar()))
                issue(Severity::Error, "CONFIG_CONTAINS_SECRET",
                      "the value of '" + path + "' looks like a token or credential; secrets must not be stored in this file",
                      node.Mark().line + 1, path);
        } else if (node.IsSequence()) {
            for (const auto& item : node) scan_values(item, path);
        } else if (node.IsMap()) {
            for (const auto& kv : node)
                scan_values(kv.second, path.empty() ? kv.first.Scalar() : path + "." + kv.first.Scalar());
        }
    }

    void unknown_key(const std::string& path, const YAML::Node& key_node, const YAML::Node& value) {
        const int line = key_node.Mark().line + 1;
        const auto dot = path.rfind('.');
        if (looks_secret_key(dot == std::string::npos ? path : path.substr(dot + 1))) {
            const bool has_value = value.IsDefined() && !value.IsNull() &&
                                   !(value.IsScalar() && trim(value.Scalar()).empty());
            if (has_value)
                issue(Severity::Error, "CONFIG_CONTAINS_SECRET",
                      "key '" + path + "' looks like a secret; secrets never belong in this file (use 'mcp token create' and the Credential Manager)",
                      line, path);
            else
                issue(Severity::Warning, "CONFIG_SECRET_KEY", "key '" + path + "' looks like a secret and is ignored", line, path);
            return;
        }
        issue(Severity::Warning, "CONFIG_UNKNOWN_KEY", "unknown key '" + path + "' is ignored", line, path);
    }

    void leaf(const KeySpec& spec, const YAML::Node& key_node, const YAML::Node& node) {
        const int line = key_node.Mark().line + 1;
        out.config.lines[spec.key] = line;
        auto bad = [&](const std::string& why) {
            issue(Severity::Error, "CONFIG_INVALID_VALUE", "'" + spec.key + "': " + why, node.Mark().line + 1, spec.key);
        };
        switch (spec.type) {
        case ValueType::String: {
            if (!node.IsScalar()) return bad("expected a string");
            Value v = node.Scalar();
            std::string why;
            if (!validate_value(spec, v, &why)) return bad(why);
            out.config.values[spec.key] = std::move(v);
            return;
        }
        case ValueType::Int: {
            if (!node.IsScalar()) return bad("expected an integer");
            const std::string text = trim(node.Scalar());
            char* end = nullptr;
            errno = 0;
            const long long n = std::strtoll(text.c_str(), &end, 10);
            if (text.empty() || *end != '\0' || errno == ERANGE) return bad("expected an integer");
            Value v = n;
            std::string why;
            if (!validate_value(spec, v, &why)) return bad(why);
            out.config.values[spec.key] = std::move(v);
            return;
        }
        case ValueType::Bool: {
            if (!node.IsScalar()) return bad("expected true or false");
            const std::string text = lower(trim(node.Scalar()));
            if (text == "true") out.config.values[spec.key] = true;
            else if (text == "false") out.config.values[spec.key] = false;
            else return bad("expected true or false");
            return;
        }
        case ValueType::StringList: {
            Vec list;
            if (node.IsNull()) {
            } else if (node.IsSequence()) {
                for (const auto& item : node) {
                    if (!item.IsScalar()) return bad("expected a list of strings");
                    list.push_back(item.Scalar());
                }
            } else {
                return bad("expected a list, e.g. [a, b]");
            }
            Value v = list;
            std::string why;
            if (!validate_value(spec, v, &why)) return bad(why);
            out.config.values[spec.key] = std::move(v);
            return;
        }
        }
    }

    void walk(const YAML::Node& root) {
        scan_values(root, "");
        if (root.IsNull()) return;
        if (!root.IsMap()) {
            issue(Severity::Error, "CONFIG_INVALID_STRUCTURE", "the file must contain a YAML mapping (server:, mode:, ...)",
                  root.Mark().line + 1);
            return;
        }
        static const std::vector<std::string> sections = {"server", "mode", "limits", "tools", "tray", "audit", "auth"};
        for (const auto& kv : root) {
            const std::string name = kv.first.Scalar();
            const int line = kv.first.Mark().line + 1;
            const KeySpec* top = find_key(name);
            if (top && name.find('.') == std::string::npos) {
                leaf(*top, kv.first, kv.second);
            } else if (std::find(sections.begin(), sections.end(), name) != sections.end()) {
                if (kv.second.IsNull()) continue;
                if (!kv.second.IsMap()) {
                    issue(Severity::Error, "CONFIG_INVALID_STRUCTURE", "'" + name + "' must be a mapping", line, name);
                    continue;
                }
                for (const auto& child : kv.second) {
                    const std::string path = name + "." + child.first.Scalar();
                    if (const KeySpec* spec = find_key(path)) leaf(*spec, child.first, child.second);
                    else unknown_key(path, child.first, child.second);
                }
            } else {
                unknown_key(name, kv.first, kv.second);
            }
        }
        cross_checks();
    }

    void cross_checks() {
        const auto ro = out.config.get_bool("mode.read_only");
        const auto aw = out.config.get_bool("mode.allow_write");
        if (ro && aw && *ro && *aw) {
            const auto it = out.config.lines.find("mode.allow_write");
            issue(Severity::Error, "CONFIG_CONFLICT", "mode.read_only and mode.allow_write cannot both be true",
                  it == out.config.lines.end() ? 0 : it->second, "mode.allow_write");
        }
    }
};

std::string join_list(const Vec& list) {
    std::string out;
    for (const auto& s : list) out += (out.empty() ? "" : ", ") + s;
    return out;
}

} // namespace

const char* source_name(Source source) {
    switch (source) {
    case Source::Flag: return "flag";
    case Source::Env: return "env";
    case Source::Yaml: return "yaml";
    default: return "default";
    }
}

const std::vector<KeySpec>& key_specs() {
    static const std::vector<KeySpec> specs = build_specs();
    return specs;
}

const KeySpec* find_key(const std::string& key) {
    for (const auto& spec : key_specs())
        if (spec.key == key) return &spec;
    return nullptr;
}

std::string value_to_string(const Value& value) {
    if (const auto* s = std::get_if<std::string>(&value)) return *s;
    if (const auto* n = std::get_if<long long>(&value)) return std::to_string(*n);
    if (const auto* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
    return "[" + join_list(std::get<Vec>(value)) + "]";
}

std::optional<Value> parse_value(const KeySpec& spec, const std::string& text, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<Value> {
        if (error) *error = why;
        return std::nullopt;
    };
    Value value;
    switch (spec.type) {
    case ValueType::String: value = trim(text); break;
    case ValueType::Int: {
        const std::string t = trim(text);
        char* end = nullptr;
        errno = 0;
        const long long n = std::strtoll(t.c_str(), &end, 10);
        if (t.empty() || *end != '\0' || errno == ERANGE) return fail("expected an integer");
        value = n;
        break;
    }
    case ValueType::Bool: {
        const std::string t = lower(trim(text));
        if (t == "1" || t == "true" || t == "yes" || t == "on") value = true;
        else if (t == "0" || t == "false" || t == "no" || t == "off") value = false;
        else return fail("expected true or false");
        break;
    }
    case ValueType::StringList: {
        Vec list;
        std::string item;
        std::istringstream in(text);
        while (std::getline(in, item, ',')) {
            // Also accept whitespace separated items (--tools "a b").
            std::istringstream words(item);
            std::string word;
            while (words >> word) list.push_back(word);
        }
        value = std::move(list);
        break;
    }
    }
    std::string why;
    if (!validate_value(spec, value, &why)) return fail(why);
    return value;
}

std::optional<std::string> McpConfig::get_string(const std::string& key) const {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    if (const auto* s = std::get_if<std::string>(&it->second)) return *s;
    return std::nullopt;
}
std::optional<long long> McpConfig::get_int(const std::string& key) const {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    if (const auto* n = std::get_if<long long>(&it->second)) return *n;
    return std::nullopt;
}
std::optional<bool> McpConfig::get_bool(const std::string& key) const {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    if (const auto* b = std::get_if<bool>(&it->second)) return *b;
    return std::nullopt;
}
std::optional<Vec> McpConfig::get_list(const std::string& key) const {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    if (const auto* l = std::get_if<Vec>(&it->second)) return *l;
    return std::nullopt;
}

bool ParseResult::ok() const {
    return std::none_of(issues.begin(), issues.end(), [](const ConfigIssue& i) { return i.severity == Severity::Error; });
}
bool ParseResult::has_secret() const {
    return std::any_of(issues.begin(), issues.end(), [](const ConfigIssue& i) { return i.code == "CONFIG_CONTAINS_SECRET"; });
}
std::string ParseResult::first_error_code() const {
    for (const auto& i : issues)
        if (i.code == "CONFIG_CONTAINS_SECRET") return i.code;
    for (const auto& i : issues)
        if (i.severity == Severity::Error) return i.code;
    return {};
}

ParseResult parse_yaml(const std::string& yaml_text) {
    ParseResult out;
    try {
        const YAML::Node root = YAML::Load(yaml_text);
        Walker{out}.walk(root);
    } catch (const YAML::Exception& e) {
        out.issues.push_back({Severity::Error, "CONFIG_PARSE_ERROR", e.msg, e.mark.line >= 0 ? e.mark.line + 1 : 0, {}});
    } catch (const std::exception& e) {
        out.issues.push_back({Severity::Error, "CONFIG_PARSE_ERROR", e.what(), 0, {}});
    }
    if (!out.ok()) out.config = McpConfig{};   // never apply a partially valid file
    return out;
}

McpConfig env_layer(const EnvLookup& env, std::vector<ConfigIssue>* issues) {
    McpConfig layer;
    for (const auto& spec : key_specs()) {
        const std::string text = env ? env(spec.env.c_str()) : std::string();
        if (trim(text).empty()) continue;
        std::string why;
        if (auto value = parse_value(spec, text, &why)) {
            layer.values[spec.key] = std::move(*value);
        } else if (issues) {
            issues->push_back({Severity::Warning, "CONFIG_ENV_INVALID",
                               "environment variable " + spec.env + " ignored: " + why, 0, spec.key});
        }
    }
    return layer;
}

Effective resolve(const McpConfig& yaml, const McpConfig& flags, const EnvLookup& env, std::vector<ConfigIssue>* env_issues) {
    const McpConfig envs = env_layer(env, env_issues);
    Effective out;
    for (const auto& spec : key_specs()) {
        EffectiveValue ev;
        if (flags.has(spec.key)) { ev.value = flags.values.at(spec.key); ev.source = Source::Flag; }
        else if (envs.has(spec.key)) { ev.value = envs.values.at(spec.key); ev.source = Source::Env; }
        else if (yaml.has(spec.key)) { ev.value = yaml.values.at(spec.key); ev.source = Source::Yaml; }
        else if (spec.def) { ev.value = *spec.def; ev.source = Source::Default; }
        else { ev.set = false; }
        out[spec.key] = std::move(ev);
    }
    return out;
}

McpConfig merged_layers(const McpConfig& yaml, const McpConfig& flags, const EnvLookup& env) {
    McpConfig out = yaml;
    const McpConfig envs = env_layer(env);
    for (const auto& kv : envs.values) out.values[kv.first] = kv.second;
    for (const auto& kv : flags.values) out.values[kv.first] = kv.second;
    return out;
}

void apply_config(const McpConfig& c, mcp::ServeOptions& o) {
    if (auto v = c.get_bool("mode.read_only")) o.read_only = *v;
    if (auto v = c.get_bool("mode.allow_write")) o.allow_write = *v;
    if (auto v = c.get_int("default_connection")) o.default_connection = static_cast<int>(*v);
    if (auto v = c.get_string("format")) o.format = *v;
    if (auto v = c.get_int("limits.max_result_chars")) o.max_result_chars = static_cast<std::size_t>(*v);
    if (auto v = c.get_int("limits.max_image_bytes")) o.max_image_bytes = static_cast<std::size_t>(*v);
    if (auto v = c.get_int("limits.max_calls_per_minute")) o.max_calls_per_minute = static_cast<int>(*v);
    if (auto v = c.get_int("limits.call_timeout_ms")) o.call_timeout_ms = static_cast<int>(*v);
    if (auto v = c.get_list("tools.families")) o.families = *v;
    if (auto v = c.get_string("server.transport")) o.transport = *v;
    if (auto v = c.get_int("server.port")) o.port = static_cast<int>(*v);
}

EnvLookup process_env() {
    return [](const char* name) -> std::string {
#ifdef _WIN32
        char* buffer = nullptr;
        size_t size = 0;
        std::string value;
        if (_dupenv_s(&buffer, &size, name) == 0 && buffer) { value = buffer; free(buffer); }
        return value;
#else
        const char* v = std::getenv(name);
        return v ? v : "";
#endif
    };
}

ConfigPath resolve_config_path(const std::string& flag_path, const EnvLookup& env) {
    ConfigPath out;
    if (!trim(flag_path).empty()) {
        out.path = flag_path; out.source = Source::Flag; out.explicit_request = true;
        return out;
    }
    const std::string from_env = env ? trim(env("FAIRYFLY_MCP_CONFIG")) : std::string();
    if (!from_env.empty()) {
        out.path = from_env; out.source = Source::Env; out.explicit_request = true;
        return out;
    }
    const std::string base = env ? env("LOCALAPPDATA") : std::string();
    std::filesystem::path root = base.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(base);
    out.path = root / "fairyfly" / "mcp.yaml";
    out.source = Source::Default;
    return out;
}

std::string config_template() {
    std::ostringstream out;
    out << "# fairyfly MCP server configuration (mcp.yaml)\n"
           "#\n"
           "# Precedence for every key: command-line flag > environment variable > this file > default.\n"
           "# Environment variable of a key: FAIRYFLY_MCP_<SECTION>_<KEY>, e.g. FAIRYFLY_MCP_SERVER_PORT.\n"
           "# The config file path itself: -c/--config PATH, FAIRYFLY_MCP_CONFIG, or\n"
           "# %LOCALAPPDATA%\\fairyfly\\mcp.yaml.\n"
           "#\n"
           "# NO SECRETS here. Bearer tokens are created with 'fairyfly mcp token create' (shown once) and\n"
           "# live in the Windows Credential Manager. Keys named password/secret/token* and secret-looking\n"
           "# values are refused (CONFIG_CONTAINS_SECRET). Unknown keys only produce a warning.\n"
           "# Validate with: fairyfly mcp config validate\n";
    std::string section = "-";
    for (const auto& spec : key_specs()) {
        const auto dot = spec.key.find('.');
        const std::string sec = dot == std::string::npos ? std::string() : spec.key.substr(0, dot);
        const std::string name = dot == std::string::npos ? spec.key : spec.key.substr(dot + 1);
        if (sec != section) {
            out << "\n";
            if (!sec.empty()) out << sec << ":\n";
            section = sec;
        }
        const std::string indent = sec.empty() ? "" : "  ";
        out << indent << "# " << spec.doc;
        if (!spec.allowed.empty()) out << " (" << join_list(spec.allowed) << ")";
        out << "\n" << indent;
        if (!spec.def) {
            out << "# " << name << ": " << (spec.type == ValueType::Int ? "0" : "\"\"") << "\n";
        } else if (spec.type == ValueType::StringList) {
            out << name << ": []\n";
        } else {
            out << name << ": " << value_to_string(*spec.def) << "\n";
        }
    }
    return out.str();
}

std::string format_effective(const Effective& effective) {
    size_t key_w = 3, val_w = 5;
    std::vector<std::array<std::string, 3>> rows;
    for (const auto& spec : key_specs()) {
        const auto& ev = effective.at(spec.key);
        std::array<std::string, 3> row{spec.key, ev.set ? value_to_string(ev.value) : "(unset)",
                                       ev.set ? source_name(ev.source) : "-"};
        key_w = std::max(key_w, row[0].size());
        val_w = std::max(val_w, row[1].size());
        rows.push_back(std::move(row));
    }
    std::ostringstream out;
    auto pad = [](std::string s, size_t w) { s.resize(std::max(w, s.size()), ' '); return s; };
    out << pad("KEY", key_w) << "  " << pad("VALUE", val_w) << "  SOURCE\n";
    for (const auto& r : rows) out << pad(r[0], key_w) << "  " << pad(r[1], val_w) << "  " << r[2] << "\n";
    return out.str();
}

nlohmann::json effective_to_json(const Effective& effective) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& spec : key_specs()) {
        const auto& ev = effective.at(spec.key);
        nlohmann::json entry;
        entry["source"] = ev.set ? source_name(ev.source) : "unset";
        if (!ev.set) entry["value"] = nullptr;
        else if (const auto* s = std::get_if<std::string>(&ev.value)) entry["value"] = *s;
        else if (const auto* n = std::get_if<long long>(&ev.value)) entry["value"] = *n;
        else if (const auto* b = std::get_if<bool>(&ev.value)) entry["value"] = *b;
        else entry["value"] = std::get<Vec>(ev.value);
        out[spec.key] = std::move(entry);
    }
    return out;
}

std::string format_issue(const ConfigIssue& issue, const std::string& file_label) {
    std::string out = file_label;
    if (issue.line > 0) out += (out.empty() ? "line " : ":") + std::to_string(issue.line);
    if (!out.empty()) out += ": ";
    out += std::string(issue.severity == Severity::Error ? "error " : "warning ") + issue.code + ": " + issue.message;
    return out;
}

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);   // UTF-8 BOM (Notepad)
    return text;
}

bool write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

} // namespace fairyfly::config

