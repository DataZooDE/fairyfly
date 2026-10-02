# MCP setup: https listener on http.sys

`fairyfly mcp --http` listens through the Windows HTTP Server API (http.sys). TLS is terminated in the kernel driver with a certificate from `LocalMachine\My`, so the server process itself runs as an ordinary, unelevated user and never touches the private key. Four things must exist on the machine first; `fairyfly mcp setup` creates them with one UAC prompt, verifies them with a real TLS round trip, and `fairyfly mcp teardown` removes them again.

```powershell
fairyfly mcp setup --self-signed --hostname sapbox.corp.example --open-firewall --dry-run   # plan only, changes nothing
fairyfly mcp setup --self-signed --hostname sapbox.corp.example --open-firewall            # asks, one UAC prompt, verifies
fairyfly mcp doctor                                                                         # read-only health check
fairyfly mcp teardown                                                                       # removes only what setup created
```

## What the machine needs, and why elevation

| Step id | What it does | Needs elevation |
|---|---|---|
| `certificate` | `--self-signed`: creates `CN=<host>` (SAN = host, RSA 2048, SHA-256, 2 years, FriendlyName `fairyfly-mcp <host>`) in `LocalMachine\My`, or reuses a valid one. `--cert-thumbprint T`: uses an existing certificate (never created or removed by fairyfly) | yes (creating) |
| `urlacl` | URL reservation `https://+:PORT/mcp/` for the server account with SDDL `D:(A;;GX;;;<SID>)` (existing entries are kept; the SID is added). Without it http.sys refuses the bind of an unelevated process | yes |
| `sslcert` | binds the certificate to `0.0.0.0:PORT` and `[::]:PORT` (SHA-1 thumbprint, store `MY`, AppId `{8e2b5c3a-4d17-4f6a-b9c0-7a1d3e5f2b64}`, no client-certificate negotiation) | yes |
| `firewall` | only with `--open-firewall`: inbound TCP rule with the display name `fairyfly MCP HTTPS <port>` and a unique internal Name `fairyfly-mcp-https-<port>-<8 hex>` (recorded in the manifest as `firewall_rule_name`). A pre-existing rule with the same display name is not claimed or duplicated: the step is `skipped` and the human item `firewall_foreign` explains | yes |
| `certificate_export` | writes the public certificate to `%LOCALAPPDATA%\fairyfly\fairyfly-mcp-<host>.cer` for the clients | no |
| `config` | writes a commented `mcp.yaml` (`tls: true`, `host: '+'`, `port`, `allowed_hosts: [host]`, `allow_ip`) when the file does not exist. It does not set `server.transport`: start the server explicitly with `fairyfly mcp --http` (or `--tray --http`), so a plain stdio `fairyfly mcp` is unaffected. An existing file is never rewritten; differing keys are listed under "Left for a human" | no |
| `manifest` | `%LOCALAPPDATA%\fairyfly\mcp-setup.json` (schema 1): what was created (including `config_created` and `config_sha256` of the mcp.yaml text, and `urlacl_created`/`urlacl_sddl`), for `teardown` and `doctor` | no |

Only the four elevated steps run in the elevated process. `setup` computes the target account's SID before elevating and writes it into a plan file; the elevated child (`fairyfly mcp setup --apply-plan FILE --result-file FILE`, started once with `ShellExecute runas`) re-validates every field of the plan file, executes the steps and writes per-step results. The parent then exports the certificate, writes the config and manifest, and verifies. The plan file lives under the per-user `%LOCALAPPDATA%\fairyfly\run\` (random name, created with `CREATE_NEW` and no write sharing, held open until the child exits) and is deleted afterwards. The child does not trust it by path: the exact plan bytes are bound to what the parent approved by `--plan-sha256 <hex>`, `--plan-nonce`, `--plan-parent-pid`, `--plan-sid` (setup) and `--plan-force-binding` on the elevated command line (visible in the UAC consent data). The child reads the file once, hashes those bytes and refuses with `INVALID_PLAN` (no change) on a hash, nonce, pid, SID or `force_binding` mismatch or when the plan is older than 10 minutes; it then parses the same in-memory bytes. The result goes to a randomly named file that the parent trusts only after a normal exit (0 or 1), at most 1 MiB, valid JSON and the echoed nonce.

