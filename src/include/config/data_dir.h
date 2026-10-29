#pragma once
// fairyfly's per-user data directory and its move from %LOCALAPPDATA%\fairyfly (docs/DATA_DIR.md).
//
// Where: config::Environment::data_dir() (FAIRYFLY_DATA_DIR > %USERPROFILE%\.fairyfly > $HOME/.fairyfly; on Windows a
// missing USERPROFILE is read from the Profile known folder). Never the temp directory or a relative path: without a
// profile the data directory is empty and every user of it fails closed. Everything fairyfly keeps per user (audit
// trail, logs, mcp.yaml, link state, sessions, `mcp setup` files) derives from it. Up to 2026.10.28 it was
// %LOCALAPPDATA%\fairyfly, which a Microsoft Store (MSIX) fairyfly cannot use on Windows 10: a new folder under AppData
// is redirected into the package's private store (invisible to other programs, deleted on uninstall), and the
// manifest's ExcludedDirectories only works on Windows 11. The user profile is not virtualized on any Windows version.
//
// The move (migrate_data_dir) runs at the start of every fairyfly.exe, before anything opens the data directory.
// A source (an old folder) is "pending" while it exists and carries no kMovedNote; the note is written only after a
// source was merged completely, so a failure at any point leaves it pending and the next start tries again.
//   - target has the marker (kDataDirMarker) and no source is pending: nothing to do (every start after the first).
//     A packaged start whose package copy is still pending (an MSI fairyfly moved %LOCALAPPDATA%\fairyfly first)
//     merges it then.
//   - otherwise, under a lock shared by every fairyfly of this user (the tray and the CLI may start together):
//       nothing pending: the target is created with the marker (a fresh install);
//       the target does not exist: every pending source is copied into a sibling staging directory (kStagingSuffix;
//         a leftover of an interrupted move is discarded first) and the staging directory is renamed to the target
//         (a run that finds the target created meanwhile merges the staging copy into it); the sources are merged
//         once more for what an older fairyfly appended meanwhile;
//       the target exists: the pending sources are merged into it.
//     Merging never overwrites, except a newer link/state.json (the pairing; the older copy is reported):
//       a file the target lacks is copied (temporary file, then published without replacing anything);
//       an audit trail (*.jsonl) gets the complete records it lacks appended, one append-only write per record, so a
//         process appending at the same time loses nothing; when the target's last line is cut off (a record being
//         written right now) the records go to "<name>.migrated.jsonl" instead;
//       any other file that differs keeps the target's copy and is reported as a conflict.
//     Conflicts are recorded in the marker (doctor, link status show them) and printed as warnings.
//   - the old folders are never deleted; each gets kMovedNote pointing to the new location.
// Under MSIX on Windows 10 the old folder may also live in the package's private copy
// (%LOCALAPPDATA%\Packages\<family>\LocalCache\Local\fairyfly); a packaged fairyfly migrates that one too.
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/config/environment.h"

