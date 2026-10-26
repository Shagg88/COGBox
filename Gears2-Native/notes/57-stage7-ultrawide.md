# 57. Stage 7.5 - ultrawide (21:9)

Switch: `--gow2_ultrawide` (default off). Source of the idea and the values: Xenia Canary's game patch "21:9 Ultrawide
Aspect Ratio Support" for 4D53082D (authors boma and TudorHorse), re-done as manifest hooks (D28: a proper widescreen
belongs to Stage 7). Every site was disassembled from our generated code first.

## How it works
The game draws a 1280x550 picture (2.33:1, square pixels; x the resolution scale) with a wider view. My first
reading - a 21:9 view squeezed into the 16:9 frame - was wrong (job 200 below).

| Site | Instruction | With the switch |
|---|---|---|
| 0x8252D73C (sub_8252D1B8) | `lfs f0,1032(r7)` - field-of-view factor | after it, f0 = 0.0106355 |
| 0x8252D608 (same) | `beq cr6,0x8252D69C` | always taken (ConstrainAspectRatio off) |
| 0x82806B10 (sub_82806A80) | `li r11,720` - scene buffer height after the video-mode query | after it, r11 = 550 |
| 0x826E3728 (sub_826E36C0) | `bctrl` - the UI scene viewport's height getter | after it, r3 = 564 (Canary replaces the call; the getter only reads) [G] |
| .rdata 0x821D0B28 | widescreen aspect, loaded at 0x82806B04 | 2.33333, written at load |

Job 200 also set the presenter's `present_letterbox = false` and `present_safe_area_y = 100`; job 201 replaces that
with `--gow2_present_frame_aspect` (below).

## Pass criteria (written before job 200)
- Generated code holds `if (Gow2UltrawideNoConstrain()) { goto loc_8252D69C;` (the jump hook really took).
- Switch off: no `gow2_ultrawide` log line; ch10 capped runs to the end with no new error lines.
- Switch on: the five changes logged (old values recorded); ch02, ch10 and the main menu run to the end, no new error
  lines, no crash; at 4K (scale 3) the same.
- Pictures, stretched to 21:9 for viewing: round things round, the view wider than at 16:9 (more on the left and
  right, same vertical view), no black bands or a cut-off part inside the frame, HUD and menu text in place and not
  squashed beyond what the patch gives on Canary.

## Job 200 result [V]
- The jump hook took (`if (Gow2UltrawideNoConstrain()) {` then `goto loc_8252D69C;`); build and 82 test cases passed.
- Logged with the switch on: aspect 1.7777778 -> 2.33333, scene height 720 -> 550, UI viewport height 550 -> 564 (the
  getter already returns the scene height), field-of-view factor 0.0087266 (pi/360) -> 0.0106355. Off: none of these.
- ch10 off / on, ch02 on, ch10 4K on, title screen on: all ran to the end, 0 error lines, no crash, 33.3 ms frames.
- Not anamorphic after all: the front buffer itself is 1280x550 (3840x1650 at 4K), a true 2.33:1 picture with square
  pixels - Marcus, the Reaver and the title logo keep their shapes; the view is wider than at 16:9 (more on both
  sides). So Canary's "letterbox off" is right only in a window of exactly that shape: the SDK presenter assumes the
  video mode's 16:9 and stretches a 16:9 window's worth of picture.
- Change (job 201): `--gow2_present_frame_aspect` (set by `--gow2_ultrawide`) gives the presenter the front buffer's own
  shape; the letterbox stays on, so it fills a 21:9 window or monitor and has bars in others.

## Pass criteria (written before job 201)
- Window screenshots (the default 16:9 window): switch on - the 2.33:1 picture, not stretched, with bars above and
  below; switch off - the normal full-window picture. Front-buffer shots unchanged from job 200's kind.
- ch10 at 4K with SMAA and the switch on: runs to the end, no error lines, SMAA ran (no "SMAA could not run").

## Job 201 result
- [V] Build, 82 test cases, commit. ch10 off / on and ch10 4K + SMAA + ultrawide: ran to the end, 0 error lines,
  33.3 ms frames; SMAA pipelines and tables made; "present at the front buffer's own shape true" logged.
- The window screenshots did not show the game: the capture took the screen area of the game window while another
  app's window covered it (the harness can't bring the game forward while the user's apps are open). So the window
  shape is not verified on screen. [G] From presenter.cpp: with the presenter told 1280x550, a 21:9 window gets the
  whole picture edge to edge and other shapes get bars (letterbox on, the default).
- Stage 7 is complete: 4K (notes/53), SMAA (54), HDR (55), frame-rate unlock (56), ultrawide (57).
