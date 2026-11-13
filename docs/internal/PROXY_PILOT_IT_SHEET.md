# fairyfly proxy test: information for your IT / Informationen für Ihre IT

## Deutsch

**Was ist fairyfly?** Ein Desktop-Programm von DataZoo. Es verbindet das SAP GUI des angemeldeten Benutzers über eine
verschlüsselte Verbindung (`wss://`/`https://`, Port 443) mit dem Dienst `fairyfly.io`. Neu ist die Unterstützung von
Firmen-Proxys; die wollen wir in Ihrem Netz prüfen.

**Was wir testen.** Ca. 60 Minuten, **ein** Windows-10/11-PC eines normalen Domänenbenutzers. Wir installieren eine
Testversion, verbinden sie über Ihren Proxy mit `fairyfly.io` und lesen höchstens eine SAP-Maske (nur lesend). Der
Inhalt dieser Maske geht über `fairyfly.io` an den dabei verwendeten KI-Assistenten. **Keine Änderung an SAP**, keine
Änderung an Ihrem Proxy nötig.

**Was wir von Ihnen brauchen.**
- Wie der PC den Proxy findet: **manuell** (Adresse:Port), **PAC-Skript** (URL; per Gruppenrichtlinie oder vom Benutzer
  gesetzt?) oder **automatische Erkennung (WPAD)**; und ob Edge, Chrome oder Firefox den Proxy **per Richtlinie**
  bekommen (Edge/Chrome `ProxyMode`/`ProxyServer`/`ProxyPacUrl`/`ProxySettings`, Firefox `Proxy` per GPO oder
  `policies.json`). fairyfly liest diese Richtlinien mit.
- Optional: Sie können den Proxy für fairyfly auch selbst vorgeben und sperren:
  `HKLM\SOFTWARE\Policies\DataZoo\fairyfly`, REG_SZ `ProxyMode` = `manual` | `pac` | `direct` | `auto`, dazu
  `ProxyServer` (`host:port`), `ProxyBypass` (`<local>;*.firma.de`) bzw. `ProxyPacUrl`. Dann kann der Benutzer die
  Einstellung nicht ändern (Fenster „Network / proxy…“ zeigt „Set by your administrator“).
- Wie sich Benutzer am Proxy anmelden: gar nicht, **Kerberos/NTLM** (Windows-Anmeldung) oder **Basic**
  (Benutzer/Passwort).
- Freigabe auf Proxy und Firewall: `CONNECT fairyfly.io:443`, möglichst **ohne TLS-Inspection**, Verbindungen dürfen
  mind. 60 s ruhen. Optional `api.github.com`, `github.com`, `*.githubusercontent.com` (Update-Prüfung).
- Optional, nur wenn Sie WPAD mit Anmeldung nutzen: jemand mit Adminrechten, der für den Test den Wert
  `HKLM\SOFTWARE\Policies\DataZoo\fairyfly`, DWORD `AllowLoginToDiscoveredProxies` = `1` setzt (danach löschen).
  Nicht dauerhaft setzen, wo WPAD gefälscht werden kann: fällt das PAC-Skript aus (z. B. weil jemand seinen Server
  blockiert), nimmt fairyfly die nächste Quelle, ggf. die automatische Erkennung, und mit diesem Wert bekäme ein
  untergeschobener WPAD-Proxy die Windows-Anmeldung.

**Was wir nie tun.** Wir fragen **keine Passwörter** ab und speichern keine. Die Windows-Anmeldung (Kerberos/NTLM) geht
nur an Proxys, die ausdrücklich eingetragen sind: manuell (Windows oder fairyfly), `netsh winhttp`, `HTTPS_PROXY`, per
Browser- oder fairyfly-Richtlinie, oder ein PAC-Skript über `https://` bzw. per Richtlinie (Windows, Edge/Chrome,
fairyfly, eine nur von Administratoren änderbare Firefox-`policies.json`) mit vollständigem Hostnamen (z. B. `http://pac.firma.de/proxy.pac`, nicht
`http://wpad/…` und keine IP-Adresse). Nie an einen automatisch gefundenen Proxy (WPAD), außer Sie setzen den Wert oben.
Eine `policies.json`, die auch Nicht-Administratoren ändern können (z. B. `C:\Firefox` auf Terminalservern), bekommt
nie Anmeldedaten.
Basic nur an einen ausdrücklich eingetragenen Proxy und nur, wenn der Benutzer selbst das Passwort in der
Windows-Anmeldeinformationsverwaltung als `fairyfly-proxy:<proxy-host>` hinterlegt. Logs bleiben auf dem PC. Nur wenn
**Sie** sie uns schicken wollen, prüfen Sie sie vorher (Befehl unten); sie enthalten keine Passwörter oder Tokens.

