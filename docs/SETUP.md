# Setup

This guide takes you from a Windows machine with SAP GUI to a working fairyfly connection that an AI agent or a
script can use. Plan about 15 minutes, plus the time your SAP administrator needs for the server setting.

## Requirements

| What | Details |
|---|---|
| Windows | Windows 10/11 or Windows Server, x64. fairyfly runs in the desktop session of a logged-on user (see [Desktop session](#desktop-session)). |
| SAP GUI for Windows | Installed and able to log on to your system. fairyfly is developed and tested with SAP GUI 8.00; the fast screen reader needs 7.70 patch level 3 or later, older releases use a slower fallback. |
| SAP GUI Scripting | Enabled on the SAP system and in SAP GUI ([step 2](#2-enable-sap-gui-scripting)). |
| SAP user | A normal dialog user. fairyfly can do exactly what this user may do, no more. Use a user with the permissions the agent really needs. |
| For AI agents | An MCP client such as Claude Code or Claude Desktop, on the same machine or on another machine ([MCP_REMOTE.md](MCP_REMOTE.md)). |

Nothing has to be installed in the SAP system, and fairyfly needs no runtime (no .NET, Java or Python).

## 1. Get fairyfly

**Download.** Once a release is published, `fairyfly.exe` (Windows x64, a single statically linked file, no
installer) is on the [GitHub releases page](https://github.com/DataZooDE/fairyfly/releases) together with a
`SHA256SUMS` file. Check the download:

~~~powershell
Get-FileHash .\fairyfly.exe        # compare with the line in SHA256SUMS
~~~

Put the file in a folder such as `C:\Tools\fairyfly\` and, optionally, add that folder to your `PATH`. Releases are
Authenticode-signed when a signing certificate is configured; see [SECURITY.md](SECURITY.md#release-integrity).
Windows may warn about a file downloaded from the internet. After checking the hash, `Unblock-File .\fairyfly.exe`
removes that mark.

**Build from source** if no release is published yet or you want to build it yourself: see
[BUILDING.md](BUILDING.md). The result is `build\bin\Release\fairyfly.exe`.

Check that it runs:

~~~powershell
fairyfly --version
fairyfly --help
~~~

## 2. Enable SAP GUI Scripting

SAP GUI Scripting is a standard SAP GUI feature, but it is switched off by default. It must be enabled in two
places.

### On the SAP system (administrator)

An SAP Basis administrator sets the profile parameter `sapgui/user_scripting` to `TRUE`:

- **Right away, until the next restart:** transaction `RZ11`, parameter `sapgui/user_scripting`, change the value to
  `TRUE`. The new value applies to new logons.
- **Permanently:** transaction `RZ10`, add `sapgui/user_scripting = TRUE` to the instance or default profile and
  activate it.

To limit scripting to selected users, also set `sapgui/user_scripting_per_user = TRUE`. Then only users with
authorization object `S_SCR` (activity 16) can script. This is the recommended way to allow scripting for the agent's
SAP user only.

SAP also offers `sapgui/user_scripting_set_readonly`, which limits scripting to read operations at the SAP level.
That is the strongest read-only enforcement, and the choice belongs to your Basis team: fairyfly's own read-only
guard is a heuristic guard rail, not a replacement for SAP-side controls. Note that with server-side read-only
scripting fairyfly cannot type into fields at all, not even into selection screens. See SAP's documentation of the
scripting profile parameters for the supported combinations with per-user scripting.

### In SAP GUI (each Windows machine)

1. In SAP Logon, open **Options** (or press Alt+F12 in a session and choose **Options**).
2. Go to **Accessibility & Scripting > Scripting**.
3. Check **Enable scripting**.
4. Uncheck **Notify when a script attaches to SAP GUI** and **Notify when a script opens a connection**. Otherwise
   SAP GUI shows a confirmation popup that blocks the agent.

When SAP GUI Security asks whether a connection may be opened, allow it (or adjust the rule under **Options >
Security > Security Settings**). Until it is allowed, `session launch` returns `SAP_GUI_SECURITY_PROMPT`.

## 3. Check the machine

Start SAP Logon, log on to your system, then run:

~~~powershell
fairyfly doctor --output markdown
~~~

`doctor` checks the desktop session, the SAP GUI process, the client scripting settings, the scripting engine and
the open connections. It changes nothing. Every warning or failure comes with a remediation. Typical findings:

| Finding | Fix |
|---|---|
| SAP GUI is not running | Start SAP Logon |
| Client scripting disabled, or notification popups enabled | [In SAP GUI](#in-sap-gui-each-windows-machine) |
| Scripting disabled on the server (`sapgui/user_scripting`) | [On the SAP system](#on-the-sap-system-administrator) |
| No active SAP connections | Log on in SAP Logon, or use `fairyfly session launch` (step 4) |
| Not an interactive desktop | Run fairyfly in the logged-on user's desktop, not as a service or scheduled task without a desktop |

## 4. Connect to a session

**Attach to a session you opened yourself.** This is the simplest start:

~~~powershell
fairyfly session list                                       # all open SAP GUI sessions with their ids
fairyfly session attach --session-id "/app/con[0]/ses[0]"   # save one as a fairyfly connection
fairyfly screen read --connection 0 --output markdown       # use the connection_id from the attach result
~~~

`session attach` without `--session-id` asks you to click into the SAP GUI window you want. Keep the
`connection_id` from the result and pass it as `--connection` to later commands; with a single saved connection it
can be omitted. (AI agents use `gui_session_attach`, which picks the session automatically when only one is open.)

**Or let fairyfly open and log on.** Store the SAP logon once in the Windows Credential Manager. You are prompted for
the password, which is never shown again:

~~~powershell
fairyfly credentials set Bigfox --user JDOE --client 001
fairyfly session launch Bigfox --login        # "Bigfox" is the name of the SAP Logon entry
~~~

The credential name matches the SAP Logon entry; `--credential NAME` picks another one. For several SAP users, single
sign-on and SNC, see [MCP_SAP_AUTHENTICATION.md](MCP_SAP_AUTHENTICATION.md). If the user is already logged on
elsewhere, the multiple-logon dialog is handled by `--multiple-logon` ([CLI.md](CLI.md#credentials-and-logon)).

## 5. Connect your AI agent

On the same machine, register fairyfly as an MCP server, for example in Claude Code:

~~~powershell
claude mcp add fairyfly -- C:\Tools\fairyfly\fairyfly.exe mcp
~~~

The server starts read-only. Claude Desktop, project configuration files, write mode and the tool list are in
[MCP.md](MCP.md). For an agent on another machine, follow [MCP_REMOTE.md](MCP_REMOTE.md).

## Desktop session

SAP GUI Scripting talks to the SAP GUI of a logged-on Windows user. fairyfly therefore needs that user's interactive
desktop:

- Keep the session logged on and unlocked. A locked screen or a disconnected Remote Desktop window stops screen
  rendering: screenshots come back black and some controls stop responding.
- On a dedicated virtual machine, disconnect Remote Desktop to the console instead of closing the window
  (`tscon <session id> /dest:console` from an elevated prompt), and disable the lock screen for that user.
  [MCP_TRAY.md](MCP_TRAY.md#rdp-and-lock-screen-caveats) has the details.
- fairyfly cannot run as a Windows service (services have no desktop). For unattended use, start it as a tray app at
  logon ([MCP_TRAY.md](MCP_TRAY.md#autostart)).

## Where fairyfly keeps its files

| What | Where |
|---|---|
| Saved connections | `%LOCALAPPDATA%\fairyfly\sessions\` (override: `FAIRYFLY_CACHE_DIR`) |
| SAP credentials | Windows Credential Manager, `fairyfly:<name>` |
| MCP access tokens (hashes only) | Windows Credential Manager, `fairyfly-mcp:<name>` |
| Audit trail | `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl` (override: `FAIRYFLY_AUDIT_FILE`) |
| MCP server configuration | `%LOCALAPPDATA%\fairyfly\mcp.yaml` (override: `-c` or `FAIRYFLY_MCP_CONFIG`) |
| Tray log | `%LOCALAPPDATA%\fairyfly\logs\mcp.log` |

`screen capture` saves a PNG to `--file`, or to a generated file name in the current directory when `--file` is
omitted (`--file -` writes to stdout, `--format base64` returns text). fairyfly has no telemetry and opens no network
port unless you start the HTTP server.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `NO_SESSIONS` | No SAP GUI session is open, or scripting is disabled. Log on in SAP Logon, then run `fairyfly doctor`. |
| `MULTIPLE_SESSIONS` | Several sessions are open. `fairyfly session list`, then `session attach --session-id ...`. |
| `SAP_GUI_SECURITY_PROMPT` | SAP GUI Security is waiting for the user to allow the connection. Allow it in SAP GUI. |
| A popup "A script is trying to attach to the GUI" appears | Disable the notifications in SAP GUI's scripting options ([step 2](#in-sap-gui-each-windows-machine)). |
| `LOGON_NOT_COMPLETED` with `multiple_logon_dialog` | The user is already logged on. Use `--multiple-logon keep` or `terminate`. |
| `READ_ONLY_REFUSED` | The read-only guard blocked a state-changing action. This is expected unless you meant to allow writes. |
| Screenshots are black, calls fail after a while | The desktop is locked or Remote Desktop is disconnected ([Desktop session](#desktop-session)). |
| The first call after logon is slow | SAP GUI is still loading; later calls are quicker. |

Add `--verbose-errors` or `-v` to a command for more detail. AI client problems are covered in
[MCP.md](MCP.md#troubleshooting), network problems in [MCP_REMOTE.md](MCP_REMOTE.md#troubleshooting).