An SslBinding whose AppId is not fairyfly's belongs to somebody else. The `sslcert` step is then `blocked` (exit 1, `PLAN_BLOCKED`) unless you pass `--force-binding`; `teardown` never removes such a binding.

### Teardown ownership rules

`teardown` removes only what the manifest proves setup created, and re-checks it before every delete (the elevated child re-checks the URL reservation itself). Everything else is skipped and named under "Left for a human".

- **URL reservation**: the manifest records `urlacl_created` (setup created the reservation, as opposed to finding one) and `urlacl_sddl` (the SDDL http.sys reported right after setup). Teardown removes the reservation only when `urlacl_created` is true and the current SDDL still equals `urlacl_sddl`. A reservation that existed before setup, that somebody changed afterwards, that belongs to a manifest written by an older version (no such fields), or that is found with `--hostname/--port` and no manifest, is left in place; the human item `urlacl_foreign` prints `netsh http delete urlacl url=<prefix>`.
- **Exported certificate file**: the manifest is user-writable, so its `cer_path` is never used to delete anything. Teardown derives the path from the validated host name and the fairyfly data directory (`%LOCALAPPDATA%airyflyairyfly-mcp-<host>.cer`, the rule setup uses), refuses (step `skipped`, human item `cer_path_untrusted`) when the manifest records a different path or host name, and deletes the file only when the certificate in it (DER or PEM) has the SHA-1 thumbprint the manifest records, checked again right before the delete (otherwise `cer_file_foreign`). Without a manifest nothing ties the file to a certificate, so it is left for a human.
- **Firewall rule**: removed by the unique internal Name recorded in the manifest (`firewall_rule_name`, which must match the fairyfly pattern), never by display name, so a rule somebody else created with the same display name is never touched. A manifest from an older version records only the display name: the step is `skipped` and the human item `firewall_legacy` gives the manual `Remove-NetFirewallRule` command. Without a manifest no rule is removed.
- **TLS binding**: removed only when its AppId is fairyfly's (unchanged rule).

### Verification (the last step)

After applying, setup re-diagnoses the machine and proves the listener works: an unauthenticated `POST /mcp` must answer `401 AUTH_REQUIRED` with a `WWW-Authenticate` header, over TLS whose certificate SHA-1 equals the bound thumbprint (a thumbprint-pinned WinHTTP client: any chain error is ignored only because the pin is checked, and no credential is ever sent). The negotiated protocol is reported; below TLS 1.2 it adds a "Left for a human" item. The round trip binds a deny-all in-process server on the real prefix for about two seconds, or talks to the already running server (tray) when the port is served. Failure exits 1 with `VERIFY_FAILED`.

## Flags

```
fairyfly mcp setup [--hostname H] [--port 8443]
                   (--self-signed | --cert-thumbprint T | --no-tls)
                   [--allow-ip CIDR,...] [--user DOMAIN\user] [--open-firewall] [--force-binding]
                   [--dry-run] [--yes] [--non-interactive] [--print-runbook] [--output text|json]
fairyfly mcp teardown [--hostname H] [--port P] [--keep-cert] [--keep-firewall]
                      [--dry-run] [--yes] [--non-interactive] [--output text|json]
fairyfly mcp cert export [--out PATH] [--format der|pem] [--hostname H] [--port P] [--output text|json]
```

