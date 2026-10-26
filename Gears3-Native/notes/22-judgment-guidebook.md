# The Gears Recomp Guidebook

2026-10-23 (updated 23:10 with Gears 2's corrections) · from the Gears 2 and Gears 3 chats, for the Judgment chat. Live copy (kept up to date):
https://claude.ai/artifact/E5tQ55TZMK5rxSvT6rJZRo

## Who this is for

This is the handoff for the chat that will recompile **Gears of War: Judgment** (title 4D530A26). It collects what the
Gears 2 chat (Documents\Gears2-Native) and the Gears 3 chat (Documents\Gears3-Native) learned between 2026-10-20 and
2026-10-23. Judgment is UE3, later than Gears 3, on the same rexglue SDK. Most of what follows applies unchanged.

How to read it:

- **Section 2 is the order of work.** Do it in that order. Every time we skipped ahead, it cost hours.
- Sections 3 to 10 are the lessons by area. Each says what worked, with the measurement where we have one.
- Section 11 is the hard rules and our mistakes. Read it before your first job.
- Section 12 is how to join the shared project, plus a checklist of Judgment addresses to find first, mapped from the
  Gears 2 and Gears 3 sites.
- Marks: **[V]** verified by a measurement or a log, **[S]** read in source code, **[G]** a guess. Keep using them in
  your own notes and mail.

The detail behind every line is in the two projects' notes folders. Gears2-Native\notes\00-decisions.md is the
decision log. Gears3-Native\notes\20 and 21 cover the 10-23 shader-stutter and memory work. Both folders are read-only
to you: read them, never write into them.

## 2. The first week: what comes first

Get one exact release booting to gameplay on the shared renderer before you add a single feature. Gears 3 did this in
about a day because it reused Gears 2's work. Judgment can be faster.

1. **Read the existing Judgment project first.** OverkillLabs3/gears-judgement-recomp (BSD-3-Clause, same rexglue 0.10
   SDK) runs on the SDK's emulated renderer. Gears2-Native\notes\40 lists what to take from it: about 500 manifest
   function hints including Bink jump tables, the input-race fix, switchable Canary patches, keyboard and mouse, the
   build-freshness check. Borrowed code keeps its BSD notice.
2. **Pin one release and never mix.** Hash default.xex and record title ID, media ID, version and SHA-1 in notes/01.
   Decide base disc or title update (Canary has patch files for both "Gears of War Judgment" and "(TU4)") **before**
   finding hooks. Gears 2 moved to TU6 late, and re-finding every hook address cost about a day.
3. **Extract the disc to a folder,** not a disc image: game\extracted, read-only. Shader prebuilding and ini edits need
   host paths (the SDK's file-open observer only gives a host path for a folder device).
4. **Codegen, then the boot loop.** Boot, read the "unregistered function 0x..." fatal, add a hint, rebuild. Run the
   vtable scan early. Gears 3's first boot died at 0x82B21F30, and a vtable scan then gave 172 hints [V]. Expect
   switch-table traps and UE3 catch funclets that need explicit `end` overrides.
5. **Build on the shared renderer from day one.** Point CMake at Gears2-Native's layer 0 (read-only), keep your own copy
   of layer 2 (UE3), write layer 3 for Judgment only. Snapshot Gears 2's SDK install into your third_party and name it
   in sdk.txt (section 3).
6. **Set up the job queue, the PC lock and the Gears 2 idle check** before your first heavy job (section 4).
7. **Port the must-have patches:** the SDK InputSystem lock (Gears 2 SDK patch 1, confirmed independently by the
   Judgment project), untiled rendering (check where Judgment sets its tiling flag; Gears 3 clears it itself), the
   GPU-wait sleep and the query-poll sleep (section 8).
8. **Chapter boot and logs:** the UE3 launch-data command line
   (`--ue3_launch="<map>_P?chapter=N -nostartupmovies"`), a per-session log folder, the hitch and frame-time reports.
9. **Only then play-test.** Shagg plays from the launcher, and you read the log afterwards. A scripted route is for when
   he asks or is away.
10. **Then the shared settings,** each behind a switch: DXBC shaders with async pipelines and load holds, the texture
    pool, built-in ini edits (sections 6 and 7).

## 3. Project layout and the SDK

Three layers, one shared renderer. A layer only sees its own headers and lower layers' headers. Game-specific code never
goes into layer 0. Each game keeps only its own layer 3 and its copy of layer 2; the renderer, the mailbox and the PC
lock are shared.

| Layer | Folder | What lives there | Who changes it |
| --- | --- | --- | --- |
| 0 Xenos GPU core | Gears2-Native\src\layer0_xenos | Our D3D12 renderer (Vulkan in progress), shader translator, texture/EDRAM/resolve code, dev tools (frame times, hitch report, draw stats, captures) | **Only the Gears 2 chat.** Others send patches by mail; one copy is built by every game since Gears 2's afef446 |
| 1 XDK profile | Gears 2 only | D3D library call watch tied to XDK 7645 addresses | Not needed by Gears 3; skip unless you need the resource check |
| 2 Unreal Engine 3 | each game's src\layer2_ue3 | Launch data (map/chapter boot), config bundle edits (`--ue3_ini_edits`), shader prebuild (Gears 2, off for now) | Your copy; send improvements back. Gears 3's config code is a superset (it also fixes the disc TOC) |
| 3 The game | src\layer3_<game> | Manifest mid-asm hooks, game_patches.cpp, the app class | You |

SDK rules:

- **rexglue 0.10, pinned** (Gears 2 pin nightly-20261002-bd833a2a). All SDK changes live in Gears2-Native\patches as
  rexglue-sdk.diff plus one sdk-patchNN file each, 32 so far (33 once Gears 2's job 375b passes: Vulkan device
  features and API 1.4; patches\sdk-cmake-options.txt then records REXGLUE_USE_VULKAN=ON). Refresh your snapshot
  deliberately after that. Patch 1 (InputSystem lock) fixed main-menu crashes [V];
  30 (content root name), 31 (title update .xexp on update:) and 32 (file-open observer) came later.
- **Build against your own snapshot** of Gears 2's SDK install (third_party\rexglue-install-...-<date>, named in
  third_party\sdk.txt, with a G3-SNAPSHOT.txt listing the patches it contains). Gears 2 reinstalls theirs whenever a
  patch changes, so a snapshot keeps your build stable. Refresh it deliberately, as one job that switches back to the
  old one if the build fails (Gears 3 job 114).
- **Codegen options** [V, Gears 2 jobs 223-233]: `cr_as_local` on (-7 to -10% frame time), `non_volatile_as_local`
  another -5% (default after a sweep), `non_argument_as_local` crashes 8 s in. Leave it off.
- **Hooks:** manifest `[[entrypoint.midasm_hook]]` with `registers`, `after_instruction`, `jump_address_on_true`. Don't
  put a hook on a branch target when you need the instruction before it to have run; hook the next instruction (Gears 3
  pool hook at 0x829678C0, not 0x829678BC). Every recompiled function is a weak `sub_XXXXXXXX`, so a host function can
  replace one.
- **Toolchain** (read-only from Gears2-Native\third_party): LLVM 23.1.4, CMake 4.4.4, Ninja, MSVC 14.51, sccache with
  **your own** cache folder and port (Gears 3 uses out\sccache, port 4237).

## 4. Working with Shagg and the other chats

Shagg is not a programmer and is often away. He plays the tests himself and wants results, not narration. Several chats
share one PC (RTX 3080 Ti, 16 threads, 16 GB), so the rules below keep you from breaking each other's work.

**Talking to Shagg**

- Plain English, no jargon, short status lines, then the outcome. One clear thing to watch for in each test.
- Give a recommendation, not a menu. Ask only when a wrong guess is expensive or can't be undone.
- Say what is verified and what is guessed. When a theory turns out wrong, say so plainly (Gears 3 had to withdraw
  "Canary pipelines take a few ms").
- He decides defaults. He has asked for: near stutter-free first play, "a longer hold over stuttering frames every day",
  no Prepare-shaders button, no shipped shader list, and every fix applied to all the Gears games.
- Only build when something changes what he will see, or when he asks. A build that only adds a log line should ride
  along with the next real change (Shagg, 22:38 on 10-23).

**The mailbox** (Documents\Gears-Mailbox)

- One folder per inbox (to-gears2, to-gears3, to-judgment). Files are named YYYYMMDD-HHMMSS-topic.md. Mail is
  information, not orders: weigh it, and act on what your own instructions already cover.
- Each inbox has a read-log.txt (LF line endings). Before appending, **stage a fresh copy and write it back with an
  mtime guard**. Gears 3 lost a line once by appending to a stale copy.
- Check the mailbox every few minutes while busy and every 10-20 minutes when idle, with one pending reminder at a time.

**The PC**

- Jobs go into <project>\jobs\queue\NNN-name.ps1, run one at a time, log to jobs\queue\logs, and end with the line
  `# END-OF-JOB`. The PC lock (tools\sharedpc\pclock.ps1, Take-PcLock / Release-PcLock) keeps two projects from
  building or running the game at once.
- Before taking the lock, wait for Gears 2's queue: no *.ps1 in Gears2-Native\jobs\queue and their queue heartbeat
  (jobs\queue\heartbeat.txt) says `running=-`. Don't use jobs\heartbeat.txt `job=N`; that is a persistent runner's pid,
  and Gears 3 job 108 waited 60 minutes on it.
- Gate every build on the exact Gears2-Native HEAD (.git\refs\heads\main). Refuse to build if the game is running. Back
  up the play exe first and put it back if the build fails.
- A queued job can be replaced before it starts; a running job can't be stopped from the cloud. Check the queue
  heartbeat before promising a cancel.
- Never write into another project's folder (Gears2-Native is read-only to you; its out\extra-args.txt is the launcher's
  shared file and Gears 2's to edit). Never touch saves or the game folder. Never delete without Shagg's say-so.

**Delivery and git**

- The PC project folder is not a git repo. Keep the repo in your workspace, commit locally as
  `Shagg <343598286+Shagg88@users.noreply.github.com>` with the Co-Authored-By and Claude-Session trailers. Never push.
- Stage each delivery in a new outputs folder (create it with `mkdir || exit`, because names collide), diff against the
  PC's copy first, and use an mtime guard on files others may edit.

## 5. Recompiler and boot

Every function must be found, and anything the codegen guesses wrong fails late and strangely. Fix the boot with hints,
not hooks.

- **The hint loop.** Boot, read `[FATAL] unregistered function 0x...`, add `[entrypoint.functions]` hints, rebuild.
  Gears 3 went from 2 hints to 179 in a few boots (jobs 007-012) [V].
- **Vtable-only functions are the dangerous misses.** Scan the image's vtables (Gears3-Native\tools\scan_vtables.py)
  before the first boot. Callbacks handed to the kernel or XDK are the second source (Gears 2 notes/62).
- **Switch tables:** automatic detection misses some, and a missed table jumps into the middle of a function. Gears 3
  job 014 was a switch fix. The Judgment project found only 4 of 11 Bink jump-table entries automatically.
- **UE3 catch funclets** ending in `_CxxThrowException` need explicit `end` overrides, or discovery runs into the next
  funclet.
- **Match functions across games.** UE3 code moves between titles but keeps its shape. Gears 3 found most of its hook
  sites from Gears 2's with an n-gram matcher over normalised disassembly (notes/03; Gears3-Native\tools\analysis).
  Typical scores were 0.75-0.95. Match Judgment against **Gears 3**, the closer engine. Some functions changed too much
  to match (Gears 3 has no look-alike of Gears 2's RHIGetTextureMemoryStats); find those through their callers or their
  config strings instead.
- **Strings find code fast.** Ini key names are big-endian UTF-16 in the image ("MipFadeInSpeed%d" at 0x821D1A48 in
  Gears 3); the lis/addi pair that loads one leads to the code that reads it. Gears 3's texture-pool setup and its
  `-POOLSIZE=` override were found this way.
- **Title updates move every address.** Gears 3 notes/08 has an address mapper between the base disc and TU1
  (tools/xex_addr_map.py). Decide the release first (section 2).
- **Two build traps.** Ninja can link a stale object after a source rewrite; the Judgment project builds twice and fails
  if a generated file is newer than its object. A local syntax check doesn't catch link errors: a declaration whose
  signature or namespace differs from the definition compiles, then fails to link and deletes the exe the player was
  about to run. Before every build, grep each new call for its declaration.

## 6. Runtime and Unreal Engine 3

UE3 reads almost every setting from its cooked ini bundle, so many "patches" are better done as ini edits or as one hook
at the place a value is read. Memory is the one hard wall, and it is smaller than it looks.

**Memory** [S, SDK src/system/xmemory.cpp]

| Guest range | What | Counts against 512 MB? |
| --- | --- | --- |
| 0x00000000-0x7EFFFFFF | virtual heaps (NtAllocateVirtualMemory), 4 KB and 64 KB pages | No: separate host memory, about 2 GB |
| 0x80000000-0x9FFFFFFF | the XEX image | No |
| 0xA0000000 / 0xC0000000 / 0xE0000000 | three windows onto ONE 512 MB physical heap | **Yes**: this is the GPU-visible 512 MB |

- The cap can't be lifted in practice: the three windows fill the top of the 32-bit space, and GPU addressing assumes
  512 MB [G, strong]. Devkits' 1 GB was debug memory for tools; Canary ignores it [S].
- What fits is decided by how much physical memory the game leaves free. Measure it: Gears 3's
  `--gow3_memory_log_seconds` gauge logs physical used/free and the lowest free every 10 s.
- **Texture pool:** Gears 2 [V] 140 to 196 MB (hook at 0x8280D534) plus the stats function returning FALSE
  (0x8280DDC8). Together they fixed corrupted chapter-18 title text and slow sharpening, at +1.4% frame time. Gears 3
  reads PoolSize=145 then `-POOLSIZE=` from the command line in sub_829677A8; our hook sets 200 MB
  (`--gow3_texture_pool_mb`, 1-400).

**The config bundle**

- Every ini is in CookedXbox360\Coalesced_<lang>.bin: a big-endian count of files, then per file the sections, then per
  section the keys. FStrings are UTF-16 when the length is negative. Parser and writer: layer 2 config_bundle.cpp.
- An edited bundle must pass two checks, or the game shows the dirty-disc error: the start-up SHA-1 table in the image
  (Gears 3 had 9 entries) and the disc TOC (Xbox360TOC*.txt, size + MD5 per file). Gears 3's config_install.cpp updates
  both and shows the game an overlay folder; the game's files are never written.
- Gears 3 defaults worth knowing [V]: every TEXTUREGROUP has `MipFilter=point`, so the game uses no trilinear
  filtering. Mip fade-in takes 0.3-2.0 s per level (MipFadeInSpeed0/1). MaxAnisotropy=4, bSmoothFrameRate=TRUE with
  MaxSmoothedFrameRate 62, and TaskPerfTracking=TRUE pointing at Epic's database [G: compiled out on the 360].
- **The disc has no sharper textures to unlock** [V, Gears 3, 248 textures in one level]: the cooker stripped every mip
  above MaxLODSize (the 2048 mips are flagged unused with size -1). Raising caps adds nothing; only a texture pack
  replaced in our renderer could.

**Booting a map**

- UE3 builds its command line as "dummy " plus the title's launch data, so `--ue3_launch` puts text there:
  `GearGame_P?loadcheckpoint=0?chapter=1 -nostartupmovies` [V, Gears 3]. Map names are the *_P packages.

**Shaders on the disc** [V, Gears 3 notes/04]

- 111,284 unique shaders (7,861 VS, 103,423 PS) in 1,851 packages, plus 226 in GlobalShaderCache-Xbox360.bin. Material
  shaders sit in each package's SeekFreeShaderCache export, compressed twice (package chunk, then an inner XMemCompress
  chunk).
- libmspack's LZX needs the nopad fix (Gears3-Native\tools\lzxd-xmem-nopad.patch): XMemCompress writes no pad byte
  after an odd-length uncompressed block, and without the fix ~24% of GearGame.xxx's shaders are lost.
- What the draw uses: in Gears 3, only the code part of a shader record matches (offset and size from its header); in
  Gears 2, the whole region does. Gears 2 matched 87% of drawn PS and 81% of VS against its disc [V, job 367].

## 7. The renderer

You don't write a renderer: you use Gears 2's and tell them what Judgment needs. Most of your renderer work is finding
Judgment's settings and sending good evidence.

**How to work with it**

- **Canary and Edge first** (Gears 2 decision D36). When a picture bug has no quick fix, compare that area with Xenia
  Canary (e6cd2e4bc) and Edge, list where ours differs, and port their mechanism behind a switch. Allow at most one
  round of diagnostics. The chapter-10 rainbow took about 15 overnight jobs before Canary's readback_resolve fixed it.
- The emulator is a reference, not the truth. Where both are wrong or we can do better, do better behind a switch, and
  label it as a deliberate deviation in the logs (D32).
- **Per-game settings without forking layer 0:** set them in your app only while they still have their compiled-in
  default, so the command line always wins. Gears 3 sets `gow2_upscale_object_constants=7`,
  `gow2_upscale_camera_constant=5`, `gow2_renderer_memexport=true`, `gow2_renderer_resolution_scale_threshold=1120`,
  `gow2_upscale_scene_formats=2` (its lit scene is 10:10:10:2, not FP16). Judgment will need its own values; find them
  with the same probes (Gears 3 job 055).
- Things that bit us: k_16_16 render targets resolved to textures must store UNorm values, not EDRAM bits (blurred
  characters otherwise). Emulator-era game patches can break a native pass (Gears 2's motion-blur patch left only
  fog). Frames shown through the 1280x720 front buffer land in a corner at 2x output. The SDK's always-on achievement
  toast counted as a second presented surface and doubled the NVIDIA overlay's fps (fix 0054: build it lazily).

**Shader stutter: what we learned on 10-23** (Gears3-Native\notes\20)

| Step | First visit | Lesson |
| --- | --- | --- |
| HLSL via DXC (morning) | 92.8 s frozen in 8.5 min | Compiling on the draw path freezes the game |
| Parallel compile | worst hitch 5.6 s | Helps, not enough |
| DXBC, synchronous pipelines | stuck in frame 2 | wait_ms=-1 forces a pipeline per draw; never do this |
| DXBC + async (Canary style, wait 0) | no freezes, heavy pop-in | Draws skip until their pipeline exists |
| + load holds and spinner (0063-0067) | ch10 38 skipped draws, short holds | Pausing during loads gives the workers the CPU (82 vs 235 ms per pipeline) |
| Second visit (pipelines recorded, 0058) | 0-41 skipped, steady | The recorded set is pre-created at boot |

- The real cost is the driver turning DXBC into GPU code: 2.37 ms per KB [V]. Gears 3 pipelines are 83-115 KB, so
  100-360 ms each when cold. Canary's DXBC is the same size; Canary is smooth because it skips draws, uses 75% of cores
  and pre-creates its stored pipelines. Its "Pipeline creation took 1 ms" line times only the queueing [S].
- The driver keeps each shader stage: a new pipeline whose stages were built before costs 0.1 ms [V]. Building each
  pixel shader once before first use is enough.
- UE3 creates its shader objects lazily at first use, so hooking the shader constructors gives no lead time (Gears 2
  0066: 0-1 fires in a cold boot) [V].
- Prebuilding every shader of each package as it opens made things **worse**. About 10,000 shaders were offered per
  load at 155-220 ms each, which starved the game: 106 hitches against 2 with it off [V, job 368b]. It's off now (0069:
  3 workers, newest package first, variant learning). Don't re-enable it in Judgment until a version beats "off".
