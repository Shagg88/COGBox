# Gears of War Native - PC ports of Gears of War 2, Gears of War 3 and Gears of War: Judgment

Fan-made **native Windows versions** of three Xbox 360 games, built by static recompilation, with a launcher called
**COGBox** that sets up your own copy of each game and starts it.

**[Download the latest release](https://github.com/Shagg88/COGBox/releases/latest)** · You need your own copy of
each game. No game files are included or ever will be.

> This is an unofficial fan project. It is not made by, endorsed by or connected to Microsoft, Xbox Game Studios,
> The Coalition, Epic Games or People Can Fly. "Gears of War", "Xbox" and "Xbox 360" are trademarks of Microsoft.

---

## What it is

The games' original Xbox 360 code is translated ahead of time into C++ and compiled into ordinary Windows programs -
no emulator interprets the game while you play. The parts the console's hardware did are supplied by our code,
most importantly **our own renderer**, which draws every frame on Direct3D 12 (Vulkan is in progress). That gives
the ports what an emulator can't easily offer:

- **Any resolution** - 1x to 4x the original (up to 5K) or matched to your monitor, with real extra detail, not a
  stretched picture.
- **Anti-aliasing and upscaling** - SMAA, AMD FSR, NVIDIA DLSS and Intel XeSS, including upscaling modes for weaker GPUs.
- **High frame rates** - the 30 fps limit lifted, vsync with a cap matched to your monitor, AMD FSR frame generation.
- **Smooth play** - shaders are built on your PC and kept, pipelines are made in the background, and a texture that
  isn't ready yet shows a moment later instead of freezing the game.
- **Lower input lag** - one frame in flight, NVIDIA Reflex / AMD Anti-Lag 2 where your GPU has them.
- **HDR10**, **ultrawide 21:9**, **16x anisotropic filtering**, field of view, and the games' own motion blur, depth of
  field and lens flares as switches.
- **Split-screen** (Gears 2), **keyboard and mouse**, **title updates**, **chapter select**.

## The games

| Game | Version the port is made for | Status |
|---|---|---|
| Gears of War 2 | US disc (0.0.0.5) + Title Update 6 | Every chapter plays in testing; Title Update 6 is the default and fastest build |
| Gears of War 3 | Disc + Title Update 6 | Playable; Title Update 6 is the default |
| Gears of War: Judgment | Disc + Title Update 4 | Playable |

"In testing" means on the developer's PC (an RTX 3080 Ti) and scripted test runs through the chapters.
Other hardware hasn't been tried yet - please report what you find.

## COGBox, the launcher

- The three games as boxes on a carousel; box art and backgrounds from the open
  [x360db](https://github.com/xenia-manager/x360db) archive, downloaded only when you press **Get box art**.
- **Add your copy**: drop your disc image (.iso), Games on Demand copy, title update package or extracted game folder
  onto the window. COGBox recognises the game, unpacks it next to itself and checks it is the right release.
- A settings page per game with every option explained (rest the mouse on an option for its description).
- Play time, plain-English messages when a game closes unexpectedly, one-click problem reports, save backups.
- Updates itself from this page's releases.

---

## What you need

- **Windows 10 or 11, 64-bit.**
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

**Controls in COGBox:** LB / RB (or the arrow keys, the mouse wheel) turn the carousel; the D-pad moves between the
buttons and A presses one; X opens settings, Y gets box art, B / Esc goes back. On a settings page LB / RB change tab.

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

- **Vulkan as a choice and later the default** - for AMD / Intel cards, and a step towards Linux and Steam Deck.
- **Background art read from your own copy of each game** inside COGBox.
- **Multi-core rendering** (the next step of the speed work) and fewer first-visit hitches.
- **DLSS Frame Generation and DLSS 4/5 features** through NVIDIA Streamline on the cards that support them.
- **Controller remapping and an accessibility section** in COGBox.
- **Achievements**, kept locally.
- More of the Gears series.

## How it's made

- Static recompilation with the rexglue SDK toolchain (a Xenia-derived runtime), clang 23.
- A four-layer design so every game shares the work: the Xbox 360 GPU (our renderer), the console's D3D library
  version, Unreal Engine 3, and each game.
- Built and tested with a scripted test harness: chapter starts, scripted controller input, frame captures compared
  with NVIDIA FLIP, and unit tests.
- Credits: the [Xenia](https://xenia.jp) project and its Canary / Edge forks, whose open code and research made the
  renderer's exact rules possible; the community Gears patches (ultrawide by boma and TudorHorse) ported as switches;
  Xenia Manager's x360db for artwork; AMD, NVIDIA and Intel for their upscalers.

Repository layout: `src/` the port and shared renderer (layer 0-3), `cogbox/` the launcher, `tools/` build and test
scripts, `notes/` the design notes and decisions log. Building it yourself needs your own game copy, the toolchain in
`notes/30` and `tools/build.ps1`.
