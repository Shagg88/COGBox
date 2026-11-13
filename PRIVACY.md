# COGBox - privacy

Short version: **COGBox has no telemetry, no ads and no analytics. It sends nothing about you.** The only account is an
optional online-play account, off until you turn it on (see "Online play" below). It goes
online only for the three things below, and two of them only when you ask.

## What COGBox connects to

| When | Where | What is sent | What comes back |
|---|---|---|---|
| **Update check** - at start, after you've accepted the welcome notice. Turn it off on any game's About tab ("Check GitHub for a newer COGBox at start"). | `github.com` (if GitHub doesn't answer: `gitlab.com`, the backup, then `sourceforge.net` while it still exists) | An ordinary web request with the user agent `COGBox-launcher/<version>`. Like any web request, the server sees your IP address. No ID, nothing about your PC or games. | The latest version's number and download link |
| **Downloading an update** - only when you press the update button. | `github.com` and the file-storage host it sends the download to (or `gitlab.com`, `sourceforge.net` / `downloads.sourceforge.net` as above) | Same as above | The new release |
| **Get box art** - only when you press it. | `raw.githubusercontent.com` (the community x360db archive) and `images.launchbox-app.com` (the LaunchBox Games Database) | Requests for each game's images by its public title ID (x360db) or by fixed image addresses (LaunchBox). Your IP address is visible to GitHub and LaunchBox. | Box art and backgrounds, saved in COGBox's folder |

The hosts' own privacy policies apply to those requests (GitHub, GitLab, SourceForge, LaunchBox). Links you click in COGBox (release notes,
the ticket page) open in your web browser.

The games themselves: the Xbox 360 ports don't connect to the internet unless you turn on online play (below). System
Link / LAN co-op talks only to other PCs on your local network, when you use it.

## Online play (off unless you turn it on)

Online play is off by default. While it is off, nothing below happens and the games behave exactly as without it. You
turn it on per profile on any game's Social tab ("Use the COGBox multiplayer server").

**Who runs the server:** the COGBox multiplayer server is run by Flippage, a COGBox contributor, on a cloud server at
`cogbox-mp.duckdns.org` (a different server can be set in `CogBox.ini`, `[Online] Server`). If it is down, online play
and the Social tab don't work until it is back; everything else in COGBox, and offline and System Link play, carry on.

**What the server stores for each account, until the account is deleted:**

- your name and its four-digit number (your tag, `Name#1234`) and the profile picture you chose (the name of one of the
  games' own pictures, never an image or file of yours);
- a made-up player ID (XUID) the games use online, and a hash of the account's secret token (not the token itself);
- your friends list, friend requests and anyone you blocked;
- when the account was made and when it was last used.

**Only while you play:** the lobby you host (its join code, game, region, player count and the game's own session
details) is kept while your game is connected and removed when it disconnects. Invites are kept in memory for at most
10 minutes. Whether you are online and which game you are in is shown to your friends and kept in memory only.
A **public** match you host in Gears 2, Gears 3 or Judgment is listed under Open matches for other signed-in players as
its game, mode, player count, region and whether it has started - never your name, tag or address.

**Your IP address:** to connect two players directly (there is no relay), the server passes each player's network
addresses (public IP address and port) to the other player of a match or party. **Players you play with can see your
IP address**, as with any peer-to-peer game. The server passes them on and doesn't store them. Like any web server, it
sees the address each request comes from.

**Deleting your account:** on the Social tab, press DELETE ONLINE ACCOUNT. Your tag, picture, friends, requests and
blocks are removed from the server at once, any lobby you host is closed, and online play is turned off for that
profile. Saves and achievements stay on your PC. You can also ask for deletion through the contact below.

**All connections to the server are encrypted (HTTPS / WSS).** COGBox refuses an unencrypted server address unless the
server runs on your own PC.

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
Windows user name and PC name are replaced, and so are other players' public IP addresses and your online
account's token. **Nothing is sent.** COGBox can open a new ticket page on GitHub in
your browser and put the report's summary on your clipboard; you decide whether to paste and post it and whether to
attach the zip. Look inside the zip first if you like.
Anything you post in a public ticket is public.

## Crash logs

Logs stay in the COGBox folder. They go anywhere only if you attach them to a report yourself.

## Contact

Questions: see "Concerns and takedown requests" in `LEGAL.md`.
