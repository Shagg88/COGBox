# 11 - Research: the player's side (Shagg 21:30 research round)

## Part 1 - other ports' player features + Windows packaging (mailed to Gears 2 21:40)
A. UnleashedRecomp (Sonic Unleashed, Xbox 360 recomp - closest to us)
 - First-run INSTALLER inside the game: language, then point at your dump; "Add Files" takes 360 containers / disc
   images and detects what each is, "Add Folder" takes extracted files; "many integrity checks", original files only,
   no region mixing; title update REQUIRED, DLC optional, re-run later from the title screen or --install [V]
   https://cdn.jsdelivr.net/gh/hedge-dev/unleashedrecomp@main/README.md ; parsers + virtual FS in install/, Dear
   ImGui UI [V] https://mintlify.wiki/hedge-dev/UnleashedRecomp/building/architecture
   OURS: today the player must extract the disc + TU by hand (game\extracted, game\tu4). A launcher "Install" step
   that takes an ISO / GoD / extracted folder + the TU file and checks SHA-1s (Gears 2's 0131 fingerprints are the
   start) is the biggest player-experience gap. Medium-high effort (360 container parsing), once for all three games.
 - ACHIEVEMENTS rebuilt in-game: pop-ups, list, 100% trophy; no Steam/RetroAchievements link; stored under user/ [V]
   (README, arch doc). OURS: hook the XAM achievement-write call the games make (layer 0/SDK side), store locally,
   show in the launcher. Low-medium.
 - OPTIONS MENU in game, pad-navigable: resolution scale, MSAA, 60 fps default + unlocked, ultrawide with HUD
   alignment, 360/PS3 glyphs, 6 languages, F1 perf graphs [V] (README). OURS: Shagg's planned in-game menu for the
   launcher settings is exactly this.
 - Input: DS4/DualSense lightbar + touchpad [V]; keyboard rebinding only by editing config.toml, UI "planned" [V].
   OURS: a remap screen in the launcher would beat them. Low-medium.
 - Mods: external Hedge Mod Manager, file replacement only, no code mods [V]. OURS: a file-override folder + load
   order is cheap (our layer 2 already edits the ini bundle; a "mods\" folder of loose packages is the next step).
 - Saves/config in %APPDATA%\UnleashedRecomp; empty portable.txt -> everything next to the exe; update = copy the new
   release over, saves kept [V]. OURS: trivial and worth copying (we already keep userdata in our out\ folders).
 - No auto-update, no crash reporting, no System Link mentioned [V/G]. Rule: "never distribute builds on other
   websites" [V]. Requirements stated: AVX CPU, D3D12 or Vulkan 1.2, Win10 1909+ [V].
 - Flatpak + Steam Deck notes (desktop mode, add as non-Steam game) [V] https://steamdeckhq.com/news/sonic-unleashed-native-pc-port-steam-deck

B. N64Recomp family (Zelda64Recomp, Banjo)
 - Picks a ROM, converts formats, accepts only the right region [V] https://github.com/JS-mylove/Zelda64Recomp
 - In-game config: graphics, audio, remapping, dead zone, axis invert, mouse/keyboard/pad navigation [V] (same).
 - Mods v1.2: drag onto the window or "Install Mods"; .nrm = zip + JSON manifest + dependencies; texture packs
   .rtz with on/off; in-game mod manager with thumbnails; hosted on Thunderstore [V]
   https://www.gamingonlinux.com/2025/05/zelda-64-recompiled-majoras-mask-adds-modding-support-texture-pack-support-optimizations-and-more/page=1/
   https://hackmd.io/@Wiseguy/HyhUXwiV0  OURS: the manifest + zip + drag-to-install idea is cheap to copy for file/ini
   mods; code mods (PowerPC) are expensive.
 - Flatpak kept, AppImage dropped over file-picker trouble [V] (GamingOnLinux). If we ever do Linux: test the native
   file picker early.

C. Ship of Harkinian (OoT)
 - Supported ROMs listed by SHA-1 in supportedHashes.json + a web checker; "Incompatible ROM hash" message [V]
   https://code.moparisthebest.com/mirror/Shipwright/blame/branch/develop-ackbar/README.md  OURS: publish our accepted
   SHA-1s and have the launcher say WHICH file mismatched. Low.
 - Accessibility submenu (e.g. no auto camera re-centre), separate X/Y sensitivity, autosave interval [V]
   https://gamingonlinux.com/2023/03/ship-of-harkinian-a-pc-port-of-ocarina-of-time-has-a-feature-filled-upgrade
   OURS: a small Accessibility group in the launcher (separate X/Y look sensitivity, camera shake off - we have it,
   subtitles forced on if the ini allows) is cheap goodwill.
 - Updates: extract over; major versions ask to regenerate the asset archive [V] https://www.shipofharkinian.com/faq
   OURS: if our extracted data format ever changes, the launcher should redo it by itself.

D. Others
 - sm64ex: options menu, remapping, mouse look, cheats menu, texture packs, --savepath [V].
   sm64coopdx: 16-player online co-op lobbies, Lua mods [V] https://en.wikipedia.org/wiki/Super_Mario_64_Coop_Deluxe
 - 360 System Link: Xenia Canary netplay fork with a lobby server [V] https://github.com/AdrianCassar/xenia-canary ;
   XLink Kai tunnels System Link [V] https://www.teamxlink.co.uk/wiki/Xenia_XLink_Kai_Setup  OURS: System Link over
   UDP (+ an optional lobby) would be unique among recomp ports; high effort; Gears versus/Horde co-op is the prize.

E. Windows packaging
 - None of these ports' pages mention code signing or antivirus trouble - most likely unsigned zips [G].
 - Unsigned exes get the strong SmartScreen block [V]. Azure Artifact Signing (ex Trusted Signing) ~US$9.99/month,
   individuals US/Canada only, CI-friendly, NO instant SmartScreen trust - reputation builds per identity; OV
   US$150-300/yr + HSM; EV US$400+/yr and no longer bypasses SmartScreen (2024) [V]
   https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options
 - SignPath Foundation (free for OSS): fully automated CI builds, OSI-licensed components, manual approval per
   request [V] https://about.signpath.io/product/open-source  - the launcher/runtime might qualify; a recompiled game
   exe probably not [G].
 - Static C runtime (/MT) so players never need the VC++ redistributable - trivial, worth checking our CMake [G].
 - Portable zip is the norm; legal model everywhere: code only, the player supplies their own dump, the port
   verifies it, no third-party hosting [V].

Ranked for us (my view): 1) launcher Install step (ISO/GoD/TU -> extracted, SHA-1 checked); 2) static CRT + published
checksums (+ signing later); 3) achievements; 4) remapping + accessibility group; 5) loose-file mod folder;
6) System Link (big). Parts 2 (stutter fixes other ports ship) and 3 (controllers / input latency) follow.

