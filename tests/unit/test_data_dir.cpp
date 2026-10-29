// fairyfly's data directory: %USERPROFILE%\.fairyfly (FAIRYFLY_DATA_DIR overrides it), and the one-time move from
// %LOCALAPPDATA%\fairyfly (config/data_dir.h). A Microsoft Store fairyfly on Windows 10 cannot keep its data under
// AppData: new folders there are redirected into the package's private store (verified live 2026-10-29).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "include/audit_log.h"
#include "include/config/data_dir.h"
#include "include/config/environment.h"
#include "include/text/utf8.h"
#include "support/temp_dir.h"

using namespace fairyfly;
namespace fs = std::filesystem;

namespace {

config::EnvLookup env_of(std::map<std::string, std::string> vars) {
    return [vars](const char* name) {
        auto i = vars.find(name);
        return i == vars.end() ? std::string() : i->second;
    };
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

void append(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::app);
    out << text;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Every regular file below `root` (relative, generic form) with its contents.
std::map<std::string, std::string> tree(const fs::path& root) {
    std::map<std::string, std::string> out;
    if (!fs::exists(root)) return out;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file()) out[fs::relative(e.path(), root).generic_string()] = read(e.path());
    return out;
}

std::size_t count_of(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size())) ++n;
    return n;
}

/// What an MSI fairyfly up to 2026.10.28 leaves in %LOCALAPPDATA%\fairyfly.
void legacy_layout(const fs::path& dir) {
    write(dir / "audit" / "2026-09.jsonl", "{\"ts\":\"2026-09-30T10:00:00.000Z\",\"command\":\"session list\"}\n");
    write(dir / "audit" / "2026-10.jsonl",
          "{\"ts\":\"2026-10-01T10:00:00.000Z\",\"command\":\"screen read\"}\n"
          "{\"ts\":\"2026-10-02T10:00:00.000Z\",\"command\":\"element click\"}\n");
    write(dir / "logs" / "link.log", "[2026-10-02] link started\n");
    write(dir / "logs" / "mcp.log", "[2026-10-02] mcp started\n");
    write(dir / "mcp.yaml", "server:\n  port: 8383\n");
    write(dir / "link" / "state.json", "{\"device_id\":\"dev_1\",\"key\":\"dpapi-blob\"}");
    write(dir / "link" / "state.json.status.json", "{\"state\":\"connected\"}");
    write(dir / "sessions" / "Bigfox.con", "{\"id\":\"Bigfox\"}");
    write(dir / "mcp-setup.json", "{\"host\":\"sap.example\"}");
}

/// A lock that is never granted (another fairyfly holds it for longer than the timeout).
class BusyLock : public config::MigrationLock {
public:
    bool acquire(std::chrono::milliseconds) override { return false; }
    void release() override {}
};

/// A lock that is always granted and counts how often.
class FreeLock : public config::MigrationLock {
public:
    int acquired = 0;
    bool acquire(std::chrono::milliseconds) override { ++acquired; return true; }
    void release() override {}
};

}  // namespace

// ---- where the data directory is ---------------------------------------------------------------------------------

TEST_CASE("data dir: %USERPROFILE%\\.fairyfly, on every Windows version and for MSI and MSIX alike", "[config][datadir]") {
    const config::Environment env(env_of({{"USERPROFILE", R"(C:\Users\ada)"}, {"LOCALAPPDATA", R"(C:\Users\ada\AppData\Local)"}}));
    CHECK(env.data_dir() == fs::path(R"(C:\Users\ada)") / ".fairyfly");
    CHECK_FALSE(env.data_dir_overridden());
    CHECK(env.legacy_data_dir() == fs::path(R"(C:\Users\ada\AppData\Local)") / "fairyfly");
}

TEST_CASE("data dir: FAIRYFLY_DATA_DIR overrides it", "[config][datadir]") {
    const config::Environment env(env_of({{"FAIRYFLY_DATA_DIR", R"(D:\ffy data)"}, {"USERPROFILE", R"(C:\Users\ada)"}}));
    CHECK(env.data_dir() == fs::path(R"(D:\ffy data)"));
    CHECK(env.data_dir_overridden());
    CHECK(config::env::kDataDir == std::string("FAIRYFLY_DATA_DIR"));
}

