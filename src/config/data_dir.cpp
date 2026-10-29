// fairyfly's data directory and its move from %LOCALAPPDATA%\fairyfly (include/config/data_dir.h).
#include "include/config/data_dir.h"
#include "include/text/utf8.h"
#include "include/version.h"

#include <algorithm>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fairyfly::config {

namespace fs = std::filesystem;

namespace {

/// Temporary files of a copy: "<file>.migrating-tmp", never copied themselves.
constexpr const char* kTempSuffix = ".migrating-tmp";
/// Where audit records go when the target file's last line is cut off: "<stem>.migrated.jsonl".
constexpr const char* kPartSuffix = ".migrated.jsonl";

fs::path with_suffix(const fs::path& p, const char* suffix) { return fs::path(p.native() + text::utf8_path(suffix).native()); }

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw fs::filesystem_error("cannot read", p, std::make_error_code(std::errc::io_error));
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Writes through a temporary file and a rename, so `p` is either the old file or the whole new one. Only for files
/// nobody else writes (the marker, the note, a file in the private staging directory).
void write_file_atomic(const fs::path& p, const std::string& data) {
    const fs::path tmp = with_suffix(p, kTempSuffix);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) throw fs::filesystem_error("cannot write", tmp, std::make_error_code(std::errc::io_error));
    }
    fs::rename(tmp, p);
}

/// Appends whole records to `p` (created when missing), one append-only write each: the system puts every write at
/// the current end, so records another process appends at the same time are neither overwritten nor split.
class RecordAppender {
public:
    explicit RecordAppender(const fs::path& p) : path_(p) {
#ifdef _WIN32
        handle_ = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw fs::filesystem_error("cannot append", p, std::error_code(static_cast<int>(GetLastError()), std::system_category()));
#else
        fd_ = ::open(p.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
        if (fd_ < 0) throw fs::filesystem_error("cannot append", p, std::error_code(errno, std::generic_category()));
#endif
    }
    ~RecordAppender() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    RecordAppender(const RecordAppender&) = delete;
    RecordAppender& operator=(const RecordAppender&) = delete;

    void append(const std::string& record) {
#ifdef _WIN32
        DWORD written = 0;
        if (!WriteFile(handle_, record.data(), static_cast<DWORD>(record.size()), &written, nullptr) || written != record.size())
            throw fs::filesystem_error("cannot append", path_, std::error_code(static_cast<int>(GetLastError()), std::system_category()));
#else
        const auto n = ::write(fd_, record.data(), record.size());
        if (n < 0 || static_cast<std::size_t>(n) != record.size())
            throw fs::filesystem_error("cannot append", path_, std::error_code(errno, std::generic_category()));
#endif
    }

private:
    fs::path path_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

/// The audit trail's monthly files: append-only JSON lines, merged record by record.
bool is_audit_trail(const fs::path& rel) { return rel.extension() == ".jsonl"; }

/// The complete lines of `text`, each with its '\n'; a last line without one (a record still being written) is left
/// out.
std::vector<std::string> complete_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t nl = text.find('\n'); nl != std::string::npos; nl = text.find('\n', start)) {
        lines.push_back(text.substr(start, nl + 1 - start));
        start = nl + 1;
    }
    return lines;
}

/// Copies `src` to `dest` (and its modification time) without ever replacing an existing `dest`: through a
/// temporary file published by a hard link (fails when `dest` exists; a rename where links are not supported).
/// False when `dest` exists already.
bool publish_copy(const fs::path& src, const fs::path& dest) {
    const fs::path tmp = with_suffix(dest, kTempSuffix);
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing);
    fs::last_write_time(tmp, fs::last_write_time(src));
    std::error_code ec;
    fs::create_hard_link(tmp, dest, ec);
    if (!ec) {
        fs::remove(tmp, ec);
        return true;
    }
    if (fs::exists(dest)) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, dest);   // no hard links on this volume
    return true;
}

