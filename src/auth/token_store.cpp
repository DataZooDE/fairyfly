#include "include/auth/token_store.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

#include "include/auth/ip.h"
#include "include/command_table.h"

namespace fairyfly::auth {

namespace {

using json = nlohmann::json;

constexpr std::size_t kSecretBytes = 32;
constexpr std::size_t kSecretChars = 43;  // base64url of 32 bytes, no padding
constexpr std::size_t kIdChars = 8;

// Howard Hinnant's civil-date algorithms.
long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

void civil_from_days(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

bool is_hex(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) && !std::isupper(c); });
}

bool is_b64url(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> string_array(const json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array())
        for (const auto& v : j[key])
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

bool valid_pattern(const std::string& s, const char* extra) {
    if (s.empty() || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [&](unsigned char c) { return std::isalnum(c) || std::strchr(extra, c) != nullptr; });
}

void validate(const NewToken& request) {
    if (!valid_token_name(request.name))
        throw AuthError("INVALID_ARGUMENT", "token name must be 1-64 characters of letters, digits, '.', '_' or '-'");
    if (request.scopes.empty()) throw AuthError("INVALID_ARGUMENT", "a token needs at least one scope");
    const auto families = command_table::families();
    for (const auto& scope : request.scopes) {
        if (scope == "*") continue;
        if (std::find(families.begin(), families.end(), scope) == families.end()) {
            std::string known;
            for (const auto& f : families) known += (known.empty() ? "" : ", ") + f;
            throw AuthError("UNKNOWN_FAMILY", "unknown scope '" + scope + "' (families: " + known + ", or *)");
        }
    }
    for (const auto& s : request.sap_systems)
        if (!valid_pattern(s, "*?/_-$")) throw AuthError("INVALID_ARGUMENT", "invalid SAP system pattern '" + s + "' (use SID/CLIENT, globs allowed)");
    for (const auto& t : request.tcodes)
        if (!valid_pattern(t, "*?/_-$.")) throw AuthError("INVALID_ARGUMENT", "invalid T-code pattern '" + t + "'");
    for (const auto& c : request.connections)
        if (!valid_pattern(c, "*?_-.$/")) throw AuthError("INVALID_ARGUMENT", "invalid connection name pattern '" + c + "' (letters, digits and * ? _ - . $ /; no spaces)");
    for (const auto& ip : request.allowed_ips)
        if (!parse_ip_rule(ip)) throw AuthError("INVALID_IP", "invalid IP address or CIDR block '" + ip + "'");
    if (request.rate_per_minute < 0) throw AuthError("INVALID_ARGUMENT", "rate must be >= 0");
}

Clock default_clock() {
    return [] { return std::chrono::system_clock::now(); };
}

RandomFn default_rng() {
    return [](unsigned char* buf, std::size_t n) { random_bytes(buf, n); };
}

} // namespace

// ---- TokenMeta -------------------------------------------------------------------------------
bool TokenMeta::has_all_scopes() const {
    return std::find(scopes.begin(), scopes.end(), "*") != scopes.end();
}

json TokenMeta::to_public_json() const {
    json j = {{"id", id},
              {"name", name},
              {"created", format_iso_utc(created)},
              {"scopes", scopes},
              {"sap_systems", sap_systems},
              {"tcodes", tcodes},
              {"connections", connections},
              {"rate_per_minute", rate_per_minute},
              {"allowed_ips", allowed_ips},
              {"read_only", read_only},
              {"revoked", revoked}};
    if (expires) j["expires"] = format_iso_utc(*expires);
    return j;
}

json TokenMeta::to_stored_json() const {
    json j = to_public_json();
    j["sha256"] = secret_hash;
    return j;
}