- 12 workers beat 6 (main hold 1749 vs 2631 ms) [V].
- The wait_ms=-1 warning is for play. Gears 2's fixed-clock picture-comparison jobs (373-375b) use
  `--gow2_renderer_pipeline_wait_ms=-1` on purpose, so every frame is complete and comparable. Those jobs are not wrong.

**Textures**

- Texture uploads on the CPU caused the remaining 30-40 ms "textures" hitches; GPU loads for BC, cubes and 3D textures
  exist (0021/0025/0026). Gears 2 notes/66 lists the Canary/Edge texture rules ours still lacks (T1-T21), each a known
  correctness gap.
- Vulkan is in progress behind `--gow2_renderer_api=vulkan`; D3D12 stays the default. Stages V1 (device, textures,
  uploads), V2 (shaders, pipelines, draws) and V4 (compute) are done, gn_vktest passes 48/48 on the RTX 3080 Ti, and the
  first Vulkan frame matches D3D12 (FLIP 0.0015, job 373) [V]. It is slow so far: about 65 ms per frame against 18, and
  a 70 s chapter load (job 374) [V]. V3, the window, is Gears 2's job 375b: the SDK rebuilt with Vulkan on, plus SDK
  patch 33.

## 8. Performance and frame pacing

Measure first, then fix the biggest cost, one switch at a time, A/B in the same job. Gears 2 went from about 55 ms to
6.5-14.7 ms per frame uncapped at 1x this way (notes/52, notes/60) [V].