/// Replaces `dest` by a copy of `src` (the newer pairing).
void replace_copy(const fs::path& src, const fs::path& dest) {
    const fs::path tmp = with_suffix(dest, kTempSuffix);
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing);
    fs::last_write_time(tmp, fs::last_write_time(src));
    fs::rename(tmp, dest);
}

void note_conflict(MigrationResult& r, MigrationConflict c) {
    for (auto& known : r.conflicts)
        if (known.path == c.path) return;   // the first decision about a path is the one that counts
    r.conflicts.push_back(std::move(c));
}

/// Adds to `into/rel` the complete records of `src` it lacks (as a multiset: a record twice in `src` and once in the
/// target is added once). Records already in "<stem>.migrated.jsonl" count as present. When the target's last line is
/// cut off, the records go to that part file instead: fairyfly never writes after a line it does not own.
void merge_audit_file(const fs::path& src, const fs::path& dest) {
    const fs::path part = dest.parent_path() / (text::path_utf8(dest.stem()) + kPartSuffix);
    std::string existing = fs::exists(dest) ? read_file(dest) : std::string();
    const std::string existing_part = fs::exists(part) ? read_file(part) : std::string();
    std::unordered_map<std::string, int> have;
    for (auto& line : complete_lines(existing)) ++have[line];
    for (auto& line : complete_lines(existing_part)) ++have[line];
    std::vector<std::string> add;
    for (auto& line : complete_lines(read_file(src))) {
        auto it = have.find(line);
        if (it != have.end() && it->second > 0) --it->second;
        else add.push_back(std::move(line));
    }
    if (add.empty()) return;
    fs::path into = dest;
    if (!existing.empty() && existing.back() != '\n') {
        into = part;
        if (!existing_part.empty() && existing_part.back() != '\n')
            throw fs::filesystem_error("both the audit file and its .migrated part end in a cut-off line", dest, part,
                                       std::make_error_code(std::errc::operation_in_progress));
    }
    RecordAppender out(into);
    for (const auto& record : add) out.append(record);
}

/// Copies what `into` lacks from `from`; never overwrites but a newer pairing (see data_dir.h). Throws on I/O errors.
void merge_tree(const fs::path& from, const fs::path& into, MigrationResult& r, const fs::path& kept_from) {
    fs::create_directories(into);
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        const fs::path rel = fs::relative(it->path(), from);
        const std::string name = text::path_utf8(rel.filename());
        if (rel == fs::path(kMovedNote) || rel == fs::path(kDataDirMarker) || ends_with(name, kTempSuffix)) continue;
        const fs::path dest = into / rel;
        if (it->is_directory()) {
            fs::create_directories(dest);
            continue;
        }
        if (!it->is_regular_file()) continue;
        if (is_audit_trail(rel)) {
            merge_audit_file(it->path(), dest);
            continue;
        }
        if (publish_copy(it->path(), dest)) continue;   // the target lacked it
        if (read_file(it->path()) == read_file(dest)) continue;
        const std::string relative = rel.generic_string();
        const std::string here = text::path_utf8(from), there = text::path_utf8(kept_from);
        if (relative == kPairingFile) {
            // never a stale pairing silently: the most recently modified one
            if (fs::last_write_time(it->path()) > fs::last_write_time(dest)) {
                replace_copy(it->path(), dest);
                note_conflict(r, {relative, here, there, "newer copy taken"});
            } else {
                note_conflict(r, {relative, there, here, "existing copy is newer"});
            }
            continue;
        }
        note_conflict(r, {relative, there, here, "kept the existing copy"});
    }
}

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

nlohmann::json read_marker(const fs::path& dir) {
    try {
        const auto j = nlohmann::json::parse(read_file(dir / kDataDirMarker));
        if (j.is_object()) return j;
    } catch (const std::exception&) {
    }
    return nlohmann::json::object();
}

