# 41 — S3.4 shader-translator oracle (gn_shader_oracle)

Stage 3 step S3.4 (D21 item 4; notes/36, notes/38 point 5). This tool runs every translated shader on the GPU
(D3D12, DXIL) and runs the SDK's CPU `ShaderInterpreter` on the same inputs, then compares the outputs.

## What it does

For each shader in the corpus (`vs-*.bin` / `ps-*.bin`):

1. Decode it with `xenos::ucode` and translate it with `xenos::hlsl` (the S3.3 translator, unchanged).
2. Build 8 input sets × 64 lanes. Set modes 0–7 (ValueSource) cover zeros, ones, small integers, random
   values and edge values: ±0, denormals, ±Inf, NaN, FLT_MAX. Bool constants are set to all-0, all-1,
   0xAAAAAAAA or random. Loop constant i31 is count 6, start 0, step 0.
   - **Vertex shaders:** the lane index goes into r0.x. Vertex data comes from a big-endian guest-memory
     buffer at 0x10000. It is t96 on the GPU and the interpreter's membase on the CPU, with the same
     vertex fetch constants (k8in32, stride from the shader).
   - **Pixel shaders:** 16 interpolators per lane, from the same sources.
3. **CPU run:** the SDK interpreter with patch 8, using an export sink plus a texture stand-in sink. A step
   guard means the interpreter cannot hang.
4. **GPU run:** a compute wrapper calls the translated stage `main` (`cs_6_6`, numthreads 64). It is compiled
   with the S3.3 DXC flags (`-HV 2021 -Gis -denorm ftz`). There is one dispatch per set, then readback.
   DXC has a 120 s timeout and the GPU fence has a 10 s timeout. On device lost, the tool stops and says so.
5. Compare the outputs and write a verdict and one CSV row per shader. The CSV holds hashes and numbers only,
   with no shader code.

## Texture stand-in

Every texel of slot k (2D, cube or 3D) holds one exact colour: (1+k)/128, (33+k)/128, (65+k)/128, (97+k)/128,
offset by dimension. The fetch constant is chosen so the translator's post-fetch step is the identity:
unsigned signs, format 38, identity swizzle, exponent 0. The CPU sink returns the same colour.

This tests the ALU code around each fetch, the fetch-constant selection and the destination swizzle. It does
**not** test filtering, addressing, LOD, gradients or format conversion. Shaders that use it carry the
`tex_standin` flag.

## Comparison

- **EXACT:** bit-equal, or NaN on both sides.
- **zero_sign:** +0 against −0.
- **CLOSE:** relative difference ≤ 1e-4, or absolute difference ≤ 1e-6. Set these with `--rel-tol` and
  `--abs-tol`. These are meant for transcendentals: the GPU's exp, log, rsq and sin differ from the CPU's
  libm.
- **Mismatch:** anything else.
- **Verdicts:** FAIL if there is any mismatch or any kill disagreement. Otherwise CLOSE if any component was
  CLOSE or zero_sign, otherwise EXACT. SKIP means no lane could be compared. ERROR means the compile or the
  GPU run failed.

Rules for particular lanes and outputs:

- **Killed lanes:** a lane killed on both sides is not compared further. A lane killed on only one side is a
  `kill_mismatch`.
- **Vertex oPos with non-finite W:** not compared. With W non-finite, the completion's `0*w` depends on the
  compiler.
- **Pixel shaders compare only what the interpreter exported** in that test case. Components that were not
  written are counted in `unwritten_skipped`. On D3D10+ an unwritten output has no defined value: D3D9 left
  it untouched and D3D10+ does not (Brief 10, the Gears of War 1 lighting pass).
- **Per-test-case written set:** the CPU run records which of oC0–3 and oDepth were written in each case. If
  the set varies, the shader gets the flag `ps_out_varies` and a `PSOUT` line is printed. Those shaders are
  candidates for the translator's merged-output fix (D29).
- **Vertex oPos is reported separately from interpolators.** Depth equality between the prepass and the base
  pass needs bit-identical clip-space positions:
  - columns `pos_mismatch` (beyond tolerance), `pos_inexact` (anything not bit-identical, including CLOSE)
    and `interp_mismatch`
  - flags `opos_mismatch`, `opos_not_bit_exact` and `interp_mismatch`
  - an `OPOS` line per affected shader
- **Expected failures:** shaders listed in `--simplifications` (notes/39) get the flag `expected:<feature>`,
  and stand-in shaders get the flag `tex_standin`. Read FAILs on those against the flags before treating
  them as translator bugs.
- **a0:** the interpreter logs the relative-index range. Ranges outside 0–255 are printed as `A0` lines.

## SDK patch 8 (in rexglue-sdk.diff, patches 1–8)

Changes to `ShaderInterpreter`:

- An optional `TextureFetchSink`. When it is not set, the default behaviour is unchanged. When it is set,
  tfetch calls the sink and stores the result, setTex* ops are a no-op, and other texture queries read
  zeros.
- A constructor with a membase override, so the interpreter runs without `rex::runtime` memory.
- Observations: `killed()` (all 9 kill ops), `hit_step_limit()` (`kMaxControlFlowSteps = 65536`), and a0
  relative-read count, minimum and maximum.

The diff also carries patch 7, the GetLoopAddress fix. The SDK must be rebuilt with this diff before the
oracle is built, because the oracle compiles `interpreter.cpp` against the installed `interpreter.h`.

## Verified in the container

- **CPU-only, all 2,474 shaders:** every one ran, in about 11 s, with no step-limit hits.
  - 84 vertex shaders show a0 ranges outside 0–255. These come from the synthetic inputs, so they say nothing
    about real draws yet.
  - Pixel-shader written sets were constant per shader across all 8 × 64 cases, including the all-bools-on
    and all-bools-off sets. `ps_written_outputs_vary` was 0.
  - Written sets: 2,144 shaders write oC0 only; 1 writes oC0+oDepth (`ps-9E098197F6FAC270`); 1 writes
    oC0+oC1 (`ps-C84DBBA59A174733`).
  - So no dynamic-output pixel shader shows up under synthetic inputs. This needs confirming with recorded
    draw constants.
- **Compile-only, all 2,474 compute wrappers** (`cs_6_6`, container DXC 1.9): `COMPILED 2474`, no ERROR.
- The oracle's main file passes a MinGW Windows syntax check.

## Not tested yet (needs the PC)

- D3D12 execution itself: the root signature, PSO creation, dispatch, readback, and the fence/device-lost
  paths in `gn_shader_oracle_d3d12.cpp`.

## Known limitations

- **The compute wrapper does not exercise pixel-shader-specific hardware behaviour:**
  - real helper lanes
  - derivatives after kill (kill becomes a flag with demote-like continuation)
  - the rasteriser's interpolation
  - early depth
  - SV_Position and front-face (both are fixed)
- **The texture stand-in does not test sampling behaviour:** see above.
- **SPIR-V is not run:** the oracle runs DXIL only.
- **Inputs are synthetic:** the a0 ranges and the written sets reflect the synthetic sets, not the game. The
  next step is to feed recorded draw constants from draw_observer into the same comparison.
