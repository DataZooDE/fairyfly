#pragma once

#include <chrono>
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace fairyfly::mcp {

// Broker-owned routing defaults. Providers may be rebuilt when tray policy changes;
// the defaults belong to the authenticated principal, not a provider instance.
class SessionRoutingState {
public:
    using Clock = std::chrono::steady_clock;
    struct StickyTarget {
        int connection;
        std::string session_identity;
    };

    std::optional<StickyTarget> sticky_target(const std::string& principal_key) const {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = sticky_.find(principal_key);
        return it == sticky_.end() ? std::nullopt : std::optional<StickyTarget>(it->second);
    }

    std::optional<int> sticky_connection(const std::string& principal_key) const {
        const auto target = sticky_target(principal_key);
        return target ? std::optional<int>(target->connection) : std::nullopt;
    }

    void set_sticky_connection(const std::string& principal_key, int connection, std::string session_identity) {
        std::lock_guard<std::mutex> lock(mu_);
        sticky_[principal_key] = StickyTarget{connection, std::move(session_identity)};
    }

    /// An unauthenticated SAP window has no SAP user to authorize. Only the token that
    /// launched this exact saved generation may complete its logon. Claims are short-lived
    /// and bounded; a reused numeric ID with a new generation replaces a stale claim.
    bool prelogin_capacity_available(const std::string& token_id, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mu_);
        prune_expired_prelogin_locked(now);
        return !token_id.empty() && prelogin_.size() < 64 &&
            std::count_if(prelogin_.begin(), prelogin_.end(), [&](const auto& entry) {
                return entry.second.token_id == token_id;
            }) < 8;
    }

    bool bind_prelogin(const std::string& token_id, int connection, const std::string& session_identity,
                       Clock::time_point now = Clock::now()) {
        if (token_id.empty() || connection < 0 || session_identity.empty()) return false;
        std::lock_guard<std::mutex> lock(mu_);
        prune_expired_prelogin_locked(now);
        const auto found = prelogin_.find(connection);
        if (found != prelogin_.end() && found->second.session_identity == session_identity &&
            found->second.token_id != token_id) return false;
        if (found == prelogin_.end() && prelogin_.size() >= 64) return false;
        if ((found == prelogin_.end() || found->second.token_id != token_id) &&
            std::count_if(prelogin_.begin(), prelogin_.end(), [&](const auto& entry) {
                return entry.second.token_id == token_id;
            }) >= 8) return false;
        prelogin_[connection] = PreloginClaim{token_id, session_identity, now + std::chrono::minutes(15)};
        return true;
    }

    bool owns_prelogin(const std::string& token_id, int connection, const std::string& session_identity,
                       Clock::time_point now = Clock::now()) const {
        std::lock_guard<std::mutex> lock(mu_);
        const auto found = prelogin_.find(connection);
        return found != prelogin_.end() && found->second.expires_at > now &&
            found->second.token_id == token_id && found->second.session_identity == session_identity;
    }

    // Routing uses this only to choose the tray control lane. The dispatcher
    // must still compare the exact live session identity before closing it.
    bool has_prelogin_claim(const std::string& token_id, int connection,
                            Clock::time_point now = Clock::now()) const {
        std::lock_guard<std::mutex> lock(mu_);
        const auto found = prelogin_.find(connection);
        return found != prelogin_.end() && found->second.expires_at > now &&
            found->second.token_id == token_id;
    }

    void finish_prelogin(const std::string& token_id, int connection, const std::string& session_identity) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto found = prelogin_.find(connection);
        if (found != prelogin_.end() && found->second.token_id == token_id &&
            found->second.session_identity == session_identity) prelogin_.erase(found);
    }

    void revoke_principal(const std::string& principal_key) {
        std::lock_guard<std::mutex> lock(mu_);
        sticky_.erase(principal_key);
        for (auto it = prelogin_.begin(); it != prelogin_.end();) {
            if (it->second.token_id == principal_key) it = prelogin_.erase(it);
            else ++it;
        }
    }

    std::set<std::string> principal_ids() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::set<std::string> ids;
        for (const auto& [id, _] : sticky_) ids.insert(id);
        for (const auto& [_, claim] : prelogin_) ids.insert(claim.token_id);
        return ids;
    }

    void retire_session(const std::string& session_identity) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = sticky_.begin(); it != sticky_.end();) {
            if (it->second.session_identity == session_identity) it = sticky_.erase(it);
            else ++it;
        }
        for (auto it = prelogin_.begin(); it != prelogin_.end();) {
            if (it->second.session_identity == session_identity) it = prelogin_.erase(it);
            else ++it;
        }
    }

private:
    void prune_expired_prelogin_locked(Clock::time_point now) {
        for (auto it = prelogin_.begin(); it != prelogin_.end();) {
            if (it->second.expires_at <= now) it = prelogin_.erase(it);
            else ++it;
        }
    }
    struct PreloginClaim {
        std::string token_id;
        std::string session_identity;
        Clock::time_point expires_at;
    };
    mutable std::mutex mu_;
    std::map<std::string, StickyTarget> sticky_;
    std::map<int, PreloginClaim> prelogin_;
};

} // namespace fairyfly::mcp
