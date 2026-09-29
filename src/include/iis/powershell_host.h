#pragma once
// Real IisHost: every system change is a CONSTANT PowerShell script run through a PowerShellRunner.
// Parameters travel as JSON in the environment variable FAIRYFLY_IIS_PARAMS; user input is never
// spliced into script text. Files use std::filesystem, the TCP probe a plain socket.
// Nothing in this header runs at start-up; tests use a fake runner (and a fake IisHost).

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/iis/iis_host.h"

namespace fairyfly::iis {

inline constexpr const char* kParamsEnvVar = "FAIRYFLY_IIS_PARAMS";
inline constexpr int kScriptTimeoutMs = 60000;

struct PsResult {
    int exit_code = 0;
    std::string out;       ///< stdout (JSON for query scripts)
    std::string err;       ///< stderr (never contains secrets: no script ever handles one)
    bool timed_out = false;
};

class PowerShellRunner {
public:
    virtual ~PowerShellRunner() = default;
    /// Runs `script` with FAIRYFLY_IIS_PARAMS=params_json. Hard timeout: the process is killed.
    virtual PsResult run(const std::string& script, const std::string& params_json, int timeout_ms) = 0;
};

/// powershell.exe (Windows PowerShell 5.1) via -EncodedCommand.
std::unique_ptr<PowerShellRunner> make_windows_powershell_runner();

/// Named constant scripts, exposed so tests can assert they are free of interpolation.
const std::vector<std::pair<std::string, std::string>>& script_catalog();

std::unique_ptr<IisHost> make_powershell_host(PowerShellRunner& runner);

} // namespace fairyfly::iis
