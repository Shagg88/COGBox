# 00 - Plan (2026-10-30, started overnight on Shagg's request 04:47)

Marks: [V] verified here, [S] read in a source, [G] guess.

Shagg: "start statically recompiling this game ... keep going with it until I wake up. If you get far enough, I'd like it
to have the same settings / quality of life additions and fixes as the other 3 games."

## Taken? (guidebook section 0)
- Web search 2026-10-30 found no public Gears 1 recompilation [S: research agent; GitHub search itself was blocked].
- Xenia Canary compatibility issue #44 (title 4D5307D5): playable; known emulator issues - black character shading
  (a game patch fixes it), flickering blood decals, lens flares through walls (occlusion), white checkerboard flashes in
  the tutorial, an XMA loop [S: github.com/xenia-canary/game-compatibility/issues/44]. Useful as things to watch for.

## Order of work (as Judgment's notes/00-plan)
1. Unpack the disc and title update, make the TU5 image, pin the release (notes/01). [done 05:00]
2. Project from GearsJ-Native (Shagg's own, copied freely): CMake, cmake/rexglue.cmake, tools, layer 2, layer 3 app.
   The shared renderer (layer 0) and the package shader reader come from Gears2-Native as for the other games.
3. First codegen; fix UnresolvedCall with hints; compile.
4. Boot loop to the title screen (SDK renderer and ours); then a chapter.
5. Launcher entry and the other games' options (frame caps, resolution, prompts, recipes...).
