#include "include/audit_retention.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>

#include "include/text/utf8.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fairyfly::audit {

namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;

namespace {

constexpr const char* kMonthSuffix = ".jsonl";
constexpr const char* kMigratedSuffix = ".migrated.jsonl";

bool digits(const std::string& s, std::size_t from, std::size_t count) {
    if (s.size() < from + count) return false;
    for (std::size_t i = from; i < from + count; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/// The first instant after the month: no record of that month can be newer.
Clock::time_point month_end(int y, int m) {
    using namespace std::chrono;
    const year_month next = year{y} / month{static_cast<unsigned>(m)} + months{1};
    return sys_days{next / day{1}};
}

std::optional<Clock::time_point> last_written(const fs::path& file) {
    std::error_code ec;
    const auto t = fs::last_write_time(file, ec);
    if (ec) return std::nullopt;
    return std::chrono::clock_cast<Clock>(t);
}

bool same_file_name(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    return a.lexically_normal() == b.lexically_normal();
}

void log_skip(const std::string& why) noexcept {
    try {
        spdlog::info("audit retention: nothing pruned ({})", why);
    } catch (...) {
    }
}

}  // namespace

std::optional<std::pair<int, int>> trail_file_month(const std::string& name) {
    // "YYYY-MM" + ".jsonl" or ".migrated.jsonl", nothing else
    if (!digits(name, 0, 4) || name.size() < 7 || name[4] != '-' || !digits(name, 5, 2)) return std::nullopt;
    const std::string rest = name.substr(7);
    if (rest != kMonthSuffix && rest != kMigratedSuffix) return std::nullopt;
    const int y = std::stoi(name.substr(0, 4));
    const int m = std::stoi(name.substr(5, 2));
    if (m < 1 || m > 12) return std::nullopt;
    return std::make_pair(y, m);
}

bool is_link_or_reparse_point(const fs::path& path) noexcept {
    try {
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(path, ec))) return true;
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
        return false;
    } catch (...) {
        return true;   // unknown: treat it as a link, i.e. leave it alone
    }
}

bool remove_if_unused(const fs::path& file) noexcept {
    try {
#ifdef _WIN32
        // No sharing: the open fails (ERROR_SHARING_VIOLATION) while any other handle is open, an appender's included
        // (AuditSink opens with FILE_SHARE_DELETE, so a plain DeleteFile would only mark the file for deletion under
        // it). FILE_FLAG_OPEN_REPARSE_POINT: a link that appeared since the scan is opened itself, never its target.
        HANDLE h = CreateFileW(file.c_str(), DELETE, 0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::error_code ec;
            return !fs::exists(fs::symlink_status(file, ec)) && !ec;
        }
        CloseHandle(h);
        std::error_code ec;
        return !fs::exists(fs::symlink_status(file, ec)) && !ec;
#else
        std::error_code ec;
        fs::remove(file, ec);   // removes a link itself, never its target
        return !ec && !fs::exists(fs::symlink_status(file, ec));
#endif
    } catch (...) {
        return false;
    }
}

namespace {

struct Scan {
    std::vector<fs::path> expired;                 ///< sorted by name
    std::optional<Clock::time_point> newest_write; ///< newest last write of any trail file (clock sanity)
};

Scan scan_trail(const fs::path& audit_dir, long long retention_days, Clock::time_point now, const fs::path& active_file,
                const LinkFn& is_link) {
    Scan out;
    if (retention_days <= 0) return out;
    const Clock::time_point cutoff = now - std::chrono::hours(24) * retention_days;
    const std::string current = monthly_file_name(now);
    const std::string current_migrated = current.substr(0, 7) + kMigratedSuffix;
    std::error_code ec;
    fs::directory_iterator it(audit_dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::path path = it->path();
        const std::string name = text::path_utf8(path.filename());
        const auto month = trail_file_month(name);
        if (!month) continue;
        if (is_link ? is_link(path) : is_link_or_reparse_point(path)) continue;   // never followed, never deleted
        std::error_code type_ec;
        if (!fs::is_regular_file(fs::symlink_status(path, type_ec)) || type_ec) continue;
        const auto written = last_written(path);
        if (written && (!out.newest_write || *written > *out.newest_write)) out.newest_write = written;
        if (name == current || name == current_migrated) continue;   // the current month, never
        if (same_file_name(path, active_file)) continue;              // this process appends to it
        if (month_end(month->first, month->second) > cutoff) continue;
        if (!written || *written > cutoff) continue;                  // appended later (or unknown): keep
        out.expired.push_back(path);
    }
    std::sort(out.expired.begin(), out.expired.end());
    return out;
}

PruneResult delete_all(const std::vector<fs::path>& files, const RemoveFn& remove) {
    PruneResult out;
    for (const auto& path : files) {
        const bool gone = remove ? remove(path) : remove_if_unused(path);
        (gone ? out.pruned : out.failed).push_back(text::path_utf8(path.filename()));
    }
    std::sort(out.pruned.begin(), out.pruned.end());
    std::sort(out.failed.begin(), out.failed.end());
    return out;
}

AuditRecord retention_record(const char* command, long long days) {
    AuditRecord record;
    record.ts = Clock::now();
    record.command = command;
    record.source = kRetentionSource;
    record.retention_days = days;
    return record;
}

}  // namespace