TEST_CASE("data dir: falls back to HOME, then to the temp directory", "[config][datadir]") {
    CHECK(config::Environment(env_of({{"HOME", "/home/ada"}})).data_dir() == fs::path("/home/ada") / ".fairyfly");
    CHECK(config::Environment(env_of({})).data_dir() == fs::temp_directory_path() / "fairyfly");
    // no LOCALAPPDATA: nothing to migrate from (never the temp directory)
    CHECK(config::Environment(env_of({})).legacy_data_dir().empty());
}

TEST_CASE("data dir: a non-ASCII user profile stays intact", "[config][datadir]") {
    const config::Environment env(env_of({{"USERPROFILE", "C:\\Users\\J\xC3\xB6rg"}}));
    CHECK(env.data_dir() == fs::path(L"C:\\Users\\J\u00F6rg") / ".fairyfly");
}

TEST_CASE("data dir: the per-user files all live in it", "[config][datadir]") {
    const auto env = env_of({{"USERPROFILE", R"(C:\Users\ada)"}, {"LOCALAPPDATA", R"(C:\Users\ada\AppData\Local)"}});
    const fs::path root = fs::path(R"(C:\Users\ada)") / ".fairyfly";
    const auto audit = audit::resolve_config(env, false, false, std::chrono::system_clock::now());
    CHECK(audit.file.parent_path() == root / "audit");
}

TEST_CASE("data dir: the old folders are LOCALAPPDATA's and, when packaged, the package's private copy", "[config][datadir]") {
    const config::Environment env(env_of({{"USERPROFILE", R"(C:\Users\ada)"}, {"LOCALAPPDATA", R"(C:\L)"}}));
    const auto unpackaged = config::legacy_data_dirs(env, std::nullopt);
    REQUIRE(unpackaged.size() == 1);
    CHECK(unpackaged[0] == fs::path(R"(C:\L)") / "fairyfly");
    const fs::path cache = fs::path(R"(C:\L\Packages\DataZoo.fairyfly_73q587949tsjt\LocalCache\Local)");
    const auto packaged = config::legacy_data_dirs(env, cache);
    REQUIRE(packaged.size() == 2);
    CHECK(packaged[0] == fs::path(R"(C:\L)") / "fairyfly");
    CHECK(packaged[1] == cache / "fairyfly");
    CHECK(config::legacy_data_dirs(config::Environment(env_of({})), std::nullopt).empty());
}

TEST_CASE("data dir: an unpackaged process has no package cache", "[config][datadir]") {
    // the unit tests run unpackaged on every machine
    CHECK_FALSE(config::package_local_cache_dir(config::Environment(env_of({{"LOCALAPPDATA", R"(C:\L)"}}))).has_value());
}

// ---- migration -----------------------------------------------------------------------------------------------------

TEST_CASE("migration: a fresh install creates the data directory with its marker", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-fresh");
    const fs::path target = tmp / "profile" / ".fairyfly";
    FreeLock lock;
    const auto r = config::migrate_data_dir(target, {tmp / "Local" / "fairyfly"}, lock);
    CHECK(r.outcome == config::MigrationOutcome::Fresh);
    CHECK(fs::is_directory(target));
    CHECK(fs::exists(target / config::kDataDirMarker));
    CHECK_FALSE(fs::exists(tmp / "Local" / "fairyfly"));  // nothing created where the old data would be
    // the next start: nothing to do, not even the lock
    FreeLock again;
    CHECK(config::migrate_data_dir(target, {tmp / "Local" / "fairyfly"}, again).outcome == config::MigrationOutcome::Current);
    CHECK(again.acquired == 0);
}

