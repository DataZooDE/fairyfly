#pragma once
// Pause, resume and status of the desktop link (D10). Portable: the marker and the status file sit next to the
// pairing state, so `fairyfly link pause|resume|status`, the tray and a running `fairyfly link run` (another
// process) agree without any IPC.
//   <state>.paused       exists while the person at the desktop has paused the link
//   <state>.status.json  written by the running link: {"state": connected|paused|stopped, "pid", "updated"}
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace fairyfly::link {

std::filesystem::path pause_marker(const std::string& state_path);
bool is_paused(const std::string& state_path);
/// Creates or removes the marker. False when the file system refused.
bool set_paused(const std::string& state_path, bool paused);

void write_status(const std::string& state_path, const std::string& state);
/// The running link's last status, or null when there is none (or it is unreadable).
nlohmann::json read_status(const std::string& state_path);
/// "connected", "paused" or "stopped" from a fresh status; "not running" when there is none or it is older than
/// kStatusStaleSeconds (the process ended without writing "stopped").
std::string describe(const nlohmann::json& status, std::int64_t now_unix);
inline constexpr std::int64_t kStatusStaleSeconds = 60;

struct SupervisorOptions {
    std::chrono::milliseconds poll{500};            ///< how often the marker is checked
    std::chrono::milliseconds restart_delay{2000};  ///< back-off when the agent ended by itself
    std::chrono::seconds heartbeat{15};             ///< status refresh while nothing changes
    /// Called on the supervising thread when the state changes ("connected" while the agent runs, "paused").
    std::function<void(const std::string& state)> on_change;
    /// Set by anyone, cleared by the supervisor: closes the connection and opens it again at once (the gateway then
    /// receives the current tool catalog, e.g. after "Allow changes in SAP").
    std::atomic<bool>* reconnect = nullptr;
};

/// Runs `agent(net_stop)` (libffl's Agent::run) on a thread while the link is not paused; pausing sets net_stop and
/// waits for the agent to close its connection; resuming starts it again. Returns when `stop` becomes true, after
/// writing the status "stopped".
void supervise(const std::string& state_path, const std::function<void(const std::atomic<bool>&)>& agent,
               const std::atomic<bool>& stop, const SupervisorOptions& options = {});

}  // namespace fairyfly::link
