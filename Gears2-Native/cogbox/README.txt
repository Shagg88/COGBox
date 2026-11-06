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
  Without it the game's box shows "Needs Title Update ..." instead of PLAY.
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
3. Drag and drop your disc image, Games on Demand file, or extracted game folder onto the
   COGBox window (this is the only way to add games, title updates and DLC).
   COGBox works out which game it is, unpacks it next to itself if needed, and checks it is the
   release the ports are made for.
4. Add the title update the same way (it's required): drop the package onto the window. COGBox unpacks it into <Game>\game\update
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


New in 0.4.6
------------
- Games, title updates and DLC are now added only by dragging and dropping them onto the window.
- Your player name is on the library screen now (click it, or press Back / View, to change it), and ACHIEVEMENTS is
  a full button next to PLAY and SETTINGS.
- New installs start in borderless fullscreen with SMAA on. Your saved settings are kept.
- Gears 1: button prompts for PlayStation and other controllers (#214).
- Gears 2: ultrawide screens no longer get the 4:3 camera offset (#132).
- Gears 3: no more freeze at the main menu.
- No sound on one output device: a game muted or at zero in the Windows volume mixer is set back to full (#203).
- System Link: Gears of War co-op reported working on two PCs (thanks to LenzTRO, #209); the setting's
  tip and this README say so.
- Achievements in all four games (thanks to Flippage, PR #187): a popup when one unlocks (Xbox 360, Xbox or
  PlayStation style, colour, position, sound), an Achievements page in the in-game menu (F7) and an ACHIEVEMENTS tab
  in the launcher.
- Ambient occlusion: AMD FidelityFX CACAO as a cheaper choice next to XeGTAO, and XeGTAO / CACAO in Judgment,
  which also fixes its shadow flicker with DLSS; DLSS / FSR / XeSS get sharper textures when they draw the scene
  below the output size, and Judgment's DLSS takes the finished picture with real depth (thanks to gmihaic,
  PR #204).
- COGBox's own text in the game (the in-game menu, the Achievements page, popups, shader notes) is in the
  launcher's language. Korean, Chinese and Japanese use the fonts Windows has.
- Launcher: simpler tabs with headings and the rarely used settings under ADVANCED, explanations as tooltips,
  a MODS tab and an ABOUT tab, and Graphics API back in the main list.
- Gears 3: depth of field can be drawn at the render resolution instead of 720p (Graphics -> Advanced, off by
  default; #105).
- Gears 3: "Unlock Insane from the start" (Gameplay; off by default; the other games are not done yet - #174,
  thanks to Flippage's Gears 3 recomp).
- Bloom can be turned off in Gears 2, 3 and Judgment, and Film grain in Judgment (from Xenia Canary's game patches
  by boma).
- Gears 1: Motion blur and Depth of field can be turned off (#179; from Xenia Canary's game patches by illusion and
  boma). Depth of field off also softens the bloom.
- A language or voice language your copy doesn't have now says so under its setting, with what the game uses
  instead (#90).
- A title update made for another release of the game is caught before the game starts, instead of the game
  stopping at start (0xC0000409).
- Crash reports also for the game's "fail-fast" exits (0xC0000409), and Windows is asked to keep a small dump.
- COGBox in a OneDrive, Dropbox or Google Drive folder: an "In a cloud folder" line says so (its tooltip has the
  advice).
- Mouse look when zoomed (#175), a clearer message for missing program files (#194), DLC found again after the
  0.4.5 update (#193 / #201), Gears 3 TU6 mod campaigns with more than five acts in Select Chapter (thanks to
  KiltedKernel, PR #205), Gears 3's path choice picked with the mouse button on that path's side (thanks to
  KiltedKernel, PR #206), the movie log (#103), screenshots saved at the size shown, the Vulkan pipeline cache read
  up to 2 GB, and user folders with non-Latin letters for the shader caches (#177).
- Frame-rate limit: the limit you pick is the limit. With Vsync it is no longer lowered to 3 under your screen's
  refresh rate (60 stays 60); only a limit above the refresh rate is brought down to it. The quality presets no
  longer change it.
- Keyboard: holding the Walk key on Ctrl no longer stops W/A/S/D, and walking is slower (#196, thanks to KiltedKernel,
  PR #197).
- Mouse sensitivity in steps of 0.01 (#195, thanks to KiltedKernel, PR #198).
- Main menu: an Exit Game entry closes the game (thanks to Flippage, PR #168). It takes the place of Credits in
  Gears 2 and of the Marketplace entry in Gears 3 and Judgment.
- Mouse buttons 4 and 5 can be bound to actions (#208).
- Judgment default keys: Grenades on 4 (LB) and Tactical display on Tab (D-pad down) (#208).
- Gears 3: side-by-side split screen is set again at every level, not only the first (#208 / #136).
- Gears 3 Title Update 6: no more crash in the Act 1 jail cutscene (#207).
- Faster: textures the game draws into on the graphics card are no longer copied back and reloaded every frame
  (about 12 a frame in Gears 2).
- A crash right after a title update that doesn't match the game now says so (#183).
- Judgment: after a loading screen the 30 fps loading limit ends sooner (3 s after loading instead of 6, at most
  8 s while the level streams in instead of 20).
- XeGTAO High uses two smoothing passes instead of three (a little faster, slightly less smooth); Ultra chooses its
  passes by itself (two without DLSS / FSR / XeSS, fewer the lower DLSS / FSR / XeSS draws the scene).
- Language: packages stored compressed as a whole are now read when switching the voice language.
- Gears 1 launcher: says when the chosen Scene resolution can't be used at the render scale (#98).
- New logs and test switches for reported issues (#90, #105, #131, #132, #185, #193, #202), all off unless asked for.
- Launcher: shorter notices, one title-update notice after adding a game, the toolbar together at the top right,
  at most three covers, a button legend with pictures for your controller or keyboard, HDR shown as
  "HDR (experimental)".

New in 0.4.5
------------
- Gears 1: no more crash while loading with a different voice language (GitHub #176). Voice language is greyed
  in Gears 1 for now: its voice files can't be checked yet, so it would change nothing.
- Vulkan on laptops uses the dedicated graphics card instead of the built-in one (#176).
- Folders with accented letters (e.g. GRAFICA with an accent) work for the game, user, update and log folders
  (#177, thanks to KiltedKernel, PR #178).
- Languages your copy of the game does not have are greyed out, voices too (#159, thanks to KiltedKernel, PR #172).
- Keyboard: a Walk key (Controls tab, KEYBOARD KEYS; none at first), held or pressed once (#125).
- Steam Deck / Proton: controllers are found when the game starts with none known (#108). Mods can add new files,
  not only replace them (thanks to KiltedKernel, PR #122).
- Gears 2 / Gears 3: "Mouse turn when aiming" now starts at the game's own slower aim (#169).
- Smoother frame rate: the picture is presented from the window's own thread, in all four games.
- Launcher: an EXIT button, it comes back to the front when a game closes, a "COGBox updated" note with a link to
  the changelog, the same Graphics and Display options in all four games (ones a game can't do yet are greyed with
  the reason), and Real occlusion is set for you per game. With DLSS / FSR / XeSS at full scene size, the log says it
  is anti-aliasing only (DLSS runs as DLAA), with no extra frames (#98).

New in 0.4.4
------------
- Screenshot key: \ (backslash) saves a screenshot in all four games, to Pictures\COGBox\<game>.
- Voice language: a game no longer freezes or crashes at start-up when the voice language differs from the game's
  language. Gears 2 (GitHub #145, thanks to KiltedKernel, PR #156), Gears 3 and Judgment (#153).
- Gears 3 with XeGTAO: a crash fixed (GitHub #146, thanks to KiltedKernel, PR #148).
- Gears 3: versus, Horde and Beast are no longer held at 60 fps (the menu limit stayed on there, #157).
- Launcher: the library music plays again (#151). Keyboard keys are kept per game (#154).
- The mouse cursor hides after 3 seconds without mouse movement, so it no longer sits over the game when you play
  with a controller (#143).
- DLSS is marked experimental again (flickering in Gears 1 and Judgment, smearing in Gears 3), and so is Vulkan in
  Gears 3 and Judgment on Windows (some things still look wrong there; on Steam Deck / Linux Vulkan stays the usual
  choice) and XeGTAO (it costs too much frame rate for now). DirectX 12 with SMAA is the tested setup on Windows.


New in 0.4.3
------------
- Gears 3 mods can do more than replace files: mod.ini rules (requires / game / enabled), config fragments, bare
  Xbox 360 files at a mod's top, new campaign chapters (32-49) and content packages. Thanks to KiltedKernel (PR #122).
- Mods that add new files (not only replace them) now work in Gears 1, Gears 3 and Judgment (PR #122).


New in 0.4.2
------------
- Vulkan and DLSS are no longer experimental - both now work as they should. Vulkan: purple / wrong textures in Gears 3 and Judgment fixed, other picture
  fixes, and Vulkan uses less CPU per frame. DirectX 12 stays the default.
- Ambient occlusion (Gears 2 / Gears 3, Graphics tab): XeGTAO high / ultra - sharper, full-resolution contact
  shadows. Default: Game's own. Thanks to gmihaic (GitHub PR #139).
- Anti-aliasing: SMAA T2x (Gears 2 / Gears 3) - smoother edges and much less shimmer, DirectX 12 and Vulkan
  (PR #139).
- DLSS / FSR / XeSS: better motion vectors and depth in Gears 2 and Gears 3 (PR #139).
- About tab: "Report a problem" makes the report zip, opens a pre-filled GitHub issue and shows the zip to
  drag into it.
- Untested game versions or title updates still play, with a clearer warning; please don't report problems
  you get with them.
- Gears 2: ultrawide 32:9 is no longer cut to 21:9 (GitHub #137); the camera turns the same amount per mouse
  movement in every view (#52).
- Gears 3: two-player split can be side by side (experimental, #136).
- In-game menu: mouse control, and the game no longer gets input while the menu is open (based on PR #134 by
  Flippage).
- Launcher: row descriptions wrap inside the page (#123).
- Thanks to gmihaic (PR #139) and Flippage (PR #134), whose pull requests are in this version, and to KiltedKernel (PR #122) and legoliamneeson (PR #43) for their contributions.


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
possible. Tested so far: Gears of War 2 campaign co-op, and Gears of War co-op (two PCs, about an
hour, no problems; thanks to LenzTRO, #209). The games' other modes, Gears of War 3 and Judgment
are less tested - please try them and tell us (GitHub issue #72).

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
Fonts: Anton, Barlow and Barlow Condensed (the launcher), and Inter (the achievement popups, assets\fonts), under the SIL
Open Font License 1.1. Achievement sound (assets\sounds\achievement.wav): made with ElevenLabs (elevenlabs.io).
