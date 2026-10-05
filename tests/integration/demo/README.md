# README demo video: three agents, three SAP windows

Scripts and runbook for the demo video in the project README: three Claude Code agents work in parallel, each in its
own SAP GUI window, through one fairyfly tray endpoint (HTTP MCP, one token per agent, read-only guard mode). Everything
runs in one Windows VM and is recorded with VirtualBox's own recording function.

```
 +--------------------+--------------------+--------------------+
 | SAP window 1       | SAP window 2       | SAP window 3       |   top row (opened by the agents)
 | SM04               | SM50               | SM59               |
 +--------------------+--------------------+--------------------+
 | agent 1            | agent 2            | agent 3            |   bottom row (Windows Terminal)
 +--------------------+--------------------+--------------------+
```

## Prerequisites

- SAP GUI with scripting enabled, SAP Logon entry `Bigfox` (A4H, client 001), stored credential
  `fairyfly credentials set Bigfox --user DEVELOPER --client 001`.
- A Release build (`build\bin\Release\fairyfly.exe`), Windows Terminal (`winget install --id Microsoft.WindowsTerminal -e`),
  Claude Code (`claude`) logged in.
- SAP Logon must be running before the tray handles the first launch (`prepare.ps1` and `launch_agents.ps1` start it
  minimized). Without it SAP GUI scripting falls back to an embedded `SapGui.ScriptingCtrl` inside the tray, whose
  windows the parallel-session workers cannot reach: the agents log on but never get control
  (`OWNER_IDENTITY_UNKNOWN`).
- The scenarios are in `demo_common.ps1` (`$DemoScenarios`, one screen column each; SM37 is a commented-out fourth).
- Transactions used (all read-only, no typing): SM04, SM50, SM59, optionally SM37 (ST22 was dropped: its dump lists are often
  empty and typing a wider selection is refused in read-only mode) (default selection for the own user only; an
  all-users SM37 search once froze SAP GUI scripting for minutes).

## Screen and look

- Guest display 3840x1351 at 100% scaling (see "Recording" below for why), so every crop in the edit is pixel-exact.
  By default each agent opens its own SAP window (`gui_session_launch` with `login: false`, then
  `gui_session_login`); `launch_agents.ps1` watches the MCP audit trail and moves each new window above its agent as
  soon as the launch returns, so the logon happens in place. SAP GUI serialises launches and logons, so the
  windows appear one after another. The scripts tile the windows over whatever the primary screen is, and
  `launch_agents.ps1` re-tiles pre-opened SAP windows
  on every run.
- Windows dark mode, plain dark wallpaper, no desktop icons, Focus assist on (no notifications).
- One SAP GUI theme for all windows (Quartz or Belize); check that a window shows the whole list without horizontal
  scrolling, otherwise lower the SAP GUI font size (Options > Visual Design > Font Settings).
- `launch_agents.ps1` installs a Windows Terminal profile fragment "fairyfly demo" (Cascadia Mono 12 pt, logo colours,
  no tab bar, fixed window titles); `-FontSize` changes the size.
- **Minimize every other window** (including the console you run the scripts from) before recording.

## Steps

```powershell
cd tests\integration\demo
.\prepare.ps1 -WhatIf          # plan
.\prepare.ps1                  # tokens, tray, agents.json (the agents open their own SAP windows)
.\prepare.ps1 -PreOpenWindows  # alternative: open and tile the SAP windows now (30-45 s each)
.\launch_agents.ps1 -NoStart   # layout and font check, no agents
```

Rehearse once with `.\launch_agents.ps1`: the first start in each agent folder may show Claude Code's one-time
"trust this folder" question; if so, answer it in every agent window. The recorded run then shows none. Close the
terminals after each run (the SAP windows stay where they are; `transaction start /n` per connection or rerunning
`prepare.ps1` after `cleanup.ps1` resets them to the start screen).

Recording runs on the VirtualBox host (`ssh jr@bigfox`; the key is in 1Password, use Windows OpenSSH
`C:\Windows\System32\OpenSSH\ssh.exe` so the 1Password agent is used). VM: `Windows 10 (Development)`, VirtualBox 7.2.

Configured once on 2026-10-05: 3840x2160, 30 fps, 30000 kbit/s, file
`~/fairyfly-demo/fairyfly-demo-screen0.webm`. VirtualBox 7.2 locks these settings while the VM runs, even with
recording disabled; changing them needs the VM saved: `controlvm savestate`, then `modifyvm --recording-video-res=WxH
--recording-video-fps=N --recording-video-rate=KBIT --recording-file=PATH`, then start it again
(`~/fairyfly-demo/restart_vm.sh` on bigfox does exactly that).

