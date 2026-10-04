# SAP authentication for tray MCP sessions

Fairyfly can run one interactive Windows tray process and route MCP calls to several SAP GUI windows. Those windows may be logged in as different SAP users. The Windows account owns the desktop and its Credential Manager; the SAP system authenticates each SAP GUI logon separately.

## Three separate identities

| Layer | What authenticates | What it controls |
|---|---|---|
| MCP | A Fairyfly bearer token in the HTTP `Authorization` header | Which Fairyfly tools and SAP identities the remote client may use |
| Windows desktop | The signed-in Windows user running the tray and SAP GUI | Access to SAP GUI windows, local credential storage, and the SNC provider |
| SAP | Username/password or the enterprise SSO/SNC credential used by SAP GUI | The SAP user and authorizations of each SAP session |

An MCP token is not a SAP password, an SNC ticket, or a SAP login token. A client that knows a saved connection ID does not thereby own it. Fairyfly must check the live `SID/CLIENT/USER` of the selected window against both the endpoint and token grants. Clients sharing one MCP token also share its permissions and sticky connection selection; issue a distinct token for each client. This is MCP authorization inside one Windows desktop, not separate Windows credential custody.

## Endpoint and token grants

Configure every SAP identity that this tray may expose in `owner.sap_identities`, using exact uppercase `SID/CLIENT/USER` values. For an endpoint with more than one allowed SAP identity, each MCP token needs at least one exact `--sap-identity` grant. A token with no such grant is refused for owner-mode session calls. One token may explicitly grant more than one SAP identity to an operator that should use both. `--system` and `--connections` are additional restrictions; a SAP Logon entry name can be shared by multiple SAP users and is not a substitute for a SAP identity grant.

Example `mcp.yaml` fragment:

~~~yaml
owner:
  sap_identities: [A4H/001/ALICE, A4H/001/BOB]
~~~

Create separate client credentials on the Windows host:

~~~powershell
fairyfly mcp token create alice-client --scope session,connection,screen,element --sap-identity A4H/001/ALICE --connections Bigfox --expires 30d
fairyfly mcp token create bob-client --scope session,connection,screen,element --sap-identity A4H/001/BOB --connections Bigfox --expires 30d
~~~

The token secret is shown once. Keep it in the MCP client's secret store. `--sap-identity` grants access to a **logged-in SAP identity**; it never claims that presenting the token logs the caller in to SAP. Revoking a token removes its MCP access; it does not log the SAP user out of an already open SAP window.

## Username and password logon

The current remote MCP tools use a **named credential already stored in this Windows user's Credential Manager**. Set it locally, once per SAP user and client, without putting a password on the command line:

~~~powershell
fairyfly credentials set Bigfox-Alice --user ALICE --client 001
fairyfly credentials set Bigfox-Bob --user BOB --client 001
~~~

The command prompts for the password; `--password-stdin` is available for a managed secret import. Then launch the approved SAP Logon entry and select the credential name:

~~~text
gui_session_launch {"name":"Bigfox","login":true,"credential":"Bigfox-Alice"}
gui_session_launch {"name":"Bigfox","login":true,"credential":"Bigfox-Bob"}
~~~

You can also use `gui_session_launch` with `login:false`, followed by `gui_session_login` on its returned saved `connection` ID and a credential name. On a **multi-identity endpoint**, Fairyfly binds an unfinished logon window's exact saved generation to the launching token for 15 minutes. Only that token can complete `gui_session_login`; a different token's guessed ID is refused before a credential is typed. A prelogin window opened outside Fairyfly has no such token binding and cannot be logged in through a multi-identity MCP endpoint. `gui_session_launch` with `login:true` remains the simpler password workflow. If a launch reports `LOGIN_NOT_COMPLETED` with a verified saved ID, the same token may retry login while its binding is valid. The MCP login tools do not accept a raw password in their JSON arguments. Fairyfly verifies the credential's anticipated identity and the authenticated SAP identity before returning a usable owner-mode session. Whether password logon is allowed on an SNC-enabled system is controlled by SAP and enterprise policy; ask the SAP administrator rather than trying to bypass that policy. [SAP GUI describes both password and SSO logon](https://help.sap.com/docs/sap_gui_for_windows/63bd20104af84112973ad59590645513/0f509cead9484fd49d33aca7e7693fc0.html).

On a multi-identity endpoint, the unfinished-window binding is held in tray memory. It expires after 15 minutes and is lost on tray restart or token revocation. The tray retains at most eight unexpired bindings per token and 64 across the endpoint. At either limit, it refuses another launch before opening SAP, including `login:true`. These limits do not count unfinished windows whose bindings have expired or been lost. While the binding is valid, the launching token can call `gui_session_disconnect` with the exact returned `connection` and `close_session:true` to close the unfinished window; this requires server write mode. Other tokens are denied, and Fairyfly checks the live saved generation and SAP window again immediately before closing. For a logon window without a server session key, Fairyfly also binds the window handle; if that handle cannot be verified, cleanup fails closed. If the binding is lost, an operator with desktop access must close the unfinished SAP window and its saved connection, then the MCP client can launch a fresh window. A remote MCP client cannot reclaim an expired or restarted binding.

