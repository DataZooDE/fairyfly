#pragma once
// fairyfly's per-user data directory and its one-time move from %LOCALAPPDATA%\fairyfly (docs/DATA_DIR.md).
//
// Where: config::Environment::data_dir() (FAIRYFLY_DATA_DIR > %USERPROFILE%\.fairyfly > $HOME/.fairyfly > temp).
// Everything fairyfly keeps per user (audit trail, logs, mcp.yaml, link state, sessions, `mcp setup` files) derives
// from it. Up to 2026.10.29 it was %LOCALAPPDATA%\fairyfly, which a Microsoft Store (MSIX) fairyfly cannot use on
// Windows 10: a new folder under AppData is redirected into the package's private store (invisible to other programs,
// deleted on uninstall), and the manifest's ExcludedDirectories only works on Windows 11. The user profile is not
// virtualized on any Windows version.
//
// The move (migrate_data_dir) runs once, at the start of every fairyfly.exe, before anything opens the data directory:
//   - target has the marker (kDataDirMarker): nothing to do (every start after the first).
//   - otherwise, under a lock shared by every fairyfly of this user (the tray and the CLI may start together):
//       no old folder: the target is created with the marker (a fresh install);
//       the target does not exist: every old folder is copied into a sibling staging directory (kStagingSuffix; a
//         leftover of an interrupted move is discarded first), the marker is written, and the staging directory is
//         renamed to the target. A run that finds the target created meanwhile merges the staging copy into it;
//       the target exists without the marker: the old folders are merged into it.
//     Merging never overwrites: a file the target lacks is copied (through a temporary file and a rename), an audit
//     trail (*.jsonl) gets the records it lacks appended (no record is lost, none is duplicated, so a rerun after a
//     crash is harmless), any other file that differs keeps the target's copy and is reported as a conflict.
//   - the old folders are never deleted; each gets kMovedNote pointing to the new location, and a folder with that
//     note is not copied again.
// Under MSIX on Windows 10 the old folder may also live in the package's private copy
// (%LOCALAPPDATA%\Packages\<family>\LocalCache\Local\fairyfly); a packaged fairyfly migrates that one too.
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "include/config/environment.h"

namespace fairyfly::config {

/// In the data directory: written when it is created or migrated (JSON: created, migrated_from).
inline constexpr const char* kDataDirMarker = ".data-dir.json";
/// In every old folder that was migrated: a note for the person who opens it.
inline constexpr const char* kMovedNote = "MOVED-TO-USERPROFILE-.fairyfly.txt";
/// The staging directory next to the target: "<target>.migrating".
inline constexpr const char* kStagingSuffix = ".migrating";

enum class MigrationOutcome {
    Current,        ///< the data directory has its marker already
    Fresh,          ///< no old folder: created empty
    Migrated,       ///< the old folder(s) copied to the target
    Merged,         ///< the target existed (without the marker): the old folders merged into it, nothing overwritten
    Overridden,     ///< FAIRYFLY_DATA_DIR is set: no migration
    LockTimeout,    ///< another fairyfly migrates and did not finish in time; nothing touched
    Failed,         ///< see `error`; the old folders are untouched, the next start tries again
};

const char* outcome_name(MigrationOutcome outcome);

struct MigrationResult {
    MigrationOutcome outcome = MigrationOutcome::Current;
    std::vector<std::filesystem::path> migrated_from;
    /// Relative paths that exist in more than one place with different contents (the first copy was kept).
    std::vector<std::string> conflicts;
    std::string error;
    bool ok() const { return outcome != MigrationOutcome::Failed && outcome != MigrationOutcome::LockTimeout; }
};

/// The per-user lock around a migration (a named mutex on Windows, flock on POSIX).
class MigrationLock {
public:
    virtual ~MigrationLock() = default;
    /// Waits up to `timeout`; true when held (also when the previous holder died while holding it).
    virtual bool acquire(std::chrono::milliseconds timeout) = 0;
    virtual void release() = 0;
};

/// The real lock. `name` is the mutex (Windows, Local\ namespace) or the lock file in the temp directory (POSIX).
std::unique_ptr<MigrationLock> make_migration_lock(const std::string& name = "fairyfly-data-migration");

struct MigrationOptions {
    std::chrono::milliseconds lock_timeout{30000};
    /// Test seam: called after the old folders are copied into the staging directory, before it is renamed.
    std::function<void()> after_staging;
};

/// Moves `sources` (old data folders, the first one wins conflicts) into `target`. Never throws, never deletes or
/// changes a source except for adding kMovedNote.
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

/// What fairyfly.exe runs at its start: Overridden with FAIRYFLY_DATA_DIR, else migrate_data_dir for this process's
/// environment and package. The result is kept for last_migration().
MigrationResult migrate_process_data_dir();
/// The result of this process's migrate_process_data_dir() (nullopt before it ran).
std::optional<MigrationResult> last_migration();

}  // namespace fairyfly::config
