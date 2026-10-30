// audit.retention_days: whole monthly audit files older than N days are pruned (audit_retention.h). Every test works in
// its own temporary data directory (test_main.cpp isolates the whole run from the real profile as well).
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "include/audit_log.h"
#include "include/audit_retention.h"
#include "include/config/data_dir.h"
#include "include/config/mcp_config.h"
#include "support/eventually.h"
#include "support/temp_dir.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace fairyfly;
using namespace fairyfly::audit;
namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;
using fairyfly::test::TempDir;

namespace {

Clock::time_point utc(int y, int m, int d, int hh = 0, int mm = 0, int ss = 0) {
    using namespace std::chrono;
    return sys_days{year{y} / month{static_cast<unsigned>(m)} / day{static_cast<unsigned>(d)}} + hours{hh} + minutes{mm} +
           seconds{ss};
}

void set_mtime(const fs::path& p, Clock::time_point t) {
    fs::last_write_time(p, std::chrono::clock_cast<fs::file_time_type::clock>(t));
}

/// A trail file with one record, last written at `written`.
void trail(const fs::path& dir, const std::string& name, Clock::time_point written) {
    fs::create_directories(dir);
    std::ofstream(dir / name, std::ios::binary) << "{\"ts\":\"x\",\"cmd\":\"screen read\"}\n";
    set_mtime(dir / name, written);
}

/// The last second of a month: what a file of that month looks like when nothing appended to it later.
Clock::time_point month_last_second(int y, int m) {
    return (m == 12 ? utc(y + 1, 1, 1) : utc(y, m + 1, 1)) - std::chrono::seconds(1);
}

std::set<std::string> names_in(const fs::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) out.insert(e.path().filename().string());
    return out;
}

std::vector<nlohmann::json> records(const fs::path& file) {
    std::vector<nlohmann::json> out;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty()) out.push_back(nlohmann::json::parse(line));
    return out;
}

const Clock::time_point kNow = utc(2026, 10, 30, 12, 0, 0);

/// 2025-12 .. 2026-10 as the trail of a desktop that used fairyfly every month (each file last written at its end,
/// the current month right now), plus files the retention must never touch.
void seed_year(const fs::path& audit) {
    trail(audit, "2025-12.jsonl", month_last_second(2025, 12));
    for (int m = 1; m <= 9; ++m) {
        char name[32];
        std::snprintf(name, sizeof name, "2026-%02d.jsonl", m);
        trail(audit, name, month_last_second(2026, m));
    }
    trail(audit, "2026-10.jsonl", kNow);
    trail(audit, "2026-01.migrated.jsonl", month_last_second(2026, 1));
    trail(audit, "notes.txt", utc(2020, 1, 1));
    trail(audit, "2026-01.jsonl.bak", utc(2020, 1, 1));
    trail(audit, "trail.jsonl", utc(2020, 1, 1));
    trail(audit, "2026-13.jsonl", utc(2020, 1, 1));
    trail(audit, "26-01.jsonl", utc(2020, 1, 1));
}

/// A sink on <data>/audit/<current month>, like a process of this data directory.
AuditSink sink_for(const fs::path& data, Mode mode = Mode::Enabled) {
    return AuditSink(AuditConfig{mode, data / "audit" / monthly_file_name(kNow)});
}

RetentionRun run_for(const fs::path& data, std::optional<long long> days, config::MigrationLock& lock) {
    RetentionRun run;
    run.retention_days = days;
    run.data_dir = data;
    run.lock = &lock;
    run.lock_timeout = std::chrono::milliseconds(50);
    run.now = kNow;
    return run;
}

}  // namespace

TEST_CASE("audit retention: trail file names map to their UTC month", "[audit][retention]") {
    CHECK(trail_file_month("2026-09.jsonl") == std::make_pair(2026, 9));
    CHECK(trail_file_month("2025-12.migrated.jsonl") == std::make_pair(2025, 12));
    CHECK_FALSE(trail_file_month("2026-13.jsonl"));
    CHECK_FALSE(trail_file_month("2026-00.jsonl"));
    CHECK_FALSE(trail_file_month("26-01.jsonl"));
    CHECK_FALSE(trail_file_month("2026-01.jsonl.bak"));
    CHECK_FALSE(trail_file_month("trail.jsonl"));
    CHECK_FALSE(trail_file_month("2026-01.JSONL.txt"));
    CHECK_FALSE(trail_file_month("x2026-01.jsonl"));
}

