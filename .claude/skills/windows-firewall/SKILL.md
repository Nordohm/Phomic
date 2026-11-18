---
name: windows-firewall
description: How Windows Firewall treats a desktop program that accepts connections - the first-run question, what answering no does, reading the rules without admin rights and fixing them with one UAC prompt. Use when a Windows program listens on the network, when "it works on my PC" fails on another, or when users can't connect to a PC program.
---

# Windows Firewall and a listening program

## What Windows does

- The first time a program listens (TCP `listen`, UDP `bind`) on a network where inbound traffic is blocked by
  default, Windows asks whether to allow it, with checkboxes for private and public networks.
- **Allow** adds inbound allow rules (one TCP, one UDP) for that exe path and the checked kinds of network.
- **Cancel** (or closing the question) adds inbound **block** rules for that path and the current kind of network.
  Windows doesn't ask again for that path, also not after updates that keep the path. The program gets no
  error: connections simply never arrive.
- Rules are per path: every build folder and install location is asked about separately, and each gets its own
  rules (all named after the exe, such as `app.exe`).
- Bluetooth, USB and outbound connections aren't affected.
- A per-user installer (no admin rights) can't add firewall rules, so the question can't be avoided there.

## Telling the user

Read the rules with `INetFwPolicy2` (`CoCreateInstance(__uuidof(NetFwPolicy2))`); no admin rights needed, and
going through all rules takes well under 100 ms. Off the UI thread.

1. `get_CurrentProfileTypes`: the kinds of network the PC is on now (domain, private, public bits).
2. Go through `get_Rules`: inbound rules whose `ApplicationName` is this exe (compare case-insensitively, after
   expanding `%…%` variables). Note per profile bit which enabled rules allow TCP, allow UDP (protocol 6, 17 or
   256 = any), or block.
3. For each current profile where `get_FirewallEnabled` is true: **blocked** if a block rule covers it or
   `get_BlockAllInboundTraffic` is on; also blocked if no allow rule covers it, `get_DefaultInboundAction` is
   block, and either some rule exists for the program (Windows asked before) or `get_NotificationsDisabled` is
   on (Windows won't ask).
4. No rule at all: Windows hasn't asked yet; show nothing, its question will come.

Recheck now and then while the window is open, so a fix made in Windows' own settings clears the message.

## Fixing it

- Needs admin rights: relaunch the program elevated (`ShellExecuteEx` with the `runas` verb) with an argument
  that only does this and exits with the `HRESULT` as exit code. Wait for it off the UI thread;
  `ERROR_CANCELLED` means the user declined the UAC prompt.
- Change the existing block rules' `Action` to allow (Windows' own "Allow an app" does the same), rather than
  deleting them: `INetFwRules::Remove` works by name, and every copy's rules share the exe's name.
- If no allow rule covers the current kind of network, add one (protocol any, inbound, those profiles). Give it
  your own name and reuse it next time instead of adding more.
- Rule changes apply at once; the listening socket doesn't need restarting.

## By hand

`wf.msc` › Inbound Rules › the program's rules with the block icon › delete them, then restart the program and
answer the question again.