json TokenMeta::to_compact_json() const {
    // Short keys, empty/default values omitted: keeps realistic records inside one Credential Manager entry.
    json j = {{"v", 2}, {"i", id}, {"n", name}, {"h", secret_hash},
              {"c", std::chrono::floor<std::chrono::seconds>(created).time_since_epoch().count()},
              {"o", read_only}};
    if (expires) j["e"] = std::chrono::floor<std::chrono::seconds>(*expires).time_since_epoch().count();
    if (!scopes.empty()) j["s"] = scopes;
    if (!sap_systems.empty()) j["y"] = sap_systems;
    if (!tcodes.empty()) j["t"] = tcodes;
    if (!connections.empty()) j["k"] = connections;
    if (rate_per_minute != 0) j["r"] = rate_per_minute;
    if (!allowed_ips.empty()) j["p"] = allowed_ips;
    if (revoked) j["x"] = true;
    return j;
}

namespace {
/// Expands a compact (v2) record into the long-key form parsed by from_json().
json expand_compact(const json& c) {
    json j = json::object();
    j["id"] = c.value("i", std::string());
    j["name"] = c.value("n", std::string());
    j["sha256"] = c.value("h", std::string());
    if (c.contains("c") && c["c"].is_number_integer())
        j["created"] = format_iso_utc(TimePoint(std::chrono::seconds(c["c"].get<long long>())));
    if (c.contains("e")) {
        // a malformed expiry must stay malformed (from_json rejects it) rather than become "never"
        j["expires"] = c["e"].is_number_integer()
                           ? json(format_iso_utc(TimePoint(std::chrono::seconds(c["e"].get<long long>()))))
                           : json("invalid");
    }
    j["scopes"] = c.contains("s") ? c["s"] : json::array();
    j["sap_systems"] = c.contains("y") ? c["y"] : json::array();
    j["tcodes"] = c.contains("t") ? c["t"] : json::array();
    j["connections"] = c.contains("k") ? c["k"] : json::array();
    j["allowed_ips"] = c.contains("p") ? c["p"] : json::array();
    if (c.contains("r")) j["rate_per_minute"] = c["r"];
    if (c.contains("o")) j["read_only"] = c["o"];
    if (c.contains("x")) j["revoked"] = c["x"];
    return j;
}
} // namespace

std::optional<TokenMeta> TokenMeta::from_json(const json& input) {
    if (!input.is_object()) return std::nullopt;
    const json j = input.contains("v") && input["v"].is_number_integer() && input["v"].get<int>() == 2 ? expand_compact(input) : input;
    TokenMeta m;
    if (!j.contains("id") || !j["id"].is_string() || !j.contains("name") || !j["name"].is_string() ||
        !j.contains("sha256") || !j["sha256"].is_string())
        return std::nullopt;
    m.id = j["id"].get<std::string>();
    m.name = j["name"].get<std::string>();
    m.secret_hash = j["sha256"].get<std::string>();
    if (m.id.size() != kIdChars || !is_hex(m.id) || m.secret_hash.size() != 64 || !is_hex(m.secret_hash)) return std::nullopt;
    if (j.contains("created") && j["created"].is_string())
        if (auto t = parse_iso_utc(j["created"].get<std::string>())) m.created = *t;
    if (j.contains("expires") && j["expires"].is_string()) {
        const auto t = parse_iso_utc(j["expires"].get<std::string>());
        if (!t) return std::nullopt;  // an unreadable expiry must not turn into "never expires"
        m.expires = *t;
    }
    m.scopes = string_array(j, "scopes");
    m.sap_systems = string_array(j, "sap_systems");
    m.tcodes = string_array(j, "tcodes");
    m.connections = string_array(j, "connections");
    m.allowed_ips = string_array(j, "allowed_ips");
    if (j.contains("rate_per_minute") && j["rate_per_minute"].is_number_integer()) m.rate_per_minute = j["rate_per_minute"].get<int>();
    m.read_only = j.contains("read_only") && j["read_only"].is_boolean() ? j["read_only"].get<bool>() : true;
    m.revoked = j.contains("revoked") && j["revoked"].is_boolean() ? j["revoked"].get<bool>() : false;
    return m;
}

