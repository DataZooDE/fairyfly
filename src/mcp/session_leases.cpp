#include "include/mcp/session_leases.h"

#include <stdexcept>
#include <utility>

#include "include/auth/crypto.h"
#include "include/mcp/session_policy_state.h"

namespace fairyfly::mcp {

namespace {
std::string window_key(const std::string& session_identity) {
    return policy_session_identity(session_identity);
}
}

SessionLeases::SessionLeases(Clock::duration ttl, IdFactory id_factory, TokenValid token_valid)
    : ttl_(ttl), id_factory_(id_factory ? std::move(id_factory) : IdFactory([] {
          return auth::random_hex(auth::random_bytes, 16);
      })), token_valid_(std::move(token_valid)) {
    if (ttl_ <= Clock::duration::zero()) throw std::invalid_argument("session lease TTL must be positive");
}

void SessionLeases::prune_expired(Clock::time_point now) {
    for (auto it = leases_.begin(); it != leases_.end(); ) {
        if ((now >= it->second.expires_at || it->second.retired) && it->second.active_writes == 0)
            it = leases_.erase(it);
        else ++it;
    }
}

LeaseResult SessionLeases::acquire(const std::string& session_key, const std::string& token_id,
                                   Clock::time_point now) {
    if (session_key.empty() || token_id.empty()) return {LeaseStatus::Invalid};
    const std::string key = window_key(session_key);
    std::string holder_token;
    std::string holder_lease;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prune_expired(now);
        const auto it = leases_.find(key);
        if (it != leases_.end() && token_valid_) {
            holder_token = it->second.token_id;
            holder_lease = it->second.lease_id;
        }
    }
    bool valid = true;
    const auto validation_started = Clock::now();
    if (!holder_lease.empty()) {
        // Credential Manager may be temporarily unavailable. Retain the holder
        // unless its revocation/expiry can be established, and never transfer
        // an active write before its worker reports completion. Do this outside
        // the lease lock so a slow store cannot stall unrelated GUI windows.
        try { valid = token_valid_(holder_token); } catch (...) {}
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // Preserve the caller's exact timestamp for an uncontested first acquire.
    // A holder check can spend substantial time in Credential Manager, so
    // advance that timestamp by the validation and second-lock wait.
    const auto effective_now = holder_lease.empty() ? now : now + (Clock::now() - validation_started);
    prune_expired(effective_now);
    auto it = leases_.find(key);
    if (!valid && it != leases_.end() && it->second.token_id == holder_token &&
        auth::constant_time_equal(it->second.lease_id, holder_lease)) {
            if (it->second.active_writes == 0) {
                leases_.erase(it);
                it = leases_.end();
            } else {
                it->second.retired = true;
            }
    }
    // Bearer tokens may be shared by several MCP clients. Only a caller holding the
    // lease secret may renew; acquire must never reveal an existing secret.
    if (it != leases_.end()) return {LeaseStatus::HeldByOther};
    const std::string id = id_factory_();
    if (id.empty()) throw std::runtime_error("session lease ID factory returned an empty ID");
    const auto expires = effective_now + ttl_;
    leases_.emplace(key, Lease{session_key, token_id, id, expires, 0, false});
    return {LeaseStatus::Granted, id, expires};
}

LeaseResult SessionLeases::renew(const std::string& session_key, const std::string& token_id,
                                 const std::string& lease_id, Clock::time_point now) {
    if (session_key.empty() || token_id.empty() || lease_id.empty()) return {LeaseStatus::Invalid};
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired(now);
    auto it = leases_.find(window_key(session_key));
    if (it == leases_.end() || it->second.retired || now >= it->second.expires_at ||
        it->second.session_identity != session_key || it->second.token_id != token_id ||
        !auth::constant_time_equal(it->second.lease_id, lease_id))
        return {LeaseStatus::NotHeld};
    it->second.expires_at = now + ttl_;
    return {LeaseStatus::Granted, lease_id, it->second.expires_at};
}

bool SessionLeases::release(const std::string& session_key, const std::string& token_id,
                            const std::string& lease_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = leases_.find(window_key(session_key));
    if (it == leases_.end() || it->second.active_writes != 0 || it->second.session_identity != session_key ||
        it->second.token_id != token_id ||
        !auth::constant_time_equal(it->second.lease_id, lease_id)) return false;
    leases_.erase(it);
    return true;
}

bool SessionLeases::permits_write(const std::string& session_key, const std::string& token_id,
                                  const std::string& lease_id, Clock::time_point now) const {
    if (session_key.empty() || token_id.empty() || lease_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = leases_.find(window_key(session_key));
    return it != leases_.end() && !it->second.retired && now < it->second.expires_at &&
           it->second.session_identity == session_key && it->second.token_id == token_id &&
           auth::constant_time_equal(it->second.lease_id, lease_id);
}

bool SessionLeases::begin_write(const std::string& session_key, const std::string& token_id,
                                const std::string& lease_id, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = leases_.find(window_key(session_key));
    if (it == leases_.end() || it->second.retired || now >= it->second.expires_at ||
        it->second.session_identity != session_key || it->second.token_id != token_id ||
        !auth::constant_time_equal(it->second.lease_id, lease_id)) return false;
    ++it->second.active_writes;
    return true;
}

void SessionLeases::end_write(const std::string& session_key, const std::string& token_id,
                              const std::string& lease_id, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = leases_.find(window_key(session_key));
    if (it == leases_.end() || it->second.session_identity != session_key || it->second.token_id != token_id ||
        !auth::constant_time_equal(it->second.lease_id, lease_id) || it->second.active_writes == 0) return;
    --it->second.active_writes;
    if (it->second.active_writes == 0 && (it->second.retired || now >= it->second.expires_at)) leases_.erase(it);
}

void SessionLeases::retire_session(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = leases_.find(window_key(session_key));
    if (it == leases_.end()) return;
    if (it->second.active_writes == 0) leases_.erase(it);
    else it->second.retired = true;
}

void SessionLeases::revoke_token(const std::string& token_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = leases_.begin(); it != leases_.end(); ) {
        if (it->second.token_id == token_id && it->second.active_writes == 0) it = leases_.erase(it);
        else if (it->second.token_id == token_id) { it->second.retired = true; ++it; }
        else ++it;
    }
}

std::set<std::string> SessionLeases::token_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> ids;
    for (const auto& [_, lease] : leases_) ids.insert(lease.token_id);
    return ids;
}

void SessionLeases::sweep_expired(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired(now);
}

} // namespace fairyfly::mcp