- `--hostname` defaults to this machine's lower-cased DNS name; `--port` to 8443 (8383 with `--no-tls`). Exactly one of `--self-signed`, `--cert-thumbprint`, `--no-tls` (else `INVALID_ARGUMENT`, exit 2). Host name, thumbprint (40 hex digits), port, CIDR and account are validated before anything is planned; nothing user-supplied is ever spliced into PowerShell (constant scripts, JSON parameters through an environment variable).
- `--user` is the account that will run the server (default: the current user, SID taken from the process token).
- `-c/--config PATH` of `fairyfly mcp` decides which `mcp.yaml` the `config` step looks at.

### Exit codes and consent

| Situation | Exit | Code |
|---|---|---|
| done, nothing to do (`nothing - already set up.` / `nothing - already removed.`), `--dry-run`, `--print-runbook` | 0 | |
| a step failed, verification failed, UAC declined, plan blocked, `mcp cert export` without a certificate | 1 | `STEP_FAILED`, `VERIFY_FAILED`, `ELEVATION_DECLINED`, `PLAN_BLOCKED`, `CERT_NOT_FOUND`, `TEARDOWN_INCOMPLETE` |
| bad arguments, confirmation refused, elevation needed in `--non-interactive`, `FAIRYFLY_READ_ONLY=1`, teardown without manifest but with leftovers | 2 | `INVALID_ARGUMENT`, `CONFIRMATION_REQUIRED`, `ELEVATION_REQUIRED`, `READ_ONLY`, `MANIFEST_MISSING`, `USER_NOT_FOUND` |

`--yes` applies without asking. On a terminal, without `--yes`, setup asks `Change this machine (<host>)? [y/N]`. Without a terminal or with `--non-interactive` and no `--yes` it refuses (`Refusing to change <host> without confirmation`). `--non-interactive --yes` never shows a UAC prompt: if elevation is needed it exits 2 with `ELEVATION_REQUIRED` and prints the exact command to run from an elevated prompt. `FAIRYFLY_READ_ONLY=1` (or `--read-only`) refuses to change the machine; `--dry-run` and `--print-runbook` always work. Setup is idempotent: a second run plans nothing, prints `nothing - already set up.` and asks for nothing.

### JSON output (`--output json`)

```json
{
  "status": "success",
  "data": {
    "operation": "setup",
    "dry_run": false,
    "url": "https://sapbox.corp.example:8443/mcp",
    "elevation": "standard_user",
    "nothing_to_do": false,
    "diagnosis": [{"id": "urlacl", "status": "missing", "detail": "..."}],
    "steps": [{"id": "certificate", "title": "...", "status": "created", "detail": "...", "elevated": true}],
    "human": [{"id": "trust_certificate", "text": "..."}],
    "verify": {"status": "ok", "protocol": "TLS 1.3", "http_status": 401, "thumbprint_match": true, "detail": ""},
    "next_steps": ["fairyfly mcp token create NAME ..."]
  },
  "error": {"code": "VERIFY_FAILED", "message": "..."}
}
```

`error` exists only when `status` is `error`. Step statuses: dry run `would_create | would_update | unchanged | skipped | blocked`; applied `created | updated | unchanged | skipped | failed`; teardown adds `would_remove | removed`. `elevation` is `elevated | admin_filtered | standard_user | unknown`. `verify` is absent for dry runs. Diagnosis statuses: `ok | missing | mismatch | warn | info | skip`. `--print-runbook --output json` returns `data.runbook`.

## Runbook: doing it by hand

`fairyfly mcp setup ... --print-runbook` prints these commands with your values (run in an elevated PowerShell):

```powershell
$c = New-SelfSignedCertificate -Subject 'CN=sapbox.corp.example' -DnsName 'sapbox.corp.example' `
  -CertStoreLocation Cert:\LocalMachine\My -FriendlyName 'fairyfly-mcp sapbox.corp.example' `
  -NotAfter (Get-Date).AddDays(730) -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 `
  -KeyUsage DigitalSignature,KeyEncipherment -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.1')
