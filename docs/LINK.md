# Cloud link (`fairyfly link`)

`fairyfly link` connects this desktop's fairyfly to the fairyfly cloud gateway, so that AI assistants your
organisation has connected there can use your SAP GUI:
- Supported assistants: Gemini Enterprise, Claude, ChatGPT and Copilot Studio.
- The connection is outbound only: no port is opened on the PC and no VPN is needed.
- The cloud side is described in the gateway's documentation (research repository,
  `2026-10-14-fairyfly-cloud-agent-access/gateway`).

| Command | What it does |
|---|---|
| `fairyfly link pair --gateway https://app.example` | Shows a QR code and a number. Scan the code with your phone, sign in, type the number. The desktop then shows **which account and organisation** it would act for (and whether the identity provider verified the e-mail address); type `yes` only if that is you |
| `fairyfly link status` | Paired or not, with device, account, organisation and gateway |
| `fairyfly link run [--allow-write] [--tools session,screen] [--approve desktop\|no]` | Connects and serves tool calls until Ctrl+C |
| `fairyfly link unpair` | Forgets the pairing here (remove the desktop in the web app too) |

The state file is `%LOCALAPPDATA%\fairyfly\link\state.json`. The device key inside it is protected with DPAPI.

## What runs where

- `link run` serves calls through **fairyfly's own MCP dispatcher, in-process**. No local HTTP endpoint and no local
  bearer token are involved.
- The same guards apply as for `fairyfly mcp`, in the same order:
  1. `--read-only` / `FAIRYFLY_READ_ONLY=1` (hard cap);
  2. read-only unless `--allow-write`;
  3. `--tools` families;
  4. the policy limits.
- Each call additionally carries the gateway's grant. A call the gateway granted read-only runs with fairyfly's
  read-only guard switched on, even when this desktop allows writes.
- A change needs the person's approval:
  - on this desktop (a dialog, `--approve desktop`, the default), or
  - with a passkey on the phone (the gateway puts the approval into the signed call).
- Every call is written to fairyfly's audit trail with `audit_source: "link"`, the principal `link:<organisation
  id>/<user>`, the AI client and the gateway's request id, so it can be matched with the cloud audit.
- Calls arrive on the link's network threads and are handed to the main (COM) thread one at a time
  (`src/link/main_thread_tools.*`).

## The shared protocol library

`third_party/libffl` holds the `ffl/1` frames, call-assertion verification (per-tenant gateway keys pinned at
pairing), pairing, the DPAPI state and the link agent:
- It is vendored from the gateway with `scripts/sync_libffl.py` (see `third_party/libffl/VERSION`), so protocol and
  verification cannot drift between the gateway and fairyfly.
- Never edit the copy here; change the gateway and sync.

## Not yet

- A tray menu item "Connect to cloud" (QR window, status, pause).
- Installer and auto-update integration.

Until then, `link run` runs in a console like `mcp`.
