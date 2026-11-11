# Proxy pilot: run sheet for a customer session (~60 min)

The condensed form of [PROXY_MANUAL_TEST.md](PROXY_MANUAL_TEST.md) for one session in a customer's network, on their
proxy, which we cannot reconfigure. Expected lines, setups and the results table are in the full checklist; case
numbers are the same. The customer's IT gets [PROXY_PILOT_IT_SHEET.md](PROXY_PILOT_IT_SHEET.md) beforehand (it is
written for them: send it as it is, with the kit's `fairyfly.msi` and `SHA256SUMS`).

The tray logs `info`/`warn` only (`FFLYLOG_LEVEL` has no effect there): every check of a `debug` line, such as the
tunnel's `authenticated with ...`, runs in the console.

## Before the session

- Answers from IT: proxy discovery (manual / PAC URL, set by GPO or user / WPAD), authentication (none / Negotiate /
  NTLM / Basic), proxy product, TLS inspection on `fairyfly.io` yes/no, whether direct 443 is blocked.
- Build: the MSI from the `build.yml` run on the commit under test (pilot kit: run 42796161961, commit 448b6975,
  `fairyfly --version` = `2026.11.09`, the same number as the release `v2026.11.09` without proxy support, so record the
  **commit**, not only the version). Unsigned; MSI preferred over the test-signed MSIX (admin + certificate, and its
  `update check` answers `UPDATE_MANAGED_BY_STORE`, so case 12 cannot run on it).
- Our side: a pilot organisation on `fairyfly.io` with a pairing account and a phone; the web app open on our laptop.
- Pick the row below that matches their setup; skip what their network cannot do.

## In the session

1. (5 min) Install the MSI as the user; `fairyfly --version`; quit the tray (a console link and the tray would collide).
   Run the Windows-settings check of "How to run a case" and note the effective source.
2. (10 min) **Case 12, pairing**: `mkdir $env:USERPROFILE\proxy-test -Force`,
   `fairyfly link pair --gateway https://fairyfly.io --log-level debug`, then
   `fairyfly link run --read-only --log-level debug *>&1 | Tee-Object $env:USERPROFILE\proxy-test\case-<n>.txt`.
   This is also the first pass of their main case (below). `fairyfly update check --log-level debug` only if GitHub is
   allowed.
3. (25 min) The cases for their setup:

| Their setup | Mandatory | Optional (if time / IT present) |
|---|---|---|
| Manual proxy, no auth | 2 | 10a/10b (bypass, `NO_PROXY`), 14 (enter `https://<proxy>` as the address) |
| Manual proxy, Kerberos/NTLM | 3a (by FQDN, `klist`) or 3b, whichever it offers | 5 (`HTTPS_PROXY`, 5b), 4a (only with a user the proxy refuses), 3c (`netsh winhttp`, admin) |
| PAC over `https://`, or `http://` FQDN set by GPO | 6 / 6b | 9 (user sets a missing PAC URL + auto-detect, if not locked by GPO) |
| PAC set by the user over `http://`, proxy authenticates | 7 (expect the refusal; this is the finding to report: IT must set the script by GPO or over https) | 7b |
| WPAD, proxy authenticates | 8 (tray), 8b + 8c in the console (admin sets/removes the policy) | 9 |
| WPAD, no auth | 8d | |
| Basic only | 13a, 13b (the **user or IT** types the password at the `cmdkey` prompt, never us), 13c | 13d |
| Direct 443 open as well | 1 (with the proxy off, if they let the user change it) | |
| Any | 15 | 11 substitute: Wi-Fi/LAN off for 1 min and on again (we cannot stop their proxy), record the reconnect; 2b only if IT adds a deny rule |

4. (10 min) **Case 15**: the collection command of the IT sheet; no match. If the customer sends logs, they run it,
   not us.
5. (10 min) Remove: the "Removing it" steps of the IT sheet, plus `Remove-Item Env:HTTPS_PROXY, Env:NO_PROXY` if set,
   `netsh winhttp reset proxy` only if case 3c changed it; remove the desktop in the web app.

## Afterwards

Fill the results table of [PROXY_MANUAL_TEST.md](PROXY_MANUAL_TEST.md#results) (cases not possible in their network:
`n/a (customer network)`), add the proxy product and their discovery/auth combination, and attach the logs only if the
customer sent them. Cases not covered by the pilot stay open for the domain test setup (appendix of the checklist).
