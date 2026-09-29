# Remote MCP: driving SAP GUI on a Windows VM from Linux or another host

This is the deployment guide for `fairyfly mcp --http`. The protocol basics, tools and stdio use are in
[MCP.md](MCP.md); the IIS reverse proxy is in [MCP_IIS.md](MCP_IIS.md); the tray, YAML config, client-config
and doctor are in [MCP_TRAY.md](MCP_TRAY.md). Verify current flags with `fairyfly mcp --help`.

## Architecture

~~~text
 Linux / macOS / Windows client                    Windows VM (interactive user session, SAP GUI running)
 Claude Code, Claude Desktop (mcp-remote), curl
        |
        |  HTTPS + "Authorization: Bearer ffy_<id>_<secret>"
        v
 +---------------------------+   plain HTTP, loopback only   +---------------------------------------+
 | IIS (URL Rewrite + ARR)   | ----------------------------> | fairyfly mcp --http  (console / tray)  |
 |  TLS, IP allow-list,      |   127.0.0.1:8383  POST /mcp   |  bearer-token auth, scopes, allowlists |
 |  /mcp only, else 404      |   X-Fairyfly-Proxy-Secret     |  rate limit, audit trail               |
 +---------------------------+   X-Forwarded-For/-Proto      |  one COM (STA) thread: one SAP call    |
                                                              |  at a time                             |
                                                              +------------------+--------------------+
                                                                                 | SAP GUI Scripting COM API
                                                                                 v
                                                                        SAP GUI for Windows -> SAP system
~~~

- IIS is optional in a lab (you can point a client at `http://127.0.0.1:8383/mcp` on the VM itself), but it is the
  supported way to expose the server: fairyfly speaks only plain HTTP, has no TLS of its own and defaults to
  binding `127.0.0.1`.
- All tool calls are serialized on one main thread (queue of 16, soft timeout 120 s). HTTP worker threads only
  queue and wait.
- The transport is `POST /mcp` only. There is no `Mcp-Session-Id` and no GET stream; every request stands alone.

### Why a console/tray program and not a Windows service

SAP GUI scripting talks to the SAP GUI process of a logged-on user and needs that user's interactive desktop.
A Windows service runs in Session 0, which has no access to that desktop, so it cannot see SAP GUI. fairyfly
is therefore an ordinary process started by the VM user: a console window, or with `--tray` a hidden console
plus a notification-area icon (see [MCP_TRAY.md](MCP_TRAY.md)). It only runs while that user is logged on to
an interactive session that stays unlocked.

## Step-by-step setup

### 1. VM prerequisites

- Windows 10/11 or Server with SAP GUI for Windows installed. Enable client-side scripting (SAP Logon options,
  Accessibility and Scripting, Scripting) and server-side scripting (`sapgui/user_scripting = TRUE`, profile
  parameter). `fairyfly doctor` reports whether scripting is available.
- A dedicated Windows user for the service role, with a logged-on SAP session (SAP Easy Access, no popup).
  Store the SAP logon in the Credential Manager once (`fairyfly credentials set NAME --user U --client 001`) so
  `session launch NAME --login` can log on again without passwords over the network.