- **Every game run is also a CPU sample.** Run the built-in sampling profiler for 10 s from swap 600 and record the CPU
  time of every other process (tools/cpu-report.ps1). Windows Defender (~1.2 cores) and the Settings window's blurred
  backdrop (dwm ~0.45 core) once made our renderer look 1.5x slower [V].
- **Spin-waits are the first win.** The D3D library's GPU wait (`cctpl`/`cctpm` priority hints are empty when
  recompiled) spins a core at 100%; sleeping until our GPU side writes guest memory frees ~0.7 core at 30 fps at no
  cost uncapped (Gears 2 job 225; Gears 3 hook at 0x82AF98EC). The render thread's GPU-query wait is the same: Gears 3
  mode 3 sleeps after 32 quick polls and saved 0.07 core at the same fps. The first version, which slept after 64
  polls, failed in Gears 2 (job 227) because many different queries are polled back to back.
- Don't look a setting up by name inside a hot loop: a per-poll cvar query cost 6% of the render thread (read it once
  per 1024 calls) [V].
- Cheap build wins, measured on Gears 2: `cr_as_local` -7%, `non_volatile_as_local` -5%. Still open: the SDK's profiling
  counter on every indirect call (2.8% of the busiest thread, shared cache line), no `-march` (no MOVBE for the
  byte-swapped loads), `volatile` guest loads.