TEST_CASE("migration: an MSI's %LOCALAPPDATA%\\fairyfly is copied completely, the old folder stays", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-msi");
    const fs::path old = tmp / "Local" / "fairyfly";
    const fs::path target = tmp / "profile" / ".fairyfly";
    legacy_layout(old);
    const auto before = tree(old);
    FreeLock lock;
    const auto r = config::migrate_data_dir(target, {old}, lock);
    REQUIRE(r.outcome == config::MigrationOutcome::Migrated);
    REQUIRE(r.migrated_from.size() == 1);
    CHECK(r.migrated_from[0] == old);
    CHECK(r.conflicts.empty());

    auto after = tree(target);
    CHECK(after.erase(config::kDataDirMarker) == 1);
    CHECK(after == before);                                           // every file, byte for byte
    CHECK(read(target / config::kDataDirMarker).find("migrated_from") != std::string::npos);

    auto old_after = tree(old);
    const std::string note = old_after[config::kMovedNote];
    CHECK(note.find(".fairyfly") != std::string::npos);               // the note points to the new location
    old_after.erase(config::kMovedNote);
    CHECK(old_after == before);                                       // the old data is never deleted or changed
    CHECK_FALSE(fs::exists(fs::path(target.string() + config::kStagingSuffix)));

    // a second start: current; the old folder with its note is not copied again
    FreeLock again;
    CHECK(config::migrate_data_dir(target, {old}, again).outcome == config::MigrationOutcome::Current);
}

TEST_CASE("migration: an old folder that carries the moved note is not migrated again", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-noted");
    const fs::path old = tmp / "Local" / "fairyfly";
    legacy_layout(old);
    write(old / config::kMovedNote, "moved");
    const fs::path target = tmp / "profile" / ".fairyfly";  // the person deleted the new folder on purpose
    FreeLock lock;
    CHECK(config::migrate_data_dir(target, {old}, lock).outcome == config::MigrationOutcome::Fresh);
    CHECK_FALSE(fs::exists(target / "audit"));
}

TEST_CASE("migration: under MSIX on Windows 10 the package's private copy is migrated too", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-msix");
    const fs::path real = tmp / "Local" / "fairyfly";
    const fs::path package = tmp / "Local" / "Packages" / "DataZoo.fairyfly_73q587949tsjt" / "LocalCache" / "Local" / "fairyfly";
    const fs::path target = tmp / "profile" / ".fairyfly";
    legacy_layout(real);
    // the Store fairyfly ran first, before the folder existed for real: its own audit records, log and pairing
    write(package / "audit" / "2026-10.jsonl",
          "{\"ts\":\"2026-10-01T10:00:00.000Z\",\"command\":\"screen read\"}\n"       // also in the real one
          "{\"ts\":\"2026-10-03T09:00:00.000Z\",\"command\":\"menu list\"}\n");       // only here
    write(package / "audit" / "2026-08.jsonl", "{\"ts\":\"2026-08-01T00:00:00.000Z\",\"command\":\"doctor\"}\n");
    write(package / "link" / "state.json", "{\"device_id\":\"dev_store\"}");          // a different pairing
    write(package / "sessions" / "Store.con", "{\"id\":\"Store\"}");

    FreeLock lock;
    const auto r = config::migrate_data_dir(target, {real, package}, lock);
    REQUIRE(r.outcome == config::MigrationOutcome::Migrated);
    CHECK(r.migrated_from.size() == 2);

    const std::string audit = read(target / "audit" / "2026-10.jsonl");
    CHECK(count_of(audit, "2026-10-01T10:00:00.000Z") == 1);           // no duplicate
    CHECK(count_of(audit, "2026-10-02T10:00:00.000Z") == 1);
    CHECK(count_of(audit, "2026-10-03T09:00:00.000Z") == 1);           // nothing lost
    CHECK(read(target / "audit" / "2026-08.jsonl") == read(package / "audit" / "2026-08.jsonl"));
    CHECK(read(target / "link" / "state.json") == read(real / "link" / "state.json"));   // the first folder wins
    CHECK(std::find(r.conflicts.begin(), r.conflicts.end(), "link/state.json") != r.conflicts.end());
    CHECK(fs::exists(target / "sessions" / "Store.con"));
    CHECK(fs::exists(real / config::kMovedNote));
    CHECK(fs::exists(package / config::kMovedNote));
}

