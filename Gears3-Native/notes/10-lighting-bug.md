# 10 - Lighting bug: ch1 black areas, ch8 darker, x2 green tint (2026-10-22)

## What job 034 showed [V]
- Shadow compares (ch1, ch8, ch0): the base-1440 depth/stencil copies differ from stock, but every one of those
  pixels is labelled "reference approximation (Canary ownership heuristic)" - stock's approximation, not ours.
- The wrong pixels are written by ONE global pixel shader, `ps 3C5E875D17B49A36` (vs B5A1208D037C2F4F, prim 4
  rect list, target #2 = the k_2_10_10_10 view of the scene at base 720; colour mask 1 (one channel); blend on;
  stencil test NOTEQUAL ref 0, no stencil write). ch1 frame 150: draw 144 writes FF000000 where stock has FFFFFFFF;
  ch8: draws 313/554/562, e.g. ours FF40FFFF vs FFD9FFFF. ch1 copy 4 (k_2_10_10_10 view, draws 127-129): 587,324 px
  "transferred but different".
- Switching off (one at a time) msaa_transfers, fill_rect_lists, resolve_view_targets, msaa_colour_resolves: no change.

## What the shader is [V, from our translation of the disc's ucode - tools/gn_hlsl_check, local build]
`ps-3C5E875D17B49A36` is one of the 226 global shaders (GlobalShaderCache). It samples texture slot 0 once and slot 1
nine times (SampleLevel, lod 0) - the shape of UE3's shadow projection: scene depth (slot 0) -> world position ->
shadow-map depth (slot 1) with a 3x3 filter -> attenuation written to one channel. [G: role from the shape]
So: ours says "in shadow" (0) where the console says "lit" (1) in ch1.

## Ruled out [V]
- Depth texture formats: k_24_8 (22) and k_24_8_FLOAT (23) decode the same on the CPU path (texture_cache.cpp
  kDepthUnorm/kDepthFloat) and the GPU path (builtin_shaders.h codes 14/15).
- The scene depth (ch8 copy 0: 99.99% equal) and the 8888 copies at 0x14028000 (96% equal) are close to stock.

## Open (job 038's capture answers these)
- Which address / format / size each slot reads in draw 144 (fetch constants in out\drawlog-038), and whether slot 1
  is the shadow depth (0x14028000, also reused by an 8888 colour copy in the same pass) or something else.
- The shader's constants (shadow matrix, depth bias / comparison constants) - a wrong constant would shift every
  comparison.
- x2: whether the same shader goes wrong there (shadow-map coordinates vs scaled textures).

## 16:25 - Gears 2's answers
- getWeights: unused in Gears 2 (2,474 shaders, 0 getWeights); our fix is harmless there.
- Address reuse within a frame (Gears 2 0x14550000: shadow maps, ping-pong, bloom) is fine 1x: resolve destinations
  keyed by (address, format, size), texture cache by fetch constant.