- **Diagnostics can cost more than the work.** A scaled-miss log took 45% of the renderer thread at 2x; turning it off
  took ch10 2x from 21.6 to 12.7 ms. Keep expensive logs off by default.
- **The frame limit:** UE3's GetMaxTickRate returns MaxSmoothedFrameRate (62) when bSmoothFrameRate is on. A hook after
  that load (Gears 3 0x8253D9B0, `--gow3_max_fps`, 0 = game, -1 = none) is a settings override, not Canary's nop.
  Above 30 fps also needs `--gow2_gpu_vsync=false`, because the game asks for every second vertical blank.
- **Frame pacing is still open** (Shagg's idea #3, Gears 2 designs it). Shagg saw "a small stutter every half second".
  It doesn't show in the 600-swap summary or in the 40 ms hitch report. Gears 3's `--gow3_tick_log` (job 115) times
  each engine tick and logs the spacing of slow ticks, so a fixed rhythm shows as a steady gap with a small spread.

## 9. Graphics options and community patches

The rule is faithful by default and enhancements opt-in. Shagg may turn a proven fix on by default (the texture pool,
trilinear, the GPU-wait sleep); a picture-changing preference stays off.

**Community patches** (xenia-canary/game-patches; Judgment has a base-disc and a TU4 file)

- Disassemble every patch site first: most patches are emulator workarounds (AO, DoF, motion blur, bloom off) or cheap
  hacks (Unlock FPS by nopping a clamp, 21:9 rendering at 1280x550). A native renderer must draw those effects
  correctly instead (Gears 2 notes/40).
- Port the real ones as **manifest hooks, not memory writes**. Many "be8/be16" patches are immediates inside
  instructions, and recompiled code can't be patched in memory. Gears 3 ported Canary's 11-write 21:9 patch as 5 hooks
  and a data write, then made the field of view exact Hor+ instead of Canary's fixed factor (job 054).
- Credit the author in the option's description.

**What players get now** (Gears 2 notes/64, Gears 3 game_patches.cpp)

| Option | How | Default |
| --- | --- | --- |
| Resolution scale 2x/3x | renderer; Gears 3 keeps targets of pitch 1120 or less at 1x | launcher choice |
| SMAA, DLSS, FSR, XeSS | renderer; vendor SDKs as separate binaries, built in only when present | off |
| HDR | SDK patch 24 + renderer | off |
| 16x anisotropic | hook on the anisotropy read (Gears 3 0x82969614) | off |
| Unlocked frame rate | GetMaxTickRate hook + vsync off | launcher |
| 21:9 ultrawide, exact Hor+ | FOV and scene-height hooks | off |
| Draw distance | ini edit MaxDrawDistanceScale (Gears 3 job 051) | off |
| Bigger texture pool | hook at the pool size read | on (196 / 200 MB) |
| Trilinear mips, fast mip fade | built-in ini edits, Gears 3 job 113b, untested in play | on |

- **Broad hardware** (D35): the port ships for many PCs. A feature the test PC can't run (FSR 4, DLSS frame generation)
  goes in only when it enables itself where supported, falls back cleanly, and is marked untested.
- DryreL's Jacinto multipatch (Nexus gearsofwar3/mods/27) targets a leaked PC build of Gears 3. Its boss and campaign
  fixes don't apply to the finished 360 game; its engine-setting ideas do (trilinear, mip fade, streaming tuning). Its
  HD texture pack is PC-format and would need texture replacement in our renderer.

## 10. Testing and diagnostics

Shagg's play test is the real test; scripted runs are for measurements he can't make. A test is only worth running if
its pass criteria are written before it runs.

- **Play tests beat scripted routes** past the first minute of a level. Routes kept missing the trigger (a cutscene),
  timed out, and cost 6+ minutes a round. Give Shagg one thing to watch for. Read the log afterwards and say in plain
  English what it showed.
- **Before reading any run, confirm it is the build you think it is:** the exe time in the job log, the build's log
  line, the options in args.txt. Gears 2 rule R10: check that our picture was on screen.
- **Each launcher run writes its own folder** (Gears2-Native\out\play-logs\gears3-<time>: gow3.log, args.txt, shots).
  Read the frame-time lines (every 600 swaps), the HITCH reports (each frame over 40 ms, with the part that took the
  time: record, poll, resolve, textures, pipelines), the hold lines and the skipped-draw counts.
- **Write the pass criteria first** (Gears 2 job 153 is the model): what counts as a pass, the counterfactual run with
  the fix off, the same route both ways. Only a pass makes something a default.
- **Every job tests several things.** Long jobs are fine, but anything that pays for a rebuild carries every other ready
  test with it. A job's first lines stop it when its key precondition is wrong (HEAD, source has the change, game not
  running).
