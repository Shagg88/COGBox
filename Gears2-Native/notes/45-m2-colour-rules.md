# 45 - M2 colour rules (Brief 12 answers, Canary e6cd2e4bc, research chat 2026-10-21 07:59)

Labels: [V] verified in source, [D] documented in a comment, [I] inferred. Reference only; the 360 hardware wins (D23).

The main finding: on its default path, Canary keeps 7e3 targets as fp16 with no clamping and no 2-bit alpha until the data leaves the render target. So your M2 comparisons need to quantise to 7e3 before comparing, or you'll get false differences.

Source is Canary `e6cd2e4bc`, paths relative to `src/xenia/gpu/`. Labels: **[V]** verified in source, **[D]** documented in a comment, **[I]** inferred.

## 1. 7e3 render targets (`k_2_10_10_10_FLOAT`, and the same for `_AS_16_16_16_16`)

**How the default RTV path stores them**
- **Host format:** `R16G16B16A16_FLOAT` for both drawing and storage (`d3d12/d3d12_render_target_cache.cc:1792–1794`, 1854–1858) [V].
- **No clamping in the pixel shader:** it only applies the colour exponent bias (`dxbc_shader_translator_om.cc:1856`). The 7e3 limits (RGB ≤ 31.875, alpha 0–1) are applied only on the ROV path (`d3d12_command_processor.cc:4602–4647`, values from `render_target_cache.cc:250–254`) [V].
- **Blending:** ordinary fp16 blending (`pipeline_cache.cc:3209–3277`) [V]. Alpha keeps full fp16 precision while it's on the render target and is never reduced to 2 bits there [V]. Xenia's comment: "alpha is fixed-point and has only 2 bits" (`render_target_cache.h:84–85`) [D].
- **`_AS_16_16_16_16`:** its storage format is `2_10_10_10_FLOAT` (`xenos.h:396`), so it goes through every code path above unchanged. The only hint of a difference is the comment "16-bit fixed point at half speed, with full blending" (`xenos.h:307`) [V/D].

**Encoding when data leaves the render target** (transfers, EDRAM dumps)
- **RGB, clamping:** `UnclampedFloat32To7e3` clamps with `max(x, 0)` then `min(x, 31.875)`, so NaN becomes 0 (`om.cc:3875–3889`) [V].
- **RGB, encoding:** then `PreClampedFloat32To7e3` (`om.cc:3816–3873`) [V]:
  - Values below 0.25 become denormals: `(mant | 0x800000) >> min(125 − exp, 24)`.
  - Otherwise the exponent is rebiased with `+0xC2000000`.
  - Then `(b + 0x7FFF + ((b >> 16) & 1)) >> 16 & 0x3FF`, which is **round to nearest even** ("rounding to the nearest even", `dxbc_shader_translator.h:578–580`) [D].
- **Alpha:** `saturate(a) × 3 + 0.5`, converted to an integer, into bits 30–31 (`d3d12_render_target_cache.cc:3493–3517`, 3797–3825, 6351–6375) [V].
- **7e3 to 7e3 transfers** copy the fp16 value unchanged, with no quantisation (`:3800–3803`) [V].

**Decoding** (resolves)
- `Float7e3To32` (`xenos.cc:104–120`) and `XeUnpackR10G10B10A2Float` (`shaders/pixel_formats.xesli:389–420`) compute RGB as `((e + 124) << 23) | (m << 16)`, with denormals normalised [V].
- Alpha is `(p >> 30) / 3`, so it can only be 0, ⅓, ⅔ or 1 [V].

**For M2** [I]
- Canary's render target holds unquantised fp16, so compare in 7e3 terms: **encode both images to 7e3 bits** (using the rules above) and compare those.
- Keep RGB clamping to [0, 31.875] behind a switch. Hardware clamps there, but Canary's default path doesn't, so turn it off while matching the emulator and on for accuracy.

