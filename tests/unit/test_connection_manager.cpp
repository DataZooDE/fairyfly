#include <catch2/catch_test_macros.hpp>
#include "include/connection_manager.h"
#include "include/exceptions.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <iterator>
#include <atomic>
#include <thread>
#include <set>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace fairyfly::cli;

TEST_CASE("Uncertain SAP observation preserves the saved connection during cleanup", "[connection_manager]") {
    const fs::path temp_dir = fs::temp_directory_path() /
        ("fairyfly_test_uncertain_cleanup_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(temp_dir));
    ConnectionManager manager(temp_dir.string());
    const auto saved = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "key:0");
    REQUIRE_THROWS_AS(manager.cleanup_invalid_connections([](const Connection&) -> bool {
        throw std::runtime_error("SAP GUI collection count unavailable");
    }), std::runtime_error);
    REQUIRE(manager.load_connection(saved.id).has_value());
    REQUIRE(manager.delete_connection(saved.id));
    REQUIRE(fs::remove(temp_dir));
}

TEST_CASE("ConnectionManager isolated cache directory", "[connection_manager]") {
    SECTION("get_default_cache_directory returns an existing valid directory") {
        std::string cache_dir = ConnectionManager::get_default_cache_directory();
        INFO("cache_dir is: " << cache_dir);
        REQUIRE(!cache_dir.empty());
        REQUIRE(fs::exists(cache_dir));
        REQUIRE(fs::is_directory(cache_dir));
    }

    SECTION("ConnectionManager in explicit directory saves and loads connections") {
        fs::path temp_dir = fs::temp_directory_path() /
            ("fairyfly_test_conns_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(fs::create_directory(temp_dir));

        ConnectionManager mgr(temp_dir.string());
        REQUIRE(mgr.get_working_directory() == temp_dir.string());

        REQUIRE(mgr.list_connections().empty());

        Connection test_conn = mgr.create_or_update_connection(
            "/app/con[0]/ses[0]",
            "/app/con[0]",
            "Test System",
            "conn_string_val",
            "SAP Easy Access"
        );
        REQUIRE(test_conn.id >= 0);

        auto loaded = mgr.load_connection(test_conn.id);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->connection_description == "Test System");
        REQUIRE(loaded->session_id == "/app/con[0]/ses[0]");

        Connection updated = mgr.create_or_update_connection(
            "/app/con[0]/ses[0]", "/app/con[0]", "Updated System",
            "conn_string_val", "SAP Easy Access");
        REQUIRE(updated.id == test_conn.id);
        loaded = mgr.load_connection(test_conn.id);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->connection_description == "Updated System");
        mgr.touch_connection(updated);
        REQUIRE(mgr.load_connection(test_conn.id).has_value());
        REQUIRE(std::distance(fs::directory_iterator(temp_dir), fs::directory_iterator{}) == 1);

        // Clean up
        REQUIRE(mgr.delete_connection(test_conn.id));
        REQUIRE(fs::remove(temp_dir));
    }

    SECTION("connection creation fails when its cache path is not a directory") {
        fs::path invalid_dir = fs::temp_directory_path() /
            ("fairyfly_test_invalid_cache_path_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        {
            std::ofstream marker(invalid_dir, std::ios::trunc);
            REQUIRE(marker.good());
        }

        ConnectionManager mgr(invalid_dir.string());
        REQUIRE_THROWS_AS(
            mgr.create_or_update_connection("/app/con[0]/ses[0]", "/app/con[0]",
                                            "Test System", "conn_string_val", "SAP Easy Access"),
            fairyfly::SystemError);

        fs::remove(invalid_dir);
    }

    SECTION("failed replacement preserves the previous target and removes temporary files") {
        fs::path temp_dir = fs::temp_directory_path() /
            ("fairyfly_test_replace_failure_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(fs::create_directory(temp_dir));
        fs::path blocked_target = temp_dir / "fairyfly.0.con";
        REQUIRE(fs::create_directory(blocked_target));

        ConnectionManager mgr(temp_dir.string());
        REQUIRE_THROWS_AS(
            mgr.create_or_update_connection("/app/con[0]/ses[0]", "/app/con[0]",
                                            "Test System", "conn_string_val", "SAP Easy Access"),
            fairyfly::SystemError);
        REQUIRE(fs::is_directory(blocked_target));
        REQUIRE(std::distance(fs::directory_iterator(temp_dir), fs::directory_iterator{}) == 1);

        REQUIRE(fs::remove(blocked_target));
        REQUIRE(fs::remove(temp_dir));
    }
}

