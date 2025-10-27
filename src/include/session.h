#pragma once

#include "core.h"
#include <vector>
#include <optional>
#include <filesystem>
#include <chrono>

namespace fairyfly {
namespace session {

/// Session state information - deprecated
struct SessionState {
    SessionId id;
    std::string profile_name;
    bool connected = false;
    std::chrono::system_clock::time_point created;
    std::chrono::system_clock::time_point last_activity;
    std::string current_transaction;

    json to_json() const { return json::object(); }
    static SessionState from_json(const json& j) { return SessionState(); }
};

/// Session manager - deprecated, use direct SAP attachment instead
class SessionManager {
public:
    /// Create session manager with default paths
    SessionManager() {}

    /// Create session manager with custom directory
    explicit SessionManager(const std::filesystem::path& session_dir) {}

    /// Load all sessions from disk (no-op)
    bool load_all() { return true; }

    /// Save a session to disk (no-op)
    bool save_session(const SessionState& session) { return true; }

    /// Load specific session (empty)
    std::optional<SessionState> get_session(const SessionId& id) { return {}; }

    /// List all active sessions (empty)
    std::vector<SessionState> list_sessions() const { return {}; }

    /// Create new session (stub)
    SessionState create_session(const std::string& profile_name) { return SessionState(); }

    /// Update session state (no-op)
    void update_session(const SessionState& session) {}

    /// Clean up expired sessions (no-op)
    int cleanup_expired_sessions() { return 0; }

    /// Get session directory path
    std::filesystem::path get_session_dir() const { return std::filesystem::path(); }
};

} // namespace session
} // namespace fairyfly
