# 35 - Stage 2 inventory: resolves, clears and passes (jobs 065-067)

Chapter 2 (index 1), steady play, frame 1500 (frame 2100 has the same structure with fewer shadowed lights), 1280x720,
untiled (D23). Tools: `--gow2_resolve_log_frames` (layer 0 `resolve_log.cpp`: every EDRAM copy and every change of
render-target setup), `--gow2_buffer_dump_frame` (layer 0 `buffer_dump.cpp`: each resolve's result as EXR/PNG, with
`--readback_resolve=full`). Interpretations marked *guess* are not verified.

## Resolves (EDRAM copies), frame 1500: 20

| # | Source (EDRAM) | Destination | Role (*guess* unless stated) |
|---|---|---|---|
| 0 | depth D24FS8, 1280 pitch | 15018000, 1280x720, k_8_8_8_8 uint | scene depth for later sampling |
| 1-7 | depth D24S8, 800 pitch | 14550000, 800x800, k_8_8_8_8 | one shadow map per shadowed light (7 lights), same texture each time |
| 8 | rt k_8_8_8_8, 640 pitch | 14370000, 640x360 | half-res buffer |
| 9-12 | rt k_16_16_FLOAT, 640 | 14550000, 640x360, k_16_16_FLOAT | half-res ping-pong (blur?) |
| 13 | rt k_16_16_FLOAT, 640 | 14460000, 640x360 | half-res result |
| 14 | rt k_16_16, 640 | 14370000, 640x360, k_16_16 | half-res (AO or velocity?) |
| 15 | rt 7e3 (k_2_10_10_10_FLOAT), 1280 | 148E8000, 1280x720, k_16_16_16_16_FLOAT, exp bias -3 | HDR scene colour |
| 16-18 | rt k_16_16_16_16_FLOAT, 400 | 14550000, 352x182, k_16_16_16_16_FLOAT | bloom chain |
| 19 | rt k_8_8_8_8, 1280 | 1F5F8000, 1280x720 | front buffer (verified: the swap's front buffer address) |

No resolve has a clear flag set and nothing is MSAA.

## Clears

D3D clears the EDRAM with a single draw that uses half the surface pitch and 4x MSAA (a fast-clear trick). Job 070
supports this: every such draw is a 3-vertex rect list with the same shader pair (vertex 1E6883FCCDE1F688, pixel
A4A965C189287B99), so a clear is recognisable by shader hash plus that setup: one such draw appears before each target is first used (depth before the prepass, the G-buffer
style MRT set at draw 164, each shadow map, each half-res and bloom target). So Stage 2 "clears" are draws with a
recognisable setup, not resolve flags.

## Pass structure, frame 1500 (1,206 draws)

| Draws | Targets | Role (*guess*) |
|---|---|---|
| 1-163 | depth only | depth prepass |
| 165-537 | rt0 7e3 + depth | base pass into the HDR target |
| 538-729 | depth test, no colour | occlusion queries |
| 730-818 | rt0 7e3 viewed as k_2_10_10_10_FLOAT_AS_16_16_16_16 (same EDRAM base 720) | lights (blendable view of the HDR target) |
| 819-912 | per light: clear, shadow depth (800x800), resolve, light draw (+ one 640-pitch draw) | 7 shadowed lights |
| 913-934 | rt0 7e3-as-16 + depth test | more lighting / translucency |
| 935-1127 | 640x360 targets | half-res post effects |
| 1128 | resolve HDR scene | |
| 1152-1180 | 400-pitch k_16_16_16_16_FLOAT | bloom |
| 1181-1205 | rt0 k_8_8_8_8 1280 | tone map + UI |

Repeated runs of 24 draws with no colour or depth output appear 7 times (938-961, 980-1003, 1007-1030, 1032-1055,
1128-1151, 1157-1180, 1182-1205). Job 070: each is a **single point** (point list, 1 vertex), with one vertex shader
(B6C9863F710683EC) and the same pixel shader as the clears (A4A965C189287B99); no memexport in either shader. They
write nothing we can see. *Guess*: a D3D-internal step after resolves (state or cache flush). For our renderer they are
no-ops to count, not to draw; still open what they are for.

## What Stage 2 needs from this

- **EDRAM aliasing:** the same EDRAM base is used with two formats (7e3 and its 16-bit blendable view); our render
  target table must key by base and format and convert between views like the emulator does.
- **Texture aliasing:** one destination texture (14550000) is reused for shadow maps, half-res buffers and bloom in one
  frame, at different sizes and formats. Resolves must be tracked by (address, format, size) per use, not by address.
- **Formats in use:** depth D24FS8 and D24S8; colour 8888, 7e3, 16_16, 16_16_FLOAT, 16_16_16_16_FLOAT; exponent bias
  -3 on the HDR resolve. No MSAA except the clear trick.

## Buffer contents (job 068, `out\harness\stage2-inventory\buffers-ch02-1500-v2`)

All 20 resolves of frame 1500 saved (EXR/PNG plus the raw tiled bytes). Checked:
- **Final image** (copy 19): matches the frame capture to a mean of 0.7/255 once red and blue are swapped (copy_dest_swap
  is set). Verified.
- **Scene depth** (copy 0, D24FS8): clean depth image. **HDR scene** (copy 15): clean, values 0-4 after the -3 exponent
  bias. **Shadow map** (copies 1-7, D24S8 unorm): the character's shadow depth in one corner of the 800x800 texture.
- **Half-res k_16_16_FLOAT** (copy 13): looks like ambient occlusion (*guess*: SSAO; red channel, scene-shaped).
- **Half-res k_16_16** (copy 14): only the characters, value 0.5 (*guess*: a character mask, for character lighting or
  motion blur).
- **Bloom** (copies 16-18): the copy fills 320x180 of the 352x182 destination; the last 32 columns are stale memory left
  by other uses of the same texture (14550000 aliasing, see above).
- Byte order: words are little-endian with only the resolve's endian swap applied (job 067's extra 32-bit swap turned
  half floats into noise). Tiling: Tiled2D from `xenos/formats.h` is right for every buffer.

