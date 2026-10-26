# 36 - Stage 3 plan: our draws (D26)

Stage 3 makes our renderer draw. The biggest piece is shader translation (Xenos microcode -> HLSL/DXIL); everything
else (pipeline state, resources from Stage 1, render targets and resolves from Stage 2) feeds it. Built from the bottom,
each step checkable on the PC or in the container before the next starts. Estimates set before starting (D19 practice).

| Step | What | Check | Estimate | Started | Done | Actual |
|---|---|---|---|---|---|---|
| S3.1 | Shader corpus: every distinct VS/PS on the regression routes (microcode + the SDK's disassembly as reference), layer 0 `--gow2_shader_dump_dir` | index of shaders, counts | 0.25 d | 2026-10-21 02:25 | 2026-10-21 02:43 | job 074; 1,442 shaders (245 vs, 1,197 ps) on 10 routes; one never-analysed vs missing, re-collected in job 078 |
| S3.2 | Our microcode decoder + disassembler (layer 0, adapted from rexglue/Xenia BSD with attribution) | (a) coverage: every corpus shader decodes with no unknown instruction or field, failures reported per route; (b) our text == the SDK's disassembly for every shader; (c) cross-check against Xenia Canary's current disassembler source, each difference recorded as ours or theirs; (d) feature census (below) | 1.5-2 d | 2026-10-21 02:30 | 2026-10-21 02:51 | ~20 min (helper agent) + job 077: 1,442/1,442 byte-identical on the PC and in the container, 0 decoder issues, 0 Canary-rule differences (notes/37) |
| S3.3 | Translator to HLSL (vertex fetch, texture fetch, ALU vector/scalar, control flow, exports) | every corpus shader compiles with DXC (SM 6.0) | 4-6 d | | | |
| S3.4 | Shader-translator check (D21 item 4): run ours next to the SDK's on recorded inputs, compare outputs | per-shader output equality within tolerance | 2-3 d | | | |
| S3.4b | Our clears and resolves replace the emulated ones (the Stage 2 work that is mapped but not yet done): clear = EDRAM tile-range clear on every aliasing target; resolves by (address, format, size) | per-resolve comparison with the emulator's readback (buffer dump tool) | 2-3 d | | | |
| S3.5 | Our D3D12 device: pipeline state from registers, resources from the Stage 1 table, render targets from Stage 2 (EDRAM aliasing, clears, resolves), behind `--gow2_renderer=ours` | first frame; then the per-draw diff against the emulator on calm routes | 6-10 d | | | |

Total estimate 16-24 days (S3.4b added 2026-10-21 02:40: Stage 2 is mapped, not done; its replacement work is planned here instead of surfacing later). The per-draw diff of S3.5 is where "no unexplained differences" (D23) becomes the pass test.

## Prior art and licences (Brief 8, 2026-10-21)

| Project | What | Licence | Use |
|---|---|---|---|
| XenosRecomp (hedge-dev) | Xenos -> HLSL ahead of time, DXC to DXIL/SPIR-V, lookup by XXH3 of the microcode | MIT | may borrow with its notice |
| Xenia Canary / Edge | Xenos -> DXBC / SPIR-V at runtime, the most complete | BSD-3 | may borrow with its notice (rexglue's copy is what S3.2 adapts) |
| LostOdysseyRecomp | Xenos -> HLSL SM6 at runtime | GPLv3 | ideas only |
| UnleashedRecomp guest device | D3D hooks only, whole-resource upload at unlock | GPLv3 | ideas only |

**Design points for S3.3 taken from it:**
- XenosRecomp's gaps (its README; its code does support a0/aL-relative operands): dynamic register indexing listed as
  missing, no integer loop constants (loops run aL
  from 0 step 1, ignoring the loop constant's start and step), no memexport, no point size, texture LOD/filter overrides
  missing, relies on the game's embedded shader reflection. We need all of these handled from the microcode alone.
- Xenia weak spots to not copy (fetch translator): 16-bit float vertex data decoded as PC halves, texture fetch offsets
  applied at mip 0, getWeights ignoring mip/filters, fetch-constant gradient exponent not applied, max/min NaN order.
- Constant layout: the guest's 512 float4 constants are shared between the two stages (XenosRecomp copies 256 for
  vertex and 224 for pixel shaders - Sonic's split, not a rule), plus bool and loop constants; we keep the raw guest
  arrays in constant buffers and let the register map decide the split.
- Control flow: a structured translation where the control flow allows it; a pc-driven loop/switch fallback.
- Resource tracking: our hooks plus a per-page write watch remain the plan (Unleashed manages with hooks only because
  Sonic's engine always locks; Gears writes outside the lock cycle - Stage 1 found page-watch-only changes).

## Feature census (S3.2 d): decides how much of XenosRecomp we can borrow

Count, over the corpus, the shaders that use: loop constants (and whether any loop could start other than 0 or step
other than 1 - that is a runtime constant, so record which shaders loop at all); a0- or aL-relative addressing; vertex
fetch of 16-bit float data; texture fetch with LOD or filter overrides or non-zero offsets; memexport; cube maps;
predication and jumps/calls. These are XenosRecomp's gaps.

## Stage 3 traps checklist

1. Half-pitch 4x MSAA clear (3-vertex rect, fixed shader pair): an EDRAM tile-range clear applied to every target that
   aliases those tiles - not an MSAA draw.
2. 24-point dummy draw groups (oPos 0,0,0,1, cover no pixels): skip drawing, keep any register/state changes, log them;
   check oPos is always (0,0,0,1).
3. Aliased EDRAM base: the 7e3 scene buffer and its blendable 16-bit view share one base - one storage, two views, made
   explicit. Not Xenia's "drop the higher slot" heuristic.
4. Half-res buffers: AO (x occlusion, y depth, temporal history - needs last frame's AO and reprojection data) and
   velocity (characters only, 0.5 = no motion, needs the previous frame's matrices - an early test that constants pass
   through correctly).

## S3.5 milestones, by render pass (user, 2026-10-21 03:50)

S3.5 is split by pass so progress is measurable and a failure points at one pass. Each milestone passes when its draws
match the emulator (per-draw diff on the calm routes; resolve outputs compared with the buffer dump tool).

| Milestone | Pass(es) | Why this order | Pass test |
|---|---|---|---|
| M1 | Depth prepass and shadow maps | depth only: the simplest case (no colour formats, no blending) | scene depth and shadow-map resolves match |
| M2 | Base pass | the 7e3 scene colour buffer | HDR resolve matches |
| M3 | Lights | the blendable 16-bit view of the same EDRAM range: the aliasing case | HDR resolve after lighting matches |
| M4 | AO and velocity | the two half-res buffers (AO needs last frame's history; velocity needs the previous frame's matrices) | both half-res resolves match |
| M5 | Post-processing and UI | bloom chain, tone map, UI | front buffer matches |
| M6 | Full frame | everything, across all chapters | FLIP against the emulator on every route; no unexplained differences (D23) |

Clears (the half-pitch 4x MSAA rect) and the 24-point dummy draws are needed from M1 on.
