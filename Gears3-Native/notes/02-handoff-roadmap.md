# Gears 3 Native Port: Handoff and Roadmap

Oct 21, 2026 · @John

Everything the Gears of War 2 native port learned (jobs 001-178, Oct 20-21 2026), packed so a Gears 3 chat can start on day one instead of rediscovering it. Gears 2 now runs on its own D3D12 renderer with no GPU emulation, all 29 chapters at a locked 30 fps, about 65 fps uncapped in the heaviest chapter.

## Starter prompt (paste into the Gears 3 chat)

> You are starting a native PC port of Gears of War 3 (Xbox 360) for Shagg, built the same way as his Gears of War 2 native port: a rexglue static recompile of the game code plus our own D3D12 renderer that replaces the emulator's GPU path. Read this whole handoff doc first; it holds the rules, the architecture, the lessons and the roadmap. Use the xbox360-static-recomp skill too.
>
> For now you work only in your own workspace: Shagg will give you the Gears 3 disc image (or `default.xex`) and a zip of the Gears 2 project's `src`, `notes`, `tests` and `patches` folders. Do not use his PC, and do not touch the Gears 2 project or its job queue. When you later get PC access, Gears 3 lives in its own folder (`Documents\Gears3-Native`) with its own job queue, and you agree with Shagg when it may run so the two games never test at the same time.
>
> First tasks, in order: (1) check whether a Gears 3 recomp already exists and tell Shagg what it covers; (2) pin the exact version (title ID, media ID, XEX version, title update, SHA-1); (3) extract and run rexglue codegen until it is clean; (4) identify the XDK and UE3 versions and map which Gears 2 layers carry over (`layer0_xenos` almost entirely, `layer1`/`layer2` with new addresses, a new `layer3_gow3`); (5) locate by pattern the Gears 3 equivalents of the Gears 2 hooks listed in this doc; (6) write a plan in `notes/00-decisions.md` style with pass criteria before any build.
>
> Rules: plain English; mark what is verified and what is guessed; say plainly when a theory is wrong; commit locally only, never push; never commit game data; never delete without Shagg's say-so; BSD/MIT code keeps its notices, GPL is ideas only, "Hollow" is never used. Xenia Canary and Edge are the reference; the real Xbox 360 is the higher truth.

## Ground rules Shagg works by
- **Stack tests (Shagg, 2026-10-22 12:17):** put as many independent tests as possible into each job (different chapters, options, checks) instead of one thing per job. Keep each *change being judged* separable (its own run, or an on/off pair) so results still point at one cause.
- **Sending files to the PC:** stage every send under a NEW folder name (outputs/u<time>/...) and read the file back afterwards (size + hash). 2026-10-22: re-sending under the same staging path sometimes delivered an older cached copy - job 027 built a stale renderer file and two notes files were stale.
- **Messages for the Gears 2 chat** go in a code block (a box with a copy button), so Shagg can copy and paste them in one click (asked 2026-10-22 10:49).

These held for the whole Gears 2 project; carry them over unchanged.

- **Git:** commit locally only; never push or share unless Shagg explicitly says so. Author `Shagg <343598286+Shagg88@users.noreply.github.com>`, with the Co-Authored-By and Claude-Session trailers.
- **No game data in git:** no `game/`, `out/`, `generated/`, `build/`, `third_party/`, `*.iso`, `*.xex`, ucode dumps, translated shaders.
- **Read-only and no deletes:** the game folder and `C:\Emulation\Games\Xbox 360\Games` are read-only. Never delete anything without Shagg's say-so (copy instead). Only `out\harness` may be pruned.
- **Never type into other apps.** No scheduled tasks; use background timers.
- **Reporting:** plain English, results not narration. Separate verified \[V\] from guessed \[G\]. Don't oversell. Say plainly when a theory was wrong. Double-check everything.
- **Licences:** our code is MIT. BSD/MIT code we borrow keeps its notices. GPL projects are ideas only. "Hollow" (a fan port built from a leaked 2008 build) is never used, not even for reference code or files.
- **References:** Xenia Canary and Edge (both BSD-3) are the reference for exact GPU rules. Real Xbox 360 behaviour is the higher truth. Where we can beat both, do it behind a switch. No real-hardware testing.
- **Hardware:** use Shagg's CPU and GPU fully (RTX 3080 Ti, 16 logical cores), jobs at High priority.
- **Process:** every job has a fail-fast check; run preflight before upload; write pass criteria before each stage; keep moving. If a setting fixes what Shagg sees, ship it first. One clear thing for him to watch for in his own play tests.
- **When Shagg is away:** keep going continuously; use SendUserMessage for anything he must read.
- **No helper agents** on the project itself (Gears 2 decision D33); the research chat is used for outside research.