// ---- format helpers --------------------------------------------------------------------------
std::optional<ParsedToken> parse_token(std::string_view token) {
    if (token.size() != 4 + kIdChars + 1 + kSecretChars) return std::nullopt;
    if (token.substr(0, 4) != "ffy_") return std::nullopt;
    const auto id = token.substr(4, kIdChars);
    if (token[4 + kIdChars] != '_') return std::nullopt;
    const auto secret = token.substr(4 + kIdChars + 1);
    if (!is_hex(id) || !is_b64url(secret)) return std::nullopt;
    return ParsedToken{std::string(id), std::string(secret)};
}

std::string format_token(const std::string& id, const std::string& secret) { return "ffy_" + id + "_" + secret; }

std::string token_display_id(const TokenMeta& meta) { return "ffy_" + meta.id + "_..."; }

std::string format_iso_utc(TimePoint t) {
    const auto secs = std::chrono::floor<std::chrono::seconds>(t).time_since_epoch().count();
    long long days = secs / 86400;
    long long rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }
    int y; unsigned m, d;
    civil_from_days(days, y, m, d);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02lld:%02lld:%02lldZ", y, m, d, rem / 3600, (rem / 60) % 60, rem % 60);
    return buf;
}

std::optional<TimePoint> parse_iso_utc(const std::string& text) {
    int y = 0; unsigned m = 0, d = 0; int hh = 0, mm = 0, ss = 0;
    char tail = 0;
    int n = sscanf_s(text.c_str(), "%d-%u-%uT%d:%d:%d%c", &y, &m, &d, &hh, &mm, &ss, &tail, 1u);
    if (n == 7 && tail == 'Z') {
        // ok
    } else {
        hh = mm = ss = 0;
        n = sscanf_s(text.c_str(), "%d-%u-%u%c", &y, &m, &d, &tail, 1u);
        if (n != 3 || text.size() != 10) return std::nullopt;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return std::nullopt;
    const long long days = days_from_civil(y, m, d);
    return TimePoint(std::chrono::seconds(days * 86400 + hh * 3600 + mm * 60 + ss));
}

std::optional<TimePoint> parse_expiry(const std::string& raw, TimePoint now) {
    std::string text = raw;
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }), text.end());
    if (text.size() >= 2 && std::all_of(text.begin(), text.end() - 1, [](unsigned char c) { return std::isdigit(c); })) {
        const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(text.back())));
        if (text.size() > 9) return std::nullopt;
        const long long amount = std::stoll(text.substr(0, text.size() - 1));
        if (amount <= 0) return std::nullopt;
        if (unit == 'd') return now + std::chrono::hours(24 * amount);
        if (unit == 'h') return now + std::chrono::hours(amount);
        if (unit == 'm') return now + std::chrono::minutes(amount);
        return std::nullopt;
    }
    return parse_iso_utc(text);
}

bool valid_token_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(),
                       [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
}

// ---- TokenStore ------------------------------------------------------------------------------
TokenStore::TokenStore(std::shared_ptr<SecretBackend> backend, Clock clock, RandomFn rng, std::chrono::milliseconds cache_ttl)
    : backend_(std::move(backend)), clock_(clock ? std::move(clock) : default_clock()),
      rng_(rng ? std::move(rng) : default_rng()), ttl_(cache_ttl) {}

namespace {
constexpr std::size_t kChunkChars = 1000;   // well below the 1280 UTF-16 limit of one Credential Manager value
constexpr std::size_t kMaxChunks = 16;

std::string chunk_name(const std::string& name, std::size_t index) { return name + "#" + std::to_string(index); }
bool is_chunk_name(const std::string& name) { return name.find('#') != std::string::npos; }
} // namespace