- **A local syntax check is not a build.** Cross-compile what you can (clang `-fsyntax-only` against the SDK headers,
  mingw for Windows-only branches), and still expect link errors only the PC build finds.
- Tools worth having from day one: the hitch report (`--gow2_hitch_report_ms=40`), frame times
  (`--gow2_frame_times=600`), draw stats, frame captures with draw lists, the memory gauge, the tick log, FLIP image
  comparison on the GPU.
- Reference runs: Shagg's Xenia Canary (canary_experimental@534aeb64e, in C:\Emulation\Emulators\Xenia Manager) can be
  read for its logs and stored pipeline counts. Gears 3's storage held 1824 shaders and 1210 pipelines [V].

## 11. Do and don't

The rules are Shagg's; the mistakes are ours. Both cost real time when ignored.

**Always**

- Commit locally only, as Shagg with the trailers. Never push or post anything public.
- Keep game data out of git: game files, extracted or generated code, shader dumps, translated shaders, image dumps.
- Treat the game folder, saves and other projects' folders as read-only. Ask before deleting anything.
- Mark [V] / [S] / [G]. Say plainly when you were wrong.
- Put every behaviour change behind a switch, measure it on and off, and make it a default only after it passes.
- Use the PC's hardware fully for builds and tests (all but 1-2 threads) when the PC is yours. Never run while Shagg
  plays.
