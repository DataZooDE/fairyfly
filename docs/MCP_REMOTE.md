# Remote MCP: driving SAP GUI on a Windows VM from Linux or another host

This is the deployment guide for `fairyfly mcp --http`. The protocol basics, tools and stdio use are in
[MCP.md](MCP.md); the one-time setup of the certificate, URL reservation and TLS binding is in
[MCP_SETUP.md](MCP_SETUP.md); the tray, YAML config, client-config and doctor are in [MCP_TRAY.md](MCP_TRAY.md).
SAP username/password and SSO/SNC setup for multiple SAP users is in [MCP_SAP_AUTHENTICATION.md](MCP_SAP_AUTHENTICATION.md).
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
- Without owner mode, all tool calls are serialized on one main thread (queue of 256 at the listener, executor queue of 16, soft timeout
  120 s). HTTP worker threads only queue and wait. Because of this, the per-token check -> invoke -> update
  sequence in the dispatcher (for example the "left the T-code allowlist" block) is atomic per process: no two
  calls, not even of the same token, are ever inside `call_tool` at once (unit test `CallExecutor: concurrent
  submitters never overlap inside call_tool`). A multi-threaded executor would need a per-principal lock first.
- The transport is `POST /mcp` only. There is no `Mcp-Session-Id` and no GET stream; every request stands alone.
- The client address is the socket peer address. Forwarded headers (`X-Forwarded-For`, `X-Forwarded-Proto`,
  `X-Fairyfly-Proxy-Secret`) are ignored like any unknown header; there is no trusted-proxy mode and none is
  planned (non-goal).

### Why a console/tray program and not a Windows service