## SSO and SNC logon

SNC protects the SAP GUI connection and uses a configured external security provider for authentication; the provider and SAP user mapping determine the identity that logs in. [SAP SNC overview](https://help.sap.com/docs/SAP_NETWEAVER_AS_ABAP_752/621bb4e3951b4a8ca633ca7ed1c0aba2/aa38ff4fa187622fe10000000a44176d.html). SAP recommends opening an SSO-enabled SAP Logon connection for scripted logon instead of embedding a password in a script. [SAP scripting guidance](https://help.sap.com/docs/sap_gui_for_windows/1f190e2f59db43e192cba638ea29870b/7e77efef27e2439a894ebeb03150ade9.html).

For each SAP Logon entry used by Fairyfly:

1. Have the SAP and identity administrators supply the application server's SNC name, the approved security provider, its client library path, and the intended SAP user mapping. Do not invent an SNC name from the SAP user name.
2. Configure the provider for the interactive Windows account that runs the tray. Where the provider requires it, set `SNC_LIB` to the installed client library path in that account's environment, then restart SAP Logon and the tray so they inherit it. [SAP's SAP Logon SNC setup](https://help.sap.com/docs/ABAP_PLATFORM_NEW/e73bba71770e4c0ca5fb2a3c17e8e229/dd2e029250f64ed682e1b2f3eda66fca.html).
3. Edit the SAP Logon connection's **Network** properties, enable **Secure Network Communication**, and enter the supplied server SNC name and protection settings. If the organization permits password authentication over SNC, its SAP Logon configuration may use **SNC logon with user/password (no Single Sign-On)**; otherwise leave SSO to the configured provider. [SAP GUI connection properties](https://help.sap.com/docs/ABAP_PLATFORM_NEW/e73bba71770e4c0ca5fb2a3c17e8e229/76ea37bd31794c1da6f1ae9ba5a9df5d.html).
4. Prove the connection on the desktop first. Record the actual SAP system, client, and user shown after logon, and grant only that `SID/CLIENT/USER` on the MCP endpoint and token. Repeat for each identity and SAP Logon entry.

For the current Fairyfly build, configure and test the SSO/SNC connection in SAP Logon for the interactive Windows user first. Open the authenticated window on the desktop and use `gui_session_attach` with its exact SAP GUI session ID, or try `gui_session_launch` with `login:false` if that SAP Logon entry signs in automatically. On a multi-identity endpoint, an unfinished SSO logon screen launched through Fairyfly is bound to its launching token, but Fairyfly cannot complete an interactive SNC challenge or choose an SNC profile through `gui_session_login`. Once SAP has authenticated the window, Fairyfly checks its live SAP user against the token grant before MCP actions. The `gui_session_login` tool currently implements stored username/password credentials; it does not inject an SNC ticket.

One Windows sign-in does not automatically yield several independent SAP SSO identities. The available identity often follows that user's Windows or Secure Login Client context. SAP documents configurations with multiple Secure Login Client profiles, SNC names, and certificate mappings for multiple SAP GUI logins; these require enterprise SSO setup and should be verified with the local provider and target SAP systems. [SAP multiple-profile guidance](https://help.sap.com/docs/SAP_SINGLE_SIGN-ON/df185fd53bb645b1bd99284ee4e4a750/817e679a3eba4a6394f388c812aef98b.html). If the provider cannot select the intended identity for each connection, use a permitted password logon or a separate interactive Windows account for that identity.

## A SAP token supplied through MCP

There is no generic SAP GUI scripting field that accepts an arbitrary bearer or OAuth token as an SNC credential. A future MCP option for a caller-supplied SAP SSO credential needs a **specific supported provider and token format**, a way to delegate that credential to the provider, and an end-to-end test that proves SAP logged in as the intended `SID/CLIENT/USER`. It must not reinterpret the Fairyfly MCP bearer token as that credential. The exact integration depends on the organization's SNC product, so this capability is not part of the current `gui_session_login` contract.

Do not send SAP passwords, Kerberos tickets, private keys, or SSO tokens in ordinary MCP tool arguments or audit fields. If a future provider integration needs a short-lived delegated token, it needs a separate secret-handling protocol and provider-specific security review.

## Operating checks

1. Confirm SAP GUI scripting and the intended password or SSO/SNC login work on the interactive desktop.
2. Configure the endpoint's exact SAP identity allowlist and issue separate token grants for the intended clients.
3. Open or log in to each SAP window; verify the live SAP system, client, and user before exposing its saved connection ID.
4. Test discovery and calls with both client tokens: each must see only its granted SAP user, and a guessed ID for the other user must fail without revealing that window.

The independent two-Windows-account isolation probe is an optional test of stronger desktop separation. The primary multi-user test uses two **SAP users under the same Windows account and tray**.
