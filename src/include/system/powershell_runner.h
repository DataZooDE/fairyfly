#pragma once
// Generic Windows PowerShell 5.1 runner. Scripts are CONSTANT text sent with -EncodedCommand; parameters
// travel as JSON in the environment variable FAIRYFLY_PS_PARAMS, never spliced into script text. The child
// is killed on a hard timeout. Tests use a fake PowerShellRunner; nothing here runs at start-up.

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace fairyfly::sys {

inline constexpr const char* kParamsEnvVar = "FAIRYFLY_PS_PARAMS";
inline constexpr int kScriptTimeoutMs = 60000;

/// Script prologue: fail-fast trap (message to stderr, exit 1), quiet progress, `$p` = parsed
/// $env:FAIRYFLY_PS_PARAMS, and `Out-Json` (compact JSON to stdout). Prepend it to every script.
inline constexpr const char kPsPreamble[] = R"PS(trap { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$p = $null
if ($env:FAIRYFLY_PS_PARAMS) { $p = $env:FAIRYFLY_PS_PARAMS | ConvertFrom-Json }
function Out-Json($o) { [Console]::Out.Write(($o | ConvertTo-Json -Compress -Depth 8)) }
)PS";

/// Optional addition after kPsPreamble: sets `$elevated` (Administrators role active in the current token).
inline constexpr const char kPsElevationCheck[] = R"PS($id = [Security.Principal.WindowsIdentity]::GetCurrent()
$elevated = ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
)PS";

struct PsResult {
    int exit_code = 0;
    std::string out;       ///< stdout (JSON for query scripts)
    std::string err;       ///< stderr (never contains secrets: no script ever handles one)
    bool timed_out = false;
};

class PowerShellRunner {
public:
    virtual ~PowerShellRunner() = default;
    /// Runs `script` with FAIRYFLY_PS_PARAMS=params_json. Hard timeout: the process is killed.
    /// Throws PowerShellError{"PS_SCRIPT_FAILED"} when powershell.exe cannot be started.
    virtual PsResult run(const std::string& script, const std::string& params_json, int timeout_ms) = 0;
};

/// Thrown when a script could not run or failed; the message never contains secrets.
struct PowerShellError {
    std::string code;     ///< PS_SCRIPT_FAILED, PS_SCRIPT_TIMEOUT
    std::string message;
};

/// powershell.exe (Windows PowerShell 5.1) via -EncodedCommand.
std::unique_ptr<PowerShellRunner> make_windows_powershell_runner();

/// Runs a script whose result is one JSON object. Maps a timeout, a non-zero exit code and non-JSON output to
/// PowerShellError (first line of stderr only, at most 300 characters).
nlohmann::json run_json_script(PowerShellRunner& runner, const std::string& script, const std::string& params_json,
                               int timeout_ms = kScriptTimeoutMs);

} // namespace fairyfly::sys
