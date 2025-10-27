#pragma once

#include "core.h"
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

/// Manages connection files in current working directory
class ConnectionManager {
private:
    std::string working_directory_;

    /// Get current timestamp in ISO 8601 format
    static std::string get_current_timestamp();

    /// Get next available connection ID by scanning existing files
    int get_next_connection_id() const;

public:
    /// Constructor - uses current working directory
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
    /// If connection with same session_id exists, updates it; otherwise creates new
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
        const std::string& window_title
    );

    /// Update last_validated timestamp for connection
    void touch_connection(int id);

    /// Delete connection file
    /// \return true if file was deleted, false if not found
    bool delete_connection(int id);

    /// Delete all invalid connections (helper for cleanup)
    /// \param validate_func Function that validates if session still exists
    /// \return Number of connections deleted
    int cleanup_invalid_connections(
        std::function<bool(const std::string& session_id)> validate_func
    );

    /// Find connection by session_id
    std::optional<Connection> find_by_session_id(const std::string& session_id) const;
};

}  // namespace cli
}  // namespace fairyfly
