#pragma once
// SetupService: gathers the Diagnosis from the hosts, drives the consent gate, applies a Plan (in process when
// elevated, otherwise through one elevated child that runs a plan file), verifies with a real round trip and
// writes the report. Every function is host-injected, so the whole flow runs against the in-memory fakes.

#include <functional>
#include <iosfwd>
#include <string>

#include "include/setup/setup_hosts.h"
#include "include/setup/setup_model.h"

namespace fairyfly::setup {

/// Ambient facts of one invocation (kept out of Options so tests can vary them).
struct RunEnv {
    bool stdin_is_tty = false;
    bool read_only = false;                                      ///< FAIRYFLY_READ_ONLY=1 / --read-only
    std::function<bool(const std::string& prompt)> confirm;      ///< answers the [y/N] prompt (only used on a TTY)
    std::ostream* out = nullptr;                                 ///< stdout
    std::ostream* err = nullptr;                                 ///< stderr (errors in text mode)
};

/// Read-only fact gathering (never changes anything). Fills defaults (hostname, port) that validate_options needs
/// first: call fill_defaults + validate_options before diagnose.
Diagnosis diagnose(Hosts& hosts, const Options& options);
TeardownDiagnosis diagnose_teardown(Hosts& hosts, const TeardownOptions& options);
void fill_defaults(Hosts& hosts, Options& options);   ///< default hostname (lower-cased computer DNS name)

/// Full `mcp setup` flow: validate, runbook | diagnose+plan, dry run, nothing-to-do, consent, elevation, apply,
/// verify. Returns the process exit code (0 done/nothing/dry-run/runbook, 1 failure, 2 misuse/refusal).
int run_setup(Hosts& hosts, Options options, const RunEnv& env);
/// `mcp teardown`: manifest-driven removal, idempotent.
int run_teardown(Hosts& hosts, TeardownOptions options, const RunEnv& env);

struct CertExportOptions {
    std::string out_path;                 ///< empty = %LOCALAPPDATA%\fairyfly\fairyfly-mcp-<host>.cer|pem
    CerFormat format = CerFormat::Der;
    std::string hostname;
    int port = 0;
    bool json = false;
};
int run_cert_export(Hosts& hosts, CertExportOptions options, const RunEnv& env);

// ---- elevated child protocol ----------------------------------------------------------------------
/// The plan file: everything the elevated child needs, with the explicit SID computed BEFORE elevation.
nlohmann::json make_elevated_work(const Plan& plan);
/// Executes an elevated work file against `hosts` (in process when already elevated, or as the child). Re-validates
/// every field; never trusts the file. Returns {"ok":bool,"steps":[{id,status,detail}],"thumbprint":"...","error":{...}}.
nlohmann::json execute_elevated_work(const nlohmann::json& work, Hosts& hosts);
/// Entry of `mcp setup --apply-plan FILE --result-file FILE`: reads the plan, executes it, writes the result. Exit code
/// 0 when every step succeeded, 1 otherwise.
int run_apply_plan(Hosts& hosts, const std::string& plan_file, const std::string& result_file);

/// Reads %LOCALAPPDATA%\fairyfly\mcp-setup.json (or `path`). nullopt when absent; `unreadable` set when present but bad.
std::optional<Manifest> load_manifest(SystemProbe& sys, const std::string& path, bool* unreadable = nullptr);
std::string manifest_path_for(SystemProbe& sys);

} // namespace fairyfly::setup
