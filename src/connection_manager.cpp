#include "include/connection_manager.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <regex>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;
using namespace fairyfly::utils;

namespace fairyfly {
namespace cli {

// Connection methods

json Connection::to_json() const {
    return json{
        {"id", id},
        {"session_id", session_id},
        {"connection_id", connection_id},
        {"connection_description", connection_description},
        {"connection_string", connection_string},
        {"window_title", window_title},
        {"created_at", created_at},
        {"last_validated", last_validated}
    };
}

Connection Connection::from_json(const json& j) {
    Connection conn;
    conn.id = j.at("id").get<int>();
    conn.session_id = j.at("session_id").get<std::string>();
    conn.connection_id = j.at("connection_id").get<std::string>();
    conn.connection_description = j.value("connection_description", "");
    conn.connection_string = j.value("connection_string", "");
    conn.window_title = j.value("window_title", "");
    conn.created_at = j.value("created_at", "");
    conn.last_validated = j.value("last_validated", "");
    return conn;
}

std::string Connection::get_file_path() const {
    return fmt::format("fairyfly.{}.con", id);
}

// ConnectionManager methods

std::string ConnectionManager::get_current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t);
#else
    gmtime_r(&time_t, &tm_buf);
#endif

    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

int ConnectionManager::get_next_connection_id() const {
    TraceGuard trace("ConnectionManager::get_next_connection_id");
    (void)trace;  // Prevent unused warning

    int max_id = -1;
    std::regex pattern(R"(fairyfly\.(\d+)\.con)");

    try {
        for (const auto& entry : fs::directory_iterator(working_directory_)) {
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();
            std::smatch match;

            if (std::regex_match(filename, match, pattern)) {
                int id = std::stoi(match[1].str());
                max_id = std::max(max_id, id);
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("Error scanning directory for connection files: {}", e.what());
    }

    int next_id = max_id + 1;
    spdlog::debug("Next connection ID: {}", next_id);
    return next_id;
}

ConnectionManager::ConnectionManager()
    : working_directory_(fs::current_path().string())
{
    spdlog::debug("ConnectionManager initialized with working directory: {}", working_directory_);
}

ConnectionManager::ConnectionManager(const std::string& working_dir)
    : working_directory_(working_dir)
{
    spdlog::debug("ConnectionManager initialized with explicit directory: {}", working_directory_);
}

std::vector<Connection> ConnectionManager::list_connections() const {
    TraceGuard trace("ConnectionManager::list_connections");

    std::vector<Connection> connections;
    std::regex pattern(R"(fairyfly\.(\d+)\.con)");

    try {
        for (const auto& entry : fs::directory_iterator(working_directory_)) {
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();
            std::smatch match;

            if (std::regex_match(filename, match, pattern)) {
                try {
                    std::ifstream file(entry.path());
                    json j;
                    file >> j;

                    Connection conn = Connection::from_json(j);
                    connections.push_back(conn);

                    spdlog::debug("Loaded connection: {}", conn.get_file_path());
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to parse connection file {}: {}", filename, e.what());
                }
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("Error listing connection files: {}", e.what());
    }

    // Sort by ID
    std::sort(connections.begin(), connections.end(),
              [](const Connection& a, const Connection& b) { return a.id < b.id; });

    spdlog::info("Found {} connection file(s)", connections.size());
    return connections;
}

std::optional<Connection> ConnectionManager::load_connection(int id) const {
    TraceGuard trace("ConnectionManager::load_connection");
    (void)trace;
    spdlog::debug("load_connection: id={}", id);

    std::string filepath = (fs::path(working_directory_) / fmt::format("fairyfly.{}.con", id)).string();

    if (!fs::exists(filepath)) {
        spdlog::debug("Connection file not found: {}", filepath);
        return std::nullopt;
    }

    try {
        std::ifstream file(filepath);
        json j;
        file >> j;

        Connection conn = Connection::from_json(j);
        spdlog::debug("Loaded connection {}: session={}", conn.id, conn.session_id);
        return conn;
    } catch (const std::exception& e) {
        spdlog::error("Failed to load connection file {}: {}", filepath, e.what());
        return std::nullopt;
    }
}

ResultT<Connection> ConnectionManager::resolve_connection(std::optional<int> explicit_id) const {
    TraceGuard trace("ConnectionManager::resolve_connection");
    (void)trace;
    spdlog::debug("resolve_connection: explicit_id={}", explicit_id.has_value() ? explicit_id.value() : -1);

    ResultT<Connection> result;

    // If explicit ID provided, try to load it
    if (explicit_id.has_value()) {
        auto conn = load_connection(explicit_id.value());
        if (!conn.has_value()) {
            result.status = ResultT<Connection>::Status::Error;
            result.error["code"] = "CONNECTION_NOT_FOUND";
            result.error["message"] = fmt::format("Connection file fairyfly.{}.con not found", explicit_id.value());
            result.error["suggestions"] = json::array({
                "Run 'fairyfly connections' to see available connections",
                "Run 'fairyfly attach' to create a new connection"
            });
            return result;
        }

        result.status = ResultT<Connection>::Status::Success;
        result.value = conn.value();
        spdlog::info("Resolved connection {} (explicit)", result.value.id);
        return result;
    }

    // No explicit ID - auto-detect
    auto connections = list_connections();

    if (connections.empty()) {
        result.status = ResultT<Connection>::Status::Error;
        result.error["code"] = "NO_CONNECTIONS";
        result.error["message"] = "No connection files found in current directory";
        result.error["suggestions"] = json::array({
            "Run 'fairyfly attach' to attach to a running SAP window",
            "Run 'fairyfly launch <connection>' to launch a new SAP connection"
        });
        return result;
    }

    if (connections.size() == 1) {
        result.status = ResultT<Connection>::Status::Success;
        result.value = connections[0];
        spdlog::info("Auto-detected single connection: {}", result.value.id);
        return result;
    }

    // Multiple connections - require explicit ID
    result.status = ResultT<Connection>::Status::Error;
    result.error["code"] = "MULTIPLE_CONNECTIONS";
    result.error["message"] = fmt::format("Found {} connections, please specify which one to use", connections.size());

    json conn_list = json::array();
    for (const auto& conn : connections) {
        conn_list.push_back({
            {"id", conn.id},
            {"session_id", conn.session_id},
            {"description", conn.connection_description}
        });
    }
    result.error["connections"] = conn_list;
    result.error["suggestions"] = json::array({
        "Add --connection <id> flag to specify which connection to use",
        "Run 'fairyfly connections' to see all available connections"
    });

    return result;
}

Connection ConnectionManager::create_or_update_connection(
    const std::string& session_id,
    const std::string& connection_id,
    const std::string& description,
    const std::string& connection_string,
    const std::string& window_title)
{
    TraceGuard trace("ConnectionManager::create_or_update_connection");
    (void)trace;
    spdlog::debug("create_or_update_connection: session_id={}", session_id);

    // Check if connection already exists for this session
    auto existing = find_by_session_id(session_id);

    Connection conn;
    std::string timestamp = get_current_timestamp();

    if (existing.has_value()) {
        // Update existing connection
        conn = existing.value();
        conn.connection_description = description;
        conn.connection_string = connection_string;
        conn.window_title = window_title;
        conn.last_validated = timestamp;

        spdlog::info("Updating existing connection {}", conn.id);
    } else {
        // Create new connection
        conn.id = get_next_connection_id();
        conn.session_id = session_id;
        conn.connection_id = connection_id;
        conn.connection_description = description;
        conn.connection_string = connection_string;
        conn.window_title = window_title;
        conn.created_at = timestamp;
        conn.last_validated = timestamp;

        spdlog::info("Creating new connection {}", conn.id);
    }

    // Write to file
    std::string filepath = (fs::path(working_directory_) / conn.get_file_path()).string();
    try {
        std::ofstream file(filepath);
        file << std::setw(2) << conn.to_json() << std::endl;
        file.close();

        spdlog::info("Connection file written: {}", filepath);
    } catch (const std::exception& e) {
        spdlog::error("Failed to write connection file {}: {}", filepath, e.what());
    }

    return conn;
}

void ConnectionManager::touch_connection(int id) {
    TraceGuard trace("ConnectionManager::touch_connection");
    (void)trace;
    spdlog::debug("touch_connection: id={}", id);

    auto conn = load_connection(id);
    if (!conn.has_value()) {
        spdlog::warn("Cannot touch connection {}: not found", id);
        return;
    }

    conn.value().last_validated = get_current_timestamp();

    std::string filepath = (fs::path(working_directory_) / conn.value().get_file_path()).string();
    try {
        std::ofstream file(filepath);
        file << std::setw(2) << conn.value().to_json() << std::endl;
        file.close();

        spdlog::debug("Updated last_validated for connection {}", id);
    } catch (const std::exception& e) {
        spdlog::error("Failed to update connection file {}: {}", filepath, e.what());
    }
}

bool ConnectionManager::delete_connection(int id) {
    TraceGuard trace("ConnectionManager::delete_connection");
    (void)trace;
    spdlog::debug("delete_connection: id={}", id);

    std::string filepath = (fs::path(working_directory_) / fmt::format("fairyfly.{}.con", id)).string();

    if (!fs::exists(filepath)) {
        spdlog::debug("Connection file does not exist: {}", filepath);
        return false;
    }

    try {
        fs::remove(filepath);
        spdlog::info("Deleted connection file: {}", filepath);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to delete connection file {}: {}", filepath, e.what());
        return false;
    }
}

int ConnectionManager::cleanup_invalid_connections(
    std::function<bool(const std::string& session_id)> validate_func)
{
    TraceGuard trace("ConnectionManager::cleanup_invalid_connections");

    auto connections = list_connections();
    int deleted_count = 0;

    for (const auto& conn : connections) {
        if (!validate_func(conn.session_id)) {
            spdlog::info("Connection {} has invalid session {}, deleting", conn.id, conn.session_id);
            if (delete_connection(conn.id)) {
                deleted_count++;
            }
        }
    }

    spdlog::info("Cleaned up {} invalid connection(s)", deleted_count);
    return deleted_count;
}

std::optional<Connection> ConnectionManager::find_by_session_id(const std::string& session_id) const {
    TraceGuard trace("ConnectionManager::find_by_session_id");
    (void)trace;
    spdlog::debug("find_by_session_id: session_id={}", session_id);

    auto connections = list_connections();

    for (const auto& conn : connections) {
        if (conn.session_id == session_id) {
            spdlog::debug("Found existing connection {} for session {}", conn.id, session_id);
            return conn;
        }
    }

    return std::nullopt;
}

}  // namespace cli
}  // namespace fairyfly
