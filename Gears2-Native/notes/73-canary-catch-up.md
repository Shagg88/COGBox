# 73 - Canary / Edge catch-up: every newer GPU fix, checked against ours (24 Oct 2026)

Part of the overhaul (notes/70, Shagg 02:48 "the overhaul I'm referring to had to do with our render and Canary and
Edge"). References checked out at their newest commits (Shagg 03:27: "use the most latest updated dependencies"):
Canary 9e03494 (23 Oct), Edge f88c7a231 (24 Oct). [V] = measured / read in code, [G] = guess.

## Shader translator (map items S1-S3)

| Canary commit | What it does | Ours | Done in |
|---|---|---|---|
| 3a44f20 | exp / log / rcp / rsq / sqrt rounded to 21 mantissa bits | yes, bit for bit (gow2_renderer_canary_r21, on) | 0037/0041 |
| 92ada8e | scalar operand Z with 3-operand vector ops | yes (gow2_renderer_canary_decode, on) | 0037/0041 |
| 0c843ef, 6a45452, d119505, 2ddc5ef, 052cb95 | Q16 rounding, integer / biased scaling via the swizzle | yes (gow2_renderer_tex_num_format, on) | 0040 |
| 8486e97 | unnormalized fetch of a locked mip uses that mip's size | yes (always) | 0040 |
| 9e9d3cd | out-of-range vertex fetch words read 0 | yes | earlier |
| fbdb1f2 + 7cd4794 | two-coordinate tfetch1D; Y only when the constant is not 1D | decode was on but the translator dropped Y - **now used** (DXBC), run-time 1D check | 0087 |
| 3ff230d | float16 extended range; NaN -> 0 | memexport had the range; **NaN -> 0 added** (gow2_renderer_memexport_f16_nan_zero, on). The ROV pack part does not apply (no ROV path) | 0087 |
| 3390fc2 | mulsc rounded toward zero (experimental, off in Canary) | **ported, off** (gow2_renderer_mulsc_round_toward_zero); CPU check: exact on 2M random pairs [V] | 0087 |
| b709f66 | texture point snap made optional, default off (regressions in two titles) | we never had the snap (c3cd861) - nothing to do | - |
| 0f2980d | per-axis gradient exponent bias (SPIR-V path only in Canary) | not applied; patch 0087 counts loads whose fetch constant has one (texture rules log line) | count first |
| 04085ef | stacked-layer clamp | does not apply (stacked always sampled as 3D) | - |
| 947075f / 6260a87 | 1D textures wider than 8192 as rows | not done; UE3 not known to use them [G] | later |

Corpus check (local, 2475 Gears 2 shaders, ucode -> DXBC): the tfetch1D and float16 rules change **0** shaders, so
the Gears 2 picture cannot change from them [V]; mulsc changes 1962 (hence off). All changed SPIR-V passes spirv-val.

## Textures, resolves, render targets

| Canary / Edge | What | Ours | Done in |
|---|---|---|---|
| 344fca7 | packed mip tail at the base address when level 0 is packed; 2x1 laid out like square | 2x1 and the non-zero mip address case were in (0022); **the mip-address-0 case added** (gow2_renderer_packed_tail_at_base, on; counted) | 0087 |
| 42c2b88 | fixed 16-bit EDRAM with 10 fractional bits | yes (0023, resolve rules canary) | 0023 |
| 534aeb6 | gamma resolve: average, then back through the curve | yes | earlier |
| 8322658 | k_8_A / k_8_B resolve like k_8 (alpha when swapped) | **ported** with the Canary resolve rules (colour_hlsl.h) | 0087 |
| e6cd2e4 | 7-bit guest format removed | nothing to do | - |
| cc4981a | rasterize MSAA at the Xenos sample positions (fixes Gears of War 1, 4D5307D5) | viewport part only; Gears 2 draws no MSAA targets [V, job 385 log: all 1x], Gears 3 does (4x). Canary's switch is off by default, and then Canary also skips the viewport sample correction ours always applies at scale > 1. Gears 3 said yes (mail 04:01). **Patch 0089**: --gow2_renderer_msaa_sample_positions = standard (default, as before) / xenos (D3D12 programmable sample positions with the Xenos pattern - 4x host i = Xenos i, native 2x swapped, as Canary's GetHostSampleXenosPositions; MSAA depth cleared under its pattern and decompressed in place before reads, as Canary's render target cache; resolves read host sample = Xenos sample) / canary_off (standard positions and no viewport sample correction - Canary's default). Vulkan: not built (Gears 3's SDK has Vulkan off) | Gears 3 to A/B |
| e1f4a0d / 6579cc2 | predicated tiling: negative window offsets | not done; Gears 2 does not tile (1x targets) | next (Gears 3 / 1) |
| 9c30b16 | indexed tessellated triangle strips -> lists (4D53085B = Halo Reach, per the patches folder) | 0087 logs tessellated strips / fans so we know if a Gears game draws one | log first |
| Edge f88c7a231 | fixed-point blending in 10-bit ints in FSI/ROV | does not apply (host render targets) | - |
| Edge 6947a91e6 / e08a7e822 / 9a9931f45 / a2b609d22 | 7e3 -> 8_8_8_8 same-base tile reuse decoded as a value (HDR -> LDR) instead of bit-reinterpreted | **not ported, on purpose**: Edge's own log says it "broke too many other games (DS2, UE3 titles)", then gated it on blending and on the written channels after it scrambled GoW Judgment's scene buffer, and calls it "not in the spirit of hardware accuracy". Our EDRAM transfers stay bit-exact (what the hardware does); revisit only if a Gears picture bug points at it | - |

## Vulkan render targets (notes/68, the light-scene GPU gap)

Patch 0087 adds --gow2_renderer_vulkan_compressible_targets: colour render targets start as one-format images (which
the driver may compress) and are made mutable, contents copied, only when a signed / bits view is really needed
(logged and counted); "never" is a measurement-only mode. Job 391 measures ch15 with all three. gn_vktest checks the
promotion (contents kept, signed view right, still renders) [V on lavapipe].

## Job 391 (24 Oct 03:47, patch 0087 = 9b55cfc) [V]

- Gate (new texture / export rules off) identical to job 390b at every shot.
- In the game: 0 of 6460 shaders changed by the default-on shader rules; the packed-tail rule did not fire in ch10.
- Defaults run vs gate: FLIP 0.002-0.045, all of it the characters' positions (Dom, the Seeker) - game timing between
  runs, the scenery pixel-identical; no rendering change.
