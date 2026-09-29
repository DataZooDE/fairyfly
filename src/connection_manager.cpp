#include "include/connection_manager.h"
#include "include/exceptions.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <cstdint>
#include <regex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <cwctype>
#include <mutex>
#include <random>
#include <set>
#include <limits>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace fairyfly::utils;

namespace fairyfly {
namespace cli {

// Connection methods

json Connection::to_json() const {
    return json{
        {"id", id},
        {"session_id", session_id},
        {"server_session_key", server_session_key},
        {"cache_generation", cache_generation},
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
    conn.server_session_key = j.value("server_session_key", "");
    conn.cache_generation = j.value("cache_generation", "");
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

namespace {
fs::path existing_connection_path(const std::string& cache_dir, int id) {
    const auto filename = fmt::format("fairyfly.{}.con", id);
    const fs::path primary = fs::path(cache_dir) / filename;
    if (fs::exists(primary)) return primary;
    const fs::path legacy = fs::current_path() / filename;
    if (fs::exists(legacy)) return legacy;
    return primary;
}
}

int ConnectionManager::get_next_connection_id() const {
    TraceGuard trace("ConnectionManager::get_next_connection_id");
    (void)trace;  // Prevent unused warning

    int max_id = -1;
    std::regex pattern(R"(fairyfly\.(\d+)\.con)");

    auto scan = [&](const fs::path& directory) {
        if (!fs::exists(directory)) return;
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();
            std::smatch match;

            if (std::regex_match(filename, match, pattern)) {
                int id = std::stoi(match[1].str());
                max_id = std::max(max_id, id);
            }
        }
    };
    try {
        scan(working_directory_);
        if (fs::absolute(working_directory_).lexically_normal() != fs::current_path()) {
            scan(fs::current_path());
        }
    } catch (const std::exception& e) {
        throw SystemError(fmt::format("Cannot scan connection files for a free ID: {}", e.what()));
    }

    if (max_id == std::numeric_limits<int>::max()) {
        throw SystemError("No connection ID is available: the maximum ID is already in use");
    }
    int next_id = max_id + 1;
    spdlog::debug("Next connection ID: {}", next_id);
    return next_id;
}

namespace {
std::string new_cache_generation() {
    std::random_device random;
    const std::uint64_t first = (static_cast<std::uint64_t>(random()) << 32) | random();
    const std::uint64_t second = (static_cast<std::uint64_t>(random()) << 32) | random();
    return fmt::format("{:016x}{:016x}", first, second);
}

bool same_cache_identity(const Connection& expected, const Connection& current) {
    if (current.id != expected.id ||
        current.session_id != expected.session_id ||
        current.connection_id != expected.connection_id ||
        current.server_session_key != expected.server_session_key) return false;
    if (!expected.cache_generation.empty()) {
        return current.cache_generation == expected.cache_generation;
    }
    // Legacy files lack a generation. Preserve them if any saved field changed.
    return current.to_json() == expected.to_json();
}

class CacheWriteLock {
public:
    explicit CacheWriteLock(const std::string& directory) {
#ifdef _WIN32
        // Fairyfly processes in one Windows session must serialize the ID scan
        // and replacement together. The mutex vanishes when the last handle
        // closes, including after a process terminates unexpectedly.
        const auto path = fs::absolute(directory).lexically_normal().wstring();
        std::uint64_t hash = 14695981039346656037ULL;
        for (wchar_t character : path) {
            hash ^= static_cast<std::uint16_t>(std::towlower(character));
            hash *= 1099511628211ULL;
        }
        const std::wstring name = L"Local\\fairyfly-cache-" + std::to_wstring(hash);
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle_) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Creating SAP connection cache mutex");
        }
        const DWORD wait = WaitForSingleObject(handle_, 30000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
            CloseHandle(handle_);
            handle_ = nullptr;
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "Waiting for SAP connection cache lock");
        }
#else
        (void)directory;
        lock_ = std::unique_lock<std::mutex>(local_mutex());
#endif
    }

