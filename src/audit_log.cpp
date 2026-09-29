#include "include/audit_log.h"
#include "include/sensitive_data.h"
#include "include/version.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cctype>
#include <ctime>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace fairyfly::audit {

namespace {

constexpr std::size_t kMaxElementBytes = 512;
constexpr std::size_t kMaxArgvElements = 64;
constexpr std::size_t kMaxRecordBytes = 16 * 1024;
constexpr std::size_t kMaxFactBytes = 64;
constexpr const char* kRedacted = "<redacted>";
constexpr const char* kTruncated = "<truncated>";

/// Cut `s` to at most `max_bytes`, never splitting a UTF-8 sequence.
std::string cap_bytes(const std::string& s, std::size_t max_bytes, bool marker) {
    if (s.size() <= max_bytes) return s;
    std::size_t keep = marker && max_bytes > 3 ? max_bytes - 3 : max_bytes;
    while (keep > 0 && (static_cast<unsigned char>(s[keep]) & 0xC0) == 0x80) --keep;
    std::string out = s.substr(0, keep);
    if (marker && max_bytes > 3) out += "...";
    return out;
}

bool looks_like_option(const std::string& t) {
    if (t.size() >= 2 && t[0] == '-' && t[1] == '-') return t.size() > 2;
    return t.size() >= 2 && t[0] == '-' && std::isalpha(static_cast<unsigned char>(t[1]));
}

std::string strip_dashes(const std::string& name) {
    std::size_t i = 0;
    while (i < name.size() && name[i] == '-') ++i;
    return name.substr(i);
}

bool option_name_is_sensitive(const std::string& raw_name) {
    const std::string name = strip_dashes(raw_name);
    if (name.empty()) return false;
    if (sap::normalize_sensitive_name(name) == "credentialsfile") return false;
    return sap::is_sensitive_data_name(name) || sap::contains_sensitive_data_name(name);
}

/// Options of `element fill` that consume the next token.
bool fill_option_takes_value(const std::string& name) {
    return name == "--connection" || name == "--row" || name == "--column";
}

std::tm utc_tm(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm out{};
#ifdef _WIN32
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

std::string two(int v) {
    std::string s = std::to_string(v);
    return s.size() < 2 ? "0" + s : s;
}

int current_pid() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

std::string dump_line(const nlohmann::ordered_json& j) {
    return j.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

nlohmann::ordered_json build_json(const AuditRecord& r, const std::vector<std::string>& argv) {
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    j["ts"] = utc_timestamp_ms(r.ts);
    j["v"] = 1;
    j["fairyfly"] = FAIRYFLY_VERSION;
    j["pid"] = r.pid != 0 ? r.pid : current_pid();
    j["cmd"] = cap_bytes(r.command, kMaxFactBytes, false);
    j["argv"] = argv;
    if (r.connection) j["connection"] = *r.connection;
    if (r.sap && r.sap->any()) {
        nlohmann::ordered_json sap = nlohmann::ordered_json::object();
        if (!r.sap->system.empty()) sap["system"] = cap_bytes(r.sap->system, kMaxFactBytes, false);
        if (!r.sap->client.empty()) sap["client"] = cap_bytes(r.sap->client, kMaxFactBytes, false);
        if (!r.sap->user.empty()) sap["user"] = cap_bytes(r.sap->user, kMaxFactBytes, false);
        if (!r.sap->transaction.empty()) sap["transaction"] = cap_bytes(r.sap->transaction, kMaxFactBytes, false);
        j["sap"] = std::move(sap);
    }
    j["read_only"] = r.read_only;
    if (r.batch_line) j["batch_line"] = *r.batch_line;
    j["status"] = r.status;
    if (!r.error_code.empty()) j["error_code"] = cap_bytes(r.error_code, kMaxFactBytes, false);
    j["exit"] = r.exit_code;
    j["duration_ms"] = r.duration_ms;
    j["audit_source"] = r.source.empty() ? std::string("cli") : cap_bytes(r.source, kMaxFactBytes, false);
    if (!r.tool.empty()) j["tool"] = cap_bytes(r.tool, kMaxFactBytes, false);
    if (!r.client.empty()) j["client"] = cap_bytes(r.client, kMaxFactBytes * 2, false);
    if (!r.request_id.empty()) j["request_id"] = cap_bytes(r.request_id, kMaxFactBytes, false);
    if (!r.principal.empty()) j["principal"] = cap_bytes(r.principal, kMaxFactBytes, false);
    if (!r.remote_addr.empty()) j["remote_addr"] = cap_bytes(r.remote_addr, kMaxFactBytes, false);
    if (!r.transport.empty()) j["transport"] = cap_bytes(r.transport, kMaxFactBytes, false);
    if (!r.era.empty()) j["era"] = cap_bytes(r.era, kMaxFactBytes, false);
    if (r.tcode_left_allowlist) j["tcode_left_allowlist"] = true;
    return j;
}

std::atomic<bool> g_warned{false};

void warn_once(const char* what) noexcept {
    try {
        if (!g_warned.exchange(true)) spdlog::warn("Audit log unavailable ({}); commands continue unaudited", what);
    } catch (...) {
    }
}

} // namespace

std::string utc_timestamp_ms(std::chrono::system_clock::time_point now) {
    const auto ms_total = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(ms_total);
    long long ms = (ms_total - secs).count();
    if (ms < 0) { ms += 1000; secs -= std::chrono::seconds(1); }
    const std::tm tm = utc_tm(std::chrono::system_clock::time_point(secs));
    std::string ms_str = std::to_string(ms);
    while (ms_str.size() < 3) ms_str = "0" + ms_str;
    return std::to_string(tm.tm_year + 1900) + "-" + two(tm.tm_mon + 1) + "-" + two(tm.tm_mday) + "T" +
           two(tm.tm_hour) + ":" + two(tm.tm_min) + ":" + two(tm.tm_sec) + "." + ms_str + "Z";
}

std::string monthly_file_name(std::chrono::system_clock::time_point now) {
    const std::tm tm = utc_tm(now);
    return std::to_string(tm.tm_year + 1900) + "-" + two(tm.tm_mon + 1) + ".jsonl";
}

AuditConfig resolve_config(const GetEnvFn& getenv_fn, bool cli_no_audit, bool cli_required,
                           std::chrono::system_clock::time_point now) {
    auto get = [&](const char* name) { return getenv_fn ? getenv_fn(name) : std::string(); };
    AuditConfig config;

    std::string mode_env = get("FAIRYFLY_AUDIT");
    for (auto& c : mode_env) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (mode_env == "0" || mode_env == "off") config.mode = Mode::Disabled;
    else if (mode_env == "required") config.mode = Mode::Required;
    if (cli_no_audit) config.mode = Mode::Disabled;
    if (cli_required) config.mode = Mode::Required;  // fail safe: required beats any disable

    const std::string file_env = get("FAIRYFLY_AUDIT_FILE");
    if (!file_env.empty()) {
        config.file = std::filesystem::path(file_env);
        return config;
    }

    std::filesystem::path dir;
#ifdef _WIN32
    const std::string local = get("LOCALAPPDATA");
    if (!local.empty()) dir = std::filesystem::path(local) / "fairyfly" / "audit";
#else
    const std::string home = get("HOME");
    if (!home.empty()) dir = std::filesystem::path(home) / ".local" / "share" / "fairyfly" / "audit";
#endif
    if (dir.empty()) {
        std::error_code ec;
        auto tmp = std::filesystem::temp_directory_path(ec);
        dir = (ec ? std::filesystem::path(".fairyfly") : tmp / "fairyfly") / "audit";
    }
    config.file = dir / monthly_file_name(now);
    return config;
}

std::vector<std::string> redact_argv(const std::vector<std::string>& argv) {
    std::vector<std::string> out;
    out.reserve(argv.size());

    bool in_fill = false;
    std::size_t fill_positional = 0;
    bool after_double_dash = false;

    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& tok = argv[i];
        if (after_double_dash) {
            if (in_fill && fill_positional++ >= 1) out.push_back(kRedacted);
            else out.push_back(tok);
            continue;
        }
        if (tok == "--") {
            after_double_dash = true;
            out.push_back(tok);
            continue;
        }
        if (looks_like_option(tok)) {
            const auto eq = tok.find('=');
            const std::string name = eq == std::string::npos ? tok : tok.substr(0, eq);
            if (option_name_is_sensitive(name)) {
                if (eq != std::string::npos) {
                    out.push_back(name + "=" + kRedacted);
                } else {
                    out.push_back(tok);
                    if (i + 1 < argv.size() && !looks_like_option(argv[i + 1])) {
                        out.push_back(kRedacted);
                        ++i;
                    }
                }
                continue;
            }
            out.push_back(tok);
            if (in_fill && eq == std::string::npos && fill_option_takes_value(name) && i + 1 < argv.size()) {
                out.push_back(argv[++i]);
            }
            continue;
        }
        if (!in_fill && tok == "fill" && i > 0 && argv[i - 1] == "element") {
            in_fill = true;
            out.push_back(tok);
            continue;
        }
        if (in_fill) {
            // First positional is the element id; every further positional is the value.
            if (fill_positional++ >= 1) out.push_back(kRedacted);
            else out.push_back(tok);
            continue;
        }
        out.push_back(tok);
    }

    for (auto& e : out) e = cap_bytes(e, kMaxElementBytes, true);
    if (out.size() > kMaxArgvElements) {
        out.resize(kMaxArgvElements - 1);
        out.push_back(kTruncated);
    }
    return out;
}

std::string format_record(const AuditRecord& record) {
    std::string line = dump_line(build_json(record, redact_argv(record.argv)));
    if (line.size() + 1 > kMaxRecordBytes) {
        line = dump_line(build_json(record, std::vector<std::string>{kTruncated}));
    }
    line += '\n';
    return line;
}

std::optional<std::string> preflight_error(Mode mode, bool probe_ok) {
    if (mode == Mode::Required && !probe_ok) return std::string("AUDIT_UNAVAILABLE");
    return std::nullopt;
}

#ifdef _WIN32
namespace {
HANDLE open_append(const std::filesystem::path& file) {
    return CreateFileW(file.c_str(), FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool ensure_parent(const std::filesystem::path& file) {
    std::error_code ec;
    const auto parent = file.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    return !ec;
}
} // namespace

bool AuditSink::probe() noexcept {
    try {
        ensure_parent(config_.file);
        HANDLE h = open_append(config_.file);
        if (h == INVALID_HANDLE_VALUE) { warn_once("cannot open audit file"); return false; }
        CloseHandle(h);
        return true;
    } catch (...) {
        warn_once("exception");
        return false;
    }
}

bool AuditSink::append(const AuditRecord& record) noexcept {
    try {
        if (!enabled()) return true;
        const std::string line = format_record(record);
        ensure_parent(config_.file);
        HANDLE h = open_append(config_.file);
        if (h == INVALID_HANDLE_VALUE) { warn_once("cannot open audit file"); return false; }
        DWORD written = 0;
        const BOOL ok = WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        CloseHandle(h);
        if (!ok || written != line.size()) { warn_once("write failed"); return false; }
        return true;
    } catch (...) {
        warn_once("exception");
        return false;
    }
}
#else
bool AuditSink::probe() noexcept {
    try {
        std::error_code ec;
        const auto parent = config_.file.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        FILE* f = std::fopen(config_.file.string().c_str(), "ab");
        if (!f) { warn_once("cannot open audit file"); return false; }
        std::fclose(f);
        return true;
    } catch (...) {
        return false;
    }
}

bool AuditSink::append(const AuditRecord& record) noexcept {
    try {
        if (!enabled()) return true;
        const std::string line = format_record(record);
        std::error_code ec;
        const auto parent = config_.file.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        FILE* f = std::fopen(config_.file.string().c_str(), "ab");
        if (!f) { warn_once("cannot open audit file"); return false; }
        const bool ok = std::fwrite(line.data(), 1, line.size(), f) == line.size();
        std::fclose(f);
        if (!ok) warn_once("write failed");
        return ok;
    } catch (...) {
        return false;
    }
}
#endif

} // namespace fairyfly::audit
