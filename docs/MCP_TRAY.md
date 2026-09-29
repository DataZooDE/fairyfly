# fairyfly MCP: tray, YAML config, client config and doctor

This page covers the operator side of the remote MCP server on the Windows VM: the system-tray mode,
the YAML config file, autostart, `mcp client-config` and `mcp doctor`. The HTTP protocol and IIS are
described elsewhere (docs/MCP.md, docs/MCP_REMOTE.md).

## Why a console app with a tray, not a Windows service

SAP GUI scripting needs the interactive desktop of a logged-on user. A service runs in Session 0 and
cannot see SAP GUI. So `fairyfly mcp` is a normal process started by the VM user; `--tray` only hides
its console and adds a notification-area icon.

## Tray mode

    fairyfly mcp --tray [-c C:\path\mcp.yaml]

The tray needs the HTTP transport (`--http`, or `server.transport: http` in the YAML). Started from a
console, the process relaunches itself detached (no window, `FAIRYFLY_TRAY_CHILD=1` marks the child),
prints the child pid and exits with 0. The child frees its console, logs to
`%LOCALAPPDATA%\fairyfly\logs\mcp.log` (rotating, 3 x 5 MB) and shows the icon. A second start prints or
pops up "already running" (named mutex `Local\fairyfly-tray`) and exits 0.

Icon colors: green = running, yellow = running with warnings, grey = stopped, red = error. The icon is
re-added when Explorer restarts (`TaskbarCreated`). Left-click shows a status balloon (endpoint, mode,
counters, warnings). Warnings are also shown in a balloon once when the server is up.

Right-click menu:

| Item | Effect |
|---|---|
| status line | endpoint and mode (read-only / write mode) |
| Warnings (n) | submenu with the posture warnings of the server |
| Start / Stop / Restart | Stop finishes the running call and stops accepting; the process and the tray stay |
| Read-only mode (check) | turning it OFF asks "Allow the MCP server to change SAP data?"; locked when `FAIRYFLY_READ_ONLY=1` |
| Open log / audit trail folder / config file | default shell action |
| Tokens > List / Create | opens a console running `fairyfly mcp token list` / `token create` |
| Quit | stops the server and ends the process |

Tokens are only ever shown by the token command in that console (once, at creation). The tray, the
YAML and the logs never contain a token.

### Threading (for developers)

The COM/STA executor of the server stays on the process main thread (`IServerRunner::run_blocking()`).
The tray UI runs on a second thread with its own message pump and talks to the server only through
`IServerControl`. See the header comment of `src/include/tray/tray.h`. Every OS effect (window/icon,
registry, processes, files, mutex, message boxes, clipboard) is behind an interface with fakes in the
unit tests; the real implementations are exercised by hidden manual tests
(`unit_tests.exe "[tray_manual]"`, see the checklist below).

## Autostart

    fairyfly mcp --tray --install-autostart [-c C:\path\mcp.yaml]
    fairyfly mcp --tray --remove-autostart

Writes/removes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value `fairyfly-mcp`:
`"C:\...\fairyfly.exe" mcp --tray [-c "<config>"]`. Put `server.transport: http` and the other server
settings into the YAML, because the Run value only carries the config path (an explicit `-c` or
`FAIRYFLY_MCP_CONFIG` at install time is recorded; otherwise the default path is used at logon).

Caveats:

* It only runs while the user is logged on to an interactive desktop. There is no service.
* A brief console window may flash at logon before the detached child takes over.
* If nobody logs on after a reboot, nothing starts. Consider Windows autologon (Sysinternals
  Autologon stores the password as an LSA secret) for a dedicated VM user. fairyfly does not configure it.

### RDP and lock-screen caveats

Screenshots and some controls need a live, unlocked desktop:

* Closing an RDP window disconnects the session; the desktop is then not rendered and `screen capture`
  returns black images. Log off is worse (the tray dies). Disconnect to the console instead:
  `tscon <session id> /dest:console` from an elevated prompt (run `query session` for the id).
* A locked workstation behaves the same. Disable the lock screen and screen-saver lock for the VM user
  (group policy or `powercfg`) or keep the session on the console.
* `mcp doctor` warns about locked and disconnected sessions and about Session 0.

## YAML config

