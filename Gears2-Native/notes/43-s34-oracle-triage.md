# 43 — S3.4 oracle triage of job 090 (235 FAIL of 2,474), interim

Job 090 ran on the RTX 3080 Ti with D3D12/DXIL: EXACT 606, CLOSE 1,633, FAIL 235.

The triage was done in the container. Lavapipe (Mesa's CPU Vulkan driver) ran the same translated HLSL compiled to
SPIR-V by DXC, through the new `--vulkan` runner, and the probe and `--temps` / `--dump` tools were used to find the
first point of difference. The GPU-side evidence for NVIDIA itself still needs job 091.

## Harness bug found first: the PC and the container did not see the same inputs

- **Cause:** the inputs were drawn with `std::uniform_*_distribution` and `std::pow`, and their results depend on the
  standard library. MSVC and libstdc++ give different values from the same mt19937 seed.
- **Effect:** in 58 of the 235 failing cases, the CPU value on the PC differed from the CPU value in the container.
- **Fix:** the distributions are now written out from the raw engine output, with only exact float operations
  (`Unit`, `Int`, ldexp). The inputs are now the same on every platform, so a PC result can be reproduced here lane
  for lane.

## Categories (job 090 counts, from the first mismatch per shader)

| Class | Count | Examples | Root cause | Verdict now |
|---|---|---|---|---|
| A. NaN divergence: one side NaN, set 4 or 7 (NaN/Inf inputs) | 153 (151 PS, 2 VS) | ps-3828A47EE5E92AD3, ps-42C1E68152AB4D23, ps-49D24B2225D411FE | See class A below | New verdict NAN_ONLY, separate from FAIL |
| B1. VS a0, interpolators | 12 | vs-133DA717DB9BD8C2, vs-4FB4D505F676A1A1 | See class B below | Fixed or reclassified |
| B2. VS a0, oPos | 21 | vs-A58C7EFC9E1D3E02, vs-1C40C8188947EFE5 | As B1 | As B1 |
| B3. VS without a0 | 4 | vs-8357E10A55770032, vs-21DCFE927D54A076, vs-A3DD7FC570ECC7A4, vs-C9EC60728D9089A0 | vs-8357E: translator bug (vertex fetch bounds) and a harness index artefact; the other three as B1 | Fixed |
| C. PS, both sides finite | 45 | small: ps-C12B99D3F8CEFE54 (3F13166F vs 3F13D465), ps-850C332EAAD6491F; large: ps-516ACEC37A5F608B (set 7), ps-0046107DA4CF0E09 (set 4) | See class C below | CLOSE (envelope) or PRECISION; the rest need job 091 |

**Class A.** In 146 of these cases the inputs matched across platforms. In all 146, lavapipe running our SPIR-V
gave the interpreter's NaN, bit for bit. So the translated HLSL has the right semantics, and the NVIDIA DXIL path
drops the NaN.

- The DXIL is as intended. Disassembly of ps-0046107DA4CF0E09 shows fcmp+select for the D3D9 max/min, no FMax,
  and no fast-math flags.
- The suspect is the driver turning `select(a >= b, a, b)` into a NaN-dropping fmax.
- The probe in job 091 tests exactly this, with a candidate fix (`max_nanfix` / `min_nanfix`).

**Class B (B1, B2, and three of the B3 shaders).** The causes were:

- **Translator bug, vertex fetch had no bounds check.** Canary and the interpreter return 0 for words outside the
  fetch constant's [address, address + size); we read whatever guest memory was there. Canary's comment: the
  hardware clamps, and "Games rely on that". Fixed.
- **Undefined float-to-int conversion of the vertex index.** HLSL `int()` and the interpreter's C++ cast are
  undefined for NaN and huge values, and they differ between drivers and compilers. Fixed on both sides with one
  rule: NaN → 0, saturate to int32 (D3D ftoi), then treat as unsigned. This is SDK patch 10 plus `xe_ftoi` in the
  translator.
- **a0 rules.** Both sides already agree: the index is clamped to [-256, 255] (NaN → -256), and constant reads
  outside 0–255 return 0 on both sides. No a0 failure remains that this rule explains. The 84 shaders with a0 out of
  range use synthetic inputs, so they stay flagged in the A0 line, not counted as FAIL.
- **Cancellation.** Last-bit transcendental differences are amplified by later subtraction, for example dp3 terms
  around 1e5 giving 0.234 against 0.266. This is the new PRECISION class (ill-conditioned: the CPU's own envelope is
  wider than the tolerance).

**Class C.**

- The small differences (relative error 1e-4 to 5e-3, 22 shaders) look like NVIDIA's hardware transcendental
  precision. Lavapipe matched the CPU on the cases checked. These are now CLOSE inside the transcendental envelope,
  or PRECISION.
- The large differences (about 23 shaders) are all set 4 or 7. They are probably the class A NaN, absorbed later into
  a finite value, plus denormal effects. Job 091 needs to confirm this.

## Translator bugs confirmed and fixed (upl-n091/src/layer0_xenos/hlsl.cpp)

1. **Vertex fetch bounds** (`xe_vf_word`): a word outside [address, address + size) reads as 0, as Canary and the
   interpreter do. Addresses are now in dwords with 32-bit wrap, as the interpreter computes them.
2. **Vertex index conversion** (`xe_ftoi`): defined for NaN and out-of-range values, matching SDK patch 10.
3. **`!=` on floats** (sne, snes, setp_ne, setp_ne_push, kill_ne, kills_ne, alpha test "not equal") is now written
   `!(a == b)`. The probe found that DXC -spirv compiles `!=` as an ordered comparison, so NaN != x gave false.
   The Xenos rule, the interpreter, and DXIL's `une` give true.
4. **`XE_FLT_MAX`** is now `asfloat(0x7F7FFFFFu)`. The decimal literal came out as 0x7F7FFFFE in SPIR-V; the probe
   showed this as `rcpc(0)` returning 7F7FFFFE.
5. **NaN-safe forms of the legacy multiply, the a0 clamp and _sat.** min(), max(), clamp() and saturate() with a NaN
   operand are defined in DXIL but undefined in SPIR-V.
   - Legacy multiply: `a == 0 || b == 0`, instead of `min(abs(a), abs(b)) == 0`.
   - a0 clamp: `xe_a0_clamp` uses comparisons.
   - _sat: `xe_sat` / `xe_sat4` give NaN → +0 and -0 → +0, as rex::saturate.

The translator now compiles every shader for both targets:

- The DXIL compile of all 2,474 shaders (container DXC 1.9, `-Gis -denorm ftz`): COMPILED 2,474.
- SPIR-V: 2,474/2,474 compiled in the lavapipe run.

## Interpreter (SDK) changes: patches 9–11

No outright interpreter bug was found in the ALU rules; they match notes/38. The changes below add determinism and
test hooks.

- **Patch 9, `SetTranscendentalBias`:** moves exp, log, logc, rcp*, rsq*, sqrt, sin and cos results by ±2^-21
  relative (log also by 2^-21 absolute), uniformly or with pseudo-random signs. The oracle runs 6 biased CPU passes
  for each transcendental shader. A GPU value inside the resulting envelope counts as CLOSE (`envelope`). If the
  envelope is wider than the tolerance, the output is ill-conditioned and the verdict is PRECISION rather than FAIL.
  This replaces "tolerance too tight". Canary's 21-bit rounding (notes/38) is about the same size.
- **Patch 10:** defined conversion of the vertex fetch index from float to int (see class B above). The old cast was
  undefined behaviour and differs between compilers.
- **Patch 11, `SetFlushDenormals(false)`:** for comparing with a GPU that cannot flush denormals (lavapipe). The
  default is unchanged.

Patch 7's packed_offsets zero-initialisation is present (hint 5).

## Harness changes (upl-n091/tools)

- Platform-independent input generation (see the first section).
- **Pixel outputs:** only exported components are compared (from n087).
- **Denormal outputs:** flushed on both sides before comparing; counted in `denorm_flushed`.
- **NAN_ONLY:** exactly one side NaN, in set 4 or 7, or with the other side ±Inf. Reported separately and not
  counted as FAIL.
- **Transcendental envelope:** gives CLOSE (`envelope`) or PRECISION (`ill_conditioned`).
- **New options:**
  - `--only a,b,c` or `--only @file`
  - `--dump set:lane`, which also prints the pixel inputs
  - `--temps`: o0–o15 (VS) or oC0–3 (PS) carry the final r0–r15 / r0–r3, to find where a lane goes wrong
  - `--no-denormals`: no denormal inputs
  - `--keep-denormals`: the interpreter does not flush
  - `--no-envelope`
  - `--vulkan`: container only
  - `--probe`
- **`--probe`:** 42 edge-value operations on 8×8 pairs of NaN, ±Inf, ±0, 1.5, -2 and FLT_MAX. Each op is checked
  against the Xenos rule. The ops include the translator's own prelude functions, plain HLSL forms, an FMA
  contraction test, NaN-safe max/min variants, and Sample with NaN coordinates.
  - On lavapipe, every translator op matches.
  - The only differences are in plain HLSL `saturate`, `max` and `min` (signed zero), and `!=` (now unused by the
    translator).

## Lavapipe, current code, whole corpus, `--no-denormals`

- **First 1,004 pixel shaders, sorted by name:** EXACT 410, CLOSE 588, NAN_ONLY 1, FAIL 5.
- **Those 5, rerun with `--keep-denormals` (both sides IEEE with denormals):** none fail any more. They were denormals
  created by arithmetic, which lavapipe does not flush, for example rcp(-FLT_MAX).
- **The 39 vertex shaders that failed in job 090, plus those 6:**
  - EXACT 11, CLOSE 14, NAN_ONLY 10, PRECISION 4, FAIL 6.
  - oPos mismatches beyond tolerance: 0.
  - The 6 remaining FAILs:
    - set 5 (inputs 1e20 to 1e35): +Inf against -Inf after overflow, in vs-15BD28C3722A7E34, vs-1988582624310CDA,
      vs-23EDBC17997302D4 and vs-D86D448D3470560A.
    - set 2 cancellation (-4 against 0): vs-21DCFE927D54A076 and vs-C9EC60728D9089A0.
  - All 6 look like inputs being amplified, not wrong operations. They need a finer look.
- **oPos bit-exactness (`opos_not_bit_exact`, 29 shaders):** these are last-bit transcendental differences between
  GPU and CPU. That does not affect prepass/base-pass depth equality. Depth equality needs the same GPU to produce the
  same bits in both passes, which holds when both passes use the same translated code. To be checked with a
  same-GPU A/B later.

## Open, for job 091

- **Which NVIDIA construct drops NaN:** `--probe` on the PC answers this. If `xe_max1` / `xe_min1` fail and
  `max_nanfix` / `min_nanfix` pass, the translator switches to the nanfix form.
- **A full rerun with the new harness and translator,** to get counts per new class.
- **A `--no-denormals` rerun of the same shards.** Its inputs are bit-identical to the container's lavapipe run, so
  NVIDIA and lavapipe can be compared lane by lane.
