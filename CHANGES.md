# COGBox changes

## 0.6.2 (in progress - not released)

<a href="https://recomplauncher.com/game/cogbox"><img src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" alt="Get it on RecompLauncher" height="56"></a>

COGBox is on RecompLauncher, the free home for recompiled PC games. Get it straight from its developer:
https://recomplauncher.com/game/cogbox

### Launcher
- **Add your game without drag and drop.** Every player now has **Choose a file...**, **Choose a folder...** and
  **Paste a path** on the main screen and on each game's About tab - for the Steam Deck's Game Mode and the Linux
  desktops where dragging onto a Proton window doesn't work. Under Linux, **Paste a Linux path** and **Look for my
  games** are still there.
- **Import saves...** (a game's About tab): your own saves of that game from Xenia (its `content` folder) or from an
  Xbox 360 (the `Content` folder copied from the console's USB stick or hard drive), with the profile's game progress.
  Whatever it replaces is moved to a `_saves-before-import-...` folder in the game's saves folder, never deleted; if
  anything can't be put in place, nothing is changed.
- **The tested versions are shown:** each game's About tab and the title-update notice name the game and title-update
  versions COGBox is made for, and how many other copies players have reported working. The README lists them too.

### Vulkan
- **No more short freeze every ~15 seconds on Vulkan.** Saving the shader cache read the whole cache on the drawing
  thread (100-335 ms each time); it now happens in the background.
- *Still being tested (off by default):* building a chapter's shaders behind its loading screen before play starts, a
  background thread for new shaders, and Vulkan pipeline libraries (the method DXVK uses).

## 0.6.1

### Judgment
- **Judgment finally looks like it does on the Xbox 360.** The game measures how bright each scene is and sets its exposure from that, and that measurement never reached it, so every scene came out about twice as bright as on the Xbox 360 (washed-out skies, grey shadows). It now does, at no cost in frame rate. Checked against Xbox 360 footage. GitHub #185, #250.
- The launcher's **Brightness** setting for Judgment is gone: it only existed to hide this, and could not bring back the washed-out highlights. A saved Brightness value is ignored.

### Performance
- **Judgment runs up to 72% faster.** Measured unlocked at 1440p on an RTX 3080 Ti in Wharf District: **Vulkan 50 to 87 fps (+72%)**, **DirectX 12 81 to 107 fps (+32%)**.
- **Vulkan is much faster once a game has loaded** (Windows; still experimental). Vulkan now skips everything the camera can't see ("real occlusion"), as DirectX 12 already did. Not yet under Proton / Wine, where real occlusion still has to be tested.
- **Vulkan, for testers:** a shader optimiser and an on-disk store for converted shaders are in this version but switched OFF (`--gow2_renderer_vulkan_spirv_optimise=true` and `--gow2_renderer_vulkan_spirv_store=true` in `extra-args.txt`). They halve the graphics driver's work per new shader pair (364 to 195 ms on an RTX 3080 Ti) and later start-up waits, but as wired today they make the first start much longer, so they stay off until Vulkan shaders are prepared in the background.
- **Known, on both DirectX 12 and Vulkan:** the very first time a PC plays a game, its graphics driver has to build thousands of shaders (a few minutes of work), and chapters started before that is done can stutter until it is. "Prepare all shaders now" on a game's Graphics tab does all of it up front. Making this automatic is what we are working on next.
- **The graphics card no longer waits for the whole frame.** It now starts on each frame while the rest of it is still being prepared, instead of sitting idle until everything is ready. No setting to change.
- **0.6.0 against 0.6.1**, both changes together, measured unlocked on the same PC:

  | Game | DirectX 12 | Vulkan |
  | --- | --- | --- |
  | Judgment | 81 to 107 fps (**+32%**) | 50 to 87 fps (**+72%**) |
  | Gears 3 | 99 to 108 fps (**+9%**), **+21%** in the heaviest stretch (84 to 102) | 85 to 93 fps (**+10%**) |
  | Gears 2 | 119 to 124 fps (**+4%**) | 106 to 115 fps (**+8%**) |
  | Gears 1 | unchanged | unchanged |

  (`--gow2_renderer_submit_draws=0` in `extra-args.txt` turns the early start off if it ever causes a problem.)
- Smoother sound and frame pacing: a stall when the sound output device is checked is gone, and several small renderer costs were removed.

### HDR (by legoliamneeson)
- **Native HDR in Gears 2 and Gears 3:** the picture is carried in high dynamic range from the game's own lighting to the screen, instead of being expanded from the standard picture. Gears 1 and Judgment keep the HDR of 0.6.0. `--gow2_hdr_native=false` goes back to it in every game. GitHub #267.

### Online play (by Flippage)
- **Smoother online and System Link matches in Gears 2, 3 and Judgment:** the games' 2008 Xbox Live network limits (7000 bytes a second per player, 45 updates a second from the host) are raised to 100,000 bytes a second and 60 updates. In testing, the old limit held the game back for about 40% of a round. A full lobby's host may now upload up to about 7 Mbit/s in the busiest moments.
- **Open matches** (an idea from Shagg): the Social tab and the in-game Social menu list the public matches in Gears 2, Gears 3 and Judgment that have real players in them, such as "KOTH · 6/10 · Gears 3 · US · In progress", with a **Join** button. Join in progress puts you in a bot's place. Only matches are listed, never parties, and no names are shown. Gears 1 isn't included (it has no join in progress).
- **Online privacy:** the games' lobby search no longer shows join codes, and a hosted lobby no longer carries your PC's local network address. The search now signs in, so the server can require it later (see PRIVACY.md).

### Fixes
- **Gears 3:** a crash with a "disc read error" when a level or a Horde match starts should be gone (the game was told that parts of the disc had failed a check that has no meaning here). GitHub #271.
- Mods added from a `.zip` unpack into the mod's own folder.
- The updater refuses oversized downloads and duplicate files in an update.
- Safer handling when the graphics device is lost while the game waits for it.

## 0.6.0

### COGBox is back on GitHub
- **GitHub is COGBox's home again:** https://github.com/Shagg88/COGBox (downloads, release notes, Issues). GitLab (https://gitlab.com/shagg88/cogbox) is the full backup, with the same releases.
- The updater checks GitHub first, then GitLab, so updates keep coming even if one site is down. The "open a ticket" buttons and links go to GitHub.

### Online play (by Flippage)
- **Online multiplayer** through COGBox's own matchmaking server, peer to peer (nothing is relayed through a server). It's off until you turn it on. What is sent and kept is in [PRIVACY.md](PRIVACY.md), and the launcher can delete your online account.
- **Social**: friends, invites and profile pictures, also as a tab of the in-game menu (F8 opens it there).
- Works behind strict routers without UPnP: when two players have no direct path, their connection is set up over a route that already works between the players, still peer to peer (no relay server). A party host's join connection is no longer closed by the game right after it opens.
- Regions: **US**, **OCE** and **Europe** (US West and US East are now one US region).
- Gears 3 and Judgment saves keep working when you switch online play on or off. Nothing is deleted, and Gears 3 saves are backed up before they're changed.
- **Online safety:** limits on what another player can make your game hold in memory or open.

### Mods (by KiltedKernel)
- **Campaign mods:** a mod can add its own campaign with chapters to Select Chapter (Gears 3), without clashing with the game's acts or other mods.
- **Mods tab:** shows what the game will do with each mod: a mod it leaves out and why, where each campaign chapter goes, and `mod.ini` keys it doesn't understand.
- Mods can set class defaults (`[ClassDefaults]`), checked against the setting's real type.
- [docs/modding.md](docs/modding.md): the mod format for creators. Developer tools: a scripted controller (Gears 1 and 3), a property log and a Kismet trace.

### Also new
- **Delete a profile:** sign in to it, then Profiles > **DELETE THIS PROFILE**. Its saves, name and picture move to the `_old-files` folder (nothing is erased); profile 1 can't be deleted.
- **Gears of War 1 System Link:** smoother network play with higher net speed limits. GitHub #72.
- **In-game menu:** Back + Start (or F1) opens the menu on the tab you last used.
- **More button prompts**: extra prompt sheets (GitHub #223).

### Faster
- **Gears of War 3:** the Character LOD option is gone; the game always uses its original character detail. The lower settings cost a lot of frame rate.

### Fixes
- **Gears 2:** "Exit Game" in the game's own menu now quits after you've played a level, instead of opening the credits. The in-game menu's Quit box can be answered Yes again.
- **Judgment:** fixed a freeze where the game waited on the graphics card forever. It now gives up after 2 seconds (by Flippage).
- **Sound:** sounds now start where the game asks instead of always from the beginning, and the game no longer reuses a sound the decoder is still reading (from Xenia Edge).
- **Timeouts:** some timed waits in the games never ran out; they do now (from Xenia Edge).
- **Launcher:** COGBox comes back to the front after you quit a game from its in-game menu.
- **Mods:** a game no longer crashes when a mod writes to memory the game has freed; mod folders and files with non-Latin names (Chinese, emoji, etc.) no longer crash the Mods list.

## 0.5.0

### Important: update by hand this one time
COGBox now lives on **GitLab**: https://gitlab.com/shagg88/cogbox - downloads, release notes and bug reports (Issues).
The updater in 0.4.8 and older only knows GitHub, so **download 0.5.0 by hand once** and unpack it over your COGBox folder (or into a new one). From 0.5.0 on, COGBox updates itself from GitLab.

Releases are now **.7z** files. Unpack with [7-Zip](https://www.7-zip.org/) (free) or Windows 11's built-in support.

### Cleaner updates
When a new version starts for the first time, COGBox tidies up after older ones. Program files, DLLs and shader caches that an older COGBox shipped or made, and that the new version no longer uses, are **moved** to the `_old-files` folder, never deleted. Your saves, settings, mods, covers and your own DLLs (ReShade, a custom DLSS file) are never touched. Old settings are carried over to the new ones, with a backup copy kept.

### A new in-game menu
The in-game menu (Back + Start, Share + Options on PlayStation, or F1) is redesigned to match the launcher: full screen, the Noto Sans font, and on ultrawide screens a centred 16:9 area.
- Works fully with a controller: the D-pad moves through the settings, left / right change them, LB / RB switch tabs.
- **Quit game asks first** (Yes / No), so you can't quit by accident.
- **Many settings now apply at once**, no restart: frame-rate cap, brightness, mouse, HDR levels, sharpening, FPS bar, popups, sound, vibration, dead zones, vsync (DirectX 12), windowed / borderless and window size, SMAA, button prompt style, and pausing AMD frame generation. Settings that still need a restart are marked "Restart required".

### Controls
- **Aim sensitivity** and **Zoom sensitivity** (%) for the mouse (Controls tab, and live in the in-game menu): slower or faster turning while aiming and while zoomed through a scope (Longshot). All four games.

### In-game menu: sliders and controls
- Sliders now move in their own steps in all four games, with the controller or the keyboard (arrows or WASD).
- The button legend at the bottom follows what you are using: the controller's buttons when you use a controller, keys when you use the keyboard.

### Faster
- Faster game code in all four games (up to ~5-10% in CPU-limited scenes). Always on; if a game's faster build crashes twice in a row, COGBox quietly switches that game to its normal build.
- **Smoother play:** far fewer stutters while new areas and textures stream in, in all four games. Textures load and unpack faster and new shaders no longer stall a frame for long. On a Ryzen 7 3700X + RTX 3080 Ti: 1% lows went from 20-26 fps to 28-29 fps in every game, and frames over 50 ms dropped by 30-70%. Gears 1 also runs about 30% faster on average and Gears 3 about 20% faster.

### Insane from the start
- **All four games:** Insane can be chosen without finishing the campaign first. Prefer the original rule? Turn on **Original unlock rules** (Gameplay tab).

### Sharper keyboard prompts
- The keyboard and mouse key caps in the games are bigger and sharper, also on small prompts and at 720p.

### Fixes
- **Slow start with keyboard and mouse:** Gears 2, Gears 3 and Judgment could take about two minutes to start after pressing Play, with the launcher "Not Responding". They start in a few seconds now.
- **Gears of War 1:** a rare crash at start is fixed. The mouse works again with DLSS frame generation.
- **Quitting with DLSS frame generation on** (from the in-game menu or by closing the window) no longer crashes or freezes, in all four games.
- **FPS bar with frame generation:** shows the frame rate you actually see, with the drawn frame rate beside it, e.g. "180 FPS (frame gen; 60 drawn)".
- **In-game menu:** the Quit box can be answered with the keyboard and controller in every case.
- **Gears of War 2:** the chainsaw rev prompt shows the right key cap.
- **PlayStation button prompts on an Xbox controller (#249):** COGBox switched to PlayStation prompts whenever any Sony device was connected (a paired controller, a headset, PS Remote Play, DS4Windows). Now only real controllers count and an Xbox controller wins. PlayStation players using DS4Windows or Steam Input get PlayStation prompts. New launcher setting: **Controls > Button prompt style** (Automatic / Xbox / PlayStation / Switch / Steam Deck).
- Giving the same game option twice (for example in extra launch options) made the game ignore all its options. Now the last one wins.
- A custom DLSS file could be moved away by the update cleanup. Fixed.
- **Gears of War: Judgment brightness:** the default is now 75 (was 65). If yours was still at the old default, it is lifted to 75 once; a value you set yourself is kept.
- Split-screen: player 2-4's settings are now saved (before, they read player 1's and changes were lost).
- Safer DLL loading: vendor DLLs load only from the game's folder or COGBox's shared folder, and Intel's must be signed by Intel.
- The updater can no longer overwrite your mods, saves or achievements folders, and moves old files instead of deleting them.

### Graphics
- **AMD FSR 4.1** (FidelityFX SDK 2.4). FSR 4 needs a Radeon RX 9000 card; other cards use FSR 3.1. **FSR on Vulkan** is new as an option.
- **DLSS model choice** (Graphics tab): Automatic, or pick the DLSS preset yourself.
- **Custom DLSS file:** point COGBox at an official NVIDIA nvngx_dlss.dll you prefer. COGBox keeps its own copy and you can switch back any time.
- **New, experimental: Intel XeSS frame generation** (Gears of War 2, DirectX 12) with Intel's low-latency mode. Runs on Intel Arc and, per Intel, other recent cards. We tested it on an NVIDIA card only, so please tell us how it runs. If it can't run, the game uses AMD frame generation instead.
- **Frame generation ghosting:** less smearing of characters in the in-between frames (Gears of War 1 and Gears of War 3).
- **4:3 aspect ratio** (Display tab) for Gears 2, Gears 3 and Judgment. Auto stays 16:9.
- Updated components: DirectX 12 Agility SDK 1.620, DirectX Shader Compiler 1.9, XeSS 3.0.2, SDL3, FFmpeg.

### Split-screen
- With Players set to 2 or more, a **name box for each extra player** appears next to player 1's sign-in.
- Clearer help: in the game choose **Campaign > Play Split-Screen**; player 2 is signed in automatically. If a controller shows up twice, turn off Steam Input or DS4Windows.
- The log now lists which controller went to which player.

### Smaller download
NVIDIA, AMD and Intel files used by all four games are now stored once, in a `shared` folder, instead of four times: about 315 MB less unpacked.

### Library
- **HD box art and backgrounds:** UPDATE ART now downloads full-size box fronts and 1920x1080 backgrounds from the LaunchBox Games Database (gamesdb.launchbox-app.com) - thanks to LaunchBox and its contributors. Cover artwork belongs to Microsoft / Xbox Game Studios and Epic Games.
- **Profiles:** each person gets their own name, picture and saves (Profiles on player 1's card); downloaded content is shared. Players 2-4 can be signed out from their cards.

### Known issues
- **Judgment too bright at higher resolution scales (#185, #250):** we could not reproduce it in 0.5.0 (Onyx Point and the start of Halvo Bay measure the same at scale 1 and 3). If you still see it, please tell us how.
- **Gears of War 1 split-screen:** Aim / Zoom sensitivity follows the camera drawn last, so player 1's mouse can change speed when player 2 aims.
- **Judgment, chapter 12 (SPE_E1_P):** a rare crash a few seconds after the chapter starts (about 1 in 16 starts in our tests, older versions too). Start the chapter again.
- **AMD FSR frame generation on Vulkan can freeze on NVIDIA graphics cards while the NVIDIA overlay (NVIDIA App, Alt+Z) is on** (0.4.8 too). The launcher warns about it. Turn the NVIDIA overlay off, or use **DirectX 12** for frame generation. AMD and Intel cards and the Steam Deck are not affected.

### Also in this release
- Translations for all new text in all 11 languages (machine-made; corrections welcome). Hungarian corrected by Hevesi J. - thank you!

### Thanks
- **Flippage** - the gow3-recomp work behind Gears of War 3's Insane unlock and our native mouse look, and the in-game menu's mouse and controller input.
- **KiltedKernel** - modcore, the mod loading that COGBox's mod support builds on.
- **Hevesi J.** - the Hungarian translation corrections.

### Licences
Third-party licences (NVIDIA Streamline, AMD FidelityFX, Intel XeSS, SDL, FFmpeg and others) are in each `licenses\` folder and in `THIRD_PARTY_NOTICES`. The FFmpeg source is offered next to the release.

Contributors: **Flippage**, **KiltedKernel** and **Hevesi J.**

COGBox is not affiliated with Microsoft, Epic Games or The Coalition.
