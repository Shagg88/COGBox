# 38 - Translator rules (S3.3 input), from Brief 9 and the corpus census

Brief 9 (research chat, 2026-10-21 02:49) answered from Xenia Canary f42147a5b, Xenia Edge 8fb2772, XenosRecomp 990d03b
and the hardware documents Canary cites. Labels: [V] verified in source, [D] documented, [I] inferred, [G] guess.
Census: **2,474 shaders** (328 vertex, 2,146 pixel) from job 081 - all 29 chapters' openings plus the menu - counted on
our decoder's disassembly (identical to the SDK's for the 2,473 that have SDK text), `_sat` variants counted with their
base op. (The first version of this table used job 074's 1,442 shaders with `_sat` counted separately.)

## Rules our HLSL must follow

| Rule | What | Source | Gears 2 usage |
|---|---|---|---|
| Legacy multiply | if either factor is +-0 or denormal the product is +0 (even times Inf/NaN): `min(abs(a),abs(b)) == 0 ? 0 : a*b`; skip the check when both operands are the same register+swizzle; `mad` = that product then a separate add (not fused, as Canary); never turn `0 + c` into a move of c | [D] AMD R5xx guide 8.7.5 via Canary ucode.h; [V] always on in Canary/Edge | everywhere: mul in 2,432 shaders, mad 2,419, dp3 2,195, dp2add 960, dp4 180, mulsc 1,961, muls_prev 1,452, muls 766 |
| Denormals | treated as zero | [D] | compile with DXC `-denorm ftz` [I] |
| max/min | D3D9 order: `max = a >= b ? a : b`, `min = a < b ? a : b` (NaN in a returns b) - not HLSL max/min | [D/V] | max 2,193 shaders, maxs 1,850, min 401, mins 50 |
| max4 | unresolved (`>` with X,Y,Z,W priority vs `>=`) | [D] | 3 shaders (same in both corpora): follow rexglue (>=), flag for a FLIP check |
| rcp/rsq/log/exp | exact cases: rcp/rsq(1)=1, log(1)=0, exp(0)=1; IEEE forms keep Inf; `c` forms clamp Inf to +-FLT_MAX; `f` forms send Inf to +-0 | [D] Canary ucode.h | rsq 1,882 shaders, exp 1,473, log 1,416, rcp 749, rcpc 19; no logc, rsqc, rcpf or rsqf |
| 21-bit rounding of transcendentals | Canary rounds exp/log/rcp/rsq to 21 mantissa bits (added for Ace Combat 6, not from a hardware document) | [V]/[I] | start without, keep a switch |
| a0 | maxas/movas set a0 rounded and clamped; docs say [-256,255], Canary DXBC clamps [0,255] | [V] | maxas in 85 shaders (84 vertex, plus the loop shader); a0-relative reads in the same 85: log any negative a0 |
| ps (previous scalar) | not updated when predication skips the instruction; every scalar op except retain_prev updates it | [D] | predication in 354 shaders |
| Scalar operand order | W then X (or Z, see below) | [D] | - |
| cube, dp2add | exact formulas in Canary ucode.h; cube gives 2x the major axis, face IDs as D3DCUBEMAP_FACES | [D] | cube in 179 shaders (all with a cube fetch), dp2add 960 |
| kill / setp | also write their own values (1.0/0.0, push/pop counters) | [D] | kill in 434 shaders |
| 16-bit float vertex data | the 360 half has no Inf/NaN (exponent 31 is normal, max 131,008); Canary still decodes vertex halves as IEEE (FIXME) | [D]/[V] | FMT_16_16_FLOAT in 170 vertex shaders (no FMT_16_16_16_16_FLOAT): the translator decodes the 360 extended-range half (S3.3) |
| Fetch offsets | signed 5-bit x 0.5 texels; Canary applies at mip 0 plus 1.5/1024 texel; hardware formula unknown | [V]/[G] | 7 shaders: do as Canary, log offset fetches from textures with mips |
| Gamma | piecewise linear: linear 0-64 -> 0-64 (slope 1), 64-128 -> 64-96 (1/2), 128-512 -> 96-192 (1/4), 512-1023 -> 192-255 (1/8), with Canary's truncation; Canary converts after filtering | [V] | use sRGB formats only if FLIP shows no difference |
| Resolve exponent bias | decode samples, sum, multiply by 2^bias / samples, convert; all colour formats; depth bias 0; separate from RB_COLOR_INFO.color_exp_bias (applied to shader output before the write) | [V] | -3 on the HDR resolve |
| tfetch1D two coordinates; scalar 2nd operand from Z | Canary/Edge changes (Aug 2026), rexglue older | [V] | 0 of 2,473 shaders affected (decoder census) |
| oPts | point size / edge flag / kill vertex; points need quad expansion and sprite coords | [D] | 0 shaders write oPts: no point expansion needed for Gears 2 |
| Memexport, aL addressing, texture filter/LOD overrides | - | - | 0 shaders: not needed for Gears 2 |

DXC flags: `-Gis` (strict IEEE) and `-denorm ftz`; mark the legacy-multiply and max/min selections `precise` so fast
math cannot fold them back. XenosRecomp ignores almost all of the maths rules above (no legacy multiply, plain
max/min, rcp/rsq/log clamped to FLT_MAX), so it is not the reference for ALU behaviour; Canary/Edge are.

## What this means for borrowing

From XenosRecomp (MIT) we can take structure (HLSL emission layout, control flow as structured code with a pc-switch
fallback, constant buffer packing ideas). The maths rules, a0 handling, gamma and resolve behaviour follow Canary/Edge
(BSD) instead. Gears 2 needs none of XenosRecomp's missing features except loop constants (still 1 shader of 2,474 loops, using
i31) and the 16-bit float vertex nuance, so the gaps that matter are small.