TEST_CASE("migration: the packaged process's merged view of the same folder adds no duplicates", "[config][datadir][migration]") {
    // A packaged process sees %LOCALAPPDATA%\fairyfly merged with its private copy, so both sources may hold the
    // very same files.
    test::TempDir tmp("datadir-merged-view");
    const fs::path a = tmp / "a" / "fairyfly";
    const fs::path b = tmp / "b" / "fairyfly";
    legacy_layout(a);
    legacy_layout(b);
    const auto expected = tree(b);
    const fs::path target = tmp / "profile" / ".fairyfly";
    FreeLock lock;
    const auto r = config::migrate_data_dir(target, {a, b}, lock);
    REQUIRE(r.outcome == config::MigrationOutcome::Migrated);
    CHECK(r.conflicts.empty());
    auto after = tree(target);
    after.erase(config::kDataDirMarker);
    CHECK(after == expected);
}

TEST_CASE("migration: an interrupted earlier move is discarded and done again", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-partial");
    const fs::path old = tmp / "Local" / "fairyfly";
    const fs::path target = tmp / "profile" / ".fairyfly";
    legacy_layout(old);
    // what a crash mid-copy leaves: a staging directory with a cut-off audit file and a half-written state
    const fs::path staging = fs::path(target.string() + config::kStagingSuffix);
    write(staging / "audit" / "2026-10.jsonl", "{\"ts\":\"2026-10-01T10:00:00.000Z\",\"command\":\"scr");
    write(staging / "link" / "state.json", "{\"devi");
    write(staging / "leftover.tmp", "x");

    FreeLock lock;
    const auto r = config::migrate_data_dir(target, {old}, lock);
    REQUIRE(r.outcome == config::MigrationOutcome::Migrated);
    auto after = tree(target);
    after.erase(config::kDataDirMarker);
    auto expected = tree(old);
    expected.erase(config::kMovedNote);
    CHECK(after == expected);
    CHECK_FALSE(fs::exists(staging));
}

TEST_CASE("migration: an existing data directory is never overwritten; old audit records are added", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-target-exists");
    const fs::path old = tmp / "Local" / "fairyfly";
    const fs::path target = tmp / "profile" / ".fairyfly";
    legacy_layout(old);
    write(target / "mcp.yaml", "server:\n  port: 9999\n");
    write(target / "audit" / "2026-10.jsonl", "{\"ts\":\"2026-10-02T10:00:00.000Z\",\"command\":\"element click\"}\n"
                                               "{\"ts\":\"2026-10-29T08:00:00.000Z\",\"command\":\"doctor\"}\n");
    FreeLock lock;
    const auto r = config::migrate_data_dir(target, {old}, lock);
    REQUIRE(r.outcome == config::MigrationOutcome::Merged);
    CHECK(read(target / "mcp.yaml") == "server:\n  port: 9999\n");      // kept
    CHECK(std::find(r.conflicts.begin(), r.conflicts.end(), "mcp.yaml") != r.conflicts.end());
    const std::string audit = read(target / "audit" / "2026-10.jsonl");
    CHECK(count_of(audit, "2026-10-01T10:00:00.000Z") == 1);             // added
    CHECK(count_of(audit, "2026-10-02T10:00:00.000Z") == 1);             // not duplicated
    CHECK(count_of(audit, "2026-10-29T08:00:00.000Z") == 1);             // kept
    CHECK(fs::exists(target / "link" / "state.json"));                   // missing files are copied
    CHECK(fs::exists(target / config::kDataDirMarker));
}

