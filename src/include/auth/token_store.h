#pragma once
// Named bearer tokens for the remote MCP endpoint.
//   token   = ffy_<id>_<secret>      id = 8 hex chars, secret = 32 CSPRNG bytes as base64url (43 chars)
//   stored  = per token NAME one compact JSON metadata blob (short keys; id, name, sha256(secret), created, expires,
//             scopes, sap_systems, tcodes, rate_per_minute, allowed_ips, read_only, revoked). A record that does not
//             fit one Credential Manager value is split over chunk entries NAME#1..n written BEFORE the head entry
//             NAME (which holds count, length and sha256 of the payload). Old long-key single entries still load.
// The secret itself is NEVER stored; it is returned once by create()/rotate().

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/auth/crypto.h"
#include "include/auth/secret_backend.h"

namespace fairyfly::auth {

using TimePoint = std::chrono::system_clock::time_point;
using Clock = std::function<TimePoint()>;

inline constexpr const char* kTokenTargetPrefix = "fairyfly-mcp:";

/// Metadata of one token (never the secret; the hash is kept internally and is not part of to_public_json()).
struct TokenMeta {
    std::string id;
    std::string name;
    std::string secret_hash;               ///< sha256(secret), 64 hex chars
    TimePoint created{};
    std::optional<TimePoint> expires;
    std::vector<std::string> scopes;       ///< tool families or "*"
    std::vector<std::string> sap_systems;  ///< "SID/CLIENT" patterns (globs)
    std::vector<std::string> tcodes;       ///< T-code globs
    int rate_per_minute = 0;               ///< 0 = server default
    std::vector<std::string> allowed_ips;  ///< addresses or CIDR blocks; empty = any
    bool read_only = true;
    bool revoked = false;

    bool has_all_scopes() const;
    /// Stored form (includes the hash).
    nlohmann::json to_stored_json() const;
    /// Storage form: short keys, defaults omitted ("v":2). from_json() reads this and the long form.
    nlohmann::json to_compact_json() const;
    /// Listing form: no hash, no secret.
    nlohmann::json to_public_json() const;
    static std::optional<TokenMeta> from_json(const nlohmann::json& j);
};

/// What create() needs. Empty `scopes` is refused (the CLI supplies the safe default).
struct NewToken {
    std::string name;
    std::vector<std::string> scopes;
    std::vector<std::string> sap_systems;
    std::vector<std::string> tcodes;
    int rate_per_minute = 0;
    std::vector<std::string> allowed_ips;
    std::optional<TimePoint> expires;
    bool read_only = true;
};

/// create()/rotate() result: the ONLY place the plain token exists.
struct CreatedToken {
    TokenMeta meta;
    std::string token;
};

// ---- token format helpers (pure) -----------------------------------------------------------
struct ParsedToken {
    std::string id;
    std::string secret;
};
/// Parses "ffy_<8 hex>_<43 base64url>"; nullopt when malformed.
std::optional<ParsedToken> parse_token(std::string_view token);
std::string format_token(const std::string& id, const std::string& secret);
/// Short, log-safe rendering: "ffy_<id>_..." (never the secret).
std::string token_display_id(const TokenMeta& meta);

std::string format_iso_utc(TimePoint t);
std::optional<TimePoint> parse_iso_utc(const std::string& text);
/// "30d", "12h", "90m", "2026-12-31" (end of that UTC day is NOT implied: midnight UTC) -> absolute time.
std::optional<TimePoint> parse_expiry(const std::string& text, TimePoint now);
bool valid_token_name(const std::string& name);

class TokenStore {
public:
    explicit TokenStore(std::shared_ptr<SecretBackend> backend, Clock clock = {}, RandomFn rng = {},
                        std::chrono::milliseconds cache_ttl = std::chrono::milliseconds(5000));

    /// Validates (UNKNOWN_FAMILY, INVALID_ARGUMENT, INVALID_IP, TOKEN_EXISTS, TOKEN_TOO_LARGE) and stores.
    CreatedToken create(const NewToken& request);
    /// Fresh read, sorted by name. Corrupt entries are skipped.
    std::vector<TokenMeta> list();
    /// Marks the token revoked (kept for the record). false when the name is unknown. Effective immediately here.
    bool revoke(const std::string& name);
    /// Removes the token record (the Credential Manager entry) whether or not it is revoked. false when the name
    /// is unknown. Effective immediately here; other processes notice within the cache TTL.
    bool remove(const std::string& name);
    /// New id+secret, same restrictions; the old token is invalid immediately. TOKEN_REVOKED if revoked.
    CreatedToken rotate(const std::string& name);

    /// O(1) lookup used on every request; served from a cache of at most `cache_ttl` age.
    std::optional<TokenMeta> find_by_id(const std::string& id);
    /// Number of stored tokens (cached like find_by_id).
    std::size_t count();
    /// Drops the cache (used after every local mutation, callable by tests/tray).
    void invalidate();

    TimePoint now() const { return clock_(); }

private:
    struct Snapshot {
        std::map<std::string, TokenMeta> by_id;
        TimePoint loaded{};
        bool valid = false;
    };
    std::vector<TokenMeta> load_all();
    const Snapshot& snapshot();  // caller holds mutex_
    void save(const TokenMeta& meta);
    /// The record JSON of `name`: one entry, or the chunks `name#1..n` behind a head entry
    /// {"chunks":n,"len":L,"sha":sha256}. nullopt when unknown; `*incomplete` is set when a chunked record is
    /// damaged (missing chunk, size or hash mismatch): such a token never authenticates.
    std::optional<std::string> read_record(const std::string& name, bool* incomplete);
    /// Writes small records as one entry, large ones as chunks (written first) plus the head entry (last).
    void write_record(const std::string& name, const std::string& blob);
    /// Removes chunk entries `name#k` with k >= first.
    void remove_chunks(const std::string& name, std::size_t first);
    CreatedToken issue(TokenMeta meta);

    std::shared_ptr<SecretBackend> backend_;
    Clock clock_;
    RandomFn rng_;
    std::chrono::milliseconds ttl_;
    std::mutex mutex_;
    Snapshot snapshot_;
};

} // namespace fairyfly::auth