    ~CacheWriteLock() {
#ifdef _WIN32
        ReleaseMutex(handle_);
        CloseHandle(handle_);
#endif
    }
    CacheWriteLock(const CacheWriteLock&) = delete;
    CacheWriteLock& operator=(const CacheWriteLock&) = delete;

private:
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#else
    static std::mutex& local_mutex() {
        static std::mutex mutex;
        return mutex;
    }
    std::unique_lock<std::mutex> lock_;
#endif
};

void save_connection_file(const fs::path& target, const Connection& conn) {
    static std::atomic_uint64_t sequence{0};
    fs::path temporary = target;
    temporary += fmt::format(".{}.{}.tmp",
                             std::chrono::steady_clock::now().time_since_epoch().count(),
                             sequence.fetch_add(1));

    try {
        std::ofstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(temporary, std::ios::binary | std::ios::trunc);
        file << std::setw(2) << conn.to_json() << '\n';
        file.close();

#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "Replacing SAP connection file");
        }
#else
        fs::rename(temporary, target);
#endif
    } catch (...) {
        std::error_code cleanup_error;
        fs::remove(temporary, cleanup_error);
        throw;
    }
}

void migrate_legacy_id_collisions(const std::string& cache_dir) {
    const fs::path primary_dir = fs::absolute(cache_dir).lexically_normal();
    const fs::path legacy_dir = fs::current_path();
    if (primary_dir == legacy_dir || !fs::is_directory(primary_dir)) return;

    CacheWriteLock lock(cache_dir);
    const std::regex pattern(R"(fairyfly\.(\d+)\.con)");
    std::set<int> primary_ids;
    std::vector<std::pair<int, fs::path>> legacy_files;
    int max_id = -1;
    auto scan = [&](const fs::path& directory, bool primary) {
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (!entry.is_regular_file()) continue;
            const std::string filename = entry.path().filename().string();
            std::smatch match;
            if (!std::regex_match(filename, match, pattern)) continue;
            const int id = std::stoi(match[1].str());
            max_id = std::max(max_id, id);
            if (primary) primary_ids.insert(id);
            else legacy_files.emplace_back(id, entry.path());
        }
    };
    scan(primary_dir, true);
    scan(legacy_dir, false);

    for (const auto& [id, old_path] : legacy_files) {
        if (!primary_ids.contains(id)) continue;
        try {
            std::ifstream input(old_path);
            json saved;
            input >> saved;
            input.close();
            Connection connection = Connection::from_json(saved);
            if (connection.id != id || max_id == std::numeric_limits<int>::max()) {
                throw std::runtime_error("Invalid or exhausted connection ID");
            }
            connection.id = ++max_id;
            connection.cache_generation = new_cache_generation();
            const fs::path new_path = legacy_dir / connection.get_file_path();
            save_connection_file(new_path, connection);
            std::error_code remove_error;
            if (!fs::remove(old_path, remove_error) || remove_error) {
                std::error_code cleanup_error;
                fs::remove(new_path, cleanup_error);
                throw std::system_error(remove_error ? remove_error :
                                        std::make_error_code(std::errc::no_such_file_or_directory),
                                        "Removing collided legacy connection file");
            }
            spdlog::info("Migrated legacy connection cache ID {} to {}", id, connection.id);
        } catch (const std::exception& e) {
            throw SystemError("Could not migrate collided legacy SAP connection file: " +
                              std::string(e.what()));
        }
    }
}

std::string get_env(const char* name) {
#ifdef _WIN32
    char buf[32767];
    DWORD len = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (len > 0 && len < sizeof(buf)) {
        return std::string(buf, len);
    }
    return "";
#else
    const char* val = std::getenv(name);
    return val ? std::string(val) : "";
#endif
}
}

std::string ConnectionManager::get_default_cache_directory() {
    std::error_code ec;

    // 1. FAIRYFLY_CACHE_DIR environment variable override
    std::string env_dir = get_env("FAIRYFLY_CACHE_DIR");
    if (!env_dir.empty()) {
        fs::path p(env_dir);
        fs::create_directories(p, ec);
        if (!ec && fs::exists(p)) {
            return p.string();
        }
    }

    // 2. User local app data on Windows, ~/.cache on POSIX
#ifdef _WIN32
    std::string local_app_data = get_env("LOCALAPPDATA");
    if (!local_app_data.empty()) {
        fs::path p = fs::path(local_app_data) / "fairyfly" / "sessions";
        fs::create_directories(p, ec);
        if (!ec && fs::exists(p)) {
            return p.string();
        }
    }
#else
    std::string home = get_env("HOME");
    if (!home.empty()) {
        fs::path p = fs::path(home) / ".cache" / "fairyfly" / "sessions";
        fs::create_directories(p, ec);
        if (!ec && fs::exists(p)) {
            return p.string();
        }
    }
#endif

    // 3. System temp directory fallback
    fs::path temp_p = fs::temp_directory_path() / "fairyfly" / "sessions";
    fs::create_directories(temp_p, ec);
    if (!ec && fs::exists(temp_p)) {
        return temp_p.string();
    }

    // 4. Fallback: .fairyfly/sessions in current directory
    fs::path p = fs::path(".fairyfly") / "sessions";
    fs::create_directories(p, ec);
    return p.string();
}

