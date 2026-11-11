# Corporate proxy: manual test before the desktop release

Owner decision (2026-11-10): the first desktop release with corporate proxy support (#149, merged 448b6975) goes out
only after this checklist has passed in a network with a real proxy: a customer network, or a test setup with a
Windows domain (appendix). The unit tests (`tests/unit/test_system_proxy.cpp`, libffl's proxy tests against a local
proxy) cover the decisions and the CONNECT exchange; they cannot cover WinHTTP's PAC/WPAD evaluation, SSPI against a
real KDC, or a real proxy's behaviour. Repeat the checklist for a later release that changes `src/link/system_proxy*`
or `third_party/libffl/src/proxy.*`.

What the code does is described in [LINK.md, Corporate proxies](../LINK.md#corporate-proxies). The expected results
below quote the exact strings the code writes; `<proxy>` is the proxy as written in the settings (`host:port`),
`<gateway>` the gateway's host name (`fairyfly.io`, or the test gateway).

**At a customer (pilot).** Owner decision (2026-11-10): the test runs at a customer/pilot. Their IT gets the bilingual
one-pager [PROXY_PILOT_IT_SHEET.md](PROXY_PILOT_IT_SHEET.md) beforehand; the session follows
[PROXY_PILOT_RUN_SHEET.md](PROXY_PILOT_RUN_SHEET.md), which says which cases are mandatory for their proxy type. Cases
their network cannot do stay open for the test setup in the appendix.

## Prerequisites

- A **Windows 10/11 PC joined to the domain**, signed in as a domain user (`whoami` shows `CORP\<user>`). A second
  domain user who is *not* allowed through the proxy is useful for case 4c.
- A **proxy** that can be switched between these modes (separate instances or ports, or a reconfigure per case):
  - no authentication;
  - Negotiate (Kerberos) and NTLM offered together, with an SPN `HTTP/<proxy host>` registered for it;
  - NTLM only;
  - Basic only (a local test user and password).
- A **PAC file** that returns `PROXY <proxy>` for the gateway, served
  - over `https://` with a certificate the PC trusts (for example from the domain's AD CS), and
  - over plain `http://`, reachable both under an FQDN (`http://pac.corp.example/proxy.pac`) and under a single-label
    name (`http://wpad/wpad.dat`).
- **WPAD**: the PAC file published as `http://wpad.<dns suffix>/wpad.dat` (DNS A record `wpad`) or through DHCP
  option 252, so that "Automatically detect settings" finds it.
- **Group policy** (or local policy for a test) that can set the automatic configuration script
  (`AutoConfigURL` under `HKLM` or `HKCU\Software\Policies\Microsoft\Windows\CurrentVersion\Internet Settings`).
- Local administrator rights on the PC for `netsh winhttp` and the `HKLM\SOFTWARE\Policies\DataZoo\fairyfly` policy.
- Access to the proxy's access log (for squid: `/var/log/squid/access.log`), which shows the authenticated user per
  CONNECT: `alice@CORP.EXAMPLE` for Kerberos, `CORP\alice` (or `alice`) for NTLM, the Basic user for Basic.
- A test organisation on the gateway under test, an account in it, and a phone or browser for pairing.
- The firewall blocks direct outbound 443 from the PC to the gateway, so that a direct connection cannot pass by
  accident (otherwise case 1 is the only case that may connect without the proxy). Keep it open for case 1, or run
  case 1 from another network.

## The build under test

- The release candidate: the `fairyfly-windows-x64` artifact (`fairyfly.exe`, `fairyfly.msi`, unsigned) of the
  `build.yml` run on the commit to be tagged, at or after 448b6975:
  `gh run list -R DataZooDE/fairyfly -w build.yml -b main -L 5`, then
  `gh run download <run id> -R DataZooDE/fairyfly -n fairyfly-windows-x64`.
- Install `fairyfly.msi` (per user). Record `fairyfly --version` and the commit in the results table. A build of a commit between two releases
  reports the version of the last release (448b6975 says `2026.11.09`, like the release `v2026.11.09` without proxy
  support), so the commit is what identifies it.
- An unsigned build cannot be updated by `fairyfly update apply` from a signed release and vice versa; for case 10 an
  update *check* is enough.

## How to run a case

- **Proxy settings are cached for 5 minutes** inside a running fairyfly. After changing any proxy setting, quit the
  link (tray **Quit**, or Ctrl+C in the console) and start it again.
- Run the link in a PowerShell console so that every line is visible and kept (quit the tray first: a second link for
  the same pairing is refused with `LINK_ALREADY_RUNNING`):

      mkdir $env:USERPROFILE\proxy-test -Force
      fairyfly link run --log-level debug *>&1 | Tee-Object $env:USERPROFILE\proxy-test\case-<n>.txt

  The console shows the status lines (`connected to <gateway> as <account> in '<organisation>' – Ctrl+C stops`,
  `connection to <gateway> lost (<reason>) – reconnecting in <n> s`, `reconnected to <gateway>`) and, with
  `--log-level debug`, the proxy lines below.
- Cases marked **(tray)** are run with the tray instead (Start menu **fairyfly**); its log is
  `%USERPROFILE%\.fairyfly\logs\link.log` (tray menu **Open log**), which has the `info` and `warn` lines only: the
  tray sets its log level to `info` (`src/tray/tray_win32.cpp`), so `FFLYLOG_LEVEL=debug` has no effect there. Check a
  `debug` line (the tunnel's `authenticated with ...`) in the console.
- Check the current Windows proxy settings before each case:

      Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' |
        Select-Object ProxyEnable, ProxyServer, ProxyOverride, AutoConfigURL
      netsh winhttp show proxy
      reg query "HKCU\Software\Policies\Microsoft\Windows\CurrentVersion\Internet Settings" /v AutoConfigURL
      reg query "HKLM\Software\Policies\Microsoft\Windows\CurrentVersion\Internet Settings" /v AutoConfigURL
      reg query HKLM\SOFTWARE\Policies\DataZoo\fairyfly /v AllowLoginToDiscoveredProxies

  "Automatically detect settings" is visible in Settings › Network & Internet › Proxy.
- A connected link answers a tool call: in the web app or an AI assistant, read the SAP screen of this desktop
  (`gui_screen_read`) once per passing case. The web app's **My desktops** shows the desktop online.

The lines the code writes (`link.log` and the console with `--log-level debug`):

| When | Line |
|---|---|
| the proxy choice for a host changes (once, not per reconnect) | `proxy: <gateway> -> <routes> (<how>)`, `<how>` one of `PAC <url>`, `automatic detection (WPAD)`, `manual proxy settings`, `WinHTTP machine setting (netsh winhttp)`, `no proxy configured`, `... : bypass list`, `... : DIRECT` |
| the link connects through a proxy | `link: connecting through proxy <proxy> (from Windows proxy settings)` or `(from environment (HTTPS_PROXY))` |
| the tunnel is open (debug) | `proxy: tunnel to <gateway>:443 through <proxy> (<source>[, authenticated with Negotiate\|NTLM\|Basic])` |
| the link is up / down | `link up: device=... gateway=...` / `link down (<error>), reconnecting in <n> ms` |

## Cases

### 1. Direct, no proxy (unchanged behaviour)

Setup: manual proxy off, no script, "Automatically detect settings" off, `netsh winhttp reset proxy`, no
`HTTPS_PROXY`/`NO_PROXY`. Direct 443 to the gateway allowed.

Expected:
- `proxy: <gateway> -> DIRECT (no proxy configured)`; no `link: connecting through proxy` line.
- `connected to <gateway> as ...`; a tool call works. Tray (if used): teal icon, status `connected`.

### 2. Manual proxy without authentication

Setup: proxy in no-auth mode. Settings › Proxy › Manual proxy setup: address and port of the proxy, on. Direct 443
blocked.

Expected:
- `proxy: <gateway> -> <proxy> (manual proxy settings)`, `link: connecting through proxy <proxy> (from Windows proxy settings)`,
  `proxy: tunnel to <gateway>:443 through <proxy> (Windows proxy settings)` (no "authenticated with").
- Connected; a tool call works. The proxy's access log shows `CONNECT <gateway>:443` with no user.
- 2b (optional): deny the gateway on the proxy (squid: `http_access deny CONNECT gateway_host` before the allow).
  Expected: `connection ... lost` / `link down (proxy refused CONNECT 403 Forbidden (<proxy> -> <gateway>:443, proxy from Windows proxy settings)), reconnecting in ...`.
  Allow it again: `reconnected to <gateway>`.

### 3. Manual proxy with the Windows login

3a **Kerberos (Negotiate)**: proxy offers Negotiate and NTLM, the proxy is set by its FQDN (the SPN is
`HTTP/<host as written>`; an IP address cannot get a Kerberos ticket).
- `proxy: tunnel to <gateway>:443 through <proxy> (Windows proxy settings, authenticated with Negotiate)`.
- `klist` on the PC lists a ticket for `HTTP/<proxy host>`; the proxy log shows `<user>@<REALM>`.
- Connected; a tool call works.

3b **NTLM**: proxy offers only NTLM.
- `... authenticated with NTLM`; the proxy log shows the user as NTLM reports it (`CORP\<user>`).
- Connected; a tool call works.

3c **netsh winhttp** (machine setting): manual proxy off, no script, no auto-detect;
`netsh winhttp set proxy proxy-server="<proxy>" bypass-list="<local>"` (elevated), proxy in Negotiate mode.
- `proxy: <gateway> -> <proxy> (WinHTTP machine setting (netsh winhttp))`, `... authenticated with Negotiate`.
- Afterwards `netsh winhttp reset proxy`.

### 4. Authentication failures with an explicit proxy

4a Proxy offers Negotiate/NTLM but the user is not allowed (sign in as the second user, or deny the user's group):
`proxy requires authentication: it refused the Negotiate credentials of this user (offers Negotiate, NTLM) (<proxy> -> <gateway>:443, proxy from Windows proxy settings)`
(or `NTLM`). The link keeps retrying; no password prompt appears.

4b (optional) Proxy closes the connection after each 407 (squid: `auth_param ntlm keep_alive off` plus
`client_persistent_connections off`): `proxy closed the connection during NTLM authentication (...)`.

### 5. HTTPS_PROXY override

Setup: Windows manual proxy set to an address that does not exist (`192.0.2.1:3128`) so that the Windows settings
would fail. In the console: `$env:HTTPS_PROXY = "http://<proxy>"`, proxy in Negotiate mode.
- `link: connecting through proxy <proxy> (from environment (HTTPS_PROXY))`; no `proxy: <gateway> -> ...` line;
  `proxy: tunnel to <gateway>:443 through <proxy> (environment (HTTPS_PROXY), authenticated with Negotiate)`.
- Connected.
- 5b `$env:HTTPS_PROXY = "https://<proxy>"`: refused with `HTTPS_PROXY: proxy scheme https:// is not supported (use http://host:port)`.
- 5c `$env:HTTPS_PROXY = "http://baduser:S3cret-Proxy-Test@<proxy>"` against the Basic-only proxy: the 407 is answered
  with that Basic credential; it is refused (`it refused the Basic credentials`) and the console and log never show
  `S3cret-Proxy-Test` (checked in case 13).
- `Remove-Item Env:HTTPS_PROXY` afterwards.

### 6. PAC script over https (trusted: Windows login sent)

Setup: manual proxy off, auto-detect off; Settings › Proxy › Use setup script: `https://pac.corp.example/proxy.pac`.
Proxy in Negotiate/NTLM mode.
- `proxy: <gateway> -> <proxy> (PAC https://pac.corp.example/proxy.pac)` (a query string is not shown),
  `... authenticated with Negotiate`. Connected.
- 6b Same script over http, set by **group policy** with the FQDN (`http://pac.corp.example/proxy.pac`, `gpupdate /force`,
  the `AutoConfigURL` policy value present): trusted as well, `... authenticated with Negotiate`. Connected.

### 7. PAC script over http, not trusted (no credentials)

Setup: script set **by the user** (not by policy) to `http://wpad/wpad.dat` (single label); proxy in Negotiate/NTLM mode.
- `proxy: <gateway> -> <proxy> (PAC http://wpad/wpad.dat)`.
- No credentials are sent (the proxy log shows only the anonymous 407). Error, retried:
  `proxy requires authentication (offers Negotiate, NTLM), and the proxy requires the Windows login (Negotiate) but was discovered automatically (WPAD): not sending credentials; ask IT to set the proxy explicitly (manual setting, netsh winhttp, PAC script or HTTPS_PROXY) (<proxy> -> <gateway>:443, proxy from Windows proxy settings)`
  and the warning `proxy: <proxy>: the proxy requires the Windows login (Negotiate) but was discovered automatically (WPAD): ...`.
  (The wording names WPAD also for an untrusted script; record it, it is the same rule.)
- 7b The same with a user-set `http://pac.corp.example/proxy.pac` (FQDN, but not by policy): the same refusal.
- 7c (optional) The script's web server requires Windows authentication (IIS, Windows auth on the PAC): for 7a no
  login is sent while downloading it, `proxy: <gateway> -> DIRECT (PAC http://wpad/wpad.dat failed (the proxy script's server refused the Windows login), no proxy configured)`
  or the next source; for 6b it is sent and the script loads.

### 8. WPAD-discovered proxy that requires authentication

Setup: no script, manual proxy off, "Automatically detect settings" on; WPAD answers with the proxy in Negotiate/NTLM mode.
Policy value absent.
- `proxy: <gateway> -> <proxy> (automatic detection (WPAD))`.
- Refused as in case 7 (`... but was discovered automatically (WPAD): not sending credentials; ask IT to set the proxy explicitly ...`),
  retried; the proxy log shows no user. **(tray)**: run this case with the tray; the same lines are in `link.log`
  (they are `info`/`warn`).
- 8b In the console (`authenticated with` is a `debug` line, which the tray does not log). Elevated: `reg add HKLM\SOFTWARE\Policies\DataZoo\fairyfly /v AllowLoginToDiscoveredProxies /t REG_DWORD /d 1 /f`.
  The policy is read at each proxy connection: without restarting the link, the next reconnect succeeds with
  `... authenticated with Negotiate`; `reconnected to <gateway>`.
- 8c `/d 0` (or delete the value): after the next reconnect (or restart) it is refused again.
- 8d WPAD with the proxy in no-auth mode: connects without the policy (discovered proxies are used, they only get no
  credentials).

### 9. PAC fallback to WPAD

Setup: script `https://pac.corp.example/missing.pac` (404) or an unreachable host, "Automatically detect settings" on.
- Warning `proxy: PAC https://pac.corp.example/missing.pac failed (the proxy script (PAC) could not be downloaded), falling back to automatic detection (WPAD); its proxies get no credentials`.
- Then `proxy: <gateway> -> <proxy> (PAC https://pac.corp.example/missing.pac failed (...), automatic detection (WPAD))`;
  with an authenticating proxy refused as in case 8.

### 10. Bypass and NO_PROXY

10a Manual proxy with "Don't use the proxy server for" `<gateway>` (or `*.<domain>` of the gateway); direct 443
allowed: `proxy: <gateway> -> DIRECT (manual proxy settings: bypass list)`, connected directly; the proxy log shows nothing.

10b Manual proxy without exception, `$env:NO_PROXY = "<gateway>"` (also try `.<parent domain>`): no `proxy:` line for the
gateway, no `connecting through proxy`, connected directly. `Remove-Item Env:NO_PROXY`.

10c (optional) PAC returning `PROXY <dead>:3128; PROXY <proxy>; DIRECT`: the dead one is skipped
(`proxy: cannot reach proxy <dead>:3128 (...)` warning), the second used.

### 11. Proxy down and back

Setup: case 3a (connected through the Kerberos proxy). Stop the proxy service.
- Console: `connection to <gateway> lost (...) – reconnecting in ...`; log `link down (cannot reach proxy <proxy> (...)), reconnecting in <n> ms`
  with growing waits (up to 60 s). The web app shows the desktop offline; a tool call gets `DEVICE_OFFLINE`.
- Tray **(tray)**: record what the icon and the status line show while the proxy is down (the status file says
  `connected` while the link is reconnecting; the notice and `link.log` are authoritative).
- Start the proxy again: within the next backoff (≤ 60 s) `reconnected to <gateway>`, `... authenticated with Negotiate`;
  a tool call works.

### 12. Pairing and update check through the proxy

Setup: case 3a (Kerberos, manual proxy), direct 443 blocked.
- `fairyfly link unpair`, then `fairyfly link pair --gateway https://<gateway> --log-level debug`:
  `Contacting https://<gateway>…` (the full URL),
  the QR code and number appear; finish on the phone; `fairyfly link run` then connects. Lines
  `proxy: <gateway> -> <proxy> (manual proxy settings)` and `proxy: tunnel to <gateway>:443 through <proxy> (..., authenticated with Negotiate)`.
- `fairyfly update check --log-level debug`: reports the latest release (no error); lines
  `proxy: api.github.com -> <proxy> (manual proxy settings)` and the tunnel to `api.github.com:443`. The proxy must
  allow `api.github.com`, `github.com` and `*.githubusercontent.com`. Tray **Check for updates** shows the same result.

### 13. Basic from the Credential Manager

Setup: proxy in Basic-only mode, set manually.
- 13a No credential stored: `proxy requires authentication (offers Basic), and no credentials are configured for it (<proxy> -> <gateway>:443, proxy from Windows proxy settings)`.
- 13b `cmdkey /generic:fairyfly-proxy:<proxy host> /user:<basic user> /pass` (password prompted; host exactly as in the
  proxy setting, without port). Restart the link: `... authenticated with Basic`, connected; the proxy log shows the
  Basic user.
- 13c Wrong password stored: `proxy requires authentication: it refused the Basic credentials of this user (offers Basic) (...)`.
- 13d The same Basic-only proxy delivered by the https PAC script (case 6): not sent,
  `proxy requires authentication (offers Basic), and Basic is sent only to a proxy set explicitly, not to one from a PAC script or automatic detection (...)`.
- `cmdkey /delete:fairyfly-proxy:<proxy host>` afterwards.

### 14. Unsupported proxy entries

- Manual proxy address `https://<proxy>` (scheme typed into the address field), or per-protocol "Secure:
  `https://<proxy>`": warning once `proxy: the proxy entry https://<proxy> is skipped: only http:// proxies are supported (no TLS to the proxy, no SOCKS)`,
  then `proxy: <gateway> -> DIRECT (manual proxy settings: none for https)`.
- PAC returning `SOCKS <host>:1080`: no SOCKS connection is attempted (the entry is ignored, or skipped with the
  warning above, depending on how WinHTTP reports it; record the `proxy: <gateway> -> ...` line).

### 15. No credentials in logs

After all cases, on the PC:

    $files = Get-ChildItem $env:USERPROFILE\.fairyfly\logs\*.log*, $env:USERPROFILE\proxy-test\*.txt
    $files | Select-String -CaseSensitive -Pattern 'Proxy-Authorization', 'TlRMTVNT', 'YII[A-Za-z0-9+/]{16}',
      '(Negotiate|NTLM) [A-Za-z0-9+/]{16}', 'Basic (?!credentials)[A-Za-z0-9+/]{8,}', 'S3cret-Proxy-Test',
      '<basic test password>', '<base64 of user:password>'

Expected: no match. (`-CaseSensitive` and the `(?!credentials)` keep the code's own lines out, such as `it refused the
Basic credentials of this user`; checked against every message of `system_proxy*.cpp` and libffl's `proxy.cpp`.) (`TlRMTVNT` is base64 of an NTLM message, `YII` the start of a base64 SPNEGO/Kerberos token.) Also
check the tray's notices and error message boxes seen during the cases: none showed a password or a token.

## Results

| # | Case | Result (pass/fail) | Lines seen / notes | Tester, date |
|---|---|---|---|---|
| | build: `fairyfly --version`, commit | | | |
| | proxy product and version, domain functional level | | | |
| 1 | direct | | | |
| 2 | manual, no auth (2b 403) | | | |
| 3a | manual, Kerberos | | | |
| 3b | manual, NTLM | | | |
| 3c | netsh winhttp | | | |
| 4 | auth refused (4a, 4b) | | | |
| 5 | HTTPS_PROXY (5b, 5c) | | | |
| 6 | PAC https (6b GPO http FQDN) | | | |
| 7 | PAC http untrusted (7b, 7c) | | | |
| 8 | WPAD refused, policy allows (8b–8d) | | | |
| 9 | PAC → WPAD fallback warning | | | |
| 10 | bypass, NO_PROXY (10c failover) | | | |
| 11 | proxy down / back | | | |
| 12 | pairing, update check | | | |
| 13 | Basic via Credential Manager (13a–13d) | | | |
| 14 | https/SOCKS entries | | | |
| 15 | no credentials in logs | | | |

Attach the filled-in table (and the `proxy-test` folder, after case 15) to the release PR. A failing case blocks the
release unless the owner decides otherwise in that PR.

## Appendix: a test proxy

There is no current Windows-native forward proxy with Kerberos and NTLM (Forefront TMG is out of support), so the
usual test setup is **squid on a Linux VM in the domain**. A customer's own proxy (Bluecoat/Symantec, Zscaler,
Skyhigh, …) is the better test where available; the cases are the same.

Domain side (on a DC, elevated):

    # service account for the proxy's Kerberos key, SPN = the proxy's FQDN
    New-ADUser svc-squid -AccountPassword (Read-Host -AsSecureString) -Enabled $true -PasswordNeverExpires $true
    setspn -S HTTP/proxy.corp.example CORP\svc-squid
    ktpass -princ HTTP/proxy.corp.example@CORP.EXAMPLE -mapuser CORP\svc-squid -crypto AES256-SHA1 `
      -ptype KRB5_NT_PRINCIPAL -pass * -out HTTP.keytab
    # DNS: A record proxy.corp.example; A record wpad -> the PAC web server. Windows DNS blocks the name "wpad" by
    # default (global query block list): allow it for the test only
    dnscmd /config /globalqueryblocklist isatap

Proxy VM (Debian/Ubuntu): `apt install squid samba winbind krb5-user`; copy `HTTP.keytab` to `/etc/squid/` (owner
`proxy`, mode 600); join for NTLM: `net ads join -U Administrator`, start `winbind`, add `proxy` to the group
`winbindd_priv`. `/etc/squid/squid.conf` (switch modes by commenting the `auth_param` blocks and `squid -k reconfigure`):

    http_port 3128
    # Negotiate (Kerberos)
    auth_param negotiate program /usr/lib/squid/negotiate_kerberos_auth -k /etc/squid/HTTP.keytab -s HTTP/proxy.corp.example@CORP.EXAMPLE
    auth_param negotiate children 10
    auth_param negotiate keep_alive on
    # NTLM (Samba winbind)
    auth_param ntlm program /usr/bin/ntlm_auth --helper-protocol=squid-2.5-ntlmssp
    auth_param ntlm children 10
    auth_param ntlm keep_alive on
    # Basic only (case 13): htpasswd -c /etc/squid/passwd basicuser
    #auth_param basic program /usr/lib/squid/basic_ncsa_auth /etc/squid/passwd
    acl SSL_ports port 443
    acl authed proxy_auth REQUIRED
    http_access deny CONNECT !SSL_ports
    http_access allow authed
    http_access deny all
    logformat ffy %ts.%03tu %>a %Ss/%03>Hs %rm %ru %un "%{User-Agent}>h"
    access_log daemon:/var/log/squid/access.log ffy

For the no-auth mode replace `http_access allow authed` with `http_access allow all` (test network only). The access
log shows the user (`%un`) and fairyfly's `ffy-link/0.2` user agent per CONNECT.

PAC file (served by nginx on the same or another VM, over `https://pac.corp.example/proxy.pac` with a certificate from
the domain CA, and over `http://` as `/proxy.pac` and `/wpad.dat`, MIME type `application/x-ns-proxy-autoconfig`):

    function FindProxyForURL(url, host) {
      if (isPlainHostName(host) || dnsDomainIs(host, ".corp.example")) return "DIRECT";
      return "PROXY proxy.corp.example:3128";
    }

WPAD by DHCP instead of DNS: option 252 (string) `http://wpad.corp.example/wpad.dat` on the test scope.
