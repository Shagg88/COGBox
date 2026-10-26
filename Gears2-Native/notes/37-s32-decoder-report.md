# S3.2 report: our Xenos microcode decoder vs the shader corpus

Date: 2026-10-21. Corpus: job 074 (`--gow2_shader_dump_dir`). It covers 10 routes: chapters 2, 4, 5, 7, 10, 13, 16, 23 and 24, plus the menu. Each route got one 70 s run. Only hashes and counts are listed here; the corpus itself is game data and stays in `out\harness`.

## Result

| Check | Result |
|---|---|
| Shaders in the corpus | **1,442** (245 vertex, 1,197 pixel; 852,396 bytes of microcode) |
| Our disassembly byte-identical to the SDK's `Shader::ucode_disassembly()` | **1,442 / 1,442** |
| Decoder issues (unknown or unhandled opcode, field or export; slot past end; mini fetch without full; mark_vs_fetch_done) | **0** |
| Shaders whose text would change under Xenia Canary's rules | **0** |

These results were checked from several sides:
- **The checker catches mismatches.** A test with one changed character in a `.sdk.txt` file gives `FAIL` with the line and exit code 1.
- **The issue flags fire.** A synthetic shader with vector opcode 30, scalar opcode 41 and fetch opcode 5 reports all three.
- **The early sample agreed.** 25 shaders staged before the job finished were also 25/25.

**One shader per run is not in the corpus.** Each run logged "N distinct, N-1 written". In the menu run's `index.csv` the missing one is `vs-AB1E86137A0240E8`: 15 dwords, used from frame 1, 15,340 draws in 70 s. The SDK never analysed it, so it has no disassembly to compare. *Guess:* it is the vertex shader bound during EDRAM copies (resolves). Copies don't go through shader translation, so the SDK never needs it. To check it, we'd dump its microcode without the reference text and decode it with our tool.

## (a) Coverage and route attribution

The decoder no longer emits silently. Every unknown or unhandled case becomes a `Program::issues` entry, and `gn_disasm_check` prints these per shader as `ISSUE`.

The tracked cases are:
- unknown fetch, vector or scalar opcode;
- fetch swizzle value 6;
- unnamed vertex format;
- invalid export register;
- exec slot past the end of the microcode;
- vfetch_mini before any vfetch_full;
- control-flow opcode 15.

There are also two "Canary differs" markers, see (b). **No shader in the corpus triggers any of them.**

**Route attribution could not be recovered from this corpus.** The dump tool rewrites every shader each run sees, so a file's modification time is the *last* route that used it, not the first. The zip timestamps form 7 clusters, not 10, so even last-route mapping isn't reliable. `index.csv` holds only the final (menu) run.

With 0 failures and 0 issues, nothing needs attributing this time. For the next corpus, `shader_dump.cpp` should:
- skip files that already exist;
- append the route name to a per-shader "first seen" record.

This is a small change and is not made here.

## (b) Cross-check with Xenia Canary

Canary source: `xenia-canary/xenia-canary` master, shallow clone, commit `e6cd2e4bc998609ca25e24a24c8e32b66c5908ad` (2026-10-20). Both trees were compared after clang-format normalisation.

| File (Canary vs rexglue) | Behavioural difference |
|---|---|
| `shader_translator_disasm.cc` vs `translator_disasm.cpp` | **None** (only includes, namespaces and macro formatting) |
| `ucode.h` / `ucode.cc` vs `format/ucode.h` / `ucode.cpp` | **None** (`[[maybe_unused]]` attributes, `constexpr` vs `const`, one unnamed bitfield) |
| `shader_translator.cc` vs `translator.cpp` (parse and disasm parts) | **Two**, below |

### 1. tfetch1D with a non-replicated swizzle

Canary reads 2 coordinate components where rexglue reads 1. Canary's comment: "1D may provide XY for a 2D fetch constant. 545407D4's UI shader does this, and it seems like we have to support it, even if it doesn't make sense on paper."

**Who looks right:** Canary, for that title. The rule only matters when the fetch constant is actually 2D. On real hardware the GPU presumably samples with the constant's dimension; *unknown* without hardware tests.

**Effect on our corpus:** none. No Gears 2 shader has a 1D fetch with a non-replicated swizzle (0 flagged).

### 2. Two-component scalar operand when the vector op has 3 operands

This affects adds, muls, maxs, mins, muls_prev2, maxas, maxasf and subs. Canary takes the operand's second component (`b`) from swizzle component Z; rexglue always takes X. Canary's comment: "The scalar operation shares source 3 with the vector operation. If the vector operation uses source 3, the scalar `b` component is Z; otherwise it is X."

