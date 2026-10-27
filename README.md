# Gears of War Native - PC ports of Gears of War 2, Gears of War 3 and Gears of War: Judgment

**Fully Playable** with a FEW hiccups.  If you come across any PLEASE upload a log and I'll try to get to it!

Hello all, I'm Shagg and I'm building a launcher for **native Windows versions** of three(eventually four) Xbox 360 games, built by static recompilation,
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
- **HDR10**, **ultrawide 21:9** (all three games), **16x anisotropic filtering**, field of view, and the games' own motion blur, depth of
  field and lens flares as switches.
- **Split-screen** (Gears 2), **keyboard and mouse** (mouse look without the stick's speed limit, keys you can change),
  **Xbox and PlayStation controllers**, **title updates**, **chapter select**.
- **DLC and mods** - add DLC packages from your console and mods as .zip files on each game's About tab.
- **Every game language** the US and European releases have (21), chosen per game - the ones on your copy work.
- **In-game menu** - Back + Start (Share + Options on PlayStation, or F1) during play: change settings, restart to
  apply them, or quit.

## The games

| Game | Version the port is made for | Status |
|---|---|---|
| Gears of War 2 | US disc (0.0.0.5) + Title Update 6 | Every chapter plays in testing; Title Update 6 is the default and fastest build |
| Gears of War 3 | Disc + Title Update 6 | Playable; Title Update 6 is the default |
| Gears of War: Judgment | Disc + Title Update 4 (required for now) | Playable |

"In testing" means on the developer's PC (an RTX 3080 Ti) and scripted test runs through the chapters.
Other hardware hasn't been tried yet - please report what you find.

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
- **Desktop shortcuts** for COGBox and each game.

---

## What you need

- **Windows 10 or 11, 64-bit** - or **Linux / Steam Deck through Proton** (experimental, see below).
- **A CPU with AVX2** (Intel Haswell / AMD Ryzen or newer - roughly 2013 on).
- **A DirectX 12 graphics card.**
- **Your own copy of the game**, as a disc image (.iso) of the disc, a Games on Demand copy from your console, or a
  folder with the disc's files you extracted yourself.
- For Gears 2 and 3, the last **title update** (Title Update 6) is recommended. It's a file on a console that has
  played the game online, in `Content\0000000000000000\<title ID>\000B0000\`
  (Gears 2: `4D53082D`, Gears 3: `4D5308AB`, Judgment: `4D530A26`).
- About 8-16 GB of free space per game if COGBox unpacks a disc image for you.

## How to play

1. Download `COGBox-<version>.zip` from [Releases](https://github.com/Shagg88/COGBox/releases/latest).
2. Unzip the **whole** folder somewhere you can write to, e.g. `C:\Games\COGBox` (not Program Files).
3. Start `CogBox.exe`. The programs aren't code-signed, so Windows may show "Windows protected your PC" - choose
   **More info**, then **Run anyway**.
4. Turn to a game and press **ADD YOUR COPY** (or drop the file on the window). Then add the title update the same way.
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
- **Gamer Tag Login**
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
- The **Anton** and **Barlow** fonts (SIL Open Font License).

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

Also used, each under its own licence: Xenia / Canary / Edge (BSD-3), the rexglue SDK (BSD-style), FFmpeg (LGPL-2.1+,
for the game movies), SDL3 (zlib), Dear ImGui (MIT), SMAA (MIT), xxHash (BSD-2), miniz (MIT), stb (MIT), the DirectX
Shader Compiler, the Vulkan Headers, AMD FidelityFX and Anti-Lag 2, NVIDIA DLSS and Reflex, Intel XeSS, and the
Anton / Barlow fonts (SIL OFL). Full texts are in [`THIRD_PARTY_NOTICES.md`](Gears2-Native/THIRD_PARTY_NOTICES.md)
and the release's `licenses` folder.

No game content is included. "Gears of War" and "Xbox" are trademarks of Microsoft; this is an unofficial fan project.