- The logon session must stay unlocked and connected. A locked screen, a disconnected RDP window or a log off
  makes the desktop unavailable: screenshots come back black and some controls stop responding. Use the
  console session (`tscon <id> /dest:console`) or disable the lock screen for that user; see
  [MCP_TRAY.md](MCP_TRAY.md#rdp-and-lock-screen-caveats).
- `fairyfly.exe` from a Release build (`build\Release\fairyfly.exe`). No OpenSSL or other runtime is needed.

### 2. Start the server

~~~powershell
fairyfly mcp --http                              # read-only guard mode, 127.0.0.1:8383, POST /mcp
fairyfly mcp --http --mcp-port 8383 --allow-write   # write mode: gui_element_fill etc. for tokens that allow it
~~~

The posture banner is printed to stderr and lists endpoint, mode, auth, binding, SSE, allowed hosts, protocol
eras and every WARNING (no tokens, tokens without expiry or with `*`, write mode, non-loopback bind, missing
proxy secret). Without any token the server starts but answers every request with 401 `AUTH_REQUIRED`.

Relevant options (see `fairyfly mcp --help`):

| Option | Effect |
|---|---|
| `--http` / `--transport http` | serve HTTP instead of stdio |
| `--mcp-host` (default 127.0.0.1), `--mcp-port` / `--port` (8383) | bind address |
| `--allow-write`, `--read-only` | server mode; a token can only narrow it; `FAIRYFLY_READ_ONLY=1` is a hard cap |
| `--allowed-hosts`, `--cors-origin` | extra accepted Host values, accepted browser Origins (default: none, any Origin is refused with 403) |
| `--sse` / `--no-sse` | SSE streaming on `tools/call` (default on) |
| `--tools FAMILIES` | expose only these tool families |
| `--insecure-no-auth` | disables authentication; never use on a reachable port |
| `-c/--config PATH`, `--tray` | YAML config, tray mode (MCP_TRAY.md) |

Note on the YAML file: `mode.*`, `limits.*`, `tools.families`, `default_connection`, `format` and `server.transport`,
`server.port`, `server.host`, `server.sse` (default on, like `--sse`), `server.allowed_hosts` and
`server.cors_origins` are applied to the running server, with precedence flag > `FAIRYFLY_MCP_*` environment >
YAML > default. `--insecure-no-auth` is flag-only on purpose: it cannot be set from YAML or the environment.

### 3. Create tokens

~~~powershell
fairyfly mcp token create linux-reader --scope session,connection,screen --expires 90d
fairyfly mcp token create linux-ops --scope session,screen,element,transaction --tcode SM50,SM37,ST22 --system A4H/001 --rate 60 --ip 203.0.113.0/24
fairyfly mcp token list
~~~

The token (`ffy_<id>_<secret>`) is printed once. See [Tokens and scopes](MCP.md#tokens-and-scopes). A token
without `--scope` gets `session,connection,screen` and is read-only; an explicit `--scope` grants what it names
(still capped by the server mode) unless `--read-only` is added. Prefer named tokens per client and an expiry.
`revoke` keeps the (revoked) Credential Manager entry `fairyfly-mcp:<name>`; `fairyfly mcp token delete NAME --yes` removes it for good.

### 4. IIS in front (TLS)

Run `fairyfly mcp iis setup --hostname mcp.example.com --self-signed --allow-ip <client CIDRs> --dry-run`, then
again with `--yes`, elevated. Prerequisites (IIS, URL Rewrite, ARR), self-signed certificate trust on Linux,
`status` and `remove` are in [MCP_IIS.md](MCP_IIS.md). The setup stores the proxy secret in the Credential
Manager (`fairyfly:fairyfly-mcp-proxy`); the running server reads it from there, so no configuration is needed
in fairyfly. Without the secret, `X-Forwarded-For` is ignored and every client appears as the proxy address
(token IP binding then cannot work; the banner warns about it).

### 5. Tray and autostart

`fairyfly mcp --tray --http -c C:\path\mcp.yaml` detaches the server into a tray icon;
`fairyfly mcp --tray --install-autostart` registers it in `HKCU\...\Run` so it starts at logon of that user.
Details, the icon menu (start/stop/restart, read-only toggle) and the manual checklist are in
[MCP_TRAY.md](MCP_TRAY.md). Put `server.transport: http` and the other settings into the YAML because the Run
value only carries the config path.

### 6. Check

~~~powershell
fairyfly mcp doctor          # config, port, SAP GUI, interactive desktop, lock/RDP, autostart, tray
fairyfly mcp iis status      # elevated
~~~

Then run the Linux-side checklist below. From the VM itself,
`tests\integration\mcp_http_smoke.ps1` exercises the whole HTTP surface against a live SAP session.

## Protocol: two eras, status codes and errors

fairyfly serves both generations of the MCP HTTP transport on the same URL:

| | Legacy (2025-06-18, 2025-11-25) | Stateless (2026-07-28) |
|---|---|---|
| Handshake | `initialize` (echoes the negotiated version), then `notifications/initialized` (answered 202) | none; `server/discover` lists `supportedVersions`, capabilities, serverInfo, instructions |
| Version selection | `params.protocolVersion` in `initialize` (negotiated against all three versions: asking for `2026-07-28` yields a stateless answer with `resultType`, an unknown version falls back to `2025-11-25`); optional `MCP-Protocol-Version` header | `MCP-Protocol-Version` header or `params._meta.protocolVersion` |
| Client info | from `initialize` | `params._meta.clientInfo` per request |
| Session | none: no `Mcp-Session-Id` is ever minted, each request is served on its own | none |
| Result extras | none | `resultType: "complete"`; `tools/list` adds `ttlMs: 30000` and `cacheScope: "private"` |

A request without `initialize` and without a version header is treated as legacy, so `tools/list` and
`tools/call` work directly (this is what the curl examples use). `tools/list` is sorted by tool name in HTTP.
The `Mcp-Method` and `Mcp-Name` headers, when present, must match the body; the `_meta` key names of the
2026-07-28 draft are accepted both plain and with the `io.modelcontextprotocol/` prefix (unverified against a
final spec, see [OPEN_WORK.md](OPEN_WORK.md)).

### HTTP status and error mapping

Failures before a tool runs are plain JSON `{"error_code","message"}` (or a JSON-RPC error object for protocol
problems); tool failures are normal `200` JSON-RPC results with `isError: true` whose text starts with
`ERROR <CODE>:`.

| Status | Code | Cause |
|---|---|---|
| 200 | tool result / JSON-RPC error | including tool errors (`SCOPE_DENIED`, `READ_ONLY`, `TCODE_DENIED`, `SYSTEM_DENIED`, `SYSTEM_UNKNOWN`, `RATE_LIMITED`, `READ_ONLY_REFUSED`, `SERVER_BUSY`, `CALL_TIMEOUT`) |
| 202 | none | a notification or a response was posted |
| 400 | JSON-RPC -32700 / -32600 | body is not valid JSON / not a valid JSON-RPC message |
| 400 | JSON-RPC **-32020** | `Mcp-Method` or `Mcp-Name` header does not match the body |
| 400 | JSON-RPC **-32022** | unsupported protocol version; `error.data.supported` lists ours |
| 401 | `AUTH_REQUIRED` | no bearer token, or no token exists on the server yet (with `WWW-Authenticate: Bearer realm="fairyfly"`) |
| 401 | `TOKEN_INVALID`, `TOKEN_EXPIRED`, `TOKEN_REVOKED` | malformed/unknown/wrong secret (deliberately not distinguishable), expired, revoked |
| 403 | `IP_NOT_ALLOWED` | client address outside the token's `--ip` list |
| 403 | `HOST_NOT_ALLOWED`, `ORIGIN_NOT_ALLOWED` | DNS-rebinding defence: Host not loopback/`--allowed-hosts`, or an Origin that is not in `--cors-origin` |
| 404 | `NOT_FOUND` | any path other than `/mcp` |
| 405 | `METHOD_NOT_ALLOWED` | anything but POST (`Allow: POST`); an OPTIONS preflight of an allowed Origin gets 204 |
| 413 | `PAYLOAD_TOO_LARGE` | body over 1 MiB |
| 415 | `UNSUPPORTED_MEDIA_TYPE` | `Content-Type` is not `application/json` |
| 500/503 | `AUTH_ERROR`, `AUTH_UNAVAILABLE`, `INTERNAL_ERROR` | authenticator or token store failure |
| 503 | JSON-RPC -32000, `Retry-After: 1` | executor queue full (16) or server shutting down |

Host, Origin, path, method, content type and size are checked before authentication, so an unauthenticated
client can see 404/405/415/413 but never learns whether a token is valid from those.

## SSE behaviour

For `tools/call` with `Accept: text/event-stream` (and `--sse`, the default) the response is a
`text/event-stream` (`Cache-Control: no-cache`, `X-Accel-Buffering: no`):

- a `: keep-alive` comment every 15 s so proxies do not time out long SAP calls;
- when the request carries `params._meta.progressToken`, `notifications/progress` events (started/finished);
- the final JSON-RPC response as `event: message`;
- if the client disconnects, the call is cancelled between GUI steps (never in the middle of a click).

Any other request (including `tools/call` without that Accept header, and `tools/list`) is a plain JSON
response. Note that most clients send `Accept: application/json, text/event-stream`, so their `tools/call`
answers arrive as SSE. IIS must not buffer or compress the stream (the generated `web.config` handles it;
`mcp iis status` checks for drift).

## Security model

Layers, outside in:

1. **TLS at IIS** (certificate by thumbprint or self-signed). fairyfly itself listens on loopback in plain HTTP.
2. **IIS IP allow-list** (`--allow-ip`), a 403 before fairyfly sees the request. Path restriction to `/mcp`,
   1 MiB body limit, header hygiene (client-supplied `X-Forwarded-*` and the proxy secret header are dropped).
3. **Bearer tokens** (`ffy_<id>_<secret>`): only SHA-256 plus metadata are stored, in Windows Credential Manager
   (`fairyfly-mcp:<name>`; a compact JSON record, split over `fairyfly-mcp:<name>#1..n` chunk entries when a long
   allowlist exceeds one Credential Manager value, up to 16 chunks, else `TOKEN_TOO_LARGE`; the head entry is written
   last and a damaged or incomplete set never authenticates; `token delete` removes every chunk, `cmdkey` users must
   delete the `#n` entries as well); constant-time comparison; optional expiry and per-token IP binding; revocation takes
   effect within 5 s (instantly in the revoking process).
4. **Scopes** per tool family, the token **read-only** flag, **SAP system/client** allowlist and **T-code**
   allowlist, a per-token **rate limit**. The effective policy is the server policy intersected with the token's:
   a token can narrow, never widen. `FAIRYFLY_READ_ONLY=1` is a hard cap that no token or flag overrides.
5. **Proxy secret** (`X-Fairyfly-Proxy-Secret`): only requests carrying it may set the client address through
   `X-Forwarded-For` (the last entry, appended by IIS, is used).
6. **Audit trail**: one record per tool call with `principal`, `transport: "http"`, `remote_addr`, `era`,
   tool, status and error code; never results, screen content, fill values or tokens.
7. The read-only **guard** of the CLI (refuses Save/Delete/Release/... in read-only mode), unchanged from stdio.

### Threat model

Assets: the SAP session and its data (whatever the logged-on SAP user may do), the SAP credentials in the
Credential Manager, the bearer tokens and the proxy secret, the audit trail.

Actors: (a) a legitimate client with a token; (b) a network attacker without a token; (c) a compromised or
malicious client or MCP host (including prompt injection through SAP screen text); (d) a local user or malware on
the VM; (e) an operator with an over-privileged token.

| Threat | Control | Residual risk |
|---|---|---|
| Sniffing or tampering on the wire | TLS at IIS; HSTS; loopback hop only | plain HTTP on loopback: any local process can talk to `127.0.0.1:8383` (it still needs a token or `--insecure-no-auth`) |
| Unauthenticated access | bearer tokens required always; 401 for everything while no token exists; `TOKEN_INVALID` does not reveal which part is wrong | a leaked token is valid until revoked, rotated or expired |
| Access from unexpected networks | IIS IP allow-list; per-token `--ip` (needs a working proxy secret) | anyone who can read the proxy secret (local admin, Credential Manager reader) can spoof `X-Forwarded-For` and defeat token IP binding |
| Browser-based attacks (DNS rebinding, CSRF) | Host check (loopback or `--allowed-hosts`); any `Origin` refused unless in `--cors-origin`; POST + JSON only | none known beyond misconfigured `--allowed-hosts`/`--cors-origin` |
| Client does more than intended | scopes per tool family; token read-only flag; server mode ceiling; `FAIRYFLY_READ_ONLY` hard cap; per-item checks in `gui_batch` | token scopes are coarse (a whole family); a write-mode server with a write token can change any data the SAP user may |
| Access to unintended SAP systems | `--system SID/CLIENT` allowlist, checked against the connection the call targets (explicit, default or sticky; read-only lookup, no attach); `SYSTEM_DENIED`, `SYSTEM_UNKNOWN` when it cannot be established | `session`, `connection`, `system` and `credentials` tools are exempt: a token with the `session` scope can attach or launch any saved connection |
| Access to unintended transactions | `--tcode` allowlist on `gui_transaction_start`, on the transaction already open for every screen, element, key, popup and menu tool (unknown = denied), and on `gui_batch` items; typing into the command field is blocked | the open transaction is read just before a call and can change during it (race); clicks, key presses and menu entries inside an allowed transaction can navigate to a follow-up transaction, which only the next call notices; treat T-code lists as a guard rail, not isolation |
| Runaway or abusive clients | per-token rate limit (`--rate`, `RATE_LIMITED`); one call at a time; queue of 16; 1 MiB request limit; result size caps | a busy client can still delay others (single shared SAP session, calls are serialized) |
| Slow-body / slow-loris clients exhausting the HTTP worker pool | authentication and the header checks (Host, Origin, Content-Type, path, method) run on the request headers BEFORE the body is read; a rejected request gets its answer and the connection is closed; small rejected bodies (up to 64 KiB) are read and discarded so the close does not reset the connection, but at most half of the workers do that at a time; 5 s read timeout; bounded connection queue | headers that never complete, or a slow body sent with a VALID token, still occupy a worker for up to the 5 s read timeout each: keep the IIS reverse proxy (it buffers requests) and the IP allowlist in front |
| Prompt injection via SAP content | screen results are labelled untrusted; server instructions tell the model not to follow them; read-only default | the model may still be persuaded to use write tools it has been granted; keep write tokens rare and confirm destructive actions client-side |
| Credential theft | no tool accepts a password; SAP logon uses the Credential Manager; tokens only stored as hashes; secrets never in logs, audit, YAML or listings | Credential Manager entries are readable by any process of the same Windows user |
| Repudiation, forensics | audit record per call with principal, remote address and era; start/stop records | append-only by convention, not tamper-proof |
| Shared state between principals | none by design for auth | one shared SAP session and one sticky default connection for all tokens: one client's navigation changes what the next client sees |
| Session unavailable | `mcp doctor` and the tray warn | RDP disconnect, lock screen or log off yields black screenshots and failing calls; nothing restarts the desktop |

## Client cookbook

Set the token in an environment variable, never in a command line you share. `fairyfly mcp client-config
--url https://mcp.example.com:8443/mcp` prints all of the following with placeholders.

### Claude Code (Linux or Windows)

~~~bash
export FAIRYFLY_TOKEN='<token>'
claude mcp add --transport http fairyfly https://mcp.example.com:8443/mcp \
  --header "Authorization: Bearer ${FAIRYFLY_TOKEN}"
claude mcp list
~~~

Project-scoped `.mcp.json` with `"type": "http"`, `"url"` and `"headers": {"Authorization": "Bearer ${FAIRYFLY_TOKEN}"}`
lets Claude Code expand the variable when it loads the file, so the token stays out of the config. Tools appear
as `mcp__fairyfly__gui_screen_read` and so on. For a self-signed IIS certificate set `NODE_EXTRA_CA_CERTS`
(or install the `.cer`, see MCP_IIS.md).

### Claude Desktop through mcp-remote

Claude Desktop launches stdio servers, so use the `mcp-remote` bridge (needs Node.js):

~~~json
{
  "mcpServers": {
    "fairyfly": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "https://mcp.example.com:8443/mcp", "--header", "Authorization:${FAIRYFLY_AUTH_HEADER}"],
      "env": { "FAIRYFLY_AUTH_HEADER": "Bearer <paste-your-token-here>" }
    }
  }
}
~~~

