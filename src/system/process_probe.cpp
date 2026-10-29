#include "include/system/process_probe.h"

#include <cctype>

#ifdef _WIN32
#include <windows.h>

#include "include/com/utf8.h"
#include "include/system/unique_handle.h"
#else
#include <cerrno>
#include <climits>
#include <csignal>
#include <unistd.h>
#endif

namespace fairyfly::system {

namespace {

std::string normalised_path(const std::string& path) {
    std::string s = path;
    if (s.rfind("\\\\?\\", 0) == 0 || s.rfind("//?/", 0) == 0) s.erase(0, 4);
    for (auto& c : s) {
        if (c == '/') c = '\\';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

#ifdef _WIN32
std::int64_t filetime_to_unix(const FILETIME& ft) {
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    constexpr std::uint64_t kEpochDifference = 116444736000000000ULL;   // 1601-01-01 to 1970-01-01 in 100 ns
    if (t.QuadPart < kEpochDifference) return 0;
    return static_cast<std::int64_t>((t.QuadPart - kEpochDifference) / 10000000ULL);
}

ProcessIdentity identity_of(HANDLE process) {
    ProcessIdentity id;
    wchar_t buffer[32768];
    DWORD size = ARRAYSIZE(buffer);
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        try {
            id.image = com::wide_to_utf8(std::wstring_view(buffer, size));
        } catch (...) {
            id.image.clear();
        }
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) id.started_unix = filetime_to_unix(created);
    return id;
}

class Win32ProcessProbe : public ProcessProbe {
public:
    std::optional<ProcessIdentity> find(std::uint32_t pid) override {
        if (pid == 0) return std::nullopt;
        const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process) {
            // ERROR_INVALID_PARAMETER: no process with this id. Access denied: it runs, but not for us to inspect.
            if (GetLastError() == ERROR_ACCESS_DENIED) return ProcessIdentity{};
            return std::nullopt;
        }
        // a process that has ended keeps its id while anyone holds a handle to it
        DWORD code = 0;
        if (GetExitCodeProcess(process.get(), &code) && code != STILL_ACTIVE) return std::nullopt;
        return identity_of(process.get());
    }
};
#else
class PosixProcessProbe : public ProcessProbe {
public:
    std::optional<ProcessIdentity> find(std::uint32_t pid) override {
        if (pid == 0 || pid > static_cast<std::uint32_t>(INT_MAX)) return std::nullopt;
        if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM) return std::nullopt;
        ProcessIdentity id;
        char buffer[4096];
        const std::string link = "/proc/" + std::to_string(pid) + "/exe";
        const auto n = ::readlink(link.c_str(), buffer, sizeof(buffer) - 1);
        if (n > 0) id.image.assign(buffer, static_cast<size_t>(n));
        return id;   // the start time is only read on Windows
    }
};
#endif

}  // namespace

ProcessProbe& os_process_probe() {
#ifdef _WIN32
    static Win32ProcessProbe probe;
#else
    static PosixProcessProbe probe;
#endif
    return probe;
}

const ProcessIdentity& current_process_identity() {
    static const ProcessIdentity identity = [] {
#ifdef _WIN32
        return identity_of(GetCurrentProcess());
#else
        return os_process_probe().find(static_cast<std::uint32_t>(::getpid())).value_or(ProcessIdentity{});
#endif
    }();
    return identity;
}

bool same_executable(const std::string& a, const std::string& b) { return normalised_path(a) == normalised_path(b); }

}  // namespace fairyfly::system
