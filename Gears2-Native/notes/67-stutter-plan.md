# 67 - Gears 2 stutter plan (option C, Shagg 09:32) - from job 330 [V numbers], plan items [G until measured]

What 330 showed (77f28b5, 2x scale, SMAA, uncapped):
- Warm cache (returning player): 0 shader work in hitch frames. Left: 5 hitches of 28-42 ms, all "textures"
  (ch05 x2, ch18 x3; ch18 worst frame 185 ms). ch10, ch02: none.
- Cold cache (first-time player, ch05): 9 hitches, worst 258 ms: shaders translated 111 + compiled 27 on the render
  thread inside those frames ("translated 42 (24.7 ms)", "compiled 6 (32.8 ms)"), pipelines are already async.
- The shader pack (0015) only helps once filled by play; a new player has none. Shipping a pack is not allowed
  (built from game files), so it must be built on the player's PC.

Plan:
C1 First-launch shader build: one pass on the player's own PC that translates + compiles every shader the game can
   use, before play, with a progress bar (UnleashedRecomp does this). Source of the shaders: [G] the game's own
   shader caches in its packages, or failing that our own run-time dumps replayed. Needs: find where Gears 2 keeps its
   Xenos microcode in the game files (UE3 ShaderCache objects) - research step, no PC.
C2 Until C1 exists: move shader translate + compile off the render thread (the draw waits a frame or two = brief
   pop-in instead of a 100-250 ms freeze). Switch, default to be decided by Shagg (pop-in vs stutter).
C3 Warm texture hitches: re-run the 330 census with --gow2_renderer_gpu_block_textures=true (8e50f40): Canary-style GPU
   loads may already remove the 30-40 ms "textures" frames; if not, spread big uploads over frames (Canary does no
   such thing - our own trick, labelled).
C4 Every test job reports GPU time per frame from now on (Shagg 09:29 asked about GPU cost), not only renderer CPU.
