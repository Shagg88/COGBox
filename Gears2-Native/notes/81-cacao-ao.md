# 81: FidelityFX CACAO as a second AO

XeGTAO (notes/80) looks right but costs more than some players want. CACAO is AMD's compute version of Intel's ASSAO
(MIT). It draws into the same `ao_final_` as XeGTAO, so the replace path, the add path and the skip of the game's chain
(notes/80 s.4, s.5, s.9) are shared. It runs in the games that have AO in the launcher: Gears 2, Gears 3 and now Judgment.

## What players get

One launcher choice, "CACAO (experimental)": `--gow2_ao=gtao --gow2_ao_algo=cacao --gow2_ao_quality=low
--gow2_ao_cacao_blur=3`. An exe without `gow2_ao_algo` falls back to the game's own AO with a launcher note. The medium
and high levels stay developer switches: nothing showed they are worth a choice of their own.

## The port (`renderer/cacao_shaders.h`)

| `--gow2_ao_quality` | Layers | Taps | Depth MIPs |
|---|---|---|---|
| low | 4 at quarter size each way, merged, then a depth-aware upsample | 5 | no |
| medium | 4 at half size each way | 5 | no |
| high, ultra | 4 at half size each way | 12 | yes |

AMD's adaptive level (importance map) is not ported: it adds cost, and the point here is the cheap end.

Differences from AMD's shaders, because the shared compute layout has 16 root constants, t0-t3, u0-u1 and no samplers:

- One 2D texture per layer, not an array, and Loads with a manual mirror.
- Depth MIPs: one dispatch per level, each built from 2 x 2 of the level before, as AMD's. A texture can't be read and
  written in one dispatch here (whole-texture transitions), so levels 0 and 2 live in one texture and 1 and 3 in another.
  The first version read each tile from the scene depth instead: 0.30 ms against 0.11 ms now.
- Normals from depth, stored as x, y (RG16F); z is rebuilt.
- The blur is AMD's group-shared form: one dispatch for all passes of a layer, one shader per pass count (1-8).

## Cost

4K, one high-end GPU, no AA, settled gameplay only, 2 blur or denoise passes unless stated. "Passes" is the sum of the
AO's own GPU stretches (`--gow2_ao_pass_times`); "added" is total GPU time against the same scene with the game's AO.

| Gears 2 launch chapter 1 (replaces the game's AO) | Total | Added | Passes |
|---|---|---|---|
| Game's own | 2.67 ms | | |
| CACAO low, 3 blur | 2.90 ms | +0.23 ms | 0.33 ms |
| XeGTAO low | 3.0-3.1 ms | +0.3-0.4 ms | 0.42 ms |
| XeGTAO high, 3 denoise | 3.18 ms | +0.51 ms | 0.62 ms |
| CACAO medium | 3.38 ms | +0.71 ms | 0.81 ms |
| CACAO high | 3.56 ms | +0.89 ms | 0.99 ms |
| XeGTAO ultra | 3.7-3.8 ms | +1.0-1.1 ms | 1.14 ms |

| Gears 3 chapter 1 (replaces the game's AO) | Total | Added | Passes |
|---|---|---|---|
| Game's own | 4.79 ms | | |
| CACAO low, 3 blur | 5.05 ms | +0.26 ms | 0.35 ms |
| XeGTAO low, 3 denoise | 5.12 ms | +0.33 ms | 0.43 ms |
| XeGTAO high, 3 denoise | 5.32 ms | +0.53 ms | 0.64 ms |
| CACAO medium | 5.52 ms | +0.73 ms | 0.83 ms |
| CACAO high | 5.63 ms | +0.84 ms | 0.94 ms |

Gears 2 launch chapter 3 (the game has no AO in this stretch, so AO is added): game's 3.57 ms, CACAO low 3 blur 3.97 ms,
XeGTAO high 2 denoise 4.08 ms. There the add pass counts too, and no game chain is skipped.

- The blur costs a read and a write of each layer, then almost nothing per pass: CACAO high passes 0.85 ms with no blur,
  0.99 with 2, 1.00 with 8. Per-pixel dispatches cost about 0.07 ms a pass.
- Total GPU time and fps need care in Gears 2: with the game's AO something other than the GPU holds it near 305 fps.

## How to measure this

- Compare the AO's pass times, or total time within one sitting. Only the scene's own settled part: Gears 2 eases by
  about 0.1 ms some 40 s in, with the game's AO too.
- Scripted presses must only get into the game. Repeated A takes cover: the view changes, and draw time then fell into two
  groups 0.7 ms apart between identical runs.
- Video playing on the same GPU moved a run from 4.8 to 5.2 ms.
- Gears 2 launch chapter 2 opens with an unskippable movie; use chapter 3 or 15 for the add path. Gears 3 chapter 1 needs
  A, then LB for the first tooltip (`15:A,40:LB,50:LUP:1`).

## By eye

CACAO low with 3 blur passes, in play:

- Gears 2 launch chapter 1: preferred to the game's own under SMAA T2x; good under DLAA and DLSS Ultra Performance. Less
  grain than XeGTAO under SMAA T2x and DLAA.
- Gears 2 launch chapter 3 (added): good under SMAA T2x.
- Gears 3 chapter 1: preferred to the game's own under SMAA T2x.
- Gears 3 chapter 8 (added): a little stronger than XeGTAO, still subtle, under SMAA T2x.

Not looked at: other chapters, Vulkan, FSR, XeSS, ultrawide, split screen. Half resolution should soften fine contact
detail compared with XeGTAO. Strength follows XeGTAO's settings: power x 1.5 / 2.2, and `--gow2_ao_radius`.

## Judgment

Judgment's own AO is half resolution with a separate apply pass, and it was the source of the shadow flicker left under
DLSS (notes/80 s.11). Confirmed live: with F5 the flicker comes back with the game's own and goes with ours, in University
under DLAA. XeGTAO high and CACAO low do not flicker; XeGTAO low does a little, and is not a launcher choice.

- The camera and depth already worked for DLSS. The chain detection finds nothing here, so ours is added before the fog
  pass, the same shader as Gears 3's (`gow2_ao_fog_ps`, `gowj_app.h`), at Gears 2's strength.
- The launcher turns the game's AO off with `AmbientOcclusion=False` when ours is chosen, so it goes in every level
  whatever its shaders. `--gow2_ao_skip_ps` is for testing only: it names the AO passes (University: `D622A9B27036CA2F`,
  `AFD089044AC2D462`, `7519EF9AB8169E14`, `1EAC0FD0D2CDFAC4`, `17D28C335665905E`) so that F5 can bring the game's back.
- Its own AO costs little (6.40 ms against 6.35 ms without it), so nearly all of ours is added cost: University, 4K, CACAO
  low 3 blur 6.60 ms, XeGTAO high 2 denoise 6.89 ms.
- Level sweep, CACAO low started as the launcher does, under DLAA: AO added on 93-100% of frames in University, Mansion,
  Island, Rooftops, Courthouse Boss and Aftermath E2, and in Aftermath E3 once its camera appears; no errors. Museum never
  reached gameplay in that run, so it says nothing. Where no camera is found there is no AO, which is no worse than the
  game's flicker.
- By eye: University only.

## Developer keys (`--gow2_ao_debug_keys`)

F9 cycles XeGTAO / CACAO / the game's own / none; F5 jumps between what is shown and the game's own; F6 XeGTAO / CACAO;
F8 XeGTAO's denoise passes or CACAO's blur passes (1, 2, 3, 4, 6, 8); F11 quality, shared by both.