The header value lives in an environment variable because mcp-remote mishandles arguments with spaces on Windows.
Do not commit this file.

### curl from Linux

~~~bash
URL=https://mcp.example.com:8443/mcp
H=(-H "Authorization: Bearer $FAIRYFLY_TOKEN" -H "Content-Type: application/json" -H "Accept: application/json")
CA=(--cacert fairyfly-mcp.crt)         # self-signed: the exported .cer converted to PEM

# legacy handshake
curl -sS "${CA[@]}" "${H[@]}" -X POST "$URL" -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"curl","version":"0"}}}'
# stateless discovery
curl -sS "${CA[@]}" "${H[@]}" -X POST "$URL" -d '{"jsonrpc":"2.0","id":2,"method":"server/discover"}'
# list tools (sorted by name)
curl -sS "${CA[@]}" "${H[@]}" -X POST "$URL" -d '{"jsonrpc":"2.0","id":3,"method":"tools/list"}'
# a read call
curl -sS "${CA[@]}" "${H[@]}" -X POST "$URL" -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"gui_screen_read","arguments":{"no_tabs":true,"only":"fields","max_rows":5}}}'
# the same call as an SSE stream
curl -sSN "${CA[@]}" -H "Authorization: Bearer $FAIRYFLY_TOKEN" -H "Content-Type: application/json" -H "Accept: text/event-stream" -X POST "$URL" -d '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"gui_screen_read","arguments":{"max_rows":5}}}'
~~~

