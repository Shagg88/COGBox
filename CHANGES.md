# COGBox changes

## 0.6.2

**Quick start:** download `COGBox-0.6.2.zip`, right-click it > **Extract All...** to a folder of its own (for example
`C:\Games\COGBox`), start `CogBox.exe`, and add your game copy and its title update.

<a href="https://recomplauncher.com/game/cogbox"><img src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" alt="Get it on RecompLauncher" height="56"></a>

COGBox is on RecompLauncher, the free home for recompiled PC games. Get it straight from its developer:
https://recomplauncher.com/game/cogbox

### New and experimental (all off by default)
- **First-person camera** (Gameplay tab, **experimental**, Gears 3 and Gears 2): the game's own camera moves to your
  character's eyes while you walk, crouch, aim and roadie run. In cover, executions, when you're down, in vehicles, on
  turrets and in cutscenes it goes back to the usual over-the-shoulder view. **Page Up** switches it off and on in the
  game. In Gears 3 you see your own arms, body and gun, while your head is hidden (with the game's own big-head
  control) so it doesn't fill the view. In Gears 2 you see your body and gun too, but it hasn't been tried in a real
  play-through yet - please tell us how it goes.
- **DualSense adaptive triggers** (Controls tab, now **all four games**, **experimental**): with a PS5 DualSense
  controller (USB or Bluetooth), the triggers push back, click or buzz to match the weapon in your hands, and shake
  while the chainsaw revs. Other controllers are not touched. If Steam Input is on for the controller, Steam keeps the
  triggers. **We haven't tried it on a real DualSense yet** - if you have one, please turn it on and tell us on GitHub
  #245 what you feel (and send the game's log if something is off). Suggested by HubertFer, GitHub #245.

