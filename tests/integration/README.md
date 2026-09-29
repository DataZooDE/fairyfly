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
