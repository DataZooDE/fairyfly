# Fronting fairyfly MCP with IIS

`fairyfly mcp iis setup|status|remove` puts IIS (URL Rewrite + Application Request Routing) in front of the
fairyfly HTTP MCP server. IIS terminates TLS, restricts client IPs and forwards exactly one path, `/mcp`,
to the fairyfly server on loopback. Everything else gets a 404.

```
MCP client --https:8443--> IIS (TLS, IP allow-list, header hygiene) --http--> 127.0.0.1:8383  fairyfly mcp --http
                            /mcp only, everything else 404               X-Fairyfly-Proxy-Secret + X-Forwarded-*
```

Related work, delivered by sibling phases of the remote-MCP effort: the server side is started with
`fairyfly mcp --http` (phase 1, protocol) and clients authenticate with bearer tokens from
`fairyfly mcp token create` (phase 2, auth). This page only covers the IIS side. Until those phases are
merged the commands below that start the server or create tokens will not exist yet.

## Security model

- **Bearer tokens are the authentication.** IIS Windows/Basic authentication is deliberately not used and no
  authentication module is configured in `web.config`.
- **The IIS IP restriction is the second factor.** `ipSecurity allowUnlisted="false"` with the addresses you pass
  in `--allow-ip`. Setup refuses to run without `--allow-ip`; `--allow-any-ip` disables the restriction and prints
  a loud warning.
- **Proxy secret.** Setup generates 32 random bytes (hex), stores them in Windows Credential Manager
  (`fairyfly:fairyfly-mcp-proxy`) and injects them into `web.config` as the request header
  `X-Fairyfly-Proxy-Secret`. fairyfly only trusts `X-Forwarded-For` / `X-Forwarded-Proto` when this secret matches.
  The secret is printed once at creation (or rotation) and never again; it is not written to any log or to the
  audit trail. `web.config` is ACL'd to the app pool identity, Administrators and SYSTEM.
- **Header hygiene.** A first rewrite rule clears client-supplied `X-Forwarded-*`, `X-Real-IP`, `Forwarded` and
  `X-Fairyfly-Proxy-Secret`; the proxy rule then sets trusted values (`{REMOTE_ADDR}`, `https`, `{HTTP_HOST}`,
  the secret). These server variables are added to `allowedServerVariables` of the site by setup.
- Other hardening: request body limit 1 MiB, only GET/POST/DELETE/OPTIONS, `Server` header removed, HSTS,
  no directory browsing, no default document, compression off (SSE must not be buffered or compressed).
- The upstream must be loopback (`127.0.0.1`, `localhost`, `[::1]`).

## Prerequisites

`setup` detects these and refuses with `IIS_PREREQUISITE_MISSING` (listing the missing items and a fix hint each)
when something is absent. **fairyfly never installs software.** Run `setup --dry-run` to see the checklist.

| Item | Install |
| --- | --- |
| Administrator rights | start PowerShell as administrator |
| IIS role + management scripts | client: `Enable-WindowsOptionalFeature -Online -FeatureName IIS-WebServerRole,IIS-ManagementScriptingTools -All`; server: `Install-WindowsFeature Web-Server,Web-Scripting-Tools`; or `dism /online /enable-feature /featurename:IIS-WebServerRole /all` |
| IP and Domain Restrictions | `Enable-WindowsOptionalFeature -Online -FeatureName IIS-IPSecurity -All` / `Install-WindowsFeature Web-IP-Security` |
| URL Rewrite module | `choco install urlrewrite -y` or <https://www.iis.net/downloads/microsoft/url-rewrite> |
| Application Request Routing 3.0 | `choco install iis-arr -y` or <https://www.iis.net/downloads/microsoft/application-request-routing> |
| ARR proxy enabled | IIS Manager, server node, Application Request Routing Cache, Server Proxy Settings, tick "Enable proxy" (or `Set-WebConfigurationProperty -PSPath 'MACHINE/WEBROOT/APPHOST' -Filter 'system.webServer/proxy' -Name enabled -Value $true`) |

## Setup, step by step

1. Start an elevated PowerShell and preview:
   ```powershell
   fairyfly mcp iis setup --hostname mcp.example.com --self-signed --allow-ip 10.0.0.0/8,192.168.1.5 --dry-run
   ```
   The plan lists every step as `would_create`, `would_update` or `unchanged`; nothing is changed.
2. Apply with `--yes` (add `--port 8443`, `--upstream http://127.0.0.1:8383`, `--site-name fairyfly-mcp`,
   `--site-path C:\inetpub\fairyfly-mcp`, `--open-firewall` as needed):
   ```powershell
   fairyfly mcp iis setup --hostname mcp.example.com --self-signed --allow-ip 10.0.0.0/8 --open-firewall --yes
   ```
   Steps: certificate, proxy secret, site directory, application pool (No Managed Code, ApplicationPoolIdentity),
   site with an https-only SNI binding, `allowedServerVariables` + `ipSecurity` unlock, `web.config`, ACL,
   a non-secret manifest `fairyfly-iis.json` in the site directory, the exported `.cer` (self-signed only),
   the firewall rule (only with `--open-firewall`).
