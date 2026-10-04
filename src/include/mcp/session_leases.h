#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace fairyfly::mcp {

enum class LeaseStatus { Granted, HeldByOther, NotHeld, Invalid };

struct LeaseResult {
    LeaseStatus status = LeaseStatus::Invalid;
    std::string lease_id;  // Returned only to the owning token.
    std::chrono::steady_clock::time_point expires_at{};
};

/// Exclusive, expiring workflow leases for a verified SAP window. A saved-cache
/// generation is required for use, while all generations of the same live
/// session share one ownership slot.
/// Thread-safe so the broker can retain leases while calls run on separate workers.
class SessionLeases {
public:
    using Clock = std::chrono::steady_clock;
    using IdFactory = std::function<std::string()>;
    using TokenValid = std::function<bool(const std::string&)>;

    explicit SessionLeases(Clock::duration ttl, IdFactory id_factory = {}, TokenValid token_valid = {});

    LeaseResult acquire(const std::string& session_key, const std::string& token_id, Clock::time_point now);
    LeaseResult renew(const std::string& session_key, const std::string& token_id,
                      const std::string& lease_id, Clock::time_point now);
    bool release(const std::string& session_key, const std::string& token_id, const std::string& lease_id);
    bool permits_write(const std::string& session_key, const std::string& token_id,
                       const std::string& lease_id, Clock::time_point now) const;
    /// Pin a verified lease while one SAP command is in flight; always pair with end_write.
    bool begin_write(const std::string& session_key, const std::string& token_id,
                     const std::string& lease_id, Clock::time_point now);
    void end_write(const std::string& session_key, const std::string& token_id,
                   const std::string& lease_id, Clock::time_point now);
    void retire_session(const std::string& session_key);
    void revoke_token(const std::string& token_id);
    void sweep_expired(Clock::time_point now);
    std::set<std::string> token_ids() const;

private:
    struct Lease {
        std::string session_identity;
        std::string token_id;
        std::string lease_id;
        Clock::time_point expires_at;
        std::size_t active_writes = 0;
        bool retired = false;
    };
    void prune_expired(Clock::time_point now);

    const Clock::duration ttl_;
    IdFactory id_factory_;
    TokenValid token_valid_;
    mutable std::mutex mutex_;
    std::map<std::string, Lease> leases_;
};

} // namespace fairyfly::mcp