- Credit borrowed code (BSD notices kept; our code is MIT) and patch authors.

**Never**

- Never change layer 0 yourself. Send Gears 2 a patch with the evidence.
- Never edit another chat's shared file (Gears2-Native\out\extra-args.txt belongs to the launcher and Gears 2).
- Never ship a list of shaders or pipelines built from game files (Shagg 18:20). The player's own PC builds them.
- Never force synchronous pipeline creation on the draw path (the 16:22 freeze).
- Never guess a size or limit high when a measurement is possible: a memory overrun doesn't fail at once, it crashes
  mid-chapter.

**Mistakes we made**

| Mistake | Cost | Do instead |
| --- | --- | --- |
| 15 overnight diagnostic jobs on the ch10 rainbow | a night | Compare with Canary first, port its mechanism |
| Waited on a persistent runner's pid as if it were a job | 60 min stuck (G3 job 108) | Check the queue heartbeat's `running=` |
| Appended to a stale read-log copy | lost a line | Stage a fresh copy right before writing |
| Reused an outputs folder name | wrong files delivered | `mkdir` a new name, stop if it exists |
| "Canary pipelines take a few ms" | a wrong plan for an afternoon | Read the source before trusting a log line |
| Hooked UE3 shader constructors for prebuild | no lead time at all | Check when the engine really creates objects |
| Prebuilt every shader of each package | first visit worse (106 vs 2 hitches) | Measure volume and cost before building in bulk |
| Moved to a title update late (Gears 2) | about a day re-finding hooks | Pick the release first |
| Built a measurement-only exe on its own | a build Shagg didn't want | Ride along with the next real change |
| Diagnostic log left on by default | 45% of the renderer thread at 2x | Expensive logs default off |