PruneResult prune_expired(const fs::path& audit_dir, long long retention_days, Clock::time_point now,
                          const fs::path& active_file, const RemoveFn& remove, const LinkFn& is_link) {
    try {
        return delete_all(scan_trail(audit_dir, retention_days, now, active_file, is_link).expired, remove);
    } catch (...) {
        return {};
    }
}

RetentionOutcome apply_retention(AuditSink& sink, const RetentionRun& run) {
    RetentionOutcome out;
    auto skip = [&](std::string why) {
        out.skipped = std::move(why);
        log_skip(out.skipped);
        return out;
    };
    try {
        if (!run.retention_days || *run.retention_days <= 0) {
            out.skipped = "retention off";
            return out;
        }
        if (*run.retention_days < kMinRetentionDays)
            return skip("audit.retention_days " + std::to_string(*run.retention_days) + " is below the minimum of " +
                        std::to_string(kMinRetentionDays));
        if (run.data_dir.empty()) return skip("no data directory");
        const fs::path audit_dir = run.data_dir / "audit";
        if (!same_file_name(sink.file().parent_path(), audit_dir))
            return skip("a custom audit file is used (FAIRYFLY_AUDIT_FILE / audit.file)");
        if (run.migration && !run.migration->ok()) return skip("the data directory migration is pending");
        std::error_code ec;
        if (fs::exists(fs::path(run.data_dir) += config::kStagingSuffix, ec))
            return skip("a data directory migration is in progress");
        if (sink.mode() == Mode::Required && !sink.probe()) return skip("the required audit trail cannot be written");

        std::unique_ptr<config::MigrationLock> own;
        config::MigrationLock* lock = run.lock;
        if (!lock) {
            own = config::make_migration_lock();
            lock = own.get();
        }
        if (!lock->acquire(run.lock_timeout)) return skip("another fairyfly holds the data directory lock");
        struct Release {
            config::MigrationLock* lock;
            ~Release() { lock->release(); }
        } release{lock};

        const Clock::time_point now = run.now == Clock::time_point{} ? Clock::now() : run.now;
        const Scan scan = scan_trail(audit_dir, *run.retention_days, now, sink.file(), {});
        if (scan.expired.empty()) return out;
        // a clock far ahead of the newest record (a wrong date, a restored VM) would make everything look expired
        if (scan.newest_write && now - *scan.newest_write > std::chrono::hours(24) * kClockJumpDays)
            return skip("the clock is more than " + std::to_string(kClockJumpDays) +
                        " days past the newest audit record; check the system date");

        // write-before-delete: the intent is in the trail before the first file goes, or nothing goes
        if (sink.enabled()) {
            AuditRecord intent = retention_record(kPruneIntentCommand, *run.retention_days);
            for (const auto& path : scan.expired) intent.prune_candidates.push_back(text::path_utf8(path.filename()));
            if (!sink.append(intent)) return skip("the audit.prune_intent record could not be written");
        }

        out.result = delete_all(scan.expired, run.remove);
        for (const auto& name : out.result.pruned) spdlog::info("audit retention: pruned {}", name);
        for (const auto& name : out.result.failed)
            spdlog::warn("audit retention: {} is in use and was kept; the next run tries again", name);
        if (!sink.enabled()) return out;
        AuditRecord record = retention_record(kPrunedCommand, *run.retention_days);
        record.pruned = out.result.pruned;
        record.prune_failed = out.result.failed;
        if (!out.result.failed.empty()) {
            record.status = "error";
            record.error_code = "AUDIT_PRUNE_INCOMPLETE";
        }
        out.recorded = sink.append(record);
        if (!out.recorded)
            spdlog::error("audit retention: the audit.pruned record could not be written (the intent record lists the files)");
    } catch (...) {
    }
    return out;
}

struct RetentionScheduler::State {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    std::thread thread;
};

RetentionScheduler::RetentionScheduler(std::function<void()> job, std::chrono::milliseconds interval)
    : state_(std::make_shared<State>()) {
    state_->thread = std::thread([state = state_, job = std::move(job), interval] {
        std::unique_lock<std::mutex> lock(state->mutex);
        while (!state->stop) {
            lock.unlock();
            try {
                job();
            } catch (...) {
            }
            lock.lock();
            state->wake.wait_for(lock, interval, [&] { return state->stop; });
        }
    });
}

RetentionScheduler::~RetentionScheduler() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stop = true;
    }
    state_->wake.notify_all();
    if (state_->thread.joinable()) state_->thread.join();
}

std::unique_ptr<RetentionScheduler> start_retention(AuditSink& sink, std::optional<long long> retention_days) {
    if (!retention_days || *retention_days <= 0) return nullptr;
    const auto run_once = [&sink, days = *retention_days] {
        RetentionRun run;
        run.retention_days = days;
        run.data_dir = config::Environment::process().data_dir();
        run.migration = config::last_migration();
        apply_retention(sink, run);
    };
    run_once();   // at the start, before the process serves anything
    struct FirstDelayed {
        std::function<void()> job;
        bool first = true;
        void operator()() {
            if (first) { first = false; return; }   // ran above
            job();
        }
    };
    return std::make_unique<RetentionScheduler>(FirstDelayed{run_once}, std::chrono::hours(24));
}

}  // namespace fairyfly::audit