## Before S3.3: review points (user, 02:52) and what was done

1. **The one loop shader** (ps-8DCC9EF73F7C1B78, loop constant i31): log i31's actual values on every route (job 078,
   `--gow2_watch_shaders`). Start 0 and step 1 everywhere -> XenosRecomp's loop code is fine; otherwise follow Xenia.
2. **Cube maps inside control flow:** XenosRecomp passes the `cube` result to the fetch through a local array, which its
   README says can break in complex control flow. Census (2,474): of the 179 cube-map shaders, **11** also branch,
   loop or predicate (1,442-shader corpus: 7 of 87). Those 7 are the first to compare against Canary.
3. **a0 in 84 vertex shaders** (85 with the loop shader; 1,442 corpus: 53) (almost certainly skinning: bone matrices indexed by a0): log any a0 index that reaches
   outside c0-c255 as well as negatives; out-of-range constant reads behave differently on PC. a0 is computed inside
   the shader, so this is checked in S3.4 (interpreter on recorded inputs), not by a register watch.
4. **Discards:** 434 shaders kill pixels and **375** of them fetch a texture after a kill (1,442 corpus: 259 / 220) (text order in the
   disassembly; approximate). On PC, derivatives on discarded lanes can be undefined; Shader Model 6.6's demote-to-helper
   (`discard` as demote) is the closest match to the console, so the translator emits kills as demote where SM6.6 is
   available and notes it where not.
5. **Test oracle for S3.3/S3.4:** rexglue already carries Xenia's CPU shader interpreter
   (`src/graphics/pipeline/shader/interpreter.cpp`, BSD), which implements the same maths rules. For each translated
   shader: the same inputs to our HLSL on the GPU and to the interpreter, including edge values (+-0, denormals, +-Inf,
   NaN, FLT_MAX), outputs compared bit for bit. That catches maths errors per shader before they become image
   differences.
6. **Cost of the legacy multiply** (about three extra instructions per multiply): correctness first; FLIP and
   `--gow2_frame_times` decide later whether any shader is worth a faster special case.

## Census on 2,474 shaders (job 081 corpus, S3.3 run)

| Feature | Shaders | Note |
|---|---|---|
| Loops | 1 | ps-8DCC9EF73F7C1B78, loop constant i31 |
| a0-relative addressing / sets a0 (maxas) | 85 | 84 vertex + the loop shader |
| aL-relative addressing | 0 | |
| Cube fetch (and cube op) | 179 | 11 of them also branch, loop or predicate |
| jmp/call/ret/loop (pc-driven control flow in our HLSL) | 255 | |
| Predication | 354 | |
| Kill / kill then a texture fetch | 434 / 375 | discard = demote to helper |
| Vertex fetch FMT_16_16_FLOAT | 170 | extended-range half decode |
| vfetch_mini | 299 | |
| Texture fetch offsets | 7 | |
| UseComputedLOD=false | 6 | |
| max4 | 3 | |
| Transcendental (rcp/rsq/log/exp, any form) | 2,065 | rcpc 19; no logc/rsqc/rcpf/rsqf |
| Filter/LOD overrides, tex queries, tfetch1D/3D, memexport, oPts, FMT_16_16_16_16_FLOAT | 0 | |