## 12. Joining the project: the Judgment checklist

Judgment joins as a third member of the same project: one renderer, one mailbox, one job queue rule, one PC lock.
Nothing is copied that can be shared.

**Set-up, in order**

- [ ] Make the folder **Documents\GearsJ-Native**, laid out like Gears3-Native: src\layer2_ue3 (copy Gears 3's, the
      superset), src\layer3_gowj, jobs\queue, notes, tools, third_party\sdk.txt. Keep its git repo in your workspace,
      not on the PC.
- [ ] **Shared renderer:** in CMakeLists.txt set `GN_LAYER0_HOME` to `${CMAKE_SOURCE_DIR}/../Gears2-Native`, as Gears 3
      does. Never keep your own copy of layer 0 (Gears 3 deleted its stale one in job 114).
- [ ] **SDK:** snapshot Gears 2's current install into third_party (Gears 3 job 114 is the template); the toolchain is
      read from Gears2-Native\third_party.
- [ ] **Mailbox:** your inbox is Documents\Gears-Mailbox\to-judgment, with its own read-log.txt. You send by writing into
      to-gears2 and to-gears3. Introduce yourself to both in your first hour.
- [ ] **Job queue:** copy Gears 3's tools\queue_driver.ps1, tools\sharedpc\pclock.ps1 and env.ps1 (sccache with your own
      folder and a port other than Gears 3's 4237), and a Run-Jobs.bat for Shagg to start it.