/// Creates or extends the marker: when it was created, every folder merged into it and every conflict.
void write_marker(const fs::path& dir, const MigrationResult& r) {
    nlohmann::json j = read_marker(dir);
    if (!j.contains("created")) j["created"] = utc_now();
    if (!j.contains("fairyfly")) j["fairyfly"] = FAIRYFLY_VERSION;
    if (!j["migrated_from"].is_array()) j["migrated_from"] = nlohmann::json::array();
    for (const auto& f : r.migrated_from)
        j["migrated_from"].push_back({{"path", text::path_utf8(f)}, {"at", utc_now()}, {"by", FAIRYFLY_VERSION}});
    if (!j["conflicts"].is_array()) j["conflicts"] = nlohmann::json::array();
    for (const auto& c : r.conflicts) j["conflicts"].push_back(to_json(c));
    write_file_atomic(dir / kDataDirMarker, j.dump(2) + "\n");
}

void write_moved_note(const fs::path& old_dir, const fs::path& target) {
    const std::string to = text::path_utf8(target);
    const std::string note =
        "fairyfly now keeps its data (audit trail, logs, mcp.yaml, cloud link pairing, sessions) in\r\n\r\n    " + to +
        "\r\n\r\nEverything in this folder was copied there on " + utc_now() + " by fairyfly " + FAIRYFLY_VERSION +
        ".\r\nThis folder is no longer used and fairyfly never deletes it. Records an older fairyfly writes here from now\r\n"
        "on are not copied. Delete the folder yourself once you have checked the new one.\r\n";
    write_file_atomic(old_dir / kMovedNote, note);
}

/// The sources still to merge: existing folders without the note (and not the target itself).
std::vector<fs::path> pending_sources(const fs::path& target, const std::vector<fs::path>& sources) {
    std::vector<fs::path> live;
    std::error_code ec;
    for (const auto& s : sources) {
        if (s.empty() || !fs::is_directory(s, ec) || fs::exists(s / kMovedNote, ec)) continue;
        if (fs::equivalent(s, target, ec)) continue;
        if (std::find(live.begin(), live.end(), s) == live.end()) live.push_back(s);
    }
    return live;
}

std::mutex& stored_mutex() {
    static std::mutex m;
    return m;
}

std::optional<MigrationResult>& stored() {
    static std::optional<MigrationResult> result;
    return result;
}