SAP GUI scripting needs the interactive desktop of a logged-on user, which Windows services do not have (see
[ARCHITECTURE.md](ARCHITECTURE.md#why-not-a-windows-service)). fairyfly is therefore an ordinary process of the VM
user: a console window, or with `--tray` a notification-area icon ([MCP_TRAY.md](MCP_TRAY.md)). It only runs while
that user is logged on to an unlocked interactive session. http.sys needs no service either: an unelevated process
can own a URL prefix once an administrator has reserved it for the user (the URL ACL created by `mcp setup`).

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
fairyfly mcp setup --hostname mcp.example.com --self-signed --allow-ip 203.0.113.0/24 --yes
~~~

`mcp setup` creates the certificate (`--self-signed`, or bring your own with `--cert-thumbprint`), the URL ACL for
your user SID, the TLS binding of the certificate to the port (default 8443) and, on request, a firewall rule
(`--open-firewall`). It self-elevates once (one UAC prompt), then proves the result with a real TLS round trip
(401 `AUTH_REQUIRED`, pinned by thumbprint, negotiated protocol reported), and it writes `mcp.yaml` with
`server.tls: true` when none exists. `--print-runbook` prints the equivalent commands for an administrator
instead. Full reference, plan, exit codes and the "Left for a human" list: [MCP_SETUP.md](MCP_SETUP.md).

### 3. Create tokens

~~~powershell
fairyfly mcp token create linux-reader --scope session.list,session.attach,connection.list,screen --expires 90d
fairyfly mcp token create linux-ops --scope session,screen,element,transaction --tcode SM50,SM37,ST22 --system A4H/001 --rate 60 --ip 203.0.113.0/24
fairyfly mcp token list
~~~

The token (`ffy_<id>_<secret>`) is printed once. See [Tokens and scopes](MCP.md#tokens-and-scopes). A token
without `--scope` gets `session.list,session.attach,connection.list,screen` and is read-only; an explicit `--scope` grants what it names
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
| `--allow-ip CIDR,...` | server-level client allow-list; loopback is exempt by default; empty = any |
| `--allow-ip-include-loopback` | require loopback callers to match a nonempty `--allow-ip` list too; YAML: `server.allow_ip_include_loopback: true` |
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

Set the exact SAP identities exposed by one interactive tray in the local YAML file:

~~~yaml
owner:
  sap_identities: [A4H/001/ALICE, A4H/001/BOB]
~~~

When this list has more than one identity, issue each client token with an exact `--sap-identity SID/CLIENT/USER` grant. Older unbound tokens are refused until reissued. See [MCP_SAP_AUTHENTICATION.md](MCP_SAP_AUTHENTICATION.md) for password and SSO/SNC logon.

On a shared Windows host, set `server.allow_ip_include_loopback: true` if the server IP allowlist must
also govern other local accounts and loopback tunnels. Include the intended `127.0.0.1/8` or `::1` peer
in `server.allow_ip` when those clients need access. A nonempty `server.allow_ip` list is required when this
setting is true; startup rejects an empty list. Bearer tokens remain required either way.

An entry is uppercase `SID/CLIENT/USER` with a three-digit client; wildcards are rejected. This opt-in rule
refuses server startup if `FAIRYFLY_MCP_OWNER_SAP_IDENTITIES` is set to an invalid value, so a typo cannot
silently remove the owner boundary. The YAML parser also rejects invalid entries.
For owner-mode tray routing, the SAP window must also belong to the same Windows user SID, interactive session,
and logon authentication ID as the tray and its private worker. An unavailable window handle or unreadable
process token denies selection. This Windows check supplements the exact SAP identity rule below. Each Windows
account needs its own interactive tray endpoint and SAP GUI if separate Windows credential custody is required;
the primary multi-user acceptance test uses different SAP users under one Windows account.
This rule
checks live SAP facts for session calls, filters `gui_session_list` per session and token system allowlist,
and checks explicit `gui_session_attach` before any saved-connection write and again afterwards. Session
commands bind to the checked saved connection and revalidate its generation, live SAP identity and nonempty
server session key in the handler before acting. A changed session during a call has its result withheld as
`OUTCOME_UNKNOWN`; a write may already have run. Unknown and foreign sessions share the public
`OWNER_SESSION_UNAVAILABLE` refusal. If a session's identity cannot be verified, the call is denied.
Owner-filtered session discovery returns validated session IDs and live connection names; it omits window
titles and other enumerated metadata that could belong to an earlier occupant of a reused session ID.
With this rule, `gui_session_launch` requires a token whose `--connections` restriction explicitly vouches for
the SAP Logon entry. The newly opened window is checked before it receives a saved connection ID and checked
again before that ID is returned. The `--allow-sapshcut` fallback is refused in owner-mode HTTP. `gui_session_login`
can authenticate a saved prelogin window when the stored credential resolves to a token-authorized SAP identity. On a multi-identity endpoint, the window must also have been launched by that same token; on this
SAP GUI installation the logon screen reports client `000` and transaction `S000` before it changes to the
authenticated client. Login holds the target session lane and pauses admission probes while it types. Implicit
attach and credential listings remain withheld. `gui_connection_list` returns only saved connections verified against
the live allowed SAP identity and token restrictions; `gui_doctor` reports only the count and readiness of those
verified connections. Existing SAP sessions can be found with `gui_session_list` and saved with explicit
`gui_session_attach`. On a multi-identity endpoint, an unfinished window launched through Fairyfly is bound to the launching token and saved generation for 15 minutes. Only that token may complete `gui_session_login`; an unbound prelogin window is refused. `gui_session_launch` with `login:true` also supports password logon, and a SAP Logon entry may complete SSO during launch. For navigation and writes, acquire an exclusive
`gui_session_lease` for the saved connection and pass its `lease_id` on each state-changing tool call; renew it
before its 60-second expiry and release it when finished. A lease is bound to the live saved-session generation
and token issuance ID. Observational reads need no lease: use `gui_screen_read` with `no_tabs=true`;
the default tab expansion and `gui_element_get` with `activate_tab=true` select tabs and require a lease.
An owner-mode `gui_screen_read` also returns a `screen_guard` in `structuredContent` when SAP reports a known
transaction, program and screen number. For a Markdown result, `structuredContent` then also carries the screen
text as `text`, because clients such as Claude Code show the model only `structuredContent` when it is present.
Pass the `screen_guard` value as `expected_screen_guard` on a later session action
to reject it with `SCREEN_CHANGED` if the session or dynpro changed while the client was deciding or waiting.
This is an optional precondition; it cannot detect edits that leave the same dynpro identity unchanged, so keep
the lease for multi-step writes.
Separate clients using one token still need the lease secret
to coordinate. Lease acquisition requires the explicit `session.lease` token scope (or `*`); the broad `session`
scope covers the other session tools (list, attach, launch, login, disconnect, subject to mode and target
restrictions) but not leases. With an owner allowlist, the tray routes established SAP
windows to private workers and ordered per-window lanes. Different windows can make progress concurrently;
calls to one window remain ordered. On the tested A4H host, a read of one window finished while a 20-read
batch ran in another, both for separate SAP connections and for two windows in one connection. SAP GUI-wide
operations and SAP backend waits can still limit concurrency.
Owner-mode `gui_batch` requires a top-level `connection` and uses one top-level `lease_id` for all items that
select tabs, navigate or write. Mixed-session batches are refused before any item runs. A queued HTTP call is
reauthenticated against fresh token metadata before execution; an action already running cannot be revoked mid-call.

### 5. Tray and autostart

`fairyfly mcp --tray --http -c C:\path\mcp.yaml` detaches the server into a tray icon;
`fairyfly mcp --tray --install-autostart` registers it in `HKCU\...\Run` so it starts at logon of that user.
Details, the icon menu (start/stop/restart, read-only toggle) and the manual checklist are in
[MCP_TRAY.md](MCP_TRAY.md). The tray needs the HTTP transport: pass `--http` when you start it by hand. For autostart,
put `server.transport: http` into the YAML first: the Run value only carries `mcp --tray` and the config path, so an
`--http` given to `--install-autostart` is not kept.
`fairyfly mcp setup` does not write `server.transport` on purpose, so a plain stdio `fairyfly mcp` (for example a
Claude Desktop config) keeps working after setup.

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

A request without `initialize` and without any version hint is treated as legacy, so `tools/list` and
`tools/call` work directly (this is what the curl examples use). `tools/list` is sorted by tool name in HTTP.

### Stateless era rules (MCP 2026-07-28)

Verified against the published specification on 2026-10-02
([transports/streamable-http](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)
and the changelog of that revision). Every POST is classified first:

- **modern** when `params._meta` carries a protocol version `>= 2026-07-28` (key `io.modelcontextprotocol/protocolVersion`,
  or the plain `protocolVersion`), or the method is `server/discover`, or the `MCP-Protocol-Version` header is `2026-07-28`;
- **legacy** otherwise (`initialize` handshakes of 2025-06-18 and 2025-11-25, requests without any version hint): the
  behaviour is unchanged, no standard header is required (the spec allows servers to serve older clients without
  the header, and fairyfly extends that to its legacy clients).

For a modern request, after authentication and before any tool or provider call:

| Rule | Failure |
|---|---|
| `MCP-Protocol-Version` is present and equals the body's `_meta` protocol version (a body without one is accepted; the header alone then decides) | 400, JSON-RPC -32020, `Header missing: MCP-Protocol-Version` / `Header mismatch: MCP-Protocol-Version header value 'x' does not match body value 'y'` |
| the version is one we serve (checked first, also for legacy requests that name one) | 400, JSON-RPC -32022, `error.data.supported` lists the versions |
| `Mcp-Method` is present and equals the body `method` (case-sensitive value) | 400, -32020, `Header missing: Mcp-Method` / `Header mismatch: Mcp-Method header value ...` |
| `tools/call`, `resources/read`, `prompts/get`: `Mcp-Name` is present and equals `params.name` (`params.uri` for `resources/read`) | 400, -32020, `Header missing: Mcp-Name` / `Header mismatch: Mcp-Name header value ...` |
| unknown method, `ping` and `logging/setLevel` (both removed in this revision) | **404**, JSON-RPC -32601 |
| `_meta` keys given in both spellings with different values (`protocolVersion`, `clientInfo`, `clientCapabilities`, `logLevel`) | 400, JSON-RPC -32602 |

- **Every era**: a legacy request is executed all the same, so headers that are PRESENT are validated for it too: `Mcp-Method`
  and `Mcp-Name` (with the same base64 sentinel decoding) must agree with the body, and an `MCP-Protocol-Version` header that
  names a served version must equal a served body `_meta` protocol version (either spelling); otherwise 400 + -32020 and no
  provider call. Absent headers stay accepted on legacy requests.
- Header names are case-insensitive, values case-sensitive. A value that is not plain ASCII is sent as
  `=?base64?<base64 of UTF-8>?=`; fairyfly decodes that sentinel (strict: standard padded base64, the closing `?=` is
  required) before comparing, and an invalid sentinel is a `-32020` mismatch. Values are shortened to 100 characters and
  control characters are escaped in error messages.
- The prefixed `_meta` keys (`io.modelcontextprotocol/protocolVersion`, `clientInfo`, `clientCapabilities`, `logLevel`)
  are the spec's names; the plain spellings are still accepted and the prefixed one wins. The 400 bodies are always JSON-RPC
  error objects (id echoed), which is what a probing client inspects to recognise a modern server; a client may probe
  with a modern request first and fall back to `initialize` on any other answer.
- Every modern result carries `resultType: "complete"` and `_meta["io.modelcontextprotocol/serverInfo"]` (`name`,
  `version`); `tools/list` adds `ttlMs: 30000`, `cacheScope: "private"` and a deterministic order. `notifications/*` and
  client responses are answered 202 with no body (the spec defines no header rules for them). `logging/setLevel` is
  gone in this era, so no `notifications/message` is ever emitted.
- GET and DELETE are 405 (`Mcp-Session-Id` and `Last-Event-ID` are ignored: no session, no resumption); an invalid
  `Origin` is 403; SSE responses carry `X-Accel-Buffering: no` and `: keep-alive` comments; closing the response
  stream cancels the call.

### HTTP status and error mapping

Failures before a tool runs are plain JSON `{"error_code","message"}` (or a JSON-RPC error object for protocol
problems); tool failures are normal `200` JSON-RPC results with `isError: true` whose text starts with
`ERROR <CODE>:`.

| Status | Code | Cause |
|---|---|---|
| 200 | tool result / JSON-RPC error | including tool errors (`SCOPE_DENIED`, `READ_ONLY`, `TCODE_DENIED`, `SYSTEM_DENIED`, `SYSTEM_UNKNOWN`, `CONNECTION_DENIED`, `RATE_LIMITED`, `READ_ONLY_REFUSED`, `SERVER_BUSY`, `CALL_TIMEOUT`) |
| 202 | none | a notification or a response was posted |
| 400 | JSON-RPC -32700 / -32600 | body is not valid JSON / not a valid JSON-RPC message |
| 400 | JSON-RPC **-32020** | modern request: `MCP-Protocol-Version`, `Mcp-Method` or `Mcp-Name` header missing, invalid or not matching the body; any era: such a header present but not matching the body |
| 400 | JSON-RPC **-32602** | `_meta` key given twice (prefixed and plain) with different values |
| 404 | JSON-RPC **-32601** | modern request: unknown method, `ping`, `logging/setLevel` |
| 400 | JSON-RPC **-32022** | unsupported protocol version; `error.data.supported` lists ours |
| 401 | `AUTH_REQUIRED` | no bearer token, or no token exists on the server yet (with `WWW-Authenticate: Bearer realm="fairyfly"`) |
| 401 | `TOKEN_INVALID`, `TOKEN_EXPIRED`, `TOKEN_REVOKED` | malformed/unknown/wrong secret (deliberately not distinguishable), expired, revoked |
| 403 | `ADDRESS_NOT_ALLOWED` | peer address outside the server's `--allow-ip` list (checked first, before everything else; loopback is exempt unless strict mode is enabled) |
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
[internal/OPEN_WORK.md](internal/OPEN_WORK.md)).

## Security model

The security layers (TLS in the kernel, client allowlist, tokens, scopes and allowlists, request limits, audit,
read-only guard) and the full threat model with residual risks are in [SECURITY.md](SECURITY.md#remote-access).

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
# stateless discovery (a modern request needs the standard headers)
curl -sS "${CA[@]}" "${H[@]}" -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: server/discover' -X POST "$URL" -d '{"jsonrpc":"2.0","id":2,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}'
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

- **`--allow-selection-input` and execution (read this before choosing `--tcode`)**: typing a selection is only half of the
  job; Enter or F8 on the initial screen EXECUTES it (the key policy allows both, by design). The T-code allowlist of a token
  created with `--allow-selection-input` must therefore contain only transactions whose execution is read-only (selection
  screens of reports that do not update data). fairyfly cannot know what a custom report or a Z-transaction does behind F8;
  the answer to "F8 can run a report that writes" is exactly this rule, and it is already true for every read-only token with
  an allowlist (Enter/F8 are allowed keys for them too). The fill result echoes the value read back from the control, except
  for credential fields; audit records never contain the typed value.
- **Rotate**: `fairyfly mcp token rotate NAME` prints a new secret and the old one stops working at once.
  A running server sees changes made by another process within 5 seconds.
- **Revoke**: `fairyfly mcp token revoke NAME`. The record stays (marked revoked) in the Credential Manager entry
  `fairyfly-mcp:<name>`; **delete** it with `fairyfly mcp token delete NAME --yes` (works for revoked tokens too, is
  audited, needs `--yes`; `cmdkey /delete:fairyfly-mcp:NAME` also works).
- **List**: `fairyfly mcp token list` (id prefix, scopes, systems, T-codes, allow_navigation, allow_selection_input, rate, IPs, expiry, revoked; never
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
| 403 `ADDRESS_NOT_ALLOWED` | the client address is not in the server's `--allow-ip` / `server.allow_ip`; loopback is exempt unless `allow_ip_include_loopback` is enabled |
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
| `INPUT_SCREEN_DENIED` / `INPUT_TARGET_DENIED` / `INPUT_NOT_ALLOWED` | a token with `--allow-selection-input` typed outside the initial screen of its transaction (start it again with `gui_transaction_start`), into a grid cell, the command field or a password/credential field, or has no T-code allowlist |
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
| 6 | `initialize` (legacy, no extra header), then `server/discover` with `MCP-Protocol-Version: 2026-07-28` and `Mcp-Method: server/discover`; a modern `tools/list` without those headers | the first two answer 200 (discover lists `supportedVersions`); the headerless modern request is 400 `-32020` |
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