std::optional<std::string> TokenStore::read_record(const std::string& name, bool* incomplete) {
    if (incomplete) *incomplete = false;
    const auto head = backend_->get(name);
    if (!head) return std::nullopt;
    json parsed;
    try {
        parsed = json::parse(*head);
    } catch (const std::exception&) {
        return *head;  // corrupt: the caller's parse reports it
    }
    if (!parsed.is_object() || !parsed.contains("chunks")) return *head;  // single-entry record (old or compact)
    const auto fail = [&]() -> std::optional<std::string> {
        if (incomplete) *incomplete = true;
        return std::nullopt;
    };
    if (!parsed["chunks"].is_number_integer() || !parsed.contains("len") || !parsed["len"].is_number_integer() ||
        !parsed.contains("sha") || !parsed["sha"].is_string())
        return fail();
    const long long count = parsed["chunks"].get<long long>();
    if (count < 1 || count > static_cast<long long>(kMaxChunks)) return fail();
    std::string payload;
    for (long long k = 1; k <= count; ++k) {
        const auto part = backend_->get(chunk_name(name, static_cast<std::size_t>(k)));
        if (!part) return fail();  // missing chunk: an interrupted write, never authenticate from it
        payload += *part;
    }
    if (static_cast<long long>(payload.size()) != parsed["len"].get<long long>() ||
        sha256_hex(payload) != parsed["sha"].get<std::string>())
        return fail();
    return payload;
}

void TokenStore::write_record(const std::string& name, const std::string& blob) {
    const std::size_t limit = backend_->max_value_chars();
    if (blob.size() <= limit) {
        backend_->put(name, blob);
        remove_chunks(name, 1);  // a former chunked record: its chunks are orphans now
        return;
    }
    const std::size_t chunk = std::min(kChunkChars, limit);
    const std::size_t count = (blob.size() + chunk - 1) / chunk;
    if (count > kMaxChunks)
        throw AuthError("TOKEN_TOO_LARGE", "token restrictions are too long to store even in chunks; shorten the allowlists");
    // Chunks first, the head entry LAST: readers ignore a set whose head is missing or does not match.
    for (std::size_t k = 1; k <= count; ++k) backend_->put(chunk_name(name, k), blob.substr((k - 1) * chunk, chunk));
    const json head = {{"v", 2}, {"chunks", count}, {"len", blob.size()}, {"sha", sha256_hex(blob)}};
    backend_->put(name, head.dump());
    remove_chunks(name, count + 1);
}

void TokenStore::remove_chunks(const std::string& name, std::size_t first) {
    const std::string prefix = name + "#";
    for (const auto& entry : backend_->list_names()) {
        if (entry.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string tail = entry.substr(prefix.size());
        if (tail.empty() || tail.size() > 6 ||
            !std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;
        if (std::stoul(tail) >= first) backend_->remove(entry);
    }
}

std::vector<TokenMeta> TokenStore::load_all() {
    std::vector<TokenMeta> out;
    for (const auto& name : backend_->list_names()) {
        if (is_chunk_name(name)) continue;  // chunk of a large record, read through its head entry
        try {
            const auto value = read_record(name, nullptr);
            if (!value) continue;
            if (auto meta = TokenMeta::from_json(json::parse(*value))) out.push_back(std::move(*meta));
        } catch (const std::exception&) {
            // corrupt or incomplete entry: ignore (fail closed: it can never authenticate)
        }
    }
    std::sort(out.begin(), out.end(), [](const TokenMeta& a, const TokenMeta& b) { return a.name < b.name; });
    return out;
}

const TokenStore::Snapshot& TokenStore::snapshot() {
    const TimePoint now = clock_();
    if (!snapshot_.valid || now - snapshot_.loaded >= ttl_ || now < snapshot_.loaded) {
        Snapshot fresh;
        for (auto& meta : load_all()) fresh.by_id[meta.id] = std::move(meta);
        fresh.loaded = now;
        fresh.valid = true;
        snapshot_ = std::move(fresh);
    }
    return snapshot_;
}

void TokenStore::invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.valid = false;
}

std::optional<TokenMeta> TokenStore::find_by_id(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& snap = snapshot();
    const auto it = snap.by_id.find(id);
    if (it == snap.by_id.end()) return std::nullopt;
    return it->second;
}

std::size_t TokenStore::count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot().by_id.size();
}

