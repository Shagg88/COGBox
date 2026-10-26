# 50 - Takeover: our renderer's image on screen, then the emulator stops drawing (2026-10-21)

Owner: me (no helpers from 2026-10-21 11:20, user's decision). Every delivery is checked against the PC mirror
(`/home/claude/mirror` in the cloud workspace: src, notes, tests, patches, CMakeLists as on the PC) before upload, and
each job stops in its first lines if the feature's key counter is wrong.

## 1. Steps (each one job, each with pass criteria written before it runs)

| Step | What | Switch (default = today) | Pass |
|---|---|---|---|
| T1 | **Our image on screen.** At each swap our front-buffer resolve is written into guest memory (guest layout, same addressing as the colour compare) and the SDK is told the range changed (`Memory::TriggerPhysicalMemoryCallbacks`), so the presenter shows our pixels. The emulator still draws underneath. | `--gow2_renderer_display=emulator\|ours`, `_wait`, `_mark` | first-swap line says "match"; "shown" ~= swaps; the magenta mark is on the screenshot; the user sees a normal game |
| T2 | **Our resolves feed the game.** Every colour/depth resolve we do is written to guest memory, so textures the game reads back (shadow maps, AO, bloom...) come from us. | `--gow2_renderer_resolves_to_guest` | no visual change vs T1, no hang |
| T3 | **The emulator stops drawing.** SDK switch: skip draw execution in the command processor (packets still parsed, state still tracked, swaps still presented). | SDK `--gpu_skip_draws` (new patch) | game plays on our image alone; emulator GPU time ~0 |
| T4 | Sync/swap from Stage 4 items 8-9 (immediate fences once the emulator no longer reads guest memory; precise vblank). Draft: `patches` handover, section 4. | patch 17 switches | no hangs, same pacing, CPU core freed |

After T3 Stage 5 (state setters) can start for real; its field-coverage tool can be built during T2/T3.

## 2. T1 design (built 2026-10-21, job 121)

- `ResolveColour` remembers each frame's 32-bit colour copies by destination address (the R32_UINT words our resolve
  shader wrote, the rectangle, endian).
- `OnSwap` (called by the SDK right before `IssueSwap`): if a copy this frame went to the swap's front buffer
  (`frontbuffer_ptr & 0x1FFFFFFF`), read it back; after our Submit, take the newest finished readback, write each word
  `Swap32(word, endian)` at `front buffer + TiledOffset2D(x, y)` and trigger the physical-memory callbacks on that range
  (virtual 0xA0000000 + physical), so the SDK's shared memory and texture cache reload it for this swap.
- Default no stall: the image shown is 1-2 swaps old. `--gow2_renderer_display_wait=true` waits for our GPU each swap.
- Only 8888-family 32-bit front buffers; other formats, MSAA and 64-bit leave the emulator's image (counted as
  "no matching copy").
- Risks [I]: the global critical region taken on the GPU thread (deadlock would show as a hang - the job has a time
  limit); a game that presents a buffer it did not resolve this frame (loading screens, movies) keeps the emulator's
  image.

### T1 results so far
- Job 121: built; our image written for 295 of the first 300 swaps (lag 1.0). It stopped on a wrong check of mine
  (the very first boot swap has no copy) - fixed.
- Job 122: 300/300 swaps written, but the frame-600 screenshot is byte-identical to job 116's emulator image: no mark.
  Cause [V in SDK source]: `Memory::TriggerPhysicalMemoryCallbacks` only fires for watched pages, and the front
  buffer's pages are not watched after the emulator's resolve, so the SDK kept its own copy.
- Fix: **SDK patch 18** (`patches/sdk-patch18-observer-guest-write.diff`): `NotifyGuestRangeWrittenByObserver(phys,
  len)` -> `SharedMemory::MemoryInvalidationCallback(phys, len, exact)` on the D3D12 command processor, which
  invalidates GPU-written pages and fires the texture cache's watches. Job 123 rebuilds the SDK (1-15 + 18).

