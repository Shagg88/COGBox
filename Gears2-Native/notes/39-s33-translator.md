# 39 - S3.3: Xenos -> HLSL translator (first version)

Layer 0 `xenos/hlsl.h`, `hlsl.cpp` (standard library only, MIT, semantics adapted from Xenia via rexglue BSD-3-Clause:
`interpreter.cpp` for ALU, control flow and vertex fetch; `dxbc_translator*.cpp` for texture fetch, position completion
and pixel parameter generation). Input is the S3.2 decoder's `xenos::ucode::Program`. Check tool: `tools/gn_hlsl_check`.
Labels: [V] verified (compiled / checked), [I] inferred, [G] guess.

## Result

| Corpus | Shaders | DXIL (vs/ps_6_6) | SPIR-V (vulkan1.3) | pc-loop control flow | Compiler warnings |
|---|---|---|---|---|---|
| job 081, all 29 chapters' openings + menu (headline) | **2,474** (328 vs, 2,146 ps) | **2,474 / 2,474** | **2,474 / 2,474** | 255 | 0 |
| job 074, 10 routes | 1,442 (245 vs, 1,197 ps) | 1,442 / 1,442 | 1,442 / 1,442 | 128 | 0 |

[V] in the container with DXC v1.9.2607 (0d3ee6b55) built from source, 2026-10-21. Includes vs-AB1E86137A0240E8 (the
copy/resolve vertex shader the SDK never analysed: no SDK disassembly, still translates and compiles) and the one
looping shader ps-8DCC9EF73F7C1B78. Vulkan descriptor sets/bindings checked in the SPIR-V disassembly of a sample
shader (sets 0/1/2 with the table's binding numbers). Not yet run on the PC (needs the release dxc.exe; job pending).

"Compiles" is the first gate only. Output correctness is S3.4 (the interpreter oracle on recorded inputs).

## Compile settings (both targets, one check)

| Target | Command |
|---|---|
| D3D12 (DXIL) | `dxc -T vs_6_6\|ps_6_6 -E main -HV 2021 -Gis -denorm ftz` |
| Vulkan (SPIR-V) | `dxc -T vs_6_6\|ps_6_6 -E main -HV 2021 -spirv -fspv-target-env=vulkan1.3` |

DXC pinned to release **v1.9.2607** on both sides: tag `v1.9.2607` = commit `0d3ee6b551b8fa768fbf825300ebab81047ef6a8`
(2026-07-28). The PC uses the official asset `dxc_2026_07_29.zip` ("dxcompiler.dll 1.9.0.5402 (0d3ee6b55-dirty)", job
079); the container builds the same commit from source with SPIRV-Tools `b707790a` as the tag pins (the release download
is blocked from the container). The release notes' link points at `a107ba613b346e96c71990729c1b763a50e7896b`
(2026-07-17, "[Cherry-Pick] moving spirv-* deps to specified commits", on the release-1.9.2607 branch); the tag and the
binary are 0d3ee6b55, which is what both sides use. (An earlier container build at main `c3a85328` was discarded.)
gn_hlsl_check quotes every DXC argument and prints both command lines for the first shader (PowerShell split a bare
`-fspv-target-env=vulkan1.3` at the dot on the PC, job 079).

`-Gis` (strict IEEE) and `-denorm ftz` are DXIL options; for SPIR-V, denormal behaviour is the device's
(`VK_KHR_shader_float_controls`: request flush-to-zero for fp32 where the device allows it - S3.5 item).
`precise` marks the legacy-multiply selects and the max/min selects on both targets.

## Binding contract (S3.5's device must follow this)

D3D12: everything in register space 0. Vulkan: `[[vk::binding(binding, set)]]`, binding numbers equal to the D3D12
register numbers so one table describes both.

| What | HLSL | D3D12 | Vulkan (set, binding) |
|---|---|---|---|
| System constants (below) | `cbuffer XeSystem` | b0 | (0, 0) |
| The stage's 256 float constants (device copies the SQ_VS_CONST / SQ_PS_CONST range) | `cbuffer XeFloatConstants { float4 xe_c[256]; }` | b1 | (0, 1) |
| Bool constants (256 bits) and loop constants (32) | `cbuffer XeBoolLoopConstants { uint4 xe_bool[2]; uint4 xe_loop[8]; }` | b2 | (0, 2) |
| Fetch constants (192 dwords: texture k at 6k..6k+5, vertex v at 2v..2v+1) | `cbuffer XeFetchConstants { uint4 xe_fetch[48]; }` | b3 | (0, 3) |
| Texture slot k, 2D (and 1D) | `Texture2D<float4> xe_tex_2d_k` | t(k) | (1, k) |
| Texture slot k, cube | `TextureCube<float4> xe_tex_cube_k` | t(32+k) | (1, 32+k) |
| Texture slot k, 3D | `Texture3D<float4> xe_tex_3d_k` | t(64+k) | (1, 64+k) |
| Guest physical memory (vertex fetch) | `ByteAddressBuffer xe_shared_memory` | t96 | (1, 96) |
| Sampler for slot k | `SamplerState xe_sampler_k` | s(k) | (2, k) |

Only the resources a shader uses are declared (Translation::texture_slots_*, uses_shared_memory).

System constants (`XeSystem`):

| Field | Meaning |
|---|---|
| `xe_sys0.x` flags | bit 0 W not reciprocal, 1 XY divided by W, 2 Z divided by W (PA_CL_VTE_CNTL), 3 primitive is polygonal, 4 primitive is a line, 5 alpha test on |
| `xe_sys0.y` | vertex index offset (added to SV_VertexID for r0.x) |
| `xe_sys0.z` | pixel shader interpolator mask (bit i: r_i comes from TEXCOORDi) |
| `xe_sys0.w` | pixel parameter generation: bit 31 on, bits 0-3 the register |
| `xe_ndc_scale`, `xe_ndc_offset` | guest-to-host position transform (Xenia's ndc_scale / ndc_offset) |
| `xe_color_exp_scale` | per render target 2^RB_COLOR_INFO.color_exp_bias |
| `xe_sys1.x`, `.y` | alpha test function (Xenos CompareFunction), reference (float bits) |

Interface: vertex output / pixel input `float4 interp[16] : TEXCOORD0..15` (Vulkan locations 0-15) plus SV_Position;
pixel outputs SV_Target0-3 and SV_Depth when the shader writes oDepth.

Device-side responsibilities (not in the shader): the texture fetch constant's swizzle (SRV / image view component
mapping), signed components (a signed view), filtering, addressing and border (sampler), gamma render-target encoding,
EDRAM, resolves, the 256-constant range per stage, index endianness and the vertex index offset.

## Design

- **Registers:** a local `float4 r[N]` (N = highest register used + 1; 64 when aL-relative addressing is used; at
  least 16 in pixel shaders for the interpolators). Plus `ps`, `p0`, `a0`, aL, the loop and call stacks (depth 4),
  the vfetch address, the register LOD and gradients.
- **Control flow:** shaders without loops, calls or jumps: straight-line inside a one-pass `do { } while (false)`, an
  executed exec_end is a `break`. Others: a pc-driven loop, `if (xe_pc == k) { ...; xe_pc = k + 1; }` per control
  flow instruction in order (consecutive instructions in one pass, backward jumps `continue`), with a guard of 65,536
  passes so a malformed shader cannot hang the GPU. Loops honour the loop constant's count, start and step (aL =
  iterator x step + start, clamped to [-256, 256]) - not XenosRecomp's 0/1.
