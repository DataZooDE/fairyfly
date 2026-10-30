#pragma once
// Local retention of the audit trail (`audit.retention_days`, docs/CLI.md "Audit trail", docs/DATA_DIR.md).
//
// Unset or 0: the trail is kept forever (the default). N > 0: whole monthly files of the data directory's audit folder
// (<data dir>\audit\YYYY-MM.jsonl and the migration's YYYY-MM.migrated.jsonl) are deleted once their newest POSSIBLE
// record is older than N days: the month ended more than N days ago AND the file was last written more than N days
// ago (a long-running process started in that month may still append to it). Never the current UTC month, never the
// file this process appends to, never a custom audit file (FAIRYFLY_AUDIT_FILE / audit.file), never while the data
// directory migration is pending or running (its lock), and never a file another process has open (Windows: the
// delete needs exclusive access; a refused delete is retried at the next run). A required audit trail honours the
// retention too (the customer's IT decides) but prunes only when the trail is writable. Every pruned file name (never
// its content) is recorded in the trail itself: cmd "audit.pruned", audit_source "retention".
//
// Long-running processes (`mcp` in every transport and tray, `link run` and its tray) prune at their start and then
// once a day (start_retention).
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/audit_log.h"
#include "include/config/data_dir.h"

namespace fairyfly::audit {

/// The largest audit.retention_days (100 years).
inline constexpr long long kMaxRetentionDays = 36500;
/// The record that lists the pruned files.
inline constexpr const char* kPrunedCommand = "audit.pruned";
inline constexpr const char* kRetentionSource = "retention";

/// The UTC year and month of a trail file name: "2026-09.jsonl" and "2026-09.migrated.jsonl" -> {2026, 9};
/// nullopt for any other name.
std::optional<std::pair<int, int>> trail_file_month(const std::string& file_name);

/// Deletes one file unless another process has it open; true when it is gone. Never throws.
using RemoveFn = std::function<bool(const std::filesystem::path&)>;
/// The real one. Windows: opened for DELETE without sharing and FILE_FLAG_DELETE_ON_CLOSE, so a file another process
/// has open (an appender, an editor, a log shipper) is refused instead of left behind as a pending delete.
bool remove_if_unused(const std::filesystem::path& file) noexcept;

struct PruneResult {
    std::vector<std::string> pruned;   ///< file names, sorted
    std::vector<std::string> failed;   ///< file names whose delete was refused (retried next time), sorted
};

/// Deletes the expired trail files directly in `audit_dir` (see the top of this file). `active_file` is never deleted.
/// retention_days <= 0 deletes nothing. Never throws.
PruneResult prune_expired(const std::filesystem::path& audit_dir, long long retention_days,
                          std::chrono::system_clock::time_point now, const std::filesystem::path& active_file,
                          const RemoveFn& remove = remove_if_unused);

/// One retention run of a process.
struct RetentionRun {
    std::optional<long long> retention_days;           ///< unset or 0: keep everything
    std::filesystem::path data_dir;                    ///< the data directory; only <data_dir>/audit is pruned
    std::optional<config::MigrationResult> migration;  ///< this process's migration (not ok: skip)
    config::MigrationLock* lock = nullptr;             ///< the migration lock (nullptr: config::make_migration_lock())
    std::chrono::milliseconds lock_timeout{2000};
    std::chrono::system_clock::time_point now{};
    RemoveFn remove;                                   ///< empty: remove_if_unused
};

struct RetentionOutcome {
    std::string skipped;    ///< why nothing was tried ("" when the folder was pruned)
    PruneResult result;
    bool recorded = false;  ///< the audit.pruned record was written
};

/// Prunes `sink`'s trail per `run` and records the result through `sink` (only when something was deleted or
/// refused). Skips: retention off, a custom audit file, a pending/failed migration, the staging folder present, the
/// migration lock held elsewhere, a required trail that cannot be written. Never throws.
RetentionOutcome apply_retention(AuditSink& sink, const RetentionRun& run);

/// Runs `job` every `interval` on its own thread until destroyed (the destructor wakes and joins it at once).
class RetentionScheduler {
public:
    RetentionScheduler(std::function<void()> job, std::chrono::milliseconds interval);
    ~RetentionScheduler();
    RetentionScheduler(const RetentionScheduler&) = delete;
    RetentionScheduler& operator=(const RetentionScheduler&) = delete;

private:
    struct State;
    std::shared_ptr<State> state_;
};

/// The retention of a long-running process: apply_retention for this process's data directory and migration now,
/// then once a day while the returned scheduler lives. nullptr when the retention is off.
std::unique_ptr<RetentionScheduler> start_retention(AuditSink& sink, std::optional<long long> retention_days);

}  // namespace fairyfly::audit