TEST_CASE("migration: records an old fairyfly appends during the copy are not lost", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-continuity");
    const fs::path old = tmp / "Local" / "fairyfly";
    const fs::path target = tmp / "profile" / ".fairyfly";
    legacy_layout(old);
    const std::string late = "{\"ts\":\"2026-10-29T12:00:00.000Z\",\"command\":\"key send\"}\n";
    config::MigrationOptions options;
    options.after_staging = [&] { append(old / "audit" / "2026-10.jsonl", late); };
    FreeLock lock;
    REQUIRE(config::migrate_data_dir(target, {old}, lock, options).outcome == config::MigrationOutcome::Migrated);
    const std::string audit = read(target / "audit" / "2026-10.jsonl");
    CHECK(count_of(audit, "2026-10-29T12:00:00.000Z") == 1);
    CHECK(count_of(audit, "2026-10-01T10:00:00.000Z") == 1);
    CHECK(audit.back() == '\n');
    // the last line of a record still being written (no newline yet) is left for the old folder, never half-copied
    test::TempDir tmp2("datadir-continuity-partial");
    legacy_layout(tmp2 / "old");
    append(tmp2 / "old" / "audit" / "2026-10.jsonl", "{\"ts\":\"2026-10-29T13:");
    REQUIRE(config::migrate_data_dir(tmp2 / ".fairyfly", {tmp2 / "old"}, lock).ok());
    CHECK(read(tmp2 / ".fairyfly" / "audit" / "2026-10.jsonl").find("2026-10-29T13:") == std::string::npos);
}

TEST_CASE("migration: when another fairyfly holds the lock too long nothing is touched", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-busy");
    const fs::path old = tmp / "Local" / "fairyfly";
    legacy_layout(old);
    const auto before = tree(old);
    BusyLock lock;
    const auto r = config::migrate_data_dir(tmp / ".fairyfly", {old}, lock);
    CHECK(r.outcome == config::MigrationOutcome::LockTimeout);
    CHECK_FALSE(r.ok());
    CHECK_FALSE(fs::exists(tmp / ".fairyfly"));
    CHECK(tree(old) == before);
}

TEST_CASE("migration: the tray and the CLI starting together migrate once", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-concurrent");
    const fs::path old = tmp / "Local" / "fairyfly";
    const fs::path target = tmp / "profile" / ".fairyfly";
    legacy_layout(old);
    const std::string lock_name = "fairyfly-data-migration-test-" + tmp.path().filename().string();
    constexpr int kStarts = 4;
    std::vector<config::MigrationResult> results(kStarts);
    std::atomic<bool> go{false};
    std::vector<std::thread> starts;
    for (int i = 0; i < kStarts; ++i)
        starts.emplace_back([&, i] {
            auto lock = config::make_migration_lock(lock_name);
            while (!go) std::this_thread::yield();
            results[i] = config::migrate_data_dir(target, {old}, *lock);
        });
    go = true;
    for (auto& t : starts) t.join();
    const auto migrated = std::count_if(results.begin(), results.end(),
                                        [](const auto& r) { return r.outcome == config::MigrationOutcome::Migrated; });
    const auto current = std::count_if(results.begin(), results.end(),
                                       [](const auto& r) { return r.outcome == config::MigrationOutcome::Current; });
    CHECK(migrated == 1);
    CHECK(current == kStarts - 1);
    const std::string audit = read(target / "audit" / "2026-10.jsonl");
    CHECK(count_of(audit, "2026-10-01T10:00:00.000Z") == 1);
}

TEST_CASE("migration: the real lock is exclusive and released", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-lock");
    const std::string name = "fairyfly-data-migration-test-" + tmp.path().filename().string();
    auto a = config::make_migration_lock(name);
    REQUIRE(a->acquire(std::chrono::milliseconds(1000)));
    bool b_got = true;
    std::thread other([&] {
        auto b = config::make_migration_lock(name);
        b_got = b->acquire(std::chrono::milliseconds(100));
        if (b_got) b->release();
    });
    other.join();
    CHECK_FALSE(b_got);
    a->release();
    std::thread later([&] {
        auto b = config::make_migration_lock(name);
        b_got = b->acquire(std::chrono::milliseconds(1000));
        if (b_got) b->release();
    });
    later.join();
    CHECK(b_got);
}