Default path `%LOCALAPPDATA%\fairyfly\mcp.yaml`; override with `-c/--config PATH` or `FAIRYFLY_MCP_CONFIG`.
Precedence for every key: **flag > environment variable > YAML > default**. Environment variable of a key:
`FAIRYFLY_MCP_<SECTION>_<KEY>` (for example `FAIRYFLY_MCP_SERVER_PORT`, `FAIRYFLY_MCP_MODE_READ_ONLY`,
`FAIRYFLY_MCP_DEFAULT_CONNECTION`; lists are comma separated). `FAIRYFLY_READ_ONLY=1` remains a hard cap.

    fairyfly mcp config init [--force]   # commented template, never overwrites without --force
    fairyfly mcp config path
    fairyfly mcp config show             # effective values with source: flag / env / yaml / default
    fairyfly mcp config validate         # line-numbered errors, exit 1 when invalid

Keys (every active line of the generated template equals the default):

| Key | Default | Meaning |
|---|---|---|
| `server.host` / `server.port` | 127.0.0.1 / 8383 | HTTP listen address (keep loopback; IIS terminates TLS) |
| `server.transport` | stdio | `stdio` or `http` |
| `server.sse` | true | allow SSE streaming on tools/call |
| `server.allowed_hosts[]` / `server.cors_origins[]` | empty | extra Host values / browser origins |
| `mode.read_only` / `mode.allow_write` | false / false | guard mode (both true is an error) |
| `limits.max_result_chars` / `max_image_bytes` / `max_calls_per_minute` / `call_timeout_ms` | 60000 / 2097152 / 120 / 120000 | limits |
| `tools.families[]` | all | expose only these families |
| `default_connection`, `format` | unset, markdown | defaults for tool calls |
| `tray.enabled` / `start_minimized_notice` / `autostart` | false / true / false | tray behavior (`autostart` documents intent; use `--install-autostart`) |
| `audit.enabled` / `required` / `file` | true / false / default | audit trail settings |
| `auth.token_prefix` / `proxy_secret_source` | ffy / credential-manager | token prefix; where the IIS proxy secret lives |

No secrets belong in this file. Unknown keys and keys named password/secret/token* produce a warning; a
secret-like key with a value, or a value that looks like a token or `Bearer ...`, is refused with
`CONFIG_CONTAINS_SECRET` (the value is never echoed). An invalid file stops `mcp` with exit code 2.
`apply_config` applies mode, limits, tools, format, default connection, transport, port and the HTTP keys
`server.host`, `server.sse`, `server.allowed_hosts`, `server.cors_origins` (flag > env > YAML > default; see
MCP_REMOTE.md). `insecure_no_auth` is not a config key: it stays a command-line flag.

## Client configs

    fairyfly mcp client-config [--claude-code] [--claude-desktop] [--mcp-remote] [--curl] [--stdio]
                               [--url https://vm:8443/mcp] [--token-env FAIRYFLY_TOKEN] [--name fairyfly]
                               [--output json]

Prints paste-ready snippets (all of them without a selector): `claude mcp add --transport http ... --header
"Authorization: Bearer ${FAIRYFLY_TOKEN}"`, the `.mcp.json` equivalent, Claude Desktop via `npx mcp-remote`
(the header value goes through an environment variable because mcp-remote on Windows mishandles arguments
with spaces), and a `curl` initialize + tools/list smoke test for a Linux host. Only placeholders are
printed; `--token-env` takes the NAME of a variable, and a token-shaped value is rejected.

## mcp doctor

    fairyfly mcp doctor [--output json]

Checks (pass / warn / fail / skip): config validity, port free or bound, SAP GUI scripting available and a
logged-in session (through the read-only `doctor` diagnostics; nothing is clicked), interactive desktop
(not Session 0), locked or RDP-disconnected session, token count, IIS status, autostart registered, tray
running. Token count and IIS report "unknown / not checked" until the token store and IIS integration
provide their probes. Exit code 1 when any check fails.

## Manual checklist (desktop, not CI)

1. `unit_tests.exe "[tray_manual][icon]"`: icon appears, cycles green/yellow/grey/red, balloon shows.
2. Restart Explorer (`taskkill /f /im explorer.exe`, then start it): the icon comes back.
3. `fairyfly mcp --tray --http` from a console: pid printed, console returns, icon appears; running it again
   says "already running".
4. Menu: Stop then Start, Restart, Open log / audit folder / config, Tokens (console stays open).
5. Toggle read-only off: confirm dialog appears; No leaves the mode unchanged.
6. `--install-autostart`, log off and on: tray starts; `--remove-autostart`, check the Run key is gone.
7. Disconnect RDP: `mcp doctor` reports the disconnected session; a screenshot is black.
8. `unit_tests.exe "[tray_manual][registry]"` and `"[tray_manual][mutex]"`, `"[tray_manual][shell]"`.