**Installation der Testversion.** `fairyfly.msi` (nicht signierter CI-Build, Prüfsummen in `SHA256SUMS`) installiert
**pro Benutzer ohne Adminrechte** nach `%LOCALAPPDATA%\Programs\fairyfly`. Rechtsklick › Eigenschaften › „Zulassen“
(oder `Unblock-File .\fairyfly.msi`), dann Doppelklick; zeigt SmartScreen eine Warnung: „Weitere Informationen ›
Trotzdem ausführen“. Bitte vorab prüfen und ggf. für den Testtag erlauben: die Richtlinien „Benutzerinstallationen
verbieten“ (`DisableUserInstalls`) und „Windows Installer deaktivieren“ (`DisableMSI`), AppLocker/WDAC für
`%LOCALAPPDATA%\Programs\fairyfly\*.exe`, SmartScreen. Ist das nicht möglich: Sie verteilen die MSI im Benutzerkontext
(z. B. Intune „Benutzer“). Das MSIX-Paket empfehlen wir nicht (Adminrechte, Testzertifikat nötig).

**Entfernen.** Tray › Beenden; `fairyfly link unpair`; Einstellungen › Apps › fairyfly › Deinstallieren; Ordner
`%USERPROFILE%\.fairyfly` und `%USERPROFILE%\proxy-test` löschen; ggf. `cmdkey /delete:fairyfly-proxy:<proxy-host>`
und die Richtlinienwerte oben löschen; `fairyfly proxy reset`, falls der Benutzer einen Proxy gesetzt hat. Wir entfernen das Gerät in fairyfly.io.

## English

**What is fairyfly?** A desktop app by DataZoo. It links the signed-in user's SAP GUI to the service `fairyfly.io`
over an encrypted connection (`wss://`/`https://`, port 443). Corporate proxy support is new; we want to verify it in
your network.

**What we test.** About 60 minutes, **one** Windows 10/11 PC of a normal domain user. We install a test build,
connect it through your proxy to `fairyfly.io` and read at most one SAP screen (read only). The content of that screen
travels through `fairyfly.io` to the AI assistant used for the test. **No change to SAP**, no change to your proxy
needed.

**What we need from you.**
- How the PC finds the proxy: **manual** (address:port), **PAC script** (URL; set by group policy or by the user?) or
  **automatic detection (WPAD)**; and whether Edge, Chrome or Firefox get the proxy **by policy** (Edge/Chrome
  `ProxyMode`/`ProxyServer`/`ProxyPacUrl`/`ProxySettings`, Firefox `Proxy` by GPO or `policies.json`). fairyfly reads
  these policies too.
- Optional: you can also set and lock the proxy for fairyfly yourself: `HKLM\SOFTWARE\Policies\DataZoo\fairyfly`,
  REG_SZ `ProxyMode` = `manual` | `pac` | `direct` | `auto`, with `ProxyServer` (`host:port`), `ProxyBypass`
  (`<local>;*.corp.example`) or `ProxyPacUrl`. The user then cannot change the setting (the "Network / proxy..." window
  shows "Set by your administrator").
- How users authenticate to the proxy: not at all, **Kerberos/NTLM** (Windows login), or **Basic** (user/password).
- Allowed on proxy and firewall: `CONNECT fairyfly.io:443`, preferably **without TLS inspection**, connections may be
  idle for at least 60 s. Optional `api.github.com`, `github.com`, `*.githubusercontent.com` (update check).
- Optional, only if you use WPAD with authentication: someone with admin rights who sets, for the test,
  `HKLM\SOFTWARE\Policies\DataZoo\fairyfly`, DWORD `AllowLoginToDiscoveredProxies` = `1` (delete it afterwards).
  Do not keep it where WPAD can be spoofed: if the PAC script fails (e.g. someone blocks its server), fairyfly takes the
  next source, possibly automatic detection, and with this value a planted WPAD proxy would get the Windows login.

