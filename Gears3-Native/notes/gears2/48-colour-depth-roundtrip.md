# 48 - Colour <-> float-depth round trips (Brief 15 answers, Canary e6cd2e4bc)

From the research chat, 2026-10-21 10:03. Labels: [V] verified in source, [D] documented in a comment, [I] inferred.

The short answer: in Canary this round trip **is bit-exact for all 2²⁴ depth patterns**. Its depth conversion has no rounding to lose bits, and stencil is either written directly or rebuilt one bit at a time. Your `0x01407C0F → 0x01407FFF` therefore points at bugs in your own implementation. I've listed the likely causes at the end.

Source is Canary `e6cd2e4bc`, paths relative to `src/xenia/gpu/`. Labels: **[V]** verified in source, **[D]** documented in a comment, **[I]** inferred.

## 1. Colour → D24FS8

**How the depth is written** [V]
- The 32-bit colour word is split as `depth24 = word >> 8` and `stencil = word & 0xFF` (`d3d12/d3d12_render_target_cache.cc:3976–3986`).
- `depth24` is treated as a **20e4 bit pattern** and converted with `Depth20e4To32(..., remap_to_0_to_0_5 = true)` (`:4219–4222`). The result goes straight to `SV_Depth` (`:4228`).
- **`Depth20e4To32`** (`dxbc_shader_translator_om.cc:4038–4117`) is pure integer bit manipulation, with no floating-point maths. Taking 4 exponent bits `e` and 20 mantissa bits `m`:
  - **Normal values** (`e ≠ 0`): `f32 = ((e + 112 − remap) << 23) | (m << 3)`.
  - **Denormals** (`e = 0`, `m ≠ 0`): normalised with `firstbit_hi`. The exponent becomes `1 − (20 − firstbithigh(m))`, and the mantissa is shifted left to match.
  - **Zero:** the exponent becomes −112 + remap, which encodes 0.0f.
  - The 0–0.5 remap is just **−1 on the float32 exponent** (`remap_bias`), so it's exact.

**Unusual bit patterns** [V/I]
- There's no special case. Every one of the 2²⁴ patterns maps to a distinct, exact float32 value.
- The largest value, `0xFFFFFF` (exponent 15), is just under 2.0. After the remap it's just under 1.0, so it stays within the D3D depth range.
- 20e4 has **no infinity or NaN**: all-ones is just the largest finite value.
- Denormals go down to 2⁻³⁴ (2⁻³⁵ after the remap). That's well inside float32's normal range, so **host denormal flushing never affects them**.

**Rounding:** none. The value comes from an integer decode written with `mov oDepth` into `D32_FLOAT`, so it's stored exactly [V/I]. The only other step:
- `depth_transfer_not_equal_test` (default **on**, `render_target_cache.cc:31`) sets the depth test to `NOT_EQUAL` (`:4425–4434`). An identical value just isn't rewritten, so it stays the same.

**Host-depth shortcut** (two-source modes such as `kColorAndHostDepthToDepth`) [V]:
- If an earlier D24FS8 host depth exists for these tiles **and** `f32tof24(host) == guest bits`, that full-precision host value is written instead (`:3988–4128`).
- Otherwise the guest bits are decoded as above.

## 2. Stencil on that leg

**Choosing the method** [V] (`d3d12_render_target_cache.cc:471–476`)
- **`SV_StencilRef` (the default):** used when `native_stencil_value_output` is on (default on), the device supports shader-specified stencil, and the GPU isn't a pre-Arc Intel.
  - The stencil byte is written directly: `ubfe oStencilRef, word, 0, 8` (`:3980–3986`).
  - Pipeline settings: `StencilWriteMask = 0xFF`, `Func = ALWAYS`, `PassOp = REPLACE`, `FailOp = KEEP`.
  - **`StencilDepthFailOp = REPLACE` when `depth_transfer_not_equal_test` is on.** Stencil must still be written where the depth test fails because the value was equal (`:4425–4440`).