## Second route: chapter 23, frame 1500 (job 071)

3,050 draws, 14 resolves. **Same structure as chapter 2:** clear (same shader pair) -> depth prepass (503) -> base pass
into 7e3 (1,737) -> scene depth resolve -> occlusion queries (272) -> lights through the 7e3-as-16 view (161) -> 2
shadowed lights (800x800 D24S8, same destination each time) -> half-res k_16_16_FLOAT ping-pong -> half-res k_16_16 ->
HDR resolve to k_16_16_16_16_FLOAT with exponent bias -3 -> 3 bloom copies (352x182) -> tone map -> front buffer
1F5F8000. The same 24-single-point groups (same vertex shader) follow the same steps. The shared scratch texture is
13BB0000 here (14550000 in chapter 2): addresses change per level, the pattern does not. So the Stage 2 table is the
engine's, not one scene's (two routes; *likely* general, not proven for every map).

## Research answers (Brief 8, research chat, 2026-10-21 02:28)

Labels from the brief: [V] verified in source, [D] documented, [I] inferred, [G] guess.
- **Clears:** no public source documents the half-pitch 4x MSAA rect clear [G], but the EDRAM arithmetic explains it
  [I]: a tile is 80x16 samples, so a 4x surface at half width and half height covers exactly the tiles of the full 1x
  surface, and the pixel shader runs a quarter as often. Our device should treat it as "clear this EDRAM tile range to
  this value" and apply it to every target aliasing those tiles, matched by the constant shader pair.
- **The 24 single-point draws:** Xenia's shader.h notes that Xbox 360 D3D9 sometimes issues single-point draws with oPos
  = (0,0,0,1) that cover no pixels, with a zero SQ_PROGRAM_CNTL and a stale pixel shader [V]. Purpose undocumented [G].
  Handling: no-op for rendering, keep the register state they set, log them. To check: oPos always (0,0,0,1).
- **Copy 13 (half-res k_16_16_FLOAT):** UE3's ambient occlusion buffer, occlusion in x and downsampled depth in y, with a
  reprojected history [I; UE3 2008 shaders, reference only]. GDC 2009 "Rendering Techniques in Gears of War 2"
  (Smedberg, Wright) covered the downsampled SSAO with reprojection caching [D].
- **Copy 14 (half-res k_16_16, characters at 0.5):** the motion-blur velocity buffer: UE3 writes Velocity*0.5+0.5, so
  0.5 means no motion, and only moving/skinned primitives are drawn [I]. Not a lighting mask.
- **Xenia quirk to avoid:** render targets bound twice at the same EDRAM base are resolved by a heuristic that drops the
  higher slot (render_target_cache ~866-900) - exactly our lights-into-a-blendable-view case. Our device must model the
  two views of one EDRAM range properly.