std::vector<TokenMeta> TokenStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    return load_all();
}

void TokenStore::save(const TokenMeta& meta) {
    write_record(meta.name, meta.to_compact_json().dump());
    snapshot_.valid = false;
}

CreatedToken TokenStore::issue(TokenMeta meta) {
    std::vector<unsigned char> secret_raw(kSecretBytes);
    rng_(secret_raw.data(), secret_raw.size());
    const std::string secret = base64url_encode(secret_raw.data(), secret_raw.size());
    meta.id = random_hex(rng_, kIdChars / 2);
    meta.secret_hash = sha256_hex(secret);
    save(meta);
    CreatedToken out;
    out.token = format_token(meta.id, secret);
    out.meta = std::move(meta);
    return out;
}

CreatedToken TokenStore::create(const NewToken& request) {
    validate(request);
    std::lock_guard<std::mutex> lock(mutex_);
    if (backend_->get(request.name)) throw AuthError("TOKEN_EXISTS", "a token named '" + request.name + "' already exists");
    TokenMeta meta;
    meta.name = request.name;
    meta.created = clock_();
    meta.expires = request.expires;
    meta.scopes = request.scopes;
    meta.sap_systems = request.sap_systems;
    meta.tcodes = request.tcodes;
    meta.connections = request.connections;
    meta.rate_per_minute = request.rate_per_minute;
    meta.allowed_ips = request.allowed_ips;
    meta.read_only = request.read_only;
    // The id must be unique among stored tokens.
    for (int attempt = 0; attempt < 8; ++attempt) {
        CreatedToken created = issue(meta);
        bool clash = false;
        for (const auto& other : load_all())
            if (other.id == created.meta.id && other.name != created.meta.name) clash = true;
        if (!clash) return created;
    }
    throw AuthError("TOKEN_STORE_ERROR", "could not allocate a unique token id");
}

bool TokenStore::revoke(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool incomplete = false;
    const auto value = read_record(name, &incomplete);
    if (!value && !incomplete) return false;
    if (!value) throw AuthError("TOKEN_CORRUPT", "the stored record of token '" + name + "' is incomplete");
    std::optional<TokenMeta> meta;
    try { meta = TokenMeta::from_json(json::parse(*value)); } catch (const std::exception&) {}
    if (!meta) throw AuthError("TOKEN_CORRUPT", "the stored record of token '" + name + "' is unreadable");
    meta->revoked = true;
    save(*meta);
    return true;
}

bool TokenStore::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool removed = backend_->remove(name);  // head first: without it the record no longer authenticates
    remove_chunks(name, 1);
    snapshot_.valid = false;
    return removed;
}

CreatedToken TokenStore::rotate(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool incomplete = false;
    const auto value = read_record(name, &incomplete);
    if (!value && !incomplete) throw AuthError("TOKEN_NOT_FOUND", "no token named '" + name + "'");
    if (!value) throw AuthError("TOKEN_CORRUPT", "the stored record of token '" + name + "' is incomplete");
    std::optional<TokenMeta> meta;
    try { meta = TokenMeta::from_json(json::parse(*value)); } catch (const std::exception&) {}
    if (!meta) throw AuthError("TOKEN_CORRUPT", "the stored record of token '" + name + "' is unreadable");
    if (meta->revoked) throw AuthError("TOKEN_REVOKED", "token '" + name + "' is revoked; create a new one");
    return issue(*meta);
}

} // namespace fairyfly::auth
