# fairyfly proxy test: information for your IT / Informationen für Ihre IT

Hand this page to the customer's IT before the session. Our run sheet is [PROXY_PILOT_RUN_SHEET.md](PROXY_PILOT_RUN_SHEET.md),
the full checklist [PROXY_MANUAL_TEST.md](PROXY_MANUAL_TEST.md).

## Deutsch

**Was ist fairyfly?** Ein Desktop-Programm von DataZoo. Es verbindet das SAP GUI des angemeldeten Benutzers über eine
verschlüsselte Verbindung (`wss://`/`https://`, Port 443) mit dem Dienst `fairyfly.io`. Neu ist die Unterstützung von
Firmen-Proxys; die wollen wir in Ihrem Netz prüfen.

**Was wir testen.** ca. 60 Minuten, **ein** Windows-10/11-PC eines normalen Domänenbenutzers. Wir installieren eine
Testversion, verbinden sie über Ihren Proxy mit `fairyfly.io` und lesen höchstens eine SAP-Maske (nur lesend). **Keine
Änderung an SAP**, keine Änderung an Ihrem Proxy nötig.

**Was wir von Ihnen brauchen.**
- Wie der PC den Proxy findet: **manuell** (Adresse:Port), **PAC-Skript** (URL; per Gruppenrichtlinie oder vom Benutzer
  gesetzt?) oder **automatische Erkennung (WPAD)**.
- Wie der Proxy anmeldet: keine Anmeldung, **Kerberos/NTLM** (Windows-Anmeldung), oder **Basic** (Benutzer/Passwort).
- Freigabe auf Proxy und Firewall: `CONNECT fairyfly.io:443`, möglichst **ohne TLS-Inspection**, Verbindungen dürfen
  mind. 60 s ruhen. Optional `api.github.com`, `github.com`, `*.githubusercontent.com` (Update-Prüfung).
- Optional, nur wenn Sie WPAD mit Anmeldung nutzen: jemand mit Adminrechten, der für den Test den Wert
  `HKLM\SOFTWARE\Policies\DataZoo\fairyfly`, DWORD `AllowLoginToDiscoveredProxies` = `1` setzt (danach löschen).

**Was wir nie tun.** Wir fragen **keine Passwörter** ab und speichern keine. Die Windows-Anmeldung (Kerberos/NTLM) geht
nur an Proxys, die Ihre IT ausdrücklich eingetragen hat (manuell, `netsh winhttp`, `HTTPS_PROXY`, PAC per `https://`
oder per Gruppenrichtlinie) – nie an einen automatisch gefundenen (WPAD), außer Sie setzen den Wert oben. Logs bleiben
auf dem PC. Nur wenn **Sie** sie uns schicken wollen, prüfen Sie sie vorher (Befehl unten); sie enthalten keine
Passwörter oder Tokens, Namen können Sie ersetzen.

**Installation der Testversion.** `fairyfly.msi` (nicht signierter CI-Build, Prüfsummen in `SHA256SUMS`) installiert
**pro Benutzer ohne Adminrechte** nach `%LOCALAPPDATA%\Programs\fairyfly`. Rechtsklick › Eigenschaften › „Zulassen“
(oder `Unblock-File .\fairyfly.msi`), dann Doppelklick. Bitte vorab prüfen und ggf. für den Testtag erlauben: MSI-
Installation durch Benutzer (Richtlinie „Benutzerinstallationen verbieten“/`DisableMSI`), AppLocker/WDAC für
`%LOCALAPPDATA%\Programs\fairyfly\*.exe`, SmartScreen. Ist das nicht möglich: Sie verteilen die MSI im Benutzerkontext
(z. B. Intune „Benutzer“). Das MSIX-Paket empfehlen wir nicht (Adminrechte, Testzertifikat nötig).

**Entfernen.** Tray › Beenden; `fairyfly link unpair`; Einstellungen › Apps › fairyfly › Deinstallieren; Ordner
`%USERPROFILE%\.fairyfly` und `%USERPROFILE%\proxy-test` löschen; ggf. `cmdkey /delete:fairyfly-proxy:<proxy-host>`
und den Richtlinienwert oben löschen. Wir entfernen das Gerät in fairyfly.io.