## Part 2 - shader/pipeline stutter fixes (mailed 21:48)
1. UnleashedRecomp (360 recomp) - pipelines tied to the GAME'S OWN LOADING
 - Hooks the game's "is this model fully made?" check (CModelData::CheckMadeAll) to answer "not yet" while that
   model's pipelines compile, and hooks the leave-loading-screen function to wait until the compile count is 0. Per
   model / terrain / particle material it predicts the exact pipelines (opaque, transparent, punch-through, water) and
   builds them on an idle-priority worker [V] https://github.com/hedge-dev/UnleashedRecomp/blob/main/UnleashedRecomp/gpu/video.cpp
 - The parts we must NOT copy (they ship game-derived data): a fixed list of 217 pipeline states compiled during the
   intro logos (gpu/cache/pipeline_state_cache.h), and every Xenos shader translated at build time (XenosRecomp) and
   embedded zstd-compressed in the exe [V] https://cdn.jsdelivr.net/gh/hedge-dev/unleashedrecomp@main/README.md
 - D3D12 specialization by LINKING: a tiny DXIL library (export uint g_SpecConstants(){return N;}) linked with each
   shader library through IDxcLinker - far cheaper than recompiling HLSL [V] (video.cpp). No ID3D12PipelineLibrary;
   relies on the driver cache [V].
 - For us [G]: the loading-screen gate maps straight onto UE3 - hook where UE3 finishes loading a level/package and
   creates material/shader resources, predict pipelines from vertex factory + material + blend, hold the loading
   screen until built. Zero frame-time cost; first-visit load time only. Judgment's ~240 x 325 ms on workers fits in a
   loading screen. Moderate work: one hook per resource kind + a "predicted vs actually used" miss log.