## 2. Fixed-point `k_16_16` and `k_16_16_16_16`

- **What the guest stores:** signed 16-bit fixed point over **−32 to +32**. Xenia: "0x8001 = −32, 0x7FFF = 32" (`xenos.h:303,306`; `draw_util.cc:1671`) [D].
  - Decoding is `max(−32, int16 × 32/32767)`, so 0x8000 and 0x8001 both give −32 (`pixel_formats.xesli:422–447`) [V].
- **Host formats:** `R16G16[B16A16]_SNORM` for drawing, typeless for storage ("SNORM has two representations of −1"), and `_UINT` for bit-exact transfers (`d3d12_render_target_cache.cc:1797–1849`) [V].
- **`snorm16_render_target_full_range`** (default on, `render_target_cache.cc:190–195`) [V/D]:
  - It applies `color_exp_bias −= 5`, so the shader output is ×1/32 (`d3d12_command_processor.cc:4583–4596`). The host SNORM bits then **equal the guest bits** (host 1.0 = guest 32) [I].
  - The cost is "multiplicative blending correctness": additive blending stays right, but any blend that multiplies by a colour or alpha sees values 32 times too small.
  - With it off, values are clamped to ±1.
- **Resolves:** decode to −32…32 as above. The +5 bias correction only applies when the option is off [V]. In that mode the correction looks double-applied against the newer ×32/32767 decode [I], but it doesn't matter at the default setting.

**For M2** [I]: use SNORM with ×1/32 as Canary does; it's bit-exact. Log any draw that blends multiplicatively into these formats; only those would be wrong.

## 3. Colour resolve, step by step (`shaders/resolve.xesli:740–888`)

1. **Decode each sample** (`:300–360`, 520–560). A format with no alpha component gets alpha = 1.0 [V]:
   - 8888: /255
   - 2_10_10_10: /1023, with alpha /3
   - 7e3: as in section 1
   - 16_16: as in section 2
   - fp16: unpack half
   - 32_FLOAT: raw bits
2. **PWL gamma decode**, RGB only, for 8888_GAMMA sources (`:721–733`) [V].
3. **Average the samples:** the bias factor is halved for each doubling of samples. **No rounding or clamping in between** [V].
4. **Multiply by `2^exp_bias`:** RB_COPY_DEST_INFO bits 16–21, signed (`:148–150`) [V].
5. **Swap red and blue**, if `copy_dest_swap` is set (`:807–810`) [V].
6. **Encode to the destination format** (`pixel_formats.xesli:104–126`) [V]:
   - **8888 and 2_10_10_10:** `uint(saturate(v) × max + 0.5)`. NaN becomes 0 through saturate. 2_10_10_10 alpha is `×3 + 0.5`.
   - **Signed number formats:** `clamp(v, −1, 1) × (2^(b−1) − 1) ± 0.5`, truncated toward zero.
   - **16_16_FLOAT and 16_16_16_16_FLOAT:** `f32tof16`, the hardware conversion (round to nearest even [I]). **No clamp, so NaN and Inf pass through** (`ui/shaders/xesl.xesli:1237`).
   - **32_FLOAT:** raw bits.
7. **Endian swap** on store (`resolve_full_32bpp.xesli:56–58`) [V].

**Fast path:** a raw byte copy is used when the formats are bitwise equivalent, the bias is 0, one sample is selected, the number format matches and the source isn't gamma (`draw_util.cc:1735–1762`) [V].

## 4. Gamma render targets (`8_8_8_8_GAMMA`)

- **Default** (`gamma_render_target_as_unorm16` on, `render_target_cache.cc:163–172`):
  - Stored **linear** in `R16G16B16A16_UNORM` (`d3d12_render_target_cache.cc:1786–1788`), so **blending happens in linear space** and the shader doesn't encode [V].
  - The PWL curve is applied only **when data leaves the render target**: EDRAM dumps and transfers to 8888 or depth (`:6320–6334`, 3450, 3690–3757). Transfers into a gamma target decode to the midpoint so values survive unorm16 rounding (`:3598–3615`) [V/D].
