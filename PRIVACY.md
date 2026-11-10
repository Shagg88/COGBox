# COGBox - privacy

Short version: **COGBox has no accounts, no telemetry, no ads and no analytics. It sends nothing about you.** It goes
online only for the three things below, and two of them only when you ask.

## What COGBox connects to

| When | Where | What is sent | What comes back |
|---|---|---|---|
| **Update check** - at start, after you've accepted the welcome notice. Turn it off on any game's About tab ("Check GitLab for a newer COGBox at start"). | `gitlab.com` (if GitLab doesn't answer: `sourceforge.net`, the old home, while it still exists) | An ordinary web request with the user agent `COGBox-launcher/<version>`. Like any web request, the server sees your IP address. No ID, nothing about your PC or games. | The latest version's number and download link |
| **Downloading an update** - only when you press the update button. | `gitlab.com` and the file-storage host it sends the download to (or `sourceforge.net` / `downloads.sourceforge.net` as above) | Same as above | The new release |
| **Get box art** - only when you press it. | `raw.githubusercontent.com` (the community x360db archive) and `images.launchbox-app.com` (the LaunchBox Games Database) | Requests for each game's images by its public title ID (x360db) or by fixed image addresses (LaunchBox). Your IP address is visible to GitHub and LaunchBox. | Box art and backgrounds, saved in COGBox's folder |

The hosts' own privacy policies apply to those requests (GitLab, SourceForge, LaunchBox). Links you click in COGBox (release notes,
the ticket page) open in your web browser.

The games themselves: the Xbox 360 ports don't connect to the internet. System Link / LAN co-op talks only to other
PCs on your local network, when you use it.

## What COGBox reads and writes on your PC

- **Pictures for the player icon:** when you start a game from COGBox, the game saves its own gamer pictures, title
  image and achievement icons as PNG files in `achievements\<TitleID>\` in the COGBox folder. COGBox itself carries no
  console key and doesn't decrypt your game files.
- **Reads:** the game copies you add, title updates and DLC, to check them and start the game. Your Windows display language, to pick COGBox's.
- **Writes:** inside the COGBox folder (`CogBox.ini`, `settings.ini`, logs, shader caches, unpacked games, box art)
  and the games' save folders. Old files are moved to `_old-files`, not deleted.
- **Your Player name** is stored only in `CogBox.ini` on your PC.

## Problem reports

When you make a problem report, COGBox writes **one zip file on your desktop** with the logs, your settings and a
description of your PC (Windows version, CPU, graphics card and driver, displays). No saves and no game files; your
Windows user name and PC name are replaced. **Nothing is sent.** COGBox can open a new ticket page on GitLab in
your browser and put the report's summary on your clipboard; you decide whether to paste and post it and whether to
attach the zip. Look inside the zip first if you like.
Anything you post in a public ticket is public.

## Crash logs

Logs stay in the COGBox folder. They go anywhere only if you attach them to a report yourself.

## Contact

Questions: see "Concerns and takedown requests" in `LEGAL.md`.
