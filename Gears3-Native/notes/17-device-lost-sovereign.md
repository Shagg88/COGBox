# 17 - Device lost in Act 1 (CNV Sovereign), reproducible spot - 2026-10-23

## Facts [V]
- Two sessions, same place, ~6 min in, ~1 min after the "SICKBAY" corridor banner (location banner "CNV Sovereign
  (Raven's Nest Class Warship)"; next shot: corridor past the Santiago locker, credits running):
  1. Gears2-Native play log gears3-20261023-002508 (Gears 2 launcher; uncapped, aniso16): 00:31:13 swap ~33,400,
     "device failed (0x887A0007)" (DXGI_ERROR_DEVICE_RESET).
  2. Gears3-Native play log 20261023-004506 (Play-Scout, 62 fps cap): 00:51:22 swap ~20,500,
     "device failed (0x887A0005) DEVICE_REMOVED"; the title ran on with nothing drawn until closed.
- Before it (log 2): GPU 11-12 ms/frame of our work, frame intervals p99 98-115 ms, max 137-184 ms for ~20 s
  (normal: p99 ~18-40 ms); "sampler table" waits 300/300 frames, 7.4-7.8 ms/frame (= the CPU waiting for the GPU,
  NextFrame's 3-pool sampler heap, i.e. GPU-bound); no SHADER-FAIL, no new render targets after 00:49:02.
- No removal reason / DRED in either log (release build).

## Reading [G]
One GPU workload in that area grows slow, then hangs past the Windows TDR limit (2 s) - a shader that loops for a very
long time (Xenos loop / predicated jump with garbage constants), a huge draw (bad index count / vertex explosion), or
a long transfer. Not a CPU crash.

## Next
- Patch 0006 (DRED breadcrumbs + page faults, Gears3-Native\patches\to-gears2): the next loss logs the unfinished
  command list op and any faulting address.
- Test at that spot (needs Shagg's go): one run with 0006, one with --gow2_gpu=sdk (does the emulator path hang too?).
- If a shader loop: log loop / jump counts of shaders first seen in that area; Canary caps loops (xenia ALU loop
  limits) - check how our translator bounds loops.

2026-10-23 01:00 [V]: session 20261023-005759 in the same corridor shows vertex explosions (shot 3600) and a black
Marcus on the way to this spot - candidate cause chain: exploded mesh -> huge triangles -> GPU stall -> TDR [G].

## Launcher session gears3-20261023-002508 (Gears2-Native\out\play-logs, read-only) [V]
- Same build, Gears 2 launcher options: 2x, threshold 1120, memexport, SMAA, aniso16, uncapped (--gow3_max_fps=-1),
  own shader cache dir (Gears2-Native\out\gears3-shader-cache), --gow2_draw_stats=600.
- Path: Nightmare prologue (sepia dream, shots 18000 / 24000) -> Sovereign; hang at swap ~33,400 (00:31:13), the
  frame before it took 3,186 ms (TDR). Both hang sessions = ~6 min of play at the same place, uncapped or 62 fps.
- draw_stats every 600 frames: indices per frame 0.7-5.5 M in gameplay, 2.6 M at frame 33000 just before the hang -
  no sampled frame with an outsized index count (but sampling is 1 frame in 600).
- "unknown (N colour)" stale tiles ~129-147k per 300 frames throughout (patch 0005's case).