TEST_CASE("ConnectionManager rejects exhausted connection ID space", "[connection_manager]") {
    const fs::path temp_dir = fs::temp_directory_path() /
        ("fairyfly_test_exhausted_ids_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(temp_dir));
    const fs::path max_id_file = temp_dir /
        ("fairyfly." + std::to_string(std::numeric_limits<int>::max()) + ".con");
    {
        std::ofstream file(max_id_file);
        REQUIRE(file.good());
    }

    ConnectionManager mgr(temp_dir.string());
    REQUIRE_THROWS_AS(
        mgr.create_or_update_connection("/app/con[0]/ses[0]", "/app/con[0]",
                                        "Test System", "route", "SAP"),
        fairyfly::SystemError);

    REQUIRE(fs::remove(max_id_file));
    REQUIRE(fs::remove(temp_dir));
}

TEST_CASE("ConnectionManager distinguishes reused SAP session paths", "[connection_manager]") {
    fs::path temp_dir = fs::temp_directory_path() /
        ("fairyfly_test_session_key_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(temp_dir));
    ConnectionManager mgr(temp_dir.string());
    const std::string path = "/app/con[0]/ses[0]";
    auto legacy = mgr.create_or_update_connection(path, "/app/con[0]", "System",
                                                   "route", "SAP");
    auto first = mgr.create_or_update_connection(path, "/app/con[0]", "System",
                                                  "route", "SAP", "old:0");
    REQUIRE(first.id != legacy.id);
    REQUIRE(mgr.load_connection(legacy.id)->server_session_key.empty());
    REQUIRE(mgr.load_connection(first.id)->server_session_key == "old:0");
    auto second = mgr.create_or_update_connection(path, "/app/con[0]", "System",
                                                   "route", "SAP", "new:0");
    REQUIRE(second.id != first.id);
    REQUIRE(mgr.load_connection(first.id)->server_session_key == "old:0");
    REQUIRE(mgr.load_connection(second.id)->server_session_key == "new:0");
    REQUIRE(mgr.set_session_key(legacy, "legacy:0").server_session_key == "legacy:0");
    REQUIRE(Connection::from_json(first.to_json()).server_session_key == "old:0");
    REQUIRE(Connection::from_json(first.to_json()).cache_generation == first.cache_generation);
    auto legacy_json = first.to_json();
    legacy_json.erase("server_session_key");
    legacy_json.erase("cache_generation");
    REQUIRE(Connection::from_json(legacy_json).server_session_key.empty());
    REQUIRE(Connection::from_json(legacy_json).cache_generation.empty());
    auto updated = mgr.create_or_update_connection(path, "/app/con[0]", "System",
                                                    "route", "SAP", "new:0");
    REQUIRE(updated.id == second.id);
    REQUIRE(mgr.cleanup_invalid_connections([](const Connection& candidate) {
        return candidate.server_session_key == "new:0";
    }) == 2);
    REQUIRE_FALSE(mgr.load_connection(first.id).has_value());
    REQUIRE_FALSE(mgr.load_connection(legacy.id).has_value());
    REQUIRE(mgr.load_connection(second.id).has_value());
    REQUIRE(mgr.delete_connection(second.id));
    REQUIRE(fs::remove(temp_dir));
}

TEST_CASE("Legacy connection files remain mutable and do not collide with new IDs", "[connection_manager]") {
    const fs::path original_directory = fs::current_path();
    const fs::path temp_dir = fs::temp_directory_path() /
        ("fairyfly_test_legacy_cache_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(temp_dir));
    const fs::path cache_dir = temp_dir / "cache";
    REQUIRE(fs::create_directory(cache_dir));
    struct RestoreDirectory {
        fs::path path;
        ~RestoreDirectory() { fs::current_path(path); }
    } restore{original_directory};
    fs::current_path(temp_dir);

    Connection legacy{};
    legacy.id = 3;
    legacy.session_id = "/app/con[0]/ses[0]";
    legacy.connection_id = "/app/con[0]";
    legacy.connection_description = "Legacy";
    {
        std::ofstream file(temp_dir / legacy.get_file_path());
        file << legacy.to_json();
        REQUIRE(file.good());
    }

    ConnectionManager manager(cache_dir.string());
    const auto created = manager.create_or_update_connection(
        "/app/con[1]/ses[0]", "/app/con[1]", "New", "route", "SAP", "new:0");
    REQUIRE(created.id == 4);
    REQUIRE(fs::exists(cache_dir / created.get_file_path()));

    manager.touch_connection(legacy);
    auto touched = manager.load_connection(legacy.id);
    REQUIRE(touched.has_value());
    REQUIRE_FALSE(touched->cache_generation.empty());
    REQUIRE_FALSE(fs::exists(cache_dir / legacy.get_file_path()));

    const auto keyed = manager.set_session_key(*touched, "legacy:0");
    REQUIRE(keyed.server_session_key == "legacy:0");
    const auto updated = manager.create_or_update_connection(
        legacy.session_id, legacy.connection_id, "Updated", "route", "SAP", "legacy:0");
    REQUIRE(updated.id == legacy.id);
    REQUIRE(manager.load_connection(legacy.id)->connection_description == "Updated");
    REQUIRE_FALSE(fs::exists(cache_dir / legacy.get_file_path()));

    REQUIRE(manager.cleanup_invalid_connections([](const Connection& candidate) {
        return candidate.server_session_key == "new:0";
    }) == 1);
    REQUIRE_FALSE(fs::exists(temp_dir / legacy.get_file_path()));
    REQUIRE(manager.load_connection(created.id).has_value());
    legacy.id = 5;
    {
        std::ofstream file(temp_dir / legacy.get_file_path());
        file << legacy.to_json();
        REQUIRE(file.good());
    }
    REQUIRE(manager.delete_connection(legacy.id));
    REQUIRE_FALSE(fs::exists(temp_dir / legacy.get_file_path()));
    REQUIRE(manager.delete_connection(created.id));
    fs::current_path(original_directory);
    REQUIRE(fs::remove(cache_dir));
    REQUIRE(fs::remove(temp_dir));
}

TEST_CASE("Pre-existing primary and legacy cache ID collision preserves both sessions", "[connection_manager]") {
    const fs::path original_directory = fs::current_path();
    const fs::path temp_dir = fs::temp_directory_path() /
        ("fairyfly_test_duplicate_cache_id_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(temp_dir));
    const fs::path cache_dir = temp_dir / "cache";
    REQUIRE(fs::create_directory(cache_dir));
    struct RestoreDirectory {
        fs::path path;
        ~RestoreDirectory() { fs::current_path(path); }
    } restore{original_directory};
    fs::current_path(temp_dir);

    Connection primary{};
    primary.id = 3;
    primary.session_id = "/app/con[0]/ses[0]";
    primary.connection_id = "/app/con[0]";
    primary.connection_description = "Primary";
    Connection legacy{};
    legacy.id = 3;
    legacy.session_id = "/app/con[1]/ses[0]";
    legacy.connection_id = "/app/con[1]";
    legacy.connection_description = "Legacy";
    {
        std::ofstream file(cache_dir / primary.get_file_path());
        file << primary.to_json();
        REQUIRE(file.good());
    }
    {
        std::ofstream file(temp_dir / legacy.get_file_path());
        file << legacy.to_json();
        REQUIRE(file.good());
    }

    ConnectionManager manager(cache_dir.string());
    const auto saved = manager.list_connections();
    REQUIRE(saved.size() == 2);
    REQUIRE(saved[0].id != saved[1].id);
    REQUIRE(manager.load_connection(3)->session_id == primary.session_id);
    const auto migrated = manager.find_by_session_id(legacy.session_id);
    REQUIRE(migrated.has_value());
    REQUIRE(migrated->id != 3);
    REQUIRE(manager.load_connection(migrated->id)->session_id == legacy.session_id);
    REQUIRE_FALSE(fs::exists(temp_dir / legacy.get_file_path()));

    REQUIRE(manager.delete_connection(3));
    REQUIRE(manager.delete_connection(migrated->id));
    fs::current_path(original_directory);
    REQUIRE(fs::remove(cache_dir));
    REQUIRE(fs::remove(temp_dir));
}

TEST_CASE("Stale cleanup does not delete a replacement connection file", "[connection_manager]") {
    fs::path temp_dir = fs::temp_directory_path() /
        ("fairyfly_test_stale_cleanup_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(temp_dir));
    ConnectionManager manager(temp_dir.string());
    auto first = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "session:0");
    Connection replacement;
    bool replaced = false;
    const int removed = manager.cleanup_invalid_connections([&](const Connection& snapshot) {
        REQUIRE(snapshot.cache_generation == first.cache_generation);
        REQUIRE(manager.delete_connection(snapshot.id));
        replacement = manager.create_or_update_connection(
            "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "session:0");
        replaced = true;
        return false;
    });
    REQUIRE(replaced);
    REQUIRE(first.id == replacement.id);
    REQUIRE(first.cache_generation != replacement.cache_generation);
    REQUIRE(removed == 0);
    REQUIRE_THROWS_AS(manager.touch_connection(first), fairyfly::SystemError);
    REQUIRE_THROWS_AS(manager.set_session_key(first, "other:0"), fairyfly::SystemError);
    REQUIRE(manager.load_connection(replacement.id)->cache_generation == replacement.cache_generation);
    REQUIRE(manager.delete_connection(replacement.id));
    REQUIRE(fs::remove(temp_dir));
}

TEST_CASE("Concurrent connection creators receive distinct cache IDs", "[connection_manager]") {
    fs::path temp_dir = fs::temp_directory_path() /
        ("fairyfly_test_concurrent_cache_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(temp_dir));

    constexpr int count = 12;
    std::atomic<bool> start{false};
    std::vector<Connection> created(count);
    std::vector<std::exception_ptr> failures(count);
    std::vector<std::thread> workers;
    workers.reserve(count);
    for (int i = 0; i < count; ++i) {
        workers.emplace_back([&, i] {
            while (!start.load()) std::this_thread::yield();
            try {
                ConnectionManager manager(temp_dir.string());
                created[i] = manager.create_or_update_connection(
                    "/app/con[" + std::to_string(i) + "]/ses[0]",
                    "/app/con[" + std::to_string(i) + "]",
                    "Test System", "route", "SAP", "session:" + std::to_string(i));
            } catch (...) {
                failures[i] = std::current_exception();
            }
        });
    }
    start.store(true);
    for (auto& worker : workers) worker.join();
    for (const auto& failure : failures) {
        if (failure) std::rethrow_exception(failure);
    }

    std::set<int> ids;
    ConnectionManager manager(temp_dir.string());
    for (int i = 0; i < count; ++i) {
        REQUIRE(ids.insert(created[i].id).second);
        const auto loaded = manager.load_connection(created[i].id);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->server_session_key == "session:" + std::to_string(i));
    }
    REQUIRE(manager.list_connections().size() == count);
    for (int id : ids) REQUIRE(manager.delete_connection(id));
    REQUIRE(fs::remove(temp_dir));
}

#ifdef _WIN32
TEST_CASE("Concurrent processes receive distinct cache IDs", "[connection_manager]") {
    wchar_t executable_path[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(nullptr, executable_path, MAX_PATH) > 0);
    const fs::path worker = fs::path(executable_path).parent_path() / "connection_cache_worker.exe";
    REQUIRE(fs::exists(worker));

    fs::path temp_dir = fs::temp_directory_path() /
        ("fairyfly_test_process_cache_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(temp_dir));
    constexpr int count = 12;
    std::vector<PROCESS_INFORMATION> processes;
    for (int i = 0; i < count; ++i) {
        const std::wstring command = L"\"" + worker.wstring() + L"\" \"" +
                                     temp_dir.wstring() + L"\" " + std::to_wstring(i);
        std::wstring mutable_command = command;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(worker.c_str(), mutable_command.data(), nullptr,
                                            nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                            nullptr, &startup, &process);
        REQUIRE(started != FALSE);
        processes.push_back(process);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool all_ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
        all_ready = true;
        for (int i = 0; i < count; ++i) {
            if (!fs::exists(temp_dir / ("ready." + std::to_string(i)))) {
                all_ready = false;
                break;
            }
        }
        if (all_ready) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::ofstream start(temp_dir / "start.signal");
        REQUIRE(start.good());
    }
    REQUIRE(all_ready);
    for (auto& process : processes) {
        const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
        DWORD code = 0;
        REQUIRE(wait == WAIT_OBJECT_0);
        REQUIRE(GetExitCodeProcess(process.hProcess, &code) != FALSE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        REQUIRE(code == 0);
    }

    ConnectionManager manager(temp_dir.string());
    const auto connections = manager.list_connections();
    REQUIRE(connections.size() == count);
    std::set<int> ids;
    std::set<std::string> keys;
    for (const auto& connection : connections) {
        REQUIRE(ids.insert(connection.id).second);
        REQUIRE(keys.insert(connection.server_session_key).second);
        REQUIRE_FALSE(connection.cache_generation.empty());
        REQUIRE(manager.delete_connection(connection.id));
    }
    for (int i = 0; i < count; ++i) {
        REQUIRE(fs::remove(temp_dir / ("ready." + std::to_string(i))));
    }
    REQUIRE(fs::remove(temp_dir / "start.signal"));
    REQUIRE(fs::remove(temp_dir));
}
#endif

TEST_CASE("Stale connection with distinct session key is not reused for same path", "[connection_manager]") {
    fs::path temp_dir = fs::temp_directory_path() /
        ("fairyfly_test_stale_key_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(temp_dir));

    ConnectionManager manager(temp_dir.string());
    // Create initial connection with old session key
    const auto old_conn = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "session:old");
    REQUIRE(old_conn.id >= 0);
    REQUIRE(old_conn.server_session_key == "session:old");

    // A new session attaches to the same GUI path /app/con[0]/ses[0] with a new session key
    const auto new_conn = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "session:new");
    REQUIRE(new_conn.id >= 0);
    REQUIRE(new_conn.id != old_conn.id); // Must receive a distinct connection ID
    REQUIRE(new_conn.server_session_key == "session:new");

    // Both connection files exist independently
    const auto loaded_old = manager.load_connection(old_conn.id);
    const auto loaded_new = manager.load_connection(new_conn.id);
    REQUIRE(loaded_old.has_value());
    REQUIRE(loaded_new.has_value());
    REQUIRE(loaded_old->server_session_key == "session:old");
    REQUIRE(loaded_new->server_session_key == "session:new");

    // Cleanup invalid connections simulates session validation: only session:new is active
    const int removed = manager.cleanup_invalid_connections([&](const Connection& c) {
        return c.server_session_key == "session:new";
    });
    REQUIRE(removed == 1);
    REQUIRE_FALSE(manager.load_connection(old_conn.id).has_value());
    REQUIRE(manager.load_connection(new_conn.id).has_value());

    REQUIRE(manager.delete_connection(new_conn.id));
    REQUIRE(fs::remove(temp_dir));
}


namespace {
fs::path make_prune_test_dir(const char* tag) {
    fs::path dir = fs::temp_directory_path() /
        (std::string("fairyfly_test_") + tag + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(dir));
    return dir;
}
}  // namespace

TEST_CASE("Prune removes other same-path entries and keeps other paths", "[connection_manager][prune]") {
    const fs::path dir = make_prune_test_dir("prune_same_path");
    ConnectionManager manager(dir.string());
    const auto other_key = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "old:0");
    const auto no_key = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "");
    const auto keep = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "live:0");
    const auto elsewhere = manager.create_or_update_connection(
        "/app/con[0]/ses[1]", "/app/con[0]", "System", "route", "SAP", "live:1");
    REQUIRE(other_key.id != keep.id);
    REQUIRE(no_key.id != keep.id);

    REQUIRE(manager.prune_other_entries_for_path(keep) == 2);
    REQUIRE_FALSE(manager.load_connection(other_key.id).has_value());
    REQUIRE_FALSE(manager.load_connection(no_key.id).has_value());
    REQUIRE(manager.load_connection(keep.id).has_value());
    REQUIRE(manager.load_connection(elsewhere.id).has_value());
    REQUIRE(manager.prune_other_entries_for_path(keep) == 0);

    fs::remove_all(dir);
}

TEST_CASE("Prune never deletes an entry holding the current live key", "[connection_manager][prune]") {
    const fs::path dir = make_prune_test_dir("prune_live_key_race");
    ConnectionManager manager(dir.string());
    const auto stale = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "old:0");
    const auto keep = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "live:0");
    // Another process cached a NEW live session on the reused path after the attach validated `keep`.
    const auto raced = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "live:1");
    REQUIRE(raced.id != keep.id);

    REQUIRE(manager.prune_other_entries_for_path(keep, "live:1") == 1);
    REQUIRE_FALSE(manager.load_connection(stale.id).has_value());
    REQUIRE(manager.load_connection(raced.id).has_value());
    REQUIRE(manager.load_connection(keep.id).has_value());

    fs::remove_all(dir);
}