#ifdef _WIN32
class Win32MigrationLock : public MigrationLock {
public:
    explicit Win32MigrationLock(const std::string& name)
        : handle_(CreateMutexW(nullptr, FALSE, (L"Local\\" + text::utf8_to_wide(name)).c_str())) {}
    ~Win32MigrationLock() override {
        release();
        if (handle_) CloseHandle(handle_);
    }
    bool acquire(std::chrono::milliseconds timeout) override {
        if (!handle_) return false;
        const DWORD wait = WaitForSingleObject(handle_, static_cast<DWORD>(timeout.count()));
        // WAIT_ABANDONED: the previous holder ended mid-migration; the staging directory it left is discarded
        held_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        return held_;
    }
    void release() override {
        if (held_ && handle_) ReleaseMutex(handle_);
        held_ = false;
    }

private:
    HANDLE handle_ = nullptr;
    bool held_ = false;
};
#else
class FlockMigrationLock : public MigrationLock {
public:
    FlockMigrationLock(const std::string& name, const fs::path& lock_dir) {
        std::error_code ec;
        const fs::path dir = lock_dir.empty() ? fs::temp_directory_path(ec) : lock_dir;
        // per user: another user's lock file in a shared /tmp neither blocks nor is opened
        if (!ec) fd_ = ::open((dir / (name + "-" + std::to_string(::getuid()) + ".lock")).c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    }
    ~FlockMigrationLock() override {
        release();
        if (fd_ >= 0) ::close(fd_);
    }
    bool acquire(std::chrono::milliseconds timeout) override {
        if (fd_ < 0) return false;
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (std::chrono::steady_clock::now() >= until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        held_ = true;
        return true;
    }
    void release() override {
        if (held_ && fd_ >= 0) ::flock(fd_, LOCK_UN);
        held_ = false;
    }

private:
    int fd_ = -1;
    bool held_ = false;
};
#endif

}  // namespace

const char* outcome_name(MigrationOutcome outcome) {
    switch (outcome) {
    case MigrationOutcome::Current: return "current";
    case MigrationOutcome::Fresh: return "fresh";
    case MigrationOutcome::Migrated: return "migrated";
    case MigrationOutcome::Merged: return "merged";
    case MigrationOutcome::Overridden: return "overridden";
    case MigrationOutcome::LockTimeout: return "lock_timeout";
    case MigrationOutcome::Failed: return "failed";
    }
    return "unknown";
}

nlohmann::json to_json(const MigrationConflict& c) {
    return {{"path", c.path}, {"kept", c.kept}, {"other", c.other}, {"reason", c.reason}};
}

std::unique_ptr<MigrationLock> make_migration_lock(const std::string& name, const fs::path& lock_dir) {
#ifdef _WIN32
    (void)lock_dir;
    return std::make_unique<Win32MigrationLock>(name);
#else
    return std::make_unique<FlockMigrationLock>(name, lock_dir);
#endif
}

MigrationResult migrate_data_dir(const fs::path& target, const std::vector<fs::path>& sources, MigrationLock& lock,
                                 const MigrationOptions& options) {
    MigrationResult r;
    if (target.empty()) {
        r.outcome = MigrationOutcome::Failed;
        r.error = "no data directory: neither FAIRYFLY_DATA_DIR nor USERPROFILE (or HOME) names one";
        return r;
    }
    std::error_code ec;
    if (fs::exists(target / kDataDirMarker, ec) && pending_sources(target, sources).empty()) return r;   // Current
    if (!lock.acquire(options.lock_timeout)) {
        r.outcome = MigrationOutcome::LockTimeout;
        r.error = "another fairyfly is moving the data directory to " + text::path_utf8(target);
        return r;
    }
    struct Release {
        MigrationLock& lock;
        ~Release() { lock.release(); }
    } release{lock};
    try {
        const bool marked = fs::exists(target / kDataDirMarker);
        const std::vector<fs::path> live = pending_sources(target, sources);
        if (marked && live.empty()) return r;   // the one that held the lock did it
        if (live.empty()) {
            fs::create_directories(target);
            write_marker(target, r);
            r.outcome = MigrationOutcome::Fresh;
            return r;
        }

        if (!fs::exists(target)) {
            const fs::path staging = with_suffix(target, kStagingSuffix);
            fs::remove_all(staging);   // an interrupted earlier move: start over (its sources are untouched)
            fs::create_directories(staging);
            for (const auto& s : live) merge_tree(s, staging, r, live.front());
            if (options.after_staging) options.after_staging();
            fs::rename(staging, target, ec);
            if (ec) {
                if (!fs::exists(target)) throw fs::filesystem_error("cannot rename", staging, target, ec);
                // a fairyfly that gave up waiting for the lock created it meanwhile: merge, never overwrite
                merge_tree(staging, target, r, target);
                fs::remove_all(staging, ec);
            }
            if (options.after_rename) options.after_rename();
            // what an older fairyfly appended to the old folders while they were copied (merging is idempotent)
            for (const auto& s : live) merge_tree(s, target, r, target);
            r.outcome = MigrationOutcome::Migrated;
        } else {
            for (const auto& s : live) merge_tree(s, target, r, target);
            r.outcome = MigrationOutcome::Merged;
        }
        r.migrated_from = live;
        write_marker(target, r);
        // only now: a source without its note stays pending, so any failure above is retried at the next start
        for (const auto& s : live) write_moved_note(s, target);
    } catch (const std::exception& e) {
        r.outcome = MigrationOutcome::Failed;
        r.migrated_from.clear();
        r.error = "cannot move fairyfly's data to " + text::path_utf8(target) + ": " + e.what();
    }
    return r;
}

std::vector<fs::path> legacy_data_dirs(const Environment& env, const std::optional<fs::path>& package_local_cache) {
    std::vector<fs::path> dirs;
    if (auto legacy = env.legacy_data_dir(); !legacy.empty()) dirs.push_back(std::move(legacy));
    if (package_local_cache && !package_local_cache->empty()) dirs.push_back(*package_local_cache / "fairyfly");
    return dirs;
}

std::optional<fs::path> package_local_cache_dir(const Environment& env) {
#ifdef _WIN32
    UINT32 length = 0;
    if (GetCurrentPackageFamilyName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0) return std::nullopt;
    std::wstring family(length, L'\0');
    if (GetCurrentPackageFamilyName(&length, family.data()) != ERROR_SUCCESS) return std::nullopt;
    family.resize(length > 0 ? length - 1 : 0);   // without the terminating null
    const std::string local = env.get(env::kLocalAppData);
    if (family.empty() || local.empty()) return std::nullopt;
    return text::utf8_path(local) / L"Packages" / family / L"LocalCache" / L"Local";
#else
    (void)env;
    return std::nullopt;
#endif
}

std::vector<fs::path> known_user_data_dirs() {
    std::vector<fs::path> dirs;
#ifdef _WIN32
    auto known = [](REFKNOWNFOLDERID id) -> fs::path {
        PWSTR path = nullptr;
        fs::path out;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path) out = path;
        if (path) CoTaskMemFree(path);
        return out;
    };
    if (auto profile = known(FOLDERID_Profile); !profile.empty()) dirs.push_back(profile / ".fairyfly");
    if (auto local = known(FOLDERID_LocalAppData); !local.empty()) dirs.push_back(local / "fairyfly");
#endif
    return dirs;
}