- **ALU:** each slot reads all sources first, then computes vector and scalar results, then writes (the hardware
  issues both together). Rules from notes/38: legacy multiply (mul, mad, dp2add/3/4, muls, muls_prev, mulsc, dst),
  `mad` not fused, D3D9 max/min order, rcp/rsq/log/exp variants (plain / c clamps Inf / f zeroes Inf), `ps` not
  updated when the slot is predicated off, setp push/pop/inv/rstr/clr as rexglue's interpreter, kill as `discard`
  (demote to helper), maxa/maxas/maxasf set a0 (round or floor, clamped to [-256, 255] as rexglue's interpreter;
  Canary's DXBC clamps to [0, 255]).
- **Vertex fetch:** from the guest memory buffer: fetch constant address & 0x1FFFFFFC + index x stride + offset, words
  endian-swapped per the fetch constant, every vertex format (8888, 2_10_10_10, 10_11_11, 11_11_10, 16_16,
  16_16_16_16, 16_16_FLOAT, 16_16_16_16_FLOAT, 32, 32_32, 32_32_32_32, 32_FLOAT, 32_32_FLOAT, 32_32_32_FLOAT,
  32_32_32_32_FLOAT) with signed / normalised / integer / signed-RF modes and the exponent adjust; 16-bit floats in the
  360's extended-range half (exponent 31 is a normal exponent, no Inf/NaN, denormals flushed to a signed zero), exactly
  as rexglue's `xenos_half_to_float` - so for the 133 shaders fetching FMT_16_16_FLOAT, rexglue's interpreter is the more
  accurate S3.4 reference than Canary's GPU translators (which decode IEEE halves); vfetch_mini reuses
  the previous full fetch's address; missing components read 0 (Xenia).
- **Texture fetch:** 2D, cube (S/TC in 1..2 plus face back to a direction, as Xenia), 3D; unnormalised coordinates;
  offsets plus Xenia's 1.5/1024 texel epsilon at mip 0; Sample / SampleBias / SampleLevel (vertex shaders, or
  UseComputedLOD=false) / SampleGrad (register gradients); setTexLOD and setGradientH/V; getCompTexLOD via
  CalculateLevelOfDetail; after the fetch, the fetch constant's biased (2x-1) and gamma components and its exponent
  adjust.