netsh http add urlacl url=https://+:8443/mcp/ user=CORP\jr
netsh http add sslcert ipport=0.0.0.0:8443 certhash=$($c.Thumbprint) appid={8e2b5c3a-4d17-4f6a-b9c0-7a1d3e5f2b64} certstorename=MY
netsh http add sslcert ipport=[::]:8443   certhash=$($c.Thumbprint) appid={8e2b5c3a-4d17-4f6a-b9c0-7a1d3e5f2b64} certstorename=MY
New-NetFirewallRule -DisplayName 'fairyfly MCP HTTPS 8443' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8443 -Profile Any
Export-Certificate -Cert $c -FilePath "$env:LOCALAPPDATA\fairyfly\fairyfly-mcp-sapbox.corp.example.cer" -Type CERT
```

Then create `mcp.yaml` with the keys `server.tls: true`, `server.host: '+'`, `server.port: 8443`, `server.allowed_hosts: [sapbox.corp.example]` (and `server.allow_ip: [...]`). Without the manifest, `teardown` needs `--hostname/--port`, or remove the objects with `netsh http delete sslcert ipport=...`, `netsh http delete urlacl url=...`, `Remove-NetFirewallRule` and `Remove-Item Cert:\LocalMachine\My\<thumbprint> -DeleteKey`.

## Trusting the certificate on clients

`--self-signed` certificates are trusted by nobody until you import the exported `.cer`; a `--cert-thumbprint` certificate from your CA usually needs nothing. `fairyfly mcp cert export [--format pem] [--out PATH]` prints the same hints:

- **Windows client** (DER): `certutil -addstore -user Root "fairyfly-mcp-<host>.cer"` (machine-wide: elevated `certutil -addstore -f Root ...`).
- **Linux / curl**: `openssl x509 -inform der -in fairyfly-mcp-<host>.cer -out fairyfly-mcp-<host>.pem`, then `curl --cacert fairyfly-mcp-<host>.pem https://<host>:8443/mcp`.
- **Node clients (Claude Code, mcp-remote)**: `export NODE_EXTRA_CA_CERTS=/path/fairyfly-mcp-<host>.pem`.
- **macOS**: `sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain fairyfly-mcp-<host>.pem`.
- **Claude Desktop** (through mcp-remote): add `"env": {"NODE_EXTRA_CA_CERTS": "<path to .pem>"}` to the server entry in `claude_desktop_config.json` and restart Claude Desktop.

Every client must also resolve `<host>` to this machine (DNS record or hosts file entry).

## `mcp doctor`

Read-only and never elevated. The http.sys checks follow `tokens`; `fairyfly mcp --http doctor` (or `server.transport: http`) judges them, as does an existing setup manifest.

| Check id | Meaning | Remedy on FAIL/WARN |
|---|---|---|
| `elevation` | info: `elevated`, `admin_filtered`, `standard_user` | |
| `setup_manifest` | setup recorded in `mcp-setup.json` | `fairyfly mcp setup ...` |
| `urlacl` | reservation of the effective prefix covers the current user (loopback prefixes need none on this Windows build: PASS) | `fairyfly mcp setup ...` |
| `sslcert` | bound on `0.0.0.0:PORT` (and `[::]`), AppId ours, matches the installed certificate; SKIP in `--no-tls` mode | `fairyfly mcp setup ...` |
| `certificate` | in `LocalMachine\My`, private key, not expired (WARN below 30 days), SAN matches the host | re-run setup / `teardown` then `setup` |
| `firewall` | only when the manifest recorded a rule | `fairyfly mcp setup ... --open-firewall` |
| `port` | free, or served (by the tray) | |
| `tls_handshake` | only when something listens: pinned handshake, protocol, thumbprint match; WARN on mismatch or below TLS 1.2 | rebind / Schannel policy |
| `legacy_proxy_secret` | WARN when the retired credential `fairyfly:fairyfly-mcp-proxy` still exists | `cmdkey /delete:fairyfly:fairyfly-mcp-proxy` |

