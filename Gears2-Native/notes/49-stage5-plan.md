# 49 - Stage 5: the game's GPU state, all of it ours (plan, 2026-10-21 12:30)

Written by me (D33: no helpers). Roadmap notes/28 Stage 5 was written for the older plan of hooking the XDK D3D
setters. What it means in today's architecture is stated here first.

## 1. Where we are

- The game's own D3D library (recompiled) writes PM4 packets into the ring. The SDK's command processor parses them into
  its register file and, at every draw/copy/swap, calls our observer. Since job 128 (`--gow2_renderer=takeover`) the
  SDK executes nothing (patch 19): our renderer draws, resolves, answers queries and supplies the image.
- So the emulator still does two things for the picture: **(a)** PM4 parsing / register tracking, which we read, and
  **(b)** presenting (gamma ramp, scaling, window).

## 2. What Stage 5 is now

**5A - coverage proof (no takeover needed, starts now).** Every GPU register the game writes that can affect output is
read by our renderer, or is shown not to matter. Tool: `--gow2_state_coverage=N` (layer 0, `state_coverage.cpp`):
registers 0x2000-0x3FFF that change or hold non-zero values during a run, minus our renderer's read list (57 registers
from pipeline_state.h / resolve.h / queries.h / shadow_renderer.cpp). Each one left over is either implemented or
written down with the reason it does not affect output (e.g. a counter, a debug register, a performance hint).

**5B - our own command processor.** Parse the ring's PM4 ourselves (layer 0): register writes, draws, events (ZPD,
SHD/fences, swaps), WAIT_REG_MEM, interrupts. Then nothing of the SDK's GPU command processing is needed for Gears 2.
This is what "the emulator receives nothing" means here, and it is the gate to Stage 6 (build without the emulator's
GPU code). Fences/sync (Stage 4 items 8-9) land here naturally: we write the fence values ourselves.

**5C - our presenter.** Our swap chain shows our front buffer with the game's gamma ramp (DC_LUT registers) and
scaling. Removes the last emulator piece in the picture path.

## 3. Pass criteria (set before the work)

- 5A: on ch02 and ch10, zero registers left that change during play and are neither read by us nor documented as
  output-neutral (with the reason).
- 5B: the takeover runs with our command processor on and the SDK's off for 80 s of ch02 and 150 s of ch10: no hang,
  same picture (screenshot difference no larger than run-to-run), same frame pacing (30 fps).
- 5C: screenshots through our presenter equal the SDK presenter's (gamma applied), window resize and fullscreen work.

## 4. Order and estimates (grounded in today's pace: a build+run round is ~2-5 min, a feature ~30-60 min of my work)

1. 5A tool + first report (job 129): now.
2. 5A triage of the list: depends on its length; most entries should be quick to classify.
3. 5B: the biggest piece (PM4 packet set used by Gears 2 is known from notes/32-35 and the SDK source) - several rounds.
4. 5C: one or two rounds.

## 5. 5A result (job 130, 2026-10-21 12:38) - checklist triaged, 5A PASSES with three follow-ups

Takeover runs, ch02 (2,100 frames, 2.27 M draws) and ch10 (2,700 frames, 10.9 M draws): 71 registers in 0x2000-0x3FFF
are in use; our renderer reads 47; **24 are not read, the same 24 in both chapters**. Each, with the reason
([V] verified in source/log, [I] inferred, [D] documented by Canary/SDK):