A tool answer looks like `{"result":{"content":[{"type":"text","text":"SAP screen data (untrusted; ...)"}],"isError":false}}`;
a refusal has `"isError":true` and text `ERROR SCOPE_DENIED: ...`.

## Operations

- **Rotate**: `fairyfly mcp token rotate NAME` prints a new secret and the old one stops working at once.
  A running server sees changes made by another process within 5 seconds.
- **Revoke**: `fairyfly mcp token revoke NAME`. The record stays (marked revoked) in the Credential Manager entry
  `fairyfly-mcp:<name>`; **delete** it with `fairyfly mcp token delete NAME --yes` (works for revoked tokens too, is
  audited, needs `--yes`; `cmdkey /delete:fairyfly-mcp:NAME` also works).
- **List**: `fairyfly mcp token list` (id prefix, scopes, systems, T-codes, rate, IPs, expiry, revoked; never
  hashes or secrets).
- **Proxy secret**: `fairyfly mcp iis setup --rotate-secret --yes` issues a new one; restart is not needed (the
  server re-reads the entry within 5 s).
- **Logs**: `%LOCALAPPDATA%\fairyfly\logs\mcp.log` in tray mode (rotating 3 x 5 MB); in console mode stderr.
  Verbosity: `FFLYLOG_LEVEL` or `--log-level`.