TEST_CASE("audit retention: unset or 0 keeps every file", "[audit][retention]") {
    TempDir data("retention");
    const fs::path audit = data / "audit";
    seed_year(audit);
    const auto before = names_in(audit);

    CHECK(prune_expired(audit, 0, kNow, audit / "2026-10.jsonl").pruned.empty());
    CHECK(prune_expired(audit, -5, kNow, audit / "2026-10.jsonl").pruned.empty());
    CHECK(names_in(audit) == before);

    auto lock = config::make_migration_lock("fairyfly-retention-test", data.path());
    AuditSink sink = sink_for(data);
    for (const std::optional<long long> days : {std::optional<long long>{}, std::optional<long long>{0}}) {
        const RetentionOutcome out = apply_retention(sink, run_for(data, days, *lock));
        CHECK_FALSE(out.skipped.empty());
        CHECK(out.result.pruned.empty());
        CHECK_FALSE(out.recorded);
    }
    CHECK(names_in(audit) == before);
    CHECK(start_retention(sink, std::nullopt) == nullptr);
    CHECK(start_retention(sink, 0) == nullptr);
}

TEST_CASE("audit retention: only whole months older than N days go, the current month never", "[audit][retention]") {
    TempDir data("retention");
    const fs::path audit = data / "audit";
    seed_year(audit);

    SECTION("90 days: the months that ended before 2026-08-01 12:00") {
        const PruneResult r = prune_expired(audit, 90, kNow, audit / "2026-10.jsonl");
        CHECK(r.failed.empty());
        CHECK(r.pruned == std::vector<std::string>{"2025-12.jsonl", "2026-01.jsonl", "2026-01.migrated.jsonl", "2026-02.jsonl",
                                                   "2026-03.jsonl", "2026-04.jsonl", "2026-05.jsonl", "2026-06.jsonl",
                                                   "2026-07.jsonl"});
        CHECK(names_in(audit) == std::set<std::string>{"2026-08.jsonl", "2026-09.jsonl", "2026-10.jsonl", "notes.txt",
                                                       "2026-01.jsonl.bak", "trail.jsonl", "2026-13.jsonl", "26-01.jsonl"});
    }
    SECTION("1 day: every finished month, the current one stays") {
        const PruneResult r = prune_expired(audit, 1, kNow, audit / "2026-10.jsonl");
        CHECK(std::count(r.pruned.begin(), r.pruned.end(), std::string("2026-09.jsonl")) == 1);
        CHECK(fs::exists(audit / "2026-10.jsonl"));
    }
    SECTION("the current month is kept even when it is not the active file and was written long ago") {
        set_mtime(audit / "2026-10.jsonl", utc(2020, 1, 1));
        prune_expired(audit, 1, kNow, audit / "elsewhere.jsonl");
        CHECK(fs::exists(audit / "2026-10.jsonl"));
    }
    SECTION("a month that ended less than N days ago stays: 2026-09 ended 29.5 days ago") {
        prune_expired(audit, 30, kNow, audit / "2026-10.jsonl");
        CHECK(fs::exists(audit / "2026-09.jsonl"));
        CHECK_FALSE(fs::exists(audit / "2026-08.jsonl"));
    }
    SECTION("a file appended to after its month (a process started then) is kept until that write is N days old") {
        set_mtime(audit / "2026-03.jsonl", utc(2026, 10, 1));
        prune_expired(audit, 90, kNow, audit / "2026-10.jsonl");
        CHECK(fs::exists(audit / "2026-03.jsonl"));
        CHECK_FALSE(fs::exists(audit / "2026-04.jsonl"));
    }
    SECTION("the file this process appends to is never deleted, whatever its month") {
        prune_expired(audit, 90, kNow, audit / "2026-02.jsonl");
        CHECK(fs::exists(audit / "2026-02.jsonl"));
        CHECK_FALSE(fs::exists(audit / "2026-03.jsonl"));
    }
    SECTION("subfolders are left alone") {
        fs::create_directories(audit / "2020-01.jsonl");
        prune_expired(audit, 1, kNow, audit / "2026-10.jsonl");
        CHECK(fs::is_directory(audit / "2020-01.jsonl"));
    }
    SECTION("a missing folder is no error") {
        CHECK(prune_expired(data / "nope", 1, kNow, {}).pruned.empty());
    }
}