std::vector<MigrationConflict> recorded_conflicts(const fs::path& data_dir) {
    std::vector<MigrationConflict> out;
    if (data_dir.empty()) return out;
    const auto j = read_marker(data_dir);
    if (!j.contains("conflicts") || !j["conflicts"].is_array()) return out;
    for (const auto& c : j["conflicts"])
        if (c.is_object())
            out.push_back({c.value("path", ""), c.value("kept", ""), c.value("other", ""), c.value("reason", "")});
    return out;
}

std::vector<std::string> migration_warnings(const MigrationResult& r) {
    std::vector<std::string> out;
    if (!r.ok()) out.push_back(r.error);
    for (const auto& c : r.conflicts)
        out.push_back("data directory: " + c.path + " differed; " + c.reason + " (kept " + c.kept + ", the other copy stays in " +
                      c.other + ")");
    return out;
}

MigrationResult migrate_process_data_dir() {
    const Environment& env = Environment::process();
    MigrationResult r;
    if (env.data_dir_overridden()) {
        r.outcome = MigrationOutcome::Overridden;
    } else {
        auto lock = make_migration_lock();
        r = migrate_data_dir(env.data_dir(), legacy_data_dirs(env, package_local_cache_dir(env)), *lock);
    }
    std::lock_guard<std::mutex> guard(stored_mutex());
    stored() = r;
    return r;
}

std::optional<MigrationResult> last_migration() {
    std::lock_guard<std::mutex> guard(stored_mutex());
    return stored();
}

fs::path default_pairing_file(const Environment& env, const std::optional<MigrationResult>& migration) {
    const fs::path data = env.data_dir();
    if (data.empty()) return {};
    const fs::path pairing = data / "link" / "state.json";
    if (!migration || migration->ok()) return pairing;
    std::error_code ec;
    if (fs::exists(pairing, ec)) return pairing;
    if (const fs::path legacy = env.legacy_data_dir(); !legacy.empty() && fs::exists(legacy / "link" / "state.json", ec))
        return legacy / "link" / "state.json";   // this run only: the move is retried at the next start
    return pairing;
}

}  // namespace fairyfly::config
