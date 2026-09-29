# SU01 disposable user workflow

`test_su01_create_user.ps1` exercises the SAP GUI SU01 controls observed on Bigfox. It checks that a test user is absent, creates it with an initial password, opens Display to read back the name, changes the password, deletes the user, and checks absence again. Add `-VerifyChangedPasswordLogin` to authenticate in a second session before deletion; if SAP requires a first-login password change, the helper completes it and verifies `session.Info.User`. Both passwords travel to the helper over standard input, never on a process command line. The script attempts deletion in `finally` if creation was attempted but a later step fails. If cleanup fails, it prints the username for manual recovery. Passwords are not printed.

Use an already logged-in connection:

```powershell
.\tests\integration\test_su01_create_user.ps1 -ExistingConnectionId 0
```

The connection ID is the saved `id` in `fairyfly connection list` (JSON by default); it can differ from the GUI collection `index` shown by `session list`. In this mode the script checks that the saved session is valid, scopes every operation with `--connection`, and leaves the connection open. For a new session, use `-LoginFromTrialEnv`: the script launches Bigfox, calls Fairyfly's native `session login --connection <id>` (Credential Manager entry named like the connection; store it first with `credentials set` or `credentials import-env`), then closes its own session with `session disconnect --close-session` after the SU01 workflow. The optional changed-password verification uses `session login --credentials-stdin` in a second session to handle SAP's forced first-login password change. Passwords are piped to Fairyfly, never passed on a process command line. The default username is a generated `ZFF` name of at most 12 characters; a specified name must be uppercase and is checked for absence before any create step. Generated passwords start with mixed case, a digit, and a special character, followed by a random suffix.

```powershell
.\tests\integration\test_su01_create_user.ps1 -LoginFromTrialEnv
.\tests\integration\test_su01_create_user.ps1 -LoginFromTrialEnv -VerifyChangedPasswordLogin
```

Run the offline contract regression first:

```powershell
.\tests\integration\test_su01_runner.ps1
```

The regression injects a fake CLI and login helpers, fails closed if the runner lacks its mock-path parameter, and asserts SU01 paths, connection scoping, password transport, and cleanup after a failed login. It does not contact SAP. On 2026-09-26 the script passed against an existing Bigfox session with `ZFFLYU2609D`, against a newly launched session with `ZFF97E9FEB4`, and with changed-password authentication for `ZFF3E55F320`. Each disposable user was deleted; owned sessions were closed, while existing-session mode preserved its caller's connection. The changed-password check first revealed that Windows PowerShell prefixed native standard input with a UTF-8 marker, causing a wrong-password error. After the helper stripped that marker, SAP accepted the password, required a new password, and completed the second-session login.