### Launcher
- **Dead bodies** (Gameplay tab, new, **Game's own** by default): **More** or **Many** keeps more fallen enemies on the
  ground (with Many, up to 20 in Gears 2 and 32 in the other games), and in Gears 1, Gears 3 and Judgment for longer.
  Cutscenes still clear them. It costs some performance and memory. All four games. GitHub #306.
- **Duplicate DLC is marked** (a game's About tab, Downloadable content): a DLC package whose files are all in another
  package you have installed, with the same contents, is marked "duplicate of ... - can be removed", and a **Remove
  duplicates** button removes them all after asking. Duplicates give the game the same items twice (for example the
  same skin twice in Gears 3). Only COGBox's unpacked copy is removed - your own package files are not touched, and the
  package that covers it is always kept. Thanks to Lerrycapetime for the tip, GitHub #283.
- **Cheats for Gears 1, Gears 2 and Judgment** (Gameplay tab, off by default): the developers' cheat keys F5-F11 (god
  mode, slow motion, free camera, hide the HUD, all weapons), as Gears 3 already had. Gears 2: only in games on your
  own PC, never in System Link or online games. Judgment: also in a System Link or online game you host, where slow
  motion and the rest affect every player - please keep them to your own games. Gears 1 has no free camera, so its
  F9 flies you through walls instead; Gears 1 in System Link is not checked yet. GitHub #264.
- **Controls tab: a clear note when Keyboard and mouse is off.** The key list could be changed while the keyboard was
  switched off and then did nothing in the game; an amber note above the list now says so. GitHub #281.
- **More of the launcher is translated:** the Social tab, the Linux System Link note, region names, friends' status
  and the new 0.6.2 text, in all 11 languages. **These translations are machine-made** - if something reads wrong in
  your language, please tell us on GitHub or Discord.

### Gears 1
- **DualSense adaptive triggers**, **Cheats** and **Dead bodies** (see above).

### Gears 2
- **The green lightning on the way to Nexus** (GitHub #287): in that cutscene the lightning effect could go black for a
  few frames the first time it played, while the graphics driver was still preparing it. On DirectX 12 the game now
  waits a moment longer for such a full-screen effect instead of leaving it out - at worst a short hitch the first
  time, never more than a few a second. (`--gow2_renderer_fullscreen_pipeline_wait_ms=0` in `extra-args.txt` goes back
  to 0.6.1's behaviour.)
- **Import saves** also takes saves from the Japanese Gears 2 disc and puts them where your copy looks for them.
- **First-person camera** (experimental, not tried in play yet), **DualSense adaptive triggers** and **Cheats** (see
  above).

### Gears 3
- **DLC at the main menu** (GitHub #283): with many DLC packages, Gears 3 often opened only the first one at start-up.
  We found why - the game closed something it had finished with a moment late, and COGBox had already reused its number
  for the DLC search, which ended it - and changed it. It should now open all of them, but we haven't had it confirmed
  on a big DLC folder yet: if DLC is still missing for you, please tell us on #283. The old workaround (**Options >
  Storage Location** at the main menu) still works, and the launcher now marks duplicate packages (see Launcher).
- **RAAM's Shadow: dying no longer restarts the whole checkpoint** (GitHub #273, #226): the wait for the level to finish
  streaming in, made for this in 0.4.8, had been left out of every build by mistake. It is in now.
- **"Reload last checkpoint" missing after a death** (GitHub #309, not fixed yet): the game's log now notes the squad
  slots the game checks before it offers Reload. If it happens to you, please press **Report a problem** on Gears 3's
  page right after and attach the zip to #309.
- **Two-player split side by side** (Gameplay > Two-player split): the game put the split back to top and bottom
  whenever a level started; COGBox now sets it again. GitHub #269.
- **No more short hitch every 5 seconds in the menus and multiplayer**, caused by COGBox's check for the path-choice
  mouse fix (GitHub #277).
- **Your options and brightness are kept after a crash:** a game setting saved at the moment of a crash or with a full
  disk could be cut short, and Gears 3 then reset all its options and showed the brightness screen again. Settings are
  now saved in one piece.
- **First-person camera** (experimental) and **DualSense adaptive triggers** (see above).

### Judgment
- **Soft shadows (PCSS) in Judgment too** (by gmihaic): Judgment draws its shadows the same way as Gears 3, so the
  Graphics tab's **Soft shadows** option now works there as well.
- **Cheats** (see Launcher). Checked on our PC: god mode, all weapons, slow motion and the free camera all work.
- **DualSense adaptive triggers** (see above).

### Graphics
- **Indirect lighting (SSIL, experimental, by gmihaic)** (Graphics tab, below Ambient occlusion; **Game's own** by
  default): light bounced from nearby lit surfaces - walls, floors and lamps brighten what is next to them, with some
  colour bleeding. Most visible in lit interiors and city streets, subtle in open or dark areas. It needs **Ambient
  occlusion: XeGTAO (high or ultra)**; with any other Ambient occlusion choice it is greyed out and does nothing. It
  is smoother with DLSS, FSR, XeSS or SMAA T2x. Gears 3's picture has a lower brightness range, so the bounce is
  gentler there. Where the graphics card can't run it on a game's picture (for example some setups under Proton), it
  switches itself off instead of failing. Cost on an RTX 3080 Ti at 2x (1440p): about **+1.6 ms** a frame in Gears 2
  and **+2.1 ms** in Gears 3.
- **Soft shadows (PCSS) fixes** (by gmihaic): lit spots just inside a shadow's edge are no longer too dark, and no
  longer jump when you move. Cost on an RTX 3080 Ti at 2x (1440p):

  | Game | PCSS High | PCSS Ultra |
  | --- | --- | --- |
  | Gears 3 | +0.1 ms | +0.6 ms |
  | Judgment | +0.2 ms | +0.25 ms |
- **Better HDR in Gears 2 and Gears 3** (by legoliamneeson, PR #295): the native HDR now uses the PsychoV35 tone mapping
  (by ShortFuse, with mqhaji's edition) and grading from RenoDX. Explosions, muzzle flashes and lights reach your
  screen's peak brightness more faithfully, while the game's own colours and shadows are kept.
- **HDR highlights** (Display tab, Gears 2 and Gears 3 with HDR on): **Off / Subtle / Default / Strong** - how much
  brighter than normal white explosions, flashes and lights get. Default is legoliamneeson's tuned look.
- **Ultrawide up to 48:9** - three 16:9 screens side by side - in Gears 2, Gears 3 and Judgment. Before, the picture
  stopped at 32:9 (Gears 2) or 4:1 (Gears 3, Judgment) with black bars beyond. Requested by ciegefaye21, GitHub #293.
- **The right graphics card on PCs with two or more** (GitHub #304, reported by praudmur): DirectX 12 and Vulkan now
  always use the same card - the one with the most video memory of its own, and between cards with about as much, the
  one your monitor is plugged into. A PC with an RTX 4090 and an RTX 3090 could run Judgment on the wrong one, and
  DirectX 12 and Vulkan could pick different cards. Checked on our two-card PC in all four games, on both. To choose a
  card yourself, add `--gow2_graphics_card=` and part of its name (for example `--gow2_graphics_card=4090`) to
  `extra-args.txt` next to CogBox.exe; the game's log lists every card it found.

### Online play
- **Your Social tab account keeps its #number** (GitHub #302): the launcher could make a new online account at every
  start, so your #number changed and friends were lost. It now always keeps the one it saved, and if it can't save one
  (for example a folder it can't write to), the Social tab says why.
- **Show ping fixes (by Flippage):** the bar shows nothing when you are not in an online match or party (0.6.1 showed
  "Ping --"); a host that stops answering drops out after a few seconds; when you host, the players in your match
  count before an old join target.
- **Private matches in Gears 2** keep their 0.6.1 behaviour until COGBox's private-match changes are tested there (by
  Flippage).

### Linux, Steam Deck and Mac
- **Mac (CrossOver)** is now a supported way to play, alongside Proton (community-tested; we have no Mac): COGBox knows
  it is on a Mac, always runs the games on DirectX 12 there (Vulkan doesn't start under CrossOver), and no longer calls
  the Mac "Linux". The README has a Mac section, with L33tRonin's fix for a constant bass hum (GitHub #261).
- **Launcher music no longer holds the sound output while it is silent** (music off, or a game running). This may help
  with sound problems under CrossOver (GitHub #261) - tell us if it does.
- **Hosting System Link from Linux / Steam Deck:** Linux keeps the port the games host on for administrator programs,
  so a game hosted there couldn't be joined. The System Link option now says so, problem reports include the setting,
  and the README has a fix to try (not confirmed yet - please tell us on GitHub whether it works).
- **Folders open in your own file manager** (Mods, Saves, Logs, Licences) instead of Wine's, and COGBox also shows the
  Linux or Mac path - useful in the Steam Deck's Game Mode, which has no file manager.
- **Desktop shortcuts under Wine** are replaced by how to start one game directly: Steam's Launch Options
  `--play <game>`, or CrossOver's Run Command saved as a launcher.
- Under Proton the Graphics API shows "DirectX 12 (Proton runs Vulkan)" when Automatic runs Vulkan; the first-start
  settings dialog shows the real frame-rate cap (the Steam Deck was told 60 while getting 45); a CPU's AVX2 is now
  found on older Wine and CrossOver too; "Look for my games" on a Mac searches your drives under /Volumes.
- **Problem reports** made under Proton or CrossOver remove your Linux or Mac account name, as they already did your
  Windows user name.

### Fixes
- **Short key and mouse taps reach the game** (GitHub #305): a tap quicker than the game's own reading of the keys
  could be lost - for example the fast presses in a chainsaw duel. COGBox now holds each press until the game has
  read it. All four games.
- **PCs with fewer than 6 processor threads** (GitHub #288): the game's busy background threads could keep the game
  itself off the few cores and freeze it; they no longer get extra priority there. The log is also no longer filled
  with "Too few processor cores" lines (it was written about 1000 times a second). PCs with 6 or more threads are
  unchanged.
- The update check falls back to the default proxy setting, as the download already did.

### Known issues
- **Gears 3: GPU at 100% in some menus and the Horde character select** on mid-range cards at 2x (GitHub #296). 1x
  render resolution helps; being worked on.
- **Gears 3: "Reload last checkpoint" sometimes missing in RAAM's Shadow** (GitHub #309): see Gears 3 above - a report
  zip from 0.6.2 helps.
- **DualSense adaptive triggers** and the **first-person camera** are experimental and off by default (see above).

## 0.6.1

**Quick start:** download `COGBox-0.6.1.zip`, right-click it > **Extract All...** to a folder of its own (for example
`C:\Games\COGBox`), start `CogBox.exe`, and add your game copy and its title update.

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
- **COGBox says when it's in the wrong place:** started from inside its zip or from a folder it can't write to (such as
  Program Files), it explains on start-up where to move it (it already warned about OneDrive / Dropbox folders).
- **A save dropped onto COGBox is imported:** an Xbox 360 save or profile package (one file) dragged onto the window (or
  onto a game's page) goes through **Import saves** instead of "This package isn't a title update or downloadable content".
- **Title updates are found after moving COGBox** (Discord): a COGBox folder set up on one PC and copied to another
  (a Steam Deck, say) saw the games and DLC but not the title updates, which it looked for at the first PC's path.
- **COGBox says when drag and drop can't work:** run as administrator, Windows doesn't let Explorer drop files onto
  it; it now says so on start-up and points to Choose a file... / Choose a folder....
- **Gears 1 has Insane from the start too** (GitHub #284): the game part has worked since 0.5.0, but the launcher still
  hid the option for Gears 1. It's now offered like in the other three games (Gameplay > Original unlock rules off).
- **The tested versions are shown:** each game's About tab and the title-update notice name the game and title-update
  versions COGBox is made for, and how many other copies players have reported working. The README on GitHub lists them too.
- **Japanese discs:** the Japanese Gears of War 2 and Gears of War: Judgment discs now count as tested (no "untested version" notice), and the Japanese Gears 2 disc is recognised as Gears 2 when you drop it on COGBox.

### Vulkan
- **Vulkan's first-play shader freezes are gone.** Playing a chapter for the very first time (no shaders built yet,
  as for every new player), Vulkan froze again and again while the graphics driver built new shaders: saving the
  shader cache, every ~15 seconds, had to wait for that work and stopped the game until it was done. It now saves in
  the background. Measured on an RTX 3080 Ti, true first run, during play:

  | Game | 0.6.0 | 0.6.1 |
  | --- | --- | --- |
  | Gears 3 | 6 freezes, longest 1.4 s | **0 freezes**, longest frame 0.12 s |
  | Gears 1 | 4 freezes, longest 0.5 s | **0 freezes**, longest frame 0.02 s |
  | Gears 2 | 2 freezes | **0 freezes** |
  | Judgment | none | none |

  The fix is in the renderer all four games share.
- *Still being tested (off by default):* building a chapter's shaders behind its loading screen before play starts, a
  background thread for new shaders, and Vulkan pipeline libraries (the method DXVK uses).

### Judgment
- **Judgment finally looks like it does on the Xbox 360.** The game measures how bright each scene is and sets its exposure from that, and that measurement never reached it, so every scene came out about twice as bright as on the Xbox 360 (washed-out skies, grey shadows). It now does, at no cost in frame rate. Checked against Xbox 360 footage. GitHub #185, #250.
- The launcher's **Brightness** setting for Judgment is gone: it only existed to hide this, and could not bring back the washed-out highlights. A saved Brightness value is ignored.

### Performance
- **Up to 77% faster.** Real 0.6.0 release against 0.6.1, same PC (RTX 3080 Ti), 2560x1440, uncapped, vsync off:

  | Game | DirectX 12 | Vulkan (experimental) |
  | --- | --- | --- |
  | Judgment | 78 to 105 fps (**+34%**) | 47 to 84 fps (**+77%**) |
  | Gears 2 | 148 to 174 fps (**+17%**) | 123 to 169 fps (**+38%**) |
  | Gears 3 | 93 to 101 fps (**+8%**) | 104 to 97 fps (-7%) |
  | Gears 1 | about the same | 123 to 117 fps (-5%) |
- **Vulkan is much faster once a game has loaded** (Windows; still experimental). Vulkan now skips everything the camera can't see ("real occlusion") in Gears 2, Gears 3 and Judgment, as DirectX 12 already did (Gears 1 keeps it off on both). Not yet under Proton / Wine, where real occlusion still has to be tested.
- **Vulkan, for testers:** a shader optimiser and an on-disk store for converted shaders are in this version but switched OFF (`--gow2_renderer_vulkan_spirv_optimise=true` and `--gow2_renderer_vulkan_spirv_store=true` in `extra-args.txt`). They halve the graphics driver's work per new shader pair (364 to 195 ms on an RTX 3080 Ti) and later start-up waits, but as wired today they make the first start much longer, so they stay off until Vulkan shaders are prepared in the background.
- **If a first play still stutters on your PC** (slower processors and graphics cards build shaders more slowly), "Prepare all shaders now" on a game's Graphics tab does all of that work up front.
- **The graphics card no longer waits for the whole frame.** It now starts on each frame while the rest of it is still being prepared, instead of sitting idle until everything is ready. No setting to change. (`--gow2_renderer_submit_draws=0` in `extra-args.txt` turns the early start off if it ever causes a problem.)
- Smoother sound and frame pacing: a stall when the sound output device is checked is gone, and several small renderer costs were removed.

### HDR (by legoliamneeson)
- **Native HDR in Gears 2 and Gears 3:** the picture is carried in high dynamic range from the game's own lighting to the screen, instead of being expanded from the standard picture. Gears 1 and Judgment keep the HDR of 0.6.0. `--gow2_hdr_native=false` goes back to it in every game. GitHub #267.

### Online play (by Flippage)
- **Smoother online and System Link matches in Gears 2, 3 and Judgment:** the games' 2008 Xbox Live network limits (7000 bytes a second per player, 45 updates a second from the host) are raised to 100,000 bytes a second and 60 updates. In testing, the old limit held the game back for about 40% of a round. A full lobby's host may now upload up to about 7 Mbit/s in the busiest moments.
- **Open matches** (an idea from Shagg): the Social tab and the in-game Social menu list the public matches in Gears 2, Gears 3 and Judgment that have real players in them, such as "KOTH · 6/10 · Gears 3 · US · In progress", with a **Join** button. Join in progress puts you in a bot's place. Only matches are listed, never parties, and no names are shown. Gears 1 isn't included (it has no join in progress).
- **Online privacy:** the games' lobby search no longer shows join codes, and a hosted lobby no longer carries your PC's local network address. The search now signs in, so the server can require it later (see PRIVACY.md).
- **Matchmaking finds the mode you picked:** searching one playlist (for example Social Guardian) could put you into another mode's lobby (Horde). Searches now only return lobbies of the playlist you chose.
- **Gears 2 hosting after a quit:** COGBox's 30-minute lift of the game's quit penalty (with its popup) never ran in Gears 2, so a player with a quit could not host versus at all. It works now.
- **Show ping** (Display tab): your ping to the host of the online match or party, in the FPS bar, or in a bar of its own when the FPS counter is off. When you host, it shows the worst ping among your players.

### Fixes
- **DirectX 12 on graphics cards with Shader Model 6.5:** COGBox no longer refuses to start with "Shader Model 6.6 is not available"; it runs with Shader Model 6.5 shaders instead (cards and drivers that have 6.6 are unchanged).
- **Gears 3: the path choice follows its prompts with a mouse again** - the left mouse button (fire) had picked the right-hand path; the swap is on by default again (by KiltedKernel).
- **Gears 3:** a crash with a "disc read error" when a level or a Horde match starts should be gone (the game was told that parts of the disc had failed a check that has no meaning here). GitHub #271.
- Mods added from a `.zip` unpack into the mod's own folder.
- The updater refuses oversized downloads and duplicate files in an update.
- Safer handling when the graphics device is lost while the game waits for it.

### Known issue
- **Gears 3: DLC sometimes missing at the main menu** (GitHub #283). With several DLC packages the game can open only the
  first one at start-up. Workaround: at the main menu open **Options > Storage Location** and pick the storage (the game
  checks the DLC again), or go back to the main menu once after loading in. Keeping only the DLC you use also helps.

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