- **Position:** Xenia's completion (1/W, XY/W, Z/W flags) then NDC scale and offset.
- **Pixel:** interpolators into r0-r15 per the mask, ps_param_gen (pixel XY floored, back-facing in X's sign, line in
  Z's sign), alpha test, colour exponent scale per target.

## Simplified or stubbed (S3.4 expects differences here)

Per shader (machine-readable: `<out_dir>/simplifications.csv`, hash,feature,note - written by gn_hlsl_check):

| Feature | Shaders (of 2,474) | Hashes |
|---|---|---|
| `max4_order` - max4 uses rexglue's >= priority; hardware order unresolved | 3 | ps-0354AF5BA0F333D1, ps-10C6A179AEBEB417, ps-EC83435F85E56F90 |
| `tex_offset_mip0` - fetch offset applied in mip-0 texels (Canary's way) | 7 | ps-0354AF5BA0F333D1, ps-80E3B687AC894882, ps-9E098197F6FAC270, ps-C84DBBA59A174733, vs-3AAB2C89B66705F3, vs-4BEB77E25080DE20, vs-6006AA67EF067311 |

No shader hit any other per-shader stub (memexport, oPts, filter overrides, getWeights/getGradients/getBCF, tex LOD
query in a vertex shader, 3D/stacked fetch, unknown opcodes, decoder issues, vertex-shader kills): the tool emits those
entries when they occur, and `simplifications.csv` (hash,feature,note; 10 rows) is the machine-readable list.

Global (apply to every shader that reaches the feature at runtime, so not listed per shader):

| Feature | What the translator does | Reference behaviour |
|---|---|---|
| Gamma texture components | piecewise-linear degamma without Canary's truncation steps | Canary's exact PWL with truncation |
| Signed texture components | left to the device's signed view | Xenia binds separate signed SRVs |
| Texture swizzle, filter, address, border | device (view mapping, sampler) | same in Xenia |
| Vertex fetch bounds | not checked | rexglue's interpreter returns 0 outside the fetch constant's size |
| Float constants beyond 256 / relative reads outside 0-255 | read 0 | interpreter also checks the SQ_*_CONST size |
| Transcendental precision | full fp32 | Canary rounds exp/log/rcp/rsq to 21 mantissa bits (option, Ace Combat 6) |
| Point sprites, point size, vertex kill, user clip planes | not generated (no Gears 2 shader writes oPts) | Xenia expands points and writes cull/clip distances |
| Depth output | SV_Depth as written (no float24 conversion) | Xenia converts to the guest depth format |
| Memexport | dropped (no Gears 2 shader uses it) | Xenia writes to shared memory |
| Vertex shader kill ops | value written, no kill | no meaning in a vertex shader |

## Notes for S3.4 (the oracle)

- rexglue's `ShaderInterpreter::ExecuteVertexFetchInstruction` has two bugs in packed formats [V, by reading; the main
  session confirmed both are also in current Xenia Canary f42147a5b, so they are upstream bugs, and patches them in the
  SDK as patch 7 before S3.4]: the
  unsigned path masks `packed_dwords[i >> 1]` without shifting by the component's offset (so only component 0 is
  right), and the signed path reads `packed_offsets[0]`, which is never set. Fix or patch both in the oracle before
  comparing vertex fetch (upstream candidate). Its loop-start code also increments the loop depth twice; the
  translator uses the documented semantics instead (count / start / step), so compare loops against the spec, not
  that code.
- The interpreter does not execute texture fetches (returns zeros); texture results need the GPU-side comparison.

## Known gaps / next

- Texture fetch from 1D/2D stacked arrays (no Gears 2 shader fetches 3D or 1D) - Texture3D only.
- getWeights / getGradients / getBCF return 0 (none in the corpus).
- Per-instruction filter overrides ignored (none in the corpus).
- Wave/derivative behaviour inside the pc loop: implicit-derivative Sample in non-uniform control flow is allowed by
  both compilers; correctness of derivatives after kills relies on demote semantics (D3D12 discard; Vulkan 1.3
  OpDemoteToHelperInvocation) - to be checked in S3.4.