```bash
VM="Windows 10 (Development)"
VBoxManage controlvm "$VM" recording on      # enable (does not record yet)
VBoxManage controlvm "$VM" recording start   # record; do NOT add --wait, it blocks until the recording ends
#   ... in the VM: .\launch_agents.ps1, wait until all agents have answered (about 30-60 s) ...
VBoxManage controlvm "$VM" recording stop
VBoxManage controlvm "$VM" recording off
```

The output is WebM (VP8) of the guest framebuffer only, independent of the host monitors, and never scaled: a guest
smaller than the recording frame is centred with black bars, a larger one is cropped. The guest therefore runs at
3840x1351 (full width at 1:1, bars above and below; crop them in the edit). VirtualBox's GUI refuses guest modes
larger than the VM window (`GUI/MaxGuestResolution`, read only when the window starts) and auto-resize is switched off
(`GUI/AutoresizeGuest false`), so set the size with `VBoxManage controlvm "$VM" setvideomodehint 3840 1351 32`.

Afterwards:

```powershell
.\cleanup.ps1                  # stop the tray, delete the demo tokens and token files
.\cleanup.ps1 -CloseSessions   # also close the demo's SAP windows
```

## Editing and publishing

1. `ffmpeg -i fairyfly-demo.webm -c:v libx264 -crf 16 raw.mp4` (lossless enough for editing).
2. Edit in Clipchamp (built into Windows) or Shotcut, output 1920x1080:
   - title card;
   - the full 4K overview, with all eight windows moving at once;
   - 1920x1080 crops: SAP windows 1+2, agents 1+2, SAP windows 3+4, agents 3+4;
   - back to the overview while the answers arrive;
   - end card.

   Speed up waiting parts 2-4x. Captions in Segoe UI or Inter semibold, at least 44 px, white on a translucent dark
   bar, for example "3 Claude Code agents, 3 SAP GUI windows, in parallel", "MCP over HTTP, one token per agent",
   "read-only guard", "every call audited". Title and end card use `assets/logo/fairyfly_logo_dark.svg` and the
   logo colours (`#0A111A`, `#25B493`). Target 45-75 s, no audio.
3. `ffmpeg -i edit.mp4 -c:v libx264 -crf 24 -preset slow -pix_fmt yuv420p -an -movflags +faststart fairyfly-demo.mp4`.
   Keep the file at 10 MB or less, GitHub's attachment limit on free plans. Raise CRF before lowering the
   resolution: resolution is what keeps text readable.
4. Check legibility: export stills at 1920 and at 830 px width (the README column) and look at them. Anything
   unreadable at 830 px gets a zoom-in or a caption.
5. Drag the MP4 into the README in GitHub's web editor. That gives a
   `https://github.com/user-attachments/assets/...` URL that renders as an inline player. Commit a sharp poster still
   as `assets/demo/fairyfly-demo-poster.png`.

## What the scripts do

| Script | Effect |
|---|---|
| `prepare.ps1` | With `-PreOpenWindows` only: opens missing SAP windows (`session launch --login --multiple-logon keep`), saves them as connections, resets each to the start screen, tiles them into the top row. Always: creates one token per agent (`demo-agent1`, `demo-agent2`, ...) (read-only, `session.lease` scope, owner identity, 1 day) into `%LOCALAPPDATA%\fairyfly-demo\agentN\token.txt`; starts the tray (`mcp --http --tray`, owner `A4H/001/DEVELOPER`, port 8383); writes `agents.json`. |
| `launch_agents.ps1` | Writes each agent folder (`mcp.json` with the HTTP endpoint and `Bearer ${FAIRYFLY_TOKEN}`, `CLAUDE.md` with its connection and the lease rule, `start.ps1`), opens one Windows Terminal window per agent below its SAP window and starts all agents at the same moment (`claude "<prompt>" --model=sonnet --mcp-config=mcp.json --strict-mcp-config --allowedTools=mcp__fairyfly`; the prompt comes first because `--allowedTools` and `--mcp-config` take several values). |
| `cleanup.ps1` | Stops the tray, deletes the tokens and token files, optionally closes the SAP windows. |
| `demo_common.ps1` | Scenarios, defaults, window tiling (Win32), SAP window handles (through VBScript, which needs no type library). |

Headless content check without windows (what the rehearsal on 2026-10-04 used): run
`claude -p "<prompt>" --model sonnet --mcp-config mcp.json --strict-mcp-config --allowedTools mcp__fairyfly` in each
agent folder in parallel with `FAIRYFLY_TOKEN` set from `token.txt`. Measured on 2026-10-04 with four agents: all answered in 24-105 s.