TEST_CASE("migration: an unwritable new location fails and leaves the old data alone", "[config][datadir][migration]") {
    test::TempDir tmp("datadir-unwritable");
    const fs::path old = tmp / "Local" / "fairyfly";
    legacy_layout(old);
    const auto before = tree(old);
    write(tmp / "profile", "a file where the profile directory should be");
    FreeLock lock;
    const auto r = config::migrate_data_dir(tmp / "profile" / ".fairyfly", {old}, lock);
    CHECK(r.outcome == config::MigrationOutcome::Failed);
    CHECK_FALSE(r.error.empty());
    CHECK(tree(old) == before);   // not even the note
}

TEST_CASE("migration: a required audit trail fails closed when the new location is not writable", "[config][datadir][audit]") {
    test::TempDir tmp("datadir-required");
    write(tmp / "profile", "not a directory");
    const auto env = env_of({{"USERPROFILE", (tmp / "profile").string()}, {"FAIRYFLY_AUDIT", "required"}});
    audit::AuditSink sink(audit::resolve_config(env, false, false, std::chrono::system_clock::now()));
    REQUIRE(sink.mode() == audit::Mode::Required);
    CHECK(sink.file().parent_path() == tmp / "profile" / ".fairyfly" / "audit");
    const bool probed = sink.probe();
    CHECK_FALSE(probed);
    const auto refusal = audit::preflight_error(sink.mode(), probed);
    REQUIRE(refusal.has_value());
    CHECK(*refusal == "AUDIT_UNAVAILABLE");
}

TEST_CASE("migration: the outcome names", "[config][datadir]") {
    CHECK(std::string(config::outcome_name(config::MigrationOutcome::Migrated)) == "migrated");
    CHECK(std::string(config::outcome_name(config::MigrationOutcome::LockTimeout)) == "lock_timeout");
    CHECK(std::string(config::outcome_name(config::MigrationOutcome::Current)) == "current");
}

// ---- the installers never delete the data -----------------------------------------------------------------------

TEST_CASE("data dir: neither the MSI nor its uninstall cleanup removes the data directory", "[config][datadir][packaging]") {
    const fs::path src = FAIRYFLY_SOURCE_DIR;
    const std::string wxs = read(src.parent_path() / "packaging" / "fairyfly.wxs");
    const std::string starter = read(src / "tray_launcher" / "fairyfly_tray.cpp");
    REQUIRE_FALSE(wxs.empty());
    REQUIRE_FALSE(starter.empty());
    for (const char* data : {"\\.fairyfly", ".fairyfly\\", "USERPROFILE", "ProfileFolder", "PersonalFolder"}) {
        CAPTURE(data);
        CHECK(wxs.find(data) == std::string::npos);
        CHECK(starter.find(data) == std::string::npos);
    }
    // the cleanup deletes registry trees only
    CHECK(starter.find("remove_all") == std::string::npos);
    CHECK(starter.find("RemoveDirectory") == std::string::npos);
    CHECK(starter.find("SHFileOperation") == std::string::npos);
}

TEST_CASE("data dir: only config/ builds paths from LOCALAPPDATA", "[config][datadir]") {
    // review 2026-10-29: the audit trail, the sessions, the link state and `mcp setup` each joined "fairyfly" onto
    // LOCALAPPDATA themselves; one root (Environment::data_dir) keeps them together
    const fs::path src = FAIRYFLY_SOURCE_DIR;
    std::vector<std::string> offenders;
    for (const auto& e : fs::recursive_directory_iterator(src)) {
        const auto ext = e.path().extension();
        if (!e.is_regular_file() || (ext != ".cpp" && ext != ".h" && ext != ".hpp")) continue;
        const fs::path rel = fs::relative(e.path(), src);
        if (rel.parent_path() == fs::path("config") || rel.parent_path() == fs::path("include") / "config") continue;
        const std::string text = read(e.path());
        for (const char* used : {"kLocalAppData", "local_app_data()", "FOLDERID_LocalAppData"})
            if (text.find(used) != std::string::npos) offenders.push_back(rel.generic_string() + ": " + used);
    }
    CAPTURE(offenders);
    CHECK(offenders.empty());
}
