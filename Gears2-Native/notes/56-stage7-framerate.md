# 56. Stage 7.4 - frame-rate unlock

## What limits the frame rate [V]
- The game asks for every second vertical blank (D3D present interval 2: sub_8298CEC0 maps the interval field to the
  VdSwap value, `li r11,2` at 0x8298CFB0) - 30 fps. Our GPU system honours it; `--gow2_gpu_vsync=false` does not wait.
- Unreal Engine 3's frame-rate smoothing: GearEngine.ini (Coalesced_int.bin) has bSmoothFrameRate=TRUE,
  MinSmoothedFrameRate=22, MaxSmoothedFrameRate=62. The engine's GetMaxTickRate (sub_824A2E78) returns
  MaxSmoothedFrameRate (`lfs f1,1284(r3)` at 0x824A2E94) and the engine waits to stay under it.
- The community "Unlock FPS" patch (illusion, boma; xenia-canary/game-patches) is exactly those two sites: nop the load
  (GetMaxTickRate then returns its default, no limit) and interval 2 -> 1.

## Job 197/198 result [V]
- Normal clock (no -BENCHMARK), `--gow2_gpu_vsync=false`: ch02 runs at 16.1-16.2 ms (62 fps cap), ch10 ~24-26 ms
  (our CPU side). Real time is kept: matching the 30 fps run's shots to the uncapped run's by picture, capped frame N
  matches uncapped frame ~2N (300<->600, 600<->1350, 750<->1500), i.e. the scene advances by wall-clock time.
  (-BENCHMARK runs use a fixed time step and so run fast when uncapped - expected, test-only.)

## Job 199
`--gow2_max_fps=N` (0 = the game's 62, N, -1 = none) through a hook after that load (manifest: Gow2MaxTickRate).
Unlock = `--gow2_gpu_vsync=false --gow2_max_fps=N`. Pass: ch02 at ~N fps (or our limit) with no error lines; the 0
default unchanged.