- x2 LEAD: a resolved texture is sampled at the scale only if our resolves wrote ALL its base-level pages
  (scaled-current); otherwise at 1x (their job 190: ch02's 800x800 shadow map stayed 1x). If the scene depth comes in
  scaled and the shadow map at 1x (or the reverse), the depth comparison uses the wrong texels = light leaking (our
  green tint?). Check with the scaled-miss "by reason" log on the shadow projection's two slots at x2.
- c255: Gears 2 uses c255 literals everywhere without trouble; if ours were 0, suspect a LOAD_ALU_CONSTANT reading
  stale memory (pm4.cpp ~440).
## Ruled out here: the colour clear of a k_2_10_10_10 target (ColorClearBits / HostColourClearValue: exact).
## getWeights implemented (hlsl.cpp, local commit) - 4 of 225 global shaders use it; goes into the next build.

## 17:20 - job 038 B1 capture (ch1 frame 150, shadow mode) [V unless marked]
- The wrong pixels sit in the k_8_8_8_8 copies of EDRAM base 720 (copies 6/9 -> 14028000), written by the shadow
  projection draws 144 / 169 (box: prim 4, 36 indices; colour mask F; blend src alpha / zero; depth ALWAYS; stencil
  NOTEQUAL ref 0). Base 720 is used both as this 8888 target and as the k_2_10_10_10 target (copies 4/7 -> 143C0000).
- Draw 144 inputs: slot 0 = scene depth k_24_8_FLOAT 0x14AF8000 1280x720 (copy 3); slot 1 = shadow depth k_24_8
  0x14028000 fetched as 1120x688 (copy 5 resolved 1120x544 there). c7 = (1120, 688, 60, 0.5): 60 = comparison sharpness.
  num_format 1 on slot 0 is normal for float formats (the SDK ignores it too) - ruled out.
- CPU emulation of the translated shader on STOCK's own dumped scene depth + shadow map (sp/emu.py, depth endian 8in32):
  pixel (733,108): receiver depth 0.4247, shadow-map taps 0.392-0.400 -> 0 (in shadow) = what OURS wrote. Stock's final
  pixel there is 1. So the shader maths agrees with ours; stock's draw 144 did not shade that pixel (or something later
  overwrote it). [V for the maths; G for the reason]
- So the ch1 black areas look like a COVERAGE difference, not a shading one. Two candidates [G]:
  (a) stencil: the box is drawn only where stencil != 0; our stencil at those pixels may be non-zero where stock's is 0
      (copies 0/3 show stencil equal at draw 127, but draws 128-143 are not compared);
  (b) the box's screen area: the light-volume vertices (VS B5A1208D037C2F4F, ib 1FAE8DE0) land elsewhere in ours.
- Next: capture our and stock's stencil at draw 144 (a depth/stencil compare right after it), or log the box's
  screen-space bounds in both paths.

## 18:15 - job 041 (memexport on) [V]
- ch1 shadow compare with memexport on: the shadow projection's black-vs-lit pixels (ps 3C5E875D17B49A36) are gone
  (B1 038: 48 sample pixels ff vs 00; 041 T0/M4: 0 big, 24 small diffs of 1-5/255). Frame 300 copies 24/27 went
  94.5% -> 99.996% and 99.2% -> 99.999% equal. The memexport copies feed this scene's data. Our tile trace shows
  stencil 00 at the old black pixel and the box drawn with NOTEQUAL - consistent with the coverage now matching.
- Still different: copy 16 / 20 (8888 at 14028000, base 720) ~81-91% equal. Unexplained samples: ps FC367470036D7CFE
  (same box VS) - a light-function / projected-texture shader: tf0 scene depth, tf2 and tf3 (implicit LOD), tf1;
  ours brighter in one channel (0x30 vs 0x1A, 0x55 vs 0x20), samples near the screen's left/right edges.
  Ruled out by reading: clamp-mode mapping matches the reference (2/4 clamp, 6 border, 3/5/7 mirror-once).
  Next: decode its fetch constants (filters, clamp, LOD bias) and compare tf2/tf3 texture content with stock.

## 19:05 - job 043 F3 (ch1 frame 150, memexport on): the light-function draw [V]
- Draw 335 (box VS B5A1208D037C2F4F, ps FC367470036D7CFE, prim 4, 1296 indices): slot 0 scene depth 20e4 0x14AF8000;
  slot 1 DXT1 256x256 at 0x1B1BF000, slot 2 DXT1 128x128 at 0x1B6E7000, slot 3 DXT5 128x128 at 0x1B6EE000 - all three
  with RGB sign mode 3 (GAMMA), linear filters. Ours vs stock: one channel brighter (0x30 vs 0x1A; draw 674 0x57 vs
  0x21), near the screen's left/right edges.
- Our gamma handling: xe_tex_post degammas AFTER filtering (notes/39 simplification); hardware degammas texels
  before filtering. [G] That alone makes ours darker, not brighter, so it is probably not the whole story; next compare
  how the reference converts gamma DXT textures, and the projected coordinates near the edges.