**What we never do.** We **never ask for or store passwords**. The Windows login (Kerberos/NTLM) is sent only to
proxies configured explicitly: manual (Windows or fairyfly), `netsh winhttp`, `HTTPS_PROXY`, by browser or fairyfly
policy, or a PAC script over `https://` or set by policy (Windows, Edge/Chrome, fairyfly, a Firefox `policies.json`
only administrators can change) with a full host name (e.g. `http://pac.corp.example/proxy.pac`, not `http://wpad/…` and not an IP
address). Never to an automatically discovered proxy (WPAD) unless you set the value above. A `policies.json` that
non-administrators can change (e.g. `C:\Firefox` on terminal servers) never gets credentials. Basic goes only to an
explicitly configured proxy, and only if the user stores the password in the Windows Credential Manager as
`fairyfly-proxy:<proxy host>` themselves. Logs stay on the PC. Only if **you** choose to send them, check them first
(command below); they contain no passwords or tokens.

**Installing the test build.** `fairyfly.msi` (unsigned CI build, checksums in `SHA256SUMS`) installs **per user
without admin rights** to `%LOCALAPPDATA%\Programs\fairyfly`. Right-click › Properties › "Unblock" (or
`Unblock-File .\fairyfly.msi`), then double-click; if SmartScreen warns: "More info › Run anyway". Please check
beforehand and allow for the test day if needed: the policies "Prohibit User Installs" (`DisableUserInstalls`) and
"Turn off Windows Installer" (`DisableMSI`), AppLocker/WDAC for `%LOCALAPPDATA%\Programs\fairyfly\*.exe`, SmartScreen.
If that is not possible, deploy the MSI in the user context (e.g. Intune "User"). We do not recommend the MSIX package
(needs admin rights and a test certificate).

**Removing it.** Tray › Quit; `fairyfly link unpair`; Settings › Apps › fairyfly › Uninstall; delete
`%USERPROFILE%\.fairyfly` and `%USERPROFILE%\proxy-test`; if created, `cmdkey /delete:fairyfly-proxy:<proxy host>` and
the policy values above; `fairyfly proxy reset` if the user set a proxy. We remove the device in fairyfly.io.

## Logs / Log-Dateien

`%USERPROFILE%\.fairyfly\logs\link.log` (tray) and `%USERPROFILE%\proxy-test\case-*.txt` (console runs). Collect and
check in PowerShell / Einsammeln und prüfen in PowerShell:

    $out = "$env:USERPROFILE\Desktop\fairyfly-proxy-logs"; New-Item -ItemType Directory $out -Force | Out-Null
    Copy-Item "$env:USERPROFILE\.fairyfly\logs\link.log*", "$env:USERPROFILE\proxy-test\*.txt" $out -ErrorAction SilentlyContinue
    Get-ChildItem $out -File | Select-String -CaseSensitive -Pattern 'Proxy-Authorization','TlRMTVNT',
      'YII[A-Za-z0-9+/]{16}','(Negotiate|NTLM) [A-Za-z0-9+/]{16}','Basic (?!credentials)[A-Za-z0-9+/]{8,}'
    # no output = no credentials / keine Ausgabe = keine Anmeldedaten
    # optional: replace DNS domain and user name / optional: DNS-Domäne und Benutzernamen ersetzen
    $names = @{ '<domain>' = $env:USERDNSDOMAIN; '<user>' = $env:USERNAME }
    Get-ChildItem $out -File | ForEach-Object { $t = Get-Content $_.FullName -Raw
      foreach ($k in $names.Keys) { if ($names[$k]) { $t = $t -replace [regex]::Escape($names[$k]), $k } }
      Set-Content $_.FullName $t -Encoding UTF8 }
    Compress-Archive "$out\*" "$out.zip" -Force

The replacement does **not** touch the NetBIOS domain (`CORP\`), the computer name, the proxy host or the fairyfly
account's e-mail address; edit those by hand if needed. / Die Ersetzung ändert **nicht** die NetBIOS-Domäne (`CORP\`),
den Computernamen, den Proxy-Host und die E-Mail-Adresse des fairyfly-Kontos; diese bei Bedarf von Hand ändern.