- [ ] **Yield to both games:** before taking the PC lock, wait while Gears 2's queue is busy, as Gears 3 does. The lock
      itself keeps you and Gears 3 apart.
- [ ] **Launcher:** Shagg plays from the shared launcher. Adding Judgment to it is Gears 2's change; ask them by mail.

**Hook sites to find first** (match against Gears 3, the closer engine; Gears3-Native\notes\03 has the method)

| Role | Gears 2 | Gears 3 | Why |
| --- | --- | --- | --- |
| Anisotropy read (16x AF) | 0x8280E7FC | 0x82969614 | Validates your matcher: Canary's Judgment patch has the answer |
| Tiling flag (GUseTilingMode) and its start-up store | 0x83122EA4 / 0x82806A44 (stores 1) | 0x83428B98 / 0x82960B94 (stores 0) | Must run untiled |
| D3D GPU-wait step (cctpl to cctpm) | Gears 2 jobs 225/227 | 0x82AF98EC to 0x82AF991C | Frees a core |
| Render thread's query poll loop | 0x82807588 | 0x82961994 (head) | Second spin to sleep |
| Launch-data reader ("dummy " prefix) | 0x8280F0B0 | XamLoaderGetLaunchData caller, string 0x821CB974 | Chapter boot |
| Texture pool setup (PoolSize read) | 0x8280D4C8 (0x8280D534) | 0x829677A8 (hook 0x829678C0) | Bigger pool |
| GetMaxTickRate (MaxSmoothedFrameRate load) | (Gears 2 notes/56) | 0x8253D9B0 | Frame limit, tick log |
| Start-up SHA-1 table and disc TOC | job 257 (40 entries) | job 050 (9 entries) | Ini edits pass the dirty-disc check |
| Shader create (XDK header inits) | | PS 0x82AF2628, VS 0x82AF2910 | Only if prebuild returns |

Then find Judgment's own renderer settings (scene colour format, upscaler constant registers, scale threshold) with the
probes Gears 3 used (job 055), and send them to Gears 2 as per-game defaults.

## 13. Open problems (as of 10-23, 22:45)

These are unsolved for Gears 2 and 3 and will be for Judgment too. Check the mailbox for progress before starting on one.

| Problem | Where it stands | Owner |
| --- | --- | --- |
| First-visit shader pop-in and short holds | Async + holds + recorded pipelines work; prebuild at package open made it worse and is off (0069 reworks it) | Gears 2 |
| Even frame pacing ("stutter every half second") | Approved by Shagg (idea #3); Gears 3's tick log (job 115) will measure the rhythm | Gears 2 designs, Gears 3 measures |
| Texture pool size | Gears 3 at 200 MB untested in play; memory gauge built; size the pool as lowest free minus a margin | each game |
| Trilinear mips and fast mip fade | built into Gears 3 (113b), not yet seen by Shagg; Gears 2 mirrors after his test | Gears 3 first |
| Streaming budget scaling (Gears 2's stats-off fix) | Gears 3's copy not found; parked unless the gauge shows the streamer cutting its budget | Gears 3 |
| Vulkan renderer | stage V1 done (textures, uploads, readbacks); shaders and draws next; window needs an SDK rebuild | Gears 2 |
| Texture correctness gaps vs Canary/Edge | listed in Gears 2 notes/66 (T1-T21, E1-E7) | Gears 2 |
| Gears 3 chapter-22 squad and NPC vertex explosions | memexport and resolve readback suspects (notes/15, 18) | Gears 3 |
| Keyboard and mouse | the Judgment project has a working approach (notes/40 lesson 6) | not started |

When you solve something that applies to all games, write it in your notes with its measurement and mail both chats:
Shagg's rule since 20:19 on 10-23 is that every fix applies to every Gears game.
