COGBox - Gears of War native PC ports
=====================================

COGBox starts the native PC ports of Gears of War, Gears of War 2, Gears of War 3 and Gears of War:
Judgment (Gears of War is new in 0.3.0 and still a preview).
These are fan-made ports, rebuilt from the Xbox 360 games' own code by static recompilation. This
is an unofficial fan project, not made by or connected to Microsoft, Xbox Game Studios, The
Coalition or Epic Games.

NO GAME FILES ARE INCLUDED. You need your own copy of each game.


What you need
-------------
- Windows 10 or 11, 64-bit - or Linux / Steam Deck through Proton (see below, experimental).
- A CPU with AVX2 (Intel Haswell / AMD Ryzen or newer, about 2013 on).
- A graphics card with Direct3D 12.
- Your own copy of the game, in one of these forms:
    * a disc image (.iso) of the US disc,
    * a Games on Demand copy from your console's hard drive, or
    * a folder with the disc's files (default.xex and the rest) that you already extracted.
- Each game's title update - REQUIRED. Title updates are now required to play; disc-only versions
  are no longer supported:
      Gears of War             Title Update 5
      Gears of War 2           Title Update 6
      Gears of War 3           Title Update 6
      Gears of War: Judgment   Title Update 4
  Without it the game's box shows "Needs Title Update ..." and ADD UPDATE instead of PLAY.
  The update is a file on a console that has played the game online, in
      Content\0000000000000000\<title ID>\000B0000\
  (Gears 1: 4D5307D5, Gears 2: 4D53082D, Gears 3: 4D5308AB, Judgment: 4D530A26).
- About 8 to 16 GB of free space per game if COGBox unpacks a disc image for you.