| Register(s) | What | Verdict |
|---|---|---|
| VGT_DRAW_INITIATOR, VGT_DMA_BASE, VGT_DMA_SIZE | the draw packet itself (type, count, index buffer) | **read, through OnDraw's arguments** (primitive, count, index buffer) [V] - not a gap |
| RB_HIZCONTROL | hierarchical-Z enable/compare | performance only; Canary ignores it [D] |
| VGT_MAX_VTX_INDX | index clamp (0xFFFF / 0xFFFFFF) | no effect on valid index data; Canary ignores it [I] |
| SQ_INTERPOLATOR_CNTL | param_shade (flat shading per interpolator) + sampling_pattern (centroid) | **follow-up 1**: Canary ignores param_shade (no flat shading) and uses sampling_pattern only for centroid with MSAA [V, pipeline_cache.cc 690-694]. We match the reference (copy 20 MATCH, job 124). Per D32 the hardware may differ (flat shading) - check when MSAA/quality work starts |
| PA_SC_AA_CONFIG (0 / 0xC003), PA_SC_AA_MASK (0xFFFF) | MSAA sample config / sample mask | Canary ignores both [V: no use in gpu/*.cc]; all samples on. **Follow-up 2** with real MSAA (Stage 7 AA) |
| VGT_HOS_CNTL, VGT_HOS_MAX/MIN_TESS_LEVEL, VGT_HOS_REUSE_DEPTH | tessellation | set once at start (1.0 / 1.0); ch02/ch10 have no patch draws. **Follow-up 3: ch18 and ch20 do (type 18 patches, census job 093)** - our renderer's handling of patch draws must be checked there |
| PA_SU_POINT_SIZE, PA_SU_POINT_MINMAX, PA_SU_LINE_CNTL, PA_SC_LINE_CNTL | point/line size | Gears 2's only points are the 1-vertex sync draws we skip as no-ops; no lines [V, census] |
| PA_CL_GB_{VERT,HORZ}_{CLIP,DISC}_ADJ | guard band | clipping-hardware detail; host clipping gives the same image [I] |
| SQ_VS_CONST (0x000FF000), SQ_PS_CONST (0x000FF100) | ALU constant split: VS base 0 / PS base 0x100 | set once to the standard split; **our renderer hard-codes exactly this** (kRegFloatConstVs 0x4000, kRegFloatConstPs 0x4400 = constant 256) [V] - add an assert later |
| VGT_VERTEX_REUSE_BLOCK_CNTL, VGT_OUT_DEALLOC_CNTL | vertex cache tuning | performance only [I] |

Pass criterion met: no register changes during play while being neither read nor documented. Open: follow-ups 1-3.
Next: 5B (our own command processor).

## 6. 5B phase 1 (built 2026-10-21, job 131): our PM4 processor next to the SDK's

- `xenos/pm4.h` + `pm4.cpp` (layer 0, standard library only): the packet set the SDK handles (type 0/1/2/3; ME_INIT,
  NOP, INTERRUPT, XE_SWAP, INDIRECT_BUFFER(_PFD), WAIT_REG_MEM, REG_RMW, REG_TO_MEM, MEM_WRITE, COND_WRITE, the
  EVENT_WRITE family, DRAW_INDX(_2), SET_CONSTANT(2), LOAD_ALU_CONSTANT, SET_SHADER_CONSTANTS, IM_LOAD(_IMMEDIATE),
  INVALIDATE_STATE, VIZ_QUERY, the SET_BIN_* predication, CONTEXT_UPDATE, WAIT_FOR_IDLE), re-expressed from the SDK's
  command_processor.cpp function by function. Phase 1 leaves memory writes, waits and interrupts to the SDK (counted).
- SDK patch 20 (`DrawObserver::OnPrimaryRange`): the observer gets each primary ring range just before the SDK runs it.
- `pm4_shadow.cpp`, `--gow2_pm4_shadow=N`: at every draw the SDK reports, our processor must have reached the same
  draw with identical registers 0x2000-0x5002 (context, control, all shader constants), the same draw initiator and
  the same vertex/pixel ucode. Draws the SDK drops before its observer (immediate indices, viz-query kills) are dropped
  the same way.
- Unit tests `tests/layer0_pm4_test.cpp` (3 cases, 30 checks, pass on Linux with Catch2): register writes, the
  write-one-register flag, SET_CONSTANT types, LOAD_ALU_CONSTANT from memory, IM_LOAD / IM_LOAD_IMMEDIATE, indirect
  buffers, DRAW_INDX / DRAW_INDX_2, predication, XE_SWAP, ring wrap, REG_RMW (registers & 0x1FFF, as the SDK) and
  COND_WRITE (memory polled with the endian in the address's low bits, as the SDK).
- Pass (phase 1): on ch02 every SDK draw compared and identical, none missing.

## 7. 5B phase 1 RESULT (job 131) and phase 2 (built 2026-10-21, job 132)

**Phase 1 passed on the first run (job 131, ch02 80 s, takeover):** 2,272,131 of 2,272,131 compared draws identical
(registers 0x2000-0x5002, initiator, both shaders' ucode); 0 missing, 0 left over; the first draw differed in 0
registers. Our processor parsed 32.8 M packets, 78 k indirect buffers, 1.66 M shader loads with 0 unknown opcodes and
0 errors. Build + 71 test cases passed.

**Phase 2 - our processor runs the ring** (`--gow2_pm4_execute`, SDK patch 21 `--gpu_external_pm4` + `GpuServices`):
- The SDK no longer parses the primary ring. Ours does, and does itself: register/constant writes (through the SDK's
  register file, so its side effects - gamma LUT, scratch write-back, coherency - stay as they were), indirect
  buffers, predication, memory writes (MEM_WRITE, REG_TO_MEM, COND_WRITE, EVENT_WRITE_SHD with the swap counter,
  EVENT_WRITE_EXT's faked extents), WAIT_REG_MEM (same compare, sleep/yield rule as the SDK), REG_RMW.
- Handed back to the SDK as single packets, because what they reach still lives there: draws (register writes +
  observer + patch-19 skip), IM_LOAD(_IMMEDIATE) (the Shader objects our renderer receives), EVENT_WRITE_ZPD (query
  reports + OnZpd), XE_SWAP (OnSwap + present), INTERRUPT (the title's callback). Predication is decided by us; the
  handed-back header has its predicate bit cleared.
- Unit test added (external mode): 4 cases, 41 checks pass on Linux.
- Pass for job 132: ch02 80 s in takeover with our processor running the ring: no hang, ~2,100 frames, our image shown,
  screenshot visually the same as job 128's.
- Still the SDK's after phase 2 (phase 3 / 5C / Stage 6): the register file storage and its side effects, Shader
  objects, query report fakes, the presenter, interrupt dispatch, the ring/read-pointer bookkeeping.

## 8. 5B phase 2 RESULT (job 132) and phase 3 (built 2026-10-21, job 133)

**Phase 2 passed (job 132, ch02 80 s, takeover + our processor running the ring):** ~2,100 frames, no hang, 0 errors,
34,482 memory words written by us, 82,211 waits; our image shown. The frame-600 screenshot differs from job 128's in
980 pixels (not yet explained; small enough to be timing - the input script lands on slightly different frames).

**Phase 3 - draws and shader loads are ours** (`--gow2_pm4_own_draws`, needs `--gow2_pm4_execute`; SDK patch 22
`GpuServices::register_file`):
- IM_LOAD / IM_LOAD_IMMEDIATE no longer go to the SDK. We keep the ucode words and hash them with XXH3-64 over the
  big-endian bytes (the same hash the SDK's `ucode_data_hash` uses, so shader-cache keys don't change).
- DRAW_INDX / DRAW_INDX_2 no longer go to the SDK. We write the initiator and DMA registers into the register file,
  drop the same draws the SDK drops (viz-query kill), build the index-buffer description and call
  `XenosDispatchOurDraw` - the observer hub then gives observers the ucode (`XenosUcodeSink::OnDrawUcode`) instead of
  SDK `Shader` objects, then `OnDraw`. Our renderer takes either.
- The SDK skipped draws and copies already (patch 19), so it loses nothing it was doing.
- Unit test: 5 cases, 49 checks pass on Linux (own mode: nothing handed back; dropped draw; registers; hash).
- Pass for job 133: ch02 80 s in takeover with own draws: no hang, ~2,100 frames, our renderer reports draws, the
  processor reports 0 draws/shader loads handed back, frame-600 screenshot visually the same as job 132's.
- Still the SDK's after phase 3: register file storage, ZPD query reports, swap/present, interrupt dispatch.

## 9. 5B phase 3 RESULT (job 133) and ZPD ours (built 2026-10-21, job 134)

**Phase 3 passed (job 133, ch02 80 s, takeover + execute + own draws):** 2,100 frames, no hang, 0 errors; handed
back to the SDK 4.84 M -> 0.90 M (draws and shader loads gone, as predicted); our renderer drew every draw (not drawn:
shader 0, pipeline 0); 2,100 of 2,100 frames shown. Frame-600 screenshot vs job 132: 557 pixels visibly different, all
on moving things (drone, debris); plus ~74 k pixels off by 1 level of 255 across the frame - not explained (guess:
time-based lighting; not verified).

**ZPD ours** (`--gow2_pm4_own_zpd`, needs execute; no SDK change): EVENT_WRITE_ZPD is no longer handed back. Our
processor writes the initiator back, reads RB_SAMPLE_COUNT_ADDR and tests the report for D3D's end marker (FF FF FE ED
in a ZPass or ZFail lane, as the SDK); `pm4_shadow.cpp` writes the report exactly as the SDK's fallback path did in
takeover (8 lanes cleared; an end gets query_occlusion_fake_sample_count in ZPass_A/Total_A) and hands the hub an
`ObservedZpd` (kBegin / kFake), so our renderer's answer mode works unchanged. Refused (stays with the SDK) when
occlusion_query_no_fake is on. Unit test: 6 cases, 57 checks (test proven to fail with a broken end test).
- Pass for job 134 (two ch02 80 s runs, A = job 133 settings, B = + own ZPD): B no hang, ~2,100 frames, "ZPD ours"
  equals the renderer's query count and its ends equal answered; handed back drops by the ZPD count; query numbers
  (per frame, answered, zero %) and draws per frame within run-to-run noise of A; frame-600 shot visually the same.
- Left with the SDK after this: register file storage, XE_SWAP (present), INTERRUPT, the ring bookkeeping.

## 10. ZPD RESULT (job 134) and 5C - our own presenter (started 2026-10-21)

**ZPD ours passed (job 134):** run B (own ZPD) 2,100 frames, 0 errors; ZPD ours 898,448 with 449,224 ends = the
renderer's 449,224 queries exactly; handed back 903,044 -> 4,200 (= 2,100 swaps + 2,100 interrupts); queries/frame
213.0 vs 213.3, draws/frame 1,063.2 vs 1,062.7, zero % 86.2 vs 86.2 (A vs B). Frame-600 shot of B is pixel-identical to
job 133's. Run A (job 133's exact settings) differs from job 133 by ~59.5 k faint pixels: two runs of the same build
differ that much, so job 133's ~74 k off-by-one pixels were run-to-run noise (closed).

**5C decision.** The SDK presenter has two halves: (1) window/swap-chain/overlay plumbing (`rex::ui::Presenter`,
generic app code, not GPU emulation) and (2) the swap itself in the command processor (`IssueSwap`: front buffer via the
SDK texture cache from guest memory, gamma ramp pass, FXAA option, `RefreshGuestOutput`). We keep (1) and replace (2):
our renderer applies the gamma ramp to its own front-buffer texture and hands the result to
`Presenter::RefreshGuestOutput`. That removes the 3.6 MB/frame guest-memory round trip and the last per-frame GPU-
emulation work, and is what Stage 6 needs. Steps:
- **5C-1 (job 135) present shadow, nothing on screen changes:** `xenos/display_gamma.h` tracks the gamma ramps from the
  DC_LUT_* register writes our PM4 processor runs (port of the SDK's WriteRegister cases; the ring carries them, not
  MMIO). `--gow2_present=shadow`: at each swap our renderer runs the SDK's gamma pass (apply_gamma table / PWL, as
  Canary's apply_gamma_*.xesli) on our front-buffer words (decoded as fetch constant 0 says: format, endian, swizzle)
  into an RGB10A2 image, reads it back and compares it with the SDK's presented image of the same frame (SDK patch 3
  frame readback; ours from the last 4 frames, best match, so the 1-frame display lag doesn't matter).
  Pass: no hang, ~2,100 frames, compared frames > 2,000, best match >= 99.9 % pixels identical on >= 99 % of compared
  frames (any differences reported as max channel difference).
- **5C-2:** shared D3D12 device (our backend on the SDK's provider device) and `--gow2_present=ours`: XE_SWAP ours,
  our gamma image written into the presenter's guest-output texture; frame shots from our image; guest-memory front
  buffer write off.

### 5C-1 RESULT (job 135) and 5C-2 (built 2026-10-21, job 136)

**5C-1 passed (job 135):** fetch constant 0 is k_8_8_8_8 (256-entry table path), endian 0, swizzle A0A; 2,094 frames
compared, 2,094 bit-identical (max channel difference 0) between our gamma image and the SDK's presented image. Our
image matched the one from 3 of our swaps earlier (our display lag plus the SDK readback delivery delay).

**5C-2 - our image goes to the window** (`--gow2_present=ours`; SDK patch 23 `GpuServices::swap_done`):
- Our backend runs on the presenter's D3D12 device and direct queue (`BackendOptions::external_device/_queue`), so our
  work and the presenter's are in queue order with no cross-device copy and no CPU wait.
- XE_SWAP is ours (`Environment::own_swap`): the hub's OnSwap (our renderer presents there), then `swap_done` (the
  SDK's profiler flip and swap counter, all ExecutePacketType3_XE_SWAP did besides IssueSwap). Only INTERRUPT is
  still handed back.
- Our renderer: the gamma pass (5C-1), then `Presenter::RefreshGuestOutput` with our refresher: the presenter's
  guest-output texture imported as a UAV (`Backend::ImportTexture`), our words unpacked into it (kPresentUnpackCs),
  transitioned back to its state, submitted before returning. No display readback, no guest-memory front buffer.
- Frame shots: our presented image read back and given to the hub (the SDK makes no readback without its swap).
  `--gow2_present_check_every=N`: every N frames the presenter's image is captured (CaptureGuestOutput, what the window
  paints) and compared with ours.
- Unit test: own swap (7 pm4 cases, 62 checks).
- Pass for job 136 (ch02 80 s, takeover + execute + own draws + own ZPD + present ours, check every 300): no hang,
  ~2,100 frames; handed back = 2,100 (interrupts only); presented = swaps in every window, refresh failed 0; screen
  checks >= 99.9% of pixels within 1 level on every check; frame-600 shot visually the same as job 135's.
