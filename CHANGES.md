# COGBox changes

## 0.6.5

**The big one: shaders.** COGBox 0.6.5 rebuilds how the games' shaders are translated, kept and built, and you feel it
from the moment you press Play:

| Gears 3, RTX 3080 Ti PC | 0.6.4 | 0.6.5 |
|---|---|---|
| Shader warm-up, every start after the first (DirectX 12) | 8.5 s | **0.2 s** (97% less) |
| CPU work for that warm-up | 85 s of thread time | **1.4 s** (98% less) |
| Start to the 300th frame | 15.4 s | **5.2 s** (66% sooner) |
| Shader warm-up, Vulkan, a later start | 9.4 s | **1.9 s** |
| Shader translation on a first-ever start | 15.8 s | **7.0 s** (56% faster) |
| Stutters over 50 ms on a first-ever start | | **67% fewer** |
| Worst frame times (p99), Vulkan, new graphics driver | 47.8 ms | **26.7 ms** (44% lower) |

Converted shaders are now kept between starts, the translator does about 88% less CPU work, saving pipelines no longer
pauses the game, and after a driver update the area you are playing is built first. Details below.

- **Much faster start-up** - compared with 0.6.4's shader translator (Gears 3, RTX 3080 Ti PC, same shaders):
  - **DirectX 12 (the default), every start after the first: shader warm-up 97% less time** (8.5 s -> 0.2 s) and **98%
    less CPU work** (85 s of thread time -> 1.4 s); from start to the 300th frame **66% sooner** (15.4 s -> 5.2 s),
    and the worst frame times (p99) **23% lower** in the first minute. Converted shaders are now kept between starts
    (`dxbc-store.bin` in the shader cache; a new COGBox build starts it again), and the saved pipelines load in one read.
    The first start of a new build already warms up **85% faster** (8.5 s -> 1.3 s) while it fills that store.
  - **Vulkan: converted shaders are kept between starts too** - shader warm-up 1.9 s on a later start, against 9.4 s for
    0.6.4's translator (measured on a first start; 0.6.4 converted every shader again at every start).
  - **The shader translator itself does about 88% less CPU work** when it runs (DirectX 12, a separate measurement: 118 s across its threads ->
    14 s), with **95% fewer** memory page faults and **98% less** time spent in Windows itself - and exactly the same
    shaders out (checked on all 9,296 Gears 3 shaders).
  - **A first-ever start** (empty shader cache, DirectX 12): shaders translated **56% faster** while you play
    (15.8 s -> 7.0 s), **12% more frames** in the first minute, **67% fewer** stutters over 50 ms, and the worst frame
    times (p99) **16% lower**. Most of a first start is now the graphics driver building its own shaders.
  - Why: our shader translator re-copied each shader it was writing for every instruction, and on Windows each copy was
    a new memory request - with 14 threads at once the PC spent most of the warm-up waiting on memory. Vulkan also hands
    each converted shader to the driver as soon as it is ready, keeps converted shaders between starts by default, and
    the async shader mode no longer competes with the start-up work.
  - **No more short pause while pipelines are saved** (DirectX 12): every ~15 s of new pipelines, saving them held up
    the pipelines being built for about a third of a second (Gears 3's ~480 MB file); a pipeline built during a save
    is now stored right after it instead of waiting.
  - **Smoother first start after a new graphics driver or card** (DirectX 12; Gears 3 with every cache emptied, NVIDIA's
    included): the shaders of the area you are playing are now built first instead of after every shader saved from earlier
    starts - **33% fewer** draws left out while waiting (17,286 -> 11,657) and worst frame times (p99) a little lower. The
    background building also runs at a lower priority with half its threads during play. And when the shaders weren't
    prepared for this driver, card and renderer, **Play now offers "Prepare all shaders" first** (then the game starts by
    itself; Play now / Don't ask again).
  - **Vulkan: no more long freezes on a first start** (same test): a draw whose pipeline wasn't built yet could build it
    right there and hold the frame up to three quarters of a second; it now waits a few milliseconds, as on DirectX 12,
    and is left out of that frame if the pipeline isn't ready. Worst frame times (p99) **44% lower** (47.8 -> 26.7 ms).
  - **"Prepare all shaders" runs offline** (#370 by Flippage): the run that prepares the shaders started like a normal
    game and signed in to the COGBox server as you - a second copy of you online for minutes. It now starts with no
    online server and without your online identity.
- **Gears 1: 16:10** (Steam Deck, 16:10 laptops): the Aspect ratio setting now covers Gears 1, and the picture fills the
  screen in gameplay instead of keeping 16:9 bars. All four games: Aspect ratio 16:10, and Auto picks it on a 16:10 screen.
- **Stick dead zones** (Controls tab): separate Left stick and Right stick dead zone sliders (0-50 %), as in popular
  emulators - a round dead zone in every direction, with the stick still reaching full at its edge. The old dead zone
  only worked to the right and up, and was about twice as big as it said.
- **Cheats reworked** (GitHub #342, #359):
  - **Chosen in the launcher** (Gameplay tab), on for the whole game: God mode, Game speed (slow motion), All weapons
    and ammo, Hide the HUD. Campaign and wave modes only, never in Versus.
  - **Cheat keys** (Cheat keys option): F9 free camera (Gears 1: fly through walls), F10 freeze the world, F11 hide / show
    the HUD. God mode, slow motion and all weapons are no longer keys.
  - **A small popup** shows what a cheat key did (God mode ON / OFF, Freeze the world ON, ...) or why it can't run.
  - **Gears 2:** cheats now also work when the campaign is started from the menus ("not recognised" in 0.6.3).
  - **Gears 3 with Title Update 6:** God mode, Game speed and Hide the HUD work; All weapons and ammo and the cheat keys
    don't yet.
- **Gears 3 Horde bots get their own characters** (Discord): bots added with the Horde-bots button were all Marcus,
  whatever their name. The game now loads each bot's character, and it comes back as that character from its next
  respawn (its first life is still Marcus).
- **Open matches: DLC light** - a dot next to each match: green when you have the host's map (base game or your
  installed DLC), red when you don't, grey while the host hasn't picked one. Gears 3 and Judgment; needs the updated
  online server.
- **Linux / Steam Deck players can be joined online again** (Discord): Linux doesn't let a game use port 1000, so on
  Proton the game was moved to another port - and other players' games, still sending to 1000, found the lobby but
  couldn't reach it, so the join failed. The game now keeps its usual port as far as the game and other players can
  tell, while using the port Linux gave it underneath. Tested on Windows with the port refused on purpose; joins through
  the online server were not tested on our PC.
- **A versus search no longer sends you into someone's campaign** (Discord): searching for a versus match could join
  another player's campaign lobby (seen with RAAM's Shadow in Gears 3). Searches for a versus playlist now skip lobbies
  that don't name a playlist (tested in Gears 3; Gears 2 and Judgment versus lobbies not checked yet).
- **Judgment: no more one-frame light flashes with XeGTAO, steady blood decals with DLSS, and no freeze back to the main
  menu** (by Flippage):
  - With XeGTAO on, large areas (and the player model) lit up for a single frame as the camera moved. On some frames
    Judgment makes an extra copy of the scene before its fog pass (what is on screen decides), and that copy was taken
    as the frame's scene, so the AO that goes in at the fog pass was left out of that frame - and DLSS / FSR / XeSS got
    the scene without its last passes. The scene copy is now one made after the fog pass
    (`--gow2_upscale_scene_after_fog`, on for Judgment); frames without the fog pass (a pause or menu over the world)
    go back to the old choice after two frames.
  - Blood decals and lighting over floors flickered off with DLSS on: Judgment now raises the decal depth offset as
    Gears 1 does (`--gow2_renderer_decal_min_bias=0.01`) and nudges such overlay draws toward passing
    (`--gow2_renderer_overlay_depth_nudge=2`, new for Judgment).
  - Back to the main menu, the game froze for 2 seconds: the GPU waited for a marker it had just written, and an
    occlusion result landing late had replaced it. In Judgment, a wait for that occlusion marker now passes once the
    GPU's own write of it has landed, as it would on the console; the 2-second limit stays as the last resort. The other
    games are unchanged.
- **The FABRICATOR is a local mod manager** (KiltedKernel): the launcher's FABRICATOR screen manages the mods of all four
  games on your PC, offline, with nothing sent anywhere (it is also reached from **More in FABRICATOR** on a game's Mods
  tab).
  - **LIBRARY:** add a mod from a `.zip` or a folder (or drop one on the window); it is checked, added, turned on and
    put last in the load order. Adding a mod again with the same mod.ini Name offers an update. Turn mods on or off,
    move them in the order, open the mods folder, and uninstall (the folder is moved to `mods\.fabricator-backup`, not
    deleted). A mod folder you put there by hand (one without COGBox's `fabricator.json`) is never moved or deleted. While the game runs, an added mod waits
    and is installed once the game is closed; a row's Update and Uninstall, on / off and the order refuse with "Close
    the game first.". Program files in a mod (programs, libraries, scripts, installers, shortcuts, archives inside the
    zip) are left out, and the install names them.
  - **STORE:** a coming-soon note: mods from Nexus Mods are planned; there is no online mod library yet.
  - **CREATOR:** check a mod folder for what a clean package needs (a mod.ini with Name and Game, no program files,
    safe file names, the limits: 50 000 files, 8 GiB unpacked, mod.ini 256 KB; the 4 GiB zip limit is checked when the
    zip is made), edit its mod.ini in a form (Name, Game, Version, Author, Description, Side, Requires; other
    sections and comments are kept), make a clean zip, and see the steps to publish it on Nexus Mods with the COGBox
    tag.
  - LEGAL.md has a new section 11, "Mods and the Fabricator" (the takedown section is now 12); PRIVACY.md lists what the
    Fabricator keeps on your PC; `docs/modding.md` has "Packaging and publishing your mod".
- **A bigger launcher** (Shagg): the window is 1280 x 800 at 100% Windows scaling (it was 1000 x 624), with the same
  text and button sizes; a screen too small for it gets a smaller window first, and smaller text only below the old size.
  The settings panel reaches to about 40 px from the window's sides, and every tab uses the room for two columns:
  Display, Graphics, Controls and Gameplay put their sections side by side, About splits in two, and Achievements has
  its popup settings at the top and your achievements two to a row below. One column again when the window is narrow.
- **Social: Invites, Matches and Recent players** (launcher and the in-game F8 page): friends on the left, each on two
  lines (tag over status), and on the right one of INVITES, MATCHES or RECENT (click, or LT / RT on a controller).
  **Recent players** lists the COGBox players you were in an online match with over the last 14 days, newest first, with
  the game and when, and an ADD FRIEND (or ACCEPT) button. The game reports the players it shows tags for, each at most
  once every 10 minutes; the server keeps one row per pair (new table recent_players, POST / GET
  /api/v1/players/recent; needs the updated online server). The Gears 3 team-picking note under Open matches is gone.
- **COGBox tags on every player's name** (all four games; GitHub #353): in online lobbies, party lists and the
  scoreboard each player's name shows their COGBox tag after it in grey, for example Flapper#5531, so you can add
  someone you just played with as a friend by their tag. The game asks the COGBox server once for each new player (up to
  32 at a time) and keeps the answer; it never asks again for a player it already knows. Off with
  --cogbox_name_tags=false. The kill feed shows the tag too, in the name's own colour (it draws plain text: COGBox takes
  the colour markup out where the HUD draws or measures its text; in the title-update builds of Gears 2 and Gears 3).
- **HDR for Judgment and Gears 1** (PR #351 by legoliamneeson - thank you), more SDR-fallback fixes, and improved
  sampling of Gears 3's colour grading in HDR (Improved HDR LUT sampling, on by default; turn it off for the previous
  look).
- **Each game has its own icon in the taskbar and Alt+Tab** (GitHub #355): the game's own title picture, read from your
  game files as it starts (nothing extra is shipped).
- **Scene resolution note** (GitHub #345): when an upscaler can't shrink the scene at your render resolution (e.g.
  DLSS Performance at 4K), a note under the setting now says it will only anti-alias (no extra frames) and what to pick
  instead.

### Fixes
- **Judgment: English voices with another language's text** (GitHub #367): the Voice language setting did nothing in
  Judgment - every localised package kept its own language, because COGBox could not read the newer package format
  Judgment uses (version 846; Gears 2 and 3 are older). It now reads it, so a package whose English twin has all its
  objects gives the English voices, as in Gears 2 and 3. Checked on Judgment's English packages; not yet played with
  another language's text.
- Kernel fixes from the Xenia Canary / Edge comparison (timers, semaphores, a read/write lock, XMA): none of our four games
  was seen to use the changed paths, so no change is expected in play.

### Translations
- **Hungarian** fixes and newly translated lines (PR #356 by hevesij - thank you).

## 0.6.4

- **Built for AVX2 CPUs only** (all four games): the game code now uses the AVX2 / FMA / BMI2 instructions of Intel Core
  (Haswell, 2013) and AMD Ryzen / Excavator (2015) CPUs and newer, instead of the 2003-era baseline every PC has. Why:
  - **It is faster in every game.** The recompiled Xbox 360 code is full of 128-bit vector maths, bit masks and
    rotates, and these instructions do that work in fewer steps. Measured on one PC with an early 0.6.4 build that
    differed from 0.6.3 mainly by AVX2 (before the memory change below), frame time was: Gears 3 6.1% less
    (159 -> 170 fps), Judgment 4.7% less (175 -> 184 fps), Gears 1 3.4% less (123 -> 128 fps), Gears 2 1.4% less
    (297 -> 301 fps); every AVX2 run beat every 0.6.3 run.
  - **One build to test and support:** a second, slower build for older CPUs would double every build and test run.
    The README already listed an AVX2 CPU (roughly 2013 or newer) as needed.
  - **The results don't change:** a multiply and an add stay separate, as in the Xbox 360 code (no fused multiply-add
    that would round differently), so the games behave as before.
  - **Who is affected:** CPUs older than Intel Haswell (2013) or AMD Excavator (2015), and some budget Pentium /
    Celeron chips of later years that leave AVX2 out. On such a CPU COGBox says so in plain words instead of crashing;
    0.6.3 still works there.
- **Faster game memory access** (all four games): addresses in the top part of the Xbox 360's memory no longer need an
  extra step on every access - the PC maps that memory the way the console does (rexglue's change, brought over), and
  the game code writes 16-byte vector values in one or two steps instead of byte by byte. About 1.4% less frame time in
  Gears 3, on top of AVX2. Needs Windows 10 version 1803 or newer.
- **Less stutter with texture packs** (all four games; GitHub #341): a pack texture is read from disk once and shared
  wherever the game uses it, and kept in video memory when the game moves or drops it, instead of being read again each
  time. New **Texture cache** setting (Graphics tab): Auto (512 MB, or 2 GB with a texture pack on cards with 6 GB or
  more), 512 MB, 1 GB, 2 GB or 4 GB.
- **Gears 3 Open matches: the host's game picks your team** (Discord report: every joiner landed on the host's team).
  JOIN in the Social tab's Open matches now has Gears 3 find the match with its own matchmaking: go to Versus > Social
  and the match's playlist (the launcher says which), and the host's game seats you as on Xbox Live. A match already in
  progress still joins the old way for now. Friends' joins and invites still put you with your friend. To pick COG or
  Locust yourself, host Versus > Private and have players join by room code or Join friend, then press Y in the lobby
  to switch team. Needs the updated online server; until then JOIN works as before.
- **Saves** (Gameplay tab): a SAVES section with Import saves, Back up saves (both moved from the About tab) and Open
  saves folder. Saves copied in by hand - from another profile, an older Xenia, or console save packages dropped in -
  are found; COGBox offers to bring them in (moving the originals aside first, never deleting them) and asks only once
  per set.
- **Default keys follow each game's own controls** (GitHub #350): in Gears 1, 2 and 3, 4 = grenades and 3 = pistol (they
  were swapped). Judgment gets its own layout: 4 = grenade, 3 = switch weapon, 1 / 2 = the two weapons, Q = point of
  interest, Tab = tactical display. Keys you set yourself are kept (a saved Judgment Q / 3 pair moves to the new layout
  once), and a new default never lands on a key you already use.
- **Shadow detail** (Graphics tab; Gears 2, 3 and Judgment; off by default): Balanced or Performance draws fewer small
  and distant shadows for a higher frame rate in busy fights, through the game's own shadow-size settings - close and
  large shadows, characters and cutscenes keep theirs. Inspired by boma's "Smart Shadow Cuts" patch, done a different
  way. Not tested on PC yet.
- **Game sound decoder brought up to Xenia Edge's** (all four games): looping sounds, one-shot sounds, seeks, stereo
  streams and sounds split across buffers decode the way the Xbox 360 does them (Edge's 24 XMA fixes and the matching
  FFmpeg ones). Not tested on PC yet.
- **Sharper textures with DLSS and FSR** (all four games): textures now use the detail bias NVIDIA's and AMD's guides
  ask for, one step sharper (XeSS keeps Intel's). The launcher also marks Scene resolution choices that can't upscale at
  your resolution as "anti-aliasing only at this render resolution" (for example Performance at 4K runs as DLAA). DLSS
  is marked experimental again.

### Fixes
- **Gears 3: DLSS really works now** (GitHub #345): with DLSS Quality or Ultra Quality, Gears 3 never upscaled - the
  scene copy it uses went through a different-size target, so the upscaler gave up every frame (no DLSS, FSR, XeSS or
  frame generation). Tested: every frame upscaled at 4K.
- **Judgment: no more stutter when moving between areas of a map** (Blood Drive most of all, by Flippage, PR #352 -
  thank you): with an upscaler on (seen with DLSS), the scene copy it used switched every other frame between a
  mid-frame HDR copy and the final scene, so the upscaler was rebuilt each time (about 33 ms, 50+ times in one Blood
  Drive match). It now keeps to the scene copy's format and is built once (and learns it again if the scene's format
  really changes). The first copy of each new resolve destination also no longer waits for the GPU (patch 0020
  read-back) unless it is small (one 32 x 32 tile, such as a value the game reads back, at most two a frame); it lands
  a frame or two later like every other copy (`--gow2_renderer_resolve_readback_first_wait=true` brings the old wait
  back for all).
- **Gears 1: crash 2 seconds in** (GitHub #346): the title update had been made for another print of the game. COGBox
  now checks that the title update matches your game files before starting (as the console does) and asks first,
  instead of the game crashing.
- **Gears 2: Horde bots no longer greyed out** (Discord report): the launcher didn't recognise that 0.6.3's Gears 2 had
  the option.
- **Steadier speed with the game window covered or minimised on Windows 11** (all four games): Windows 11 could drop
  the fine timer the games ask for back to its 15.6 ms tick while the window was hidden; COGBox now opts out of that
  (as Xenia Canary does).
- **Exact results for a few rare CPU instructions** (all four games): converting a very large number to a whole number,
  some signed shifts and one vector compare now give exactly what the Xbox 360 gives (rexglue's fixes, brought over).
- **French achievement wording now matches the French Xbox 360** (by IamBugReporter, GitHub #339 - thank you):
  "déverrouillé" instead of "débloqué", "Succès caché" for secret achievements and "Gamerscore" instead of "Score".
- **Mods tab translated:** its lines (left-out content, texture pack counts, chapter names, co-op notes) now show in
  all 11 languages.
- **Translations:** all new text is in all 11 languages (machine-made; corrections welcome).

## 0.6.3

- **Flickering ground patches fixed** (Real occlusion, by Flippage): some flat ground pieces - in Gears 3 on Mercy's
  countdown screen, in matches and in the campaign - switched between textured and grey every frame. Their occlusion
  box, lying almost exactly on the patch, lost against the patch's own surface in the game's half-size depth (which
  keeps the nearest of each 2x2 pixels), so the game hid the patch, then showed it again the next frame. Occlusion
  boxes now get a small slope-scaled depth bias toward "visible". Occlusion still culls as much as before; only these
  false hides are gone.
- **XeGTAO ambient occlusion: no more ghosting through floors and walls, and steadier shadows near the camera**
  (Gears 3 and Judgment, by Flippage): Gears 3 and Judgment copy their scene depth out once or twice a frame, and with
  two copies the first holds only about half the scene. XeGTAO used the first copy, so geometry drawn after it - the
  ground, most visibly - was missing from its depth, and hidden objects (stairs, buildings, players behind a wall)
  showed through as faint shading. XeGTAO now reads every scene-depth copy and runs its passes on the last one, once a
  frame. Its samples close to the camera also no longer leave the screen or read coarse depth levels (new settings
  `--gow2_ao_max_screen_radius`, default 0.325, and `--gow2_ao_mip_offset`, default 6.5), which removes the shadow
  flicker at the bottom of the screen, on your character and while aiming.
- **Windowed / borderless full screen key, all four games:** **Alt+Enter** in the game window switches between a window
  and borderless full screen on the monitor the game is on, and back to the window's size and place. It is not F11
  (F11 is the cheat keys' "all weapons"); change it with a `--bind_cogbox_fullscreen=<key>` line in `extra-args.txt`
  (`None` turns it off). The key is shown on the Controls tab. Suggested in GitHub #316.
- **Custom controller layout in the launcher:** Controls tab > Controller layout > **Customise...** (or **Custom**) lists the game's buttons; click one and press the controller button you want for it (without a controller, pick it from a list), with **Swap sticks** and **Reset to the game's own**. A button already in use swaps with the old one. One layout for all four games, still saved as `[Pad] Remap=` in `CogBox.ini` - no more editing it by hand (a hand-written line is read and kept).
- **Cheats are off in Versus** (all four games): the cheat keys (F5-F11, `--gowX_cheats`) no longer work in any Versus match - online, System Link, private matches and local split screen, with or without bots. They still work in the campaign (solo and co-op) and in Horde, Beast and Survival. Any game type not on that list is treated as Versus. Other console commands from `--ue3_hotkeys` / `--ue3_exec` are refused in Versus too (the launcher's own display settings still apply). Cheats are also off in the menus and lobbies. (The Horde-bots button no longer touches the cheat flag - see the next entry.)
- **Horde bots no longer break online play or hide the scoreboard** (Gears 2 and 3): with the Horde bots option on, Back switched the game to offline mode, turned the cheat flag on and added two bots in every mode, Versus too - when the host of an online or System Link game pressed it, matchmaking and the game's session broke - and Back no longer opened the scoreboard. Now Back shows the scoreboard as before and, in a Horde game you host, one press fills your team to five with bots (spawned at once if a wave is under way). It does nothing in Versus, in the lobbies or in someone else's game. For bots in Versus use the lobby's own bot setting. Tested on PC in Gears 3 (games hosted as network games): in Horde, Back opens and closes the scoreboard and fills the team to five, and the game stays online; in Versus, Back shows only the scoreboard. Gears 2 is not tested on PC yet.
- **Trigger rumble for Xbox One / Series controllers** (Controls tab, all four games, **experimental**, off by default):
  the small motors in the triggers give a short kick with each shot - harder for the Gnasher, Boomshot and heavy
  weapons - and buzz while the chainsaw revs or a Mulcher, Troika or Scorcher fires. Nothing in menus, cutscenes, while
  paused or dead, with the game window in the background, or while vibration is off (COGBox's Controller vibration or
  the game's own option). For USB or the Xbox Wireless Adapter (Bluetooth may give none) with Steam Input off for the
  controller. Only Xbox One / Series controllers get trigger pulses, but while the option is on every Xbox-style
  controller (Xbox 360 and pads in XInput mode too) is read through Windows raw input: until its first button press
  the triggers, the Guide button and rumble may not work right, split-screen players may get their controllers in
  another order, and in rare cases Windows stops sending controller input until the PC restarts - turn the option off
  if a controller misbehaves. PlayStation controllers are not affected; DualSense adaptive triggers work as before.
  **Not tried with an Xbox controller yet.** GitHub #245.
- **Smaller download:** COGBox no longer ships a second, slower copy of each game (`gow2-normal.exe` and
  so on) that it switched to by itself after a game crashed twice in a row. In the crash reports since 0.5.0 that
  happened twice, and the slower copy crashed the same way both times, so it did not help. Each game now has just the
  one, faster program. When you update, the old copies are moved to the `_old-files` folder like other old program files
  (safe to delete).
- **Social: take back a friend request** (launcher's SOCIAL tab and the in-game Social page): a request you sent
  ("Waiting for them to accept") now has a **CANCEL** button, so a request to an account nobody uses any more no
  longer sits there for ever. Suggested in GitHub #332.
- **Texture packs** (by KiltedKernel, GitHub #319 / PR #325 - thank you), all four games: a mod can now carry
  replacement textures, usually bigger and sharper ones, as DDS files named after the hash of the game texture each
  replaces (`<hash>_....dds`, in any folder of the mod). Install a pack like any mod: a folder in `<Game>\mods\` or a
  `.zip` added in the Mods tab, which shows how many textures each mod's pack has. Where two packs replace the same
  texture, the mod lower in the list wins. Packs made for the Gears of War 2 PC port work unchanged (put the pack's
  folder, with its `4D53082D\replace` inside, in `Gears2\mods\`). To make a pack, add
  `--gow2_texture_pack_dump_dir=<folder>` to `extra-args.txt`: every texture the game draws is written there once,
  already named for a pack. Replacements may be the original size or 2x-8x, in BC1-5, BC7 or plain RGBA, with their mip
  levels and at most 64 MB per level; the game's log names any file it cannot use and why. Known limit: the few
  textures the game reads by texel position (look-up tables, some fonts and UI) show the wrong part when replaced
  with a bigger one - keep those at 1x. See docs/modding.md.

### Fixes
- **Show ping after a matchmaking join** (by Flippage, PR #328 - thank you): joining a match through playlist matchmaking showed
  "Ping -- (hosting)" instead of your ping to the host. It now shows the host's ping, as joins from an invite, Join
  friend or the open matches list already did. SDK patch 107.
- **Gears 3 no longer throws away its campaign progress and options when your online account changes** (GitHub #329):
  Gears 3 ties its progress and options to the online account they were saved with. After a new online account (a new
  #number, a deleted account, online play turned off without one) or with saves from another PC or an old backup, COGBox
  left them as they were and Gears 3 started over with the brightness screen, writing fresh files over the old ones
  (checkpoints were kept). COGBox now remembers the account your saves were last used with and moves them to the new
  one with a check. Saves from an account it can't check (another PC, an old backup) are moved too, as long as they
  are complete and look like real Gears 3 saves; a copy of each file COGBox changes is kept as it was
  (`.cogbox-adopted-<date>`). Before any start whose account differs from the last one, COGBox also copies the Gears 3
  and Judgment saves to `user\_cogbox-save-backups` (a copy is only removed when a newer one holds the same saves, and
  folders you put there yourself are never touched). Saves that Gears 3 already reset can be put back and will now load.
  Close the game first. `PlayerStorage.dat` can come from a backup or from its `.pre-online` copy. The three profile
  files `63E83FFF`, `63E83FFE` and `63E83FFD` must go back together, from one backup that holds all three (your own, or
  a folder in `user\_cogbox-save-backups`); a `.pre-online` copy of `63E83FFF` alone won't load. Judgment's saves can't
  be moved to a new account the same way: with a new online account Judgment's online progress still starts fresh, but
  its old files are in that backup folder (and its offline progress comes back whenever online play is off). Tested on
  PC: an account change, saves from an unknown account and a restore were each handled without a reset.
- **Crash reports are no longer empty** (all four games, GitHub #286): a game that froze before crashing could leave an
  empty `crash-*.txt` and a cut-off `.dmp`. The crash report is now written without touching anything the frozen game
  might be holding, lists where every thread was stuck, and the crash dump is written by COGBox from outside the game
  (the game writes it itself when started without COGBox).
- **Hosting no longer halves your frame-rate limit** (Gears 2, Gears 3 and Judgment): while friends were in a match you
  hosted, a limit of 60 gave 30 (144 gave 72) until they left. The game asks for the limit a second time each frame
  when it sends updates to other players, and COGBox waited a whole frame both times; it now waits once per frame.
  Unlimited was not affected, and neither was Gears 1. (Tested on PC with two copies of the game: a 60 limit stayed at 60.)
- **Frame-rate limit in split screen (GitHub #313, Gears 2 and 3): not fixed yet, more detail in the log.** In both
  games' code the limit is applied once per frame however many players are in, so the cause is not known yet. While
  two or more players are in, the log now shows every 10 seconds how many frames the game makes, how many reach the
  screen and how often the limit had to hold a frame back - a report zip from a split-screen session tells us which
  part goes wrong.
- **Less stutter while COGBox prepares shaders, all four games:** at every start COGBox read all of the game's files again (about 4 GB for Gears 3) to find the shaders it prepares in the background, beside the game. Now it remembers each file it has read to the end (in the game's shader cache folder: `prebuild-done.bin`, a few MB) and only reads new or changed files at the next start - on every PC, after the first full start nothing is read again (a COGBox update starts the list once more). On PCs where the game may use 8 logical processors or fewer (for example 4 cores / 8 threads, or a game limited to 8 with an affinity setting), that reading also uses one thread instead of two and slows right down while you play (full speed again on loading screens, menus and videos), and the background shader building (Direct3D 12) keeps half of its threads for what the game needs right now during play, at a lower priority. Faster PCs keep their old thread counts and speeds. The first start still has the reading to do, and an area you reach before its shaders were prepared can still hitch briefly the first time. Not tested on PC yet.
- **Gears 3: Stats & Awards pages drawn correctly with Ultrawide / Match my monitor** (GitHub #330): with a picture
  wider than 16:9 (Ultrawide ticked, or Render resolution "Match my monitor" / "1080p" on a wide monitor), the
  Achievements, Ribbons and Medals pages showed stacked teal bands instead of their panel. The game's screen-sized
  work buffers kept the 16:9 height (as in the Canary ultrawide patch COGBox's follows); these pages copy the screen
  into them and draw it back, so each copy came back squeezed. The buffers now match the picture, as they do at 16:9.
  The block of memory those buffers share with the shadow map keeps its 16:9 size, so the shadow map still fits (a
  first test build crashed shortly after start without this). The picture's height is now a multiple of 8 rows, so
  its shape can differ from your monitor's by a few pixels (a thin border). If the game crashes or draws wrongly
  with Ultrawide since this change, `--gow3_aspect_targets_720=true` in `extra-args.txt` brings back the old buffers -
  please tell us. Tested on PC (Gears 3, D3D12 and Vulkan) at 21:9, 3440x1440, 8:3, 32:9 and 48:9: the pages draw
  correctly, and the campaign, Horde and the lobby run without crashing.
- **Gears 3: Horde bots are no longer always Dom and Carmine.** The Horde-bots button (Back) asked the game for those two characters by number every time; it now lets the game pick each bot's character at random, the way the game picks bots for its own matches. (Gears 2's bots were already random; its two unused numbers are dropped.) GitHub #270; tested on PC in Gears 3 (bots such as Marcus, Anya, Classic Marcus and Hoffman).
- **Launcher: the library music plays again** (silent in 0.6.2: that launcher was built against the wrong FFmpeg version and could not find the music decoder shipped beside it). The build and the packager now check that the launcher and the decoder DLLs match, so it cannot ship silent again.
- **Judgment: the loading screen after Monroe Commons no longer hangs above 30 fps** (GitHub #318, also #54 / #38 /
  #77 - not confirmed yet): COGBox now asks the game itself whether its loading screen is still up, and keeps the
  frame rate at 30 for exactly that long (before, it guessed from the files being read and could let go after
  5 seconds). Your own frame-rate limit comes back as soon as the loading screen ends.
- **The mouse cursor now hides when you play with a controller** (all four games, GitHub #320): the GitHub #143 fix most likely never hid it. While the cursor is over the game window, it now hides as soon as you use the controller (otherwise after 3 seconds without mouse movement), and COGBox checks that Windows really hid it and tries again if not. Tiny mouse movements of 1-2 pixels no longer bring it back - a real movement or a click does. It is left alone while the in-game menu is open or mouse look has the mouse. (Not yet confirmed by the reporter.)
- **No more stutter with DLSS, FSR or XeSS when Gears 2 changes how wide it draws** (GitHub #323): in heavy views (for
  example in Nexus right after the "Reinforced" cutscene) the game narrows its picture by itself, many times a second,
  and COGBox set the upscaler up again from scratch each time (about 20 ms, so 30-40 ms frames). It is now set up once
  for the full width and keeps its history when the width changes (DLSS, XeSS and FSR, also with Vulkan). If one
  view made it larger (in an ultrawide session, say), it is set up for the smaller size again after about 600 frames
  that fit it. The log's upscaler line counts the size changes and set-ups. All four games use the same code. Not
  tested on PC yet.
- **Judgment: blue / purple blocks on surfaces (#315)**, mostly seen on AMD graphics cards at high frame rates and building up over a session: a texture the game had just moved in video memory could be loaded from the old copy and keep it. It now waits for the moved copy first, also when the game moves the same texture again. With DirectX 12, two writes the GPU makes to game memory one after the other can no longer overlap (AMD cards could run them out of order). (Not yet confirmed by the reporter.)
  Also (Judgment and Gears 3): compressed textures the game streams into memory the graphics card wrote last now take
  the game's own bytes (after any pending move has landed) - what the test line `--gow2_renderer_bc_cpu_over_gpu=true`
  did in 0.6.2, which made the reporter's campaign run clean. Which writes it catches there is not known yet (the
  0.6.2 logs ruled out one candidate, read-only memory; `--gow2_renderer_bc_gpu_page_log=200` now names the others). It
  is now the default there; you can remove that line from `extra-args.txt` (`--gow2_renderer_bc_cpu_over_gpu=false` or
  `--no-gow2_renderer_bc_cpu_over_gpu` gives the 0.6.2 behaviour; an unknown value is logged and treated as the
  default). Gears 1 and 2 are not changed.
- **Launcher: no more crash now and then when you close COGBox** (GitHub #331, `d3d11.dll_unloaded` in Windows'
  error log - not confirmed yet): the graphics library the launcher draws with let go of Direct3D 11 while something
  else (most likely an overlay or capture tool such as Steam, Discord, RivaTuner, Xbox Game Bar or OBS) could still be
  using it. COGBox now keeps it loaded until it has fully closed. Nothing changes while COGBox is open.
- **Translations:** all new text is in all 11 languages (machine-made; corrections welcome). Hungarian updated and
  corrected by Hevesi J. (PR #324 - thank you!). In several languages the Brightness and mouse-speed texts showed in
  English; they are translated again.
- **Thank you** to everyone who reported issues on GitHub and tested in the Discord - this release is built on your reports.

## 0.6.2

**Quick start:** download `COGBox-0.6.2.7z`, right-click it > **Extract All...** (Windows 11; on Windows 10 use 7-Zip) to a folder of its own (for example
`C:\Games\COGBox`), start `CogBox.exe`, and add your game copy and its title update.

<a href="https://recomplauncher.com/game/cogbox"><img src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" alt="Get it on RecompLauncher" height="56"></a>

COGBox is on RecompLauncher, the free home for recompiled PC games. Get it straight from its developer:
https://recomplauncher.com/game/cogbox

### New and experimental (all off by default)
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
- **DualSense adaptive triggers** and **Cheats** (see above).

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
  It no longer gives up for the rest of a level when the game puts the split back right after COGBox set it: it tries
  again, waiting longer each time (up to 30 seconds). GitHub #313.
- **Frame limit in split screen** (GitHub #313, not reproduced yet): Gears 3's log now says how many game ticks did
  not reach COGBox's frame limiter and how many players are in, so a report from split screen shows why the limit did
  not hold.
- **No more short hitch every 5 seconds in the menus and multiplayer**, caused by COGBox's check for the path-choice
  mouse fix (GitHub #277).
- **Your options and brightness are kept after a crash:** a game setting saved at the moment of a crash or with a full
  disk could be cut short, and Gears 3 then reset all its options and showed the brightness screen again. Settings are
  now saved in one piece.
- **DualSense adaptive triggers** (see above).

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
  card yourself, use the new **Graphics card** list on a game's **Graphics** tab: **Automatic** (the default) or one of
  your PC's graphics cards, shown with its name and video memory. The choice is kept by the card's name for all four
  games; if that card is not in the PC any more, the list shows it as not found and Automatic is used. (Still possible
  by hand: `--gow2_graphics_card=` and part of the card's name in `extra-args.txt`; the game's log lists every card it
  found.)

### Online play
- **Your Social tab account keeps its #number** (GitHub #302): the launcher could make a new online account at every
  start, so your #number changed and friends were lost. It now always keeps the one it saved, and if it can't save one
  (for example a folder it can't write to), the Social tab says why.
  OneDrive or a virus scanner holding the file for a moment no longer counts as "no account": the file is saved as a whole
  new file moved into place with a few short retries, read with retries too, and when it is busy the launcher waits and
  tries again instead of making a new account.
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
- **White or flickering surfaces on DirectX 12** (GitHub #311, #282, #255, #297, Judgment #315 / #243 - not confirmed yet):
  a texture whose loading was cut short on DirectX 12 could be shown with whatever an earlier texture had left in that
  memory - white floors, bright blue blocks. It is now loaded again, and new textures start out clean, as on Vulkan.
  We couldn't reproduce these here (they were reported on AMD cards), so please tell us whether they are gone. All four
  games. (`--gow2_renderer_zero_new_textures=false` in `extra-args.txt` turns the clean start off.)
- **No more crash at the start on a PC without speakers or headphones** (GitHub #312): with no sound output device, the
  sound start-up failed and the game crashed a few seconds after the Epic logo. The game now runs silently instead, and
  picks up sound by itself when a device is plugged in. All four games.
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
- **DualSense adaptive triggers** are experimental and off by default (see above).

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
