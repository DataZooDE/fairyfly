#pragma once

#include "core.h"
#include <functional>
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace fairyfly {
namespace cli {

using json = nlohmann::json;

/// Connection information stored in fairyfly.N.con files
struct Connection {
    int id;                           ///< Connection ID (N in fairyfly.N.con)
    std::string session_id;           ///< Full session path (e.g., /app/con[0]/ses[0])
    std::string server_session_key;   ///< SAP SystemSessionId/SessionNumber, when available
    std::string cache_generation;     ///< Unique identity of this cache-file creation
    std::string connection_id;        ///< Connection path (e.g., /app/con[0])
    std::string connection_description; ///< Human-readable connection name
    std::string connection_string;    ///< SAP connection string
    std::string window_title;         ///< Window title at attachment time
    std::string created_at;           ///< ISO 8601 timestamp of creation
    std::string last_validated;       ///< ISO 8601 timestamp of last validation

    /// Convert to JSON for serialization
    json to_json() const;

    /// Parse from JSON
    static Connection from_json(const json& j);

    /// Get file path for this connection
    std::string get_file_path() const;
};

/// Result of splitting cache entries by whether their SAP session is alive
struct ConnectionPartition {
    std::vector<Connection> live;
    std::vector<Connection> stale;
};

/// Split connections using a liveness predicate (unit-testable, no SAP access).
ConnectionPartition partition_connections(
    const std::vector<Connection>& connections,
    const std::function<bool(const Connection&)>& is_live);

/// Manages connection files in current working directory
class ConnectionManager {
private:
    std::string working_directory_;

    /// Get current timestamp in ISO 8601 format
    static std::string get_current_timestamp();

    /// Get next available connection ID by scanning existing files
    int get_next_connection_id() const;

public:
    /// Get default isolated cache directory for connection files
    static std::string get_default_cache_directory();

    /// Get current working directory used by this connection manager
    const std::string& get_working_directory() const { return working_directory_; }

    /// Constructor - uses default isolated cache directory
    ConnectionManager();

    /// Constructor with explicit working directory (for testing)
    explicit ConnectionManager(const std::string& working_dir);

    /// Scan directory for all connection files
    std::vector<Connection> list_connections() const;

    /// Load specific connection by ID
    std::optional<Connection> load_connection(int id) const;

    /// Resolve connection for command execution
    /// Returns connection if single file exists, requires explicit ID if multiple
    /// \param explicit_id Optional connection ID specified by user
    /// \return Connection to use, or error explaining what's wrong
    ResultT<Connection> resolve_connection(std::optional<int> explicit_id) const;

    /// Create or update connection file
    /// Updates a matching session path and server key, or creates a new file
    /// \param session_id Session path from SAP
    /// \param connection_id Connection path from SAP
    /// \param description Human-readable connection name
    /// \param connection_string SAP connection string
    /// \param window_title Window title at attachment
    /// \return Created/updated connection
    Connection create_or_update_connection(
        const std::string& session_id,
        const std::string& connection_id,
        const std::string& description,
        const std::string& connection_string,
        const std::string& window_title,
        const std::string& server_session_key = ""
    );

    /// Create a new saved connection for an owner-mode launch. Never rewrites an
    /// existing file; a colliding or keyless reused GUI path is ambiguous.
    Connection create_new_connection(
        const std::string& session_id,
        const std::string& connection_id,
        const std::string& description,
        const std::string& connection_string,
        const std::string& window_title,
        const std::string& server_session_key = ""
    );

    /// Update last_validated only if the file still matches the selected snapshot
    void touch_connection(const Connection& expected);

    /// Upgrade an explicitly selected legacy cache file after live validation.
    Connection set_session_key(const Connection& expected, const std::string& server_session_key);

    /// Delete connection file
    /// \return true if file was deleted, false if not found
    bool delete_connection(int id);

    /// Delete only if the cache file still represents the validated snapshot.
    bool delete_connection_if_unchanged(const Connection& expected);

    /// Check an exact cache generation under the cross-process cache lock.
    bool matches_connection_generation(const Connection& expected) const;

    /// Delete every other cache entry for the same session path as `keep`.
    /// Only safe after `keep` was validated live: a path names one live session.
    /// Entries replaced concurrently are skipped.
    /// `live_key` is the CURRENT live server session key of that path, re-read right before
    /// pruning: an entry whose non-empty key equals it is never deleted (another process may
    /// have cached a newer live session on the reused path since the list). Empty = old behavior.
    /// \return Number of stale entries deleted
    int prune_other_entries_for_path(const Connection& keep, const std::string& live_key = "");

    /// Delete all invalid connections (helper for cleanup)
    /// \param validate_func Function that validates the saved session identity
    /// \return Number of connections deleted
    int cleanup_invalid_connections(
        std::function<bool(const Connection&)> validate_func
    );

    /// Find connection by session_id
    std::optional<Connection> find_by_session_id(const std::string& session_id) const;
};

}  // namespace cli
}  // namespace fairyfly
