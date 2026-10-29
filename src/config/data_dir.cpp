// fairyfly's data directory and its one-time move from %LOCALAPPDATA%\fairyfly (include/config/data_dir.h).
#include "include/config/data_dir.h"
#include "include/text/utf8.h"
#include "include/version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
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

/// Temporary files of an atomic write: "<file>.migrating-tmp", never copied themselves.
constexpr const char* kTempSuffix = ".migrating-tmp";

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw fs::filesystem_error("cannot read", p, std::make_error_code(std::errc::io_error));
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Writes through a temporary file and a rename, so `p` is either absent, the old file, or the whole new one.
void write_file_atomic(const fs::path& p, const std::string& data) {
    const fs::path tmp = fs::path(p.native() + text::utf8_path(kTempSuffix).native());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) throw fs::filesystem_error("cannot write", tmp, std::make_error_code(std::errc::io_error));
    }
    fs::rename(tmp, p);
}

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

void note_conflict(MigrationResult& r, const std::string& rel) {
    if (std::find(r.conflicts.begin(), r.conflicts.end(), rel) == r.conflicts.end()) r.conflicts.push_back(rel);
}

/// Adds to `dest` the records of `src` it lacks (as a multiset: a record twice in `src` and once in `dest` is added
/// once), appending: a writer that appends to `dest` meanwhile loses nothing.
void merge_audit_file(const fs::path& src, const fs::path& dest) {
    const std::string existing = read_file(dest);
    std::unordered_map<std::string, int> have;
    for (auto& line : complete_lines(existing)) ++have[line];
    std::string add;
    for (auto& line : complete_lines(read_file(src))) {
        auto it = have.find(line);
        if (it != have.end() && it->second > 0) --it->second;
        else add += line;
    }
    if (add.empty()) return;
    if (!existing.empty() && existing.back() != '\n') add.insert(add.begin(), '\n');   // never glue onto a cut-off line
    std::ofstream out(dest, std::ios::binary | std::ios::app);
    out.write(add.data(), static_cast<std::streamsize>(add.size()));
    out.flush();
    if (!out) throw fs::filesystem_error("cannot append", dest, std::make_error_code(std::errc::io_error));
}

/// Copies what `into` lacks from `from`; never overwrites (see data_dir.h). Throws on I/O errors.
void merge_tree(const fs::path& from, const fs::path& into, MigrationResult& r) {
    fs::create_directories(into);
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        const fs::path rel = fs::relative(it->path(), from);
        const std::string name = text::path_utf8(rel.filename());
        if (rel == fs::path(kMovedNote) || rel == fs::path(kDataDirMarker)) continue;
        if (name.size() > std::string(kTempSuffix).size() && name.rfind(kTempSuffix) == name.size() - std::string(kTempSuffix).size())
            continue;
        const fs::path dest = into / rel;
        if (it->is_directory()) {
            fs::create_directories(dest);
            continue;
        }
        if (!it->is_regular_file()) continue;
        if (!fs::exists(dest)) {
            if (is_audit_trail(rel)) {
                std::string records;
                for (auto& line : complete_lines(read_file(it->path()))) records += line;
                write_file_atomic(dest, records);
            } else {
                const fs::path tmp = fs::path(dest.native() + text::utf8_path(kTempSuffix).native());
                fs::copy_file(it->path(), tmp, fs::copy_options::overwrite_existing);
                fs::rename(tmp, dest);
            }
        } else if (is_audit_trail(rel)) {
            merge_audit_file(it->path(), dest);
        } else if (read_file(it->path()) != read_file(dest)) {
            note_conflict(r, rel.generic_string());   // the first copy stays
        }
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

void write_marker(const fs::path& dir, const std::vector<fs::path>& from) {
    nlohmann::json j;
    j["fairyfly"] = FAIRYFLY_VERSION;
    j["created"] = utc_now();
    j["migrated_from"] = nlohmann::json::array();
    for (const auto& f : from) j["migrated_from"].push_back(text::path_utf8(f));
    write_file_atomic(dir / kDataDirMarker, j.dump(2) + "\n");
}

void write_moved_note(const fs::path& old_dir, const fs::path& target) {
    const std::string to = text::path_utf8(target);
    const std::string note =
        "fairyfly now keeps its data (audit trail, logs, mcp.yaml, cloud link pairing, sessions) in\r\n\r\n    " + to +
        "\r\n\r\nEverything in this folder was copied there on " + utc_now() + " by fairyfly " +
        FAIRYFLY_VERSION + ".\r\nThis folder is no longer used and fairyfly never deletes it. Delete it yourself once you\r\n"
        "have checked the new folder.\r\n";
    try {
        write_file_atomic(old_dir / kMovedNote, note);
    } catch (const std::exception&) {
        // best effort: the marker in the new folder already stops a second migration
    }
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
    explicit FlockMigrationLock(const std::string& name) {
        std::error_code ec;
        const fs::path p = fs::temp_directory_path(ec) / (name + ".lock");
        if (!ec) fd_ = ::open(p.c_str(), O_CREAT | O_RDWR, 0600);
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

std::unique_ptr<MigrationLock> make_migration_lock(const std::string& name) {
#ifdef _WIN32
    return std::make_unique<Win32MigrationLock>(name);
#else
    return std::make_unique<FlockMigrationLock>(name);
#endif
}

MigrationResult migrate_data_dir(const fs::path& target, const std::vector<fs::path>& sources, MigrationLock& lock,
                                 const MigrationOptions& options) {
    MigrationResult r;
    std::error_code ec;
    if (fs::exists(target / kDataDirMarker, ec)) return r;   // Current: every start after the first
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
        if (fs::exists(target / kDataDirMarker, ec)) return r;   // the one that held the lock did it

        std::vector<fs::path> live;
        for (const auto& s : sources) {
            if (s.empty() || !fs::is_directory(s, ec) || fs::exists(s / kMovedNote, ec)) continue;
            if (fs::equivalent(s, target, ec)) continue;
            live.push_back(s);
        }
        if (live.empty()) {
            fs::create_directories(target);
            write_marker(target, {});
            r.outcome = MigrationOutcome::Fresh;
            return r;
        }

        if (!fs::exists(target)) {
            const fs::path staging = fs::path(target.native() + text::utf8_path(kStagingSuffix).native());
            fs::remove_all(staging);   // an interrupted earlier move: start over (its sources are untouched)
            fs::create_directories(staging);
            for (const auto& s : live) merge_tree(s, staging, r);
            if (options.after_staging) options.after_staging();
            write_marker(staging, live);
            fs::rename(staging, target, ec);
            if (ec) {
                if (!fs::exists(target)) throw fs::filesystem_error("cannot rename", staging, target, ec);
                // a fairyfly that gave up waiting for the lock created it meanwhile: merge, never overwrite
                merge_tree(staging, target, r);
                write_marker(target, live);
                fs::remove_all(staging, ec);
            }
            // what an older fairyfly appended to the old folder while it was copied (merging is idempotent)
            for (const auto& s : live) merge_tree(s, target, r);
            r.outcome = MigrationOutcome::Migrated;
        } else {
            for (const auto& s : live) merge_tree(s, target, r);
            write_marker(target, live);
            r.outcome = MigrationOutcome::Merged;
        }
        r.migrated_from = live;
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
    const fs::path local = env.local_app_data();
    if (family.empty() || env.get(env::kLocalAppData).empty()) return std::nullopt;
    return local / L"Packages" / family / L"LocalCache" / L"Local";
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

}  // namespace fairyfly::config