## Sharing the PC with Gears 2 (agreed with the Gears 2 chat, 2026-10-21 21:01)

1. Own folder (`Documents\Gears3-Native`) and own queue (`jobs\queue`, started with `Run-Jobs.bat`). Never write into
   `Documents\Gears2-Native` or its queue. Its toolchain (`third_party`) is used read-only (`tools\env.ps1`).
2. Below Normal priority for every Gears 3 job (the driver sets it; child processes inherit it), so Gears 2's builds,
   tests and frame-time measurements are not disturbed. sccache uses its own folder and port (4237).
3. No Gears 3 game runs while Gears 2 tests run. Running the game waits for a gap the Gears 2 chat leaves on request,
   or an agreed time. Extraction, codegen, builds and analysis may run any time.
4. Since 2026-10-22 06:51 (D15) rules 2-3 work through the shared lock, tools\sharedpc\pclock.ps1: game runs,
   benchmarks, full builds and SDK rebuilds take the lock (each job ~20 min or less, turn-taking, status.txt);
   High priority only while holding it. Everything else runs Below Normal without it.

## What Gears 2 built, and what carries over

The game's PowerPC code is statically recompiled with rexglue (Xenia-derived runtime). Everything graphics-side is ours: our own GPU system, our own PM4 command processor, and our own D3D12 renderer that replaces Xenia's GPU emulation entirely.

| Layer (CMake library) | What it holds | Carries to Gears 3? |
| --- | --- | --- |
| `src/layer0_xenos` | Xbox 360 GPU core: PM4 processor, our GPU system (ring, registers, interrupts, vsync), D3D12 backend, renderer (render-target/EDRAM cache, texture cache, shader translator ucode to HLSL to DXIL via DXC), GPU-memory watch, profiler, frame capture, game-free dev tools | Almost all, unchanged |
| `src/layer1_xdk7645` | Hooks and knowledge tied to the D3D library version (XDK 7645) | Re-check: Gears 3 likely uses a later XDK; function addresses change |
| `src/layer2_ue3` | Unreal Engine 3 knowledge (e.g. 16\_16 velocity targets, query polling) | Mostly; Gears 3 is a later UE3 build |
| `src/layer3_gow2` | Gears 2 only: manifest hooks, game patches (texture pool 196 MB, stats-off, Black Shading Fix, query-poll yield), chapter boot | No: write a new `layer3_gow3` |

**How a frame flows now (all defaults on):**

1. The game's threads write the ring buffer, as on a 360.
2. **GPU Commands thread** (our GPU system): parses PM4, keeps the register file, runs shader loads (cached), writes ZPD query reports, and queues items.
3. **GPU Render thread** (`--gow2_pm4_split`): runs the queued draws in order with its own register copy: ownership/EDRAM transfers, textures, constants, vertex/index data, state, then backend calls.
4. **GPU Recording thread** (`--gow2_renderer_threaded_recording`): replays our D3D12 command-list calls onto the real list.
5. Pipeline creation runs on 14 worker threads; our presenter shows our own image (no front-buffer copy).

**Key renderer design decisions (Gears 2 D29 and later):**

- Host render targets keyed by (EDRAM base, storage class, pitch, MSAA); EDRAM ownership tracked per tile, with transfers between targets when ownership changes.
- Resolves write into a GPU mirror of guest memory; textures load and untile on the GPU (compute).
- Occlusion queries: the game sees the fake "visible" count by default. Our real answers arrived about 3 frames late and made distant surfaces flicker.
- The game runs untiled (patch at `0x82806A44` in Gears 2) instead of emulating predicated tiling.
- Takeover was done in steps, each with a switch and a screenshot check: shadow first (both draw, compare), then our image on screen, then our resolves, queries, our PM4, our swap, and finally our own GPU system with no SDK GPU code loaded.

## Status (2026-10-22 10:20)