ConnectionManager::ConnectionManager()
    : working_directory_(get_default_cache_directory())
{
    migrate_legacy_id_collisions(working_directory_);
    spdlog::debug("ConnectionManager initialized with cache directory: {}", working_directory_);
}

ConnectionManager::ConnectionManager(const std::string& working_dir)
    : working_directory_(working_dir)
{
    migrate_legacy_id_collisions(working_directory_);
    spdlog::debug("ConnectionManager initialized with explicit directory: {}", working_directory_);
}

std::vector<Connection> ConnectionManager::list_connections() const {
    TraceGuard trace("ConnectionManager::list_connections");

    std::vector<Connection> connections;
    std::regex pattern(R"(fairyfly\.(\d+)\.con)");

    auto scan_dir = [&](const std::string& dir) {
        if (!fs::exists(dir)) return;
        try {
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;

                std::string filename = entry.path().filename().string();
                std::smatch match;

                if (std::regex_match(filename, match, pattern)) {
                    try {
                        std::ifstream file(entry.path());
                        json j;
                        file >> j;

                        Connection conn = Connection::from_json(j);
                        // Prevent duplicates if already discovered
                        bool already_present = false;
                        for (const auto& existing : connections) {
                            if (existing.id == conn.id) {
                                already_present = true;
                                break;
                            }
                        }
                        if (!already_present) {
                            connections.push_back(conn);
                            spdlog::debug("Loaded connection: {}", conn.get_file_path());
                        }
                    } catch (const std::exception& e) {
                        spdlog::warn("Failed to parse connection file {}: {}", filename, e.what());
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("Error listing connection files in {}: {}", dir, e.what());
        }
    };

    // Scan primary working/cache directory
    scan_dir(working_directory_);

    // If different from current path, scan current directory for legacy connection files
    std::string current_dir = fs::current_path().string();
    if (working_directory_ != current_dir) {
        scan_dir(current_dir);
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

    const fs::path filepath = existing_connection_path(working_directory_, id);
    if (!fs::exists(filepath)) {
        spdlog::debug("Connection file not found: {}", filepath.string());
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
        spdlog::error("Failed to load connection file {}: {}", filepath.string(), e.what());
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
    const std::string& window_title,
    const std::string& server_session_key)
{
    TraceGuard trace("ConnectionManager::create_or_update_connection");
    (void)trace;
    CacheWriteLock lock(working_directory_);
    spdlog::debug("create_or_update_connection: session_id={}", session_id);

    // Check if connection already exists for this session
    std::optional<Connection> existing;
    for (const auto& candidate : list_connections()) {
        if (candidate.session_id != session_id) continue;
        if (!server_session_key.empty() &&
            candidate.server_session_key == server_session_key) {
            existing = candidate;
            break;
        }
        if (server_session_key.empty() && candidate.server_session_key.empty() && !existing) {
            existing = candidate;
        }
    }

    Connection conn;
    std::string timestamp = get_current_timestamp();

    if (existing.has_value()) {
        // Update existing connection
        conn = existing.value();
        conn.cache_generation = new_cache_generation();
        conn.connection_description = description;
        conn.server_session_key = server_session_key;
        conn.connection_string = connection_string;
        conn.window_title = window_title;
        conn.last_validated = timestamp;

        spdlog::info("Updating existing connection {}", conn.id);
    } else {
        // Create new connection
        conn.id = get_next_connection_id();
        conn.cache_generation = new_cache_generation();
        conn.session_id = session_id;
        conn.server_session_key = server_session_key;
        conn.connection_id = connection_id;
        conn.connection_description = description;
        conn.connection_string = connection_string;
        conn.window_title = window_title;
        conn.created_at = timestamp;
        conn.last_validated = timestamp;

        spdlog::info("Creating new connection {}", conn.id);
    }

    // Write to file
    fs::path filepath = existing.has_value()
        ? existing_connection_path(working_directory_, conn.id)
        : fs::path(working_directory_) / conn.get_file_path();
    try {
        save_connection_file(filepath, conn);
        spdlog::info("Connection file written: {}", filepath.string());
    } catch (const std::exception& e) {
        spdlog::error("Failed to write connection file {}: {}", filepath.string(), e.what());
        throw SystemError("Could not save SAP connection file: " + std::string(e.what()));
    }

    return conn;
}

void ConnectionManager::touch_connection(const Connection& expected) {
    TraceGuard trace("ConnectionManager::touch_connection");
    (void)trace;
    CacheWriteLock lock(working_directory_);
    spdlog::debug("touch_connection: id={}", expected.id);

    auto conn = load_connection(expected.id);
    if (!conn || !same_cache_identity(expected, *conn)) {
        throw SystemError("SAP connection file changed during validation");
    }

    conn.value().last_validated = get_current_timestamp();
    if (conn.value().cache_generation.empty()) {
        conn.value().cache_generation = new_cache_generation();
    }

    fs::path filepath = existing_connection_path(working_directory_, conn.value().id);
    try {
        save_connection_file(filepath, conn.value());
        spdlog::debug("Updated last_validated for connection {}", expected.id);
    } catch (const std::exception& e) {
        spdlog::error("Failed to update connection file {}: {}", filepath.string(), e.what());
    }
}

Connection ConnectionManager::set_session_key(const Connection& expected,
                                              const std::string& server_session_key) {
    CacheWriteLock lock(working_directory_);
    auto conn = load_connection(expected.id);
    if (!conn || !same_cache_identity(expected, *conn)) {
        throw SystemError("SAP connection file changed during validation");
    }
    if (!conn->server_session_key.empty() &&
        conn->server_session_key != server_session_key) {
        throw SystemError("SAP connection already has a different server session key");
    }
    conn->server_session_key = server_session_key;
    conn->cache_generation = new_cache_generation();
    conn->last_validated = get_current_timestamp();
    const fs::path path = existing_connection_path(working_directory_, conn->id);
    try {
        save_connection_file(path, *conn);
    } catch (const std::exception& e) {
        throw SystemError("Could not update SAP connection key: " + std::string(e.what()));
    }
    return *conn;
}

bool ConnectionManager::delete_connection(int id) {
    TraceGuard trace("ConnectionManager::delete_connection");
    (void)trace;
    CacheWriteLock lock(working_directory_);
    spdlog::debug("delete_connection: id={}", id);

    const fs::path filepath = existing_connection_path(working_directory_, id);

    if (!fs::exists(filepath)) {
        spdlog::debug("Connection file does not exist: {}", filepath.string());
        return false;
    }

    try {
        fs::remove(filepath);
        spdlog::info("Deleted connection file: {}", filepath.string());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to delete connection file {}: {}", filepath.string(), e.what());
        return false;
    }
}

bool ConnectionManager::delete_connection_if_unchanged(const Connection& expected) {
    CacheWriteLock lock(working_directory_);
    const auto current = load_connection(expected.id);
    if (!current || !same_cache_identity(expected, *current)) return false;

    const fs::path path = existing_connection_path(working_directory_, expected.id);
    try {
        return fs::remove(path);
    } catch (const std::exception& e) {
        spdlog::error("Failed to conditionally delete connection file {}: {}", path.string(), e.what());
        return false;
    }
}

ConnectionPartition partition_connections(
    const std::vector<Connection>& connections,
    const std::function<bool(const Connection&)>& is_live)
{
    ConnectionPartition parts;
    for (const auto& conn : connections) {
        (is_live(conn) ? parts.live : parts.stale).push_back(conn);
    }
    return parts;
}

int ConnectionManager::prune_other_entries_for_path(const Connection& keep, const std::string& live_key) {
    TraceGuard trace("ConnectionManager::prune_other_entries_for_path");
    (void)trace;
    int pruned = 0;
    for (const auto& conn : list_connections()) {
        if (conn.id == keep.id || conn.session_id != keep.session_id) continue;
        if (!live_key.empty() && conn.server_session_key == live_key) continue;
        if (delete_connection_if_unchanged(conn)) {
            spdlog::info("Pruned stale connection {} for session {}", conn.id, conn.session_id);
            pruned++;
        }
    }
    return pruned;
}

int ConnectionManager::cleanup_invalid_connections(
    std::function<bool(const Connection&)> validate_func)
{
    TraceGuard trace("ConnectionManager::cleanup_invalid_connections");

    auto connections = list_connections();
    int deleted_count = 0;

    for (const auto& conn : connections) {
        if (!validate_func(conn)) {
            spdlog::info("Connection {} has invalid session {}, deleting", conn.id, conn.session_id);
            if (delete_connection_if_unchanged(conn)) {
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