- **With the option off, or on ROV:**
  - Stored as `R8G8B8A8_UNORM`, and the shader applies the real PWL encode after the exponent bias (`om.cc:1861–1877`). Blending then happens in gamma space.
  - Xenia's own comment: "this is incorrect, since it must be done after blending" [D].
- **Resolves always decode PWL to linear and never re-encode** (`resolve.xesli:722–729`). There's an unused `XeLinearToPWLGamma` at :686, and a stale comment at :663 says the opposite [V/D].
  - So a gamma target resolved into an 8888 texture is written as **linear** 8-bit, while hardware probably copies the PWL bytes as they are [I].
  - **Check whether Gears uses 8888_GAMMA render targets at all.** If it does, compare that resolve output carefully: a texture read as gamma would be decoded twice.

## 5. MRT

- **Write masks** (`GetNormalizedColorMask`, `draw_util.cc:1039–1081`) [V]:
  - Targets the shader never writes, and targets whose `RB_COLOR_MASK` is 0, are **removed completely**: not bound, and not part of ownership changes.
  - Components the format doesn't have are added to the mask.
- **Blending** (`pipeline_cache.cc:1628–1647`, 3209–3277) [V]:
  - `IndependentBlendEnable` is always on, with per-target blend states from `RB_BLENDCONTROL0–3`.
  - Blending is enabled only when it isn't the identity (1·src + 0·dst).
  - For MIN and MAX, the blend factors are forced to ONE.
- **A target the shader writes on some code paths but not others:** the output is set to zero at the start, so a path that skips it **writes 0** (`dxbc_shader_translator.cc:970`) [V]. On hardware, and in D3D9, an unwritten output doesn't write at all. Xenia acknowledges D3D10+ "assumed to be always written" (`render_target_cache.cc:882–885`) [D].
  - **For you** [I]: that's the Gears lighting case. Track which outputs each path actually wrote, and turn off writes (through the blend state or the merged-output approach) rather than writing 0.
- **The same EDRAM base bound twice:** the higher slot is dropped (`render_target_cache.cc:866–905`) [V].
- **Alpha test and alpha-to-coverage** happen in the shader before the exponent bias. Blending is unaffected (`om.cc:1854–1855`) [V].

## 6. Resolve-clears of colour (`d3d12_render_target_cache.cc:5353–5502`)

- **Value:**
  - 32bpp formats use `RB_COLOR_CLEAR`.
  - 64bpp formats use `RB_COLOR_CLEAR_LO | (RB_COLOR_CLEAR << 32)`, so **LO is the low dword** (`:1535–1543`) [V/D].
- **Per format** [V]:

| Format | Written as |
|---|---|
| 8888 | byte / 255 |
| 8888_GAMMA (linear unorm16 storage) | byte / 255, then PWL to linear on RGB |
| 2_10_10_10 | /1023, alpha /3 |
| 7e3 | `Float7e3To32` on RGB, alpha 2-bit /3 |
| 16_16, 16_16_16_16 and both fp16 formats | **raw bits** through the integer view. Deliberately not ÷32: "Disregarding the current −32…32 vs. −1…1 settings" |
| 32_FLOAT, 32_32_FLOAT | integer clear; falls back to a draw if the value exceeds 2^24 |

## Edge differences
- Same option defaults, gamma decode on resolve, 7e3 clamp, −32 decode and clear dword order.
- Its ROV 16_16_FLOAT clamp is ±65,504 instead of Canary's ±131,008.
- It doesn't have Canary's MIN/MAX premultiply on render target 0.

Source: [Xenia Canary](https://github.com/xenia-canary/xenia-canary) `e6cd2e4bc`, with Edge `8fb2772` for comparison.