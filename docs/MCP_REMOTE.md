# Remote MCP: driving SAP GUI on a Windows VM from Linux or another host

This is the deployment guide for `fairyfly mcp --http`. The protocol basics, tools and stdio use are in
[MCP.md](MCP.md); the one-time setup of the certificate, URL reservation and TLS binding is in
[MCP_SETUP.md](MCP_SETUP.md); the tray, YAML config, client-config and doctor are in [MCP_TRAY.md](MCP_TRAY.md).
Verify current flags with `fairyfly mcp --help`.

## Architecture

~~~text
 Linux / macOS / Windows client                     Windows VM (interactive user session, SAP GUI running)
 Claude Code, Claude Desktop (mcp-remote), curl
        |
        |  HTTPS (TLS 1.2+) + "Authorization: Bearer ffy_<id>_<secret>"
        v
 +------------------------------------------------------------------------------------+
 | http.sys (Windows kernel HTTP driver)                                              |
 |   TLS termination with the certificate bound by `fairyfly mcp setup` (Schannel)    |
 |   kernel timeouts, header/body limits, URL prefix routing (only /mcp/ is served)   |
 +---------------------------------------+--------------------------------------------+
                                         | HTTP Server API request queue
                                         v
 +------------------------------------------------------------------------------------+
 | fairyfly mcp --http  (console or tray, UNELEVATED, the logged-on user's session)   |
 |   server --allow-ip, Host/Origin checks, bearer-token auth (token --ip on the peer)|
 |   scopes, allowlists, rate limit, audit trail; 16 receive workers, queue of 256    |
 |   one COM (STA) thread: one SAP call at a time                                     |
 +------------------------------------+-----------------------------------------------+
                                      | SAP GUI Scripting COM API
                                      v
                             SAP GUI for Windows -> SAP system
~~~

- fairyfly listens itself, through the HTTP Server API (http.sys), and needs no IIS, no reverse proxy and no
  extra software. TLS is terminated in the kernel: the private key stays in the machine certificate store and is
  never loaded into the fairyfly process, which runs unelevated and holds no key and no administrator rights.
- Plain HTTP on `127.0.0.1` stays possible for development and tests (`fairyfly mcp --http` without `--tls`).
  Plain HTTP on any other host is refused (`INSECURE_BIND`) unless the flag-only `--insecure-http` is given.
- All tool calls are serialized on one main thread (queue of 256 at the listener, executor queue of 16, soft timeout
  120 s). HTTP worker threads only queue and wait. Because of this, the per-token check -> invoke -> update
  sequence in the dispatcher (for example the "left the T-code allowlist" block) is atomic per process: no two
  calls, not even of the same token, are ever inside `call_tool` at once (unit test `CallExecutor: concurrent
  submitters never overlap inside call_tool`). A multi-threaded executor would need a per-principal lock first.
- The transport is `POST /mcp` only. There is no `Mcp-Session-Id` and no GET stream; every request stands alone.
- The client address is the socket peer address. Forwarded headers (`X-Forwarded-For`, `X-Forwarded-Proto`,
  `X-Fairyfly-Proxy-Secret`) are ignored like any unknown header; there is no trusted-proxy mode and none is
  planned (non-goal).

### Why a console/tray program and not a Windows service

SAP GUI scripting talks to the SAP GUI process of a logged-on user and needs that user's interactive desktop.
A Windows service runs in Session 0, which has no access to that desktop, so it cannot see SAP GUI. fairyfly
is therefore an ordinary process started by the VM user: a console window, or with `--tray` a hidden console
plus a notification-area icon (see [MCP_TRAY.md](MCP_TRAY.md)). It only runs while that user is logged on to
an interactive session that stays unlocked. http.sys does not need a service either: an unelevated process can
own a URL prefix once an administrator has reserved it for the user (the URL ACL created by `mcp setup`).

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

### 2. One-time setup (one elevated step)

~~~powershell
fairyfly mcp setup --hostname mcp.example.com --self-signed --allow-ip 203.0.113.0/24 --dry-run   # plan only
fairyfly mcp setup --hostname mcp.example.com --self-signed --yes
~~~

`mcp setup` creates the certificate (`--self-signed`, or bring your own with `--cert-thumbprint`), the URL ACL for
your user SID, the TLS binding of the certificate to the port (default 8443) and, on request, a firewall rule
(`--open-firewall`). It self-elevates once (one UAC prompt), then proves the result with a real TLS round trip
(401 `AUTH_REQUIRED`, pinned by thumbprint, negotiated protocol reported), and it writes `mcp.yaml` with
`server.tls: true` when none exists. `--print-runbook` prints the equivalent commands for an administrator
instead. Full reference, plan, exit codes and the "Left for a human" list: [MCP_SETUP.md](MCP_SETUP.md).

### 3. Create tokens

~~~powershell
fairyfly mcp token create linux-reader --scope session,connection,screen --expires 90d
fairyfly mcp token create linux-ops --scope session,screen,element,transaction --tcode SM50,SM37,ST22 --system A4H/001 --rate 60 --ip 203.0.113.0/24
fairyfly mcp token list
~~~

The token (`ffy_<id>_<secret>`) is printed once. See [Tokens and scopes](MCP.md#tokens-and-scopes). A token
without `--scope` gets `session,connection,screen` and is read-only; an explicit `--scope` grants what it names
(still capped by the server mode) unless `--read-only` is added. Prefer named tokens per client and an expiry.
`--ip` binds a token to client addresses; the address checked is the real socket peer.
`revoke` keeps the (revoked) Credential Manager entry `fairyfly-mcp:<name>`; `fairyfly mcp token delete NAME --yes` removes it for good.

### 4. Start the server

~~~powershell
fairyfly mcp --http --tls --mcp-host mcp.example.com --mcp-port 8443    # HTTPS, read-only guard mode
fairyfly mcp --http --tls --mcp-host mcp.example.com --allow-write      # write mode: gui_element_fill etc. for tokens that allow it
fairyfly mcp --http                                                     # dev: plain HTTP on 127.0.0.1:8383
~~~

With `server.tls: true` and the host in `mcp.yaml` (written by `mcp setup`), `fairyfly mcp --http` is enough. The
posture banner is printed to stderr and lists endpoint, `tls: http.sys`, mode, auth, client allow-list, SSE,
allowed hosts, protocol eras and every WARNING (no tokens, tokens without expiry or with `*`, write mode,
non-loopback bind). Without any token the server starts but answers every request with 401 `AUTH_REQUIRED`.

Relevant options (see `fairyfly mcp --help`):

| Option | Effect |
|---|---|
| `--http` / `--transport http` | serve MCP over HTTP or HTTPS (http.sys) instead of stdio |
| `--tls` / `--no-tls` | HTTPS; the certificate binding must exist (`mcp setup`) |
| `--mcp-host` (default 127.0.0.1), `--mcp-port` / `--port` (8383; `mcp setup` chooses 8443 for TLS) | URL prefix: `127.0.0.1` (loopback), `+` (all interfaces, TLS recommended) or a host name; `localhost` is treated as 127.0.0.1 |
| `--allow-ip CIDR,...` | server-level client allow-list; loopback is always allowed; empty = any |
| `--insecure-http` | allow plain HTTP on a non-loopback host (flag-only, dangerous) |
| `--allow-write`, `--read-only` | server mode; a token can only narrow it; `FAIRYFLY_READ_ONLY=1` is a hard cap |
| `--allowed-hosts`, `--cors-origin` | extra accepted Host values, accepted browser Origins (default: none, any Origin is refused with 403) |
| `--sse` / `--no-sse` | SSE streaming on `tools/call` (default on) |
| `--tools FAMILIES` | expose only these tool families |
| `--insecure-no-auth` | disables authentication; never use on a reachable port |
| `-c/--config PATH`, `--tray` | YAML config, tray mode (MCP_TRAY.md) |

Note on the YAML file: `mode.*`, `limits.*`, `tools.families`, `default_connection`, `format` and `server.transport`,
`server.port`, `server.host`, `server.tls`, `server.allow_ip`, `server.sse` (default on, like `--sse`),
`server.allowed_hosts` and `server.cors_origins` are applied to the running server, with precedence flag >
`FAIRYFLY_MCP_*` environment (for example `FAIRYFLY_MCP_SERVER_TLS`, `FAIRYFLY_MCP_SERVER_ALLOW_IP`) > YAML >
default. `--insecure-no-auth` and `--insecure-http` are flag-only on purpose: they cannot be set from YAML or the
environment. The old `auth.proxy_secret_source` key is gone (reported as an unknown key).

### 5. Tray and autostart

`fairyfly mcp --tray --http -c C:\path\mcp.yaml` detaches the server into a tray icon;
`fairyfly mcp --tray --install-autostart` registers it in `HKCU\...\Run` so it starts at logon of that user.
Details, the icon menu (start/stop/restart, read-only toggle) and the manual checklist are in
[MCP_TRAY.md](MCP_TRAY.md). Put `server.transport: http` and the other settings into the YAML because the Run
value only carries the config path.

### 6. Trust the certificate on the client

For a self-signed certificate export it on the VM and trust it on every client:

~~~powershell
fairyfly mcp cert export --out fairyfly.cer            # or --format pem
~~~

curl: `--cacert fairyfly.pem`; Node (Claude Code, mcp-remote): `NODE_EXTRA_CA_CERTS=/path/fairyfly.pem`; Windows:
`certutil -addstore -user Root fairyfly.cer`. Never use `curl -k`. A certificate from your own CA needs only the
CA to be trusted. Details in [MCP_SETUP.md](MCP_SETUP.md).

### 7. Check

~~~powershell
fairyfly mcp doctor          # config, elevation, URL ACL, TLS binding, certificate, firewall, port, TLS handshake, SAP GUI, desktop, tokens, autostart, tray
~~~

Then run the Linux-side checklist below. From the VM itself,
`tests\integration\mcp_http_smoke.ps1` exercises the whole HTTP surface against a live SAP session.

### 8. Teardown

~~~powershell
fairyfly mcp teardown --dry-run
fairyfly mcp teardown --yes        # one UAC prompt; removes only what setup created (manifest-driven, idempotent)
~~~

`--keep-cert` and `--keep-firewall` keep those parts. Tokens are separate: `fairyfly mcp token delete NAME --yes`.

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
| 200 | tool result / JSON-RPC error | including tool errors (`SCOPE_DENIED`, `READ_ONLY`, `TCODE_DENIED`, `SYSTEM_DENIED`, `SYSTEM_UNKNOWN`, `CONNECTION_DENIED`, `RATE_LIMITED`, `READ_ONLY_REFUSED`, `SERVER_BUSY`, `CALL_TIMEOUT`) |
| 202 | none | a notification or a response was posted |
| 400 | JSON-RPC -32700 / -32600 | body is not valid JSON / not a valid JSON-RPC message |
| 400 | JSON-RPC **-32020** | `Mcp-Method` or `Mcp-Name` header does not match the body |
| 400 | JSON-RPC **-32022** | unsupported protocol version; `error.data.supported` lists ours |
| 401 | `AUTH_REQUIRED` | no bearer token, or no token exists on the server yet (with `WWW-Authenticate: Bearer realm="fairyfly"`) |
| 401 | `TOKEN_INVALID`, `TOKEN_EXPIRED`, `TOKEN_REVOKED` | malformed/unknown/wrong secret (deliberately not distinguishable), expired, revoked |
| 403 | `ADDRESS_NOT_ALLOWED` | peer address outside the server's `--allow-ip` list (checked first, before everything else; loopback always passes) |
| 403 | `IP_NOT_ALLOWED` | peer address outside the token's `--ip` list |
| 403 | `HOST_NOT_ALLOWED`, `ORIGIN_NOT_ALLOWED` | DNS-rebinding defence: Host not loopback/`--allowed-hosts`, or an Origin that is not in `--cors-origin` |
| 404 | `NOT_FOUND` | any path other than `/mcp` |
| 405 | `METHOD_NOT_ALLOWED` | anything but POST (`Allow: POST`); an OPTIONS preflight of an allowed Origin gets 204 |
| 413 | `PAYLOAD_TOO_LARGE` | body over 1 MiB |
| 415 | `UNSUPPORTED_MEDIA_TYPE` | `Content-Type` is not `application/json` |
| 500/503 | `AUTH_ERROR`, `AUTH_UNAVAILABLE`, `INTERNAL_ERROR` | authenticator or token store failure |
| 503 | JSON-RPC -32000, `Retry-After: 1` | executor queue full (16) or server shutting down |

The server allow-list, Host, Origin, path, method, content type and size are checked before authentication, so an
unauthenticated client can see 403/404/405/415/413 but never learns whether a token is valid from those. Requests
the kernel rejects itself (malformed HTTP, unregistered paths, header limits, timeouts) get http.sys' own answer
and never reach fairyfly.

## SSE behaviour

For `tools/call` (with `--sse`, the default) the response is a `text/event-stream` only when the client asks for it
through content negotiation, else plain JSON. The rule honours q-values on the `Accept` header:

- SSE when `text/event-stream` is the only acceptable type, or has a higher q-value than `application/json`;
- SSE when the request carries `params._meta.progressToken` and `text/event-stream` is acceptable (q > 0), so
  clients that need progress notifications still get them;
- plain JSON otherwise, in particular for `Accept: application/json, text/event-stream` (equal preference, the
  usual header of curl-style and MCP SDK clients) without a progress token.

The SSE response is a `text/event-stream` (`Cache-Control: no-cache`, `X-Accel-Buffering: no`):

- a `: keep-alive` comment every 15 s so intermediaries and idle timeouts do not cut long SAP calls;
- when the request carries `params._meta.progressToken`, `notifications/progress` events (started/finished);
- the final JSON-RPC response as `event: message`;
- if the client disconnects, the call is cancelled between GUI steps (never in the middle of a click).

Any other request (including `tools/call` under the rules above, and `tools/list`) is a plain JSON
response. Events are written progressively by http.sys; put no buffering or compressing intermediary between
client and server. HTTP/2 and HTTP/1.1 clients are both served (verification against a live host is tracked in
[OPEN_WORK.md](OPEN_WORK.md)).

## Security model

Layers, outside in:

1. **TLS in the kernel** (http.sys with the certificate bound by `mcp setup`; Schannel negotiates, machine policy
   sets the minimum version, setup warns below TLS 1.2). The private key never enters the fairyfly process.
2. **Server `--allow-ip`**: a client allow-list of addresses or CIDR blocks on the socket peer, checked first, a
   403 `ADDRESS_NOT_ALLOWED` before anything else is looked at. Loopback always passes; empty = any. Combine it
   with a firewall rule (`mcp setup --open-firewall`) for defence in depth.
3. **Bearer tokens** (`ffy_<id>_<secret>`): only SHA-256 plus metadata are stored, in Windows Credential Manager
   (`fairyfly-mcp:<name>`; a compact JSON record, split over `fairyfly-mcp:<name>#1..n` chunk entries when a long
   allowlist exceeds one Credential Manager value, up to 16 chunks, else `TOKEN_TOO_LARGE`; the head entry is written
   last and a damaged or incomplete set never authenticates; `token delete` removes every chunk, `cmdkey` users must
   delete the `#n` entries as well); constant-time comparison; optional expiry and per-token **IP binding on the
   real peer address** (`IP_NOT_ALLOWED`); revocation takes effect within 5 s (instantly in the revoking process).
4. **Scopes** per tool family, the token **read-only** flag, **SAP system/client** allowlist and **T-code**
   allowlist, a per-token **rate limit**. The effective policy is the server policy intersected with the token's:
   a token can narrow, never widen. `FAIRYFLY_READ_ONLY=1` is a hard cap that no token or flag overrides.
5. **Request limits**: kernel timeouts (header wait 10 s, body 15 s, idle connection 120 s, minimum send rate),
   16 receive workers, a request queue of 256, 1 MiB body limit answered before the body is read.
6. **Audit trail**: one record per tool call with `principal`, `transport: "http"`, `remote_addr` (the real peer),
   `era`, tool, status and error code; never results, screen content, fill values or tokens.
7. The read-only **guard** of the CLI (refuses Save/Delete/Release/... in read-only mode), unchanged from stdio.

There is no reverse proxy and no trusted-proxy mode: `X-Forwarded-For`, `X-Forwarded-Proto` and
`X-Fairyfly-Proxy-Secret` are ignored, so a client cannot choose the address it is judged by. Terminating TLS in a
front proxy is a non-goal; if you run one anyway, token `--ip` and server `--allow-ip` see the proxy's address.

### Threat model

Assets: the SAP session and its data (whatever the logged-on SAP user may do), the SAP credentials in the
Credential Manager, the bearer tokens, the TLS private key, the audit trail.

Actors: (a) a legitimate client with a token; (b) a network attacker without a token; (c) a compromised or
malicious client or MCP host (including prompt injection through SAP screen text); (d) a local user or malware on
the VM; (e) an operator with an over-privileged token.

| Threat | Control | Residual risk |
|---|---|---|
| Sniffing or tampering on the wire | TLS 1.2+ in the kernel (http.sys/Schannel); plain HTTP only on loopback unless `--insecure-http`; certificate pinned by thumbprint in the setup verification | a self-signed certificate must be distributed and trusted on each client by hand; the minimum TLS version follows the machine's Schannel policy, not fairyfly; on loopback any local process can reach `127.0.0.1` (it still needs a token or `--insecure-no-auth`) |
| Unauthenticated access | bearer tokens required always; 401 for everything while no token exists; `TOKEN_INVALID` does not reveal which part is wrong | a leaked token is valid until revoked, rotated or expired |
| Access from unexpected networks | server `--allow-ip` (403 `ADDRESS_NOT_ALLOWED`); per-token `--ip`; both judge the real socket peer, forwarded headers are ignored; optional firewall rule | behind NAT or a proxy every client shares one address; an allowed host that is compromised is trusted; IPv6 privacy addresses need CIDR blocks |
| Browser-based attacks (DNS rebinding, CSRF) | Host check (loopback or `--allowed-hosts`); any `Origin` refused unless in `--cors-origin`; POST + JSON only | none known beyond misconfigured `--allowed-hosts`/`--cors-origin` |
| Client does more than intended | scopes per tool family; token read-only flag; server mode ceiling; `FAIRYFLY_READ_ONLY` hard cap; per-item checks in `gui_batch` | token scopes are coarse (a whole family); a write-mode server with a write token can change any data the SAP user may |
| Access to unintended SAP systems | `--system SID/CLIENT` allowlist, checked against the connection the call targets (explicit, default or sticky; read-only lookup, no attach); `SYSTEM_DENIED`, `SYSTEM_UNKNOWN` when it cannot be established | login/attach/disconnect --close-session are checked against their own target (attach: the LIVE connection description and system, never a saved record); a launch (also `login=true`) under `--system` is fail closed: it needs the entry name in `--connections` (operator vouches that the name maps to an allowed system) and any open session of that name must be on an allowed system, else `SYSTEM_UNKNOWN`/`SYSTEM_DENIED`; a changed SAP Logon entry can still be reached by the launch itself once; `--connections NAME` restricts saved connections by name (`CONNECTION_DENIED`); the three listing tools return only the token's connections (result filtering), and the SAP Logon system of a not-yet-open entry cannot be known |
| Access to unintended transactions | `--tcode` allowlist on `gui_transaction_start`, on the transaction already open for every screen, element, key, popup and menu tool (unknown = denied), and on `gui_batch` items; typing into the command field is blocked; with an allowlist `gui_menu_select` is denied and `gui_key_send` is limited to enter, f4, f8 and page keys unless the token has `--allow-navigation` (fail closed); after every screen-acting call the transaction is read again: a token that ended outside its allowlist gets `tcode_left_allowlist` in the result and audit and is blocked (`TCODE_DENIED`) until an allowed `gui_transaction_start` succeeds | partly mitigated: `gui_element_click`, `gui_element_f4` and `gui_popup_close` can still navigate, and that is noticed only after the call (the destination screen has loaded); tokens with `--allow-navigation` can use menus and F3/F12 freely; the transaction is read just before a call and can change during it; treat T-code lists as a guard rail, not isolation |
| Runaway or abusive clients | per-token rate limit (`--rate`, optional per-family `--rate-family element=10,key=10`, `RATE_LIMITED`); one call at a time; executor queue of 16, listener queue of 256, 16 receive workers; 1 MiB request limit; result size caps; kernel timeouts | a busy client can still delay others (single shared SAP session, calls are serialized) |
| Slow-body / slow-loris clients exhausting the workers | the kernel enforces the timeouts (header wait 10 s, entity body 15 s, drain 5 s, idle connection 120 s, minimum send rate) and never hands a request without complete headers to fairyfly; authentication and the header checks run on the headers BEFORE the body is read, and a rejected request never has its body read | a slow body sent with a VALID token still occupies one of the 16 workers until the kernel timeout; the server allow-list, a firewall rule and short-lived tokens limit who can do that |
| Local privilege boundary | `mcp setup`/`teardown` need exactly one elevated step (self-elevating, explicit user SID in the plan file); the running server is unelevated, holds no private key and no administrator rights; the URL ACL reserves the prefix for one user SID only, so other local users cannot bind it or hijack the port | a local administrator can rebind the port or read the machine key; another process of the same user can bind another prefix and can read that user's Credential Manager entries |
| Legacy artefacts of earlier proxy setups | none are created any more | a leftover Credential Manager entry `fairyfly:fairyfly-mcp-proxy` is unused: delete it (`cmdkey /delete:fairyfly:fairyfly-mcp-proxy`); `mcp doctor` reports it |
| Prompt injection via SAP content | screen results are labelled untrusted; server instructions tell the model not to follow them; read-only default | the model may still be persuaded to use write tools it has been granted; keep write tokens rare and confirm destructive actions client-side |
| Credential theft | no tool accepts a password; SAP logon uses the Credential Manager; tokens only stored as hashes; secrets never in logs, audit, YAML or listings | Credential Manager entries are readable by any process of the same Windows user |
| Repudiation, forensics | audit record per call with principal, real peer address and era; start/stop records | append-only by convention, not tamper-proof |
| Shared state between principals | none by design for auth | the sticky default connection and the default rate budget are per token, but the SAP GUI session and its screen state (open transaction, popups, field contents) are shared: one client's navigation still changes what the next client sees, so tokens with different purposes should use different saved connections (`--connections`) |
| Session unavailable | `mcp doctor` and the tray warn | RDP disconnect, lock screen or log off yields black screenshots and failing calls; nothing restarts the desktop |

## Client cookbook

Set the token in an environment variable, never in a command line you share. `fairyfly mcp client-config
--url https://mcp.example.com:8443/mcp` prints all of the following with placeholders and the certificate trust hint.

### Claude Code (Linux or Windows)

~~~bash
export FAIRYFLY_TOKEN='<token>'
claude mcp add --transport http fairyfly https://mcp.example.com:8443/mcp \
  --header "Authorization: Bearer ${FAIRYFLY_TOKEN}"
claude mcp list
~~~

Project-scoped `.mcp.json` with `"type": "http"`, `"url"` and `"headers": {"Authorization": "Bearer ${FAIRYFLY_TOKEN}"}`
lets Claude Code expand the variable when it loads the file, so the token stays out of the config. Tools appear
as `mcp__fairyfly__gui_screen_read` and so on. For a self-signed certificate set `NODE_EXTRA_CA_CERTS` to the
exported PEM (`fairyfly mcp cert export --format pem`, see [MCP_SETUP.md](MCP_SETUP.md)).

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
CA=(--cacert fairyfly.pem)             # self-signed: `fairyfly mcp cert export --format pem`; never use -k

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
- **List**: `fairyfly mcp token list` (id prefix, scopes, systems, T-codes, allow_navigation, rate, IPs, expiry, revoked; never
  hashes or secrets).
- **Certificate**: `fairyfly mcp cert export` writes the public certificate for clients; renew it by running
  `mcp setup` again ([MCP_SETUP.md](MCP_SETUP.md)); `mcp doctor` warns before it expires.
- **Logs**: `%LOCALAPPDATA%\fairyfly\logs\mcp.log` in tray mode (rotating 3 x 5 MB); in console mode stderr.
  Verbosity: `FFLYLOG_LEVEL` or `--log-level`.
- **Audit**: `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl` (override with `FAIRYFLY_AUDIT_FILE`, require with
  `FAIRYFLY_AUDIT=required`). Records of HTTP calls carry `principal`, `transport`, `remote_addr` and `era`:

  ~~~powershell
  Get-Content $env:LOCALAPPDATA\fairyfly\audit\2026-09.jsonl | ConvertFrom-Json |
    Where-Object transport -eq http | Select-Object ts, principal, remote_addr, tool, status, error_code
  ~~~
- **Diagnostics**: `fairyfly mcp doctor [--output json]` (read-only, never elevated; exit 1 on a failed check)
  with stable check ids and a remedy for each failure. The `tokens` check reports "unknown / not checked"; use
  `mcp token list` directly.
- **Mode switch**: the tray toggles read-only/write at run time; otherwise restart with or without `--allow-write`.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| 401 `AUTH_REQUIRED` and the message says no tokens are configured | no token exists on the VM (as this user): `fairyfly mcp token create NAME` |
| 401 `TOKEN_INVALID` | wrong, truncated or rotated token, or the client added quotes; recreate or rotate |
| 401 `TOKEN_REVOKED` / `TOKEN_EXPIRED` | revoked or past its `--expires`; create or rotate a token |
| 403 `ADDRESS_NOT_ALLOWED` | the client address is not in the server's `--allow-ip` / `server.allow_ip`; loopback is always allowed |
| 403 `IP_NOT_ALLOWED` | the client address is not in the token's `--ip`; the real peer address is judged, `X-Forwarded-For` is ignored (check NAT: the server may see the NAT address) |
| 403 `HOST_NOT_ALLOWED` / `ORIGIN_NOT_ALLOWED` | a client uses a Host name that is not loopback or in `--allowed-hosts` (add the DNS name of the VM), or a browser Origin; use `--allowed-hosts` / `--cors-origin` |
| 404 | wrong path; it must be exactly `/mcp` |
| 405 / 415 | client did GET or sent another content type; use POST with `application/json` |
| Connection refused or timeout | fairyfly is not running, wrong port, or the Windows firewall blocks the port: `mcp doctor` (`firewall`, `port`), `mcp setup --open-firewall` |
| TLS handshake fails, "certificate verify failed" | the client does not trust the certificate: export and trust it (`mcp cert export`; curl `--cacert`, `NODE_EXTRA_CA_CERTS`, `certutil`); the name in the URL must be in the certificate's SAN |
| TLS handshake fails, "protocol version" / negotiated below TLS 1.2 | the client or the machine's Schannel policy only offers TLS 1.0/1.1; enable TLS 1.2 on the older side; `mcp setup` verify and `mcp doctor` (`tls_handshake`) report the negotiated protocol |
| Client cannot resolve the host name | DNS or hosts entry for the certificate's name is missing on the client; use the name given to `mcp setup --hostname`, or add a hosts entry |
| Tool error `SCOPE_DENIED` | the token lacks the tool's family; create a token with that `--scope` |
| `READ_ONLY` | the token is read-only; `TOOL_UNAVAILABLE_READ_ONLY` / `READ_ONLY_REFUSED`: the server is in read-only mode, restart with `--allow-write` if intended |
| `TCODE_DENIED` | transaction not in the token's `--tcode`; the same code blocks typing into the command field, `gui_menu_select` and navigating keys (F3, F12, ...) unless the token was created with `--allow-navigation`, and every screen call after a result carried `tcode_left_allowlist` until an allowed `gui_transaction_start` succeeds |
| `SYSTEM_UNKNOWN` / `SYSTEM_DENIED` | a token with `--system` needs a target whose system is known (an open session; a launch needs the entry name in the token's `--connections` list as well, see docs/MCP.md; otherwise start it on the desktop and attach); or wrong system. `CONNECTION_DENIED`: the connection name is not in the token's `--connections` |
| `RATE_LIMITED` | over the token's `--rate` (or the server default 120/min); combine steps with `gui_batch` |
| `NO_SESSIONS`, `MULTIPLE_SESSIONS` | no SAP session, or several open; `gui_session_list` then `gui_session_attach` with `session_id` |
| Screenshot is black, calls fail after a while | the desktop is locked or the RDP session is disconnected; `tscon`, disable lock, see MCP_TRAY.md |
| SSE events arrive in one burst | an intermediary (proxy, WAF, antivirus HTTPS inspection) buffers the stream; connect directly |
| Startup: `BIND_FAILED` (exit 2), reason `no_url_reservation` | no URL ACL for this user and prefix: run `fairyfly mcp setup` (elevated once), or as an administrator `netsh http add urlacl url=<prefix> user=<DOMAIN\user>` |
| Startup: `BIND_FAILED`, reason `prefix_registered` | another process (a second fairyfly, the tray) already serves this prefix; stop it or choose another `--mcp-port` |
| Startup: `INSECURE_BIND` (exit 2) | plain HTTP on a non-loopback host; use `--tls` after `mcp setup`, or accept the risk with `--insecure-http` |
| Startup: `INVALID_ARGUMENT` about `--allow-ip` | an entry is not an IPv4/IPv6 address or CIDR block |
| `mcp` exits at once when started with a config error | `fairyfly mcp config validate` |

## Linux-side manual check list

Run these from the Linux client (not on the VM) against the real HTTPS endpoint after every setup or upgrade.
`URL`, `TOKEN` and `CA` as in the cookbook; expected results in the right column.

| # | Check | Expected |
|---|---|---|
| 1 | `curl -i "${CA[@]}" $URL` (GET, no token) | 405 (`Allow: POST`); never 200 |
| 2 | `curl -i "${CA[@]}" -X POST $URL -H 'Content-Type: application/json' -d '{}'` | 401 `AUTH_REQUIRED` with `WWW-Authenticate: Bearer realm="fairyfly"` |
| 3 | same with `-H "Authorization: Bearer ffy_00000000_bad"` | 401 `TOKEN_INVALID` |
| 4 | `curl -i "${CA[@]}" https://host:8443/other` | 404 (http.sys, no JSON body) |
| 5 | TLS: `openssl s_client -connect host:8443 -servername host </dev/null` and `curl` without `--cacert` | negotiated TLS 1.2 or 1.3, the expected subject; without `--cacert` the documented failure for a self-signed certificate; plain HTTP on the TLS port is not answered with a 2xx |
| 6 | `initialize` (legacy), then `server/discover` with `MCP-Protocol-Version: 2026-07-28` | both answer 200; discover lists `supportedVersions` |
| 7 | `tools/list` | 200; tool names sorted; `gui_element_fill` present only when the server is in write mode |
| 8 | `tools/call gui_screen_read` (token with `screen`) | 200, text starts with `SAP screen data (untrusted` |
| 9 | `tools/call gui_transaction_start` with a token lacking `transaction` | tool error `SCOPE_DENIED` |
| 10 | read-only token, `tools/call gui_element_fill` on a write server | tool error `READ_ONLY` |
| 11 | SSE: `Accept: text/event-stream`, `curl -N` on a slow call (for example `gui_screen_read` on a 12-tab screen) | frames arrive progressively, `: keep-alive` on long calls, final `event: message`; nothing is held back until the end |
| 12 | request from an address outside the server `--allow-ip` (a second host) | 403 `ADDRESS_NOT_ALLOWED` |
| 13 | token with `--ip <your CIDR>` used from another address, with and without a spoofed `X-Forwarded-For` naming an allowed address | 403 `IP_NOT_ALLOWED` both times (the real peer decides) |
| 14 | `fairyfly mcp token revoke NAME` on the VM, retry within 6 s | 401 `TOKEN_REVOKED` |
| 15 | `claude mcp add --transport http ...` then `claude mcp list` and a prompt that reads a screen | server is connected, tool call succeeds |
| 16 | MCP Inspector CLI: `npx @modelcontextprotocol/inspector --cli $URL --transport http --header "Authorization: Bearer $TOKEN" --method tools/list` | tool list |
| 17 | disconnect the RDP window on the VM, repeat 8 and a `gui_screen_capture` | expected failure mode is a black image; confirm the console-session mitigation works |
