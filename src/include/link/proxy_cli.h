#pragma once

// `fairyfly proxy show|set|test|reset` and what the tray's "Network / proxy..." window and `fairyfly doctor` show:
// the setting (and whether the administrator's policy locks it), the route in plain words, its trust and the result
// of "Test connection" (include/link/proxy_config.h). The services are the process's real ones (process_services());
// tests pass their own inputs.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/core.h"
#include "include/link/proxy_config.h"

namespace fairyfly::link::proxycfg {

struct Services {
    std::function<Inputs()> inputs;
    AutoProxy autoproxy;
    std::function<bool(const Setting&)> save;  ///< the user's setting (Auto = cleared)
    std::function<void()> changed;             ///< after a save: the process's cached routes are dropped
    std::function<ProbeTargets()> targets;
    /// "Test connection" with `candidate` in place of the saved user setting (nullopt: as saved).
    std::function<ProbeResult(const ProbeTargets&, const std::optional<Setting>& candidate)> probe;
};
/// The process's: install_system_proxy()'s resolver, the user setting in HKCU, the pairing's link address (else
/// FAIRYFLY_LINK_GATEWAY, else https://fairyfly.io). Outside Windows: a resolver set by a test, else empty inputs.
Services process_services(const std::string& state_path = {});

struct CliArgs {
    std::string verb;  ///< show | set | test | reset
    std::optional<std::string> mode, server, bypass, pac;
};
/// The setting the flags describe (set; test with --mode). Error: INVALID_ARGUMENT with the reason.
std::optional<Setting> setting_from_args(const CliArgs& a, std::string& error);

/// What `proxy show` prints (and the base of set/test/reset's data): setting, lock, route, trust, sources.
nlohmann::json show_json(const Inputs& in, const RouteDecision& d, const ProbeTargets& t);

/// The whole verb. set/reset under the administrator's policy: PROXY_SETTING_LOCKED; a failed test:
/// PROXY_TEST_FAILED (data keeps the details).
Result run_cli(const CliArgs& a, Services& s);

/// The text the locked fields show.
inline constexpr const char* kLockedText = "Set by your administrator (HKLM\\SOFTWARE\\Policies\\DataZoo\\fairyfly)";
inline constexpr const char* kUnsupportedText = "Only HTTP proxies (host:port) are supported: no https:// proxy address, no SOCKS.";

}  // namespace fairyfly::link::proxycfg
