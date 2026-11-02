# Gears of War Native - PC ports of Gears of War 2, Gears of War 3 and Gears of War: Judgment

**PLEASE FOR NOW MAKE SURE TO USE THE LATEST TITLE UPDATE FOR EACH GAME**

**Fully Playable** with a FEW hiccups. If you come across any PLEASE upload a log and I'll try to get to it!

Hello all, I'm Shagg and I'm building a launcher for my own **native Windows versions** of three(eventually four) Xbox 360 games, built by static recompilation,
with a launcher called **COGBox** that sets up your own copy of each game and starts it.  These games were one of my favorite franchises so this
project is near and dear to my heart.  Gears of War 1 is currently not a priority because there's about 5 versions (it feels like) that are available
natively to PC.

**[Download the latest release](https://github.com/Shagg88/COGBox/releases/latest)** · You need your own copy of
each game. No game files are included or ever will be.

> This is an unofficial fan project. It is not made by, endorsed by or connected to Microsoft, Xbox Game Studios,
> The Coalition, Epic Games or People Can Fly. "Gears of War", "Xbox" and "Xbox 360" are trademarks of Microsoft.

---

## What it is

The games' original Xbox 360 code is translated ahead of time into C++ and compiled into ordinary Windows programs -
no emulator interprets the game while you play. The parts the console's hardware did are supplied by our code,
most importantly **our own renderer**, which draws every frame on Direct3D 12 (or Vulkan, experimental). That gives
the ports what an emulator can't easily offer:

- **Any resolution** - 1x to 4x the original (up to 5K) or matched to your monitor, with real extra detail, not a
  stretched picture - and a fast **1080p** mode (720p drawn, upscaled and sharpened) for weaker graphics cards.
- **Anti-aliasing and upscaling** - SMAA, AMD FSR, NVIDIA DLSS and Intel XeSS, including upscaling modes for weaker GPUs.
- **High frame rates** - the 30 fps limit lifted, vsync with a cap matched to your monitor, AMD FSR frame
  generation (highly experimental for now).
- **Smooth play** - the games' shaders are translated straight to Direct3D 12 shader code in under a millisecond each
  (as Xenia Canary does), kept on your PC, and pipelines are made in the background - a new area shows a moment
  later instead of freezing the game.
- **Vulkan (experimental)** - a choice on each game's Graphics tab, for testing; close to Direct3D 12 already.
- **Lower input lag** - one frame in flight, NVIDIA Reflex / AMD Anti-Lag 2 where your GPU has them.
- **HDR10**, **ultrawide 21:9** (all three games), **16x anisotropic filtering**, field of view, and the games' own motion blur and depth of
  field as switches (lens flares are set per game automatically: on where Real occlusion hides them behind walls).
- **Split-screen** (Gears 2), **keyboard and mouse** (mouse look without the stick's speed limit, keys you can change),
  **Xbox and PlayStation controllers** with **auto-switching button prompts** (the game's own for Xbox pads; PlayStation, Switch, Steam Deck, Steam Controller and keyboard prompts as you switch; `--gow_button_prompts=auto|original|xbox|playstation|switch|steamdeck|steamcontroller|keyboard`), **title updates**, **chapter select**.
- **DLC and mods** - add DLC packages from your console and mods as .zip files on each game's About tab.
- **Every game language** the US and European releases have (21), chosen per game - the ones on your copy work.
  Japanese (player reports, #103): Gears 3 and Judgment work from a Japanese disc (text and voices). Gears 2 is not
  supported in Japanese out of the box: text and subtitles only with a manual workaround, and movie voices stay English.
- **In-game menu** - Back + Start (Share + Options on PlayStation, or F1) during play: change settings, restart to
  apply them, or quit.

## The games

| Game | Version the port is made for | Status |
|---|---|---|
| Gears of War 2 | US disc (0.0.0.5) + Title Update 6 (required) | Every chapter plays in testing |
| Gears of War 3 | Disc + Title Update 6 (required) | Playable |
| Gears of War: Judgment | Disc + Title Update 4 (required) | Playable |

**Title updates are now required to play. Disc-only versions are no longer supported.** Every game needs its title
update (Gears of War: Title Update 5, Gears 2 and 3: Title Update 6, Judgment: Title Update 4) - see
[What you need](#what-you-need).

"In testing" means on the developer's PC (an RTX 3080 Ti) and scripted test runs through the chapters.
Other hardware hasn't been tried yet - please report what you find.

## Upgrading to 0.4.0 - what you need to do

### 1. Add each game's title update (now required)
Title updates are now required to play. Disc-only versions are no longer supported.

| Game | Title update needed | Title ID folder on the console |
|---|---|---|
| Gears of War | Title Update 5 | `4D5307D5` |
| Gears of War 2 | Title Update 6 | `4D53082D` |
| Gears of War 3 | Title Update 6 | `4D5308AB` |
| Gears of War: Judgment | Title Update 4 | `4D530A26` |

1. Copy the update package from your own console: it's in `Content\0000000000000000\<title ID>\000B0000\`.
2. In COGBox, turn to the game and press **ADD UPDATE** (or drop the package file onto the COGBox window, or use the
   game's About tab).
3. COGBox checks it and unpacks it into `<COGBox>\<game>\game\update`. Nothing else to set: Play always uses it.

**If the update is missing**, the game's box shows **Needs Title Update ...** and **ADD UPDATE** instead of Play. Your
copy and saves stay where they are; add the update and the game is ready again.

The Gears 2 and Gears 3 disc builds are no longer in the download, and the "Game version" option is gone (greyed out).

### 2. Gears 3 saves are converted automatically
The first time you start Gears 3 on 0.4.0, your existing saves are converted for the friendly-fire fix (#68). You
don't need to do anything.
Only Gears 3 saves are converted - Gears of War, Gears 2 and Judgment saves are not touched.

- A backup of each changed file is kept next to it, named `<file>.pre-xuid0`: `PlayerStorage.dat` (your campaign
  progress) and `profile\User\63E83FFF` (your profile), both under `<COGBox>\Gears3\user\`.
- **To roll back:** close Gears 3, rename the converted file (e.g. add `.new`), then rename the `.pre-xuid0` copy
  back to the original name (remove `.pre-xuid0`).
- **Going back to 0.3.x?** Restore the `.pre-xuid0` files first (steps above). A converted save started on 0.3.x
  makes Gears 3 reset it. Restored saves are converted again the next time you start 0.4.0.

### 3. Removed in this release
- **Gears 3 Cheats and "Unlock all mutators"** are gone for now. They only worked with the disc version (Title Update 6
  shows a dirty-disc error with them).
- **The Gears 2 / Gears 3 disc builds** are no longer included (see 1).

### 4. Your name in the games (Player name, #119)
1. In COGBox, open any game's **Settings**, then the **About** tab.
2. Type your name in **Player name** (up to 15 letters, digits or spaces; extra spaces are trimmed). It's saved straight away.
3. Start (or restart) any game: player 1 shows that name in all four games. Leave it empty for "User".
   It only changes the name shown: your profile folder and saves stay exactly where they are.

## COGBox, the launcher

- The three games as boxes on a carousel; box art and backgrounds from the open
  [x360db](https://github.com/xenia-manager/x360db) archive, downloaded only when you press **Get box art**.
- **Add your copy**: drop your disc image (.iso), Games on Demand copy, title update package or extracted game folder
  onto the window. COGBox recognises the game, unpacks it next to itself and checks it is the right release.
- A settings page per game with every option explained (rest the mouse on an option for its description).
- Play time, plain-English messages when a game closes unexpectedly, one-click problem reports, save backups.
- Updates itself from this page's releases: an **UPDATE TO ...** button appears when a new version is out.
- Works fully with a controller - Xbox or PlayStation (its button names switch to Cross / Circle / Square / Triangle).
- Grows with its window, and has a **borderless** option (its own minimise / maximise / close).
- **In 11 languages**: English, Spanish, French, German, Italian, Portuguese (Brazil), Polish, Russian, Japanese,
  Chinese (Simplified) and Korean - asked on first start, changeable on any game's About tab.
- **DLC** (Downloadable content on the About tab): add the packages from your console's
  `Content\0000000000000000\<title ID>\00000002` folder; remove them there too.
- **Mods** (Mods on the About tab): add a mod's .zip or drop its folder into the game's `mods` folder, tick it, set the
  order. A mod is laid out like the game's own files (`GearGame\...`) and replaces or adds files without changing your
  copy; programs are never taken from a mod.
- **Desktop shortcuts** for COGBox and each game. For a **Steam** entry per game, add `CogBox.exe` as a non-Steam game
  with the Launch Options `--play gears1` / `gears2` / `gears3` / `judgment` - not the game's own `gow3.exe` etc., which
  stops with "--game_data_root was not provided" (COGBox passes the game folder and your settings).
- **Player name** (About tab): the name every game shows for player 1 (display only, saves untouched).

### Adding DLC

The easiest way: drag the DLC's folder (or its package file) onto the COGBox window, or use **Downloadable content**
on the game's About tab. COGBox copies it to the right place. If you would rather do it by hand, a package goes into
the game's user folder under the console's own layout:

`<COGBox>\<game>\user\0000000000000000\<title ID>\00000002\<package file>`

For example, Gears of War 3 (title ID `4D5308AB`):
`<COGBox>\Gears3\user\0000000000000000\4D5308AB\00000002\<package file>`. Restart the game afterwards.

### Cheats (Gears of War 3)

> **Not available in this release:** the cheats and **Unlock all mutators** only work with the disc version so far
> (Title Update 6 shows a dirty-disc error with them), and title updates are now required. They'll come back once they
> work with Title Update 6.

Tick **Gameplay > Cheats (hotkeys F5-F11)** in the game's settings, then in game:

| Key | Cheat |
|---|---|
| F5 | God mode |
| F6 | Slow motion (0.5, 0.25, back to 1) |
| F8 | Free camera (photo mode) |
| F9 | Freeze the world |
| F10 | Hide / show the HUD |
| F11 | All weapons and ammo |

**Unlock all mutators** (same tab) makes every mutator available without touching your profile.

### System Link / LAN co-op (experimental)

Tick **Gameplay > System Link - LAN play (experimental)** on every PC. Every PC needs the same COGBox version (0.4.0)
and the same game with its title update. The PCs must be on
the same network, or on a LAN VPN such as Radmin VPN, ZeroTier or Hamachi (not an internet VPN like NordVPN). If Windows
asks about the firewall, allow Private networks. With 3 or more PCs, each PC must list every other PC (full steps, including Radmin VPN, are in the
README.txt inside the download). Gears 2 campaign co-op worked in testing; other modes and games are less
tested. There is no voice chat yet, and no internet (Xbox Live) play.

**On 0.4.0, Gears 3 System Link co-op needs a two-PC test.** The #68 fix changes two things the game sees: XNetRandom
now gives real random numbers (before, every PC could produce the same "random" values, so this should help), and
Gears 3 now gets "no online ID" (0) for every player on every PC. COGBox's own System Link code finds and connects PCs
by IP address and doesn't use those IDs (the offline IDs were already the same on every PC), but we haven't yet
checked what Gears 3's own session code does with them. Please report how it goes. Gears of War, Gears 2 and Judgment
are unchanged.

---

## What you need

- **Windows 10 or 11, 64-bit** - or **Linux / Steam Deck through Proton** (experimental, see below).
- **A CPU with AVX2** (Intel Haswell / AMD Ryzen or newer - roughly 2013 on).
- **A DirectX 12 graphics card.**
- **Your own copy of the game**, as a disc image (.iso) of the disc, a Games on Demand copy from your console, or a
  folder with the disc's files you extracted yourself.
- **The game's title update - required.** Title updates are now required to play; disc-only versions are no longer
  supported. Gears of War needs Title Update 5, Gears 2 and 3 Title Update 6, Judgment Title Update 4. It's a file on a
  console that has played the game online, in `Content\0000000000000000\<title ID>\000B0000\`
  (Gears of War: `4D5307D5`, Gears 2: `4D53082D`, Gears 3: `4D5308AB`, Judgment: `4D530A26`).
- About 8-16 GB of free space per game if COGBox unpacks a disc image for you.

## How to play

1. Download `COGBox-<version>.zip` from [Releases](https://github.com/Shagg88/COGBox/releases/latest).
2. Unzip the **whole** folder somewhere you can write to, e.g. `C:\Games\COGBox` (not Program Files).
3. Start `CogBox.exe`. The programs aren't code-signed, so Windows may show "Windows protected your PC" - choose
   **More info**, then **Run anyway**.
4. Turn to a game and press **ADD YOUR COPY** (or drop the file on the window). Then add the title update the same way - it's required.
5. Press **PLAY**. **SETTINGS** has the graphics, frame rate, controls and gameplay options.

**Controls in COGBox:** the D-pad (or the arrow keys, the mouse wheel) turns the carousel; LB / RB choose Play or
Settings and A presses it; X opens settings, Y gets box art. On a settings page LB / RB change tab, D-pad down past the
last option reaches Library / Save / Play, and B jumps to Library.

**PlayStation controllers** work too - COGBox shows Cross / Circle / Square / Triangle and L1 / R1 instead.

**In the game:** Back + Start (Share + Options on PlayStation, or F1) opens the in-game menu - the same settings, **Resume**, **Restart to apply** and
**Quit game**. Most settings take effect when the game restarts; Restart to apply does that for you.

**Graphics API:** Direct3D 12 is the default and the tested renderer. **Vulkan (experimental)** is on each game's
Graphics tab - FSR / DLSS / XeSS and frame generation are Direct3D 12 only. Problem reports from Vulkan help a lot.

**Linux and Steam Deck (experimental):** unzip COGBox in your home folder, add `CogBox.exe` to Steam as a non-Steam
game, and in its Properties > Compatibility force **Proton Experimental** (or a recent Proton / GE-Proton). On a small
screen like the Deck's, COGBox starts maximised and borderless. Under Proton it skips exclusive fullscreen, Reflex and
Anti-Lag. Both Direct3D 12 (through vkd3d-proton) and Vulkan can be tried. Untested on real hardware yet - reports
very welcome.

**Updating:** use the update button on any game's About tab, or unzip a new release over the old folder. Your games,
saves and settings are not in the zip, so they are kept.

## If something goes wrong

- If a game closes unexpectedly, COGBox says what happened and offers a **problem report**: one zip on your desktop
  with the log, your settings and a description of your PC - no saves, no game files, your Windows user name removed.
  Nothing is sent anywhere by itself.
- Open an [issue](https://github.com/Shagg88/COGBox/issues), say what you were doing, and attach that zip.
- Common causes: a CPU without AVX2 (the game can't start), an outdated graphics driver, a different release of the
  game than the one the port is made for (COGBox warns about this when you add it).

---

## Legal

- **No game content is included.** Not in the downloads, not in this repository's code or history. You supply your
  own copy, and COGBox checks it.
- **Please only use a copy of the game you own**, and don't share game files, disc images or anything extracted from
  them - here or anywhere. Issues or pull requests containing game files will be removed.
- **What this project contains:** our own code (MIT licence, see `LICENSE`) and pieces of the Xenia emulator projects
  (BSD-3-Clause); the release adds the rexglue SDK runtime (BSD-3-Clause) and the vendors' own upscaler libraries (NVIDIA DLSS,
  AMD FidelityFX, Intel XeSS) and the DirectX Shader Compiler under their own licences. Every licence is in
  `THIRD_PARTY_NOTICES.md` and in the release's `licenses` folder.
- **Box art and backgrounds** are not included; COGBox downloads them from the x360db archive only when you ask, onto
  your own PC.
- This project is offered for preservation and personal use, as is, with no warranty. It is not legal advice, and the
  rules differ by country.

---

## What's coming

Plans, not promises:
- **Gamer Tag Login** - a Player name (display only) is in since 0.4.0; real sign-in later.
- **Texture packs** - a texture dumper and replacement packs (mods and DLC are in since 0.2.0).
- **Mac, Linux, Steamdeck** - Linux and Steam Deck through Proton are in as experimental now; tested support next.
- **Online Co-Op** - Hopefully.
- **Vulkan as the default** - it's already a choice (experimental); next: matching Direct3D 12 everywhere, then default.
- **Better frame generation** - an OptiScaler-style frame generation to replace today's experimental AMD FSR one.
- **Background art read from your own copy of each game** inside COGBox.
- **Multi-core rendering** (the next step of the speed work) and shaders built at the main menu, for no first-visit hitches.
- **DLSS Frame Generation and DLSS 4/5 features** through NVIDIA Streamline on the cards that support them.
- **More controllers (Switch and others) and an accessibility section** in COGBox.
- **Split-screen across two monitors**, and the European releases at full speed.
- **Achievements**, kept locally.
- More of the Gears series.

## Thank you

This project stands on the work of many open-source projects and their communities. Thank you to:

- **[Xenia](https://xenia.jp)** - Ben Vanik and the Xenia contributors, and the **Xenia Canary** and **Xenia Edge** forks.
  Their open code and years of research into the Xbox 360 made this possible; much of our renderer's exact behaviour,
  its texture and resolve rules, and the fast shader translation follow their work.
- **[Flippage](https://github.com/Flippage)** - mouse control and input isolation for the in-game menu (PR #134), the
  controller prompt glyphs (PR #117), and the gow3-recomp mouse look our native mouse look builds on.
- **rexglue SDK** - the static-recompilation toolchain and runtime the ports are built
  with (Xenia-derived).
- **[SDL3](https://libsdl.org)** - windows, controllers (Xbox, PlayStation and more) and input, in the games and COGBox.
- **[Dear ImGui](https://github.com/ocornut/imgui)** - the COGBox launcher and the in-game menu.
- **[SMAA](https://github.com/iryoku/smaa)** - Jorge Jimenez and co-authors, the anti-aliasing.
- **[xxHash](https://github.com/Cyan4973/xxHash)**, **[miniz](https://github.com/richgel999/miniz)** and
  **[stb](https://github.com/nothings/stb)** - fast hashing, zip files and images.
- **[DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)** and the
  **[Vulkan Headers](https://github.com/KhronosGroup/Vulkan-Headers)** - Microsoft and the Khronos Group.
- **AMD FidelityFX (FSR)**, **NVIDIA DLSS** and **Intel XeSS** - the upscalers, under their vendors' own licences.
- **[Xenia Manager](https://github.com/xenia-manager)** and its **[x360db](https://github.com/xenia-manager/x360db)**
  archive - the box art and backgrounds COGBox downloads.
- The **Gears community patches** - ultrawide by boma and TudorHorse (Gears 2, 3 and Judgment), ported here as switches.
- **Microsoft DirectX 12 Agility SDK** - the DirectX 12 runtime the ports bring along (so Windows 10 works too).
- **[NVIDIA FLIP](https://github.com/NVlabs/flip)** - the picture comparisons in our test runs.
- The **Anton** and **Barlow** fonts (SIL Open Font License), and **[Kenney Input Prompts](https://kenney.nl/assets/input-prompts)** (CC0 1.0 Public Domain) for controller and keyboard/mouse prompt glyphs.

Every licence is in `THIRD_PARTY_NOTICES.md` and in the release's `licenses` folder. If we've missed anyone, please
open an issue and we'll add them.

## How it's made

- Static recompilation with the rexglue SDK toolchain (a Xenia-derived runtime), clang 23.
- A four-layer design so every game shares the work: the Xbox 360 GPU (our renderer), the console's D3D library
  version, Unreal Engine 3, and each game.
- Built and tested with a scripted test harness: chapter starts, scripted controller input, frame captures compared
  with NVIDIA FLIP, and unit tests.
- Credits: the [Xenia](https://xenia.jp) project and its Canary / Edge forks, whose open code and research made the
  renderer's exact rules possible; the community Gears patches (ultrawide by boma and TudorHorse) ported as switches;
  Xenia Manager's x360db for artwork; AMD, NVIDIA and Intel for their upscalers.

Repository layout - one folder per game, side by side (the folder names matter: Gears 3 and Judgment build against
`../Gears2-Native`):

- `Gears2-Native/` - Gears of War 2, the shared renderer and runtime layers (`src/layer0_xenos` and up), the COGBox
  launcher (`cogbox/`), the unit tests and the toolchain notes (`notes/30`).
- `Gears3-Native/` - Gears of War 3 (layers 2-3 for its engine and game, its SDK patches in `patches/`).
- `GearsJ-Native/` - Gears of War: Judgment.

Each folder has `src/`, `tools/` (build and test scripts; `tools/build.ps1` builds) and `notes/` (design notes and
decisions log). Building it yourself needs your own game copy and the toolchain in `Gears2-Native/notes/30`.

## Where things are (on the PC)

`C:\COGBox` is the whole project in one folder:

| Folder | What | In git |
|---|---|---|
| `Gears2-Native\`, `Gears3-Native\`, `GearsJ-Native\` | Each game's port: source (`src`, `tools`, `notes`), and on the PC its `third_party` (build tools / SDK), `game` (your own game files), `build` and `out` | source only |
| `Gears2-Native\cogbox\` | The COGBox launcher | yes |
| `releases\<version>\` | Every release: the zip, its `.sha256` and the release text | no |
| `history\` | The work history: the chats' mailbox, the full old git histories, notes | no |
| `work\` | Test copies (`work\cogbox-dev`) | no |

Build a game: `<game folder>\tools\build.ps1` (Gears 2 TU6: `-Config Release -Tu6`). Pack a release:
`Gears2-Native\cogbox\tools\package_cogbox.ps1` (lands in `releases\<version>`).

## Licences

COGBox and the ports' own code are under the **MIT licence** ([`LICENSE`](LICENSE)).

**Using our code?** You're welcome to. If you use COGBox's code in your own project, please credit
**"COGBox by Shagg"** and link to <https://github.com/Shagg88/COGBox> in your readme or credits. (The MIT licence
already asks you to keep the copyright notice in copies of the code.)

Also used, each under its own licence: Xenia / Canary / Edge (BSD-3), the rexglue SDK (BSD-style), FFmpeg (LGPL-2.1+,
for the game movies), SDL3 (zlib), Dear ImGui (MIT), SMAA (MIT), xxHash (BSD-2), miniz (MIT), stb (MIT), the DirectX
Shader Compiler, the Vulkan Headers, AMD FidelityFX and Anti-Lag 2, NVIDIA DLSS and Reflex, Intel XeSS, and the
Anton / Barlow fonts (SIL OFL). Full texts are in [`THIRD_PARTY_NOTICES.md`](Gears2-Native/THIRD_PARTY_NOTICES.md)
and the release's `licenses` folder.

No game content is included. "Gears of War" and "Xbox" are trademarks of Microsoft; this is an unofficial fan project.