## Teardown

`fairyfly mcp teardown` follows the manifest: it removes both TLS bindings (only when their AppId is fairyfly's), the URL reservation(s), the firewall rule (only when setup recorded one, not with `--keep-firewall`), the certificate and its private key (only when setup created it as `self-signed`, never a user-supplied thumbprint, not with `--keep-cert`), the exported `.cer`, the `mcp.yaml` (step `config`; only when setup created it and its sha256 still equals the `config_sha256` recorded in the manifest, so an edited file stays and is named under "Left for a human") and finally the manifest. One UAC prompt. A second run prints `nothing - already removed.`. Without a manifest it needs `--hostname` and/or `--port` (`MANIFEST_MISSING`, exit 2, when fairyfly reservations exist on the default ports 8443/8383 but nothing identifies them; a clean machine is simply "nothing"). If `mcp.yaml` still says `tls: true` teardown lists that under "Left for a human". Clients keep trusting the old certificate until you remove it.

## Development: `--no-tls` and the test reservation

`fairyfly mcp setup --no-tls [--port 8383]` prepares plain HTTP on `127.0.0.1` (loopback only; refuses `--open-firewall` and `--allow-ip`). On this Windows build an unelevated process binds a loopback prefix without a URL reservation, so the `urlacl` step is `skipped` ("not required for loopback prefixes ... verified by a bind probe") and setup needs no elevation. If your build does refuse the bind (`no_url_reservation`), reserve the prefix once from an elevated prompt. The loopback unit tests use the fixed port 18383:

```powershell
netsh http add urlacl url=http://127.0.0.1:18383/mcp/ user=%USERDOMAIN%\%USERNAME%
netsh http delete urlacl url=http://127.0.0.1:18383/mcp/       # to undo
```

One port is one reservation: there are no ephemeral ports with http.sys.

## Troubleshooting

- **`ACCESS_DENIED` / `no_url_reservation` when starting the server**: the prefix has no reservation for your account. Run `fairyfly mcp doctor` (`urlacl`), then `fairyfly mcp setup ...`. For another account use `--user DOMAIN\user`.
- **`prefix_registered` / port in use**: another process (the tray, an old server, another product) already owns the prefix. `fairyfly mcp doctor` (`port`) shows it; setup's verify then talks to the running server instead of binding one. `PLAN_BLOCKED` on `sslcert` means a foreign AppId owns the port binding: pick another `--port`, or `--force-binding` if you are sure.
- **TLS below 1.2 negotiated**: the machine's Schannel policy still allows TLS 1.0/1.1; setup and `doctor` report the negotiated protocol and warn. Raise the policy (registry `SCHANNEL\Protocols`, or group policy); fairyfly does not change machine-wide crypto settings.
- **Hostname does not resolve**: setup's verify falls back to `127.0.0.1` (pinned by thumbprint) and still passes, but clients need a DNS record or hosts entry for `<host>`, and the certificate SAN must equal the name they use (`certificate` check: SAN).
- **UAC prompt declined**: exit 1, nothing changed; run again, or run the printed command in an elevated prompt with `--non-interactive --yes`.
- **Clients report an untrusted certificate**: import the exported `.cer` (see above); curl/Node need the PEM.
- **Renewing an expiring self-signed certificate**: `fairyfly mcp teardown` then `fairyfly mcp setup --self-signed ...` (the thumbprint changes, so clients must trust the new file). An expired or wrong-name certificate is replaced automatically by the next `setup`.

## For scripts (integration tests)

Use `--output json` and parse `status`, `data.steps[].status`, `data.verify.*`, `error.code`, and the exit code. `mcp setup --dry-run --output json` never changes anything and never prompts. `mcp setup ... --yes --output json` prompts for UAC once; `--non-interactive --yes` from an elevated shell never prompts. `mcp doctor --output json` returns `data.checks[]` with `id`, `status`, `message`, `remediation`.