- **Audit**: `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl` (override with `FAIRYFLY_AUDIT_FILE`, require with
  `FAIRYFLY_AUDIT=required`). Records of HTTP calls carry `principal`, `transport`, `remote_addr` and `era`:

  ~~~powershell
  Get-Content $env:LOCALAPPDATA\fairyfly\audit\2026-09.jsonl | ConvertFrom-Json |
    Where-Object transport -eq http | Select-Object ts, principal, remote_addr, tool, status, error_code
  ~~~
- **Diagnostics**: `fairyfly mcp doctor [--output json]` (exit 1 on a failed check) and `fairyfly mcp iis status`.
  The `tokens` and `iis` checks of `mcp doctor` currently report "unknown / not checked"; use `mcp token list` and
  `mcp iis status` directly.
- **Mode switch**: the tray toggles read-only/write at run time; otherwise restart with or without `--allow-write`.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| 401 `AUTH_REQUIRED` and the message says no tokens are configured | no token exists on the VM (as this user): `fairyfly mcp token create NAME` |
| 401 `TOKEN_INVALID` | wrong, truncated or rotated token, or the client added quotes; recreate or rotate |
| 401 `TOKEN_REVOKED` / `TOKEN_EXPIRED` | revoked or past its `--expires`; create or rotate a token |
| 403 `IP_NOT_ALLOWED` | the client address is not in the token's `--ip`; behind IIS this also needs the proxy secret (banner warning "proxy secret not set") |
| 403 from IIS (HTML, no JSON) | client outside `--allow-ip` of `mcp iis setup` |
| 403 `HOST_NOT_ALLOWED` / `ORIGIN_NOT_ALLOWED` | a client talks to fairyfly with an unexpected Host (direct, not via IIS) or a browser Origin; use `--allowed-hosts` / `--cors-origin` |
| 404 | wrong path; it must be exactly `/mcp` |
| 405 / 415 | client did GET or sent another content type; use POST with `application/json` |
| 502.3 from IIS | fairyfly is not running or is on another port; see MCP_IIS.md troubleshooting |
| Tool error `SCOPE_DENIED` | the token lacks the tool's family; create a token with that `--scope` |
| `READ_ONLY` | the token is read-only; `TOOL_UNAVAILABLE_READ_ONLY` / `READ_ONLY_REFUSED`: the server is in read-only mode, restart with `--allow-write` if intended |
| `TCODE_DENIED` | transaction not in the token's `--tcode`; the same code blocks typing into the command field |
| `SYSTEM_UNKNOWN` / `SYSTEM_DENIED` | a token with `--system` needs a known attached session (call `gui_session_attach` first with a `session`-scoped token); or wrong system |
| `RATE_LIMITED` | over the token's `--rate` (or the server default 120/min); combine steps with `gui_batch` |
| `NO_SESSIONS`, `MULTIPLE_SESSIONS` | no SAP session, or several open; `gui_session_list` then `gui_session_attach` with `session_id` |
| Screenshot is black, calls fail after a while | the desktop is locked or the RDP session is disconnected; `tscon`, disable lock, see MCP_TRAY.md |
| SSE events arrive in one burst | IIS/another proxy buffers; `mcp iis status` drift check, no WAF or antivirus proxy in between |
| Startup: `BIND_FAILED` (exit 2) | port already in use; choose `--mcp-port` |
| `mcp` exits at once when started with a config error | `fairyfly mcp config validate` |