- **Job 123: T1 works.** SDK 1-15 + 18 built; 300/300 swaps shown; the frame-600 screenshot has the magenta mark
  (pixels (0,0)-(31,31) = 255,0,255) and is our image. One visible fault: **red and blue exchanged** (blue signs red).
  Cause [V]: our colour resolve's raw path ignored RB_COPY_DEST_INFO's swap bit (24); the reference's fast resolve
  shaders apply it by source format (Canary resolve.xesli 1011-1055). Job 116's copy 20 example proves it: ours
  00FF8A5E, guest 005E8AFF = bytes 0 and 2 exchanged. This is also the M2 front-buffer open item (53.7% bytes equal =
  R and B differ, G and A equal). Fix in builtin_shaders.h (raw path + swap: 8888 bytes 0/2, 2_10_10_10 fields 0/2,
  16_16_16_16 low halves of the two words); DXC 1.9 compiles it (cs_6_6, -HV 2021). Job 124 checks it.

- **Job 124: T1 done.** Copy 20 (the front buffer) MATCHES at frame 600, 3,686,400 of 3,686,400 bytes (it was 53.7%
  since M2); copy 9 still MATCHES; our image shown 300/300 swaps; screenshot correct.

## 2b. T2 design (built 2026-10-21, job 125)

- `--gow2_renderer_resolves_to_guest` (default off). After each colour/depth resolve we do, a compute pass
  (`kWordsToGuestCs`) writes the destination words into our GPU's guest-memory mirror at dest_base + TiledOffset2D,
  endian-swapped (the colour comparison's addressing, verified 100% on copies 9 and 20). The backend's guest buffer is
  now UAV-capable (`ComputeBindings::guest_u0`).
- `GuestWatch::MarkGpuWritten` bumps those pages and records "the mirror holds our content at this version". Texture
  loads skip re-uploading such pages from CPU memory and content validation counts them as changed. A CPU write ends it.
- Hybrid on purpose: resolves we don't do yet (MSAA colour, 8/16/128bpp destinations, unmodelled sources) still come
  from the emulator through `--readback_resolve=full`.
- Known limits [I]: textures loaded on the CPU path (block-compressed, cube, 3D, stacked, depth formats if not in the
  GPU untile) read CPU bytes, i.e. the emulator's - counted as "CPU-path loads that read our pages". Pages are whole
  4 KB, so a neighbour sharing a page with a resolve destination would read stale mirror bytes (resolve destinations
  are normally page-aligned).
- Pass: our resolves written every frame, pages kept > 0, no crash/device loss, copy 20 still MATCH at frame 600 with
  everything downstream reading our resolves.

- **Job 125: T2 runs** (built clean, 68 tests pass, no crash): our resolves written for 3,848 of 3,849 resolves in
  frames 1-300 and 6,134 of 6,134 in 301-600, none skipped. But "pages kept 0": with `--readback_resolve=full` the
  emulator's own readback write lands after our mark [I: it raises the memory callback], so the hand-over between its
  bytes and ours is mixed. Downstream copies changed (copy 20 100% -> 84.8% equal; copies 11-14, 17-19 moved), and the
  frame-600 screenshot differs from job 124's by more than 8 levels in only 4,089 of 921,600 pixels - consistent with
  our own (slightly different) HDR/bloom copies now feeding the tone map. Not chased further: in T3 the emulator writes
  nothing, so the mix cannot happen there, and T3 is the real test of T2.

## 2c. T3 design (built 2026-10-21, job 126)

- **SDK patch 19** (`patches/sdk-patch19-skip-draws.diff`): `--gpu_skip_draws` (default off). The D3D12 command
  processor returns from IssueDraw before doing anything for draws and copies (so no resolves, no EDRAM clears, no
  readback), but still analyses the active shaders (the observer reads their bindings) - packets are parsed, state
  tracked and every draw/copy still reaches our observer; swaps are still presented, from guest memory, where T1 puts
  our image.
- Run with T1 + T2 on and `--readback_resolve` off (nothing to read back).
- Pass: no crash/hang/device loss for 80 s of ch02; our image shown ~300/300 swaps; T2 "pages kept" > 0 (our resolves
  are what our textures read); the screenshot is the game, visually like job 124's.

- **Job 126: T3 runs - the emulator draws nothing and the game still plays on our renderer alone.** SDK 1-15+18+19
  built; 80 s of ch02 with no crash or hang, and 900 frames instead of ~700 in the same time (the emulator's GPU work is
  gone); our image shown 300/300 swaps; our resolves written for every resolve. **The frame-600 screenshot is the scene,
  correct colours, but blurred all over** (the HUD text is sharp).
  - Cause [I, strong]: 24:8 depth textures (formats 22/23: the scene depth copy 0 and the shadow-map copies 1-8) load
    on the texture cache's CPU path, which reads CPU memory - and with the emulator no longer resolving, CPU memory has
    no depth there (zeros). Depth-of-field then sees everything as out of focus. Until T3 the emulator's readback filled
    that memory, which hid it.
  - Second fault, mine: the T2 counters ("pages kept", "CPU-path loads of our pages") were printed after the summary
    reset the texture counters, so they always read 0 (jobs 125-126). Now printed before the reset, per 300 frames.
  - Fix (job 127): depth formats 22/23 load on the GPU path from the mirror (untile codes 14/15 = the CPU path's
    conversions: (n >> 8) / 16777215, and Float20e4To32 of n >> 8).

- **Job 127: T3 PASSES - Stage 3 complete (2026-10-21 12:15).** The emulator draws nothing; the game runs on our
  renderer alone, sharp (the user watched it live; frame-600 screenshot visually the same as job 124's - 2.2% of
  pixels differ by more than 8 levels, consistent with timing). Our textures read our resolves: 1.4-1.7 million pages
  kept per 300 frames, CPU-path loads of our pages 0. Our image shown 300/300 swaps after start-up. Speed: 2,100 frames
  in 80 s (~26 fps) vs ~700-900 with the emulator drawing too. No crash, no skipped resolve.

## 2d. Takeover preset (job 128)

`--gow2_renderer=takeover` = shadow renderer + `display=ours` + `resolves_to_guest` + `queries=answer` +
`compare_every=0`, and, only once our renderer has initialised, the SDK's `gpu_skip_draws` (patch 19) through
`rex::cvar::SetFlagByName`. If our renderer fails to start, the emulator keeps drawing. Pass for job 128: the TAKEOVER
line, our occlusion answers used (draws per frame fall as in job 120), image shown, pages kept > 0, no crash, sharp
screenshot.

## 3. What is NOT ours yet in T1

Everything the emulator computes and the game reads back (resolved textures, query answers unless `=answer`), and the
presenter itself (gamma ramp, scaling) - T1 replaces only the pixels of the final image.

## 4. Stage 4 sync/swap handover (from the Stage 4 helper, 2026-10-21 11:21; source-verified unless marked)

- Host present already runs independently (D3D12 presenter, tearing allowed): nothing to do there.
- Guest vblank is fixed 60 Hz only with `--vsync` on; with vsync off it runs at 1000 Hz (unlocks the game's pacing).
- Vblank thread waits with 1 ms sleeps and nothing raises the Windows timer resolution, so sleeps may be ~15.6 ms:
  a possible 30 fps jitter source [I, measure].
- `82977688` (the game's GPU-wait step) is a pure busy loop (its delay instructions compile to nothing): a core spent.
- Immediate fence write-back is unsafe until T3 (the emulator still reads guest memory after each fence).
- Draft SDK patch 17 (unbuilt, one known compile error: the new switch block sits inside the stock
  `swap_post_effect` definition in graphics_system.cpp): precise sleep, `--guest_vblank_fixed`, `--guest_vblank_precise`,
  `--wait_reg_mem_yield`, `--gpu_sync_log_seconds`. Kept in the cloud workspace with the handover; ships with T4.