3. Every step is idempotent. Re-running `setup` reports `unchanged`; `--rotate-secret` issues a new secret.
   If a step fails, the error lists the applied steps and how to continue: fix the cause and re-run, or undo with
   `fairyfly mcp iis remove --yes`.
4. Start the server and create a token (sibling phases): `fairyfly mcp --http`, `fairyfly mcp token create <name>`.
5. Check: `fairyfly mcp iis status`.

Output format follows `--output json|markdown|toon`.

### CA certificate instead of self-signed

Import a certificate with private key into `LocalMachine\My` (its SAN or CN must cover the host name; a wildcard
covers one label) and pass `--cert-thumbprint <40 hex>`. Setup verifies existence, private key, expiry and host
name and warns below 30 days.

### Self-signed certificates: client trust

With `--self-signed`, setup exports `<site-path parent>\<site-name>-<hostname>.cer` (for the defaults
`C:\inetpub\fairyfly-mcp-mcp.example.com.cer`). Every client must trust it.

- Linux: `sudo cp fairyfly-mcp-mcp.example.com.cer /usr/local/share/ca-certificates/fairyfly-mcp.crt`
  (convert first if needed: `openssl x509 -inform der -in file.cer -out fairyfly-mcp.crt`), then
  `sudo update-ca-certificates`. For a single tool: `curl --cacert fairyfly-mcp.crt ...`,
  `NODE_EXTRA_CA_CERTS=/path/fairyfly-mcp.crt`.
- Windows: `Import-Certificate -FilePath .\fairyfly-mcp-mcp.example.com.cer -CertStoreLocation Cert:\LocalMachine\Root`
  (elevated) or double-click the file and install to "Trusted Root Certification Authorities".

## Verifying with curl

```bash
curl -i --cacert fairyfly-mcp.crt https://mcp.example.com:8443/mcp                       # 401 without a token
curl -i --cacert fairyfly-mcp.crt https://mcp.example.com:8443/other                     # 404
curl -N --cacert fairyfly-mcp.crt -H "Authorization: Bearer <token>" \
     -H "Accept: text/event-stream, application/json" -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","id":1,"method":"ping"}' https://mcp.example.com:8443/mcp
```

A request from an address outside `--allow-ip` is answered by IIS with 403 before fairyfly sees it.

## status and remove

`fairyfly mcp iis status` returns a checklist: prerequisites, site state, app pool, https binding, certificate
expiry (warning below 30 days), stored secret, `web.config` drift (comparison of hashes of the expected and the
found content with the secret masked; the content itself is never printed), firewall rule and upstream reachability
(TCP connect to the loopback port, 1 s). Summary is `healthy`, `degraded` or `unhealthy`.

`fairyfly mcp iis remove [--remove-cert] [--remove-firewall] [--delete-secret] [--dry-run] --yes` removes the site,
application pool, `web.config`, manifest and exported `.cer`. The certificate is only removed with `--remove-cert`
and only if setup created it (self-signed); the proxy secret is kept unless `--delete-secret`.

## Troubleshooting

- **502.3 Bad Gateway**: nothing listens on the upstream (`fairyfly mcp --http` not running or another port), or the
  ARR proxy timeout was exceeded. `status` shows the upstream check.
- **502.3 with "connection refused" only over IPv6**: use `--upstream http://127.0.0.1:PORT`, not `localhost`.
- **Events arrive in one burst (SSE buffering)**: the generated `web.config` sets `responseBufferLimit="0"` on the
  ARR `proxy` element, disables compression and empties `Accept-Encoding` towards fairyfly. Check `status` for drift
  (a hand-edited `web.config` or a re-applied server-level ARR setting) and that no other module (WAF, antivirus
  proxy) sits in between.
- **ARR proxy disabled** (500 / 404 after the rewrite): enable it under Server Proxy Settings; `status` reports
  `arr_proxy_enabled` as missing.
- **500.19 on the site**: a section is still locked. Re-run `setup` (it unlocks `ipSecurity`/`proxy` for the site
  and adds `allowedServerVariables`), or check `applicationHost.config`.
- **Idle timeout / dropped long calls**: the proxy timeout is 5 minutes (MCP call timeout default 120 s). Keep the
  app pool idle time-out (default 20 min) and any load balancer in front larger than that; SSE streams keep the
  worker busy. Set the pool's Start Mode to AlwaysRunning if the first request after idle is slow.
- **TLS errors on clients**: self-signed certificate not trusted (see above), or the host name does not match.
- **403 from IIS**: the client IP is not in `--allow-ip`.
- **`credentials list` shows `fairyfly-mcp-proxy`**: that is the proxy secret entry (user `proxy`); it is not a SAP
  connection.