Getting started
---------------
1. Unzip the whole folder somewhere you can write to, for example C:\Games\COGBox. Program Files
   won't work, because Windows protects it.
   Keep the folders when unzipping (Windows' Extract All, 7-Zip, or "unzip" on Linux). Some tools,
   such as KDE's Ark, flatten them - COGBox then can't find the games.
2. Start CogBox.exe. The programs aren't code-signed, so the first time Windows may show "Windows
   protected your PC": choose "More info", then "Run anyway".
3. Turn to a game and press ADD YOUR COPY. Choose your disc image, Games on Demand file, or
   default.xex in an extracted folder. You can also drop it onto the COGBox window.
   COGBox works out which game it is, unpacks it next to itself if needed, and checks it is the
   release the ports are made for.
4. Add the title update the same way (it's required): ADD UPDATE, "Add the title update" on the
   game's About tab, or drop the package onto the window. COGBox unpacks it into <Game>\game\update
   and always plays with it.
5. Press PLAY. SETTINGS opens the game's page, which has graphics, frame rate, controls and more.
   Every setting has a short description.

Controller: the D-pad turns the carousel, LB / RB choose Play or Settings, A presses it,
X opens the settings, Y gets box art. On a settings page LB / RB change tab, D-pad down past
the last option reaches Library / Save / Play, and B jumps to Library.
Keyboard: the arrow keys turn it, Enter plays, Esc goes back.
To close COGBox with a controller: Start on the library screen (Esc on the keyboard) asks to quit.
PlayStation controllers work too; COGBox then shows Cross / Circle / Square / Triangle, L1 / R1
instead of A / B / X / Y, LB / RB.

In the game: Back + Start (Share + Options on PlayStation, or F1) opens the in-game menu - the same settings, Resume,
Restart to apply and Quit game. Most settings take effect when the game restarts; Restart to
apply does that for you.


New in 0.4.1
------------
- Steam Deck / Linux (Proton): controllers work in the games again - the game now finds a pad that
  was not there when it started looking (GitHub #108).
- With one player, every connected controller controls player 1.
- Keyboard keys: right-click an action to give it a second key.

New in 0.4.0 - please read
--------------------------
- Title updates are required (see "What you need"). The Gears 2 / Gears 3 disc builds are no longer
  included, and the Game version setting is gone.
- Gears 3 saves are converted automatically the first time you start Gears 3 on 0.4.0 (part of the
  friendly-fire fix, GitHub #68). Nothing to do. A backup of each changed file is kept next to it as
  <file>.pre-xuid0 (PlayerStorage.dat and profile\User\63E83FFF, under Gears3\user\).
  To roll back: close Gears 3, rename the converted file, and rename the .pre-xuid0 copy back to the
  original name. Going back to COGBox 0.3.x? Restore the .pre-xuid0 files first - a converted save
  started on 0.3.x makes Gears 3 reset it. Only Gears 3 saves are converted.
- Gears 3 Cheats and "Unlock all mutators" are removed for now (they only worked with the disc
  version).
- Player name: any game's Settings > About tab > Player name. Up to 15 letters, digits or spaces;
  empty = "User". Every game shows it for player 1. It only changes the name shown - your profile
  folder and saves stay where they are.


Graphics API: DirectX 12 is the default. Vulkan is on each
game's Graphics tab for testing; FSR / DLSS / XeSS and frame generation are DirectX 12 only. If
something looks wrong on Vulkan, please send a problem report.

Fullscreen: F11, or both triggers (LT + RT) together on a controller, from any screen.
The window: COGBox grows with its window. "Borderless window" on any game's About tab hides the
title bar; COGBox then has its own minimise / maximise / close buttons in the top-right corner.

COGBox's language: asked on first start (or Automatic, Windows' language); change it on any game's
About tab. The game's own language: a game's Gameplay tab - every language of the US and European
releases is there; the ones on your copy of the game work, the others fall back to English.
Keyboard and mouse: every key can be changed on a game's Controls tab.
DLC and mods: a game's About tab (Downloadable content, Mods).


System Link - LAN play (experimental; Title Update versions of all four games)
-------------------------------------------------------------------------------
Play the games' System Link modes (co-op and the other System Link modes) with other PCs - in the
same house, or over the internet with a free "LAN VPN". Thanks to HubertFer, whose patch made this
possible. Tested so far: Gears of War 2 campaign co-op. Gears of War 2's other modes and Gears of
War, Gears of War 3 and Judgment have not been tested yet - please try them and tell us (GitHub
issue #72).

What every player needs
- The same COGBox version on every PC (0.4.0) and your own copy of the game with its title update.
  Every player must use the same game.
- Gears 3 on 0.4.0: System Link needs a two-PC test. The friendly-fire fix (#68) changes the random
  numbers and the "online ID" Gears 3 sees. COGBox's own System Link code finds PCs by their IP address
  and doesn't use those IDs, but Gears 3 co-op on 0.4.0 hasn't been tried on two PCs yet - please
  tell us how it goes (GitHub issue #72).
- Turn OFF internet VPNs such as NordVPN, ExpressVPN or Surfshark while you play. They do not link
  PCs together, and they can make System Link use the wrong network.

A. PCs in the same house (same router / Wi-Fi)
1. On every PC: the game > Settings > Gameplay > tick "System Link - LAN play".
2. Start the game on every PC. The first time, Windows may ask whether to allow the game through
   the firewall: tick "Private networks" and allow it.
3. One player hosts. Gears 2 co-op: Campaign > Co-op > Host, then in the lobby set the party type
   to "System Link".
4. The other player picks Join (System Link) and chooses the host's game from the list.
5. Gears 2 co-op: the joining player presses RT ("Press RT to begin") to take over Dom.

B. Friends in another house (over the internet, with Radmin VPN)
Radmin VPN is free and makes your PCs act as if they were on one home network. ZeroTier and Hamachi
work the same way; the steps below use Radmin VPN.
1. Every player: download Radmin VPN from its official website (radmin-vpn.com) and install it.
2. ONE player (the host): open Radmin VPN > Network > Create network. Type a network name and a
   password, and send both to your friends.
3. Every other player: Radmin VPN > Network > Join network, and type that name and password. You
   should now see each other's PCs in Radmin VPN, each with a green dot and an address that
   starts with 26. (for example 26.10.20.30). Write down the host's 26.x address.
4. Every player: make the Radmin network "Private" in Windows. Windows often marks it "Public",
   which hides the game. Windows 11: Settings > Network & internet > Advanced network settings >
   the Radmin VPN adapter > Network profile type: Private. (Windows 10: Settings > Network &
   Internet > Ethernet > Radmin VPN > Private.) If you can't change it, allow the game on
   Public networks when Windows asks.
5. Every player: the game > Settings > Gameplay > tick "System Link - LAN play".
6. Start the game, host and join exactly as in A, steps 2-5.
7. If the joining player does not see the host's game after 10-20 seconds, tell each game the other
   PCs' Radmin addresses: on EACH PC open (or create) the file extra-args.txt in the COGBox folder
   (next to CogBox.exe) and add one line with the OTHER players' addresses, commas between several:
       --net_peer_ip=26.10.20.30
   Save it, restart the game and try again.
   With --net_peer_ip set, the game sends its "find games" messages only to the addresses listed
   there, so with 3 or more PCs every PC must list ALL the other PCs (commas between them).

If it doesn't work
- Can't select System Link / "No Network Connection": the box in step 1 is not ticked.
- The host's game never shows up: check the firewall (Private network, game allowed), that both of
  you are in the same Radmin network (green dots), that internet VPNs are off, then try step B7.
- Please send a problem report (the game's About tab) from BOTH PCs to GitHub issue #72 - it lists
  System Link and helps us fix it.
Not yet: voice chat; hosting from Linux / Steam Deck (joining should work). With the box off the
game works exactly as before.

Linux and Steam Deck (Proton) - experimental
---------------------------------------------
1. Unzip COGBox somewhere in your home folder.
2. In Steam: Add a Game > Add a Non-Steam Game > browse to CogBox.exe.
3. The game's Properties > Compatibility > "Force the use of a specific Steam Play compatibility
   tool": Proton Experimental (or a recent Proton / GE-Proton).
4. Start it from Steam. On a small screen like the Steam Deck's, COGBox starts maximised and
   borderless. Add your copy as on Windows.
Under Proton COGBox skips exclusive fullscreen, Reflex and Anti-Lag (the compositor owns the
screen). The games use Vulkan there by themselves (DirectX 12 does not start under Proton yet).
Steam Input should present the controller as a standard gamepad. Problem reports from Linux
help a lot: "Make a problem report" saves them in COGBox's reports folder (and opens it).

One Steam entry per game (Windows or Linux): add CogBox.exe again for each game and set its
Launch Options to --play gears1, --play gears2, --play gears3 or --play judgment - COGBox then
starts that game straight away with your settings. Adding gow3.exe (or another game program)
itself does not work: it stops with "--game_data_root was not provided", because COGBox passes
it the game folder and your settings.


Where things are
----------------
Everything stays inside the COGBox folder:
  COGBox.ini            what you added, play time, launcher choices
  settings.ini          each game's settings
  <Game>\game\          the files unpacked from your copy (not there if you pointed at your own folder)
  <Game>\user\          saves
  <Game>\logs\          one folder per play: the log, the arguments used, any crash report
  <Game>\shadercache\   shaders built while playing, kept so the next play doesn't stutter
  <Game>\mods\          mods: one folder each, laid out like the game's own files (GearGame\...);
                        tick them on the game's About tab, or add a mod's .zip there
  <Game>\user\0000000000000000\   downloadable content you added on the About tab
  lang\                 COGBox's own translations
  covers\               put your own box art here as gears2.png, gears3.png and judgment.png
                        (or .jpg). Without them, COGBox draws its own covers.

To update, use the UPDATE TO button on the main screen (or the one on any game's About tab), or unzip a new release over the folder. Your
games, saves and settings are not in the zip, so they are kept.


When something goes wrong
-------------------------
If a game closes unexpectedly, COGBox says what the exit code means. It also offers to make a
problem report: one zip on your desktop with the log, the settings and a description of your PC.
It has no saves and no game files, and your Windows user name is taken out. Nothing is sent
anywhere. Attach it to an issue at https://github.com/Shagg88/COGBox/issues


Licences
--------
COGBox and the ports' own code are under the MIT licence (LICENSE.txt). The libraries, fonts and
vendor upscalers included (SDL3, Dear ImGui, miniz, stb, FFmpeg's XMA decoder, the rexglue SDK, AMD FidelityFX, NVIDIA
DLSS, Intel XeSS, the DirectX Shader Compiler) come under their own terms, in the licenses folder.
Fonts: Anton, Barlow and Barlow Condensed, under the SIL Open Font License 1.1.