## Linux-side manual check list

Run these from the Linux client (not on the VM) against the real IIS endpoint after every setup or upgrade.
`URL`, `TOKEN` and `CA` as in the cookbook; expected results in the right column.

| # | Check | Expected |
|---|---|---|
| 1 | `curl -i "${CA[@]}" $URL` (GET, no token) | 405 (`Allow: POST`) from fairyfly, or 401/403 from IIS if the request is rejected earlier; never 200 |
| 2 | `curl -i "${CA[@]}" -X POST $URL -H 'Content-Type: application/json' -d '{}'` | 401 `AUTH_REQUIRED` with `WWW-Authenticate: Bearer realm="fairyfly"` |
| 3 | same with `-H "Authorization: Bearer ffy_00000000_bad"` | 401 `TOKEN_INVALID` |
| 4 | `curl -i "${CA[@]}" https://host:8443/other` | 404 (IIS) |
| 5 | TLS: `openssl s_client -connect host:8443 -servername host </dev/null` and `curl` without `--cacert` | trusted chain (CA certificate) or the documented failure for self-signed; no fallback to HTTP on port 80 |
| 6 | `initialize` (legacy), then `server/discover` with `MCP-Protocol-Version: 2026-07-28` | both answer 200; discover lists `supportedVersions` |
| 7 | `tools/list` | 200; tool names sorted; `gui_element_fill` present only when the server is in write mode |
| 8 | `tools/call gui_screen_read` (token with `screen`) | 200, text starts with `SAP screen data (untrusted` |
| 9 | `tools/call gui_transaction_start` with a token lacking `transaction` | tool error `SCOPE_DENIED` |
| 10 | read-only token, `tools/call gui_element_fill` on a write server | tool error `READ_ONLY` |
| 11 | SSE: `Accept: text/event-stream`, `curl -N` on a slow call (for example `gui_screen_read` on a 12-tab screen) | frames arrive progressively, `: keep-alive` on long calls, final `event: message`; nothing is held back until the end |
| 12 | request from an address outside `--allow-ip` (a second host) | 403 from IIS |
| 13 | token with `--ip <your CIDR>` used from another address | 403 `IP_NOT_ALLOWED` (proves the proxy secret and `X-Forwarded-For` work) |
| 14 | `fairyfly mcp token revoke NAME` on the VM, retry within 6 s | 401 `TOKEN_REVOKED` |
| 15 | `claude mcp add --transport http ...` then `claude mcp list` and a prompt that reads a screen | server is connected, tool call succeeds |
| 16 | MCP Inspector CLI: `npx @modelcontextprotocol/inspector --cli $URL --transport http --header "Authorization: Bearer $TOKEN" --method tools/list` | tool list |
| 17 | disconnect the RDP window on the VM, repeat 8 and a `gui_screen_capture` | expected failure mode is a black image; confirm the console-session mitigation works |
