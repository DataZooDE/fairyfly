#pragma once
// Is a recorded process still running, and is it still the same process? The link's status file names the process that
// wrote it (pid, executable, start time); `fairyfly link status` and the trays ask this before they believe the file,
// and a tray that finds its single-instance guard held asks it about the holder. Fakes in tests.
#include <cstdint>
#include <optional>
#include <string>

namespace fairyfly::system {

struct ProcessIdentity {
    std::string image;               ///< full path of its executable (UTF-8); empty when it could not be read
    std::int64_t started_unix = 0;   ///< creation time in Unix seconds; 0 when it could not be read
};

class ProcessProbe {
public:
    virtual ~ProcessProbe() = default;
    /// The running process with this id, or nullopt when there is none (it has ended). A process that runs but cannot
    /// be inspected (another user, elevated) comes back with an empty identity.
    virtual std::optional<ProcessIdentity> find(std::uint32_t pid) = 0;
};

/// The operating system's processes.
ProcessProbe& os_process_probe();
/// This process: its executable and start time (read once).
const ProcessIdentity& current_process_identity();

/// The same executable file by Windows rules: case-insensitive (ASCII), '/' and '\' alike, a "\\?\" prefix ignored.
bool same_executable(const std::string& a, const std::string& b);
/// Two readings of one process's start time may differ by rounding.
inline constexpr std::int64_t kStartTimeToleranceSeconds = 2;

}  // namespace fairyfly::system