namespace fairyfly::config {

/// In the data directory: JSON with created, migrated_from and conflicts.
inline constexpr const char* kDataDirMarker = ".data-dir.json";
/// In every old folder that was migrated completely: a note for the person who opens it.
inline constexpr const char* kMovedNote = "MOVED-TO-USERPROFILE-.fairyfly.txt";
/// The staging directory next to the target: "<target>.migrating".
inline constexpr const char* kStagingSuffix = ".migrating";
/// The pairing: on a conflict the most recently modified copy wins (never a stale pairing silently).
inline constexpr const char* kPairingFile = "link/state.json";

enum class MigrationOutcome {
    Current,        ///< the data directory has its marker and nothing is pending
    Fresh,          ///< no old folder: created empty
    Migrated,       ///< the old folder(s) copied to a new target
    Merged,         ///< the target existed: pending old folders merged into it
    Overridden,     ///< FAIRYFLY_DATA_DIR is set: no migration
    LockTimeout,    ///< another fairyfly migrates and did not finish in time; nothing touched
    Failed,         ///< see `error`; the old folders stay pending, the next start tries again
};

const char* outcome_name(MigrationOutcome outcome);

/// A relative path that differed between two places; `kept` is the folder whose copy the data directory has now.
struct MigrationConflict {
    std::string path;     ///< relative, generic ("link/state.json")
    std::string kept;     ///< the folder whose copy was kept
    std::string other;    ///< the folder whose copy was not taken (it is still there)
    std::string reason;   ///< "kept the existing copy" | "newer copy taken" | "existing copy is newer"
};

nlohmann::json to_json(const MigrationConflict& conflict);

struct MigrationResult {
    MigrationOutcome outcome = MigrationOutcome::Current;
    std::vector<std::filesystem::path> migrated_from;
    std::vector<MigrationConflict> conflicts;
    std::string error;
    bool ok() const { return outcome != MigrationOutcome::Failed && outcome != MigrationOutcome::LockTimeout; }
};

/// The per-user lock around a migration (a named mutex in the session on Windows, flock on POSIX).
class MigrationLock {
public:
    virtual ~MigrationLock() = default;
    /// Waits up to `timeout`; true when held (also when the previous holder died while holding it).
    virtual bool acquire(std::chrono::milliseconds timeout) = 0;
    virtual void release() = 0;
};

/// The real lock. Windows: the mutex Local\<name>. POSIX: the file <lock_dir>/<name>-<uid>.lock (lock_dir defaults
/// to the temp directory).
std::unique_ptr<MigrationLock> make_migration_lock(const std::string& name = "fairyfly-data-migration",
                                                  const std::filesystem::path& lock_dir = {});

struct MigrationOptions {
    std::chrono::milliseconds lock_timeout{30000};
    /// Test seam: called after the old folders are copied into the staging directory, before it is renamed.
    std::function<void()> after_staging;
    /// Test seam: called after the staging directory was renamed to the target (may throw: a failure there).
    std::function<void()> after_rename;
};

/// Moves the pending `sources` (old data folders; the first one wins conflicts, but the pairing goes to the newest)
/// into `target`. Never throws, never deletes or changes a source except for adding kMovedNote once it is merged.
MigrationResult migrate_data_dir(const std::filesystem::path& target, const std::vector<std::filesystem::path>& sources,
                                 MigrationLock& lock, const MigrationOptions& options = {});

/// The old folders to migrate from: env.legacy_data_dir(), then (when running packaged) `package_local_cache`'s
/// "fairyfly" (the package's private copy of %LOCALAPPDATA%). Empty paths are left out.
std::vector<std::filesystem::path> legacy_data_dirs(const Environment& env,
                                                    const std::optional<std::filesystem::path>& package_local_cache);

/// %LOCALAPPDATA%\Packages\<family>\LocalCache\Local when this process has package identity (MSIX), else nullopt.
std::optional<std::filesystem::path> package_local_cache_dir(const Environment& env);

/// The real user's data directories from the Windows known folders (Profile\.fairyfly, then LocalAppData\fairyfly),
/// whatever the environment says; empty elsewhere. The link's test guard protects the pairings in them.
std::vector<std::filesystem::path> known_user_data_dirs();

/// The conflicts the marker in `data_dir` records (empty when there is no marker or it cannot be read).
std::vector<MigrationConflict> recorded_conflicts(const std::filesystem::path& data_dir);

/// What fairyfly.exe runs at its start: Overridden with FAIRYFLY_DATA_DIR, else migrate_data_dir for this process's
/// environment and package. The result is kept for last_migration().
MigrationResult migrate_process_data_dir();
/// The result of this process's migrate_process_data_dir() (nullopt before it ran).
std::optional<MigrationResult> last_migration();
/// The warning lines for a result (failure, lock timeout, each conflict); empty when there is nothing to say.
std::vector<std::string> migration_warnings(const MigrationResult& result);

/// The pairing file `link` uses by default: <data dir>/link/state.json. When `migration` failed or timed out and only
/// the old folder (env.legacy_data_dir()) has a pairing, that one for this run, so the link does not look unpaired.
/// Empty without a data directory.
std::filesystem::path default_pairing_file(const Environment& env, const std::optional<MigrationResult>& migration);

}  // namespace fairyfly::config