**Who looks right:** Canary looks deliberate and newer (rexglue's code matches the older Xenia form). Still *unknown* without hardware or an XNA disassembler reference.

**Effect on our corpus:** none. No shader pairs one of these scalar ops with a 3-operand vector op (0 flagged).

**What the decoder does:** it defaults to rexglue's rules, because the SDK renderer is our reference. Both Canary rules can be switched on through `DecodeOptions`. `gn_disasm_check` prints any shader whose text would change under them (`CANARY-DIFF`); there are 0.

The translator (S3.3) can follow Canary on both points without affecting Gears 2. Gears 3 and Judgment corpora should be re-checked with this tool.

## (c) Feature census (corpus of 1,442)

| Feature | Shaders | Vertex / pixel | Examples |
|---|---|---|---|
| Loops (loop/endloop) | 1 | 0 / 1 | ps-8DCC9EF73F7C1B78. It uses loop constant **i31** (start and step are runtime values). Its loop body has predicated 2D fetches with `UseComputedLOD=false`. |
| a0-relative operands | 54 | 53 / 1 | vs-02BA3EC0A0FC995E, vs-043A75D80AF36283, vs-047F4CAC9DCBCB79, ps-8DCC9EF73F7C1B78. *Guess:* the vertex shaders are skinning, indexing bone constants. |
| aL-relative operands or destinations | 0 | — | — |
| vfetch FMT_16_16_FLOAT | 133 | 133 / 0 | vs-0191542E252B4B79, vs-01C2428C8E7C796D, vs-0291CBCFB6438CA8 |
| vfetch FMT_16_16_16_16_FLOAT | 0 | — | — |
| Texture filter or LOD override | 3 | 0 / 3 | ps-0E44DE1308067C2D, ps-197FD5E60FC648D9, ps-8DCC9EF73F7C1B78. All three are `UseComputedLOD=false` only. There are no Mag/Min/Mip/Aniso filter overrides, no register LOD or gradients, and no LOD bias. |
| Texture fetch with non-zero offset | 3 | 0 / 3 | ps-0354AF5BA0F333D1, ps-9E098197F6FAC270, ps-C84DBBA59A174733. Offsets seen: OffsetX 1, 2, 3 and OffsetY 1, 2, 3 (in texels × 0.5 units as printed). |
| Memexport (alloc export / eA / eM) | 0 | — | — |
| Cube fetches (tfetchCube) | 87 | 0 / 87 | ps-024B98EE3657EB99, ps-026FFB42772A26CD, ps-0285C90908FAE2DE |
| Predication ((p0) / (!p0)) | 193 | 11 / 182 | ps-00CE7D578ADA4DD3, ps-023A0584A4CE92F9, ps-029C1A37D8F39C52 |
| jmp / call / ret | 128 | 0 / 128 | ps-00CE7D578ADA4DD3, ps-023A0584A4CE92F9, ps-029C1A37D8F39C52 |
| Kill instructions | 259 | 0 / 259 | ps-00CE7D578ADA4DD3, ps-01BA6758E8E8A395, ps-023A0584A4CE92F9 |
| Vertex shader writes oPts | 0 | — | — |
| vfetch_mini | 223 | 223 / 0 | vs-0191542E252B4B79, vs-01C2428C8E7C796D |
| getCompTexLOD / getGradients / getWeights / getBCF / setTex* | 0 | — | — |

**What this means for S3.3 (the translator).** The Gears 2 corpus needs:
- ALU ops, including predication and kills;
- vertex fetch including minis and 16-bit float pairs;
- 2D, 3D and cube texture fetch;
- bool-constant jumps and calls;
- a0 relative addressing;
- one loop shader;
- three shaders with texel offsets and three with `UseComputedLOD=false`.

It does **not** need memexport, aL addressing, point size, LOD or gradient queries, or texture filter overrides. Those can wait (and be flagged as unsupported) until another title needs them.

## Files

Paths are relative to `upl-n075`.

| File | Purpose |
|---|---|
| `src/layer0_xenos/include/xenos/ucode.h` | Decoded structures, issue list, Canary options. Standalone (no SDK headers). |
| `src/layer0_xenos/ucode.cpp` | Decoder and disassembler. BSD attribution to Xenia via rexglue in the header comment. |
| `tools/gn_disasm_check.cpp` | Corpus check: PASS/FAIL, issues, Canary-diff, summary. |
| `cmake-snippet.txt` | Additions to `CMakeLists.txt`. |