TEST_CASE("audit retention: the pruned file names are recorded in the trail itself, never their content",
          "[audit][retention]") {
    TempDir data("retention");
    const fs::path audit = data / "audit";
    seed_year(audit);
    auto lock = config::make_migration_lock("fairyfly-retention-test", data.path());
    AuditSink sink = sink_for(data);

    const RetentionOutcome out = apply_retention(sink, run_for(data, 200, *lock));
    CHECK(out.skipped.empty());
    CHECK(out.result.pruned == std::vector<std::string>{"2025-12.jsonl", "2026-01.jsonl", "2026-01.migrated.jsonl",
                                                        "2026-02.jsonl", "2026-03.jsonl"});
    REQUIRE(out.recorded);
    const auto all = records(sink.file());
    REQUIRE_FALSE(all.empty());
    const nlohmann::json& rec = all.back();
    CHECK(rec["cmd"] == kPrunedCommand);
    CHECK(rec["audit_source"] == kRetentionSource);
    CHECK(rec["status"] == "success");
    CHECK(rec["retention_days"] == 200);
    CHECK(rec["pruned"] == nlohmann::json(out.result.pruned));
    CHECK_FALSE(rec.contains("prune_failed"));
    CHECK(rec.dump().find("screen read") == std::string::npos);   // the pruned records are not copied anywhere

    SECTION("nothing to prune: no record") {
        const auto count = records(sink.file()).size();
        const RetentionOutcome again = apply_retention(sink, run_for(data, 200, *lock));
        CHECK(again.result.pruned.empty());
        CHECK_FALSE(again.recorded);
        CHECK(records(sink.file()).size() == count);
    }
}

TEST_CASE("audit retention: a refused delete (the file is open elsewhere) is reported and retried next time",
          "[audit][retention]") {
    TempDir data("retention");
    const fs::path audit = data / "audit";
    seed_year(audit);
    auto lock = config::make_migration_lock("fairyfly-retention-test", data.path());
    AuditSink sink = sink_for(data);

    RetentionRun run = run_for(data, 200, *lock);
    run.remove = [](const fs::path& p) {
        if (p.filename() == "2026-02.jsonl") return false;   // another process holds it open
        return remove_if_unused(p);
    };
    const RetentionOutcome first = apply_retention(sink, run);
    CHECK(first.result.failed == std::vector<std::string>{"2026-02.jsonl"});
    CHECK(fs::exists(audit / "2026-02.jsonl"));
    CHECK_FALSE(fs::exists(audit / "2026-03.jsonl"));
    REQUIRE(first.recorded);
    const nlohmann::json rec = records(sink.file()).back();
    CHECK(rec["cmd"] == kPrunedCommand);
    CHECK(rec["status"] == "error");
    CHECK(rec["error_code"] == "AUDIT_PRUNE_INCOMPLETE");
    CHECK(rec["prune_failed"] == nlohmann::json::array({"2026-02.jsonl"}));

    // the next run (the next day) gets it
    const RetentionOutcome second = apply_retention(sink, run_for(data, 200, *lock));
    CHECK(second.result.pruned == std::vector<std::string>{"2026-02.jsonl"});
    CHECK_FALSE(fs::exists(audit / "2026-02.jsonl"));
}