- **One bit per pass (the fallback)** [V], in this order:
  1. **Clear stencil to 0** over the transfer rectangles: `ClearDepthStencilView(STENCIL, …, 0)`, "will be setting the bits that need to be 1 by discarding samples" (`:4942–4980`).
  2. **Set the stencil reference to 0xFF** (`SetStencilReference(UINT8_MAX)`, `:4826`).
  3. **Run 8 draws, bit `i` = 0 to 7.** Each pipeline has `StencilWriteMask = 1 << i`, `Func = ALWAYS`, `PassOp = REPLACE`, `FailOp` and `DepthFailOp = KEEP`, and no depth writes (`:4374–4410`).
  4. **In each draw, the shader discards** any sample where `(stencil & (1 << i)) == 0`. The mask comes from a constant buffer (`:4233–4243`).

  The result: each set bit is written as 1 by `REPLACE` with reference 0xFF, limited by the write mask. Clear bits stay 0 from the clear. The `no_discard_stencil_in_transfer_pipelines` debug setting turns the discard off, and then every bit becomes 1.

## 3. D24FS8 → colour

- **The host depth becomes 20e4 bits with `PreClampedDepthTo20e4(..., round, remap_from_0_to_0_5 = true)`** (`d3d12_render_target_cache.cc:3577–3585`; helper at `dxbc_shader_translator_om.cc:3968–4036`) [V].
- **The round setting is `!depth_float24_convert_in_pixel_shader && depth_float24_round`.** Both settings default to false, so **the conversion truncates by default** [V]. Xenia recommends that: "false: Recommended" (`render_target_cache.cc:47–55`) [D].
- **The algorithm** [V]. Everything is integer maths on the float32 bits:
  - If the value is below 2⁻¹⁴ (adjusted for the remap), it's a denormal: `((f32 & 0x7FFFFF) | 0x800000) >> min(113 − remap − exp, 24)`.
  - Otherwise it's `f32 + 0xC8000000 + (remap << 23)`.
  - Then **only if rounding is on**, add `3 + ((x >> 3) & 1)` **before** shifting.
  - Finally `f24 = (x >> 3) & 0xFFFFFF`.
- **Exactness** [I]: an f32 produced by `Depth20e4To32` always has its low 3 mantissa bits clear. So truncation and round-to-nearest-even both give back exactly the original 24 bits. Canary needs no "host copy" trick for this round trip; that trick only protects depth that was *rendered* at full host precision.
- **Stencil** goes into bits 0–7: `bfi word, depth24 << 8 | stencil` (`:3586`) [V].

## 4. Known losses in Canary's comments

- **Nothing documents colour → float depth → colour as lossy**, and there are no TODOs about it [V, by search].
- **The documented losses are about host-rendered depth** [D]:
  - `depth_float24_round` warns that rounding can make "half of the samples… failing the depth test" after a full-precision round trip.
  - Mixing D24S8 and D24FS8 views of the same depth without the host-copy check causes the striping in title 534507D6 (`render_target_cache.h:569–579`, 685–697).
  - `depth_transfer_not_equal_test` exists so that unchanged values aren't rewritten, which helps depth compression (`render_target_cache.cc:31–40`).

## Your `0x01407C0F → 0x01407FFF` [I]

`0x01407C` has exponent bits = 0, so **it's a 20e4 denormal**. Its mantissa came back **+3** (`0x7C → 0x7F`), and the stencil byte came back as all ones.

**Depth (+3), most likely causes:**
1. **The rounding constant is added after the `>> 3` shift instead of before.** `(x >> 3) + 3 + bit` adds exactly 3 when bit 3 is 0. In Canary's order it's `(x + 3 + ((x >> 3) & 1)) >> 3`, which does nothing to exact values.
2. **A bug in your denormal path.** The shift amount must be `min(113 − remap − exp, 24)`, and the normalisation needs `firstbithigh`. Check how a different denormal pattern behaves, and compare one normal value.

**Stencil (0F → FF), most likely causes:**
1. **In the bit-per-pass path, the per-pass discard is missing**, or every pass uses write mask 0xFF. With reference 0xFF, every bit then gets set.
2. **The 0 clear before the passes is missing**, so old bits survive.
3. **If you copied the `NOT_EQUAL` depth test but left `StencilDepthFailOp = KEEP`**, pixels whose depth was equal never get their stencil written, so whatever was there before stays (0xFF from an earlier clear, for example). Canary uses `REPLACE` for exactly this reason.

**Test to add:** a CPU loop over all 2²⁴ depth values × a few stencil values through your two conversion shaders, compared bit for bit with Canary's `xenos::Float20e4To32` / `Float32To20e4` (`xenos.cc:126–166`, BSD-licensed). It runs in seconds and catches both kinds of bug for good.

Source: [Xenia Canary](https://github.com/xenia-canary/xenia-canary) `e6cd2e4bc`.