2. RT64 / Zelda64Recomp - ubershader + background specialization
 - One ubershader built at start-up as 8 pipelines (depth compare x depth write x coverage) + 3 dither pipelines;
   specialized shaders compile at idle priority; each draw uses the specialized one if ready, else the ubershader. On
   D3D12 specialized = HLSL compiled as lib_6_3 linked to a pre-built DXIL library [V]
   https://github.com/rt64/rt64 , src/render/rt64_raster_shader_cache.cpp , rt64_raster_shader.cpp
 - Cost: weak Intel GPUs ran slowly on the ubershader; a fork added a pre-built list + "Preparing graphics" bar [V]
   https://github.com/amazingalek/rush-2049-recomp/pull/91
 - For us [G]: works because the N64 pipeline is small and fixed. A Xenos ubershader = a microcode INTERPRETER in a
   pixel shader - possible, the biggest job and the slowest per pixel; blend/depth/RT formats still need separate PSOs.

3. Dolphin - "exclusive" (all draws through a few big shaders) vs "hybrid" (ubershader while the specialized one
   compiles). Pure ubershaders = "a massive performance drain" (~50% speed on integrated GPUs at 1x); settings changes
   force a recompile pause; and the trap that hits us too: a new blend/depth state still needs a new pipeline, and many
   drivers don't share compilation across pipelines, so a small stutter remains [V]
   https://dolphin-emu.org/blog/2017/07/30/ubershaders/

4. Xenia / Canary / RPCS3
 - Xenia mainline: per-game .xsh (microcode) + .xpso (pipeline descriptions, separate ROV/RTV) stored; at boot every
   stored pipeline is rebuilt on workers before the game runs [V]
   https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/pipeline_cache.cc
 - Canary: async_shader_compilation on by default - D3D12 SKIPS a draw whose pipeline isn't ready ("Skipping draw -
   pipeline not ready"); Vulkan draws a placeholder with a trivial pixel shader and swaps later; stored-cache load
   overlaps the intros instead of blocking boot; d3d12_pipeline_creation_threads = 75% of cores; store_shaders on [V]
   https://github.com/xenia-canary/xenia-canary/blob/canary_experimental/src/xenia/gpu/gpu_flags.cc
 - RPCS3: default "Async Recompiler with Shader Interpreter" (hybrid); interpreter less accurate, can pin the GPU at
   100%; boot shows "Loading/Compiling pipeline object N of M" [V]
   https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/system_config.h , https://github.com/RPCS3/rpcs3/pull/8106
 - For us [G]: a list recorded on the PLAYER'S OWN machine and rebuilt at boot is within our rules (nothing shipped).
   Skipping draws = pop-in / missing one-shot effects; a depth-only placeholder is the safer middle ground.

5. UE5's own fix (PSO precaching, 5.2+): at level load, candidate PSOs from each primitive's materials + vertex factory
   compiled behind the loading screen; later-streamed objects wait or use a compiled default material. One draw can
   cost "tens of milliseconds"; an empty driver cache adds 20-30 s to Fortnite's load; keeping PSOs alive instead of
   trusting the driver cache can cost >1 GB [V]
   https://www.unrealengine.com/tech-blog/game-engines-and-shader-stuttering-unreal-engines-solution-to-the-problem

6. API mechanisms
 - ID3D12PipelineLibrary: named, serializable PSOs; load fails with DRIVER_VERSION_MISMATCH / ADAPTER_NOT_FOUND after a
   driver/GPU change (rebuild locally); thread-safe except loading the same PSO twice at once [V]
   https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nn-d3d12-id3d12pipelinelibrary  - few MB; helps the second
   visit when the driver cache was evicted, not the first [G].
 - D3D12_SHADER_CACHE_SUPPORT_FLAGS tells what the driver/OS caches do [V]
   https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_shader_cache_support_flags
 - VK_EXT_graphics_pipeline_library (fast link at draw time, optimized compile swapped in later) - no D3D12 equivalent
   to rely on [V/G] https://www.khronos.org/blog/reducing-draw-time-hitching-with-vk-ext-graphics-pipeline-library
 - Advanced Shader Delivery (Agility SDK 1.618): store-installed precompiled database, Win11 + Xbox PC app only -
   does not fit us (a shipped list) [V/G] https://devblogs.microsoft.com/directx/agility-sdk-1-618/