#ifdef _WIN32
TEST_CASE("audit retention: Windows refuses to delete a trail file another process has open for append",
          "[audit][retention][windows]") {
    TempDir data("retention");
    const fs::path audit = data / "audit";
    trail(audit, "2026-01.jsonl", month_last_second(2026, 1));
    // opened exactly like AuditSink::append opens it (sharing read, write and delete)
    HANDLE h = CreateFileW((audit / "2026-01.jsonl").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    const PruneResult held = prune_expired(audit, 30, kNow, audit / "2026-10.jsonl");
    CHECK(held.failed == std::vector<std::string>{"2026-01.jsonl"});
    CHECK(held.pruned.empty());
    DWORD written = 0;
    CHECK(WriteFile(h, "x\n", 2, &written, nullptr));   // the appender is not disturbed
    CloseHandle(h);
    CHECK(fs::exists(audit / "2026-01.jsonl"));
    // that write made it recent (kept by the last-write rule); as if it had been the month's last record
    set_mtime(audit / "2026-01.jsonl", month_last_second(2026, 1));
    const PruneResult later = prune_expired(audit, 30, kNow, audit / "2026-10.jsonl");
    CHECK(later.pruned == std::vector<std::string>{"2026-01.jsonl"});
    CHECK_FALSE(fs::exists(audit / "2026-01.jsonl"));
}
#endif

TEST_CASE("audit retention: never during or after a failed data directory migration", "[audit][retention][migration]") {
    TempDir data("retention");
    const fs::path audit = data / "audit";
    seed_year(audit);
    const auto before = names_in(audit);
    auto lock = config::make_migration_lock("fairyfly-retention-test", data.path());
    AuditSink sink = sink_for(data);

    SECTION("this process's migration failed or timed out: the old folder is still pending") {
        for (const auto outcome : {config::MigrationOutcome::Failed, config::MigrationOutcome::LockTimeout}) {
            RetentionRun run = run_for(data, 1, *lock);
            run.migration = config::MigrationResult{};
            run.migration->outcome = outcome;
            CHECK_FALSE(apply_retention(sink, run).skipped.empty());
        }
    }
    SECTION("a staging folder of a move in progress") {
        fs::create_directories(data.path().string() + config::kStagingSuffix);
        CHECK_FALSE(apply_retention(sink, run_for(data, 1, *lock)).skipped.empty());
    }
    SECTION("another fairyfly holds the migration lock") {
        auto other = config::make_migration_lock("fairyfly-retention-test", data.path());
        REQUIRE(other->acquire(std::chrono::milliseconds(1000)));
        std::atomic<bool> skipped{false};
        // a lock of this process would be reentrant on Windows: try from another thread like another process would
        std::thread([&] { skipped = !apply_retention(sink, run_for(data, 1, *lock)).skipped.empty(); }).join();
        other->release();
        CHECK(skipped);
    }
    CHECK(names_in(audit) == before);

    SECTION("a successful migration does not stop it") {
        RetentionRun run = run_for(data, 1, *lock);
        run.migration = config::MigrationResult{};
        run.migration->outcome = config::MigrationOutcome::Migrated;
        CHECK(apply_retention(sink, run).skipped.empty());
        CHECK_FALSE(fs::exists(audit / "2026-09.jsonl"));
    }
}

TEST_CASE("audit retention: a custom audit file is never pruned around", "[audit][retention]") {
    TempDir data("retention");
    TempDir elsewhere("retention-custom");
    seed_year(data / "audit");
    seed_year(elsewhere.path());
    auto lock = config::make_migration_lock("fairyfly-retention-test", data.path());
    AuditSink sink(AuditConfig{Mode::Enabled, elsewhere / "trail.jsonl"});
    const RetentionOutcome out = apply_retention(sink, run_for(data, 1, *lock));
    CHECK_FALSE(out.skipped.empty());
    CHECK(fs::exists(elsewhere / "2026-01.jsonl"));
    CHECK(fs::exists(data / "audit" / "2026-01.jsonl"));
}

TEST_CASE("audit retention: a required trail honours the retention, but only while it can record it",
          "[audit][retention]") {
    TempDir data("retention");
    const fs::path audit = data / "audit";
    seed_year(audit);
    auto lock = config::make_migration_lock("fairyfly-retention-test", data.path());

    SECTION("writable: pruned and recorded") {
        AuditSink sink = sink_for(data, Mode::Required);
        const RetentionOutcome out = apply_retention(sink, run_for(data, 1, *lock));
        CHECK(out.skipped.empty());
        CHECK(out.recorded);
        CHECK_FALSE(fs::exists(audit / "2026-09.jsonl"));
    }
    SECTION("not writable (the active file is a folder): nothing pruned") {
        fs::remove(audit / "2026-10.jsonl");
        fs::create_directories(audit / "2026-10.jsonl");
        AuditSink sink = sink_for(data, Mode::Required);
        const RetentionOutcome out = apply_retention(sink, run_for(data, 1, *lock));
        CHECK_FALSE(out.skipped.empty());
        CHECK(fs::exists(audit / "2026-09.jsonl"));
    }
    SECTION("audit off: the retention still applies, without a record") {
        AuditSink sink = sink_for(data, Mode::Disabled);
        const auto count = records(audit / "2026-10.jsonl").size();
        const RetentionOutcome out = apply_retention(sink, run_for(data, 1, *lock));
        CHECK(out.skipped.empty());
        CHECK_FALSE(out.recorded);
        CHECK_FALSE(fs::exists(audit / "2026-09.jsonl"));
        CHECK(records(audit / "2026-10.jsonl").size() == count);
    }
}

TEST_CASE("audit retention: long-running processes prune at their start and then on every interval",
          "[audit][retention]") {
    std::atomic<int> runs{0};
    {
        RetentionScheduler scheduler([&] { ++runs; }, std::chrono::milliseconds(10));
        CHECK(test::eventually([&] { return runs.load() >= 3; }));
    }
    const int after = runs.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(runs.load() == after);   // stopped with the scheduler

    // a daily interval does not hold up the end of the process
    const auto start = std::chrono::steady_clock::now();
    { RetentionScheduler daily([] {}, std::chrono::hours(24)); }
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

TEST_CASE("audit retention: start_retention prunes this process's data directory at once", "[audit][retention]") {
    const fs::path data = config::Environment::process().data_dir();   // test_main.cpp's temporary profile
    REQUIRE_FALSE(data.empty());
    const fs::path audit = data / "audit";
    trail(audit, "2001-01.jsonl", month_last_second(2001, 1));
    AuditSink sink(AuditConfig{Mode::Enabled, audit / monthly_file_name(Clock::now())});
    auto scheduler = start_retention(sink, 30);
    CHECK(scheduler != nullptr);
    CHECK_FALSE(fs::exists(audit / "2001-01.jsonl"));
    scheduler.reset();
    std::error_code ec;
    fs::remove(sink.file(), ec);
}

TEST_CASE("audit retention: the record fields are only emitted for audit.pruned", "[audit][retention]") {
    AuditRecord plain;
    plain.command = "screen read";
    const auto j = nlohmann::json::parse(format_record(plain));
    CHECK_FALSE(j.contains("pruned"));
    CHECK_FALSE(j.contains("prune_failed"));
    CHECK_FALSE(j.contains("retention_days"));
}

// ---- configuration: one key in the key table (yaml < env < flag) -------------------------------------------------

TEST_CASE("audit retention: audit.retention_days is a key of mcp.yaml with its env variable", "[audit][retention][config]") {
    const config::KeySpec* spec = config::find_key("audit.retention_days");
    REQUIRE(spec != nullptr);
    CHECK(spec->type == config::ValueType::Int);
    CHECK_FALSE(spec->def.has_value());   // unset = keep forever
    CHECK(spec->env == "FAIRYFLY_MCP_AUDIT_RETENTION_DAYS");
    CHECK(spec->cli == std::vector<std::string>{"--audit-retention-days"});
    CHECK(spec->min == 0);
    CHECK(spec->max == kMaxRetentionDays);

    const auto r = config::parse_yaml("audit:\n  retention_days: 400\n");
    REQUIRE(r.ok());
    CHECK(config::audit_settings(r.config).retention_days == 400);
    CHECK_FALSE(config::audit_settings(config::McpConfig{}).retention_days.has_value());
    mcp::ServeOptions o;
    config::apply_config(r.config, o);
    CHECK(o.audit_file.retention_days == 400);
    // the template documents it, commented out (unset)
    CHECK(config::config_template().find("# retention_days: 0") != std::string::npos);
}

TEST_CASE("audit retention: invalid values are refused like every other key", "[audit][retention][config]") {
    for (const char* bad : {"-1", "36501", "ninety", "1.5", "[30]", "30d"}) {
        CAPTURE(bad);
        const auto r = config::parse_yaml(std::string("audit:\n  retention_days: ") + bad + "\n");
        CHECK_FALSE(r.ok());
        CHECK(r.first_error_code() == "CONFIG_INVALID_VALUE");
        CHECK_FALSE(r.config.has("audit.retention_days"));
    }
    CHECK(config::parse_yaml("audit:\n  retention_days: 0\n").ok());
    CHECK(config::parse_yaml("audit:\n  retention_days: 36500\n").ok());

    // the environment: ignored with a warning (CONFIG_ENV_INVALID), as for every key
    std::vector<config::ConfigIssue> issues;
    const auto env = config::env_layer([](const char* name) {
        return std::string(name) == "FAIRYFLY_MCP_AUDIT_RETENTION_DAYS" ? std::string("-3") : std::string();
    }, &issues);
    CHECK_FALSE(env.has("audit.retention_days"));
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "CONFIG_ENV_INVALID");
    CHECK(issues[0].severity == config::Severity::Warning);

    // a flag: parse_value refuses it (flags_layer turns that into CONFIG_FLAG_INVALID)
    std::string why;
    CHECK_FALSE(config::parse_value(*config::find_key("audit.retention_days"), "-1", &why));
    CHECK_FALSE(why.empty());
}

TEST_CASE("audit retention: precedence flag > env > yaml", "[audit][retention][config][precedence]") {
    config::McpConfig yaml, env, flags;
    yaml.set("audit.retention_days", 365LL);
    CHECK(config::audit_settings(yaml, env, flags).retention_days == 365);
    env.set("audit.retention_days", 180LL);
    CHECK(config::audit_settings(yaml, env, flags).retention_days == 180);
    flags.set("audit.retention_days", 0LL);   // an explicit 0 on the command line keeps everything for this run
    CHECK(config::audit_settings(yaml, env, flags).retention_days == 0);

    const auto effective = config::resolve(yaml, flags, [](const char* name) {
        return std::string(name) == "FAIRYFLY_MCP_AUDIT_RETENTION_DAYS" ? std::string("90") : std::string();
    });
    CHECK(effective.at("audit.retention_days").source == config::Source::Flag);
}

TEST_CASE("audit retention: `link run` reads it from mcp.yaml and FAIRYFLY_MCP_AUDIT_RETENTION_DAYS",
          "[audit][retention][config]") {
    TempDir dir("retention-config");
    const fs::path file = dir / "mcp.yaml";
    std::map<std::string, std::string> vars{{"FAIRYFLY_MCP_CONFIG", file.string()}};
    const auto lookup = [&vars](const char* name) {
        const auto i = vars.find(name);
        return i == vars.end() ? std::string() : i->second;
    };
    CHECK_FALSE(config::configured_audit_retention(lookup).has_value());   // no file: keep
    REQUIRE(config::write_text_file(file, "audit:\n  retention_days: 400\n"));
    CHECK(config::configured_audit_retention(lookup) == 400);
    vars["FAIRYFLY_MCP_AUDIT_RETENTION_DAYS"] = "120";
    CHECK(config::configured_audit_retention(lookup) == 120);

    // a broken file or value never deletes anything: keep, with an issue
    vars.erase("FAIRYFLY_MCP_AUDIT_RETENTION_DAYS");
    REQUIRE(config::write_text_file(file, "audit:\n  retention_days: -4\n"));
    std::vector<config::ConfigIssue> issues;
    CHECK_FALSE(config::configured_audit_retention(lookup, &issues).has_value());
    CHECK_FALSE(issues.empty());
    vars["FAIRYFLY_MCP_AUDIT_RETENTION_DAYS"] = "soon";
    issues.clear();
    CHECK_FALSE(config::configured_audit_retention(lookup, &issues).has_value());
    CHECK_FALSE(issues.empty());
}