## English

**What is fairyfly?** A desktop app by DataZoo. It links the signed-in user's SAP GUI to the service `fairyfly.io`
over an encrypted connection (`wss://`/`https://`, port 443). Corporate proxy support is new; we want to verify it in
your network.

**What we test.** About 60 minutes, **one** Windows 10/11 PC of a normal domain user. We install a test build,
connect it through your proxy to `fairyfly.io` and read at most one SAP screen (read only). **No change to SAP**, no
change to your proxy needed.

**What we need from you.**
- How the PC finds the proxy: **manual** (address:port), **PAC script** (URL; set by group policy or by the user?) or
  **automatic detection (WPAD)**.
- How the proxy authenticates: none, **Kerberos/NTLM** (Windows login), or **Basic** (user/password).
- Allowed on proxy and firewall: `CONNECT fairyfly.io:443`, preferably **without TLS inspection**, connections may be
  idle for at least 60 s. Optional `api.github.com`, `github.com`, `*.githubusercontent.com` (update check).
- Optional, only if you use WPAD with authentication: someone with admin rights who sets, for the test,
  `HKLM\SOFTWARE\Policies\DataZoo\fairyfly`, DWORD `AllowLoginToDiscoveredProxies` = `1` (delete it afterwards).

**What we never do.** We **never ask for or store passwords**. The Windows login (Kerberos/NTLM) is sent only to
proxies your IT configured explicitly (manual, `netsh winhttp`, `HTTPS_PROXY`, a PAC script over `https://` or by group
policy) – never to an automatically discovered one (WPAD) unless you set the value above. Logs stay on the PC. Only if
**you** choose to send them, check them first (command below); they contain no passwords or tokens, and you can replace
names.

**Installing the test build.** `fairyfly.msi` (unsigned CI build, checksums in `SHA256SUMS`) installs **per user
without admin rights** to `%LOCALAPPDATA%\Programs\fairyfly`. Right-click › Properties › "Unblock" (or
`Unblock-File .\fairyfly.msi`), then double-click. Please check beforehand and allow for the test day if needed: user
MSI installs (policy "Prohibit User Installs"/`DisableMSI`), AppLocker/WDAC for `%LOCALAPPDATA%\Programs\fairyfly\*.exe`,
SmartScreen. If that is not possible, deploy the MSI in the user context (e.g. Intune "User"). We do not recommend the
MSIX package (needs admin rights and a test certificate).

**Removing it.** Tray › Quit; `fairyfly link unpair`; Settings › Apps › fairyfly › Uninstall; delete
`%USERPROFILE%\.fairyfly` and `%USERPROFILE%\proxy-test`; if created, `cmdkey /delete:fairyfly-proxy:<proxy host>` and
the policy value above. We remove the device in fairyfly.io.

## Logs / Log-Dateien

`%USERPROFILE%\.fairyfly\logs\link.log` (tray) and `%USERPROFILE%\proxy-test\case-*.txt` (console runs). Collect and
check in PowerShell / Einsammeln und prüfen in PowerShell:

    $out = "$env:USERPROFILE\Desktop\fairyfly-proxy-logs"; New-Item -ItemType Directory $out -Force | Out-Null
    Copy-Item "$env:USERPROFILE\.fairyfly\logs\link.log*", "$env:USERPROFILE\proxy-test\*.txt" $out -ErrorAction SilentlyContinue
    Get-ChildItem $out -File | Select-String -Pattern 'Proxy-Authorization','TlRMTVNT','YII[A-Za-z0-9+/]{16}',
      '(Negotiate|NTLM) [A-Za-z0-9+/]{16}','Basic [A-Za-z0-9+/=]{8}'
    # no output = no credentials / keine Ausgabe = keine Anmeldedaten. Optional: replace names / Namen ersetzen
    Get-ChildItem $out -File | ForEach-Object { (Get-Content $_.FullName) -replace [regex]::Escape($env:USERDNSDOMAIN),'<domain>' `
      -replace [regex]::Escape($env:USERNAME),'<user>' | Set-Content $_.FullName }
    Compress-Archive "$out\*" "$out.zip" -Force
