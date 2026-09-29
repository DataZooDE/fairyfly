# SAP GUI integration scripts

The retained Python SM59 runner and PowerShell SU01 script exercise live SAP GUI behavior. They require Windows, SAP GUI with client and server scripting enabled, and a suitable SAP test session. They are not registered with CTest. Read a script before running it: both can navigate transactions or change the active screen.

Build the CLI first:

~~~powershell
cmake --build build --config Release --target fairyfly --parallel
~~~

The scripts use the staged executable at `build/bin/Release/fairyfly.exe`. The direct Visual Studio output had a file-specific attachment failure on this machine; refreshing the artifact and rebuilding restored it, but the staging path remains the integration-script default.

Both retained runners use Fairyfly's default JSON output and error-level logging. They request `--output markdown` only for the Markdown screen-read check.

Check the visible SAP GUI connections from the repository root before choosing a live script:

~~~powershell
.\build\bin\Release\fairyfly.exe list
~~~

The old one-off VBScript screen probes were removed after their findings were recorded in the issue log. There is no Invoke-IntegrationTests.ps1 or Test-Integration.ps1 runner in the current tree. Use each retained script's prerequisites and arguments against a test system.

`test_integration.py` checks for server-side scripting before attempting its SM59 workflow. It targets the numeric connection-file ID returned by `launch` and closes only the SAP GUI session it created. If SM59 navigation fails, it skips the screen checks and proceeds to targeted cleanup. It reads JSON and Markdown screen output in memory and verifies a relative screenshot path in a temporary directory, which it removes after checking the PNG signature. It does not create persistent `test_screen_sm59.json`, `.md`, or PNG captures. The failure and authenticated paths have offline regressions in `test_integration_runner.py` and were verified on live Bigfox.

For the authenticated launch path, run `python tests/integration/test_integration.py --login-from-trial-env` from the repository root. The option calls Fairyfly's native `login --credentials-file trial.env` command for the newly launched GUI session; it needs no VBScript host. The password is not placed on a process command line or printed by the runner. A failed login still closes the runner-owned session.

When a suitable SAP GUI session is already open, run `python tests/integration/test_integration.py --existing-connection-id 0` with its actual Fairyfly connection-file ID. This mode checks that the ID exists, uses it for SM59 navigation and screen reads, and leaves the session connected. It never launches another connection.

For the disposable SU01 create, readback, password-change, and delete workflow, run `powershell -NoProfile -File tests/integration/test_su01_create_user.ps1 -LoginFromTrialEnv`. Add `-VerifyChangedPasswordLogin` to authenticate the disposable user in a second session and complete SAP's first-login password change before deletion. Passwords are piped to the GUI helper, not passed as command arguments. Both test-owned sessions are closed after cleanup. See [the SU01 workflow guide](README_SU01_TEST.md) for details.

For SAP-independent checks, build unit_tests and run ctest --test-dir build -C Release --output-on-failure.

After building the Release CLI, `python -m unittest tests/integration/test_cli_logging.py` checks that an error-level environment override leaves routine JSON output free of early informational stderr. The test also passed with a live SM59 session open.


## Bigfox regression suite and build comparison

Two read-only PowerShell 5.1 scripts turn the ad-hoc live checks into a repeatable suite against the Bigfox (SAP A4H) system. They are not registered with CTest. Verification of the checks is done by live runs of the maintainers/orchestrator; see [BASIS_SMOKE_TESTS.md](BASIS_SMOKE_TESTS.md) for the transaction catalogue.

Prerequisites

- Windows, SAP GUI with scripting enabled (client and server), no external PowerShell modules.
- A logged-in Bigfox session open as `/app/con[0]/ses[0]`, sitting on the SAP Easy Access screen, with no popup open. The user needs display authorizations only.
- A built `fairyfly.exe` (`build\Release\fairyfly.exe` by default; `build\bin\Release\fairyfly.exe` is used as a fallback).
- The scripts never read `trial.env` and never print credentials.

Transactions used: RZ11, ST22, SM37, SU01 (display), SM59, SE80 (bigfox_regression.ps1); additionally SM50, RZ04, SE16 (TADIR), SE11 (TADIR), SEGW, /IWFND/MAINT_SERVICE, SE38, SICF (compare_builds.ps1).

They are read-only apart from selection-screen fills (RZ11 parameter name, SM37 user `*` and from-date, SU01 user `DEVELOPER`, SE16/SE11 table `TADIR`). Buttons that are deliberately never pressed: SM37 btn[46] Release, btn[25] Stop, btn[14] Delete; SU01 btn[8] Create and btn[20] Change Password; any Save. The `--read-only` guard checks only send refused actions and change no state. Every script ends with `tcode /n` and closes leftover popups even on failure.

### bigfox_regression.ps1

~~~powershell
# plan only, does not call fairyfly
powershell -NoProfile -File tests\integration\bigfox_regression.ps1 -DryRun
# single run
powershell -NoProfile -File tests\integration\bigfox_regression.ps1
# soak: 20 iterations, 2 s slow threshold, log copy
powershell -NoProfile -File tests\integration\bigfox_regression.ps1 -Iterations 20 -SlowMs 2000 -LogFile scratch\soak.log
~~~

Parameters: `-Exe`, `-Iterations` (default 1), `-SlowMs` (default 3000), `-LogFile`, `-NoDestructiveGuardTests` (skip the `--read-only` refusal checks), `-DryRun`.

Output: one line per check, `PASS|FAIL|SKIP <name> [<ms>ms]` (with a reason after `-` for FAIL/SKIP), then a summary. SKIP means a precondition was missing (for example no ST22 dumps today) and does not fail the run; checks that depend on a failed or skipped earlier check are skipped. In soak mode the summary adds the slowest 10 calls (command line, ms, iteration), every call slower than `-SlowMs`, and a list of intermittent checks (passed in some iterations, failed in others). Exit code: 0 all passed, 1 at least one FAIL, 2 no SAP session.

Expected runtime: roughly 1 to 3 minutes per iteration on a responsive Bigfox (about 100 fairyfly calls); soak runs scale linearly.

### compare_builds.ps1

~~~powershell
powershell -NoProfile -File tests\integration\compare_builds.ps1 -OldExe C:\builds\old\fairyfly.exe -NewExe build\Release\fairyfly.exe
powershell -NoProfile -File tests\integration\compare_builds.ps1 -OldExe old.exe -NewExe new.exe -Screens SM50,SE80
~~~

For each of 18 read-only screens the script navigates once with `-NewExe`, then reads `screen read --no-tabs --max-rows 100 --output json` with both builds (best of 2 runs each), and prints Screen / OldMs / NewMs / OldEl / NewEl / OnlyOld / OnlyNew / OldKB / NewKB. The first 8 ids that differ are printed per screen. Exit code 1 if any element id differs, 2 if no SAP session. A screen whose navigation or read fails is reported as SKIP.
