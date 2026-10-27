#pragma once
// The states of the cloud link as the status file, `link status` and both trays spell them.

namespace fairyfly::link::state {

inline constexpr const char* kConnected = "connected";   ///< the agent runs (or is reconnecting)
inline constexpr const char* kPaused = "paused";         ///< `link pause`: no connection until `link resume`
inline constexpr const char* kStopped = "stopped";       ///< written by the link when it ends
inline constexpr const char* kNotRunning = "not running";  ///< describe(): no fresh status (none, stale or stopped)

}  // namespace fairyfly::link::state