TEST_CASE("Conditional delete skips an entry that was replaced concurrently", "[connection_manager][prune]") {
    const fs::path dir = make_prune_test_dir("prune_replaced");
    ConnectionManager manager(dir.string());
    const auto stale = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "old:0");
    const auto keep = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "live:0");

    // Refreshing the entry gives it a new cache generation, so the snapshot
    // taken before the refresh (what prune deletes from) no longer matches.
    const auto refreshed = manager.create_or_update_connection(
        "/app/con[0]/ses[0]", "/app/con[0]", "System", "route", "SAP", "old:0");
    REQUIRE(refreshed.id == stale.id);
    REQUIRE_FALSE(manager.delete_connection_if_unchanged(stale));
    REQUIRE(manager.load_connection(stale.id).has_value());
    REQUIRE(manager.load_connection(keep.id).has_value());

    fs::remove_all(dir);
}

TEST_CASE("partition_connections splits live and stale entries", "[connection_manager][prune]") {
    std::vector<Connection> all(3);
    for (int i = 0; i < 3; ++i) {
        all[i].id = i + 1;
        all[i].session_id = "/app/con[0]/ses[0]";
    }
    const auto parts = partition_connections(all, [](const Connection& c) { return c.id == 2; });
    REQUIRE(parts.live.size() == 1);
    REQUIRE(parts.live[0].id == 2);
    REQUIRE(parts.stale.size() == 2);
    REQUIRE(parts.stale[0].id == 1);
    REQUIRE(parts.stale[1].id == 3);
}