Suggested order for layer 0 [G]:
 1. Loading-screen gate (UnleashedRecomp / UE5 idea) via UE3 load hooks - the game-specific hooks would live in each
    game's layer 3, the "hold until N pipelines done" + prediction API in layer 0. Should remove most first-visit freezes.
 2. Per-player recorded pipeline list rebuilt at boot / during the intros (Xenia / RPCS3); ID3D12PipelineLibrary as
    the second-level store. (Your shader prebuild 0068 + the launcher's Pre-build shaders are halfway there.)
 3. Depth-only placeholder pipeline for in-play misses, replacing the "hold when 8+ draws wait" stall.
 4. Measure WHY one PSO takes ~325 ms: if DXBC conversion is the slow part, pre-built DXIL libraries + DXC linking
    (RT64 / UnleashedRecomp) could cut it.
 5. Full Xenos-microcode ubershader only as a last resort.
Part 3 (controllers / input latency) follows.

## Part 3 - controllers, Steam Input / Deck, gyro, input latency (mailed 21:55)
1. Controller APIs
 - XInput: 4 fixed slots, two motors, no extra buttons [V] https://learn.microsoft.com/en-us/windows/win32/xinput/getting-started-with-xinput ;
   Xbox One trigger motors need Windows.Gaming.Input [V] https://discourse.libsdl.org/t/supporting-more-than-4-xinput-capable-devices-on-windows-rfc/25666/13
   OURS: keep XInput only as the FORMAT the game expects, fed from SDL. No cost.
 - GameInput: Microsoft's superset API, "lowest possible input latency" [V] https://devdocs.xbox.com/build/core-features/common/input/overviews/input-overview.md ;
   SDL3's GameInput backend is off by default outside GDK [V] https://wiki.libsdl.org/SDL3/SDL_HINT_JOYSTICK_GAMEINPUT/raw  - don't depend on it.
 - SDL3 HIDAPI drivers on by default: PS4, PS5, Switch, Switch 2, Steam Deck, Xbox [V] https://wiki.libsdl.org/SDL3/SDL_HINT_JOYSTICK_HIDAPI
   Trigger rumble (SDL_RumbleGamepadTriggers) on Xbox One pads only [V] https://wiki.libsdl.org/SDL3/SDL_RumbleGamepadTriggers/raw ;
   DualSense lightbar + rumble supported, adaptive triggers via the generic "send effect" call [V]
   https://discourse.libsdl.org/t/current-dualsense-support/31507
   OURS: 360 rumble -> SDL rumble (check our SDK does this today); trigger rumble / adaptive triggers / lightbar = optional extras. Low.
 - Others: UnleashedRecomp sets the DS4/DualSense LED from the game, warns DS4Windows/Steam Input can block LED/touchpad,
   has AllowBackgroundInput [V] (README; https://mintlify.wiki/hedge-dev/UnleashedRecomp/configuration/input).
   Zelda64Recomp: gyro on DS4/DualSense/Switch Pro, warns BetterJoy/DS4Windows can eat gyro [V]. Xenia: input system
   any/nop/sdl/winkey/xinput [V] https://xeniamanager.wiki/xenia-controller/ . Dolphin added an "SDL Gamepad (Stock)"
   profile [V] https://dolphin-emu.org/blog/7F
   OURS: SDL3 gamepad with its defaults. Low. Add "input while the window is in the background" as a launcher box (cheap).

2. Steam Input / Steam Deck
 - Steam tells SDL which pads to ignore (SDL_GAMECONTROLLER_IGNORE_DEVICES / ALLOW_STEAM_VIRTUAL_GAMEPAD) to avoid double
   input; SDL ignores Steam's virtual pads unless running under Steam [V]
   https://discourse.libsdl.org/t/how-does-sdl-filter-out-gamepads-that-are-being-used-as-steaminput-devices/43510
   OURS: never override those variables (the launcher must pass the environment through). No cost.
 - Deck gyro reads zero if the game bypasses Steam's virtual pad; Zelda64Recomp tells players to set Steam gyro "As
   Mouse" [V] https://discourse.libsdl.org/t/gyroscope-on-steam-deck-using-hid-driver/69064 ; README
   OURS: accept mouse motion for aiming while a pad is active (gyro-as-mouse works then). Low.
 - Deck Verified: glyphs must match the device in use; hide keyboard glyphs when not used; no setting needed to enable
   the pad [V] https://partner.steamgames.com/doc/steamdeck/compat
 - Glyphs: SDL_GetGamepadButtonLabelForType gives Cross/A per pad type [V] https://wiki.libsdl.org/SDL3/SDL_GetGamepadButtonLabelForType/raw ;
   Steam API alternative (GetInputTypeForHandle) [V] https://partner.steamgames.com/doc/api/isteaminput
   OURS: Auto / Xbox / PlayStation / Nintendo icons, like UnleashedRecomp's ControllerIcons. Medium - the game's
   button-icon textures/fonts have to be redirected per game (Gears 3 already noted pad glyphs in its prompts) [G].

3. Gyro aiming (worth it for a shooter [G])
 - Jibb Smart's rules: gyro = mouse; sensitivity 1 = 1 deg controller turn -> 1 deg camera, range to >=10; always a
   gyro-off button; acceleration/dead zone/smoothing optional and off by default; manual calibration [V]
   https://gamedeveloper.com/design/the-absolute-basics-of-good-gyro-controls
 - Flick stick (right stick snaps the camera, gyro fine-aims): Steam Input, Fortnite (third person), CS:GO, MWII [V]
   https://en.wikipedia.org/wiki/Flick_Stick
 - OURS: gyro via SDL sensors, added as camera movement ON TOP of the right stick (not faked stick input) [G] -
   needs each game's camera/aim code found and fed (layer 3 per game). Medium.

4. Input latency
 - Waitable swap chain: max frame latency 1 = lowest latency (2 only if CPU+GPU > 16.7 ms); wait on the handle at the
   very start of the frame [V] https://learn.microsoft.com/it-it/previous-versions/windows/apps/dn448914(v=win.10) ;
   without it DXGI queues 3 frames [V] https://developer.nvidia.com/blog/advanced-api-performance-swap-chains ;
   UnleashedRecomp waits "directly before the game's input polling" [V] (README).
   OURS: wait -> poll SDL -> answer XInputGetState. Probably the biggest single win; low cost - your layer 0 / SDK present path.
 - Flip model with 1-2 buffers more than the allowed queue; tearing/VRR needs ALLOW_TEARING on the swap chain and on
   Present; true independent flip needs borderless fullscreen [V] (NVIDIA link above). Low.
 - VRR: cap 3+ fps under refresh (141 at 144 Hz), driver V-Sync on, game V-Sync off [V]
   https://www.blurbusters.com/gsync/gsync101-input-lag-tests-and-settings/14/  -> a launcher "VRR" preset. Low.
 - Reflex: "up to 33%" lower latency when GPU-bound; needs NVAPI or Streamline; its markers (sim start, input sample,
   present) measure input->display [V] https://developer.nvidia.com/blog/optimizing-system-latency-for-esports-with-nvidia-reflex-sdk ,
   https://developer.nvidia.com/blog/understanding-and-measuring-pc-latency
 - Anti-Lag 2: call Update() just before input sampling; RDNA1+ and driver 24.7.1+ for D3D12; expect ~1-2 frames when
   working [V] https://gpuopen.com/learn/integrating-amd-radeon-anti-lag-2-sdk-in-your-game/
   OURS: medium per vendor; the Gears games are mostly CPU-bound with 30 fps-era logic, so the gain may be small [G].
 - Emulators: Dolphin measured ~60 ms console latency; its "Rush" present cut ~8-14 ms (Sunshine), Immediate+Rush ~10 ms
   (Wind Waker) [V] https://dolphin-emu.org/blog/7F . RetroArch run-ahead removes ~17 ms but needs fast save states and
   ~2x CPU [V] https://docs.libretro.com/guides/runahead/ - not feasible for Gears [G].
   OURS: a "present as soon as possible after input" mode is cheap [G]; measure with PresentMon/FrameView or Reflex markers.
 - Polling: read every frame, compare dwPacketNumber; empty slots only every few seconds [V] (MS XInput link).
   OURS: XInputGetState must return state SDL refreshed AFTER the frame wait, not cached earlier [G] - worth checking in the SDK.

Ranked for us (my view): 1) waitable swap chain + input read right after the wait (layer 0/SDK, low cost, biggest
win); 2) VRR-friendly limiter preset + ALLOW_TEARING; 3) SDL3 gamepad defaults + never override Steam's variables +
mouse-for-aim alongside a pad (Deck gyro); 4) glyph sets per pad type; 5) gyro aim; 6) Reflex / Anti-Lag 2 later.
That completes Judgment's three parts; findings are also in GearsJ-Native\notes\11-research-player-side.md.
