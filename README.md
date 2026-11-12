# Gears of War Native - PC ports of Gears of War, Gears of War 2, Gears of War 3 and Gears of War: Judgment

Hello all, I'm Shagg. These are my personal **native Windows versions** of the four Xbox 360 Gears of War games, made by static
recompilation, with a launcher called **COGBox** that sets up your own copy of each game and starts it. This franchise
is one of my favourites, so the project is near and dear to my heart.

**Fully playable** with a few hiccups - if you hit one, please send a problem report (see below).

**COGBox is back on GitHub!** This is our home again, and we're keeping [GitLab](https://gitlab.com/shagg88/cogbox) as a full backup
with the same releases.

**[Download the latest release](https://github.com/Shagg88/COGBox/releases)** · You need your own copy of each game
and its title update. No game files are included, ever.

**Online multiplayer is here (0.6.0):** play Gears of War online with friends again, peer to peer through COGBox's own
matchmaking - no Xbox Live needed. Off until you turn it on in the launcher's Social tab.

**[Join the COGBox Discord](https://discord.gg/Y6qGbzBKQZ)** - find people to play with, get help, share feedback.

<a href="https://recomplauncher.com/game/cogbox"><img src="https://recompstore.com/assets/press/badge-dark.svg" alt="Get it on RecompStore" height="56"></a>

> An unofficial fan project, not affiliated with Microsoft, Epic Games or The Coalition. "Gears of War", "Xbox" and
> "Xbox 360" are trademarks of Microsoft.

---

## The games

| Game | Needs | Status |
|---|---|---|
| Gears of War | Disc + Title Update 5 | Playable |
| Gears of War 2 | US disc + Title Update 6 | Every chapter plays in testing |
| Gears of War 3 | Disc + Title Update 6 | Playable |
| Gears of War: Judgment | Disc + Title Update 4 | Playable |

Title updates are required; disc-only copies aren't supported.

## Features

The games' Xbox 360 code is translated ahead of time into ordinary Windows programs - no emulator runs while you play -
and our own renderer draws every frame on Direct3D 12 (or Vulkan).

- **Any resolution** up to 5K, or matched to your monitor, with real extra detail; **ultrawide** and **HDR10**.
- **Upscaling and anti-aliasing:** SMAA, AMD FSR, NVIDIA DLSS and Intel XeSS (experimental).
- **High frame rates:** the 30 fps limit lifted, frame-rate caps from 30 to 180 (and 40 / 45 for the Steam Deck), and
  frame generation (AMD FSR, NVIDIA DLSS on RTX 40/50; experimental).
- **Lower input lag:** NVIDIA Reflex and AMD Anti-Lag 2 where your GPU has them.
- **Keyboard and mouse** or **Xbox / PlayStation controllers**, with button prompts that follow what you use.
- **Split-screen co-op** in all four games, and **System Link** LAN co-op (experimental).
- **Online multiplayer** (0.6.0, off by default): peer to peer through COGBox's own matchmaking.
- **Insane from the start**, **achievements** in all four games, chapter select, and an **in-game menu**
  (Back + Start, or F1) to change settings or quit.
- **DLC and mods**, every language your copy has, and screenshots with **\\**.

## What you need

- **Windows 10 or 11, 64-bit** (or Linux / Steam Deck through Proton - experimental).
- **A CPU with AVX2** (roughly 2013 or newer) and **a DirectX 12 graphics card**.
- **Your own copy of each game** - a disc image (.iso), a Games on Demand copy or the disc's extracted files - and its
  **title update**, copied from your console's `Content\0000000000000000\<title ID>\000B0000\` folder
  (Gears of War `4D5307D5`, Gears 2 `4D53082D`, Gears 3 `4D5308AB`, Judgment `4D530A26`).
- About 8-16 GB of free space per game.

## How to play

1. Download `COGBox-<version>.7z` from [Releases](https://github.com/Shagg88/COGBox/releases) and unpack the whole
   folder (7-Zip, or Windows 11 itself) somewhere you can write to, e.g. `C:\Games\COGBox` - not Program Files, and not a OneDrive / Dropbox folder.
2. Start `CogBox.exe`. If Windows says "Windows protected your PC", choose **More info > Run anyway** (the programs
   aren't code-signed).
3. Drag your game copy onto the COGBox window, then its title update. COGBox checks and unpacks them.
4. Press **PLAY**. **SETTINGS** has graphics, controls and gameplay options; rest the mouse on any option to see what
   it does.

**In COGBox:** the D-pad or arrow keys pick a game, LB / RB pick a button, A presses it. The bar at the bottom shows
the buttons for your controller or keyboard.

**DLC:** drop the DLC's folder or package onto COGBox, or use **Downloadable content** on the game's About tab.

**Players and profiles:** click the **PLAYER** plate to set your name and picture. Each person can have their own
profile with their own saves; players 2-4 can be signed out from their cards.

**Box art:** press **UPDATE ART** to download HD box art and backgrounds.

**Updating:** use the update button in COGBox, or unzip a new release over the old folder - your games, saves and
settings are kept.

## Linux and Steam Deck (experimental)

1. Unzip COGBox into your home folder (on a Steam Deck, in Desktop Mode).
2. In Steam: **Games > Add a Non-Steam Game**, set the file type to **All Files**, and pick `CogBox.exe`.
3. In its **Properties > Compatibility**, force **Proton 11.0** (or Proton Experimental / GE-Proton).
4. Start COGBox. It picks suitable settings by itself (Vulkan, FSR, 60 fps on the Steam Deck) and greys out what can't
   work under Proton, with the reason.
5. Add your games with **Look for my games** or **Choose a folder...** (your Linux files are under `Z:`).

The first minutes of each game can stutter while shaders are built; **Prepare all shaders now** on the Graphics tab
does it in one go.

## If something goes wrong

When a game closes unexpectedly, COGBox explains what happened and offers a **problem report** - one zip with the log,
your settings and your PC's specs (no saves or game files, and your Windows user name removed). Nothing is sent by
itself: open a [ticket on GitHub](https://github.com/Shagg88/COGBox/issues), say what you were doing and attach the
zip.

Common causes: a CPU without AVX2, an old graphics driver, or a different release of the game than the port is made for
(COGBox warns you when you add it).

## Legal

- **No game files are included** - not in the download or the source. COGBox never downloads games.
- **Use your own, legally obtained copy**, copied from your own hardware. We don't help with or support pirated copies,
  cracks, repacks or copy-protection bypasses.
- COGBox is provided **as is, without warranty**; you're responsible for following the laws and licences that apply to
  you, including each game's licence agreement.

The full text, including takedown requests, is in [`LEGAL.md`](LEGAL.md); what COGBox stores and sends is in
[`PRIVACY.md`](PRIVACY.md). Want to help? See [`CONTRIBUTING.md`](CONTRIBUTING.md).

## What's coming

Plans, not promises: Vulkan as the default, better frame generation, texture packs, multi-core
rendering, more controllers and accessibility options, and tested Linux / Steam Deck support.

## Community translations

- **Gears of War 2 in Brazilian Portuguese** - menus, subtitles and a full campaign dub by HubertFer and the
  Gears2-PTBR team: <https://github.com/Gears2-PTBR/Gears2-PTBR/releases> (their project; check that its version
  matches your COGBox version).

## Contributors

Thank you to everyone who has contributed code, translations, testing and tips (what each did is in the release notes):

- **Flippage**
- **KiltedKernel**
- **gmihaic**
- **HubertFer**
- **Hevesi J.**
- **LenzTRO**
- **francoissubke-blip**

## Thank you

This project stands on the work of many open-source projects and communities:

- **[Xenia](https://xenia.jp)** and its **Canary** and **Edge** forks - years of open Xbox 360 research our renderer
  follows - and the **rexglue SDK**, the recompilation toolchain and runtime the ports are built with.
- **[SDL3](https://libsdl.org)**, **[Dear ImGui](https://github.com/ocornut/imgui)**,
  **[SMAA](https://github.com/iryoku/smaa)**, **[xxHash](https://github.com/Cyan4973/xxHash)**,
  **[miniz](https://github.com/richgel999/miniz)**, **[stb](https://github.com/nothings/stb)**, the
  **[DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)**, the
  **[Vulkan Headers](https://github.com/KhronosGroup/Vulkan-Headers)**, the **DirectX 12 Agility SDK** and
  **[NVIDIA FLIP](https://github.com/NVlabs/flip)**.
- **AMD FidelityFX**, **NVIDIA DLSS, Streamline and Reflex**, and **Intel XeSS**.
- The **[LaunchBox Games Database](https://gamesdb.launchbox-app.com/)** and
  **[x360db](https://github.com/xenia-manager/x360db)** for box art and backgrounds.
- The **Gears community patches** (ultrawide by boma and TudorHorse) and
  **[Xenia Canary's game patches](https://github.com/xenia-canary/game-patches)** (illusion, boma), ported as switches.
- The **Anton**, **Barlow**, **Inter** and **Noto** fonts and **[Kenney Input Prompts](https://kenney.nl/assets/input-prompts)**.

If we've missed anyone, please open an issue and we'll add them.

## For developers

The ports are built with the rexglue SDK and clang, in four layers shared by every game: the Xbox 360 GPU (our
renderer), the console's D3D library, Unreal Engine 3, and each game. Each game has its own folder (`Gears1-Native`,
`Gears2-Native`, `Gears3-Native`, `GearsJ-Native`); `Gears2-Native` also holds the shared layers and the COGBox launcher
(`Gears2-Native/cogbox`). Building needs your own game copy - see [`docs/building.md`](docs/building.md).

## Licences

COGBox and the ports' own code are under the **MIT licence** ([`LICENSE`](LICENSE)).

**Using our code?** You're welcome to. If you use COGBox's code in your own project, please credit
**"COGBox by Shagg"** and link to <https://github.com/Shagg88/COGBox> in your readme or credits. (The MIT licence
already asks you to keep the copyright notice in copies of the code.)

Also used, each under its own licence: Xenia / Canary / Edge (BSD-3), the rexglue SDK (BSD-style), FFmpeg (LGPL-2.1+,
for XMA music and in-game audio), SDL3 (zlib), Dear ImGui (MIT), SMAA (MIT), xxHash (BSD-2), miniz (MIT), stb (MIT), the DirectX
Shader Compiler, the Vulkan Headers, AMD FidelityFX and Anti-Lag 2, NVIDIA DLSS, DLSS Frame Generation, Streamline (MIT) and
Reflex (NVIDIA RTX SDKs License / Reflex SDK licence), Intel XeSS, and the
Anton / Barlow / Inter fonts (SIL OFL). Full texts are in [`THIRD_PARTY_NOTICES.md`](Gears2-Native/THIRD_PARTY_NOTICES.md)
and the release's `licenses` folder.

**Upscaling and frame generation credits** (text only, no vendor logos):
- NVIDIA® DLSS — Uses NVIDIA DLSS technology (the NVIDIA DLSS SDK, the NVIDIA Streamline SDK and the NVIDIA Reflex SDK).
  NVIDIA, GeForce RTX, DLSS and Reflex are trademarks of NVIDIA Corporation.
- AMD FidelityFX™ Super Resolution (FSR) — Uses AMD FidelityFX FSR technology (AMD FidelityFX SDK).
  AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc.
- Intel® XeSS — Uses Intel XeSS technology (XeSS Super Resolution, XeSS Frame Generation, XeLL).
  Intel and XeSS are trademarks of Intel Corporation or its subsidiaries.

What ships in the download, and under which terms (the last column is the path inside the release download; in this
repository the texts are in `Gears2-Native/THIRD_PARTY_NOTICES.md` and `Gears2-Native/cogbox/licenses/`):

| Component (shipped in the download) | Version | Licence | Text in the download |
|---|---|---|---|
| COGBox launcher and the ports' own code | 0.6.0 | MIT | `LICENSE.txt` |
| rexglue SDK runtime (`rexruntime.dll`, `rexgpu-xenos.dll`), derived from Xenia | 0.10.0-dev (bd833a2 + our patches) | BSD-3-Clause | `licenses\rexglue-sdk\` (+ its bundled libraries' licences) |
| Xenia / Canary / Edge code ported into the renderer | Canary e6cd2e4, Edge 5045c19 | BSD-3-Clause | `licenses\Xenia-LICENSE.txt` |
| FFmpeg XMA decoder (`avcodec-63.dll`, `avutil-61.dll`; also linked into `rexruntime.dll`) | 9.0.2 (launcher); Xenia fork (runtime) | LGPL-2.1-or-later (not a GPL build) | `licenses\FFmpeg-NOTICE.txt`, `FFmpeg-LGPL-2.1.txt` |
| SDL3 (`SDL3.dll`) | 3.4.20 | zlib | `licenses\SDL3-LICENSE.txt` |
| Dear ImGui, miniz, stb (stb_image, stb_dxt), LZMA SDK 7z decoder | - | MIT / MIT / MIT or public domain / public domain | `licenses\` |
| SMAA, XeGTAO, FidelityFX CACAO, modcore (KiltedKernel), native mouse look (gow2/gow3-recomp) | - | MIT / MIT / MIT / MIT / BSD-3-Clause | `licenses\Gears2-Native-THIRD_PARTY_NOTICES.md`, `licenses\modcore-LICENSE.txt` (in this repository: `Gears2-Native/THIRD_PARTY_NOTICES.md`) |
| NVIDIA DLSS Super Resolution and Frame Generation (`nvngx_dlss.dll`, `nvngx_dlssg.dll`) | 310.9.16 | NVIDIA RTX SDKs License (v. March 14, 2024), proprietary | `licenses\NVIDIA-Streamline-DLSS-G-NOTICE.txt`, `licenses\dlss-sdk\` |
| NVIDIA Streamline (`sl.*.dll`) and Reflex | 2.14.16 | MIT (Streamline); NVIDIA Reflex SDK licence | `licenses\streamline-sdk\`, `licenses\streamline-release\` |
| AMD FidelityFX SDK: FSR upscaler, frame generation, loaders (DX12 and Vulkan) | SDK 2.4.0 (upscaler 4.1.2, FG 4.0.2) | AMD FidelityFX licence (binary-only redistribution) | `licenses\AMD-FidelityFX-SDK-LICENSE.md`, `licenses\fidelityfx-sdk\` |
| AMD Anti-Lag 2 (header only, no DLL) | 2.0.0a | MIT | `licenses\antilag2-sdk\` |
| Intel XeSS (`libxess.dll`), XeSS Frame Generation (`libxess_fg.dll`), XeLL (`libxell.dll`) | 2.0.2.68 / 1.3.1.78 / 1.3.2.10 | Intel Simplified Software License (Oct 2022), proprietary | `licenses\xess-sdk-3.0.2\` (+ `third-party-programs.txt`) |
| Microsoft DirectX 12 Agility SDK (`D3D12Core.dll`) | 1.620.1 | Microsoft Software License Terms (redistributable) | `licenses\Microsoft-DirectX-redist-NOTICE.txt`, `licenses\agility-sdk-1.620.1\` |
| Microsoft DirectX Shader Compiler (`dxcompiler.dll`, `dxil.dll`) | 1.9.2609 | Microsoft Software License Terms (redistributable); LLVM / MIT parts | `licenses\Microsoft-DirectX-redist-NOTICE.txt`, `licenses\dxc-1.9.2609\` |
| Fonts: Anton, Barlow, Barlow Condensed, Inter 4.1, Noto Sans | - | SIL Open Font License 1.1 | `licenses\*-OFL.txt`, `assets\fonts\` |
| Kenney Input Prompts (button glyphs) | 1.5A | CC0 1.0 | `licenses\Kenney-Input-Prompts-LICENSE.txt` |
| Achievement chime (`assets\sounds\achievement.wav`) | - | made for COGBox, MIT | `LICENSE.txt` |

Not shipped: box art and backgrounds are downloaded by COGBox from the LaunchBox Games Database and the x360db archive only when you press UPDATE ART (see `licenses\LaunchBox-Games-Database-NOTICE.txt`); no game files are included.

This list is not legal advice; the licence texts in the `licenses` folder are what count.

No game content is included. "Gears of War" and "Xbox" are trademarks of Microsoft; this is an unofficial fan project.
