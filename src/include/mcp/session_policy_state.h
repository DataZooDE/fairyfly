#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "include/auth/authorize.h"

namespace fairyfly::mcp {

// Policy follows the live SAP window across a Fairyfly saved-connection
// detach/reattach. The route descriptor's third component is the cache
// generation; including it here would discard a T-code block on reattach.
inline std::string policy_session_identity(const std::string& route_identity) {
    const auto first = route_identity.find('|');
    const auto second = first == std::string::npos ? std::string::npos : route_identity.find('|', first + 1);
    return second == std::string::npos ? route_identity : route_identity.substr(0, second);
}

// Security decisions that must survive provider rebuilds and, later, worker replacement.
// Keys are token issuance ID plus the checked session identity (see session_policy_key).
class SessionPolicyState {
public:
    std::set<std::string> token_ids() const {
        std::set<std::string> ids;
        const auto add = [&](const std::string& key) {
            const auto separator = key.find('\x1f');
            const std::string id = key.substr(0, separator);
            if (!id.empty()) ids.insert(id);
        };
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& key : tcode_left_) add(key);
        for (const auto& [key, _] : initial_screen_) add(key);
        return ids;
    }

    bool tcode_blocked(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        return tcode_left_.count(key) != 0;
    }

    void set_tcode_blocked(const std::string& key, bool blocked) {
        std::lock_guard<std::mutex> lock(mu_);
        if (blocked) tcode_left_.insert(key);
        else tcode_left_.erase(key);
    }

    std::optional<auth::InitialScreen> initial_screen(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = initial_screen_.find(key);
        return it == initial_screen_.end() ? std::nullopt : std::optional<auth::InitialScreen>(it->second);
    }

    void set_initial_screen(const std::string& key, const std::optional<auth::InitialScreen>& screen) {
        std::lock_guard<std::mutex> lock(mu_);
        if (screen) initial_screen_[key] = *screen;
        else initial_screen_.erase(key);
    }

    // A closed/replaced saved session cannot carry its policy state to a new
    // occupant of the same numeric connection ID. The session key and its
    // conservative connection fallback are retired together.
    void retire_session(const std::string& identity, int connection, bool retire_connection_id = true) {
        if (identity.empty() || connection < 0) return;
        const std::string session_suffix = std::string("\x1f") + "session:" + policy_session_identity(identity);
        const std::string connection_suffix = std::string("\x1f") + "connection:" + std::to_string(connection);
        const auto matches = [&](const std::string& key) {
            return key.ends_with(session_suffix) || (retire_connection_id && key.ends_with(connection_suffix));
        };
        std::lock_guard<std::mutex> lock(mu_);
        std::erase_if(tcode_left_, [&](const std::string& key) { return matches(key); });
        std::erase_if(initial_screen_, [&](const auto& entry) { return matches(entry.first); });
    }

    // Detach deletes only the saved numeric ID. The SAP window remains open,
    // so its session-keyed transaction block must survive reattach.
    void retire_connection(int connection) {
        if (connection < 0) return;
        const std::string suffix = std::string("\x1f") + "connection:" + std::to_string(connection);
        std::lock_guard<std::mutex> lock(mu_);
        std::erase_if(tcode_left_, [&](const std::string& key) { return key.ends_with(suffix); });
        std::erase_if(initial_screen_, [&](const auto& entry) { return entry.first.ends_with(suffix); });
    }

    void revoke_token(const std::string& token_id) {
        if (token_id.empty()) return;
        const std::string prefix = token_id + "\x1f";
        const auto matches = [&](const std::string& key) {
            return key == token_id || key.starts_with(prefix);
        };
        std::lock_guard<std::mutex> lock(mu_);
        std::erase_if(tcode_left_, [&](const std::string& key) { return matches(key); });
        std::erase_if(initial_screen_, [&](const auto& entry) { return matches(entry.first); });
    }

private:
    mutable std::mutex mu_;
    std::set<std::string> tcode_left_;
    std::map<std::string, auth::InitialScreen> initial_screen_;
};

} // namespace fairyfly::mcp