| Roadmap step | State | Where |
|---|---|---|
| 1 Pin and extract | done: base disc XEX (TU not used), XDK 20500; disc unpacked | notes/01, D7 |
| 2 Recompile that boots | done: title, menus, chapters on the stock backend. 179 function hints, one jump-table override. Save FATAL ('Invalid UTF-8') fixed by Gears 2 SDK patch 30 (our find), job 023 | notes/05 |
| 3 Harness | done: run.ps1, direct chapter loading (31 chapters), scripted input, screenshots, chapter_sweep.ps1 (not run yet), Play.bat | notes/06 |
| 4 Untiled / pool | done: untiled by default, no hook; pool = -POOLSIZE= launch option, patch only on symptoms | notes/07 |
| 5 Shadow mode | done for the title (job 019): every resolve matches the stock backend except the one we then fixed | notes/05 |
| 6 Takeover | **in progress**: full takeover runs (Gears 2's layer 0). Title screen matches stock (job 021), chapter 0 cutscene drawn (job 022), menus drawn (job 023). Two layer 0 fixes: stacked-3D GPU load (colour-grading table), resolve-only view targets. Next: new campaign from the menus (job 024), a gameplay frame compared in shadow mode | notes/05 |
| 7 Chapter sweep | not yet - after one chapter plays well on ours (paired with stock reference runs) | notes/06 |
| 8 Speed | Gears 2's speed defaults already on; not profiled for Gears 3 yet | - |
| 9 Extras | not yet (resolution/4K, AA, HDR, frame-rate unlock, ultrawide; split-screen route known from Gears 2) | notes/07 |
| Shaders | all 111,284 disc shaders translate and compile (job 006) | notes/04 |
| Toolkit | own SDK copy = Gears 2's after their job 228 (patches 25-30), job 023 | D16 |

## Roadmap for Gears 3

Gears 2 needed Stages 0-6 to build the renderer from nothing. Gears 3 starts with that renderer, so its route is shorter: boot on the stock emulator path first (the reference), then switch our renderer on and fix the differences.

1. **Pin and extract.** Exact version: title ID, media ID, XEX version, title update, SHA-1 of `default.xex` (Gears 2 used title 4D53082D, media 63AF211E, version 0.0.0.5). Check that nobody has already shipped a port (KiltedKernel and GloriousEggroll were listed on Gears 3 in Oct 2026).
2. **Full recompile that boots** on the pinned rexglue with the stock Xenia GPU backend. Fix unresolved calls with manifest hints; give UE3 catch funclets explicit `end` overrides.
3. **Harness before bugs:** chapter boot from the command line (Gears 2: `GearGame_P?loadcheckpoint=0?chapter=N -nostartupmovies -BENCHMARK -FIXEDSEED`), scripted pad input, frame times, screenshots every N frames, a chapter sweep. Gears 3's launch-data format must be found again.
4. **Untiled and game patches:** find Gears 3's predicated-tiling switch (Gears 2: the Black Shading Fix at `0x82806A44`) and the texture-pool size (Gears 2: pool at `0x8280D4C8`, raised from 140 to 196 MB).
5. **Our renderer in shadow mode:** both draw, compare per draw and per frame. New shader ucode, formats, texture fetch modes and EDRAM uses show up here. Expect new tessellation or memexport cases.
6. **Takeover in the same steps as Gears 2:** our image, our resolves, queries (fake count), our PM4, our swap, our GPU system. Each step has a switch and a screenshot check against the emulator.
7. **Full chapter sweep** against the emulator; fix per chapter, not per sweep.
8. **Speed defaults on** (split, recording thread, vertex reuse, shader-load cache). Profile the heaviest chapter.
9. **Stage 7 extras** (the Gears 2 plan, in order): resolution/4K, AA, HDR, frame-rate unlock, ultrawide.

**Where Gears 2 lost the most time (avoid these):**

- Chasing visual bugs through the emulator's own bugs. The emulator is a reference, not the truth: compare with Canary's patches applied.
- Re-uploading a job under the same name: the queue skipped it silently. Always use a new job number.
- Stale builds: `Copy-Item` kept upload timestamps, so ninja skipped changed files. Touch every copied file.
  (Gears 3 re-learned it with robocopy in job 015: robocopy keeps source timestamps by default - use /COPY:DA.)
- Scripted pad routes past the first minute of a chapter: they miss triggers. Shagg's own short play tests found more.
- A threaded renderer that deferred work past a fence: it hung after one frame (job 173). Any deferred guest write must be published at once.

## Hard-won lessons

Each was found by measurement in Gears 2; most will recur in Gears 3 because the engine and GPU are the same family.

| Symptom | Cause | Fix |
| --- | --- | --- |
| Title text corrupted, textures slow to sharpen (ch18) | Texture pool too small: the streamer kept defragmenting and moving textures mid-use | Pool 140 to 196 MB (hook at the PoolSize read), streamer told it never runs short (stats function returns 0) |
| Flat white water (ch19 Hive) | A texture fetch whose dimension didn't match the bound texture read white | A mismatched fetch reads zero, like Canary's null view |
| Distant surfaces flicker | Our real occlusion answers arrived about 3 frames late, so objects coming into view vanished | Give the game the fake "visible" count, as the emulator does |
| Characters blurred as if moving fast | 16\_16 velocity targets resolved as EDRAM fixed-point bits instead of UNorm | Store UNorm(value); UE3 writes v\*0.5+0.5 |
| An emulator-era game patch ruined the native final pass (fog only) | The patch assumed the emulator's pipeline | Implement the option inside our pass instead |
| ch10 GPU stall | Two barriers per draw around guest-memory uploads | Small vertex ranges read straight from the upload ring (no barrier) |
| Threaded renderer hung after one frame | A fence write sat in an unpublished queue block while the game waited for it | Deferred guest writes are published immediately |
| Rare input crash | Guest threads poll XInput concurrently | A lock in the SDK's InputSystem (our SDK patch) |
| Build succeeded locally, failed to link on the PC | `-fsyntax-only` cross-compiles don't catch signature or namespace mismatches | Grep each changed call for its declaration before building |
| Screenshots "differ" after a speed change | Uncapped and threaded runs land in a few timing variants (animation only) | Compare against every known variant, and look at a diff image before calling it a regression |
| GPU much slower than expected (now) | NVIDIA has no shader-written stencil reference, so each depth transfer between render targets takes 9 passes | Open: transfer only the stencil bits the game uses, or fewer transfers (in progress) |

## Speed: what worked and what didn't

Gears 2 chapter 10 (about 5,000 draws a frame) went from about 17 fps to about 65 fps uncapped in one day; the game itself was never the limit, our one translation thread was. In GPU-light scenes (chapter 2, \~105 fps) the GPU is now the limit, and 77% of its time is EDRAM transfers between render targets.

| Change | Effect (ch10 uncapped) | Status |
| --- | --- | --- |
| Texture pool fix + GPU stall removal | about 17 to 45 fps | On |
| rdtsc timers, PM4 reader without a division, BuildRuns scratch, memoized MarkWritten | 23.4 to 22.3 ms | On |
| Recording thread (D3D12 calls replayed on another core) | about 7% | On |
| Shader-load cache (97% of \~2,800 loads a frame are repeats) + one-pass constants | 21.1 to 20.0 ms | On |
| PM4 / renderer split (command parsing and renderer on separate cores; fences and the read pointer written when the renderer gets there) | 20.6 to 18.3 ms | On |
| Vertex reuse within a release epoch (82% of vertex bytes are repeats) | 18.3 to 16.7 ms with the split | On |
| Shader-compile poll skip, vertex memo | 16.7 to 15.4 ms | On |
| Copy threads for guest-memory uploads | 0 to slightly worse: wake-ups and \~70 sync points a frame ate the gain | Off |
| Hash-based vertex dedupe | 0: hashing reads the same cold bytes as copying | Off |
| Index reuse, game query-poll yield, shared VS/PS constants, GPU time by category | Being measured (jobs 177-178) | Switches |

**Rules that made it safe:**

- A **release point** is anything that tells the game "the GPU got here": a fence memory write, the ring read pointer, an interrupt, a scratch-register write-back, a failing wait. Between two release points the game cannot legally rewrite memory an unfinished draw uses, so copies can be skipped or deferred inside that window.
- Every speed change ships behind a switch, is A/B tested in one job, and must keep 30 fps capped screenshots byte-identical (capped runs are deterministic with `-BENCHMARK -FIXEDSEED`).
- Canary and Edge record on a single thread; their speed comes from removing stalls and redundant work. Multi-core recording is new ground.

## Tooling, the job queue and the test process

**Toolchain (Gears 2 D13, all portable in `third_party`):** LLVM/clang 23.1.4, CMake 4.4.4, Ninja 1.13.2, MSVC 14.51 libraries (VS 18.11.1), Windows SDK 10.0.26100, sccache 0.18.0. rexglue pinned to `nightly-20261002-bd833a2a`, with every SDK change kept in `patches/rexglue-sdk.diff`, each commented. DXC for shaders. No emulator GPU code in the shipping build.

**The job queue (how Claude works on the PC without typing into it):**

- A driver script (`tools/queue_driver.ps1`, High priority) runs `jobs/queue/NNN-name.ps1` one after another, oldest name first. A job is picked once its last line is `# END-OF-JOB` and it is 3 s old. Logs go to `jobs/queue/logs`, finished scripts to `done`. `stop.flag` stops the driver after the current job; there is no way to abort a running job.
- Claude uploads with the remote-device commit tool into `jobs/queue/pending-NNN/`, files first and the script last, then verifies with a directory listing.
- **A script name runs once.** A re-upload under the same name goes straight to `done`. Always use a new number.
- **Touch copied files** after `Copy-Item` (it keeps upload times, so ninja skips them). Every job: copy, touch, build, unit tests, commit, runs, summary lines in the log.
- Before every upload, compare against a mirror of the PC repo (`preflight`) so no stale file ships.
- Write pass criteria in the notes before the job runs, and record the result after.

**Test process that worked:**

- Chapter boot + scripted pad input + `--gow2_frame_times` + screenshots every 300-600 frames; capped runs are deterministic, uncapped and threaded runs land in a few timing variants.
- A 29-chapter sweep (60 s each, about 32 minutes) after big changes: frames, errors, normal exit, shots compared by hash, then diff images for anything that differs.
- Our sampling profiler (`--gow2_profile=S`): per-thread CPU, self and inclusive time, callers of the top functions, allocator frames skipped. GPU time per category from timestamps (job 177).
- Shagg's own play tests: one `Play-TestN.bat` per test, one thing to watch for, logs read afterwards.
- Notes per topic (`notes/NN-topic.md`) and one decisions log (`00-decisions.md`) as the single source of truth.

## What the Gears 3 chat can do now, without the PC

All of this runs in the chat's own workspace from the XEX (or ISO) Shagg gives it; nothing builds or runs the game yet.

- [ ] Check the port scene for an existing Gears 3 recomp (GitHub topics `rexglue`, the Recompendium, the ReXGlue Discord) and note what it covers.
- [ ] Unpack the disc (`xdvdfs unpack`), decrypt and decompress `default.xex` with rexglue, record title ID, media ID, version, title update and SHA-1.
- [ ] Identify the XDK version (compare with Gears 2's 7645) and the UE3 build; list the D3D library functions our `layer1` hooks would need at new addresses.
- [ ] Run `rexglue init` and `rexglue codegen` against a first manifest; collect the unresolved-call list and add hints until codegen is clean.
- [ ] Find the UE3 catch funclets ending in `_CxxThrowException` and give them `end` overrides.
- [ ] Find by pattern (from Gears 2's addresses and their surrounding code): the predicated-tiling switch, the texture pool size and stats function, the render thread's query-poll function, the launch-data / chapter-select path.
- [ ] Plan `src/layer3_gow3` and the Gears 3 manifest; list which Gears 2 patches have Gears 3 equivalents.
- [ ] Draft the harness: the chapter list and launch command line, the scripted input routes.
- [ ] Read Shagg's Gears 2 notes (`00-decisions.md`, `28-native-renderer-roadmap.md`, `51-stage6-plan.md`, `52-speed-plan.md`) once he shares a copy.

**What to ask Shagg for:** a zip of the Gears 2 project's `src`, `notes`, `tests` and `patches` folders (no game files in them), and the Gears 3 disc image. The Gears 3 project gets its own folder (for example `Documents\Gears3-Native`) and, once it uses the PC, its own job-queue folder; the two games never run test jobs at the same time.

## Gears 3 risks and open questions

- **Later XDK and UE3 build:** every `layer1`/`layer3` address changes; some D3D behaviours may differ (new formats, memexport use, more tessellation).
- **Heavier rendering:** more draws per frame than Gears 2's \~5,000 in busy scenes, and more GPU effects. The speed work matters even more; the EDRAM-transfer GPU cost seen in Gears 2 will likely be worse.
- **Unknown shader features:** the translator covers every ucode construct Gears 2 uses (2,474 shaders); Gears 3 may use ones it hasn't met. Run a full shader-corpus pass early, as Gears 2 did.
- **Multiplayer, Horde, Beast and co-op:** Gears 2's Stage 6 still has these open (plus movies, menus, cutscenes and window behaviour); expect the same list for Gears 3.
- **Title update choice:** pick one exact version and build only for it.
- **Existing ports:** if a Gears 3 port is already far along, decide with Shagg whether to contribute instead.
- **Open:** whether Gears 3's render thread also spins on query polls (Gears 2 does, about 45% of a core). Answered: no
  predicated tiling by default (D10); the D3D built-in shaders are identical to XDK 7645's; all disc shaders translate.

## After Gears 3 (Shagg, 2026-10-22 10:22)
Next project: **Gears of War: Judgment** (2013, Xbox 360 only - no PC version), not Gears 1 (which has PC versions).
Closest to Gears 3 technically (later UE3, newer XDK), so layer 0, the SDK patches and Gears 3's notes should carry
over well. Before starting: check the public Judgment recompiles (OverkillLabs3/gears-judgement-recomp,
GrizzlyOne95/gears-judgement-recomp) - how far they are; contribute or go our own way.

## 2026-10-22 14:47 - user preference: avoid game patches
Shagg: "I'd like to avoid patches as much as possible, especially if they're hacky and not important." The Canary
"Unlock FPS" patch (base file: 0x8253D9B0 skip the object's tick-rate load -> default 0.0 = uncapped; 0x82AF66BC
presentation interval 2 -> 1) was written as an off-by-default switch and then removed unadded. A frame-rate unlock, if
ever, is done properly (engine-side, measured), not as a patch. Prefer renderer-side improvements (resolution scale,
SMAA) over game-code patches.

## 2026-10-22 16:12 - user preference: no full chapter sweeps
Shagg: "No full chapter sweep. Nothing ever good comes from it." Test chapters chosen for a reason (a bug seen, a stock
pair) instead; long-session checks on one chapter are fine (job 035).

## Local syntax check of layer-0 renderer files (17:25, after job 040's build failure)
clang++ --target=x86_64-w64-mingw32 -fsyntax-only with: the PC's patched SDK headers staged from
third_party/rexglue-install-llvm23-rwdi-20261022-1000/include/rex/graphics (first), /root/ext/rexglue includes and
thirdparty (fmt, spdlog, xxHash, tomlplusplus, simde, cli11, tracy), mingw libstdc++ (g++-mingw-w64-x86-64-posix), and a
shim dir (case symlinks ObjBase.h; empty DXProgrammableCapture.h / dxcapi.h / DxbcConverter.h). Remaining errors are
environment only (d3d12_provider.h, FLAGS_present_hdr). Run it before every delivery of renderer code.

## 2026-10-24 09:18 - Shagg: check on jobs every 5 minutes
"Remember to always check on jobs every 5 minutes." Whenever a Gears 3 job is queued or running, check its log every
5 minutes (one pending check at a time), and tell Shagg the result as soon as it is in. Mailbox checks while idle stay
at 30-60 minutes.

## 2026-10-24 15:39 - Shagg: jobs wait for the lock only, every 15 s
"Your job should always be looking for the open lock every 15 seconds. That's what I have the other chats doing."
Jobs no longer wait for Gears 2's queue to be empty (the old G2Busy loop, 30 s polls, 60-90 min limit); they go
straight to Take-PcLock with -PollSeconds 15. Job 123 was already in the old wait when this came in (a running job
can't be changed); 124 on use the new rule.

## 2026-10-24 19:44 - Shagg: check the mail every 4 minutes (19:46: every 3 minutes)
"You should be checking the mail every 4 minutes." Done with a chain of one-shot scheduled checks (send_later, 3 min since 19:46),
each scheduling the next first; each check also looks at our running / queued job.

## 2026-10-24 20:05 - Shagg: 2 or 3 more Vulkan tries, then back burner
"Let's do 2 or 3 more tries/jobs/tests on Vulkan. If it doesn't work, let's switch to D3D12 and improve all three
games outside of the vulkan issue." (via Gears 2 mail 20:06). Gears 2 counts the tries across all games: 1 = measure
inputs (our job 133 on patch 0125), 2 = a fix patch, 3 = one follow-up. Then D3D12 work. One question per job.

## 2026-10-24 20:34 - Shagg: Vulkan parked; launcher first, then D3D12 optimisation
"Okay let's park it." (via Gears 2 mail 20:35). No more Vulkan jobs (134b was already queued and runs once).
Next: launcher items across all three games (Shagg's 10 from 09:48 + existing options), then D3D12 speed. Gears 3
launcher state mailed 20:40 (20261024-204000-gears3-launcher-state-vs-list.md). Vulkan findings: notes/26.
