# 55. Stage 7.3 - HDR output

What the game makes is an SDR image: the scene is lit in 7e3 / fp16, tone-mapped by the game's own post-processing into
the 8-bit front buffer, and the swap's gamma ramp shapes it for a TV. Real scene HDR needs that tone-map pass replaced
(a later step: pass overrides by shader hash). This first step gives a correct HDR10 path end to end:
- **SDK patch 24** (`patches/sdk-patch24-hdr.diff`, in the combined diff): `--present_hdr=true` makes the presenter's
  swap chain R10G10B10A2 and sets DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 when the output supports it (logged);
  the immediate drawer (overlay UI) uses the same format.
- **Game side** (kHdrEncodeCs): after the swap's gamma pass (and SMAA if on), the image is decoded to linear light
  (gamma 2.2), its brightest part optionally expanded toward the display's peak (`--gow2_hdr_knee`, `--gow2_hdr_expand`,
  `--gow2_hdr_peak`), scaled so full white is `--gow2_hdr_paper_white` nits (203, BT.2408), converted BT.709 -> BT.2020
  and PQ-encoded. With `--gow2_hdr_expand=0` it is the SDR picture shown correctly inside HDR10.
- Not testable here (no HDR display in the harness): the job checks the colour space call and that nothing else
  changes; how it looks is for the user's screen. Frame shots in HDR mode hold PQ values (they look washed out).

## Pass criteria (job 196)
- SDK rebuilds with patch 24; game builds; unit tests pass.
- `--present_hdr=false`: unchanged (ch02 capped shots vs job 195's no-SMAA run where variants match; normal exit).
- `--present_hdr=true`: runs to the end with no error lines; the log names the colour-space result.
